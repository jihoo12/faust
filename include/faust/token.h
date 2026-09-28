#pragma once

#include <string>

namespace faust {
struct Token {
  std::string text;
  unsigned line = 1;
  unsigned column = 1;
};

} // namespace faust
