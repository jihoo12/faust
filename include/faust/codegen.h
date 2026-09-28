#pragma once

#include "faust/ast.h"

#include <string>

namespace faust {
// Emit verified LLVM IR. The program must have passed check() first.
// LLVM implementation types stay private to the backend.
std::string generateIR(const Program &program);

} // namespace faust
