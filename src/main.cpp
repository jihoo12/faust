#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Token {
  std::string text;
  unsigned line = 1;
  unsigned column = 1;
};

[[noreturn]] void fail(const Token &token, const std::string &message) {
  throw std::runtime_error(std::to_string(token.line) + ":" +
                           std::to_string(token.column) +
                           ": error: " + message);
}

std::vector<Token> lex(const std::string &source) {
  std::vector<Token> tokens;
  unsigned line = 1, column = 1;
  size_t pos = 0;
  auto advance = [&] {
    if (source[pos++] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  };
  while (pos < source.size()) {
    unsigned char ch = source[pos];
    if (std::isspace(ch)) {
      advance();
      continue;
    }
    if (source.compare(pos, 2, "//") == 0) {
      while (pos < source.size() && source[pos] != '\n')
        advance();
      continue;
    }
    Token token{"", line, column};
    size_t start = pos;
    if (std::isalpha(ch) || ch == '_') {
      do {
        advance();
      } while (pos < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[pos])) ||
                source[pos] == '_'));
    } else if (std::isdigit(ch)) {
      do {
        advance();
      } while (pos < source.size() &&
               std::isdigit(static_cast<unsigned char>(source[pos])));
    } else if (source.compare(pos, 2, "->") == 0) {
      advance();
      advance();
    } else if (std::string("(){}:,;=!+-*").find(ch) != std::string::npos) {
      advance();
    } else {
      fail(token, "unexpected character");
    }
    token.text = source.substr(start, pos - start);
    tokens.push_back(std::move(token));
  }
  tokens.push_back({"", line, column});
  return tokens;
}

struct Expr {
  enum Kind { Integer, Variable, Call, Binary, Negate } kind;
  Token token;
  int32_t value = 0;
  std::vector<std::unique_ptr<Expr>> children;
};
struct Statement {
  enum Kind { Let, Evaluate, Return } kind;
  Token token;
  std::unique_ptr<Expr> expression;
};
struct Function {
  Token name;
  std::vector<Token> parameters;
  std::set<std::string> effects;
  std::vector<Statement> body;
};

class Parser {
  std::vector<Token> tokens;
  size_t pos = 0;
  const Token &peek() const { return tokens[pos]; }
  Token take() { return tokens[pos++]; }
  bool accept(const std::string &text) {
    if (peek().text != text)
      return false;
    take();
    return true;
  }
  Token expect(const std::string &text) {
    if (peek().text != text)
      fail(peek(), "expected '" + text + "'");
    return take();
  }
  Token identifier() {
    const auto &t = peek();
    if (t.text.empty() ||
        !(std::isalpha(static_cast<unsigned char>(t.text[0])) ||
          t.text[0] == '_') ||
        t.text == "fn" || t.text == "let" || t.text == "return" ||
        t.text == "i32")
      fail(t, "expected an identifier");
    return take();
  }
  std::unique_ptr<Expr> primary() {
    Token token = peek();
    if (accept("(")) {
      auto result = expression();
      expect(")");
      return result;
    }
    if (accept("-")) {
      // Accept the negative endpoint without accepting 2147483648 on its own.
      if (accept("2147483648")) {
        auto result = std::make_unique<Expr>();
        result->kind = Expr::Integer;
        result->token = token;
        result->value = INT32_MIN;
        return result;
      }
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Negate;
      result->token = token;
      result->children.push_back(primary());
      return result;
    }
    auto result = std::make_unique<Expr>();
    result->token = token;
    if (!token.text.empty() &&
        std::isdigit(static_cast<unsigned char>(token.text[0]))) {
      take();
      int64_t value = 0;
      for (char digit : token.text) {
        value = value * 10 + digit - '0';
        if (value > INT32_MAX)
          fail(token, "integer literal is outside the i32 range");
      }
      result->kind = Expr::Integer;
      result->value = static_cast<int32_t>(value);
      return result;
    }
    identifier();
    result->kind = Expr::Variable;
    if (accept("(")) {
      result->kind = Expr::Call;
      if (!accept(")")) {
        do {
          result->children.push_back(expression());
        } while (accept(","));
        expect(")");
      }
    }
    return result;
  }
  std::unique_ptr<Expr> expression(int minimum = 0) {
    auto left = primary();
    while (true) {
      const auto op = peek();
      int precedence = op.text == "*"                       ? 20
                       : (op.text == "+" || op.text == "-") ? 10
                                                            : -1;
      if (precedence < minimum)
        break;
      take();
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Binary;
      result->token = op;
      result->children.push_back(std::move(left));
      result->children.push_back(expression(precedence + 1));
      left = std::move(result);
    }
    return left;
  }

public:
  explicit Parser(const std::string &source) : tokens(lex(source)) {}
  std::vector<Function> parse() {
    std::vector<Function> functions;
    while (!peek().text.empty()) {
      expect("fn");
      Function function;
      function.name = identifier();
      expect("(");
      if (!accept(")")) {
        do {
          function.parameters.push_back(identifier());
          expect(":");
          expect("i32");
        } while (accept(","));
        expect(")");
      }
      expect("->");
      expect("i32");
      if (accept("!")) {
        expect("{");
        if (!accept("}")) {
          do {
            Token effect = identifier();
            if (effect.text != "io" && effect.text != "alloc" &&
                effect.text != "block")
              fail(effect, "unknown effect '" + effect.text + "'");
            if (!function.effects.insert(effect.text).second)
              fail(effect, "duplicate effect '" + effect.text + "'");
          } while (accept(","));
          expect("}");
        }
      }
      expect("{");
      bool returned = false;
      while (!accept("}")) {
        if (peek().text.empty())
          fail(peek(), "expected '}'");
        if (returned)
          fail(peek(), "statement after return");
        Statement statement;
        statement.token = peek();
        if (accept("let")) {
          statement.kind = Statement::Let;
          statement.token = identifier();
          expect("=");
        } else if (accept("return")) {
          statement.kind = Statement::Return;
          returned = true;
        } else {
          statement.kind = Statement::Evaluate;
        }
        statement.expression = expression();
        expect(";");
        function.body.push_back(std::move(statement));
      }
      if (!returned)
        fail(function.name, "function must end with a return statement");
      functions.push_back(std::move(function));
    }
    return functions;
  }
};

