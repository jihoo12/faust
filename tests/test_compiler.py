"""End-to-end tests for Faust syntax, resource contracts, and generated code."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

COMPILER, LLI, CLANG, EXAMPLES = sys.argv[1:]
sys.argv = sys.argv[:1]


def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=15)


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
        # Invalid programs must not leave partially generated IR behind.
        result = self.compile(source)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertRegex(result.stderr, r"test\.faust:\d+:\d+: error:")
        self.assertFalse(self.ir.exists())

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
                    "call to 'report' requires effect 'alloc' in contract of 'main'")

    def test_each_effect_is_required_at_call_sites(self):
        for effect in ("alloc", "io", "block"):
            with self.subTest(effect=effect):
                self.reject(f"fn resource() -> i32 !{{{effect}}} {{ return 0; }}\n"
                            "fn main() -> i32 { return resource(); }",
                            f"requires effect '{effect}'")

    def test_transitive_effect_cannot_be_hidden(self):
        self.reject("fn leaf() -> i32 !{io} { return 0; }\n"
                    "fn middle() -> i32 { return leaf(); }\n"
                    "fn main() -> i32 !{io} { return middle(); }",
                    "requires effect 'io' in contract of 'middle'")

    def test_print_requires_each_effect(self):
        for missing in ("alloc", "io", "block"):
            with self.subTest(missing=missing):
                effects = ",".join(e for e in ("alloc", "io", "block") if e != missing)
                self.reject(f"fn main() -> i32 !{{{effects}}} {{ return print(1); }}",
                            f"requires effect '{missing}'")

    def test_forward_calls_and_contract_superset(self):
        self.execute("fn main() -> i32 !{alloc,io,block} { print(first()); return 0; }\n"
                     "fn first() -> i32 !{io} { return second(); }\n"
                     "fn second() -> i32 !{io} { return 42; }", "42\n")

    def test_arithmetic_and_wrapping(self):
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "let a = 2 + 3 * 4; print(a); print((2 + 3) * 4);\n"
                     "print(10 - 3 - 2); print(-a); print(-2147483648);\n"
                     "print(2147483647 + 1); return 0; }",
                     "14\n20\n5\n-14\n-2147483648\n-2147483648\n")

    def test_left_to_right_arguments(self):
        self.execute("fn pair(a: i32, b: i32) -> i32 { return a + b; }\n"
                     "fn main() -> i32 !{alloc,io,block} {\n"
                     "pair(print(1), print(2)); return 0; }", "1\n2\n")

    def test_runtime_symbol_isolation(self):
        self.execute("fn printf() -> i32 { return 42; }\n"
                     "fn main() -> i32 !{alloc,io,block} { print(printf()); return 0; }",
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
            ("fn print(x: i32) -> i32 { return x; } fn main() -> i32 { return 0; }", "reserved"),
            ("fn main() -> i32 !{network} { return 0; }", "unknown effect"),
            ("fn main() -> i32 !{io,io} { return 0; }", "duplicate effect"),
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
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "if true { print(1); } else { print(0); }\n"
                     "if false { print(2); } else { print(3); }\n"
                     "return 0; }", "1\n3\n")

    def test_comparison_operators(self):
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "if 1 < 2 { print(1); } else { print(0); }\n"
                     "if 2 > 3 { print(2); } else { print(0); }\n"
                     "if 3 <= 3 { print(1); } else { print(0); }\n"
                     "if 4 >= 5 { print(2); } else { print(0); }\n"
                     "if 5 == 5 { print(1); } else { print(0); }\n"
                     "if 6 != 7 { print(1); } else { print(0); }\n"
                     "return 0; }", "1\n0\n1\n0\n1\n1\n")

    def test_logical_operators(self):
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "if true && true { print(1); } else { print(0); }\n"
                     "if true && false { print(2); } else { print(0); }\n"
                     "if false || true { print(1); } else { print(0); }\n"
                     "if false || false { print(2); } else { print(0); }\n"
                     "if !false { print(1); } else { print(0); }\n"
                     "if !true { print(2); } else { print(0); }\n"
                     "return 0; }", "1\n0\n1\n0\n1\n0\n")

    def test_short_circuit_evaluation(self):
        self.execute("fn side_effect() -> i32 !{alloc,io,block} {\n"
                     "print(99); return 0; }\n"
                     "fn main() -> i32 !{alloc,io,block} {\n"
                     "if false && side_effect() == 1 { print(1); }\n"
                     "if true || side_effect() == 1 { print(2); }\n"
                     "return 0; }", "2\n")

    def test_if_else_if_chain(self):
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "let x = 2;\n"
                     "if x == 1 { print(1); }\n"
                     "else if x == 2 { print(2); }\n"
                     "else { print(3); }\n"
                     "return 0; }", "2\n")

    def test_while_loop(self):
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "let i = 3;\n"
                     "while i > 0 {\n"
                     "print(i);\n"
                     "i = i - 1;\n"
                     "}\n"
                     "return 0; }", "3\n2\n1\n")

    def test_nested_control_flow(self):
        self.execute("fn main() -> i32 !{alloc,io,block} {\n"
                     "let i = 2;\n"
                     "while i > 0 {\n"
                     "if i == 2 { print(10); } else { print(20); }\n"
                     "i = i - 1;\n"
                     "}\n"
                     "return 0; }", "10\n20\n")

    def test_bool_function_param_and_return(self):
        self.execute("fn is_positive(x: i32) -> bool {\n"
                     "return x > 0; }\n"
                     "fn main() -> i32 !{alloc,io,block} {\n"
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
        self.execute("fn f() -> bool { return true; }\n"
                     "fn main() -> i32 !{alloc,io,block} {\n"
                     "if f() { print(1); } else { print(0); }\n"
                     "return 0; }", "1\n")

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
