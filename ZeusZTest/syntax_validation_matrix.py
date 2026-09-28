"""Grammar-backed syntax diagnostic smoke matrix for Zeus."""

from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "Zeus.exe"

VALID = {
    "comments_and_literals": '// line\n/* block */\nlet x = [1, 2, {"k": "v"}];',
    "variable_declaration": "var const value = 1; let other = value;",
    "assignments": "let value = [1]; value = [2]; value[0] = 3;",
    "member_assignment": "class Box { var item = 0; } let box = new Box(); box.item = 2;",
    "simple_statements": "func f() { return; } for (1) [i] { break; }",
    "if_else": "if (true) { let x = 1; } else if (false) print(0); else print(1);",
    "for_condition": "for (false) { break; }",
    "for_count_simple": "for (3) [i] { break; }",
    "for_count_range": "for (0..3..1) [i] { break; }",
    "for_dynamic_range": "for (0..len([1, 2])) [i] { break; }",
    "for_list": "for ([1, 2]) [item] { break; }",
    "for_dict": 'let data = {"k": 1}; for (data) [key: value] { break; }',
    "function_defaults": "func f(a = 1 + 2, b = a * 2) { return b; }",
    "typed_function": "func f(value: bool) -> bool { return !value; }",
    "reference_function_type": "func f(value: &int) -> &int { return value; }",
    "captures": "func outer() { let x = 1; func inner() [&x] { return x; } }",
    "capture_all": "func outer() { let x = 1; func inner() [&] { return x; } }",
    "trailing_commas": 'func f(a, b,) { print(a, b,); } let values = [1, 2,]; let data = {"k": 1,};',
    "forward_declaration": "decl func f(a, b);",
    "decorators": '@static func f() {} @native("symbol") decl func g(); func static h() {} func i static() {}',
    "class_receiver_alias": "class Box { using &self; var item = 0; func get() { return self.item; } }",
    "class_explicit_receiver": "class Box { var item = 0; func get(this) { return this.item; } }",
    "class_capture_receiver": "class Box { var item = 0; func get() [&self] { return self.item; } }",
    "shared_class": "shared class Box {} class shared Other {}",
    "operator_overload": "class Box { func operator[+](self, other) { return self; } }",
    "operator_index_overloads": "class Index { func operator[[]](self, index) { return index; } func operator[[]=](self, index, value) { return value; } }",
    "new_expression": "let box = new pkg.Box();",
    "imports": "import pkg.name [&alias];",
    "using_forms": "using original; using original {}",
    "try_catch": "try { throw nil; } catch (&err) { print(err); }",
    "with_forms": "with (nil) {} with ([1]) [&item] {}",
    "expression_operators": "let value = !false + 1 * 2 == 3 and true or false;",
    "logical_or_alias": "let value = false || true;",
    "postfix_expressions": "let value = [1][0] + {\"k\": 2}[\"k\"];",
    "delete_names": "let a = 1; let b = 2; delete a, b;",
}

