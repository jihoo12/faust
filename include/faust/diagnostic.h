#pragma once

#include "faust/token.h"

#include <string>

namespace faust {
[[noreturn]] void fail(const Token &token, const std::string &message);

} // namespace faust
