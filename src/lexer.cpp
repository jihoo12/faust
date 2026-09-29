#include "faust/lexer.h"
#include "faust/diagnostic.h"

#include <cctype>
#include <utility>

namespace faust {
std::vector<Token> lex(const std::string &source) {
  std::vector<Token> tokens;
  unsigned line = 1, column = 1;
  size_t pos = 0;
  auto advance = [&] {
    if (source[pos++] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  };
  while (pos < source.size()) {
    unsigned char ch = source[pos];
    if (std::isspace(ch)) {
      advance();
      continue;
    }
    if (source.compare(pos, 2, "//") == 0) {
      while (pos < source.size() && source[pos] != '\n')
        advance();
      continue;
    }
    Token token{"", line, column};
    size_t start = pos;
    if (std::isalpha(ch) || ch == '_') {
      do {
        advance();
      } while (pos < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[pos])) ||
                source[pos] == '_'));
    } else if (std::isdigit(ch)) {
      do {
        advance();
      } while (pos < source.size() &&
               std::isdigit(static_cast<unsigned char>(source[pos])));
    } else if (source.compare(pos, 2, "->") == 0) {
      advance();
      advance();
    } else if (source.compare(pos, 2, "&&") == 0) {
      advance();
      advance();
    } else if (source.compare(pos, 2, "||") == 0) {
      advance();
      advance();
    } else if (source.compare(pos, 2, "<=") == 0) {
      advance();
      advance();
    } else if (source.compare(pos, 2, ">=") == 0) {
      advance();
      advance();
    } else if (source.compare(pos, 2, "==") == 0) {
      advance();
      advance();
    } else if (source.compare(pos, 2, "!=") == 0) {
      advance();
      advance();
    } else if (ch == '"') {
      advance();
      while (pos < source.size() && source[pos] != '"') {
        if (source[pos] == '\\' && pos + 1 < source.size()) {
          advance();
          advance();
        } else {
          advance();
        }
      }
      if (pos < source.size())
        advance();
    } else if (source.compare(pos, 3, "...") == 0) {
      advance();
      advance();
      advance();
    } else if (std::string("(){}:,;=!+-*<>").find(ch) != std::string::npos) {
      advance();
    } else {
      fail(token, "unexpected character");
    }
    token.text = source.substr(start, pos - start);
    tokens.push_back(std::move(token));
  }
  tokens.push_back({"", line, column});
  return tokens;
}

} // namespace faust
