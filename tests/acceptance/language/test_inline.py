from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains
from tests.harness.compiler import CompilerHarness


def test_inline_bindings_fold_as_compile_time_constants(compiler: CompilerHarness) -> None:
    inline_json = _emit_json(
        compiler,
        "inline_binding_json.lo",
        """
        def main() i32 {
            inline value = 4
            ret value
        }
        """,
    )
    for needle in ['"type": "VarDef"', '"storageKind": "inline"', '"field": "value"']:
        assert_contains(inline_json, needle, label="inline binding json")

    input_path = compiler.write_source(
        "inline_binding_runtime.lo",
        """
        inline base = 4
        inline size i32 = base + cast[i32](sizeof[i64]()) + 1
        inline truth = size == 13
        if !truth {
            ret 1
        }
        ret size
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="inline_binding_runtime"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(13)


def test_inline_string_binding_reuses_same_constant_address(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "inline_string_binding.lo",
        """
        inline msg = "hi"
        inline same = msg == msg
        if msg != msg {
            ret 1
        }
        if !same {
            ret 2
        }
        ret 0
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="inline_string_binding"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)


def test_top_level_inline_string_binding_reuses_same_constant_address_across_functions(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "inline_string_binding_across_functions.lo",
        """
        inline msg = "hi"

        def left() u8 const[*] {
            ret msg
        }

        def right() u8 const[*] {
            ret msg
        }

        if left() != right() {
            ret 1
        }
        ret 0
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="inline_string_binding_across_functions"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)


def test_inline_truthy_pointer_and_short_circuit_semantics(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "inline_truthy_short_circuit.lo",
        """
        inline not_zero = !0
        inline and_ok = 1 && 2
        inline or_ok = 0 || 2
        inline skipped_and = false && (1 / 0 == 1)
        inline skipped_or = 1 || (1 / 0 == 1)
        inline msg = "hi"
        inline view i8 const[*] = cast[i8 const[*]](msg)
        inline ptr_same = view == view

        if !not_zero {
            ret 1
        }
        if !and_ok {
            ret 2
        }
        if !or_ok {
            ret 3
        }
        if skipped_and {
            ret 4
        }
        if !skipped_or {
            ret 5
        }
        if !ptr_same {
            ret 6
        }
        ret 0
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="inline_truthy_short_circuit"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)


def test_top_level_inline_is_visible_to_functions_and_importers_without_symbols(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "inline_import/dep.lo",
        """
        inline answer = 40 + 2
        """,
    )
    main_path = compiler.write_source(
        "inline_import/main.lo",
        """
        import dep

        inline base = 7
        inline total = dep.answer + base

        def local() i32 {
            ret base
        }

        def imported() i32 {
            ret dep.answer
        }

        ret imported() + local() + total - 56
        """,
    )

    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_not_contains(ir, "@base =", label="top-level inline local ir")
    assert_not_contains(ir, "@total =", label="top-level inline local ir")
    assert_not_contains(ir, "@dep.answer =", label="top-level inline import ir")

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="inline_import.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(42)


def test_inline_rejects_poison_integer_folding_cases(compiler: CompilerHarness) -> None:
    failures = [
        (
            "inline_signed_div_overflow_bad.lo",
            """
            def main() i32 {
                inline value i32 = -2147483648 / -1
                ret 0
            }
            """,
            ["inline binding `value` signed division overflows"],
        ),
        (
            "inline_signed_mod_overflow_bad.lo",
            """
            def main() i32 {
                inline value i32 = -2147483648 % -1
                ret 0
            }
            """,
            ["inline binding `value` signed remainder overflows"],
        ),
        (
            "inline_signed_shift_negative_bad.lo",
            """
            def main() i32 {
                inline value i32 = 1 << -1
                ret 0
            }
            """,
            ["inline binding `value` shift count is out of range"],
        ),
        (
            "inline_signed_shift_wide_bad.lo",
            """
            def main() i32 {
                inline value i32 = 1 >> 32
                ret 0
            }
            """,
            ["inline binding `value` shift count is out of range"],
        ),
        (
            "inline_unsigned_shift_wide_bad.lo",
            """
            def main() i32 {
                inline value u32 = 1_u32 << 32_u32
                ret 0
            }
            """,
            ["inline binding `value` shift count is out of range"],
        ),
    ]

    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_inline_rejects_runtime_and_non_scalar_values(compiler: CompilerHarness) -> None:
    failures = [
        (
            "inline_null_infer_bad.lo",
            """
            def main() i32 {
                inline ptr = null
                ret 0
            }
            """,
            ["cannot infer the type of `ptr` from `null`"],
        ),
        (
            "inline_runtime_value_bad.lo",
            """
            def main() i32 {
                var runtime i32 = 1
                inline value i32 = runtime
                ret 0
            }
            """,
            ["inline binding `value` initializer must be a compile-time constant expression"],
        ),
        (
            "inline_tuple_bad.lo",
            """
            def main() i32 {
                inline pair = (1, 2)
                ret 0
            }
            """,
            ["inline binding `pair` only supports scalar and pointer value types"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
