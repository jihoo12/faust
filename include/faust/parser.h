#pragma once

#include "faust/ast.h"

#include <string>

namespace faust {
// Parse source into an owned AST; report invalid syntax through diagnostics.
Program parse(const std::string &source);

} // namespace faust
