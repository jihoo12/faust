"""End-to-end tests for Faust syntax, syscall contracts, and generated code."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

COMPILER, LLI, CLANG, EXAMPLES = sys.argv[1:]
sys.argv = sys.argv[:1]


def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=15)


PRINT_WRAPPER = 'extern printf(fmt: *i8, ...) -> i32 !{asm, syscalls 1};\nfn print(x: i32) -> i32 !{asm, syscalls 1} {\n  return printf("%d\\n", x);\n}\n'


class CompilerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "test.faust"
        self.ir = self.root / "test.ll"

    def compile(self, source, check=False):
        self.source.write_text(source)
        if check:
            return run(COMPILER, "--check", str(self.source))
        return run(COMPILER, str(self.source), "-o", str(self.ir))

    def execute(self, source, expected):
        result = self.compile(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = run(LLI, str(self.ir))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, expected)

    def reject(self, source, message):
        result = self.compile(source)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertRegex(result.stderr, r"test\.faust:\d+:\d+: error:")

    def test_hello_interpreted_and_native(self):
        source = (pathlib.Path(EXAMPLES) / "hello.faust").read_text()
        self.execute(source, "42\n")
        executable = self.root / "hello"
        result = run(CLANG, "-Wno-unused-command-line-argument",
                     "-Wno-override-module", str(self.ir), "-o", str(executable))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = run(str(executable))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "42\n")

    def test_contract_error_example(self):
        self.reject((pathlib.Path(EXAMPLES) / "contract_error.faust").read_text(),
                    "call to 'report' requires syscall 1 in contract of 'main'")

    def test_each_syscall_is_required_at_call_sites(self):
        for syscall in (0, 1):
            with self.subTest(syscall=syscall):
                self.reject(f"fn resource() -> i32 !{{asm, syscalls {syscall}}} {{ return 0; }}\n"
                            "fn main() -> i32 { return resource(); }",
                            f"requires syscall {syscall}")

    def test_transitive_syscall_cannot_be_hidden(self):
        self.reject("fn leaf() -> i32 !{asm, syscalls 1} { return 0; }\n"
                    "fn middle() -> i32 { return leaf(); }\n"
                    "fn main() -> i32 !{asm, syscalls 1} { return middle(); }",
                    "requires syscall 1 in contract of 'middle'")

    def test_forward_calls_and_contract_superset(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} { print(first()); return 0; }\n"
                     "fn first() -> i32 !{asm, syscalls 1} { return second(); }\n"
                     "fn second() -> i32 !{asm, syscalls 1} { return 42; }", "42\n")

    def test_arithmetic_and_wrapping(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let a = 2 + 3 * 4; print(a); print((2 + 3) * 4);\n"
                     "print(10 - 3 - 2); print(-a); print(-2147483648);\n"
                     "print(2147483647 + 1); return 0; }",
                     "14\n20\n5\n-14\n-2147483648\n-2147483648\n")

    def test_left_to_right_arguments(self):
        self.execute(PRINT_WRAPPER +
                     "fn pair(a: i32, b: i32) -> i32 { return a + b; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "pair(print(1), print(2)); return 0; }", "1\n2\n")

    def test_runtime_symbol_isolation(self):
        self.execute(PRINT_WRAPPER +
                     "fn my_printf() -> i32 { return 42; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} { print(my_printf()); return 0; }",
                     "42\n")

    def test_empty_contract_and_comments(self):
        result = self.compile("// Pure entry point.\nfn main() -> i32 !{} { return 0; }", True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse(self.ir.exists())

    def test_recursion_checks_without_execution(self):
        result = self.compile("fn main() -> i32 { return main(); }", True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_programs(self):
        cases = [
            ("", "program must define main"),
            ("fn main(a: i32) -> i32 { return a; }", "main must have no parameters"),
            ("fn main() -> i32 {}", "must end with a return"),
            ("fn main() -> i32 { return 0; let x = 1; }", "statement after return"),
            ("fn main() -> i32 { return x; }", "unknown variable 'x'"),
            ("fn main() -> i32 { let x = x; return x; }", "unknown variable 'x'"),
            ("fn main() -> i32 { return missing(); }", "unknown function 'missing'"),
            ("fn main() -> i32 { return main(1); }", "wrong number of arguments"),
            ("fn main() -> i32 { let x = 1; let x = 2; return x; }", "duplicate local"),
            ("fn f(x: i32, x: i32) -> i32 { return x; } fn main() -> i32 { return 0; }",
             "duplicate parameter"),
            ("fn main() -> i32 { return 0; } fn main() -> i32 { return 1; }", "duplicate or reserved"),
            ("fn main() -> i32 { return 0; } fn main() -> i32 { return 1; }", "duplicate or reserved"),
            ("fn main() -> i32 !{syscalls 1000} { return 0; }", "syscall number too large"),
            ("fn main() -> i32 !{syscalls 1,1} { return 0; }", "duplicate syscall 1"),
            ("fn main() -> i32 !{unknown} { return 0; }", "expected 'syscalls' or 'asm'"),
            ("fn main() -> i32 { asm { \"nop\" } return 0; }", "requires asm"),
            ("fn main() -> i32 { return 2147483648; }", "outside the i32 range"),
            ("fn main() -> i32 { return 999999999999999999999999999; }", "outside the i32 range"),
            ("fn main() -> i32 { return -2147483649; }", "outside the i32 range"),
            ("fn main() -> i32 { return 0 }", "expected ';'"),
            ("fn main() -> i32 { return 0;", "expected '}'"),
            ("fn main() -> i32 { return (0; }", "expected ')'"),
            ("fn main() -> i32 { return @; }", "unexpected character"),
            ("fn main() -> i32 { let return = 0; return 0; }", "expected an identifier"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                self.reject(source, message)

    def test_stdout_ir(self):
        self.source.write_text("fn main() -> i32 { return 0; }")
        result = run(COMPILER, str(self.source))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("define i32 @main()", result.stdout)

    def test_boolean_literals(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "if true { print(1); } else { print(0); }\n"
                     "if false { print(2); } else { print(3); }\n"
                     "return 0; }", "1\n3\n")

    def test_comparison_operators(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "if 1 < 2 { print(1); } else { print(0); }\n"
                     "if 2 > 3 { print(2); } else { print(0); }\n"
                     "if 3 <= 3 { print(1); } else { print(0); }\n"
                     "if 4 >= 5 { print(2); } else { print(0); }\n"
                     "if 5 == 5 { print(1); } else { print(0); }\n"
                     "if 6 != 7 { print(1); } else { print(0); }\n"
                     "return 0; }", "1\n0\n1\n0\n1\n1\n")

    def test_logical_operators(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "if true && true { print(1); } else { print(0); }\n"
                     "if true && false { print(2); } else { print(0); }\n"
                     "if false || true { print(1); } else { print(0); }\n"
                     "if false || false { print(2); } else { print(0); }\n"
                     "if !false { print(1); } else { print(0); }\n"
                     "if !true { print(2); } else { print(0); }\n"
                     "return 0; }", "1\n0\n1\n0\n1\n0\n")

    def test_short_circuit_evaluation(self):
        self.execute(PRINT_WRAPPER +
                     "fn side_effect() -> i32 !{asm, syscalls 1} {\n"
                     "print(99); return 0; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "if false && side_effect() == 1 { print(1); }\n"
                     "if true || side_effect() == 1 { print(2); }\n"
                     "return 0; }", "2\n")

    def test_if_else_if_chain(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let x = 2;\n"
                     "if x == 1 { print(1); }\n"
                     "else if x == 2 { print(2); }\n"
                     "else { print(3); }\n"
                     "return 0; }", "2\n")

    def test_while_loop(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let i = 3;\n"
                     "while i > 0 {\n"
                     "print(i);\n"
                     "i = i - 1;\n"
                     "}\n"
                     "return 0; }", "3\n2\n1\n")

    def test_nested_control_flow(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let i = 2;\n"
                     "while i > 0 {\n"
                     "if i == 2 { print(10); } else { print(20); }\n"
                     "i = i - 1;\n"
                     "}\n"
                     "return 0; }", "10\n20\n")

    def test_bool_function_param_and_return(self):
        self.execute(PRINT_WRAPPER +
                     "fn is_positive(x: i32) -> bool {\n"
                     "return x > 0; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "if is_positive(5) { print(1); } else { print(0); }\n"
                     "if is_positive(-3) { print(2); } else { print(0); }\n"
                     "return 0; }", "1\n0\n")

    def test_assignment_type_mismatch(self):
        self.reject("fn main() -> i32 { let x = 1; x = true; return 0; }",
                    "assignment type mismatch")

    def test_if_condition_must_be_bool(self):
        self.reject("fn main() -> i32 { if 1 { return 0; } return 0; }",
                    "if condition must be bool")

    def test_while_condition_must_be_bool(self):
        self.reject("fn main() -> i32 { while 1 { return 0; } return 0; }",
                    "while condition must be bool")

    def test_arithmetic_on_bool_rejected(self):
        self.reject("fn main() -> i32 { let b = true + false; return 0; }",
                    "arithmetic requires i32 operands")

    def test_comparison_on_bool_rejected(self):
        self.reject("fn main() -> i32 { let b = true < false; return 0; }",
                    "comparison requires i32 operands")

    def test_logical_on_i32_rejected(self):
        self.reject("fn main() -> i32 { let b = 1 && 0; return 0; }",
                    "logical operators require bool operands")

    def test_not_on_i32_rejected(self):
        self.reject("fn main() -> i32 { let b = !1; return 0; }",
                    "'!' requires bool")

    def test_return_type_mismatch(self):
        self.reject("fn f() -> bool { return 1; }\n"
                    "fn main() -> i32 { return 0; }",
                    "return type mismatch")

    def test_bool_return_type(self):
        self.execute(PRINT_WRAPPER +
                     "fn f() -> bool { return true; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "if f() { print(1); } else { print(0); }\n"
                     "return 0; }", "1\n")

    def test_extern_declaration(self):
        self.execute("extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "return write(1, 0, 0); }", "")

    def test_extern_requires_syscall_contract(self):
        self.reject("extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                    "fn main() -> i32 { return write(1, 0, 0); }",
                    "requires syscall 1")

    def test_extern_transitive_syscall(self):
        self.reject("extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                    "fn helper() -> i32 !{asm, syscalls 1} { return write(1, 0, 0); }\n"
                    "fn main() -> i32 { return helper(); }",
                    "requires syscall 1")

    def test_extern_multiple_syscalls(self):
        self.execute("extern read(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 0};\n"
                     "extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 0,1} {\n"
                     "read(0, 0, 0); write(1, 0, 0); return 0; }", "")

    def test_asm_block(self):
        self.execute("fn main() -> i32 !{asm} {\n"
                     "asm { \"nop\" }\n"
                     "return 0; }", "")

    def test_asm_requires_contract(self):
        self.reject("fn main() -> i32 { asm { \"nop\" } return 0; }",
                    "requires asm effect")

    def test_asm_transitive(self):
        self.reject("fn helper() -> i32 !{asm} { asm { \"nop\" } return 0; }\n"
                    "fn main() -> i32 { return helper(); }",
                    "requires asm")

    def test_combined_syscall_and_asm(self):
        self.execute("fn main() -> i32 !{asm, syscalls 1} {\n"
                     "asm { \"nop\" }\n"
                     "return 0; }", "")

    def test_pure_function_no_effects(self):
        self.execute("fn add(a: i32, b: i32) -> i32 { return a + b; }\n"
                     "fn main() -> i32 { let x = add(20, 22); return 0; }", "")

    def test_syscall_not_in_contract_rejected(self):
        self.reject("fn helper() -> i32 !{asm, syscalls 1} { return 0; }\n"
                     "fn main() -> i32 !{asm} { return helper(); }",
                     "requires syscall 1")

    def test_syscall_transitive_through_multiple_levels(self):
        self.reject("fn a() -> i32 !{asm, syscalls 1} { return 0; }\n"
                     "fn b() -> i32 { return a(); }\n"
                     "fn c() -> i32 { return b(); }\n"
                     "fn main() -> i32 !{asm, syscalls 1} { return c(); }",
                     "requires syscall 1 in contract of 'b'")

    def test_syscall_partial_contract_rejected(self):
        self.reject("extern read(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 0};\n"
                     "extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 0} {\n"
                     "read(0, 0, 0); write(1, 0, 0); return 0; }",
                     "requires syscall 1")

    def test_syscall_in_contract_but_not_used(self):
        self.execute("extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                      "fn main() -> i32 !{asm, syscalls 0,1} { return 0; }", "")

    def test_syscall_through_if_branch(self):
        self.reject("fn helper() -> i32 !{asm, syscalls 1} { return 0; }\n"
                     "fn main() -> i32 !{asm} {\n"
                     "if true { helper(); }\n"
                     "return 0; }",
                     "requires syscall 1")

    def test_syscall_through_while_loop(self):
        self.reject("fn helper() -> i32 !{asm, syscalls 1} { return 0; }\n"
                     "fn main() -> i32 !{asm} {\n"
                     "while false { helper(); }\n"
                     "return 0; }",
                     "requires syscall 1")

    def test_syscall_zero_required(self):
        self.reject("extern read(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 0};\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "return read(0, 0, 0); }",
                     "requires syscall 0")

    def test_syscall_without_asm_rejected(self):
        self.reject("fn main() -> i32 !{syscalls 1} { return 0; }",
                     "function declares syscalls but missing asm effect")

    def test_asm_block_no_contract_rejected(self):
        self.reject("fn main() -> i32 { asm { \"nop\" } return 0; }",
                     "requires asm effect")

    def test_syscall_call_no_contract_rejected(self):
        self.reject("fn helper() -> i32 !{asm, syscalls 1} { return 0; }\n"
                     "fn main() -> i32 { return helper(); }",
                     "requires syscall 1")

    def test_asm_and_syscall_no_contract_rejected(self):
        self.reject("fn helper() -> i32 !{asm, syscalls 1} { return 0; }\n"
                     "fn main() -> i32 { asm { \"nop\" } return helper(); }",
                     "requires asm effect")

    def test_syscall_multiple_all_required(self):
        self.reject("extern read(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 0};\n"
                     "extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                     "fn helper() -> i32 !{asm, syscalls 0,1} {\n"
                     "read(0, 0, 0); write(1, 0, 0); return 0; }\n"
                     "fn main() -> i32 !{asm, syscalls 0} { return helper(); }",
                     "requires syscall 1")

    def test_cli_errors(self):
        self.source.write_text("fn main() -> i32 { return 0; }")
        for args in ([], ["--unknown"], [str(self.source), "-o"],
                     ["--check", str(self.source), "-o", str(self.ir)],
                     [str(self.root / "missing.faust")],
                     [str(self.source), "-o", str(self.root / "missing" / "out.ll")]):
            with self.subTest(args=args):
                self.assertEqual(run(COMPILER, *args).returncode, 1)


if __name__ == "__main__":
    unittest.main()
