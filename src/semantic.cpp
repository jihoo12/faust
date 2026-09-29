#include "faust/semantic.h"
#include "faust/diagnostic.h"

#include <map>
#include <set>

namespace faust {
namespace {
struct Signature {
  std::vector<Type> paramTypes;
  Type returnType;
  std::set<std::string> effects;
};

void checkExpr(Expr &expr, const Function &function,
               const std::map<std::string, Type> &locals,
               const std::map<std::string, Signature> &signatures) {
  switch (expr.kind) {
  case Expr::Integer:
    expr.type = Type::I32;
    break;
  case Expr::Boolean:
    expr.type = Type::Bool;
    break;
  case Expr::Variable: {
    auto found = locals.find(expr.token.text);
    if (found == locals.end())
      fail(expr.token, "unknown variable '" + expr.token.text + "'");
    expr.type = found->second;
    break;
  }
  case Expr::Call: {
    auto found = signatures.find(expr.token.text);
    if (found == signatures.end())
      fail(expr.token, "unknown function '" + expr.token.text + "'");
    if (expr.children.size() != found->second.paramTypes.size())
      fail(expr.token,
           "wrong number of arguments to '" + expr.token.text + "'");
    for (const auto &effect : found->second.effects) {
      if (!function.effects.count(effect))
        fail(expr.token, "call to '" + expr.token.text + "' requires effect '" +
                              effect + "' in contract of '" +
                              function.name.text + "'");
    }
    for (size_t i = 0; i < expr.children.size(); ++i) {
      checkExpr(*expr.children[i], function, locals, signatures);
      if (expr.children[i]->type != found->second.paramTypes[i])
        fail(expr.children[i]->token,
             "wrong type for argument " + std::to_string(i + 1) + " to '" +
                 expr.token.text + "'");
    }
    expr.type = found->second.returnType;
    break;
  }
  case Expr::Negate:
    checkExpr(*expr.children[0], function, locals, signatures);
    if (expr.children[0]->type != Type::I32)
      fail(expr.token, "unary '-' requires i32");
    expr.type = Type::I32;
    break;
  case Expr::Not:
    checkExpr(*expr.children[0], function, locals, signatures);
    if (expr.children[0]->type != Type::Bool)
      fail(expr.token, "'!' requires bool");
    expr.type = Type::Bool;
    break;
  case Expr::Binary:
    checkExpr(*expr.children[0], function, locals, signatures);
    checkExpr(*expr.children[1], function, locals, signatures);
    if (expr.children[0]->type != Type::I32 ||
        expr.children[1]->type != Type::I32)
      fail(expr.token, "arithmetic requires i32 operands");
    expr.type = Type::I32;
    break;
  case Expr::Compare:
    checkExpr(*expr.children[0], function, locals, signatures);
    checkExpr(*expr.children[1], function, locals, signatures);
    if (expr.children[0]->type != Type::I32 ||
        expr.children[1]->type != Type::I32)
      fail(expr.token, "comparison requires i32 operands");
    expr.type = Type::Bool;
    break;
  case Expr::Logical:
    checkExpr(*expr.children[0], function, locals, signatures);
    checkExpr(*expr.children[1], function, locals, signatures);
    if (expr.children[0]->type != Type::Bool ||
        expr.children[1]->type != Type::Bool)
      fail(expr.token, "logical operators require bool operands");
    expr.type = Type::Bool;
    break;
  }
}

void checkStatement(Statement &statement, const Function &function,
                    std::map<std::string, Type> &locals,
                    const std::map<std::string, Signature> &signatures) {
  switch (statement.kind) {
  case Statement::Let:
    checkExpr(*statement.expression, function, locals, signatures);
    statement.type = statement.expression->type;
    if (!locals.emplace(statement.token.text, statement.type).second)
      fail(statement.token, "duplicate local '" + statement.token.text + "'");
    break;
  case Statement::Evaluate:
    checkExpr(*statement.expression, function, locals, signatures);
    break;
  case Statement::Return:
    checkExpr(*statement.expression, function, locals, signatures);
    if (statement.expression->type != function.returnType)
      fail(statement.token, "return type mismatch");
    break;
  case Statement::If:
    checkExpr(*statement.condition, function, locals, signatures);
    if (statement.condition->type != Type::Bool)
      fail(statement.condition->token, "if condition must be bool");
    {
      auto saved = locals;
      for (auto &s : statement.body)
        checkStatement(s, function, locals, signatures);
      locals = saved;
    }
    {
      auto saved = locals;
      for (auto &s : statement.elseBody)
        checkStatement(s, function, locals, signatures);
      locals = saved;
    }
    break;
  case Statement::While:
    checkExpr(*statement.condition, function, locals, signatures);
    if (statement.condition->type != Type::Bool)
      fail(statement.condition->token, "while condition must be bool");
    {
      auto saved = locals;
      for (auto &s : statement.body)
        checkStatement(s, function, locals, signatures);
      locals = saved;
    }
    break;
  case Statement::Assign: {
    auto found = locals.find(statement.token.text);
    if (found == locals.end())
      fail(statement.token,
           "unknown variable '" + statement.token.text + "'");
    checkExpr(*statement.expression, function, locals, signatures);
    if (statement.expression->type != found->second)
      fail(statement.token, "assignment type mismatch");
    break;
  }
  }
}

} // namespace

void check(Program &functions) {
  std::map<std::string, Signature> signatures = {
      {"print", {{Type::I32}, Type::I32, {"alloc", "block", "io"}}}};
  for (const auto &function : functions) {
    if (!signatures
             .emplace(function.name.text,
                      Signature{function.paramTypes, function.returnType,
                                function.effects})
             .second)
      fail(function.name,
           "duplicate or reserved function '" + function.name.text + "'");
  }
  auto main = signatures.find("main");
  if (main == signatures.end())
    fail(Token{}, "program must define main");
  if (!main->second.paramTypes.empty())
    fail(Token{}, "main must have no parameters");
  for (auto &function : functions) {
    std::map<std::string, Type> locals;
    for (size_t i = 0; i < function.parameters.size(); ++i) {
      if (!locals.emplace(function.parameters[i].text, function.paramTypes[i])
               .second)
        fail(function.parameters[i],
             "duplicate parameter '" + function.parameters[i].text + "'");
    }
    for (auto &statement : function.body)
      checkStatement(statement, function, locals, signatures);
  }
}

} // namespace faust
