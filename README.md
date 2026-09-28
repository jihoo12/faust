# Faust

Faust is an experimental systems programming language built around explicit
resource contracts. This first compiler parses Faust source, checks function
contracts, and emits verified LLVM 22 IR for interpreted or native execution.

```text
fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn main() -> i32 !{alloc, io, block} {
  let answer = add(20, 22);
  print(answer);
  return 0;
}
```

## Build and run

Install Nix with the `nix-command` and `flakes` experimental features enabled.
The flake provides LLVM 22, Clang, clangd, LLD, LLDB, CMake, Ninja, and Python
for tests. Environments are exposed for x86_64 and aarch64 Linux and macOS;
execution has been tested on x86_64 Linux.

```sh
nix develop
cmake --fresh -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/faust examples/hello.faust -o build/hello.ll
lli build/hello.ll
clang -Wno-unused-command-line-argument -Wno-override-module build/hello.ll -o build/hello
./build/hello
```

Both execution commands print `42`. `--fresh` clears cached compiler and LLVM
paths when changing toolchain versions. Exit an old development shell before
entering the updated one.

Check a program without generating IR:

```sh
./build/faust --check examples/hello.faust
./build/faust --check examples/contract_error.faust
```

The second command deliberately fails: `main` has not declared the effects of
`report`. Diagnostics include the source path, line, and column. Without `-o`,
the compiler writes IR to standard output. Invalid programs do not generate IR.

## Resource contracts

A contract is an upper bound on a function's permitted effects:

```text
fn compute() -> i32 !{} { return 42; }
fn report() -> i32 !{alloc, io, block} { return print(compute()); }
```

Omitting `!{...}` is equivalent to the empty contract `!{}`. At every call,
the callee's declared effects must be a subset of the caller's contract.
Contracts are explicit, not inferred. Unused permissions are allowed.
Forward calls and recursion follow the same rule. Every function is checked,
including functions that are never called, so a wrapper cannot hide effects.

| Effect | Resource behavior |
| --- | --- |
| `alloc` | May dynamically allocate memory |
| `io` | May interact with external input or output |
| `block` | May wait for an external resource |

The built-in `print(value: i32)` prints a signed decimal integer and a newline,
then returns `0`. It requires **all three effects**, because its C `printf`
implementation may allocate and block as well as perform I/O. Its return value
does not report output errors. `main` explicitly declares its own allowed effects;
the prototype has no separate host policy restricting the entry point.

These contracts check calls in Faust source. They are not an operating-system
sandbox, memory budgets, a capability token system, a termination proof, or
memory safety guarantees. Stack use, runtime startup, and execution time are
not tracked. In particular, a function without `block` can still recurse forever.
There are no user allocation or waiting primitives yet; `alloc` and `block` are
already checked in declared contracts and in calls to `print`.

## Current language

- Functions use `fn name(parameter: i32) -> i32`, with an optional contract.
- All values, parameters, and function results are signed 32-bit integers.
- `let name = expression;` creates an immutable local. Repeated local and
  parameter names are rejected; local shadowing is not supported.
- Expressions support calls, parentheses, unary `-`, and binary `+`, `-`, `*`.
  Multiplication binds tighter than addition and subtraction. Binary operators
  associate left to right; operands and arguments evaluate left to right.
- Arithmetic wraps modulo 2^32. Literals must fit in `i32`, including `-2147483648`.
- Expression statements discard their result. Every function must end with
  `return expression;`; statements after a return are rejected.
- `//` starts a line comment. Programs must define `main() -> i32`.
- `print` is reserved. User functions have separate LLVM names from C runtime
  symbols. A source function named `printf` does not replace the output runtime.

The compiler intentionally starts small. It does not yet implement conditionals,
loops, booleans, strings, pointers, heap allocation, structures, modules, foreign
function declarations, or ownership. The next design work can build on the
contract checker before expanding the systems programming surface.

## Compiler structure

The compilation pipeline is source → lexer → parser → AST → semantic checks →
LLVM IR. The CLI orchestrates these stages and handles file input and output.

| Module | Responsibility |
| --- | --- |
| `include/faust/token.h`, `ast.h` | Tokens, source positions, and owned syntax trees |
| `src/diagnostic.cpp` | Shared source-located error reporting |
| `src/lexer.cpp` | Source text to tokens |
| `src/parser.cpp` | Tokens to an AST |
| `src/semantic.cpp` | Name resolution, call arity, entry point, and contracts |
| `src/codegen.cpp` | Checked AST to verified LLVM IR |
| `src/main.cpp` | Command-line arguments, files, and pipeline orchestration |

Module interfaces live in `include/faust/` under the `faust` namespace.
Parser and generator implementation classes remain private to their source files.
The `faust_frontend` CMake library has no LLVM dependency; `faust_codegen` keeps
LLVM headers and compiler definitions private. Call `faust::check()` before
`faust::generateIR()`; the backend accepts a semantically valid AST.

## Reproducible build

```sh
nix build
nix flake check
./result/bin/faust --check examples/hello.faust
```

`nix build` runs the compiler tests and installs `result/bin/faust`. Tests cover
contract violations, transitive calls, diagnostics, arithmetic, evaluation order,
LLVM execution, and native compilation. `flake.lock` pins Nixpkgs; update it with
`nix flake update`.

## References

- [LLVM documentation](https://releases.llvm.org/22.1.0/docs/)
- [LLVM on NixOS](https://wiki.nixos.org/wiki/LLVM)
