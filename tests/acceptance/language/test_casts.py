from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_builtin_casts_and_cast_restrictions(compiler: CompilerHarness) -> None:
    numeric_ir = _emit_ir(
        compiler,
        "cast_numeric.lo",
        """
        def main() i32 {
            var base i32 = 1
            var sample f32 = cast[f32](base)
            var widened f64 = cast[f64](sample)
            ret cast[i32](widened)
        }
        """,
    )
    for needle in ["sitofp i32", "fpext float", "fptosi double"]:
        assert_contains(numeric_ir, needle, label="numeric cast ir")

    pointer_ir = _emit_ir(
        compiler,
        "cast_pointer.lo",
        """
        def main() i32 {
            var bytes u8[2] = {7, 9}
            var raw u8* = &bytes(0)
            var view u8[*] = cast[u8[*]](raw)
            ret view(1)
        }
        """,
    )
    for needle in ["ptrtoint ptr %", "inttoptr i64 %"]:
        assert_contains(pointer_ir, needle, label="pointer cast ir")
    assert_regex(pointer_ir, r"getelementptr( inbounds)? i8, ptr %", label="pointer cast ir")

    pointer_rebind_ir = _emit_ir(
        compiler,
        "cast_pointer_rebind.lo",
        """
        struct Box {
            set value i32
        }

        def main() i32 {
            var box Box
            var bytes u8[*] = cast[u8[*]](&box)
            var view Box[*] = cast[Box[*]](bytes)
            view(0).value = 7
            ret box.value
        }
        """,
    )
    for needle in ["define i32 @main", "ptrtoint ptr %", "inttoptr i64 %"]:
        assert_contains(pointer_rebind_ir, needle, label="pointer rebind cast ir")

def test_float_numeric_and_tobits_paths(compiler: CompilerHarness) -> None:
    float_ir = _emit_ir(
        compiler,
        "float.lo",
        """
        def id(v f32) f32 {
            ret v
        }

        def bad() f32 {
            var x f32 = 1.0 + 2.0
            ret id(-x)
        }
        """,
    )
    for needle in ["define float @id", "define float @bad", "store float 3.000000e+00", "fneg float %", "call float @id(float %"]:
        assert_contains(float_ir, needle, label="float ir")

    numeric_convert_ir = _emit_ir(
        compiler,
        "numeric_convert.lo",
        """
        def main() i32 {
            var base i32 = cast[i32](1.5)
            var sample f32 = cast[f32](base)
            var promoted f64 = sample
            ret cast[i32](promoted)
        }
        """,
    )
    for needle in ["fptosi double", "sitofp i32", "fpext float"]:
        assert_contains(numeric_convert_ir, needle, label="numeric convert ir")

    numeric_convert_chain_ir = _emit_ir(
        compiler,
        "numeric_convert_chain.lo",
        """
        def chain(v f64) i32 {
            ret cast[i32](cast[f32](cast[i32](v)))
        }
        """,
    )
    for needle in ["fptosi double", "sitofp i32", "fptosi float"]:
        assert_contains(numeric_convert_chain_ir, needle, label="numeric convert chain ir")

    explicit_f64_to_f32_ir = _emit_ir(
        compiler,
        "explicit_f64_to_f32.lo",
        """
        def hold(v f32) f32 {
            ret v
        }

        def main() i32 {
            const x = 1.5_f32
            var y f32 = hold(2.5_f64)
            ret cast[i32](x + y)
        }
        """,
    )
    for needle in [
        "store float 1.500000e+00",
        "call float @hold(float 2.500000e+00)",
        "fadd float",
        "fptosi float",
    ]:
        assert_contains(explicit_f64_to_f32_ir, needle, label="explicit f64 to f32 ir")

    tobits_ir = _emit_ir(
        compiler,
        "tobits.lo",
        """
        def main() i32 {
            var v i8 = -1
            var raw u8[1] = v.tobits()
            var wide i32 = raw.toi32()
            ret wide
        }
        """,
    )
    for needle in ["insertvalue [1 x i8]", "extractvalue [1 x i8]", "zext i8"]:
        assert_contains(tobits_ir, needle, label="tobits ir")
    assert_not_contains(tobits_ir, "sext i8", label="tobits ir")

    tobits_infer_ir = _emit_ir(
        compiler,
        "tobits_infer.lo",
        """
        def main() i32 {
            var raw = 7.tobits()
            ret raw.toi32()
        }
        """,
    )
    for needle in ['alloca [4 x i8]', 'store [4 x i8] c"\\07\\00\\00\\00"', 'extractvalue [4 x i8]']:
        assert_contains(tobits_infer_ir, needle, label="tobits infer ir")

    wide_bits_ir = _emit_ir(
        compiler,
        "wide_bits.lo",
        """
        def main() i32 {
            var raw u8[256] = {}
            ret raw.toi32()
        }
        """,
    )
    assert wide_bits_ir.count("extractvalue [256 x i8]") == 4, wide_bits_ir
    assert_not_contains(wide_bits_ir, "i2048", label="wide bits ir")

    implicit_numeric_ir = _emit_ir(
        compiler,
        "implicit_numeric.lo",
        """
        def widen(v u8) u16 {
            ret v
        }

        def main() i32 {
            var x u8 = 1
            ret widen(x)
        }
        """,
    )
    for needle in ["store i8 1", "zext i8", "zext i16"]:
        assert_contains(implicit_numeric_ir, needle, label="implicit numeric ir")

    mixed_numeric_op_ir = _emit_ir(
        compiler,
        "mixed_numeric_op.lo",
        """
        def main() i32 {
            var a u8 = 1
            var b i32 = 2
            ret a + b
        }
        """,
    )
    for needle in ["zext i8", "add i32"]:
        assert_contains(mixed_numeric_op_ir, needle, label="mixed numeric op ir")

    float_nan_cmp_ir = _emit_ir(
        compiler,
        "float_nan_cmp.lo",
        """
        def bad() bool {
            var z f64 = 0.0
            var nan f64 = z / z
            ret nan != nan
        }
        """,
    )
    assert_contains(float_nan_cmp_ir, "fcmp une double", label="float nan cmp ir")

    failures = [
        (
            "numeric_cross_bad.lo",
            """
            def main() i32 {
                var x f32 = 1
                ret 0
            }
            """,
            ["initializer type mismatch for `x`: expected f32, got i32", "cast[T](expr)"],
        ),
        (
            "numeric_member_removed.lo",
            """
            def main() i32 {
                var x i32 = 1
                var y f32 = x.tof32()
                ret 0
            }
            """,
            ["unknown member `i32.tof32`"],
        ),
        (
            "injected_member_bad.lo",
            """
            def main() i32 {
                var x = 1.tobits
                ret 0
            }
            """,
            ["injected member `tobits` can only be used as a direct call callee"],
        ),
        (
            "float_literal_target_bad.lo",
            """
            def bad() u64 {
                var x u64 = 1.25
                ret x
            }
            """,
            ["floating-point literal doesn't match the expected target type"],
        ),
        (
            "bool_bytecopy_bad.lo",
            """
            def bad() i8 {
                var b bool = true
                var x i8 = b
                ret x
            }
            """,
            ["initializer type mismatch for `x`: expected i8, got bool"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
