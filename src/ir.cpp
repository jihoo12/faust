#include "faust/ir.h"

#include <llvm/Support/raw_ostream.h>

namespace faust {

std::string printIR(llvm::Module &module) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  module.print(stream, nullptr);
  return output;
}

} // namespace faust
