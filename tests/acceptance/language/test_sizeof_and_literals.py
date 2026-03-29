from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_sizeof_keyword_and_usize_follow_target_pointer_width(
    compiler: CompilerHarness,
) -> None:
    x64_ir = compiler.emit_ir(
        compiler.write_source(
            "sizeof_x64.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            def make_pair() Pair {
                ret Pair(left = 1, right = 2)
            }

            def main() usize {
                var value i32 = 7
                var tiny i8 = 0
                var literal usize = 1_usize
                var from_value usize = sizeof(value)
                var from_builtin_type usize = sizeof[i16]()
                var from_array_type usize = sizeof[i16[4]]()
                var from_small_value usize = sizeof(tiny)
                var from_pointer_type usize = sizeof[i32*]()
                var from_call usize = sizeof(make_pair())
                ret literal + from_value + from_builtin_type + from_array_type + from_small_value + from_pointer_type + from_call
            }
            """,
        ),
        target="x86_64-unknown-linux-gnu",
    ).expect_ok().stdout
    assert_contains(x64_ir, "define i64 @main()", label="sizeof x64 ir")
    for needle in [
        "store i64 4",
        "store i64 2",
        "store i64 8",
        "store i64 1",
    ]:
        assert_contains(x64_ir, needle, label="sizeof x64 ir")

    x86_ir = compiler.emit_ir(
        compiler.write_source(
            "sizeof_x86.lo",
            """
            def main() usize {
                ret sizeof[i32*]()
            }
            """,
        ),
        target="i686-unknown-linux-gnu",
    ).expect_ok().stdout
    assert_contains(x86_ir, "define i32 @main()", label="sizeof x86 ir")
    assert_contains(x86_ir, "store i32 4", label="sizeof x86 ir")

    x86_mixed_ir = compiler.emit_ir(
        compiler.write_source(
            "sizeof_x86_mixed.lo",
            """
            def main() usize {
                var mixed usize = (sizeof[i32*]() - 5_u32) / 2_u32
                var literal usize = 1_usize
                ret mixed + literal
            }
            """,
        ),
        target="i686-unknown-linux-gnu",
    ).expect_ok().stdout
    assert_contains(x86_mixed_ir, "define i32 @main()", label="sizeof x86 mixed ir")
    assert_contains(x86_mixed_ir, "store i32 2147483647", label="sizeof x86 mixed ir")
    assert_contains(x86_mixed_ir, "store i32 1", label="sizeof x86 mixed ir")
    assert_contains(x86_mixed_ir, "add i32", label="sizeof x86 mixed ir")
    assert_not_contains(x86_mixed_ir, "udiv i64", label="sizeof x86 mixed ir")
    assert_not_contains(x86_mixed_ir, "trunc i64", label="sizeof x86 mixed ir")

    _expect_ir_failure(
        compiler,
        "sizeof_null_bad.lo",
        """
        def main() usize {
            ret sizeof(null)
        }
        """,
        [
            "`sizeof` value operand must have a concrete type",
            "Use `sizeof[T]()` for a type, or pass a typed runtime value.",
        ],
    )

    usize_range = compiler.emit_ir(
        compiler.write_source(
            "usize_literal_range_bad.lo",
            """
            def main() usize {
                ret 4294967296_usize
            }
            """,
        ),
        target="i686-unknown-linux-gnu",
    ).expect_failed()
    assert_contains(
        usize_range.stderr,
        "integer literal is out of range for `usize` on the active target",
        label="usize literal range diagnostic",
    )

    _expect_ir_failure(
        compiler,
        "sizeof_type_in_value_form_bad.lo",
        """
        struct Pair {
            left i32
            right i32
        }

        def main() usize {
            ret sizeof(Pair)
        }
        """,
        [
            "`sizeof` value operand must have a concrete type",
            "Use `sizeof[T]()` for a type, or pass a typed runtime value.",
        ],
    )

def test_numeric_literal_prefix_suffix_and_separator_forms(compiler: CompilerHarness) -> None:
    numeric_ir = _emit_ir(
        compiler,
        "numeric_literal_forms.lo",
        """
        def main() i64 {
            var dec i32 = 12_345
            var bin u16 = 0b1010_0101_u16
            var oct u16 = 0o755_u16
            var hex i32 = 0x10
            var wide u64 = 0x1234_5678_u64
            var sample f32 = 1_234.5_f32
            var table i32[0x4_u16] = {1, 2, 3, 4}
            ret cast[i64](wide)
        }
        """,
    )
    for needle in [
        "store i32 12345",
        "store i16 165",
        "store i16 493",
        "store i32 16",
        "store i64 305419896",
        "store float 1.234500e+03",
        "alloca [4 x i32]",
    ]:
        assert_contains(numeric_ir, needle, label="numeric literal forms ir")

    signed_min_ir = _emit_ir(
        compiler,
        "signed_min_literals.lo",
        """
        def min_i8() i8 {
            ret -128_i8
        }

        def min_i16() i16 {
            ret -32768_i16
        }

        def min_i32() i32 {
            ret -2147483648
        }

        def min_i64() i64 {
            ret -9223372036854775808_i64
        }
        """,
    )
    for needle in [
        "define i8 @min_i8()",
        "store i8 -128",
        "define i16 @min_i16()",
        "store i16 -32768",
        "define i32 @min_i32()",
        "store i32 -2147483648",
        "define i64 @min_i64()",
        "store i64 -9223372036854775808",
    ]:
        assert_contains(signed_min_ir, needle, label="signed min literal ir")

    failures = [
        (
            "binary_literal_digit_bad.lo",
            """
            def main() i32 {
                var x i32 = 0b102
                ret x
            }
            """,
            [
                "I couldn't recognize this numeric literal: `0b102`",
                "Use only `0` and `1` after `0b`",
            ],
        ),
        (
            "numeric_suffix_separator_bad.lo",
            """
            def main() u64 {
                var x u64 = 123u64
                ret x
            }
            """,
            [
                "I couldn't recognize this numeric literal: `123u64`",
                "Numeric type suffixes must use `_type`",
            ],
        ),
        (
            "float_integer_suffix_bad.lo",
            """
            def main() i32 {
                var x = 1.5_i32
                ret 0
            }
            """,
            [
                "floating-point literal cannot use integer suffix `i32`",
                "Use `_f32` or `_f64`",
            ],
        ),
        (
            "positive_signed_min_magnitude_bad.lo",
            """
            def main() i32 {
                var x = 128_i8
                ret 0
            }
            """,
            [
                "integer literal magnitude is only valid with unary `-` for `i8`",
                "Write `-128_i8` if you want the minimum `i8` value.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)

def test_char_literal_semantics(compiler: CompilerHarness) -> None:
    char_ir = _emit_ir(
        compiler,
        "char_literal.lo",
        """
        def main() i32 {
            var a = 'a'
            var nl = '\\n'
            var ctrl = '\\x4'
            ret a + nl + ctrl - 14
        }
        """,
    )
    for needle in ["alloca i8", "store i8 97", "store i8 10", "store i8 4", "zext i8"]:
        assert_contains(char_ir, needle, label="char literal ir")

    failures = [
        (
            "char_empty_bad.lo",
            """
            def main() i32 {
                var a = ''
                ret a
            }
            """,
            ["character literal must contain exactly one ASCII byte"],
        ),
        (
            "char_multi_bad.lo",
            """
            def main() i32 {
                var a = 'ab'
                ret a
            }
            """,
            ["character literal must contain exactly one ASCII byte"],
        ),
        (
            "char_utf8_bad.lo",
            """
            def main() i32 {
                var a = '你'
                ret a
            }
            """,
            ["character literal must contain exactly one ASCII byte"],
        ),
        (
            "char_non_ascii_escape_bad.lo",
            """
            def main() i32 {
                var a = '\\x80'
                ret a
            }
            """,
            ["character literal must be ASCII"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
