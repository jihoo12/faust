#include "faust/codegen.h"
#include "faust/parser.h"
#include "faust/semantic.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
  std::string input, output;
  bool checkOnly = false;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--help") {
      std::cout << "Usage: faust [--check] <source.faust> [-o output.ll]\n";
      return 0;
    }
    if (argument == "--check")
      checkOnly = true;
    else if (argument == "-o" && i + 1 < argc)
      output = argv[++i];
    else if (!argument.empty() && argument[0] != '-' && input.empty())
      input = argument;
    else {
      std::cerr << "error: unexpected argument '" << argument << "'\n";
      return 1;
    }
  }
  if (input.empty() || (checkOnly && !output.empty())) {
    std::cerr << "Usage: faust [--check] <source.faust> [-o output.ll]\n";
    return 1;
  }
  try {
    std::ifstream file(input);
    if (!file)
      throw std::runtime_error("cannot open input file");
    std::string source((std::istreambuf_iterator<char>(file)), {});
    if (file.bad())
      throw std::runtime_error("cannot read input file");
    auto program = faust::parse(source);
    faust::check(program);
    if (checkOnly)
      return 0;
    auto ir = faust::generateIR(program);
    if (output.empty()) {
      std::cout << ir;
      if (!std::cout)
        throw std::runtime_error("cannot write IR to standard output");
    } else {
      std::ofstream file(output);
      file << ir;
      file.close();
      if (!file)
        throw std::runtime_error("cannot write output file '" + output + "'");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << input << ":" << error.what() << '\n';
    return 1;
  }
}
