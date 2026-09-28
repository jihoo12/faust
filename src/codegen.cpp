#include "faust/codegen.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

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

  llvm::Type *llvmType(Type type) {
    return type == Type::I32 ? builder.getInt32Ty() : builder.getInt1Ty();
  }

  llvm::Value *emit(const Expr &expr) {
    switch (expr.kind) {
    case Expr::Integer:
      return builder.getInt32(expr.value);
    case Expr::Boolean:
      return builder.getInt1(expr.value != 0);
    case Expr::Variable:
      return locals.at(expr.token.text);
    case Expr::Negate:
      return builder.CreateNeg(emit(*expr.children[0]));
    case Expr::Not:
      return builder.CreateNot(emit(*expr.children[0]));
    case Expr::Binary: {
      auto *left = emit(*expr.children[0]);
      auto *right = emit(*expr.children[1]);
      if (expr.token.text == "+")
        return builder.CreateAdd(left, right);
      if (expr.token.text == "-")
        return builder.CreateSub(left, right);
      if (expr.token.text == "*")
        return builder.CreateMul(left, right);
      if (expr.token.text == "==")
        return builder.CreateICmpEQ(left, right);
      if (expr.token.text == "!=")
        return builder.CreateICmpNE(left, right);
      if (expr.token.text == "<")
        return builder.CreateICmpSLT(left, right);
      if (expr.token.text == "<=")
        return builder.CreateICmpSLE(left, right);
      if (expr.token.text == ">")
        return builder.CreateICmpSGT(left, right);
      if (expr.token.text == ">=")
        return builder.CreateICmpSGE(left, right);
      throw std::runtime_error("internal error: unknown binary operator");
    }
    case Expr::Call: {
      std::vector<llvm::Value *> arguments;
      for (const auto &child : expr.children)
        arguments.push_back(emit(*child));
      if (expr.token.text == "print") {
        auto printfFunction = module.getOrInsertFunction(
            "printf", llvm::FunctionType::get(builder.getInt32Ty(),
                                              {builder.getPtrTy()}, true));
        auto *format = builder.CreateGlobalString("%d\n", "format");
        builder.CreateCall(printfFunction, {format, arguments[0]});
        return builder.getInt32(0);
      }
      return builder.CreateCall(functions.at(expr.token.text), arguments);
    }
    }
    throw std::runtime_error("internal error: unknown expression");
  }

public:
  std::string generate(const std::vector<Function> &program) {
    for (const auto &function : program) {
      std::vector<llvm::Type *> parameters;
      for (const auto &parameter : function.parameters)
        parameters.push_back(llvmType(parameter.type));
      auto *type =
          llvm::FunctionType::get(llvmType(function.returnType), parameters, false);
      bool isMain = function.name.text == "main";
      functions[function.name.text] = llvm::Function::Create(
          type,
          isMain ? llvm::Function::ExternalLinkage
                 : llvm::Function::InternalLinkage,
          isMain ? "main" : "faust." + function.name.text, module);
    }
    for (const auto &function : program) {
      auto *target = functions.at(function.name.text);
      builder.SetInsertPoint(
          llvm::BasicBlock::Create(context, "entry", target));
      locals.clear();
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        target->getArg(i)->setName(function.parameters[i].name.text);
        locals[function.parameters[i].name.text] = target->getArg(i);
      }
      for (const auto &statement : function.body) {
        auto *value = emit(*statement.expression);
        if (statement.kind == Statement::Let)
          locals[statement.token.text] = value;
        if (statement.kind == Statement::Return)
          builder.CreateRet(value);
      }
    }
    if (llvm::verifyModule(module, &llvm::errs()))
      throw std::runtime_error("internal error: invalid LLVM module");
    std::string output;
    llvm::raw_string_ostream stream(output);
    module.print(stream, nullptr);
    return output;
  }
};
} // namespace

std::string generateIR(const Program &program) {
  return Generator().generate(program);
}

} // namespace faust
