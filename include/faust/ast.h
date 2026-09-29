#pragma once

#include "faust/token.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace faust {
struct Type {
  enum Kind { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Bool,
              Pointer, Array, Void } kind = I32;
  std::shared_ptr<Type> element;
  size_t length = 0;

  Type() = default;
  Type(Kind kind) : kind(kind) {}

  static Type pointer(Type pointee) {
    Type type(Pointer);
    type.element = std::make_shared<Type>(std::move(pointee));
    return type;
  }
  static Type array(Type elementType, size_t length) {
    Type type(Array);
    type.element = std::make_shared<Type>(std::move(elementType));
    type.length = length;
    return type;
  }
};

inline bool operator==(const Type &a, const Type &b) {
  if (a.kind != b.kind || a.length != b.length)
    return false;
  if (a.element && b.element)
    return *a.element == *b.element;
  return !a.element && !b.element;
}
inline bool operator!=(const Type &a, const Type &b) { return !(a == b); }

struct Expr {
  enum Kind { Integer, Boolean, String, Variable, Call, Binary, Negate, Not,
              AddressOf, Dereference, ArrayLiteral, Index, Compare, Logical } kind;
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
  Type returnType = Type::I32;
  std::set<int> syscalls;
  bool hasAsm = false;
  bool hasExtern = false;
  bool isExtern = false;
  bool isVariadic = false;
  std::vector<Statement> body;
  std::set<std::string> storedLocals;
};

using Program = std::vector<Function>;

} // namespace faust
