from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_addressing_struct_init_named_calls_and_struct_field_types(compiler: CompilerHarness) -> None:
    address_field_ir = _emit_ir(
        compiler,
        "address_field.lo",
        """
        struct Counter {
            set value i32
        }

        def main() i32 {
            var counter Counter
            counter.value = 5
            var p i32* = &counter.value
            *p = 8
            ret counter.value
        }
        """,
    )
    assert_regex(address_field_ir, r"getelementptr inbounds %.*Counter, ptr %.*, i32 0, i32 0", label="address field ir")
    assert_contains(address_field_ir, "store i32 8", label="address field ir")

    address_array_ir = _emit_ir(
        compiler,
        "address_array_elem.lo",
        """
        def main() i32 {
            var row i32[4] = {}
            var p i32* = &row(1)
            *p = 6
            ret row(1)
        }
        """,
    )
    for needle in ["getelementptr inbounds [4 x i32]", "store i32 6"]:
        assert_contains(address_array_ir, needle, label="address array elem ir")

    struct_init_ir = _emit_ir(
        compiler,
        "struct_init.lo",
        """
        struct Complex {
            real i32
            img i32
        }

        def fold(real i32, img i32) i32 {
            ret real + img
        }

        def main() i32 {
            var c = Complex(img = 2, real = 1)
            ret fold(img = c.img, real = c.real)
        }
        """,
    )
    for needle in ["store %", "Complex { i32 1, i32 2 }", "call i32 @fold(i32"]:
        assert_contains(struct_init_ir, needle, label="struct init ir")

    named_call_mix_ir = _emit_ir(
        compiler,
        "named_call_mix.lo",
        """
        def mix(x i32, y i32) i32 {
            ret x + y
        }

        def main() i32 {
            ret mix(1, y = 2)
        }
        """,
    )
    assert_contains(named_call_mix_ir, "call i32 @mix(i32 1, i32 2)", label="named call mix ir")

    struct_field_types_ir = _emit_ir(
        compiler,
        "struct_field_types.lo",
        """
        def inc(v i32) i32 {
            ret v + 1
        }

        struct Mixed {
            flag bool
            ratio f32
            set bits u8[4]
            pair <i32, bool>
            ptr i32*
            cb (i32: i32)
        }

        def main() i32 {
            var x i32 = 41
            var raw u8[4] = cast[f32](1).tobits()
            var pair <i32, bool> = (1, true)
            var mixed = Mixed(flag = true, ratio = cast[f32](1), bits = raw, pair = pair, ptr = &x, cb = @inc)
            if mixed.flag && mixed.pair._2 && (mixed.ratio >= cast[f32](1)) {
                mixed.bits(0) = 1
                ret mixed.cb(*mixed.ptr) + mixed.bits(0) + mixed.pair._1
            }
            ret 0
        }
        """,
    )
    assert_regex(struct_field_types_ir, r"type \{ i8, float, \[4 x i8\], \{ i32, i8 \}, ptr, ptr \}", label="struct field types ir")
    assert_regex(struct_field_types_ir, r"call i32 %.*\(i32 %.*\)", label="struct field types ir")
    assert_regex(struct_field_types_ir, r"getelementptr inbounds .* i32 0, i32 4", label="struct field types ir")
    assert_regex(struct_field_types_ir, r"getelementptr inbounds .* i32 0, i32 5", label="struct field types ir")

    failures = [
        (
            "pointer_arith_bad.lo",
            """
            def main() i32 {
                var raw u8[4] = {1, 2, 3, 4}
                var bytes u8* = &raw(0)
                var next = bytes + 1
                ret 0
            }
            """,
            ["operator `+` doesn't support `u8*` and `i32`"],
        ),
        (
            "address_expr_bad.lo",
            """
            def main() i32 {
                var x i32 = 1
                var p i32* = &(x + 1)
                ret 0
            }
            """,
            ["address-of expects an addressable value"],
        ),
        (
            "address_temp_bad.lo",
            """
            struct Point {
                x i32
                y i32
            }

            def main() i32 {
                var p Point* = &Point(1, 2)
                ret 0
            }
            """,
            ["address-of expects an addressable value"],
        ),
        (
            "ctor_ref_bad.lo",
            """
            struct Complex {
                real i32
                img i32
            }

            def bad() i32 {
                var x i32 = 1
                var c = Complex(ref real = x, img = 2)
                ret 0
            }
            """,
            ["constructor arguments do not accept `ref`", "Constructors copy field values. Remove `ref` from this argument."],
        ),
        (
            "named_call_bad.lo",
            """
            def mix(x i32, y i32) i32 {
                ret x + y
            }

            def bad() i32 {
                ret mix(x = 1)
            }
            """,
            ["missing parameter `y` for function call"],
        ),
        (
            "named_call_order.lo",
            """
            def mix(x i32, y i32) i32 {
                ret x + y
            }

            def bad() i32 {
                ret mix(x = 1, 2)
            }
            """,
            ["positional arguments must come before named arguments"],
        ),
        (
            "ctor_unknown_field.lo",
            """
            struct Complex {
                real i32
                img i32
            }

            def bad() i32 {
                var c = Complex(real = 1, phase = 2)
                ret 0
            }
            """,
            ["unknown field `phase` for constructor `", "Complex"],
        ),
        (
            "struct_ref_field_bad.lo",
            """
            struct Bad {
                ref slot i32
            }
            """,
            ["syntax error: I couldn't parse this statement: unexpected ref.", "help: Check for a missing separator, unmatched delimiter, or mistyped keyword near here."],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
