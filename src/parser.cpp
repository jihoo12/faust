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
  bool isIdentifier(const Token &t) const {
    if (t.text.empty())
      return false;
    if (!(std::isalpha(static_cast<unsigned char>(t.text[0])) ||
          t.text[0] == '_'))
      return false;
    if (t.text == "fn" || t.text == "let" || t.text == "return" ||
        t.text == "i32" || t.text == "bool" || t.text == "if" ||
        t.text == "else" || t.text == "while" || t.text == "true" ||
        t.text == "false")
      return false;
    return true;
  }
  Token identifier() {
    const auto &t = peek();
    if (!isIdentifier(t))
      fail(t, "expected an identifier");
    return take();
  }
  Type parseType() {
    if (accept("i32"))
      return Type::I32;
    if (accept("bool"))
      return Type::Bool;
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
    if (accept("!")) {
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Not;
      result->token = token;
      result->children.push_back(primary());
      return result;
    }
    if (accept("true")) {
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Boolean;
      result->token = token;
      result->value = 1;
      return result;
    }
    if (accept("false")) {
      auto result = std::make_unique<Expr>();
      result->kind = Expr::Boolean;
      result->token = token;
      result->value = 0;
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
      int precedence;
      Expr::Kind kind;
      if (op.text == "||") {
        precedence = 5;
        kind = Expr::Logical;
      } else if (op.text == "&&") {
        precedence = 6;
        kind = Expr::Logical;
      } else if (op.text == "==" || op.text == "!=") {
        precedence = 7;
        kind = Expr::Compare;
      } else if (op.text == "<" || op.text == ">" || op.text == "<=" ||
                 op.text == ">=") {
        precedence = 8;
        kind = Expr::Compare;
      } else if (op.text == "*") {
        precedence = 20;
        kind = Expr::Binary;
      } else if (op.text == "+" || op.text == "-") {
        precedence = 10;
        kind = Expr::Binary;
      } else {
        break;
      }
      if (precedence < minimum)
        break;
      take();
      auto result = std::make_unique<Expr>();
      result->kind = kind;
      result->token = op;
      result->children.push_back(std::move(left));
      result->children.push_back(expression(precedence + 1));
      left = std::move(result);
    }
    return left;
  }
  std::vector<Statement> parseBlock() {
    expect("{");
    std::vector<Statement> body;
    while (!accept("}")) {
      if (peek().text.empty())
        fail(peek(), "expected '}'");
      body.push_back(parseStatement());
    }
    return body;
  }
  Statement parseStatement() {
    Statement statement;
    statement.token = peek();
    if (accept("let")) {
      statement.kind = Statement::Let;
      statement.token = identifier();
      expect("=");
      statement.expression = expression();
      expect(";");
    } else if (accept("return")) {
      statement.kind = Statement::Return;
      statement.expression = expression();
      expect(";");
    } else if (accept("if")) {
      statement.kind = Statement::If;
      if (accept("(")) {
        statement.condition = expression();
        expect(")");
      } else {
        statement.condition = expression();
      }
      statement.body = parseBlock();
      if (accept("else")) {
        if (accept("if")) {
          Statement nestedIf;
          nestedIf.kind = Statement::If;
          if (accept("(")) {
            nestedIf.condition = expression();
            expect(")");
          } else {
            nestedIf.condition = expression();
          }
          nestedIf.body = parseBlock();
          if (accept("else")) {
            nestedIf.elseBody = parseBlock();
          }
          statement.elseBody.push_back(std::move(nestedIf));
        } else {
          statement.elseBody = parseBlock();
        }
      }
    } else if (accept("while")) {
      statement.kind = Statement::While;
      if (accept("(")) {
        statement.condition = expression();
        expect(")");
      } else {
        statement.condition = expression();
      }
      statement.body = parseBlock();
    } else {
      Token ident = peek();
      if (isIdentifier(ident) && pos + 1 < tokens.size() &&
          tokens[pos + 1].text == "=") {
        statement.kind = Statement::Assign;
        statement.token = take();
        take();
        statement.expression = expression();
        expect(";");
      } else {
        statement.kind = Statement::Evaluate;
        statement.expression = expression();
        expect(";");
      }
    }
    return statement;
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
          function.paramTypes.push_back(parseType());
        } while (accept(","));
        expect(")");
      }
      expect("->");
      function.returnType = parseType();
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
        function.body.push_back(parseStatement());
        if (function.body.back().kind == Statement::Return)
          returned = true;
      }
      if (!returned)
        fail(function.name, "function must end with a return statement");
      functions.push_back(std::move(function));
    }
    return functions;
  }
};

} // namespace

Program parse(const std::string &source) { return Parser(source).parse(); }

} // namespace faust
