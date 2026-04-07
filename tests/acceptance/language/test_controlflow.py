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


def test_else_if_chain_parses_and_preserves_nested_if_in_json(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "else_if_json.lo",
        """
        def classify(v i32) i32 {
            if v < 0 {
                ret -1
            } else if v == 0 {
                ret 0
            } else {
                ret 1
            }
        }
        """,
    )
    result = compiler.emit_json(input_path).expect_ok()
    assert result.stdout.count('"type": "If"') >= 2
    assert_contains(result.stdout, '"else": {', label="else-if json")


def test_else_if_chain_runs_with_expected_branching(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "else_if_exec.lo",
        """
        def classify(v i32) i32 {
            if v < 0 {
                ret 1
            } else if v < 10 {
                ret 2
            } else {
                ret 3
            }
        }

        ret classify(7)
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="else_if_exec"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(2)


def test_if_else_can_start_on_next_line(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "if_else_next_line.lo",
        """
        def choose(flag bool) i32 {
            if flag {
                ret 1
            }
            else {
                ret 2
            }
        }

        ret choose(false)
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="if_else_next_line"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(2)


def test_else_can_follow_blank_lines_and_comment_only_lines(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "else_blank_lines.lo",
        """
        def choose(flag bool) i32 {
            if flag {
                ret 1
            }

            // comment before else
            else {
                ret 2
            }
        }

        def run() i32 {
            var i i32 = 0
            for i < 1 {
                i = i + 1
            }

            // comment before for-else
            else {
                ret 5
            }
            ret 0
        }

        ret choose(false) + run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="else_blank_lines"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


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


def test_nested_blocks_have_separate_local_scopes(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "block_scope_locals.lo",
        """
        def shadow_inner() i32 {
            var value i32 = 1
            if true {
                var value i32 = 5
                if value != 5 {
                    ret 99
                }
            }
            ret value
        }

        def redeclare_after_if() i32 {
            if true {
                var detached i32 = 7
                if detached != 7 {
                    ret 99
                }
            }

            var detached i32 = 2
            ret detached
        }

        ret shadow_inner() + redeclare_after_if()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="block_scope_locals"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(3)


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
