from __future__ import annotations

from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_syntax_error_diagnostic_shows_location_and_help(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "syntax_diag.lo",
        """
        def bad() i32 {
            var x i32 =
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "syntax error: I couldn't parse this statement: unexpected newline.", label="syntax diagnostic")
    assert_contains(result.stderr, f" --> {input_path}:2:16", label="syntax diagnostic")
    assert_contains(result.stderr, " 2 |     var x i32 =", label="syntax diagnostic")
    assert_contains(
        result.stderr,
        "help: Check for a missing separator, unmatched delimiter, or mistyped keyword near here.",
        label="syntax diagnostic",
    )


def test_undefined_identifier_diagnostic_is_precise(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "semantic_diag.lo",
        """
        def bad() i32 {
            ret foo
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "semantic error: undefined identifier `foo`", label="semantic diagnostic")
    assert_contains(result.stderr, f" --> {input_path}:2:9", label="semantic diagnostic")
    assert_contains(result.stderr, " 2 |     ret foo", label="semantic diagnostic")
    assert_contains(
        result.stderr,
        "help: Declare it with `var` before using it, or check the spelling.",
        label="semantic diagnostic",
    )


def test_duplicate_parameter_diagnostic_points_at_redefinition(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "duplicate_param.lo",
        """
        def bad(a i32, a i32) i32 {
            ret a
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "semantic error: duplicate function parameter `a`", label="duplicate parameter diagnostic")
    assert_contains(result.stderr, f" --> {input_path}:1:16", label="duplicate parameter diagnostic")
    assert_contains(
        result.stderr,
        "help: Rename one of the parameters so each binding is unique.",
        label="duplicate parameter diagnostic",
    )


def test_missing_return_diagnostic_blames_function_header(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "missing_return.lo",
        """
        def bad(a i32) i32 {
            if a < 1 {
                ret 1
            }
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "semantic error: not all paths return a value", label="missing return diagnostic")
    assert_contains(result.stderr, f" --> {input_path}:1:5", label="missing return diagnostic")
    assert_contains(result.stderr, " 1 | def bad(a i32) i32 {", label="missing return diagnostic")


def test_missing_return_value_diagnostic_points_at_ret(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "missing_ret_value.lo",
        """
        def bad() i32 {
            ret
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "semantic error: missing return value", label="missing return value diagnostic")
    assert_contains(result.stderr, f" --> {input_path}:2:5", label="missing return value diagnostic")
    assert_contains(result.stderr, " 2 |     ret", label="missing return value diagnostic")


def test_imported_module_error_reports_defining_module_location(compiler: CompilerHarness) -> None:
    import_dir = compiler.output_path("import_diag")
    import_dir.mkdir(parents=True, exist_ok=True)
    dep_path = import_dir / "bad_dep.lo"
    dep_path.write_text(
        "def bad(flag bool) i32 {\n"
        "    if flag {\n"
        "        ret 1\n"
        "    }\n"
        "}\n",
        encoding="utf-8",
    )
    main_path = import_dir / "main.lo"
    main_path.write_text(
        "import bad_dep\n\n"
        "def main() i32 {\n"
        "    ret bad_dep.bad(false)\n"
        "}\n",
        encoding="utf-8",
    )

    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "semantic error: not all paths return a value", label="import diagnostic")
    assert_contains(result.stderr, f" --> {dep_path}:1:5", label="import diagnostic")
    assert_contains(result.stderr, " 1 | def bad(flag bool) i32 {", label="import diagnostic")
