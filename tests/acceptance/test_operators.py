from __future__ import annotations

from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_operator_ir_covers_integer_bool_float_and_pointer_ops(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "operator_ir.lo",
        """
        def compute(a i32, b i32, flag bool) bool {
            var mix i32 = a % b
            mix = mix + (a << 1)
            mix = mix - (b >> 1)
            mix = mix ^ ~a
            mix = mix | (a & b)
            ret (mix <= a) || (flag && (b >= a))
        }

        def float_cmp(a f32, b f32) bool {
            ret (a + b) >= (a - b)
        }

        def unsigned_mix(a u32, b u32) u32 {
            ret (a / b) + (a % b) + (a >> b)
        }

        def bool_bits(a bool, b bool) bool {
            ret (a & b) | (a ^ b)
        }

        def ptr_same(v i32) bool {
            var value i32 = v
            var ptr i32* = &value
            ret ptr == ptr
        }

        def main() i32 {
            var ok bool = compute(9, 4, false)
            var cmp bool = float_cmp(3.0, 1.0)
            var bits bool = bool_bits(true, false)
            var same bool = ptr_same(7)
            if ok || cmp || bits || same {
                ret 1
            }
            ret 0
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    for needle in [
        "srem i32",
        "udiv i32",
        "urem i32",
        "shl i32",
        "ashr i32",
        "lshr i32",
        "and i32",
        "and i1",
        "xor i32",
        "xor i1",
        "or i32",
        "or i1",
        "icmp sle",
        "icmp sge",
        "icmp eq ptr",
        "phi i1",
        "fadd float",
        "fsub float",
        "fcmp oge float",
    ]:
        assert_contains(ir, needle, label="operator ir")


def test_invalid_float_bitwise_operator_is_rejected(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "operator_bad.lo",
        """
        def bad(a f32, b f32) f32 {
            ret a & b
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(result.stderr, "operator `&` doesn't support `f32` and `f32`", label="operator diagnostic")


def test_short_circuit_runtime_skips_side_effects(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "short_circuit.lo",
        """
        def mark(ptr i32*, value i32) bool {
            *ptr = value
            ret true
        }

        def run() i32 {
            var state i32 = 0
            false && mark(&state, 1)
            true || mark(&state, 2)
            ret state
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(input_path, output_name="short_circuit")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)


def test_mixed_signedness_uses_signed_lowering_and_runs(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "mixed_sign.lo",
        """
        def less(a i64, b u8) bool {
            ret a < b
        }

        def divide(a i64, b u8) i64 {
            ret a / b
        }

        def shift(a i64, b u8) i64 {
            ret a >> b
        }

        def run() i32 {
            var a i64 = -1
            var b u8 = 1
            if less(a, b) && divide(a, b) == -1 && shift(a, b) == -1 {
                ret 0
            }
            ret 1
        }

        ret run()
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "icmp slt i64", label="mixed sign ir")
    assert_contains(ir, "sdiv i64", label="mixed sign ir")
    assert_contains(ir, "ashr i64", label="mixed sign ir")

    build_result, exe_path = compiler.build_system_executable(input_path, output_name="mixed_sign")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)

