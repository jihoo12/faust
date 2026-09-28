#pragma once

#include "faust/ast.h"

namespace faust {
// Validate names, call arity, entry point, and resource contracts.
// Throws a diagnostic on failure; leaves the AST unchanged.
void check(const Program &program);

} // namespace faust
