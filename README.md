# Faust

Faust is an experimental systems programming language built around explicit
syscall contracts. The compiler parses Faust source, checks function contracts,
and emits verified LLVM 22 IR for interpreted or native execution.

## Abstract

Faust explores whether explicit effect contracts can make systems code safer.
Every function declares an upper bound on its system effects — which syscalls
it may invoke, whether it may contain inline assembly, and whether it may cross
an external C ABI boundary. The compiler enforces
these contracts at every call site, transitively, so a wrapper cannot hide the
effects of the functions it calls. The goal is not a sandbox or a proof system,
but a lightweight, composable way to reason about what a function can do.

## Syscall contracts

A contract is an upper bound on a function's permitted system effects:

```text
fn compute() -> i32 !{} { return 42; }
fn report() -> i32 !{asm, syscalls 1} { return write(1, 0, 0); }
```

Omitting `!{...}` is equivalent to the empty contract `!{}`. At every call,
the callee's declared syscalls must be a subset of the caller's contract.
Contracts are explicit, not inferred. Unused permissions are allowed.
Forward calls and recursion follow the same rule. Every function is checked,
including functions that are never called, so a wrapper cannot hide effects.

| Effect | Resource behavior |
| --- | --- |
| `asm` | May contain inline assembly |
| `syscalls N` | May invoke syscall number N (x86-64) |
| `extern` | May call an external/C function |

Declaring `syscalls` requires `asm` — syscalls are implemented via inline
assembly. The compiler enforces this: a function with `syscalls` but no `asm`
is rejected.

`extern` declares a C function. Calling any external function requires the
caller to include the `extern` effect; any other effects on the declaration
also propagate transitively:

```text
extern write(fd: i32, buf: *i8, len: i32) -> i32 !{asm, syscalls 1};

fn report(buf: *i8, len: i32) -> i32 !{extern, asm, syscalls 1} {
  return write(1, buf, len);
}
```

`asm` blocks embed inline assembly with optional output/input constraints:

```text
fn port_in(port: i16) -> i8 !{asm} {
  let result: i8;
  asm {
    "inb %dx, %al"
    : "=al"(result)
    : "dx"(port)
  };
  return result;
}
```

These contracts check calls in Faust source. They are not an operating-system
sandbox, memory budgets, a capability token system, a termination proof, or
memory safety guarantees. Stack use, runtime startup, and execution time are
not tracked. In particular, a function without `asm` can still recurse forever.

## Structs

Structs are nominal aggregate types with named fields:

```text
struct Point {
  x: i32,
  y: i32
}

fn sum(p: Point) -> i32 {
  return p.x + p.y;
}
```

Constructors name every field, and construction order does not have to match
declaration order. Fields are lvalues, so they can be assigned or addressed:

```text
let p = Point { y: 22, x: 20 };
p.x = 21;
let px: *i32 = &p.x;
```

Immutable scalar and struct values are kept as LLVM SSA values when no address
is required. Mutable values, address-taken values, and arrays use stack storage.
The compiler does not synthesize heap allocation; heap allocation must be an
explicit external call or direct syscall chosen by the program.

## Language documentation

See [docs/docs.md](docs/docs.md) for the full language specification.

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

The second command deliberately fails: `main` has not declared the syscalls
required by `report`. Diagnostics include the source path, line, and column.
Without `-o`, the compiler writes IR to standard output. Invalid programs do
not generate IR.

## Compiler structure

The compilation pipeline is source → lexer → parser → AST → semantic checks →
LLVM IR. The CLI orchestrates these stages and handles file input and output.

| Module | Responsibility |
| --- | --- |
| `include/faust/token.h`, `ast.h` | Tokens, source positions, and owned syntax trees |
| `src/diagnostic.cpp` | Shared source-located error reporting |
| `src/lexer.cpp` | Source text to tokens |
| `src/parser.cpp` | Tokens to an AST |
| `src/semantic.cpp` | Name resolution, call arity, entry point, types, and syscall contracts |
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
syscall contract violations, transitive calls, asm blocks, diagnostics,
arithmetic, pointers, arrays, structs, evaluation order, control flow, LLVM
execution, variadic ABI lowering, and native compilation. `flake.lock` pins Nixpkgs; update it with `nix flake update`.

## References

- [LLVM documentation](https://releases.llvm.org/22.1.0/docs/)
- [LLVM on NixOS](https://wiki.nixos.org/wiki/LLVM)
