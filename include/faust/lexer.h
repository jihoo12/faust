#pragma once

#include "faust/token.h"

#include <string>
#include <vector>

namespace faust {
// The final token has empty text and marks the end of input.
std::vector<Token> lex(const std::string &source);

} // namespace faust
