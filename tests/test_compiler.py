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
