from __future__ import annotations

from tests.acceptance.language._syntax_helpers import (
    _emit_ir,
    _emit_json,
    _expect_ir_failure,
)
from tests.harness import assert_contains, assert_not_contains, assert_regex
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

        impl[T Hash] Box[T]: Hash

        def id[T](value T) T {
            ret value
        }

        def show() {
            var typed_box Box[i32]
            var typed_pair Pair[i32, bool]
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
        '"boundTrait": "Hash"',
        '"selfType": "Box[T]"',
        '"type": "TypeApply"',
        '"type": "FuncRef"',
        '"typeArgs": [',
        '"declaredType": "Box[i32]"',
        '"declaredType": "Pair[i32, bool]"',
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
            var box Box[i32]
        }
        """,
        [
            "type `Box[i32]` applies `[...]` arguments to a non-generic type",
            "Remove the `[...]` arguments, or make the base type generic before specializing it.",
        ],
    )


def test_generic_v0_same_module_applied_structs_support_by_value_storage(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_box_by_value_storage_round3.lo",
        """
        struct Box[T] {
            value T
        }

        def show() {
            var box Box[i32]
        }
        """,
    )
    assert_contains(ir, "define void @show()", label="generic box by value ir")


def test_generic_v0_same_module_applied_structs_support_ctor_field_param_return_and_method(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_box_runtime_round3.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }

        struct Holder {
            box Box[i32]
        }

        def make_box() Box[i32] {
            ret Box[i32](value = 41)
        }

        def take_box(box Box[i32]) i32 {
            ret box.get()
        }

        def main() i32 {
            var box Box[i32] = make_box()
            var holder Holder = Holder(box = box)
            ret take_box(holder.box) + 1
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="generic box runtime ir")
    assert_contains(
        ir,
        "define",
        label="generic box runtime ir",
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

        def size_of_item[T]() usize {
            ret sizeof[T]()
        }

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="generic template body ir")
    assert_not_contains(ir, "@keep_ptr(", label="generic template body ir")
    assert_not_contains(ir, "@Box.keep", label="generic template body ir")
    assert_not_contains(ir, "@size_of_item(", label="generic template body ir")


def test_generic_v0_template_bodies_reject_unconstrained_capability_assumptions(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_unconstrained_method_capability_bad_round7.lo",
            """
            def bad[T](obj T) i32 {
                ret obj.hash()
            }
            """,
            [
                "unconstrained generic parameter `T` does not provide member `hash`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
        (
            "generic_unconstrained_field_capability_bad_round7.lo",
            """
            def bad[T](obj T) i32 {
                ret obj.value
            }
            """,
            [
                "unconstrained generic parameter `T` does not provide member `value`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
        (
            "generic_unconstrained_equality_capability_bad_round7.lo",
            """
            def bad[T](left T, right T) bool {
                ret left == right
            }
            """,
            [
                "unconstrained generic parameter `T` does not support operator `==`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
        (
            "generic_unconstrained_operator_capability_bad_round7.lo",
            """
            def bad[T](left T, right T) T {
                var local = left
                ret local + right
            }
            """,
            [
                "unconstrained generic parameter `T` does not support operator `+`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_generic_v0_template_bodies_reject_unconstrained_helper_alias_escapes(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_unconstrained_helper_alias_method_bad_round8.lo",
            """
            def id[T](value T) T {
                ret value
            }

            def bad[T](obj T) i32 {
                var alias = id(obj)
                ret alias.hash()
            }
            """,
            [
                "unconstrained generic parameter `T` does not provide member `hash`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
        (
            "generic_unconstrained_helper_alias_applied_bad_round8.lo",
            """
            struct Box[T] {
                value T
            }

            def head[T](box Box[T]) T {
                ret box.value
            }

            def bad[T](box Box[T]) i32 {
                var alias = head(box)
                ret alias.hash()
            }
            """,
            [
                "unconstrained generic parameter `T` does not provide member `hash`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
        (
            "generic_unconstrained_method_alias_applied_bad_round9.lo",
            """
            struct Box[T] {
                value T

                def get() T {
                    ret self.value
                }
            }

            def bad[T](box Box[T]) i32 {
                var alias = box.get()
                ret alias.hash()
            }
            """,
            [
                "unconstrained generic parameter `T` does not provide member `hash`",
                "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_generic_v0_concrete_helper_aliases_still_allow_member_access(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_concrete_helper_alias_round8.lo",
        """
        struct Point {
            def hash() i32 {
                ret 7
            }
        }

        def id[T](value T) T {
            ret value
        }

        def main() i32 {
            var alias = id(Point())
            ret alias.hash()
        }
        """,
    )
    assert_contains(
        ir,
        "@generic_concrete_helper_alias_round8.Point.hash",
        label="generic concrete helper alias ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_concrete_helper_alias_round8.Point.hash(",
        label="generic concrete helper alias ir",
    )


def test_generic_v0_concrete_method_aliases_still_allow_member_access(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_concrete_method_alias_round9.lo",
        """
        struct Point {
            def hash() i32 {
                ret 7
            }
        }

        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }

        def main() i32 {
            var box = Box[Point](value = Point())
            var alias = box.get()
            ret alias.hash()
        }
        """,
    )
    assert_contains(
        ir,
        "@generic_concrete_method_alias_round9.Point.hash",
        label="generic concrete method alias ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_concrete_method_alias_round9.Point.hash(",
        label="generic concrete method alias ir",
    )


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
                "Call it directly, for example `id[T](...)`, or instantiate it first with `id[T]&<>` if you need a function pointer.",
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

        def take_box_ptr[T](value Box[T]*) Box[T]* {
            ret value
        }

        def main() i32 {
            var box Box[i32]* = null
            var out Box[i32]* = take_box_ptr(box)
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

        def take_pair_ptr[T](value Pair[T, bool]*) Pair[T, bool]* {
            ret value
        }

        def main() i32 {
            var pair Pair[i32, bool]* = null
            var out Pair[i32, bool]* = take_pair_ptr(pair)
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

        def take_tuple[T](value <Pair[T, bool]*, i32>) Pair[T, bool]* {
            ret value._1
        }

        def main() i32 {
            var pair Pair[i32, bool]* = null
            var out Pair[i32, bool]* = take_tuple((pair, 1))
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

        def borrow_const[T](value Box[T] const*) Box[T] const* {
            ret value
        }

        def main() i32 {
            var ptr Box[i32] const* = null
            var out Box[i32] const* = borrow_const(ptr)
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
            "generic type template `Box` requires explicit `[...]` type arguments",
            "Write `Box[...]` with explicit type arguments",
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
                "generic apply uses `[...]`, not `<>`",
                "Write `Name[T]` instead of `Name<T>` in type strings.",
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
                var box Box[any]* = null
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

        impl[T Hash] Box[T]: Hash
        impl Box[i32]: Hash

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="generic trait impl header ir")


def test_generic_v0_bounded_generic_functions_require_visible_impls_and_trait_qualified_calls(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_bound_function_round12.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: Hash

        def hash_one[T Hash](value T) i32 {
            ret Hash.hash(&value)
        }

        def main() i32 {
            ret hash_one(Point(value = 41))
        }
        """,
    )
    assert_regex(
        ir,
        r"@generic_bound_function_round12\.hash_one__inst__.*Point",
        label="generic bound function ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_bound_function_round12.Point.hash(",
        label="generic bound function ir",
    )


