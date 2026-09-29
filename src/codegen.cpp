#include "faust/codegen.h"
#include "faust/ir.h"
#include "faust/string_literal.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <map>
#include <stdexcept>

namespace faust {
namespace {

bool isFloatType(Type type) {
  return type.kind == Type::F32 || type.kind == Type::F64;
}

class Generator {
  llvm::LLVMContext context;
  llvm::Module module{"faust", context};
  llvm::IRBuilder<> builder{context};
  std::map<std::string, llvm::Function *> functions;
  std::map<std::string, llvm::AllocaInst *> locals;
  llvm::Function *currentFunction = nullptr;

  llvm::Type *llvmType(Type type) {
    switch (type.kind) {
    case Type::I8: return builder.getInt8Ty();
    case Type::U8: return builder.getInt8Ty();
    case Type::I16: return builder.getInt16Ty();
    case Type::U16: return builder.getInt16Ty();
    case Type::I32: return builder.getInt32Ty();
    case Type::U32: return builder.getInt32Ty();
    case Type::I64: return builder.getInt64Ty();
    case Type::U64: return builder.getInt64Ty();
    case Type::F32: return builder.getFloatTy();
    case Type::F64: return builder.getDoubleTy();
    case Type::Bool: return builder.getInt1Ty();
    case Type::Pointer: return builder.getPtrTy();
    case Type::Array:
      return llvm::ArrayType::get(llvmType(*type.element), type.length);
    case Type::Void: return builder.getVoidTy();
    }
    return builder.getInt32Ty();
  }

  llvm::Value *convertValue(llvm::Value *value, Type target) {
    auto *targetType = llvmType(target);
    if (value->getType() == targetType)
      return value;
    if (value->getType()->isIntegerTy() && targetType->isIntegerTy())
      return builder.CreateIntCast(value, targetType, true);
    if (value->getType()->isIntegerTy() && targetType->isFloatingPointTy())
      return builder.CreateSIToFP(value, targetType);
    return value;
  }

  llvm::Value *emitAddress(const Expr &expr) {
    switch (expr.kind) {
    case Expr::Variable:
      return locals.at(expr.token.text);
    case Expr::Dereference:
      return emitExpr(*expr.children[0]);
    case Expr::Index: {
      const Expr &base = *expr.children[0];
      auto *baseAddress = emitAddress(base);
      auto *index = emitExpr(*expr.children[1]);
      if (!index->getType()->isIntegerTy(64))
        index = builder.CreateIntCast(index, builder.getInt64Ty(), true);
      return builder.CreateInBoundsGEP(
          llvmType(base.type), baseAddress,
          {builder.getInt64(0), index});
    }
    default:
      throw std::runtime_error("internal error: expression is not addressable");
    }
  }

  llvm::Value *emitArrayAddress(const Expr &expr) {
    if (expr.kind == Expr::String)
      return builder.CreateGlobalString(decodeStringLiteral(expr.token));
    return emitAddress(expr);
  }

  llvm::Value *emitArrayDecay(const Expr &expr) {
    auto *address = emitArrayAddress(expr);
    if (expr.kind == Expr::String)
      return address;
    return builder.CreateInBoundsGEP(
        llvmType(expr.type), address,
        {builder.getInt64(0), builder.getInt64(0)});
  }

