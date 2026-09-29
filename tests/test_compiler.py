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
                     "arithmetic requires numeric operands")

    def test_comparison_on_bool_rejected(self):
        self.reject("fn main() -> i32 { let b = true < false; return 0; }",
                     "comparison requires numeric operands")

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

    def test_address_of_and_dereference(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let x = 42; let p: *i32 = &x; print(*p); return 0; }",
                     "42\n")

    def test_store_through_pointer(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let x = 1; let p: *i32 = &x; *p = 42; print(x); return 0; }",
                     "42\n")

    def test_pointer_parameter_dereference(self):
        self.execute(PRINT_WRAPPER +
                     "fn read(p: *i32) -> i32 { return *p; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let x = 7; print(read(&x)); return 0; }",
                     "7\n")

    def test_address_of_rvalue_rejected(self):
        self.reject("fn main() -> i32 { let p = &(1 + 2); return 0; }",
                    "'&' requires an addressable expression")

    def test_dereference_non_pointer_rejected(self):
        self.reject("fn main() -> i32 { let x = *42; return 0; }",
                    "'*' requires pointer type")

    def test_array_literal_and_index(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs: [4]i32 = [10, 20, 30, 40]; print(xs[2]); return 0; }",
                     "30\n")

    def test_array_index_assignment(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [1, 2, 3]; xs[1] = 42; print(xs[1]); return 0; }",
                     "42\n")

    def test_array_index_variable(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [10, 20, 30]; let i = 2; print(xs[i]); return 0; }",
                     "30\n")

    def test_array_element_type_mismatch_rejected(self):
        self.reject("fn main() -> i32 { let xs = [1, true]; return 0; }",
                    "array elements must have the same type")

    def test_array_index_type_rejected(self):
        self.reject("fn main() -> i32 { let xs = [1, 2]; let x = xs[true]; return 0; }",
                    "index must be an integer")

    def test_empty_array_rejected_without_context(self):
        self.reject("fn main() -> i32 { let xs = []; return 0; }",
                    "cannot infer type of empty array")

    def test_array_decays_to_pointer_argument(self):
        self.execute(PRINT_WRAPPER +
                     "fn first(p: *i32) -> i32 { return *p; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [10, 20, 30]; print(first(xs)); return 0; }",
                     "10\n")

    def test_array_decay_pointer_can_mutate_array(self):
        self.execute(PRINT_WRAPPER +
                     "fn set_first(p: *i32) -> i32 { *p = 42; return 0; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [1, 2, 3]; set_first(xs); print(xs[0]); return 0; }",
                     "42\n")

    def test_array_decay_rejects_wrong_element_type(self):
        self.reject("fn first(p: *i8) -> i8 { return *p; }\n"
                    "fn main() -> i32 { let xs = [1, 2]; first(xs); return 0; }",
                    "wrong type for argument 1")

    def test_address_of_array_element(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [10, 20, 30]; let p: *i32 = &xs[1]; print(*p); return 0; }",
                     "20\n")

    def test_address_of_dereference(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let x = 7; let p: *i32 = &x; let q: *i32 = &*p; *q = 9; print(x); return 0; }",
                     "9\n")

    def test_non_lvalue_assignment_rejected(self):
        self.reject("fn main() -> i32 { (1 + 2) = 3; return 0; }",
                    "left side of assignment is not assignable")

    def test_unary_applies_after_postfix(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [10, 20]; let p: *i32 = &xs[1]; print(*p); return 0; }",
                     "20\n")

    def test_parenthesized_array_index(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [10, 20, 30]; print((xs)[1]); return 0; }",
                     "20\n")

    def test_call_result_pointer_index(self):
        self.execute(PRINT_WRAPPER +
                     "fn identity(p: *i32) -> *i32 { return p; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [4, 5, 6]; print(identity(xs)[2]); return 0; }",
                     "6\n")

    def test_chained_array_index(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [[1, 2], [3, 4]]; print(xs[1][0]); return 0; }",
                     "3\n")

    def test_pointer_index_read(self):
        self.execute(PRINT_WRAPPER +
                     "fn second(p: *i32) -> i32 { return p[1]; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [10, 20, 30]; print(second(xs)); return 0; }",
                     "20\n")

    def test_pointer_index_write(self):
        self.execute(PRINT_WRAPPER +
                     "fn set_second(p: *i32) -> i32 { p[1] = 42; return 0; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [1, 2, 3]; set_second(xs); print(xs[1]); return 0; }",
                     "42\n")

    def test_address_of_pointer_index(self):
        self.execute(PRINT_WRAPPER +
                     "fn get_second(p: *i32) -> *i32 { return &p[1]; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [4, 5, 6]; let p: *i32 = get_second(xs); print(*p); return 0; }",
                     "5\n")

    def test_array_literal_decays_to_pointer_argument(self):
        self.execute(PRINT_WRAPPER +
                     "fn first(p: *i32) -> i32 { return p[0]; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} { print(first([41, 42])); return 0; }",
                     "41\n")

    def test_array_decay_in_typed_let(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [11, 22]; let p: *i32 = xs; print(p[1]); return 0; }",
                     "22\n")

    def test_array_decay_in_assignment(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [7, 8]; let p: *i32 = 0; p = xs; print(*p); return 0; }",
                     "7\n")

    def test_array_decay_in_return(self):
        self.execute(PRINT_WRAPPER +
                     "fn data(p: *i32) -> *i32 { return p; }\n"
                     "fn first(xs: *i32) -> i32 { return *xs; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let xs = [31, 32]; print(first(xs)); return 0; }",
                     "31\n")

    def test_null_pointer_literal(self):
        self.execute("fn main() -> i32 { let p: *i32 = 0; return 0; }", "")

    def test_integer_literal_conversion_in_call(self):
        self.execute("fn narrow(x: i8) -> i32 { return x; }\n"
                     "fn main() -> i32 { narrow(42); return 0; }", "")

    def test_integer_literal_call_range_rejected(self):
        self.reject("fn narrow(x: i8) -> i32 { return x; }\n"
                    "fn main() -> i32 { return narrow(256); }",
                    "outside the range of i8")

    def test_extern_declaration(self):
        self.execute("extern write(fd: i32, buf: i32, len: i32) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "return write(1, 0, 0); }", "")

    def test_extern_i64_return_type_in_ir(self):
        result = self.compile("extern wide() -> i64;\n"
                              "fn main() -> i32 { wide(); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("declare i64 @wide()", self.ir.read_text())

    def test_extern_pointer_return_type_in_ir(self):
        result = self.compile("extern data() -> *i8;\n"
                              "fn main() -> i32 { data(); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("declare ptr @data()", self.ir.read_text())

    def test_string_literal_is_pointer_typed(self):
        self.execute("extern puts(s: *i8) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 1} { puts(\"hello\"); return 0; }",
                     "hello\n")

    def test_string_literal_escape_decoding(self):
        self.execute("extern printf(fmt: *i8, ...) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 1} { printf(\"a\\tb\\n\"); return 0; }",
                     "a\tb\n")

    def test_variadic_string_literal_decays_to_pointer(self):
        self.execute("extern printf(fmt: *i8, ...) -> i32 !{asm, syscalls 1};\n"
                     "fn main() -> i32 !{asm, syscalls 1} { printf(\"%s\\n\", \"hello\"); return 0; }",
                     "hello\n")

    def test_variadic_small_integer_promotions_in_ir(self):
        result = self.compile(
            "extern sink(tag: i32, ...) -> i32;\n"
            "fn main() -> i32 { let a: i8 = 1; let b: u8 = 2; sink(0, a, b); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        ir = self.ir.read_text()
        self.assertIn("sext i8", ir)
        self.assertIn("zext i8", ir)

    def test_variadic_f32_promotes_to_f64_in_ir(self):
        result = self.compile(
            "extern sink(tag: i32, ...) -> i32;\n"
            "fn main() -> i32 { let x: f32 = 1; sink(0, x); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("fpext float", self.ir.read_text())

    def test_string_literal_rejected_for_wrong_pointer_element_type(self):
        self.reject("extern takes_i32_ptr(p: *i32) -> i32 !{asm, syscalls 1};\n"
                    "fn main() -> i32 !{asm, syscalls 1} { return takes_i32_ptr(\"hello\"); }",
                    "wrong type for argument 1")

    def test_string_literal_rejected_for_integer_param(self):
        self.reject("fn takes_int(x: i32) -> i32 { return x; }\n"
                    "fn main() -> i32 { return takes_int(\"hello\"); }",
                    "wrong type for argument 1")

    def test_extern_call_requires_extern_contract(self):
        self.reject("extern puts(s: *i8) -> i32;\n"
                    "fn main() -> i32 { puts(\"hello\"); return 0; }",
                    "requires extern effect")

    def test_extern_call_with_extern_contract(self):
        self.execute("extern puts(s: *i8) -> i32;\n"
                     "fn main() -> i32 !{extern} { puts(\"hello\"); return 0; }",
                     "hello\n")

    def test_extern_effect_is_transitive(self):
        self.reject("extern puts(s: *i8) -> i32;\n"
                    "fn helper() -> i32 !{extern} { return puts(\"hello\"); }\n"
                    "fn main() -> i32 { helper(); return 0; }",
                    "requires extern effect")

    def test_extern_and_syscall_contract(self):
        self.execute("extern puts(s: *i8) -> i32;\n"
                     "fn main() -> i32 !{extern, asm, syscalls 1} { puts(\"hello\"); return 0; }",
                     "hello\n")

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

    def test_i8_type(self):
        self.execute("fn main() -> i32 { let x: i8 = 42; return 0; }", "")

    def test_u8_type(self):
        self.execute("fn main() -> i32 { let x: u8 = 255; return 0; }", "")

    def test_i16_type(self):
        self.execute("fn main() -> i32 { let x: i16 = 1000; return 0; }", "")

    def test_u16_type(self):
        self.execute("fn main() -> i32 { let x: u16 = 65535; return 0; }", "")

    def test_i64_type(self):
        self.execute("fn main() -> i32 { let x: i64 = 100000; return 0; }", "")

    def test_u64_type(self):
        self.execute("fn main() -> i32 { let x: u64 = 100000; return 0; }", "")

    def test_unsigned_comparison(self):
        self.execute(PRINT_WRAPPER +
                     "fn main() -> i32 !{asm, syscalls 1} {\n"
                     "let a: u8 = 255; let b: u8 = 1; if a > b { print(1); } else { print(0); } return 0; }",
                     "1\n")

    def test_unsigned_extension_to_i32(self):
        self.execute(PRINT_WRAPPER +
                     "fn widen(x: u8) -> i32 { return x; }\n"
                     "fn main() -> i32 !{asm, syscalls 1} { print(widen(255)); return 0; }",
                     "255\n")

    def test_unsigned_to_float_codegen(self):
        result = self.compile(
            "fn to_float(x: u32) -> f32 { let y: f32 = x; return y; }\n"
            "fn main() -> i32 { to_float(1); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("uitofp i32", self.ir.read_text())

    def test_unsigned_comparison_codegen(self):
        result = self.compile(
            "fn greater(a: u32, b: u32) -> bool { return a > b; }\n"
            "fn main() -> i32 { greater(1, 2); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("icmp ugt i32", self.ir.read_text())

    def test_f32_type(self):
        self.execute("fn main() -> i32 { let x: f32 = 1; return 0; }", "")

    def test_f64_type(self):
        self.execute("fn main() -> i32 { let x: f64 = 1; return 0; }", "")

    def test_f32_arithmetic(self):
        self.execute("fn calc(a: f32, b: f32) -> f32 { return a + b * a - b; }\n"
                     "fn main() -> i32 { calc(2, 3); return 0; }", "")

    def test_f64_negation(self):
        self.execute("fn negate(x: f64) -> f64 { return -x; }\n"
                     "fn main() -> i32 { negate(7); return 0; }", "")

    def test_float_comparisons(self):
        self.execute("fn less(a: f32, b: f32) -> bool { return a < b; }\n"
                     "fn equal(a: f64, b: f64) -> bool { return a == b; }\n"
                     "fn main() -> i32 { less(1, 2); equal(3, 3); return 0; }", "")

    def test_float_codegen_uses_float_instructions(self):
        result = self.compile(
            "fn calc(a: f32, b: f32) -> bool { let x = -(a + b * a); return x <= b; }\n"
            "fn main() -> i32 { calc(1, 2); return 0; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        ir = self.ir.read_text()
        self.assertIn("fmul float", ir)
        self.assertIn("fadd float", ir)
        self.assertIn("fneg float", ir)
        self.assertIn("fcmp ole float", ir)

    def test_void_return(self):
        self.execute("fn helper() -> void { return; }\n"
                      "fn main() -> i32 { helper(); return 0; }", "")

    def test_void_with_value_rejected(self):
        self.reject("fn helper() -> void { return 1; }\n"
                     "fn main() -> i32 { return 0; }",
                     "void function cannot return a value")

    def test_i8_arithmetic(self):
        self.execute("fn main() -> i32 { let a: i8 = 100; let b: i8 = 50; let c = a + b; return 0; }", "")

    def test_i8_overflow_rejected(self):
        self.reject("fn main() -> i32 { let x: i8 = 256; return 0; }",
                     "outside the range of i8")

    def test_type_mismatch_rejected(self):
        self.reject("fn main() -> i32 { let x: i8 = true; return 0; }",
                     "assignment type mismatch")


if __name__ == "__main__":
    unittest.main()
