#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

int main() {
  llvm::LLVMContext context;
  llvm::Module module("llvm-demo", context);
  llvm::IRBuilder<> builder(context);
  auto *i32 = builder.getInt32Ty();

  // Build an addition function with runtime parameters to preserve the add IR.
  auto *addType = llvm::FunctionType::get(i32, {i32, i32}, false);
  auto *add = llvm::Function::Create(
      addType, llvm::Function::ExternalLinkage, "add", module);
  add->getArg(0)->setName("a");
  add->getArg(1)->setName("b");
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", add));
  builder.CreateRet(builder.CreateAdd(add->getArg(0), add->getArg(1), "sum"));

  // The generated main prints the result through the host C library.
  auto printfFunction = module.getOrInsertFunction(
      "printf", llvm::FunctionType::get(i32, {builder.getPtrTy()}, true));
  auto *mainFunction = llvm::Function::Create(
      llvm::FunctionType::get(i32, false), llvm::Function::ExternalLinkage,
      "main", module);
  builder.SetInsertPoint(
      llvm::BasicBlock::Create(context, "entry", mainFunction));
  auto *result = builder.CreateCall(
      add, {builder.getInt32(20), builder.getInt32(22)}, "result");
  auto *format = builder.CreateGlobalString("%d\n", "format");
  builder.CreateCall(printfFunction, {format, result});
  builder.CreateRet(builder.getInt32(0));

  if (llvm::verifyModule(module, &llvm::errs())) {
    return 1;
  }
  module.print(llvm::outs(), nullptr);
  return 0;
}
