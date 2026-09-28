# LLVM demo

A Nix flake providing LLVM 22, Clang, clangd, LLD, LLDB, CMake, and Ninja.
The C++ demo uses the LLVM API to generate and verify IR containing an `add`
function and a `main` function that prints `42`.

## Development

Install Nix with the `nix-command` and `flakes` experimental features enabled.
The flake exposes environments for x86_64 and aarch64 Linux and macOS.

```sh
nix develop
cmake --fresh -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/llvm-demo > build/demo.ll
lli build/demo.ll
```

The last command prints `42`. Inspect `build/demo.ll` to see the generated IR.
The generator itself prints IR to standard output; `lli` executes that IR.
The test checks successful generation, successful execution, and exact output.
`--fresh` clears cached compiler and LLVM paths when changing toolchain versions.

To compile the generated IR into a native executable:

```sh
clang build/demo.ll -o build/demo
./build/demo
```

## Reproducible build

```sh
nix build
nix flake check
./result/bin/llvm-demo
```

`nix build` builds and tests the generator, then installs it under `result/bin`.
`flake.lock` pins the Nixpkgs revision. Run `nix flake update` to update that pin.

## References

- [LLVM documentation](https://releases.llvm.org/22.1.0/docs/)
- [LLVM on NixOS](https://wiki.nixos.org/wiki/LLVM)
