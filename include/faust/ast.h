#pragma once

#include "faust/token.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace faust {
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

using Program = std::vector<Function>;

} // namespace faust