  llvm::Value *emitExpr(const Expr &expr) {
    switch (expr.kind) {
    case Expr::Integer: {
      llvm::Type *ty = llvmType(expr.type);
      if (expr.type.kind == Type::F32 || expr.type.kind == Type::F64)
        return llvm::ConstantFP::get(ty, static_cast<double>(expr.value));
      return llvm::ConstantInt::get(ty, expr.value, true);
    }
    case Expr::Boolean:
      return builder.getInt1(expr.value != 0);
    case Expr::String:
      return builder.CreateLoad(llvmType(expr.type), emitArrayAddress(expr));
    case Expr::Variable: {
      auto *slot = locals.at(expr.token.text);
      return builder.CreateLoad(slot->getAllocatedType(), slot, expr.token.text);
    }
    case Expr::ArrayLiteral: {
      auto *arrayType = llvmType(expr.type);
      llvm::Value *value = llvm::UndefValue::get(arrayType);
      for (size_t i = 0; i < expr.children.size(); ++i)
        value = builder.CreateInsertValue(
            value, convertValue(emitExpr(*expr.children[i]), *expr.type.element),
            {static_cast<unsigned>(i)});
      return value;
    }
    case Expr::Index:
      return builder.CreateLoad(llvmType(expr.type), emitAddress(expr));
    case Expr::AddressOf:
      return emitAddress(*expr.children[0]);
    case Expr::Dereference: {
      auto *pointer = emitExpr(*expr.children[0]);
      return builder.CreateLoad(llvmType(expr.type), pointer);
    }
    case Expr::Negate:
      return builder.CreateNeg(emitExpr(*expr.children[0]));
    case Expr::Not:
      return builder.CreateXor(emitExpr(*expr.children[0]),
                               builder.getInt1(true));
    case Expr::Binary: {
      auto *left = emitExpr(*expr.children[0]);
      auto *right = emitExpr(*expr.children[1]);
      if (expr.token.text == "+")
        return builder.CreateAdd(left, right);
      if (expr.token.text == "-")
        return builder.CreateSub(left, right);
      return builder.CreateMul(left, right);
    }
    case Expr::Compare: {
      auto *left = emitExpr(*expr.children[0]);
      auto *right = emitExpr(*expr.children[1]);
      if (expr.token.text == "<")
        return builder.CreateICmpSLT(left, right);
      if (expr.token.text == ">")
        return builder.CreateICmpSGT(left, right);
      if (expr.token.text == "<=")
        return builder.CreateICmpSLE(left, right);
      if (expr.token.text == ">=")
        return builder.CreateICmpSGE(left, right);
      if (expr.token.text == "==")
        return builder.CreateICmpEQ(left, right);
      return builder.CreateICmpNE(left, right);
    }
    case Expr::Logical: {
      auto *left = emitExpr(*expr.children[0]);
      bool isAnd = expr.token.text == "&&";
      auto *rhsBlock =
          llvm::BasicBlock::Create(context, "rhs", currentFunction);
      auto *shortBlock =
          llvm::BasicBlock::Create(context, "short", currentFunction);
      auto *mergeBlock =
          llvm::BasicBlock::Create(context, "merge", currentFunction);
      builder.CreateCondBr(left, isAnd ? rhsBlock : shortBlock,
                           isAnd ? shortBlock : rhsBlock);
      builder.SetInsertPoint(rhsBlock);
      auto *right = emitExpr(*expr.children[1]);
      builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(shortBlock);
      builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(mergeBlock);
      auto *phi = builder.CreatePHI(builder.getInt1Ty(), 2);
      if (isAnd) {
        phi->addIncoming(right, rhsBlock);
        phi->addIncoming(builder.getInt1(false), shortBlock);
      } else {
        phi->addIncoming(builder.getInt1(true), shortBlock);
        phi->addIncoming(right, rhsBlock);
      }
      return phi;
    }
    case Expr::Call: {
      std::vector<llvm::Value *> arguments;
      auto *callee = functions.at(expr.token.text);
      for (size_t i = 0; i < expr.children.size(); ++i) {
        const auto &child = *expr.children[i];
        if (i < callee->arg_size() && child.type.kind == Type::Array &&
            callee->getFunctionType()->getParamType(i)->isPointerTy())
          arguments.push_back(emitArrayDecay(child));
        else
          arguments.push_back(emitExpr(child));
      }
      return builder.CreateCall(callee, arguments);
    }
    }
    throw std::runtime_error("internal error: unknown expression");
  }