def test_generic_v0_bound_failures_are_checked_at_instantiation_sites(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "generic_bound_failure_round12.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32
        }

        def hash_one[T Hash](value T) i32 {
            ret 0
        }

        def main() i32 {
            ret hash_one(Point(value = 41))
        }
        """,
        [
            "type `generic_bound_failure_round12.Point` does not satisfy bound `generic_bound_failure_round12.Hash` for generic parameter `T` in generic function `hash_one`",
            "Add `impl generic_bound_failure_round12.Point: generic_bound_failure_round12.Hash` in a visible module, or choose a type that already satisfies the bound.",
        ],
    )


def test_generic_v0_bounded_params_allow_plain_dot_lookup_for_bound_methods(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_bound_member_lookup_ok_round14.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: Hash

        def hash_one[T Hash](value T) i32 {
            ret value.hash()
        }

        def main() i32 {
            ret hash_one(Point(value = 41))
        }
        """,
    )
    assert_regex(
        ir,
        r"@generic_bound_member_lookup_ok_round14\.hash_one__inst__.*Point",
        label="bounded dot lookup ir",
    )
    assert_contains(
        ir,
        "call i32 @generic_bound_member_lookup_ok_round14.Point.hash(",
        label="bounded dot lookup ir",
    )


def test_generic_v0_bounded_array_projection_results_allow_plain_dot_lookup(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_bound_array_projection_ok_round15.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: Hash

        struct Box[T Hash] {
            items T[2]

            set def store(index i32, value T) {
                self.items(index) = value
            }

            def slot_hash(index i32) i32 {
                ret self.items(index).hash()
            }
        }

        def main() i32 {
            var box Box[Point]
            box.store(0, Point(value = 10))
            box.store(1, Point(value = 41))
            ret box.slot_hash(1)
        }
        """,
    )
    assert_contains(
        ir,
        "call i32 @generic_bound_array_projection_ok_round15.Point.hash(",
        label="bounded array projection ir",
    )


def test_generic_v0_bounded_params_still_reject_bare_member_read_and_write(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_bound_bare_member_read_rejected_round16.lo",
            """
            trait Value {
                def value() i32
            }

            struct Record {
                value i32

                def value() i32 {
                    ret self.value
                }
            }

            impl Record: Value

            def bad_read[T Value](x T) i32 {
                ret x.value
            }
            """,
            [
                "generic parameter `T` does not provide member `value` through bound `Value`",
                "Bounded generic parameters only allow methods provided by bound `Value`, such as `value.method()`.",
            ],
        ),
        (
            "generic_bound_bare_member_write_rejected_round16.lo",
            """
            trait Value {
                def value() i32
            }

            struct Record {
                value i32

                def value() i32 {
                    ret self.value
                }
            }

            impl Record: Value

            def bad_write[T Value](x T) i32 {
                var copy = x
                copy.value = 9
                ret 0
            }
            """,
            [
                "generic parameter `T` does not provide member `value` through bound `Value`",
                "Bounded generic parameters only allow methods provided by bound `Value`, such as `value.method()`.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_generic_v0_trait_impl_headers_enable_trait_qualified_calls_for_applied_generic_receivers(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_trait_impl_static_call_round12.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value
            }
        }

        impl Point: Hash

        struct Box[T] {
            value T

            def hash() i32 {
                ret 1
            }
        }

        impl[T Hash] Box[T]: Hash

        def main() i32 {
            var box Box[Point] = Box[Point](value = Point(value = 7))
            ret Hash.hash(&box)
        }
        """,
    )
    assert_regex(
        ir,
        r"define i32 @Box_5b.*Point.*_5d\.hash\(ptr ",
        label="generic trait impl static call ir",
    )
    assert_regex(
        ir,
        r"call i32 @Box_5b.*Point.*_5d\.hash\(ptr ",
        label="generic trait impl static call ir",
    )


