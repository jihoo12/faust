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

Pointer types use `*T` syntax (currently mapped to `i32`).

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
- Declaring `syscalls` requires `asm`
- Omitting `!{...}` is equivalent to `!{}` (no effects)

Contracts are checked at every call site, transitively. The callee's syscalls
must be a subset of the caller's contract.

### Extern functions

```text
extern write(fd: i32, buf: *i8, len: i32) -> i32 !{asm, syscalls 1};
```

`extern` declares a C function with a syscall contract. Extern functions can
be variadic using `...`:

```text
extern printf(fmt: *i8, ...) -> i32 !{asm, syscalls 1};
```

## Statements

### Variable binding

```text
let name = expression;
let name: type = expression;
```

### Assignment

```text
name = expression;
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

## Comments

```text
// Line comment
```

## Example

```text
extern printf(fmt: *i8, ...) -> i32 !{asm, syscalls 1};

fn add(a: i32, b: i32) -> i32 {
  return a + b;
}

fn print(x: i32) -> i32 !{asm, syscalls 1} {
  return printf("%d\n", x);
}

fn main() -> i32 !{asm, syscalls 1} {
  let answer = add(20, 22);
  print(answer);
  return 0;
}
```

## Limitations

- No heap allocation
- No structures or modules
- No ownership system
- Strings are limited to function call arguments
- No floating-point literals in source (use integer literals with type annotations)
