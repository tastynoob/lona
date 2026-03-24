from __future__ import annotations

from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_controlflow_json_covers_for_break_continue_else(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "controlflow_json.lo",
        """
        def walk(limit i32) i32 {
            var i i32 = 0
            for i < limit {
                i = i + 1
                if i < limit {
                    continue
                }
                break
            } else {
                ret 7
            }
            ret 0
        }
        """,
    )
    result = compiler.emit_json(input_path).expect_ok()
    assert_contains(result.stdout, '"type": "For"', label="controlflow json")
    assert_contains(result.stdout, '"type": "Break"', label="controlflow json")
    assert_contains(result.stdout, '"type": "Continue"', label="controlflow json")
    assert_contains(result.stdout, '"else": {', label="controlflow json")


def test_natural_for_else_runs_else_block(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "natural_else.lo",
        """
        def run() i32 {
            var i i32 = 0
            var out i32 = 0
            for i < 3 {
                i = i + 1
            }
            else {
                out = 7
            }
            ret out
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(input_path, output_name="natural_else")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


def test_break_skips_for_else_block(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "break_else.lo",
        """
        def run() i32 {
            var i i32 = 0
            var out i32 = 0
            for i < 3 {
                break
            } else {
                out = 9
            }
            ret out
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(input_path, output_name="break_else")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)


def test_continue_still_allows_eventual_for_else_execution(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "continue_else.lo",
        """
        def run() i32 {
            var i i32 = 0
            var out i32 = 0
            for i < 3 {
                i = i + 1
                if i < 3 {
                    continue
                }
                out = 5
            } else {
                out = out + 2
            }
            ret out
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(input_path, output_name="continue_else")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


def test_inner_break_only_skips_inner_else(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "nested_break.lo",
        """
        def run() i32 {
            var outer i32 = 0
            for outer < 1 {
                outer = outer + 1
                var inner i32 = 0
                for inner < 2 {
                    inner = inner + 1
                    break
                } else {
                    ret 1
                }
            } else {
                ret 0
            }
            ret 2
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(input_path, output_name="nested_break")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)


def test_terminating_for_else_still_lowers_to_ir(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "terminating_for_else.lo",
        """
        def choose(v bool) i32 {
            for v {
                ret 1
            } else {
                ret 2
            }
        }

        def main() i32 {
            ret choose(false)
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_ok()
    assert_contains(result.stdout, "define i32 @choose", label="terminating for-else ir")


def test_break_outside_loop_is_rejected(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "break_bad.lo",
        """
        def main() {
            break
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "`break` can only appear inside `for` loops", label="break diagnostic")


def test_continue_outside_loop_is_rejected(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "continue_bad.lo",
        """
        def main() {
            continue
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "`continue` can only appear inside `for` loops", label="continue diagnostic")


def test_break_inside_for_else_block_is_rejected(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "break_else_bad.lo",
        """
        def main() {
            for false {
            } else {
                break
            }
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "`break` can only appear inside `for` loops", label="for-else break diagnostic")