struct Signature {
  size_t arity;
  std::set<std::string> effects;
};

void checkExpr(const Expr &expr, const Function &function,
               const std::set<std::string> &locals,
               const std::map<std::string, Signature> &signatures) {
  if (expr.kind == Expr::Variable && !locals.count(expr.token.text))
    fail(expr.token, "unknown variable '" + expr.token.text + "'");
  if (expr.kind == Expr::Call) {
    auto found = signatures.find(expr.token.text);
    if (found == signatures.end())
      fail(expr.token, "unknown function '" + expr.token.text + "'");
    if (expr.children.size() != found->second.arity)
      fail(expr.token,
           "wrong number of arguments to '" + expr.token.text + "'");
    for (const auto &effect : found->second.effects) {
      if (!function.effects.count(effect))
        fail(expr.token, "call to '" + expr.token.text + "' requires effect '" +
                             effect + "' in contract of '" +
                             function.name.text + "'");
    }
  }
  for (const auto &child : expr.children)
    checkExpr(*child, function, locals, signatures);
}

void check(const std::vector<Function> &functions) {
  // printf may allocate and block, so printing conservatively requires all
  // three.
  std::map<std::string, Signature> signatures = {
      {"print", {1, {"alloc", "block", "io"}}}};
  for (const auto &function : functions) {
    if (!signatures
             .emplace(function.name.text,
                      Signature{function.parameters.size(), function.effects})
             .second)
      fail(function.name,
           "duplicate or reserved function '" + function.name.text + "'");
  }
  auto main = signatures.find("main");
  if (main == signatures.end())
    fail(Token{}, "program must define main");
  if (main->second.arity != 0)
    fail(Token{}, "main must have no parameters");
  for (const auto &function : functions) {
    std::set<std::string> locals;
    for (const auto &parameter : function.parameters) {
      if (!locals.insert(parameter.text).second)
        fail(parameter, "duplicate parameter '" + parameter.text + "'");
    }
    for (const auto &statement : function.body) {
      checkExpr(*statement.expression, function, locals, signatures);
      if (statement.kind == Statement::Let &&
          !locals.insert(statement.token.text).second)
        fail(statement.token, "duplicate local '" + statement.token.text + "'");
    }
  }
}

class Generator {
  llvm::LLVMContext context;
  llvm::Module module{"faust", context};
  llvm::IRBuilder<> builder{context};
  std::map<std::string, llvm::Function *> functions;
  std::map<std::string, llvm::Value *> locals;

