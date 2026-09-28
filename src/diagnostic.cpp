#include "faust/diagnostic.h"

#include <stdexcept>

namespace faust {
[[noreturn]] void fail(const Token &token, const std::string &message) {
  throw std::runtime_error(std::to_string(token.line) + ":" +
                           std::to_string(token.column) +
                           ": error: " + message);
}

} // namespace faust
