#pragma once

#include "faust/token.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace faust {
enum class Type { I32, Bool };

struct Expr {
  enum Kind { Integer, Boolean, Variable, Call, Binary, Negate, Not, Compare,
              Logical } kind;
  Type type = Type::I32;
  Token token;
  int32_t value = 0;
  std::vector<std::unique_ptr<Expr>> children;
};
struct Statement {
  enum Kind { Let, Evaluate, Return, If, While, Assign } kind;
  Type type = Type::I32;
  Token token;
  std::unique_ptr<Expr> expression;
  std::unique_ptr<Expr> condition;
  std::vector<Statement> body;
  std::vector<Statement> elseBody;
};
struct Function {
  Token name;
  std::vector<Token> parameters;
  std::vector<Type> paramTypes;
  Type returnType = Type::I32;
  std::set<std::string> effects;
  std::vector<Statement> body;
};

using Program = std::vector<Function>;

} // namespace faust