  void emitStatement(const Statement &statement, const Function &function) {
    switch (statement.kind) {
    case Statement::Let: {
      auto *value = emitExpr(*statement.expression);
      auto *slot = builder.CreateAlloca(llvmType(statement.type), nullptr,
                                        statement.token.text);
      builder.CreateStore(convertValue(value, statement.type), slot);
      locals[statement.token.text] = slot;
      break;
    }
    case Statement::Evaluate:
      emitExpr(*statement.expression);
      break;
    case Statement::Return:
      if (function.returnType == Type::Void)
        builder.CreateRetVoid();
      else {
        auto *retVal = emitExpr(*statement.expression);
        if (retVal->getType() != llvmType(function.returnType)) {
          if (isFloatType(function.returnType))
            retVal = builder.CreateSIToFP(retVal, llvmType(function.returnType));
          else
            retVal = builder.CreateSExt(retVal, llvmType(function.returnType));
        }
        builder.CreateRet(retVal);
      }
      break;
    case Statement::Assign:
      builder.CreateStore(
          convertValue(emitExpr(*statement.expression), statement.condition->type),
          emitAddress(*statement.condition));
      break;
    case Statement::If: {
      auto *cond = emitExpr(*statement.condition);
      auto *thenBlock =
          llvm::BasicBlock::Create(context, "then", currentFunction);
      auto *elseBlock =
          llvm::BasicBlock::Create(context, "else", currentFunction);
      auto *mergeBlock =
          llvm::BasicBlock::Create(context, "merge", currentFunction);
      builder.CreateCondBr(cond, thenBlock, elseBlock);
      builder.SetInsertPoint(thenBlock);
      for (const auto &s : statement.body)
        emitStatement(s, function);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(elseBlock);
      for (const auto &s : statement.elseBody)
        emitStatement(s, function);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(mergeBlock);
      break;
    }
    case Statement::While: {
      auto *loopBlock =
          llvm::BasicBlock::Create(context, "loop", currentFunction);
      auto *bodyBlock =
          llvm::BasicBlock::Create(context, "body", currentFunction);
      auto *exitBlock =
          llvm::BasicBlock::Create(context, "exit", currentFunction);
      builder.CreateBr(loopBlock);
      builder.SetInsertPoint(loopBlock);
      auto *cond = emitExpr(*statement.condition);
      builder.CreateCondBr(cond, bodyBlock, exitBlock);
      builder.SetInsertPoint(bodyBlock);
      for (const auto &s : statement.body)
        emitStatement(s, function);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(loopBlock);
      builder.SetInsertPoint(exitBlock);
      break;
    }
    case Statement::Asm: {
      std::string asmStr = statement.asmCode;
      if (!asmStr.empty() && asmStr.front() == '"')
        asmStr = asmStr.substr(1);
      if (!asmStr.empty() && asmStr.back() == '"')
        asmStr.pop_back();
      std::string constraints = statement.asmOutputs;
      if (!statement.asmInputs.empty())
        constraints += "," + statement.asmInputs;
      bool hasOutputs = !statement.asmOutputs.empty();
      auto *retTy = hasOutputs ? builder.getInt32Ty() : builder.getVoidTy();
      auto *asmTy = llvm::FunctionType::get(retTy, {}, false);
      auto *asmFn = llvm::InlineAsm::get(asmTy, asmStr, constraints, true);
      builder.CreateCall(asmFn, {});
      break;
    }
    }
  }

public:
  std::string generate(const std::vector<Function> &program) {
    for (const auto &function : program) {
      std::vector<llvm::Type *> parameters;
      for (Type type : function.paramTypes)
        parameters.push_back(llvmType(type));
      auto *type = llvm::FunctionType::get(
          function.isExtern ? builder.getInt32Ty()
                             : llvmType(function.returnType),
          parameters, function.isVariadic);
      bool isMain = function.name.text == "main";
      llvm::Function::LinkageTypes linkage = llvm::Function::InternalLinkage;
      if (isMain || function.isExtern)
        linkage = llvm::Function::ExternalLinkage;
      std::string irName = isMain ? "main" : "faust." + function.name.text;
      if (function.isExtern)
        irName = function.name.text;
      functions[function.name.text] = llvm::Function::Create(
          type, linkage, irName, module);
    }
    for (const auto &function : program) {
      if (function.isExtern)
        continue;
      auto *target = functions.at(function.name.text);
      currentFunction = target;
      builder.SetInsertPoint(
          llvm::BasicBlock::Create(context, "entry", target));
      locals.clear();
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        target->getArg(i)->setName(function.parameters[i].text);
        auto *slot = builder.CreateAlloca(llvmType(function.paramTypes[i]), nullptr,
                                          function.parameters[i].text);
        builder.CreateStore(target->getArg(i), slot);
        locals[function.parameters[i].text] = slot;
      }
      for (const auto &statement : function.body)
        emitStatement(statement, function);
    }
    if (llvm::verifyModule(module, &llvm::errs()))
      throw std::runtime_error("internal error: invalid LLVM module");
    return printIR(module);
  }
};
} // namespace

std::string generateIR(const Program &program) {
  return Generator().generate(program);
}

} // namespace faust