def test_generic_v0_struct_decl_bounds_and_generic_methods_lower_for_same_module_receivers(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "generic_struct_decl_bound_and_method_round13.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        struct Other {
            value i32

            def hash() i32 {
                ret self.value + 10
            }
        }

        impl Point: Hash
        impl Other: Hash

        struct Box[T Hash] {
            value T

            def hash_value() i32 {
                ret Hash.hash(&self.value)
            }

            def hash() i32 {
                ret self.hash_value()
            }

            def echo[U](value U) U {
                ret value
            }

            def score_with[U Hash](other U) i32 {
                ret Hash.hash(&self.value) + Hash.hash(&other)
            }
        }

        impl[T Hash] Box[T]: Hash

        def main() i32 {
            var box Box[Point] = Box[Point](value = Point(value = 41))
            if box.hash_value() != 42 {
                ret 1
            }
            if box.echo[i32](7) != 7 {
                ret 2
            }
            if !box.echo(true) {
                ret 3
            }
            if box.score_with(Other(value = 5)) != 57 {
                ret 4
            }
            ret Hash.hash(&box)
        }
        """,
    )
    assert_regex(
        ir,
        r"@Box_5b.*Point.*_5d\.echo__inst__i32",
        label="generic method explicit instantiation ir",
    )
    assert_regex(
        ir,
        r"@Box_5b.*Point.*_5d\.echo__inst__bool",
        label="generic method inferred instantiation ir",
    )
    assert_regex(
        ir,
        r"@Box_5b.*Point.*_5d\.score_with__inst__.*Other",
        label="generic method bounded instantiation ir",
    )
    assert_regex(
        ir,
        r"call i32 @Box_5b.*Point.*_5d\.hash_value\(ptr ",
        label="struct decl bound method ir",
    )


def test_generic_v0_struct_decl_bounds_are_checked_when_materializing_applied_types(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "generic_struct_decl_bound_failure_round13.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32
        }

        struct Box[T Hash] {
            value T
        }

        def main() i32 {
            var box Box[Point] = Box[Point](value = Point(value = 41))
            ret 0
        }
        """,
        [
            "type `generic_struct_decl_bound_failure_round13.Point` does not satisfy bound `generic_struct_decl_bound_failure_round13.Hash` for generic parameter `T` in generic type `generic_struct_decl_bound_failure_round13.Box`",
            "Add `impl generic_struct_decl_bound_failure_round13.Point: generic_struct_decl_bound_failure_round13.Hash` in a visible module, or choose a type that already satisfies the bound.",
        ],
    )


def test_generic_v0_rejects_multi_bounds(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "generic_multi_bound_bad_round12.lo",
            """
            trait Hash {
                def hash() i32
            }

            trait Eq {
                def eq(other i32) bool
            }

            def hash_one[T Hash + Eq](value T) i32 {
                ret 0
            }
            """,
            [
                "generic v0 only supports a single trait bound per type parameter",
                "Write one bound like `[T Hash]` for now. Multi-bound forms like `[T Hash + Eq]` are not supported yet.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