  llvm::Value *emit(const Expr &expr) {
    switch (expr.kind) {
    case Expr::Integer:
      return builder.getInt32(expr.value);
    case Expr::Variable:
      return locals.at(expr.token.text);
    case Expr::Negate:
      return builder.CreateNeg(emit(*expr.children[0]));
    case Expr::Binary: {
      auto *left = emit(*expr.children[0]);
      auto *right = emit(*expr.children[1]);
      if (expr.token.text == "+")
        return builder.CreateAdd(left, right);
      if (expr.token.text == "-")
        return builder.CreateSub(left, right);
      return builder.CreateMul(left, right);
    }
    case Expr::Call: {
      std::vector<llvm::Value *> arguments;
      for (const auto &child : expr.children)
        arguments.push_back(emit(*child));
      if (expr.token.text == "print") {
        auto printfFunction = module.getOrInsertFunction(
            "printf", llvm::FunctionType::get(builder.getInt32Ty(),
                                              {builder.getPtrTy()}, true));
        auto *format = builder.CreateGlobalString("%d\n", "format");
        builder.CreateCall(printfFunction, {format, arguments[0]});
        return builder.getInt32(0);
      }
      return builder.CreateCall(functions.at(expr.token.text), arguments);
    }
    }
    throw std::runtime_error("internal error: unknown expression");
  }

public:
  std::string generate(const std::vector<Function> &program) {
    for (const auto &function : program) {
      std::vector<llvm::Type *> parameters(function.parameters.size(),
                                           builder.getInt32Ty());
      auto *type =
          llvm::FunctionType::get(builder.getInt32Ty(), parameters, false);
      bool isMain = function.name.text == "main";
      // Keep source names separate from C runtime symbols such as printf.
      functions[function.name.text] = llvm::Function::Create(
          type,
          isMain ? llvm::Function::ExternalLinkage
                 : llvm::Function::InternalLinkage,
          isMain ? "main" : "faust." + function.name.text, module);
    }
    for (const auto &function : program) {
      auto *target = functions.at(function.name.text);
      builder.SetInsertPoint(
          llvm::BasicBlock::Create(context, "entry", target));
      locals.clear();
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        target->getArg(i)->setName(function.parameters[i].text);
        locals[function.parameters[i].text] = target->getArg(i);
      }
      for (const auto &statement : function.body) {
        auto *value = emit(*statement.expression);
        if (statement.kind == Statement::Let)
          locals[statement.token.text] = value;
        if (statement.kind == Statement::Return)
          builder.CreateRet(value);
      }
    }
    if (llvm::verifyModule(module, &llvm::errs()))
      throw std::runtime_error("internal error: invalid LLVM module");
    std::string output;
    llvm::raw_string_ostream stream(output);
    module.print(stream, nullptr);
    return output;
  }
};
} // namespace

int main(int argc, char **argv) {
  std::string input, output;
  bool checkOnly = false;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--help") {
      std::cout << "Usage: faust [--check] <source.faust> [-o output.ll]\n";
      return 0;
    }
    if (argument == "--check")
      checkOnly = true;
    else if (argument == "-o" && i + 1 < argc)
      output = argv[++i];
    else if (!argument.empty() && argument[0] != '-' && input.empty())
      input = argument;
    else {
      std::cerr << "error: unexpected argument '" << argument << "'\n";
      return 1;
    }
  }
  if (input.empty() || (checkOnly && !output.empty())) {
    std::cerr << "Usage: faust [--check] <source.faust> [-o output.ll]\n";
    return 1;
  }
  try {
    std::ifstream file(input);
    if (!file)
      throw std::runtime_error("cannot open input file");
    std::string source((std::istreambuf_iterator<char>(file)), {});
    if (file.bad())
      throw std::runtime_error("cannot read input file");
    auto program = Parser(source).parse();
    check(program);
    if (checkOnly)
      return 0;
    auto ir = Generator().generate(program);
    if (output.empty()) {
      std::cout << ir;
      if (!std::cout)
        throw std::runtime_error("cannot write IR to standard output");
    } else {
      std::ofstream file(output);
      file << ir;
      file.close();
      if (!file)
        throw std::runtime_error("cannot write output file '" + output + "'");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << input << ":" << error.what() << '\n';
    return 1;
  }
}
