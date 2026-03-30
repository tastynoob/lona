from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _expect_ir_failure
from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_global_scalar_definition_and_mutation(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "global_scalar.lo",
        """
        global counter = 1

        def bump(step i32) i32 {
            counter = counter + step
            ret counter
        }

        ret bump(5)
        """,
    )

    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "@counter = global i32 1", label="global ir")

    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="global_scalar"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(6)


def test_imported_module_global_access_and_mutation(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "import_global/dep.lo",
        """
        global count = 5
        """,
    )
    main_path = compiler.write_source(
        "import_global/main.lo",
        """
        import dep

        dep.count = dep.count + 4
        ret dep.count
        """,
    )

    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "@dep.count = global i32 5", label="imported global ir")

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="import_global.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(9)


def test_hosted_args_are_accessible_via_extern_globals(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "hosted_args_global.lo",
        """
#[extern]
        global __lona_argc i32

#[extern]
        global __lona_argv u8 const[*][*]

        ret __lona_argv(1)(0) + __lona_argc
        """,
    )

    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="hosted_args_global"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path, args=["A"]).expect_exit_code(67)


def test_global_initializer_rejects_non_static_expressions(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "global_non_static_bad.lo",
        """
        def seed() i32 {
            ret 1
        }

        global value = seed()

        ret value
        """,
    )

    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "global `value` requires a statically typed initializer for inference",
        label="global non-static diagnostic",
    )


def test_global_initializer_preserves_scalar_type_checks(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "global_scalar_float_to_int_bad.lo",
            """
            global x i32 = 1.25

            ret x
            """,
            ["floating-point literal doesn't match the expected target type"],
        ),
        (
            "global_scalar_int_to_float_bad.lo",
            """
            global y f32 = 1

            ret 0
            """,
            ["global `y` initializer type mismatch: expected f32, got i32"],
        ),
        (
            "global_scalar_int_to_bool_bad.lo",
            """
            global b bool = 2

            ret 0
            """,
            ["global `b` initializer type mismatch: expected bool, got i32"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_global_initializer_accepts_implicit_numeric_widening(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "global_numeric_widen.lo",
        """
        global wide_signed i64 = 1
        global wide_unsigned u64 = 1_u8
        global wide_float f64 = 1.5_f32

        ret 0
        """,
    )

    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "@wide_signed = global i64 1", label="signed widen ir")
    assert_contains(ir, "@wide_unsigned = global i64 1", label="unsigned widen ir")
    assert_contains(
        ir,
        "@wide_float = global double 1.500000e+00",
        label="float widen ir",
    )


def test_global_string_literal_requires_const_byte_views(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "global_string_mut_ptr_bad.lo",
            """
            global p u8* = "x"

            ret 0
            """,
            ["global `p` initializer type mismatch: expected u8*, got u8 const[*]"],
        ),
        (
            "global_string_mut_indexable_bad.lo",
            """
            global p u8[*] = "x"

            ret 0
            """,
            ["global `p` initializer type mismatch: expected u8[*], got u8 const[*]"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_global_unary_plus_requires_numeric_literals(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "global_unary_plus_bool_bad.lo",
            """
            global g = +true

            ret 0
            """,
            ["operator `+` doesn't support `bool`"],
        ),
        (
            "global_unary_plus_string_bad.lo",
            """
            global g = +"x"

            ret 0
            """,
            ["operator `+` doesn't support `u8 const[*]`"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
