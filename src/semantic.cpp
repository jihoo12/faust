#include "faust/semantic.h"
#include "faust/diagnostic.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace faust {
namespace {
struct Signature {
  std::vector<Type> parameters;
  Type result;
  std::set<std::string> effects;
};

const char *typeName(Type type) { return type == Type::I32 ? "i32" : "bool"; }

Type checkExpr(const Expr &expr, const Function &function,
               const std::map<std::string, Type> &locals,
               const std::map<std::string, Signature> &signatures) {
  switch (expr.kind) {
  case Expr::Integer:
    return Type::I32;
  case Expr::Boolean:
    return Type::Bool;
  case Expr::Variable: {
    auto found = locals.find(expr.token.text);
    if (found == locals.end())
      fail(expr.token, "unknown variable '" + expr.token.text + "'");
    return found->second;
  }
  case Expr::Negate: {
    auto operand = checkExpr(*expr.children[0], function, locals, signatures);
    if (operand != Type::I32)
      fail(expr.token, "unary '-' requires i32");
    return Type::I32;
  }
  case Expr::Not: {
    auto operand = checkExpr(*expr.children[0], function, locals, signatures);
    if (operand != Type::Bool)
      fail(expr.token, "unary '!' requires bool");
    return Type::Bool;
  }
  case Expr::Binary: {
    auto left = checkExpr(*expr.children[0], function, locals, signatures);
    auto right = checkExpr(*expr.children[1], function, locals, signatures);
    const auto &op = expr.token.text;
    if (op == "+" || op == "-" || op == "*") {
      if (left != Type::I32 || right != Type::I32)
        fail(expr.token, "operator '" + op + "' requires i32 operands");
      return Type::I32;
    }
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
      if (left != Type::I32 || right != Type::I32)
        fail(expr.token, "operator '" + op + "' requires i32 operands");
      return Type::Bool;
    }
    if (left != right)
      fail(expr.token, "operator '" + op + "' requires operands of the same type");
    return Type::Bool;
  }
  case Expr::Call: {
    auto found = signatures.find(expr.token.text);
    if (found == signatures.end())
      fail(expr.token, "unknown function '" + expr.token.text + "'");
    if (expr.children.size() != found->second.parameters.size())
      fail(expr.token, "wrong number of arguments to '" + expr.token.text + "'");
    for (const auto &effect : found->second.effects) {
      if (!function.effects.count(effect))
        fail(expr.token, "call to '" + expr.token.text + "' requires effect '" +
                             effect + "' in contract of '" + function.name.text + "'");
    }
    for (size_t i = 0; i < expr.children.size(); ++i) {
      auto actual = checkExpr(*expr.children[i], function, locals, signatures);
      auto expected = found->second.parameters[i];
      if (actual != expected)
        fail(expr.children[i]->token,
             "argument " + std::to_string(i + 1) + " to '" + expr.token.text +
                 "' must be " + typeName(expected));
    }
    return found->second.result;
  }
  }
  fail(expr.token, "internal error: unknown expression");
}

} // namespace

void check(const Program &functions) {
  std::map<std::string, Signature> signatures = {
      {"print", {{Type::I32}, Type::I32, {"alloc", "block", "io"}}}};
  for (const auto &function : functions) {
    std::vector<Type> parameters;
    for (const auto &parameter : function.parameters)
      parameters.push_back(parameter.type);
    if (!signatures.emplace(function.name.text,
                            Signature{parameters, function.returnType,
                                      function.effects}).second)
      fail(function.name,
           "duplicate or reserved function '" + function.name.text + "'");
  }
  auto main = signatures.find("main");
  if (main == signatures.end())
    fail(Token{}, "program must define main");
  if (!main->second.parameters.empty())
    fail(Token{}, "main must have no parameters");
  if (main->second.result != Type::I32)
    fail(Token{}, "main must return i32");

  for (const auto &function : functions) {
    std::map<std::string, Type> locals;
    for (const auto &parameter : function.parameters) {
      if (!locals.emplace(parameter.name.text, parameter.type).second)
        fail(parameter.name, "duplicate parameter '" + parameter.name.text + "'");
    }
    for (const auto &statement : function.body) {
      Type expressionType =
          checkExpr(*statement.expression, function, locals, signatures);
      if (statement.kind == Statement::Let) {
        if (!locals.emplace(statement.token.text, expressionType).second)
          fail(statement.token, "duplicate local '" + statement.token.text + "'");
      } else if (statement.kind == Statement::Return &&
                 expressionType != function.returnType) {
        fail(statement.token,
             "return type mismatch: expected " + std::string(typeName(function.returnType)) +
                 ", got " + typeName(expressionType));
      }
    }
  }
}

} // namespace faust
