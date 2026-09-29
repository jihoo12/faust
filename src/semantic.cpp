#include "faust/semantic.h"
#include "faust/diagnostic.h"

#include <map>
#include <set>

namespace faust {
namespace {
struct Signature {
  std::vector<Type> paramTypes;
  std::vector<bool> paramIsPointer;
  Type returnType;
  std::set<int> syscalls;
  bool hasAsm;
  bool isVariadic;
};

bool isNumeric(Type type) {
  return type == Type::I8 || type == Type::U8 || type == Type::I16 ||
         type == Type::U16 || type == Type::I32 || type == Type::U32 ||
         type == Type::I64 || type == Type::U64 || type == Type::F32 ||
         type == Type::F64;
}

bool isIntegerType(Type type) {
  return type == Type::I8 || type == Type::U8 || type == Type::I16 ||
         type == Type::U16 || type == Type::I32 || type == Type::U32 ||
         type == Type::I64 || type == Type::U64;
}

bool isFloatType(Type type) {
  return type == Type::F32 || type == Type::F64;
}

bool fitsInRange(int64_t value, Type type) {
  switch (type) {
  case Type::I8: return value >= -128 && value <= 127;
  case Type::U8: return value >= 0 && value <= 255;
  case Type::I16: return value >= -32768 && value <= 32767;
  case Type::U16: return value >= 0 && value <= 65535;
  case Type::I32: return value >= INT32_MIN && value <= INT32_MAX;
  case Type::U32: return value >= 0 && value <= 4294967295LL;
  case Type::I64: return true;
  case Type::U64: return value >= 0;
  default: return true;
  }
}

const char *typeName(Type type) {
  switch (type) {
  case Type::I8: return "i8";
  case Type::U8: return "u8";
  case Type::I16: return "i16";
  case Type::U16: return "u16";
  case Type::I32: return "i32";
  case Type::U32: return "u32";
  case Type::I64: return "i64";
  case Type::U64: return "u64";
  case Type::F32: return "f32";
  case Type::F64: return "f64";
  case Type::Bool: return "bool";
  case Type::Void: return "void";
  }
  return "unknown";
}

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
  case Expr::String:
    expr.type = Type::I32;
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
    if (expr.children.size() < found->second.paramTypes.size() ||
        (!found->second.isVariadic && expr.children.size() != found->second.paramTypes.size()))
      fail(expr.token,
           "wrong number of arguments to '" + expr.token.text + "'");
    for (int syscall : found->second.syscalls) {
      if (!function.syscalls.count(syscall))
        fail(expr.token, "call to '" + expr.token.text + "' requires syscall " +
                              std::to_string(syscall) + " in contract of '" +
                              function.name.text + "'");
    }
    if (found->second.hasAsm && !function.hasAsm)
      fail(expr.token, "call to '" + expr.token.text + "' requires asm effect in contract of '" +
                            function.name.text + "'");
    for (size_t i = 0; i < found->second.paramTypes.size(); ++i) {
      checkExpr(*expr.children[i], function, locals, signatures);
      if (expr.children[i]->type != found->second.paramTypes[i]) {
        bool isNullPointer = found->second.paramIsPointer[i] &&
                             expr.children[i]->kind == Expr::Integer &&
                             expr.children[i]->value == 0;
        bool isStringAsPointer = found->second.paramIsPointer[i] &&
                                 expr.children[i]->kind == Expr::String;
        if (!isNullPointer && !isStringAsPointer)
          fail(expr.children[i]->token,
               "wrong type for argument " + std::to_string(i + 1) + " to '" +
                   expr.token.text + "'");
      }
    }
    for (size_t i = found->second.paramTypes.size(); i < expr.children.size(); ++i)
      checkExpr(*expr.children[i], function, locals, signatures);
    expr.type = found->second.returnType;
    break;
  }
  case Expr::Negate:
    checkExpr(*expr.children[0], function, locals, signatures);
    if (!isNumeric(expr.children[0]->type))
      fail(expr.token, "unary '-' requires numeric type");
    expr.type = expr.children[0]->type;
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
    if (!isNumeric(expr.children[0]->type) ||
        !isNumeric(expr.children[1]->type))
      fail(expr.token, "arithmetic requires numeric operands");
    expr.type = expr.children[0]->type;
    break;
  case Expr::Compare:
    checkExpr(*expr.children[0], function, locals, signatures);
    checkExpr(*expr.children[1], function, locals, signatures);
    if (!isNumeric(expr.children[0]->type) ||
        !isNumeric(expr.children[1]->type))
      fail(expr.token, "comparison requires numeric operands");
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
    if (statement.hasTypeAnnotation) {
      if (statement.type != statement.expression->type) {
        bool isIntLiteral = statement.expression->kind == Expr::Integer;
        bool isCompatible = isIntLiteral && isIntegerType(statement.type);
        bool isIntToFloat = isIntLiteral && isFloatType(statement.type);
        if (!isCompatible && !isIntToFloat)
          fail(statement.token, "assignment type mismatch");
      }
      if (statement.expression->kind == Expr::Integer && isIntegerType(statement.type)) {
        int64_t val = statement.expression->value;
        if (!fitsInRange(val, statement.type))
          fail(statement.token, "integer literal is outside the range of " + std::string(typeName(statement.type)));
      }
    } else {
      statement.type = statement.expression->type;
    }
    if (!locals.emplace(statement.token.text, statement.type).second)
      fail(statement.token, "duplicate local '" + statement.token.text + "'");
    break;
  case Statement::Evaluate:
    checkExpr(*statement.expression, function, locals, signatures);
    break;
  case Statement::Return:
    if (function.returnType == Type::Void) {
      if (statement.expression)
        fail(statement.token, "void function cannot return a value");
    } else {
      if (!statement.expression)
        fail(statement.token, "return statement requires an expression");
      checkExpr(*statement.expression, function, locals, signatures);
      if (statement.expression->type != function.returnType) {
        bool isIntToI32 = function.returnType == Type::I32 &&
                         isIntegerType(statement.expression->type);
        if (!isIntToI32)
          fail(statement.token, "return type mismatch");
      }
    }
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
  case Statement::Asm:
    if (!function.hasAsm)
      fail(statement.token, "asm block requires asm effect in contract of '" +
                            function.name.text + "'");
    break;
  }
}

} // namespace

void check(Program &functions) {
  std::map<std::string, Signature> signatures;
  for (const auto &function : functions) {
    if (!signatures
             .emplace(function.name.text,
                      Signature{function.paramTypes, function.paramIsPointer,
                                function.returnType, function.syscalls,
                                function.hasAsm, function.isVariadic})
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
    if (function.isExtern)
      continue;
    if (!function.syscalls.empty() && !function.hasAsm)
      fail(function.name, "function declares syscalls but missing asm effect");
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
