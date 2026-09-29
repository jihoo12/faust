#include "faust/codegen.h"
#include "faust/ir.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <map>
#include <stdexcept>

namespace faust {
namespace {
class Generator {
  llvm::LLVMContext context;
  llvm::Module module{"faust", context};
  llvm::IRBuilder<> builder{context};
  std::map<std::string, llvm::Function *> functions;
  std::map<std::string, llvm::Value *> locals;
  llvm::Function *currentFunction = nullptr;

  llvm::Type *llvmType(Type type) {
    return type == Type::Bool ? builder.getInt1Ty() : builder.getInt32Ty();
  }

  llvm::Value *emitExpr(const Expr &expr) {
    switch (expr.kind) {
    case Expr::Integer:
      return builder.getInt32(expr.value);
    case Expr::Boolean:
      return builder.getInt1(expr.value != 0);
    case Expr::String: {
      std::string text = expr.token.text;
      if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
      std::string processed;
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
          ++i;
          if (text[i] == 'n') processed += '\n';
          else if (text[i] == 't') processed += '\t';
          else if (text[i] == 'r') processed += '\r';
          else if (text[i] == '\\') processed += '\\';
          else if (text[i] == '"') processed += '"';
          else processed += text[i];
        } else {
          processed += text[i];
        }
      }
      return builder.CreateGlobalString(processed);
    }
    case Expr::Variable:
      return locals.at(expr.token.text);
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
      for (const auto &child : expr.children)
        arguments.push_back(emitExpr(*child));
      return builder.CreateCall(functions.at(expr.token.text), arguments);
    }
    }
    throw std::runtime_error("internal error: unknown expression");
  }

  void emitStatement(const Statement &statement) {
    switch (statement.kind) {
    case Statement::Let:
      locals[statement.token.text] = emitExpr(*statement.expression);
      break;
    case Statement::Evaluate:
      emitExpr(*statement.expression);
      break;
    case Statement::Return:
      builder.CreateRet(emitExpr(*statement.expression));
      break;
    case Statement::Assign:
      locals[statement.token.text] = emitExpr(*statement.expression);
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
        emitStatement(s);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(elseBlock);
      for (const auto &s : statement.elseBody)
        emitStatement(s);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(mergeBlock);
      break;
    }
    case Statement::While: {
      auto *predBlock = builder.GetInsertBlock();
      auto *loopBlock =
          llvm::BasicBlock::Create(context, "loop", currentFunction);
      auto *bodyBlock =
          llvm::BasicBlock::Create(context, "body", currentFunction);
      auto *exitBlock =
          llvm::BasicBlock::Create(context, "exit", currentFunction);
      builder.CreateBr(loopBlock);
      builder.SetInsertPoint(loopBlock);
      std::vector<std::string> modifiedVars;
      for (const auto &s : statement.body) {
        if (s.kind == Statement::Assign)
          modifiedVars.push_back(s.token.text);
      }
      std::vector<llvm::PHINode *> phis;
      for (const auto &var : modifiedVars) {
        auto *phi = builder.CreatePHI(locals[var]->getType(), 2,
                                      var + ".phi");
        phi->addIncoming(locals[var], predBlock);
        phis.push_back(phi);
        locals[var] = phi;
      }
      auto *cond = emitExpr(*statement.condition);
      builder.CreateCondBr(cond, bodyBlock, exitBlock);
      builder.SetInsertPoint(bodyBlock);
      for (const auto &s : statement.body)
        emitStatement(s);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(loopBlock);
      for (size_t i = 0; i < modifiedVars.size(); ++i) {
        phis[i]->addIncoming(locals[modifiedVars[i]],
                              builder.GetInsertBlock());
      }
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
      for (size_t i = 0; i < function.paramTypes.size(); ++i)
        parameters.push_back(function.paramIsPointer[i] ? builder.getPtrTy()
                                                        : llvmType(function.paramTypes[i]));
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
        locals[function.parameters[i].text] = target->getArg(i);
      }
      for (const auto &statement : function.body)
        emitStatement(statement);
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
