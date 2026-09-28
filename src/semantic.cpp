#include "faust/semantic.h"
#include "faust/diagnostic.h"

#include <map>
#include <set>

namespace faust {
namespace {
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

} // namespace

void check(const Program &functions) {
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

} // namespace faust
