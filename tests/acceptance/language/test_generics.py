from __future__ import annotations

from tests.acceptance.language._syntax_helpers import (
    _emit_ir,
    _emit_json,
    _expect_ir_failure,
)
from tests.harness import assert_contains, assert_not_contains
from tests.harness.compiler import CompilerHarness


def test_generic_v0_surface_json_includes_type_params_type_apply_and_any_pointer_types(
    compiler: CompilerHarness,
) -> None:
    json_out = _emit_json(
        compiler,
        "generic_surface_json.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Box[T] {
            value T
        }

        struct Pair[A, B] {
            left A
            right B
        }

        impl[T] Box![T]: Hash

        def id[T](value T) T {
            ret value
        }

        def show() {
            var typed_box Box![i32]
            var typed_pair Pair![i32, bool]
            var value i32 = 1
            var typed i32* = &value
            var erased any* = cast[any*](typed)
            var readonly any const* = cast[any const*](typed)
            id[i32](1)
            var cb (i32: i32) = id[i32]&<>
        }
        """,
    )
    for needle in [
        '"typeParams": [',
        '"selfType": "Box![T]"',
        '"type": "TypeApply"',
        '"type": "FuncRef"',
        '"typeArgs": [',
        '"declaredType": "Box![i32]"',
        '"declaredType": "Pair![i32, bool]"',
        '"declaredType": "any*"',
        '"declaredType": "any const*"',
    ]:
        assert_contains(json_out, needle, label="generic surface json")


def test_generic_v0_frontend_rejects_type_args_on_non_generic_box(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "generic_box_type_position_frontend.lo",
        """
        struct Box {
            value i32
        }

        def show() {
            var box Box![i32]
        }
        """,
        [
            "type `Box![i32]` applies `![...]` arguments to a non-generic type",
            "Remove the `![...]` arguments, or make the base type generic before specializing it.",
        ],
    )


def test_generic_v0_frontend_rejects_by_value_storage_of_opaque_applied_type(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "generic_box_by_value_storage_round4.lo",
        """
        struct Box[T] {
            value T
        }

        def show() {
            var box Box![i32]
        }
        """,
        [
            "opaque struct `Box![i32]` cannot be used by value in variable `box`",
            "Use `Box![i32]*` instead. Opaque structs are only supported behind pointers.",
        ],
    )


def test_generic_v0_template_decls_reach_frontend_without_unknown_t_failures(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_template_decls_round3.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }

        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="generic template decl ir")
    assert_not_contains(ir, "@id(", label="generic template decl ir")
    assert_not_contains(ir, "@Box.get", label="generic template decl ir")


def test_generic_v0_template_bodies_still_report_undefined_names_and_types(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_struct_method_body_resolves_names_round9.lo",
            """
            struct Box[T] {
                def get() T {
                    ret missing
                }
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "undefined identifier `missing`",
                "Declare it with `var` before using it, or check the spelling.",
            ],
        ),
        (
            "generic_function_body_validates_local_types_round9.lo",
            """
            def make[T]() i32 {
                var x U
                ret 0
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "unknown type for local variable `x`: U",
                "Type parameters are only visible inside the generic item that declares them.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_generic_v0_template_bodies_accept_local_uses_of_visible_type_params(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_template_body_uses_type_params_round10.lo",
        """
        struct Box[T] {
            def keep(ptr T*) T* {
                var local T* = ptr
                ret local
            }
        }

        def keep_ptr[T](ptr T*) T* {
            var local T* = ptr
            ret local
        }

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="generic template body ir")
    assert_not_contains(ir, "@keep_ptr(", label="generic template body ir")
    assert_not_contains(ir, "@Box.keep", label="generic template body ir")


def test_generic_v0_explicit_type_args_enter_semantic_call_path(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_explicit_type_args_round3.lo",
        """
        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            ret id[i32](1)
        }
        """,
    )
    assert_contains(
        ir,
        "@generic_explicit_type_args_round3.id__inst__i32",
        label="generic explicit instantiation ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_explicit_type_args_round3.id__inst__i32(i32 1)",
        label="generic explicit instantiation ir",
    )


def test_generic_v0_specialized_function_refs_use_concrete_signature_path(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_specialized_func_ref_round8.lo",
        """
        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            var cb (i32: i32) = id[i32]&<>
            ret cb(1)
        }
        """,
    )
    assert_contains(
        ir,
        "store ptr @generic_specialized_func_ref_round8.id__inst__i32",
        label="generic function ref ir",
    )
    assert_contains(
        ir,
        "call i32 %2(i32 1)",
        label="generic function ref ir",
    )


