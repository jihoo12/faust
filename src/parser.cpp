#include "faust/parser.h"
#include "faust/diagnostic.h"
#include "faust/lexer.h"

#include <cctype>
#include <utility>

namespace faust {
namespace {
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
        !(std::isalpha(static_cast<unsigned char>(t.text[0])) || t.text[0] == '_') ||
        t.text == "fn" || t.text == "let" || t.text == "return" ||
        t.text == "if" || t.text == "else" || t.text == "i32" ||
        t.text == "bool" || t.text == "true" || t.text == "false")
      fail(t, "expected an identifier");
    return take();
  }
  Type type() {
    if (accept("i32")) return Type::I32;
    if (accept("bool")) return Type::Bool;
    fail(peek(), "expected a type");
  }
  std::unique_ptr<Expr> primary() {
    Token token = peek();
    if (accept("(")) {
      auto result = expression();
      expect(")");
      return result;
    }
    if (accept("-")) {
      if (accept("2147483648")) {
        auto result = std::make_unique<Expr>();
        result->kind = Expr::Integer; result->token = token; result->value = INT32_MIN;
        return result;
      }
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Negate; result->token = token;
      result->children.push_back(primary());
      return result;
    }
    if (accept("!")) {
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Not; result->token = token;
      result->children.push_back(primary());
      return result;
    }
    auto result = std::make_unique<Expr>();
    result->token = token;
    if (token.text == "true" || token.text == "false") {
      take(); result->kind = Expr::Boolean; result->value = token.text == "true";
      return result;
    }
    if (!token.text.empty() && std::isdigit(static_cast<unsigned char>(token.text[0]))) {
      take();
      int64_t value = 0;
      for (char digit : token.text) {
        value = value * 10 + digit - '0';
        if (value > INT32_MAX) fail(token, "integer literal is outside the i32 range");
      }
      result->kind = Expr::Integer; result->value = static_cast<int32_t>(value);
      return result;
    }
    identifier();
    result->kind = Expr::Variable;
    if (accept("(")) {
      result->kind = Expr::Call;
      if (!accept(")")) {
        do { result->children.push_back(expression()); } while (accept(","));
        expect(")");
      }
    }
    return result;
  }
  std::unique_ptr<Expr> expression(int minimum = 0) {
    auto left = primary();
    while (true) {
      const auto op = peek();
      int precedence = op.text == "*" ? 40
          : (op.text == "+" || op.text == "-") ? 30
          : (op.text == "<" || op.text == "<=" || op.text == ">" || op.text == ">=") ? 20
          : (op.text == "==" || op.text == "!=") ? 10 : -1;
      if (precedence < minimum) break;
      take();
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Binary; result->token = op;
      result->children.push_back(std::move(left));
      result->children.push_back(expression(precedence + 1));
      left = std::move(result);
    }
    return left;
  }

  bool block(std::vector<Statement> &statements) {
    expect("{");
    bool returned = false;
    while (!accept("}")) {
      if (peek().text.empty()) fail(peek(), "expected '}'");
      if (returned) fail(peek(), "statement after return");
      Statement statement;
      statement.token = peek();
      if (accept("if")) {
        statement.kind = Statement::If;
        statement.expression = expression();
        bool thenReturns = block(statement.thenBranch);
        bool elseReturns = false;
        if (accept("else"))
          elseReturns = block(statement.elseBranch);
        returned = thenReturns && elseReturns;
      } else {
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
      }
      statements.push_back(std::move(statement));
    }
    return returned;
  }

public:
  explicit Parser(const std::string &source) : tokens(lex(source)) {}
  Program parse() {
    Program functions;
    while (!peek().text.empty()) {
      expect("fn");
      Function function;
      function.name = identifier();
      expect("(");
      if (!accept(")")) {
        do {
          Parameter parameter;
          parameter.name = identifier(); expect(":"); parameter.type = type();
          function.parameters.push_back(std::move(parameter));
        } while (accept(","));
        expect(")");
      }
      expect("->"); function.returnType = type();
      if (accept("!")) {
        expect("{");
        if (!accept("}")) {
          do {
            Token effect = identifier();
            if (effect.text != "io" && effect.text != "alloc" && effect.text != "block")
              fail(effect, "unknown effect '" + effect.text + "'");
            if (!function.effects.insert(effect.text).second)
              fail(effect, "duplicate effect '" + effect.text + "'");
          } while (accept(","));
          expect("}");
        }
      }
      if (!block(function.body))
        fail(function.name, "function must return on all paths");
      functions.push_back(std::move(function));
    }
    return functions;
  }
};
} // namespace

Program parse(const std::string &source) { return Parser(source).parse(); }

} // namespace faust
