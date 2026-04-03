from __future__ import annotations

import os
import re

from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_method_self_lowering_uses_struct_gep(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "method_self.lo",
        """
        struct Counter {
            value i32

            def bump(step i32) i32 {
                ret self.value + step
            }
        }

        def main() i32 {
            var c = Counter(value = 2)
            ret c.bump(3)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"@.*Counter\.bump", label="method self ir")
    assert_regex(ir, r"getelementptr inbounds %.*Counter", label="method self ir")


def test_mutating_method_lowers_to_pointer_receiver(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "method_self_mutate.lo",
        """
        struct Counter {
            value i32

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }

        def main() i32 {
            var c = Counter(value = 2)
            c.bump(3)
            ret c.value
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^define i32 @.*Counter\.bump\(ptr ", label="mutating method ir")
    assert_regex(ir, r"call i32 @.*Counter\.bump\(ptr ", label="mutating method ir")
    assert_regex(ir, r"store i32 .*ptr %", label="mutating method ir")


def test_method_self_can_be_used_as_pointer_value(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "method_self_pointer.lo",
        """
        struct Counter {
            value i32

            set def bump(step i32) i32 {
                var self_ptr Counter* = self
                self_ptr.value = self_ptr.value + step
                ret self_ptr.value
            }
        }

        def main() i32 {
            var c = Counter(value = 2)
            ret c.bump(3)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^define i32 @.*Counter\.bump\(ptr ", label="method self pointer ir")
    assert_regex(ir, r"call i32 @.*Counter\.bump\(ptr ", label="method self pointer ir")


def test_set_field_allows_external_assignment(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "set_field_external.lo",
        """
        struct Counter {
            set value i32
        }

        def main() i32 {
            var c Counter
            c.value = 3
            ret c.value
        }
        """,
    )
    compiler.emit_ir(input_path).expect_ok()


def test_non_set_field_rejects_external_assignment(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "get_only_field_external_bad.lo",
        """
        struct Counter {
            value i32
        }

        def main() i32 {
            var c Counter
            c.value = 3
            ret c.value
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "assignment target contains read-only storage: i32 const",
        label="get-only field write diagnostic",
    )


def test_imported_non_set_field_rejects_external_assignment(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "import_get_only_field/dep.lo",
        """
        struct Point {
            x i32
        }
        """,
    )
    main_path = compiler.write_source(
        "import_get_only_field/main.lo",
        """
        import dep

        def main(score i32) i32 {
            var point = dep.Point(x = 1)
            point.x = score
            ret point.x
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "assignment target contains read-only storage: i32 const",
        label="imported get-only field write diagnostic",
    )


def test_conflicting_imported_extern_globals_report_semantic_error(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "extern_global_conflict/dep_a.lo",
        """
        #[extern]
        global errno i32
        """,
    )
    compiler.write_source(
        "extern_global_conflict/dep_b.lo",
        """
        #[extern]
        global errno i64
        """,
    )
    main_path = compiler.write_source(
        "extern_global_conflict/main.lo",
        """
        import dep_a
        import dep_b

        ret 0
        """,
    )

    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "conflicting declarations for global `errno`",
        label="extern global conflict diagnostic",
    )
    assert_not_contains(
        result.stderr,
        "internal error",
        label="extern global conflict category",
    )


def test_import_searches_repeated_include_paths_after_current_directory(
    compiler: CompilerHarness,
) -> None:
    first_include = compiler.tmp_path / "include_search" / "first"
    second_include = compiler.tmp_path / "include_search" / "second"
    compiler.write_source(
        "include_search/second/math.lo",
        """
        def answer() i32 {
            ret 42
        }
        """,
    )
    main_path = compiler.write_source(
        "include_search/app/main.lo",
        """
        import math

        def main() i32 {
            ret math.answer()
        }
        """,
    )
    ir = compiler.emit_ir(
        main_path, include_paths=[first_include, second_include]
    ).expect_ok().stdout
    assert_contains(ir, "define i32 @math.answer()", label="include path ir")
    assert_contains(ir, "store i32 42", label="include path ir")


def test_import_prefers_current_directory_over_include_paths(
    compiler: CompilerHarness,
) -> None:
    include_dir = compiler.tmp_path / "include_shadow" / "include"
    compiler.write_source(
        "include_shadow/include/util.lo",
        """
        def answer() i32 {
            ret 27
        }
        """,
    )
    compiler.write_source(
        "include_shadow/app/util.lo",
        """
        def answer() i32 {
            ret 13
        }
        """,
    )
    main_path = compiler.write_source(
        "include_shadow/app/main.lo",
        """
        import util

        def main() i32 {
            ret util.answer()
        }
        """,
    )
    ir = compiler.emit_ir(main_path, include_paths=[include_dir]).expect_ok().stdout
    assert_contains(ir, "store i32 13", label="local import precedence ir")


def test_imported_trait_supports_local_impl_static_dispatch(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "trait_import_local_impl/dep.lo",
        """
        trait Hash {
            def hash() i32
        }
        """,
    )
    main_path = compiler.write_source(
        "trait_import_local_impl/main.lo",
        """
        import dep

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: dep.Hash

        def main() i32 {
            var point = Point(value = 41)
            ret dep.Hash.hash(&point)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"call i32 @.*Point\.hash\(ptr ", label="imported trait local impl ir")
    assert_not_contains(ir, "trait namespaces can't be used", label="imported trait local impl ir")


def test_imported_trait_uses_imported_impl_for_static_dispatch(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "trait_import_dep_impl/dep.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 2
            }
        }

        impl Point: Hash

        def make() Point {
            ret Point(value = 40)
        }
        """,
    )
    main_path = compiler.write_source(
        "trait_import_dep_impl/main.lo",
        """
        import dep

        def main() i32 {
            var point = dep.make()
            ret dep.Hash.hash(&point)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"call i32 @dep\.Point\.hash\(ptr ", label="imported trait imported impl ir")
    assert_not_contains(ir, "store i32 27", label="local import precedence ir")


def test_imported_generic_applied_pointer_signatures_instantiate_concrete_symbol(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_applied_ptr/dep.lo",
        """
        struct Box[T] {
            value T
        }

        def take_box_ptr[T](value Box![T]*) Box![T]* {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_applied_ptr/main.lo",
        """
        import dep

        def main() i32 {
            var box dep.Box![i32]* = null
            var out dep.Box![i32]* = dep.take_box_ptr(box)
            ret 0
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(
        ir,
        "@dep.take_box_ptr__inst__i32",
        label="imported generic applied ptr ir",
    )
    assert len(re.findall(r"^define ptr @dep\.take_box_ptr__inst__i32\(ptr ", ir, re.M)) == 1


def test_imported_generic_applied_pointer_signatures_ignore_local_same_name_shadowing(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_applied_ptr_shadow/dep.lo",
        """
        struct Box[T] {
            value T
        }

        def take_box_ptr[T](value Box![T]*) Box![T]* {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_applied_ptr_shadow/main.lo",
        """
        import dep

        struct Box[U] {
            value U
        }

        def main() i32 {
            var box dep.Box![i32]* = null
            var out dep.Box![i32]* = dep.take_box_ptr(box)
            ret 0
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(
        ir,
        "@dep.take_box_ptr__inst__i32",
        label="imported generic applied ptr shadow ir",
    )
    assert len(re.findall(r"^define ptr @dep\.take_box_ptr__inst__i32\(ptr ", ir, re.M)) == 1


def test_imported_generic_signatures_use_owner_module_context_for_secondary_qualified_types(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_owner_context/helper.lo",
        """
        struct Box[T] {
            value T
        }
        """,
    )
    compiler.write_source(
        "generic_import_owner_context/dep.lo",
        """
        import helper

        struct Box[T] {
            value T
        }

        def make_helper_ptr() helper.Box![i32]* {
            ret null
        }

        def take_helper_ptr[T](value helper.Box![T]*) helper.Box![T]* {
            ret value
        }
        """,
    )
    inferred_path = compiler.write_source(
        "generic_import_owner_context/main.lo",
        """
        import dep

        def main() i32 {
            var out = dep.take_helper_ptr(dep.make_helper_ptr())
            ret 0
        }
        """,
    )
    inferred_ir = compiler.emit_ir(inferred_path).expect_ok().stdout
    assert_contains(
        inferred_ir,
        "@dep.take_helper_ptr__inst__i32",
        label="imported generic owner context ir",
    )

    explicit_path = compiler.write_source(
        "generic_import_owner_context/main_explicit.lo",
        """
        import dep

        def main() i32 {
            var out = dep.take_helper_ptr[i32](dep.make_helper_ptr())
            ret 0
        }
        """,
    )
    explicit_ir = compiler.emit_ir(explicit_path).expect_ok().stdout
    assert_contains(
        explicit_ir,
        "@dep.take_helper_ptr__inst__i32",
        label="imported generic owner context explicit ir",
    )
    assert len(re.findall(r"^define ptr @dep\.take_helper_ptr__inst__i32\(ptr ", explicit_ir, re.M)) == 1


def test_imported_generic_function_refs_instantiate_one_concrete_symbol(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_function_ref/dep.lo",
        """
        def id[T](value T) T {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_function_ref/main.lo",
        """
        import dep

        def main() i32 {
            var cb (i32: i32) = dep.id[i32]&<>
            ret cb(dep.id[i32](1))
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "@dep.id__inst__i32", label="imported generic ref ir")
    assert len(re.findall(r"^define i32 @dep\.id__inst__i32\(i32 ", ir, re.M)) == 1


def test_imported_generic_helper_results_do_not_hide_unconstrained_template_use(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_alias_escape/dep.lo",
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

        def head[T](box Box![T]) T {
            ret box.value
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_alias_escape/main.lo",
        """
        import dep

        def bad_value[T](obj T) i32 {
            var alias = dep.id(obj)
            ret alias.hash()
        }

        def bad_box[T](box dep.Box![T]) i32 {
            var alias = dep.head(box)
            ret alias.hash()
        }

        """,
    )

    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "unconstrained generic parameter `T` does not provide member `hash`",
        label="imported helper alias diagnostic",
    )
    assert_contains(
        result.stderr,
        "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
        label="imported helper alias hint",
    )


def test_imported_generic_method_results_do_not_hide_unconstrained_template_use(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_method_alias_escape/dep.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_method_alias_escape/main.lo",
        """
        import dep

        def bad_method[T](box dep.Box![T]) i32 {
            var alias = box.get()
            ret alias.hash()
        }
        """,
    )

    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "unconstrained generic parameter `T` does not provide member `hash`",
        label="imported method alias diagnostic",
    )
    assert_contains(
        result.stderr,
        "Unconstrained generic parameters only allow type-level uses such as `sizeof[T]()`",
        label="imported method alias hint",
    )


def test_imported_bounded_generic_functions_check_bounds_and_lower_trait_qualified_calls(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_bound_function/dep.lo",
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

        def make() Point {
            ret Point(value = 41)
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_bound_function/main.lo",
        """
        import dep

        def main() i32 {
            ret dep.hash_one(dep.make())
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(
        ir,
        r"@dep\.hash_one__inst__.*dep_2ePoint",
        label="imported generic bound function ir",
    )
    assert_contains(
        ir,
        "call i32 @dep.Point.hash(",
        label="imported generic bound function ir",
    )


def test_imported_generic_trait_impl_headers_enable_trait_qualified_calls_for_applied_receivers(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_trait_impl_header/dep.lo",
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

        struct Box[T] {
            value T

            def hash() i32 {
                ret 1
            }
        }

        impl[T Hash] Box[T]: Hash

        def make() Box![Point] {
            ret Box[Point](value = Point(value = 41))
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_trait_impl_header/main.lo",
        """
        import dep

        def main() i32 {
            var box dep.Box![dep.Point] =
                dep.Box[dep.Point](value = dep.Point(value = 41))
            ret dep.Hash.hash(&box)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(
        ir,
        r"define i32 @dep_2eBox_21_5b.*dep_2ePoint.*_5d\.hash\(ptr ",
        label="imported generic trait impl header ir",
    )
    assert_regex(
        ir,
        r"call i32 @dep_2eBox_21_5b.*dep_2ePoint.*_5d\.hash\(ptr ",
        label="imported generic trait impl header ir",
    )


def test_imported_generic_structs_materialize_by_value_layout_and_methods(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "generic_import_struct_runtime/dep.lo",
        """
        struct Box[T] {
            value T

            def get() T {
                ret self.value
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "generic_import_struct_runtime/main.lo",
        """
        import dep

        def main() i32 {
            var box dep.Box![i32] = dep.Box[i32](value = 7)
            var copy dep.Box![i32] = box
            ret copy.get()
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(
        ir,
        '%"dep.Box![i32]" = type { i32 }',
        label="imported generic struct runtime ir",
    )
    assert_contains(
        ir,
        "@dep_2eBox_21_5bi32_5d.get",
        label="imported generic struct runtime ir",
    )


def test_imported_trait_supports_local_impl_dynamic_dispatch(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "trait_dyn_import_local_impl/dep.lo",
        """
        trait Hash {
            def hash() i32
        }
        """,
    )
    main_path = compiler.write_source(
        "trait_dyn_import_local_impl/main.lo",
        """
        import dep

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: dep.Hash

        def main() i32 {
            var point = Point(value = 41)
            var h dep.Hash dyn = cast[dep.Hash dyn](&point)
            ret h.hash()
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "@__lona_trait_witness__", label="imported trait local dyn ir")
    assert_regex(
        ir,
        r"@__lona_trait_witness__.*\[ptr @.*Point\.hash\]",
        label="imported trait local dyn ir",
    )
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="imported trait local dyn ir",
    )


def test_imported_trait_uses_imported_impl_for_dynamic_dispatch(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "trait_dyn_import_dep_impl/dep.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 2
            }
        }

        impl Point: Hash

        def make() Point {
            ret Point(value = 40)
        }
        """,
    )
    main_path = compiler.write_source(
        "trait_dyn_import_dep_impl/main.lo",
        """
        import dep

        def main() i32 {
            var point dep.Point = dep.make()
            var h dep.Hash dyn = cast[dep.Hash dyn](&point)
            ret h.hash()
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(
        ir,
        r"@__lona_trait_witness__.*\[ptr @dep\.Point\.hash\]",
        label="imported trait imported dyn ir",
    )
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="imported trait imported dyn ir",
    )


def test_wrapper_trait_impl_on_imported_self_type_carries_methods_downstream(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "trait_import_wrapper_other/other.lo",
        """
        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 2
            }
        }

        def make() Point {
            ret Point(value = 40)
        }
        """,
    )
    compiler.write_source(
        "trait_import_wrapper_other/dep.lo",
        """
        import other

        trait Hash {
            def hash() i32
        }

        impl other.Point: Hash

        def make() other.Point {
            ret other.make()
        }
        """,
    )
    main_path = compiler.write_source(
        "trait_import_wrapper_other/main.lo",
        """
        import dep

        def main() i32 {
            var point = dep.make()
            var h dep.Hash dyn = cast[dep.Hash dyn](&point)
            ret dep.Hash.hash(&point) + h.hash() - 42
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(
        ir,
        r"call i32 @other\.Point\.hash\(ptr ",
        label="wrapper imported-self impl ir",
    )
    assert_regex(
        ir,
        r"@__lona_trait_witness__.*\[ptr @other\.Point\.hash\]",
        label="wrapper imported-self impl ir",
    )
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="wrapper imported-self impl ir",
    )


def test_imported_trait_readonly_dyn_signatures_work_across_module_boundaries(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "trait_dyn_import_readonly_signature/dep.lo",
        """
        trait Hash {
            def hash() i32
        }

        def forward(value Hash const dyn) Hash const dyn {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "trait_dyn_import_readonly_signature/main.lo",
        """
        import dep

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Point: dep.Hash

        def main() i32 {
            const point = Point(value = 41)
            var view dep.Hash const dyn = dep.forward(cast[dep.Hash dyn](&point))
            ret view.hash()
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "@__lona_trait_witness__", label="imported readonly dyn signature ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="imported readonly dyn signature ir",
    )


def test_import_supports_nested_module_paths_from_include_root(
    compiler: CompilerHarness,
) -> None:
    include_root = compiler.tmp_path / "include_nested" / "include"
    compiler.write_source(
        "include_nested/include/math/ops.lo",
        """
        def answer() i32 {
            ret 64
        }
        """,
    )
    main_path = compiler.write_source(
        "include_nested/app/main.lo",
        """
        import math/ops

        def main() i32 {
            ret ops.answer()
        }
        """,
    )
    ir = compiler.emit_ir(main_path, include_paths=[include_root]).expect_ok().stdout
    assert_contains(ir, "define i32 @ops.answer()", label="nested include path ir")
    assert_contains(ir, "store i32 64", label="nested include path ir")


def test_unreadable_include_path_reports_driver_error(
    compiler: CompilerHarness,
) -> None:
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return

    unreadable_dir = compiler.tmp_path / "include_unreadable" / "blocked"
    unreadable_dir.mkdir(parents=True, exist_ok=True)
    main_path = compiler.write_source(
        "include_unreadable/app/main.lo",
        """
        import dep

        def main() i32 {
            ret dep.answer()
        }
        """,
    )

    unreadable_dir.chmod(0)
    try:
        result = compiler.emit_ir(
            main_path, include_paths=[unreadable_dir]
        ).expect_failed()
    finally:
        unreadable_dir.chmod(0o755)

    assert_contains(
        result.stderr,
        "I couldn't inspect include path",
        label="unreadable include diagnostic",
    )
    assert_not_contains(
        result.stderr,
        "internal error:",
        label="unreadable include diagnostic",
    )


def test_non_set_method_cannot_write_even_set_fields(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "non_set_method_write_bad.lo",
        """
        struct Counter {
            set value i32

            def bump() i32 {
                self.value = self.value + 1
                ret self.value
            }
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "assignment target contains read-only storage: i32 const",
        label="non-set method write diagnostic",
    )


def test_get_only_field_projection_blocks_set_method_calls(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "get_only_field_method_bad.lo",
        """
        struct Counter {
            set value i32

            def read() i32 {
                ret self.value
            }

            set def bump() i32 {
                self.value = self.value + 1
                ret self.value
            }
        }

        struct Wrapper {
            counter Counter
        }

        def main() i32 {
            var w = Wrapper(counter = Counter(value = 4))
            ret w.counter.bump()
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "set method `bump` requires a writable receiver, got get_only_field_method_bad.Counter const",
        label="get-only field method diagnostic",
    )


def test_get_only_pointer_field_only_freezes_the_slot(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "get_only_pointer_field_slot.lo",
        """
        struct Box {
            ptr i32*
        }

        def main() i32 {
            var x i32 = 1
            var b = Box(ptr = &x)
            *b.ptr = 7
            ret x
        }
        """,
    )
    compiler.emit_ir(input_path).expect_ok()


def test_non_set_method_cannot_write_tuple_projection(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "non_set_method_tuple_write_bad.lo",
        """
        struct Box {
            set pair <i32, bool>

            def rewrite() i32 {
                self.pair._1 = 7
                ret self.pair._1
            }
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "assignment target contains read-only storage: i32 const",
        label="non-set tuple write diagnostic",
    )


def test_const_pointer_slots_stay_shallow_for_indexed_writes(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "const_pointer_slot_shallow.lo",
        """
        def main() i32 {
            var data i32[2] = {1, 2}
            const view = cast[i32[*]](&data(0))
            view(0) = 7
            ret data(0)
        }
        """,
    )
    compiler.emit_ir(input_path).expect_ok()


def test_get_only_tuple_field_projection_blocks_set_method_calls(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "get_only_tuple_method_bad.lo",
        """
        struct Counter {
            set value i32

            set def bump() i32 {
                self.value = self.value + 1
                ret self.value
            }
        }

        struct Box {
            pair <Counter, bool>
        }

        def main() i32 {
            var box = Box(pair = (Counter(value = 1), true))
            ret box.pair._1.bump()
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "set method `bump` requires a writable receiver, got get_only_tuple_method_bad.Counter const",
        label="get-only tuple method diagnostic",
    )


def test_top_level_mix_emits_language_entry(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "top_level_mix.lo",
        """
        def inc(a i32) i32 {
            ret a + 1
        }

        var x i32 = 3
        var y i32 = inc(x)
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_contains(ir, "define i32 @__lona_main__()", label="top level mix ir")
    assert_contains(ir, "@inc", label="top level mix ir")


def test_field_method_chain_uses_pointer_receiver(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "field_method_chain.lo",
        """
        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }
        }

        struct Wrapper {
            counter Counter
        }

        def main() i32 {
            var w = Wrapper(counter = Counter(value = 4))
            ret w.counter.read()
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"@.*Counter\.read", label="field method chain ir")
    assert_regex(ir, r"call i32 @.*Counter\.read\(ptr ", label="field method chain ir")


def test_import_links_functions_types_and_constructors(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "import/math.lo",
        """
        def inc(v i32) i32 {
            ret v + 1
        }

        def helper(v i32) i32 {
            ret inc(v)
        }

        struct Point {
            x i32
        }

        struct Box {
            point Point
        }
        """,
    )

    main_path = compiler.write_source(
        "import/main.lo",
        """
        import math

        def main() i32 {
            ret math.helper(4)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "define i32 @math.inc(i32", label="import function ir")
    assert_contains(ir, "define i32 @math.helper(i32", label="import function ir")
    assert_contains(ir, "call i32 @math.helper(i32 4)", label="import function ir")
    assert_not_contains(ir, "declare", label="import function ir")

    main_path = compiler.write_source(
        "import/main.lo",
        """
        import math

        def main() i32 {
            var p = math.Point(x = math.inc(4))
            ret p.x
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "%math.Point = type { i32 }", label="import type ir")
    assert_contains(ir, "call i32 @math.inc(i32 4)", label="import type ir")

    main_path = compiler.write_source(
        "import/main.lo",
        """
        import math

        def main() i32 {
            ret math.Box(point = math.Point(x = 4)).point.x
        }
        """,
    )
    compiler.emit_ir(main_path).expect_ok()


def test_imported_type_annotations_accept_dot_like_names(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "import_type_annotations/dep.lo",
        """
        struct Pair {
            left i32
            right i32
        }
        """,
    )
    main_path = compiler.write_source(
        "import_type_annotations/main.lo",
        """
        import dep

        def bounce(v dep.Pair) dep.Pair {
            ret v
        }

        def main() i32 {
            var pair dep.Pair = dep.Pair(left = 1, right = 2)
            ret bounce(pair).right
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "@bounce", label="imported type annotation ir")


def test_imported_type_name_works_in_sizeof(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "sizeof_import/dep.lo",
        """
        struct Pair {
            left i32
            right i32
        }
        """,
    )
    main_path = compiler.write_source(
        "sizeof_import/main.lo",
        """
        import dep

        def main() usize {
            var from_named_type usize = sizeof[dep.Pair]()
            var from_named_pointer_type usize = sizeof[dep.Pair*]()
            ret from_named_type + from_named_pointer_type
        }
        """,
    )
    ir = compiler.emit_ir(
        main_path, target="x86_64-unknown-linux-gnu"
    ).expect_ok().stdout
    assert_contains(ir, "store i64 8", label="import sizeof ir")


def test_imported_c_abi_symbols_keep_global_names(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "import_c_abi/dep.lo",
        """
        #[extern "C"]
        def abs(v i32) i32

        #[extern "C"]
        def c_inc(v i32) i32 {
            ret abs(v) + 1
        }

        def native_wrap(v i32) i32 {
            ret c_inc(v)
        }
        """,
    )
    main_path = compiler.write_source(
        "import_c_abi/main.lo",
        """
        import dep

        def main() i32 {
            ret dep.native_wrap(-4) + dep.c_inc(-2)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"^declare .*i32 @abs\(i32\)", label="import c abi ir")
    assert_contains(ir, "define i32 @c_inc(i32 ", label="import c abi ir")
    assert_contains(ir, "define i32 @dep.native_wrap(i32 ", label="import c abi ir")
    assert_contains(ir, "call i32 @dep.native_wrap(i32 -4)", label="import c abi ir")
    assert_contains(ir, "call i32 @c_inc(i32 -2)", label="import c abi ir")
    assert_contains(ir, "call i32 @c_inc(i32 %", label="import c abi ir")
    assert_contains(ir, "call i32 @abs(i32 %", label="import c abi ir")
    assert_not_contains(ir, "@dep.c_inc", label="import c abi ir")


def test_imported_c_repr_types_work_and_native_struct_pointer_is_rejected(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "import_c_repr/dep.lo",
        """
        struct FILE

        #[repr "C"]
        struct Point {
            x i32
            y i32
        }
        """,
    )
    main_path = compiler.write_source(
        "import_c_repr/main.lo",
        """
        import dep

        #[extern "C"]
        def shift(p dep.Point*, fp dep.FILE*) dep.Point*

        def main() i32 {
            ret 0
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"^declare .*ptr @shift\(ptr, ptr\)", label="import c repr ir")

    compiler.write_source(
        "import_c_repr/dep.lo",
        """
        struct Pair {
            left i32
            right i32
        }
        """,
    )
    main_path = compiler.write_source(
        "import_c_repr/main.lo",
        """
        import dep

        #[extern "C"]
        def bad(p dep.Pair*) i32
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        'semantic error: #[extern "C"] function `bad` uses unsupported parameter `p`: dep.Pair*',
        label="import native struct ptr diagnostic",
    )
    assert_contains(
        result.stderr,
        'help: Use pointers to scalars, pointers, opaque `struct` declarations, or `#[repr "C"] struct` types. Ordinary Lona structs cannot cross the C FFI boundary.',
        label="import native struct ptr diagnostic",
    )


def test_import_namespace_misuse_and_shadowing_are_rejected(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "import_shadow/math.lo",
        """
        def inc(v i32) i32 {
            ret v + 1
        }

        struct Point {
            x i32
        }
        """,
    )

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        def main() i32 {
            ret math(4)
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "module `math` does not support call syntax", label="module call diagnostic")
    assert_contains(
        result.stderr,
        "Call a concrete member like `math.func(...)` or `math.Type(...)` instead.",
        label="module call diagnostic",
    )

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        def main() i32 {
            ret math.missing(4)
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "unknown module member `math.missing`", label="unknown member diagnostic")

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        def main() i32 {
            var ns = math
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "module namespaces can't be used as runtime values", label="module namespace diagnostic")

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        def main() i32 {
            var cb = math.inc
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "unsupported bare function variable type for `cb`: (i32: i32)", label="function value diagnostic")

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        def main() i32 {
            var math i32 = 1
            ret math
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "local binding `math` conflicts with imported module alias `math`",
        label="local shadow diagnostic",
    )

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        def math() i32 {
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "top-level function `math` conflicts with imported module alias `math`",
        label="top-level shadow diagnostic",
    )

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        struct math {
            value i32
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "struct `math` conflicts with imported module alias `math`", label="type shadow diagnostic")

    main_path = compiler.write_source(
        "import_shadow/main.lo",
        """
        import math

        global math = 1
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "global `math` conflicts with imported module alias `math`",
        label="global shadow diagnostic",
    )


def test_imported_methods_and_aggregate_calls_lower_correctly(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "import_named_method/dep.lo",
        """
        struct Vec2 {
            x i32
            y i32

            def add(dx i32, dy i32) i32 {
                ret self.x + self.y + dx + dy
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "import_named_method/main.lo",
        """
        import dep

        def main() i32 {
            var v = dep.Vec2(x = 1, y = 2)
            ret v.add(dy = 4, dx = 3)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "define i32 @dep.Vec2.add(ptr ", label="named imported method ir")
    assert_contains(ir, "call i32 @dep.Vec2.add(", label="named imported method ir")

    compiler.write_source(
        "import_mutating_method/dep.lo",
        """
        struct Counter {
            value i32

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "import_mutating_method/main.lo",
        """
        import dep

        def main() i32 {
            var c = dep.Counter(value = 4)
            c.bump(3)
            ret c.value
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "define i32 @dep.Counter.bump(ptr ", label="mutating imported method ir")
    assert_contains(ir, "call i32 @dep.Counter.bump(ptr ", label="mutating imported method ir")
    assert_regex(
        ir,
        r"(?s)define i32 @dep\.Counter\.bump\(ptr %0, i32 %1\).*?getelementptr inbounds %dep\.Counter, ptr %\d+, i32 0, i32 0",
        label="mutating imported method ir",
    )

    compiler.write_source(
        "import_packed_aggregate/dep.lo",
        """
        struct Pair {
            left i32
            right i32

            def swap(extra i32) Pair {
                ret Pair(left = self.right + extra, right = self.left + extra)
            }
        }

        def echo(v Pair) Pair {
            ret v
        }
        """,
    )
    main_path = compiler.write_source(
        "import_packed_aggregate/main.lo",
        """
        import dep

        def main() i32 {
            var pair = dep.Pair(left = 1, right = 2)
            ret dep.echo(pair).swap(3).left
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"^define i64 @dep\.echo\(i64 [^)]+\)", label="packed aggregate ir")
    assert_regex(ir, r"^define i64 @dep\.Pair\.swap\(ptr [^,]+, i32 [^)]+\)", label="packed aggregate ir")
    assert_regex(ir, r"call i64 @dep\.echo\(i64 %", label="packed aggregate ir")
    assert_regex(ir, r"call i64 @dep\.Pair\.swap\(ptr [^,]+, i32 3\)", label="packed aggregate ir")

    compiler.write_source(
        "import_direct_return/dep.lo",
        """
        struct Triple {
            a i32
            b i32
            c i32

            def shift(delta i32) Triple {
                ret Triple(a = self.b + delta, b = self.c + delta,
                           c = self.a + delta)
            }
        }

        def echo(v Triple) Triple {
            ret v
        }
        """,
    )
    main_path = compiler.write_source(
        "import_direct_return/main.lo",
        """
        import dep

        def main() i32 {
            var triple = dep.Triple(a = 1, b = 2, c = 3)
            ret dep.echo(triple).shift(4).b
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"^%dep\.Triple = type \{ i32, i32, i32 \}", label="direct return aggregate ir")
    assert_regex(ir, r"^define %dep\.Triple @dep\.echo\(ptr [^)]+\)", label="direct return aggregate ir")
    assert_regex(
        ir,
        r"^define %dep\.Triple @dep\.Triple\.shift\(ptr [^,]+, i32 [^)]+\)",
        label="direct return aggregate ir",
    )
    assert_regex(ir, r"call %dep\.Triple @dep\.echo\(ptr %", label="direct return aggregate ir")
    assert_regex(ir, r"call %dep\.Triple @dep\.Triple\.shift\(ptr [^,]+, i32 4\)", label="direct return aggregate ir")
    assert_not_contains(ir, "sret", label="direct return aggregate ir")


def test_transitive_imports_do_not_leak(compiler: CompilerHarness) -> None:
    compiler.write_source(
        "import_transitive/leaf.lo",
        """
        global count = 9

        def inc(v i32) i32 {
            ret v + 1
        }

        struct Point {
            x i32
        }
        """,
    )
    compiler.write_source(
        "import_transitive/mid.lo",
        """
        import leaf

        def call_leaf(v i32) i32 {
            ret leaf.inc(v)
        }
        """,
    )
    main_path = compiler.write_source(
        "import_transitive/main.lo",
        """
        import mid

        def main() i32 {
            ret mid.call_leaf(4)
        }
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "call i32 @mid.call_leaf(i32 4)", label="transitive import ir")

    main_path = compiler.write_source(
        "import_transitive/main.lo",
        """
        import mid

        def main() i32 {
            ret leaf.inc(4)
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "semantic error: undefined identifier `leaf`", label="transitive function access diagnostic")

    main_path = compiler.write_source(
        "import_transitive/main.lo",
        """
        import mid

        def main() i32 {
            var p leaf.Point
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(result.stderr, "semantic error: unknown variable type", label="transitive type access diagnostic")
    assert_contains(result.stderr, "var p leaf.Point", label="transitive type access diagnostic")

    main_path = compiler.write_source(
        "import_transitive/main.lo",
        """
        import mid

        def main() i32 {
            ret leaf.count
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "semantic error: undefined identifier `leaf`",
        label="transitive global access diagnostic",
    )


def test_imported_module_top_level_execution_emits_init_chain_and_runs(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "import_exec/dep.lo",
        """
        global count = 0

        count = count + 5
        """,
    )
    main_path = compiler.write_source(
        "import_exec/main.lo",
        """
        import dep

        ret dep.count
        """,
    )
    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_regex(ir, r"define i32 @__.*dep.*_init_entry__\(\)", label="imported executable ir")
    assert_regex(ir, r"call i32 @__.*dep.*_init_entry__\(\)", label="imported executable ir")

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="import_exec.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(5)


def test_imported_module_init_runs_once_across_diamond_dependencies(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "module_init_diamond/leaf.lo",
        """
        global counter = 0

        counter = counter + 1
        """,
    )
    compiler.write_source(
        "module_init_diamond/left.lo",
        """
        import leaf

        def read() i32 {
            ret leaf.counter
        }
        """,
    )
    compiler.write_source(
        "module_init_diamond/right.lo",
        """
        import leaf

        def read() i32 {
            ret leaf.counter
        }
        """,
    )
    main_path = compiler.write_source(
        "module_init_diamond/main.lo",
        """
        import left
        import right

        ret left.read() + right.read()
        """,
    )

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="module_init_diamond.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(2)


def test_imported_module_init_failure_is_propagated_to_root(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "module_init_failure/dep.lo",
        """
        ret 7
        """,
    )
    main_path = compiler.write_source(
        "module_init_failure/main.lo",
        """
        import dep

        ret 0
        """,
    )

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="module_init_failure.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


def test_import_only_root_module_still_emits_language_entry_and_runs_init(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "module_import_only_root/dep.lo",
        """
        ret 7
        """,
    )
    main_path = compiler.write_source(
        "module_import_only_root/main.lo",
        """
        import dep
        """,
    )

    ir = compiler.emit_ir(main_path).expect_ok().stdout
    assert_contains(ir, "define i32 @__lona_main__()", label="import-only root ir")

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="module_import_only_root.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


def test_import_only_mid_module_still_propagates_dependency_init_failure(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "module_import_only_mid/leaf.lo",
        """
        ret 9
        """,
    )
    compiler.write_source(
        "module_import_only_mid/mid.lo",
        """
        import leaf
        """,
    )
    main_path = compiler.write_source(
        "module_import_only_mid/main.lo",
        """
        import mid
        """,
    )

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="module_import_only_mid.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(9)


def test_module_init_respects_direct_import_order(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "module_init_order/leaf.lo",
        """
        global trace = 0
        """,
    )
    compiler.write_source(
        "module_init_order/first.lo",
        """
        import leaf

        leaf.trace = leaf.trace * 10 + 1
        """,
    )
    compiler.write_source(
        "module_init_order/second.lo",
        """
        import leaf

        leaf.trace = leaf.trace * 10 + 2
        """,
    )
    main_path = compiler.write_source(
        "module_init_order/main.lo",
        """
        import first
        import second
        import leaf

        ret leaf.trace
        """,
    )

    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="module_init_order.bin"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(12)


def test_imported_module_name_conflicts_are_rejected(
    compiler: CompilerHarness,
) -> None:

    compiler.write_source(
        "import_conflict/conflict_dep.lo",
        """
        struct Counter {
            value i32
        }

        def Counter(value i32) i32 {
            ret value
        }
        """,
    )
    main_path = compiler.write_source(
        "import_conflict/main.lo",
        """
        import conflict_dep

        def main() i32 {
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "top-level function `Counter` conflicts with struct `Counter`",
        label="imported conflict diagnostic",
    )


def test_large_struct_returns_and_grammar_subset_stay_lowerable(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "large_struct_return.lo",
        """
        struct Big {
            a i32
            b i32
            c i32
            d i32
            e i32

            def add(v i32) Big {
                ret Big(a = self.a + v, b = self.b + v, c = self.c + v,
                        d = self.d + v, e = self.e + v)
            }
        }

        def make_big(v i32) Big {
            var base = Big(v, v + 1, v + 2, v + 3, v + 4)
            ret base.add(1)
        }

        var sample = make_big(3)
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^%.*Big = type \{ i32, i32, i32, i32, i32 \}", label="large struct ir")
    assert_regex(ir, r"^define void @.*Big\.add\(ptr ", label="large struct ir")
    assert_contains(ir, "define void @make_big(ptr ", label="large struct ir")
    assert_regex(ir, r"call void @.*Big\.add\(ptr ", label="large struct ir")

    input_path = compiler.write_source(
        "grammar_subset.lo",
        """
        struct Name {
            a i32
            b i32
        }

        def make_name(a i32, b i32) Name {
            ret Name(a = a, b = b)
        }

        var sample = make_name(1, 2)
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^%.*Name = type \{ i32, i32 \}", label="grammar subset ir")
    assert_regex(ir, r"^define i64 @make_name\(i32 [^,]+, i32 [^)]+\)", label="grammar subset ir")
