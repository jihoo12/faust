#pragma once

#include "faust/ast.h"

namespace faust {
// Validate names, call arity, entry point, resource contracts, and types.
// Throws a diagnostic on failure. Fills in type fields on the AST.
void check(Program &program);

} // namespace faust
