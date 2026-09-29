#include "faust/codegen.h"
#include "faust/parser.h"
#include "faust/semantic.h"
#include "faust/string_literal.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace {
faust::Program loadProgram(const std::filesystem::path &path,
                           std::set<std::filesystem::path> &loaded,
                           std::set<std::filesystem::path> &active) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error)
    canonical = std::filesystem::absolute(path);

  if (active.count(canonical))
    throw std::runtime_error("cyclic include involving '" + canonical.string() + "'");
  if (loaded.count(canonical))
    return {};

  std::ifstream file(canonical);
  if (!file)
    throw std::runtime_error("cannot open included file '" + canonical.string() + "'");
  std::string source((std::istreambuf_iterator<char>(file)), {});
  if (file.bad())
    throw std::runtime_error("cannot read included file '" + canonical.string() + "'");

  active.insert(canonical);
  auto parsed = faust::parse(source);
  faust::Program result;
  for (const auto &include : parsed.includes) {
    auto includePath =
        canonical.parent_path() / faust::decodeStringLiteral(include);
    auto child = loadProgram(includePath, loaded, active);
    result.structs.insert(result.structs.end(),
                          std::make_move_iterator(child.structs.begin()),
                          std::make_move_iterator(child.structs.end()));
    result.functions.insert(result.functions.end(),
                            std::make_move_iterator(child.functions.begin()),
                            std::make_move_iterator(child.functions.end()));
  }
  result.structs.insert(result.structs.end(),
                        std::make_move_iterator(parsed.structs.begin()),
                        std::make_move_iterator(parsed.structs.end()));
  result.functions.insert(result.functions.end(),
                          std::make_move_iterator(parsed.functions.begin()),
                          std::make_move_iterator(parsed.functions.end()));
  active.erase(canonical);
  loaded.insert(canonical);
  return result;
}
} // namespace

int main(int argc, char **argv) {
  std::string input, output;
  bool checkOnly = false;
  bool objectOnly = false;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--help") {
      std::cout << "Usage: faust [--check] <source.faust> [-o output.ll]\n";
      return 0;
    }
    if (argument == "--check")
      checkOnly = true;
    else if (argument == "-c")
      objectOnly = true;
    else if (argument == "-o" && i + 1 < argc)
      output = argv[++i];
    else if (!argument.empty() && argument[0] != '-' && input.empty())
      input = argument;
    else {
      std::cerr << "error: unexpected argument '" << argument << "'\n";
      return 1;
    }
  }
  if (input.empty() || (checkOnly && (!output.empty() || objectOnly))) {
    std::cerr << "Usage: faust [--check] <source.faust> [-o output.ll]\n";
    return 1;
  }
  try {
    std::set<std::filesystem::path> loaded, active;
    auto program = loadProgram(input, loaded, active);
    faust::check(program);
    // check() fills in type fields on the AST
    if (checkOnly)
      return 0;
    if (objectOnly) {
      if (output.empty())
        throw std::runtime_error("-c requires -o <object-file>");
      faust::generateObject(program, output);
      return 0;
    }
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