def test_generic_v0_reports_type_arg_arity_and_inference_diagnostics(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_type_arg_arity_round3.lo",
            """
            def id[T](value T) T {
                ret value
            }

            def main() i32 {
                ret id[i32, bool](1)
            }
            """,
            [
                "generic type argument count mismatch for `id`: expected 1, got 2",
                "Match the number of `[` `]` type arguments to the generic parameter list.",
            ],
        ),
        (
            "generic_type_inference_missing_round3.lo",
            """
            def choose[T](value i32) T {
                ret value
            }

            def main() i32 {
                ret choose(1)
            }
            """,
            [
                "cannot infer generic type argument `T` for `choose`",
                "Pass explicit type arguments like `choose[T](...)`.",
            ],
        ),
        (
            "generic_template_runtime_value_bad_round11.lo",
            """
            def id[T](value T) T {
                ret value
            }

            def main() i32 {
                var f = id
                ret 0
            }
            """,
            [
                "generic function `id` cannot be used as a runtime value before instantiation",
                "Call it directly, for example `id[T](...)`, or wait until monomorphization support lands.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_generic_v0_direct_inference_emits_concrete_instance(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_direct_inference_round3.lo",
        """
        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            ret id(1)
        }
        """,
    )
    assert_contains(
        ir,
        "@generic_direct_inference_round3.id__inst__i32",
        label="generic direct inference ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_direct_inference_round3.id__inst__i32(i32 1)",
        label="generic direct inference ir",
    )


def test_generic_v0_recursively_substitutes_pointer_signatures_before_pending_instantiation(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_pointer_substitution_round4.lo",
        """
        def passthrough_ptr[T](value T*) T* {
            ret value
        }

        def main() i32 {
            var value i32 = 1
            var ptr i32* = &value
            var out i32* = passthrough_ptr(ptr)
            ret *out
        }
        """,
    )
    assert_contains(
        ir,
        "define ptr @generic_pointer_substitution_round4.passthrough_ptr__inst__i32",
        label="generic pointer substitution ir",
    )


def test_generic_v0_allows_box_t_pointer_signatures_before_pending_instantiation(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_box_t_pointer_signature_round5.lo",
        """
        struct Box[T] {
            value T
        }

        def take_box_ptr[T](value Box![T]*) Box![T]* {
            ret value
        }

        def main() i32 {
            var box Box![i32]* = null
            var out Box![i32]* = take_box_ptr(box)
            ret 0
        }
        """,
    )
    assert_contains(
        ir,
        "define ptr @generic_box_t_pointer_signature_round5.take_box_ptr__inst__i32",
        label="generic applied pointer ir",
    )


def test_generic_v0_infers_applied_type_args_through_pair_pointer_patterns(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_pair_t_bool_pointer_signature_round5.lo",
        """
        struct Pair[A, B] {
            left A
            right B
        }

        def take_pair_ptr[T](value Pair![T, bool]*) Pair![T, bool]* {
            ret value
        }

        def main() i32 {
            var pair Pair![i32, bool]* = null
            var out Pair![i32, bool]* = take_pair_ptr(pair)
            ret 0
        }
        """,
    )
    assert_contains(
        ir,
        "define ptr @generic_pair_t_bool_pointer_signature_round5.take_pair_ptr__inst__i32",
        label="generic pair pointer ir",
    )


def test_generic_v0_infers_applied_type_args_through_tuple_patterns(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_tuple_applied_signature_round10.lo",
        """
        struct Pair[A, B] {
            left A
            right B
        }

        def take_tuple[T](value <Pair![T, bool]*, i32>) Pair![T, bool]* {
            ret value._1
        }

        def main() i32 {
            var pair Pair![i32, bool]* = null
            var out Pair![i32, bool]* = take_tuple((pair, 1))
            ret 0
        }
        """,
    )
    assert_contains(
        ir,
        "define ptr @generic_tuple_applied_signature_round10.take_tuple__inst__i32",
        label="generic tuple pointer ir",
    )


def test_generic_v0_infers_applied_type_args_through_const_pointer_patterns(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_const_applied_signature_round10.lo",
        """
        struct Box[T] {
            value T
        }

        def borrow_const[T](value Box![T] const*) Box![T] const* {
            ret value
        }

        def main() i32 {
            var ptr Box![i32] const* = null
            var out Box![i32] const* = borrow_const(ptr)
            ret 0
        }
        """,
    )
    assert_contains(
        ir,
        "define ptr @generic_const_applied_signature_round10.borrow_const__inst__i32",
        label="generic const pointer ir",
    )


def test_generic_v0_rejects_bare_generic_templates_in_runtime_type_positions(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "generic_bare_template_runtime_type_round4.lo",
        """
        struct Box[T] {
            value T
        }

        def main() i32 {
            var p Box* = null
            ret 0
        }
        """,
        [
            "generic type template `Box` requires explicit `![...]` type arguments",
            "Write `Box![...]` with explicit type arguments",
        ],
    )


def test_generic_v0_reports_targeted_diagnostics_for_surface_cut_limits(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_angle_type_bad.lo",
            """
            def bad(ptr Name<T>) {
            }
            """,
            [
                "generic apply uses `![...]`, not `<>`",
                "Write `Name![T]` instead of `Name<T>` in generic v0.",
            ],
        ),
        (
            "generic_old_bang_apply_bad.lo",
            """
            def id[T](value T) T {
                ret value
            }

            def main() i32 {
                ret id![i32](1)
            }
            """,
            [
                "expression-side generic apply uses `[...]`, not `![...]`",
                "Write `name[T](...)`, `name[T]&<>`, or `Type[T](...)` in expression contexts. Reserve `Type![T]` for handwritten type strings.",
            ],
        ),
        (
            "generic_old_square_type_apply_bad.lo",
            """
            struct Box[T] {
                value T
            }

            def main() i32 {
                var box Box[i32]
                ret 0
            }
            """,
            [
                "generic apply uses `![...]`, not `[...]`",
                "Write `Type![T]` in handwritten type strings. Expression-side calls and constructors use `name[T](...)`.",
            ],
        ),
        (
            "generic_old_square_nested_type_apply_bad.lo",
            """
            struct Box[T] {
                value T
            }

            def main() i32 {
                var pair <Box[i32]*, i32>
                ret 0
            }
            """,
            [
                "generic apply uses `![...]`, not `[...]`",
                "Write `Type![T]` in handwritten type strings. Expression-side calls and constructors use `name[T](...)`.",
            ],
        ),
        (
            "generic_struct_method_bad.lo",
            """
            struct Box {
                def map[T](value T) T {
                    ret value
                }
            }
            """,
            [
                "generic methods are not supported in generic v0:",
                "move type variation to the enclosing type instead of declaring a generic method",
            ],
        ),
        (
            "generic_trait_method_bad.lo",
            """
            trait Hash {
                def map[T](value T) T
            }
            """,
            [
                "generic methods are not supported in generic v0: `Hash.map`",
                "keep trait v0 methods monomorphic in the first implementation cut",
            ],
        ),
        (
            "generic_bare_any_var_bad.lo",
            """
            def bad() {
                var x any
            }
            """,
            [
                "bare `any` is not a value type in generic v0: any",
                "Use `any*`, `any const*`, `any[*]`, or `any const[*]`",
            ],
        ),
        (
            "generic_bare_any_return_bad.lo",
            """
            def bad() any {
                ret 0
            }
            """,
            [
                "bare `any` is not a value type in generic v0: any",
                "Use `any*`, `any const*`, `any[*]`, or `any const[*]`",
            ],
        ),
        (
            "generic_bare_any_field_bad.lo",
            """
            struct Bad {
                value any
            }
            """,
            [
                "bare `any` is not a value type in generic v0: any",
                "Use `any*`, `any const*`, `any[*]`, or `any const[*]`",
            ],
        ),
        (
            "generic_bare_any_type_arg_bad.lo",
            """
            struct Box[T] {
                value T
            }

            def main() i32 {
                var box Box![any]* = null
                ret 0
            }
            """,
            [
                "bare `any` is not a value type in generic v0: any",
                "Use `any*`, `any const*`, `any[*]`, or `any const[*]`",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_generic_v0_trait_impl_headers_accept_applied_self_types(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_trait_impl_applied_self_headers_round9.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Box[T] {
            def hash() i32 {
                ret 1
            }
        }

        impl[T] Box![T]: Hash
        impl Box![i32]: Hash

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="generic trait impl header ir")
