from __future__ import annotations

from tests.harness import assert_contains
from tests.harness.compiler import CompilerHarness


def test_embedded_field_is_marked_in_frontend_json(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_json.lo",
        """
        struct Inner {
            value i32
        }

        struct Outer {
            _ Inner
        }
        """,
    )
    result = compiler.emit_json(input_path).expect_ok()
    assert_contains(result.stdout, '"embeddedField": true', label="embedding json")


def test_embedded_field_constructor_and_explicit_access_work(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_explicit.lo",
        """
        struct Inner {
            value i32
        }

        struct Outer {
            _ Inner
            value i32
        }

        def run() i32 {
            var outer = Outer(Inner = Inner(value = 2), value = 5)
            ret outer.value + outer.Inner.value
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_explicit"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


def test_module_qualified_embedded_type_uses_final_type_name_for_constructor(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "embedding_qualified/dep.lo",
        """
        struct Inner {
            value i32
        }
        """,
    )
    main_path = compiler.write_source(
        "embedding_qualified/main.lo",
        """
        import dep

        struct Outer {
            _ dep.Inner
        }

        def run() i32 {
            var outer = Outer(Inner = dep.Inner(value = 4))
            ret outer.Inner.value + outer.value
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="embedding_qualified.exe"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(8)


def test_embedded_access_name_can_match_enclosing_struct_name(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "embedding_same_name/dep.lo",
        """
        struct Outer {
            value i32

            def read() i32 {
                ret self.value
            }
        }
        """,
    )
    main_path = compiler.write_source(
        "embedding_same_name/main.lo",
        """
        import dep

        struct Outer {
            _ dep.Outer
        }

        def run() i32 {
            var outer = Outer(Outer = dep.Outer(value = 4))
            ret outer.value + outer.Outer.read()
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="embedding_same_name.exe"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(8)


def test_promoted_field_and_method_work_inside_set_method(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_promoted_internal.lo",
        """
        struct Inner {
            set value i32

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }

        struct Outer {
            _ Inner

            set def bump_twice() i32 {
                self.value = self.value + 1
                self.bump(2)
                ret self.value
            }
        }

        ret Outer(Inner = Inner(value = 3)).bump_twice()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_promoted_internal"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(6)


def test_promoted_field_supports_ref_and_address_of_inside_set_method(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_promoted_ref.lo",
        """
        struct Inner {
            set value i32
        }

        struct Outer {
            _ Inner

            set def poke() i32 {
                ref alias i32 = self.value
                alias = alias + 4
                var p i32* = &self.value
                *p = *p + 1
                ret self.value
            }
        }

        ret Outer(Inner = Inner(value = 2)).poke()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_promoted_ref"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)


def test_direct_method_shadows_promoted_method_but_explicit_path_still_works(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_method_shadow.lo",
        """
        struct Inner {
            def read() i32 {
                ret 1
            }
        }

        struct Outer {
            _ Inner

            def read() i32 {
                ret 5
            }
        }

        def run() i32 {
            var outer = Outer(Inner = Inner())
            ret outer.read() + outer.Inner.read()
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_method_shadow"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(6)


def test_promoted_non_set_method_reads_through_embedded_field(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_promoted_read.lo",
        """
        struct Inner {
            value i32

            def read() i32 {
                ret self.value
            }
        }

        struct Outer {
            _ Inner
        }

        def run() i32 {
            var outer = Outer(Inner = Inner(value = 5))
            ret outer.read() + outer.Inner.read()
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_promoted_read"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(10)


def test_multi_level_promoted_lookup_works(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_multilevel.lo",
        """
        struct A {
            value i32
        }

        struct B {
            _ A
        }

        struct C {
            _ B
        }

        def run() i32 {
            var c = C(B = B(A = A(value = 6)))
            ret c.value + c.B.A.value
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_multilevel"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(12)
 

def test_shallower_promoted_candidate_wins_over_deeper_candidate(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_depth_priority.lo",
        """
        struct A {
            value i32
        }

        struct B {
            _ A
        }

        struct C {
            _ A
            _ B
        }

        def run() i32 {
            var c = C(A = A(value = 3), B = B(A = A(value = 8)))
            ret c.value + c.B.A.value
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_depth_priority"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(11)


def test_positional_constructor_treats_embedded_field_as_single_slot(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_positional_ctor.lo",
        """
        struct Inner {
            value i32
        }

        struct Outer {
            _ Inner
            tail i32
        }

        def run() i32 {
            var outer = Outer(Inner(value = 4), 6)
            ret outer.value + outer.tail
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_positional_ctor"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(10)


def test_promoted_member_is_not_a_constructor_field_name(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_ctor_promoted_bad.lo",
        """
        struct Inner {
            value i32
        }

        struct Outer {
            _ Inner
        }

        def main() i32 {
            var outer = Outer(value = 1)
            ret outer.value
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "unknown field `value` for constructor `",
        label="promoted constructor field diagnostic",
    )


def test_external_promoted_set_method_requires_writable_embedded_receiver(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_promoted_set_bad.lo",
        """
        struct Inner {
            set value i32

            set def bump() i32 {
                self.value = self.value + 1
                ret self.value
            }
        }

        struct Outer {
            _ Inner
        }

        def main() i32 {
            var outer = Outer(Inner = Inner(value = 1))
            ret outer.bump()
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "set method `bump` requires a writable receiver",
        label="promoted set method diagnostic",
    )


def test_external_promoted_field_write_reuses_explicit_path_read_only_rule(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_promoted_write_bad.lo",
        """
        struct Inner {
            set value i32
        }

        struct Outer {
            _ Inner
        }

        def main() i32 {
            var outer = Outer(Inner = Inner(value = 1))
            outer.value = 2
            ret outer.value
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "assignment target contains read-only storage: i32 const",
        label="promoted field write diagnostic",
    )


def test_explicit_embedded_path_write_uses_same_read_only_rule(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_explicit_write_bad.lo",
        """
        struct Inner {
            set value i32
        }

        struct Outer {
            _ Inner
        }

        def main() i32 {
            var outer = Outer(Inner = Inner(value = 1))
            outer.Inner.value = 2
            ret outer.Inner.value
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "assignment target contains read-only storage: i32 const",
        label="explicit embedded field write diagnostic",
    )


def test_external_ref_can_bind_const_view_of_explicit_embedded_field(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_explicit_ref_const.lo",
        """
        struct Inner {
            value i32

            def read() i32 {
                ret self.value
            }
        }

        struct Outer {
            _ Inner
        }

        def run() i32 {
            var outer = Outer(Inner = Inner(value = 4))
            ref alias Inner const = outer.Inner
            ret alias.read()
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="embedding_explicit_ref_const"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(4)


def test_ambiguous_promoted_member_reports_explicit_paths(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_ambiguous.lo",
        """
        struct A {
            x i32
        }

        struct B {
            x i32
        }

        struct C {
            _ A
            _ B
        }

        def main() i32 {
            var c = C(A = A(x = 1), B = B(x = 2))
            ret c.x
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "ambiguous promoted member `x`",
        label="ambiguous promoted member diagnostic",
    )
    assert_contains(result.stderr, "c.A.x", label="ambiguous promoted member diagnostic")
    assert_contains(result.stderr, "c.B.x", label="ambiguous promoted member diagnostic")


def test_ambiguous_promoted_method_reports_explicit_paths(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_ambiguous_method.lo",
        """
        struct A {
            def read() i32 {
                ret 1
            }
        }

        struct B {
            def read() i32 {
                ret 2
            }
        }

        struct C {
            _ A
            _ B
        }

        def main() i32 {
            var c = C(A = A(), B = B())
            ret c.read()
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "ambiguous promoted member `read`",
        label="ambiguous promoted method diagnostic",
    )
    assert_contains(
        result.stderr, "c.A.read", label="ambiguous promoted method diagnostic"
    )
    assert_contains(
        result.stderr, "c.B.read", label="ambiguous promoted method diagnostic"
    )


def test_imported_embedded_members_promote_across_modules(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "embedding_imported/dep.lo",
        """
        struct Inner {
            value i32

            def read() i32 {
                ret self.value
            }
        }

        struct Outer {
            _ Inner
        }
        """,
    )
    main_path = compiler.write_source(
        "embedding_imported/main.lo",
        """
        import dep

        def run() i32 {
            var outer = dep.Outer(Inner = dep.Inner(value = 7))
            ret outer.value + outer.read()
        }

        ret run()
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        main_path, output_name="embedding_imported.exe"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(14)


def test_regular_field_name_cannot_conflict_with_embedded_access_name(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "embedding_name_conflict_bad.lo",
        """
        struct Inner {
            value i32
        }

        struct Outer {
            _ Inner
            Inner i32
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "struct `Outer` field `Inner` conflicts with an existing member",
        label="embedded access name conflict diagnostic",
    )


def test_duplicate_embedded_access_names_from_qualified_types_are_rejected(
    compiler: CompilerHarness,
) -> None:
    compiler.write_source(
        "embedding_duplicate_access/a.lo",
        """
        struct Inner {
            value i32
        }
        """,
    )
    compiler.write_source(
        "embedding_duplicate_access/b.lo",
        """
        struct Inner {
            value i32
        }
        """,
    )
    main_path = compiler.write_source(
        "embedding_duplicate_access/main.lo",
        """
        import a
        import b

        struct Outer {
            _ a.Inner
            _ b.Inner
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        "embedded field access name `Inner` conflicts with an existing member",
        label="duplicate embedded access name diagnostic",
    )


def test_embedded_field_rejects_non_struct_type(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "embedding_non_struct_bad.lo",
        """
        struct Outer {
            _ i32
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "embedded field `_ i32` must use a struct type",
        label="embedded non-struct diagnostic",
    )


def test_embedded_field_rejects_set_modifier(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "embedding_set_bad.lo",
        """
        struct Inner {
            value i32
        }

        struct Outer {
            set _ Inner
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "embedded field `_ Inner` cannot use `set`",
        label="embedded set diagnostic",
    )
