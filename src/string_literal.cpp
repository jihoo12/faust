#include "faust/string_literal.h"

namespace faust {

std::string decodeStringLiteral(const Token &token) {
  std::string text = token.text;
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    text = text.substr(1, text.size() - 2);

  std::string result;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      result += text[i];
      continue;
    }
    switch (text[++i]) {
    case 'n': result += '\n'; break;
    case 't': result += '\t'; break;
    case 'r': result += '\r'; break;
    case '\\': result += '\\'; break;
    case '"': result += '"'; break;
    default: result += text[i]; break;
    }
  }
  return result;
}

} // namespace faust
