from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_json_milestone_nodes_and_legacy_cast_rejection(compiler: CompilerHarness) -> None:
    json_out = _emit_json(
        compiler,
        "milestone_json.lo",
        """
        struct Complex {
            real i32
            img i32
        }

        def mix(x i32, y i32) i32 {
            ret x + y
        }

        def show() {
            var pair <i32, bool> = (1, true)
            var matrix i32[4][5] = {1, 2, 3, 4}
            var c = Complex(real = 1, img = 2)
            var p = math.Point(x = 1)
            ret mix(y = c.img, x = c.real)
        }
        """,
    )
    for needle in [
        '"declaredType": "<i32, bool>"',
        '"type": "TupleLiteral"',
        '"declaredType": "i32[4][5]"',
        '"type": "BraceInit"',
        '"type": "FieldCall"',
        '"type": "NamedCallArg"',
        '"kind": "positional"',
        '"name": "math"',
    ]:
        assert_contains(json_out, needle, label="milestone json")

    _expect_ir_failure(
        compiler,
        "legacy_cast.lo",
        """
        def bad() i32 {
            var x i32 = i32 1
            ret x
        }
        """,
        ["syntax error: I couldn't parse this statement:"],
    )

def test_multiline_parentheses_calls_and_operator_continuations(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "multiline_layout.lo",
        """
        def add(
            a i32,
            b i32
        ) i32 {
            ret a +
                b
        }

        def ping(
        ) i32 {
            ret 1
        }

        def main() i32 {
            var grouped i32 =
                (
                    1 +
                    2
                )
            var total i32 = add(
                grouped,
                ping(
                )
            )
            ret total
        }
        """,
    )
    assert_contains(ir, "define i32 @add(i32", label="multiline layout ir")
    assert_contains(ir, "define i32 @ping()", label="multiline layout ir")
    assert_contains(ir, "define i32 @main()", label="multiline layout ir")

def test_multiline_bracket_and_tuple_type_layouts(compiler: CompilerHarness) -> None:
    ir = _emit_ir(
        compiler,
        "multiline_types.lo",
        """
        def make_pair() <
            i32,
            bool
        > {
            ret (
                1,
                true
            )
        }

        def main() usize {
            var data i32[
                2
            ] = {}
            var pair <
                i32,
                bool
            > = make_pair()
            var size usize = sizeof[
                i32[
                    2
                ]
            ]()
            ret size + cast[usize](pair._1) + cast[usize](data(0))
        }
        """,
    )
    assert_contains(ir, "define i64 @make_pair()", label="multiline type ir")
    assert_contains(ir, "alloca { i32, i8 }", label="multiline type ir")
    assert_contains(ir, "define i64 @main()", label="multiline type ir")

def test_controlflow_braces_must_stay_on_same_line_as_if_and_else(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "if_brace_next_line.lo",
            """
            def bad(flag bool) i32 {
                if flag
                {
                    ret 1
                }
                ret 0
            }
            """,
            ["syntax error:"],
        ),
        (
            "else_brace_next_line.lo",
            """
            def bad(flag bool) i32 {
                if flag {
                    ret 1
                }
                else
                {
                    ret 2
                }
                ret 0
            }
            """,
            ["syntax error:"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)

def test_unary_operators_cannot_continue_onto_next_line(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "unary_minus_next_line.lo",
            """
            def bad() i32 {
                ret -
                    1
            }
            """,
            ["syntax error:"],
        ),
        (
            "unary_deref_next_line.lo",
            """
            def bad(ptr i32*) i32 {
                ret *
                    ptr
            }
            """,
            ["syntax error:"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)

def test_reports_targeted_diagnostics_for_invalid_pointer_and_aggregate_casts(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "cast_pointer_nested_const_bad.lo",
            """
            def main() i32 {
                var bytes u8 const[2] = {1, 2}
                var view u8 const[*] = &bytes(0)
                var slot u8 const[*]* = &view
                var bad u8[*][*] = cast[u8[*][*]](slot)
                ret bad(0)(0)
            }
            """,
            ["unsupported builtin cast from `u8 const[*]*` to `u8[*][*]`"],
        ),
        (
            "cast_bad.lo",
            """
            def main() i32 {
                var value i32 = 1
                var raw u8* = cast[u8*](value)
                ret 0
            }
            """,
            ["unsupported builtin cast from `i32` to `u8*`"],
        ),
        (
            "cast_tuple_bad.lo",
            """
            def main() i32 {
                var pair <i32, i32> = (1, 2)
                var copy <i32, i32> = cast[<i32, i32>](pair)
                ret copy._1
            }
            """,
            [
                "unsupported builtin cast from `<i32, i32>` to `<i32, i32>`",
                "Builtin cast only supports builtin scalar and pointer types.",
            ],
        ),
        (
            "cast_struct_bad.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            def main() i32 {
                var pair = Pair(left = 1, right = 2)
                var copy Pair = cast[Pair](pair)
                ret copy.left
            }
            """,
            [
                "unsupported builtin cast from `",
                "Pair` to `",
                "Builtin cast only supports builtin scalar and pointer types.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