INVALID = {
    "missing_statement_semicolon": "let item = 1",
    "missing_declaration_value": "let item =;",
    "missing_variable_name": "let = 1;",
    "empty_new": "let item = new();",
    "anonymous_function_statement": "func() {}",
    "anonymous_function_value": "let item = func() {};",
    "empty_if_header": "if () {}",
    "missing_if_body": "if (true);",
    "missing_else_body": "if (true) {} else;",
    "empty_for_header": "for () {}",
    "missing_for_body": "for (true);",
    "c_style_for": "for (let i = 0; i < 3; i = i + 1) {}",
    "while_keyword_loop": "while (true) {}",
    "invalid_for_binding": "for ([1]) [item, other] {}",
    "empty_expression_operand": "let item = !;",
    "trailing_binary_operator": "let item = 1 +;",
    "malformed_group": "let item = (1 + 2;",
    "empty_index": "let item = values[];",
    "missing_member_name": "let item = value.;",
    "malformed_call": "print(,);",
    "malformed_list": "let item = [1,,2];",
    "malformed_dictionary": 'let item = {"key":};',
    "invalid_assignment_target": "1 = 2;",
    "missing_throw_value": "throw;",
    "missing_delete_target": "delete;",
    "malformed_import": "import pkg.;",
    "malformed_using": "using something;",
    "missing_function_name": "func() {}",
    "missing_function_body": "func f();",
    "missing_function_default": "func f(value =) {}",
    "missing_parameter_type": "func f(value:) {}",
    "missing_return_type": "func f() -> {}",
    "empty_operator_name": "func operator[](self) {}",
    "unknown_decorator": "@unknown func f() {}",
    "bad_native_decorator": "@native(7) decl func f();",
    "empty_native_symbol": '@native("") decl func f();',
    "static_decorator_argument": '@static("x") func f() {}',
    "decorator_without_function": "@static let item = 1;",
    "class_missing_name": "class {}",
    "missing_class_body": "class Box;",
    "duplicate_receiver_capture": "class Box { using &this; func get() [&this] { return this; } }",
    "duplicate_receiver_parameter": "class Box { using &this; func get(this) { return this; } }",
    "orphan_catch": "catch (error) {}",
    "try_missing_catch": "try {}",
    "empty_with_header": "with () {}",
    "unmatched_closing_delimiter": "let item = 1; }",
    "unsupported_continue": "continue;",
}

LEXICAL_INVALID = {
    "unterminated_block_comment": "/* comment",
    "unterminated_string": 'let item = "text;',
    "unterminated_bytes": "let item = b'bytes;",
}


def run_case(name: str, source_text: str, expect_error: bool, temp: Path) -> str | None:
    source = temp / f"{name}.zeu"
    output = temp / f"{name}.ll"
    source.write_text(source_text + "\n", encoding="utf-8")
    completed = subprocess.run(
        [str(COMPILER), "--emit-llvm", str(source), str(output)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    transcript = completed.stdout + completed.stderr
    has_syntax_error = re.search(r"error\[[PL]\d+\]", transcript) is not None
    if expect_error and not has_syntax_error:
        return f"{name}: expected a located parser/lexer diagnostic, got exit {completed.returncode}: {transcript.strip()}"
    if not expect_error and has_syntax_error:
        return f"{name}: valid syntax was rejected: {transcript.strip()}"
    if not expect_error and ("Compile failed (IR stopped.)" in transcript or
                             "IR Generation failed." in transcript):
        return f"{name}: parser/IR accepted syntax without completing compilation: {transcript.strip()}"
    if expect_error and not re.search(r".+:\d+:\d+: error\[[PL]\d+\]:", transcript):
        return f"{name}: diagnostic lacks file:line:column: {transcript.strip()}"
    return None


def main() -> int:
    if not COMPILER.is_file():
        print(f"missing compiler: {COMPILER}")
        return 2

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="zeus_syntax_matrix_") as temp_dir:
        temp = Path(temp_dir)
        for name, source in VALID.items():
            failure = run_case(name, source, False, temp)
            if failure:
                failures.append(failure)
        for name, source in INVALID.items():
            failure = run_case(name, source, True, temp)
            if failure:
                failures.append(failure)
        for name, source in LEXICAL_INVALID.items():
            failure = run_case(name, source, True, temp)
            if failure:
                failures.append(failure)

    unary_run = subprocess.run(
        [str(COMPILER), str(ROOT / "ZeusZTest" / "unary_not_test.zeu")],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    expected_unary_output = [
        "true false false false",
        "true false",
        "true false",
        "unary not condition passed",
    ]
    if unary_run.returncode != 0 or unary_run.stdout.splitlines() != expected_unary_output:
        failures.append(
            "unary_not_runtime: output mismatch: "
            f"exit {unary_run.returncode}, {unary_run.stdout.strip()}"
        )

    print(
        f"checked {len(VALID)} valid, {len(INVALID)} invalid syntax, "
        f"and {len(LEXICAL_INVALID)} invalid lexical cases"
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("all syntax cases passed; parser diagnostics include file, line, and column")
    print("unary ! runtime smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
