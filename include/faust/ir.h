#pragma once

#include <llvm/IR/Module.h>

#include <string>

namespace faust {
std::string printIR(llvm::Module &module);

} // namespace faust
