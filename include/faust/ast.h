#pragma once

#include "faust/token.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace faust {
enum class Type { I32, Bool };

struct Parameter {
  Token name;
  Type type = Type::I32;
};

struct Expr {
  enum Kind { Integer, Boolean, Variable, Call, Binary, Negate, Not } kind;
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
  std::vector<Parameter> parameters;
  Type returnType = Type::I32;
  std::set<std::string> effects;
  std::vector<Statement> body;
};

using Program = std::vector<Function>;

} // namespace faust
