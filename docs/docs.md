# Faust Language Specification

## Overview

Faust is an experimental systems programming language with explicit syscall
contracts. Programs are compiled to LLVM IR for interpreted or native execution.

## Types

| Type | Description | LLVM type |
| --- | --- | --- |
| `i8` | 8-bit signed integer | `i8` |
| `u8` | 8-bit unsigned integer | `i8` |
| `i16` | 16-bit signed integer | `i16` |
| `u16` | 16-bit unsigned integer | `i16` |
| `i32` | 32-bit signed integer | `i32` |
| `u32` | 32-bit unsigned integer | `i32` |
| `i64` | 64-bit signed integer | `i64` |
| `u64` | 64-bit unsigned integer | `i64` |
| `f32` | 32-bit floating point | `float` |
| `f64` | 64-bit floating point | `double` |
| `bool` | Boolean | `i1` |
| `void` | No return value | `void` |
| `*T` | Pointer to `T` | LLVM opaque pointer (`ptr`) |
| `[N]T` | Fixed-size array of `N` values | `[N x T]` |
| user-defined struct | Nominal aggregate type | named LLVM struct |

### Type annotations

Variables can have explicit type annotations:

```text
let x: i8 = 42;
let y: f64 = 3.14;
```

Integer literals are implicitly converted to any integer or float type.
Range checking is performed based on the annotated type.

## Functions

```text
fn name(parameter: type, ...) -> return_type !{contract} {
  body
}
```

- Functions must end with `return expression;`
- Statements after a return are rejected
- `main() -> i32` is the entry point
- Functions can be forward-declared and recursive

### Contracts

A contract declares the function's permitted system effects:

```text
!{asm, syscalls 0,1}
```

- `asm` — may contain inline assembly
- `syscalls N` — may invoke syscall number N (x86-64)
- `extern` — may call an external/C function
- Declaring `syscalls` requires `asm`
- Omitting `!{...}` is equivalent to `!{}` (no effects)

Contracts are checked at every call site, transitively. A caller must include
all effects required by the callee. External declarations intrinsically carry
the `extern` effect, so every Faust function that calls one must declare
`extern`.

### Extern functions

```text
extern write(fd: i32, buf: *i8, len: i32) -> i32 !{asm, syscalls 1};
```

`extern` declares a C function. Its Faust caller must include `extern` in its
contract. Extern functions can be variadic using `...`:

```text
extern printf(fmt: *i8, ...) -> i32;
```

## Statements

### Variable binding

```text
let name = expression;
let name: type = expression;
```

### Assignment

The left side can be a variable, dereference, array/pointer index, or struct
field:

```text
name = expression;
*p = expression;
xs[i] = expression;
value.field = expression;
```

### If/else

```text
if condition { body }
if condition { body } else { body }
if condition { body } else if condition { body } else { body }
```

### While loops

```text
while condition { body }
```

Loop-carried variables use phi nodes.

### Asm blocks

```text
asm { "instruction" }
asm { "instruction" : "output"(var) }
asm { "instruction" : "output"(var) : "input"(var) }
```

### Return

```text
return expression;
return;  // for void functions
```

## Expressions

### Literals

- Integer: `42`, `-2147483648`
- Boolean: `true`, `false`
- String: `"hello\n"`

### Operators

| Operator | Description |
| --- | --- |
| `+`, `-`, `*` | Arithmetic |
| `<`, `>`, `<=`, `>=` | Comparison |
| `==`, `!=` | Equality |
| `&&`, `\|\|` | Logical (short-circuit) |
| `!` | Logical not |
| `-` | Unary negation |

### Function calls

```text
name(argument, ...)
```

## Pointers and arrays

Pointer types use `*T`. Address-of and dereference expressions are lvalues and
compose with indexing:

```text
let x = 42;
let p: *i32 = &x;
*p = 41;
```

Fixed-size arrays use `[N]T` and array literals use `[...]`:

```text
let xs: [3]i32 = [10, 20, 30];
xs[1] = 21;
let p: *i32 = &xs[1];
```

Arrays decay to pointers with the same element type when passed where a pointer
is required. The array expression must already have addressable storage; the
compiler does not create a hidden temporary for an array literal just to make
it decay. Pointer indexing is supported. There are currently no runtime bounds
checks.

## Structs

Struct declarations introduce nominal types:

```text
struct Point {
  x: i32,
  y: i32
}
```

Values are constructed with named fields. Every declared field must be supplied
exactly once; field order in the literal is independent of declaration order:

```text
let p = Point { y: 22, x: 20 };
return p.x;
```

Structs can be passed to and returned from functions by value. Field expressions
are lvalues, so both mutation and taking a field address are supported:

```text
p.x = 21;
let px: *i32 = &p.x;
```

Read-only structs can remain aggregate SSA values. A struct is given stack
storage when mutation or address-taking requires an address.

## Storage model

The compiler does not implicitly allocate heap memory. Immutable scalar and
struct values stay in LLVM SSA form when possible. Mutable/address-taken values
and arrays use stack storage, while string literals use static global storage.
Dynamic heap allocation must be explicit in source, for example through an
external allocator or an explicitly permitted syscall.

## Comments

```text
// Line comment
```

## Example

```text
extern printf(fmt: *i8, ...) -> i32;

fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn print(x: i32) -> i32 !{extern} {
  return printf("%d\n", x);
}

fn main() -> i32 !{extern} {
  let answer = add(20, 22);
  print(answer);
  return 0;
}
```

## Limitations

- No compiler-generated heap allocation; dynamic allocation must be explicit
- No modules, generics, ADTs, ownership system, or inductive types
- No runtime array/pointer bounds checks
- No floating-point literals in source (use integer literals with type annotations)


## Source includes

A Faust source file can include declarations from another source file with:

```faust
include "lib.faust";

fn main() -> i32 {
  return helper();
}
```

Include paths are resolved relative to the file containing the `include` statement.
Includes may be nested. A source file included more than once is loaded only once, and
cyclic include chains are rejected.


## Native object files

Use `-c` with `-o` to emit a native object file for the host target instead
of textual LLVM IR:

```sh
faust -c main.faust -o main.o
clang main.o -o main
```

On platforms that conventionally use COFF objects, the output may be named
`.obj`; the object format is selected by LLVM for the host target.
