#pragma once

#include "faust/token.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace faust {
enum class Type { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Bool, Void };

struct Expr {
  enum Kind { Integer, Boolean, String, Variable, Call, Binary, Negate, Not,
              Compare, Logical } kind;
  Type type = Type::I32;
  Token token;
  int32_t value = 0;
  std::vector<std::unique_ptr<Expr>> children;
};
struct Statement {
  enum Kind { Let, Evaluate, Return, If, While, Assign, Asm } kind;
  Type type = Type::I32;
  bool hasTypeAnnotation = false;
  Token token;
  std::unique_ptr<Expr> expression;
  std::unique_ptr<Expr> condition;
  std::vector<Statement> body;
  std::vector<Statement> elseBody;
  std::string asmCode;
  std::string asmOutputs;
  std::string asmInputs;
};
struct Function {
  Token name;
  std::vector<Token> parameters;
  std::vector<Type> paramTypes;
  std::vector<bool> paramIsPointer;
  Type returnType = Type::I32;
  std::set<int> syscalls;
  bool hasAsm = false;
  bool isExtern = false;
  bool isVariadic = false;
  std::vector<Statement> body;
};

using Program = std::vector<Function>;

} // namespace faust
