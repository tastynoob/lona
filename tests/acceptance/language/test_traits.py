from __future__ import annotations

from tests.acceptance.language._syntax_helpers import (
    _emit_ir,
    _emit_json,
    _expect_ir_failure,
)
from tests.harness import assert_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_trait_surface_json_includes_trait_impl_and_dyn_type_nodes(
    compiler: CompilerHarness,
) -> None:
    json_out = _emit_json(
        compiler,
        "trait_surface_json.lo",
        """
        trait Hash {
            def hash() u64
            set def rewrite(value i32)
        }

        struct Point {
            value i32
        }

        impl Hash for Point {
            def hash() u64 {
                ret self.hash()
            }
            set def rewrite(value i32) {
                self.rewrite(value)
            }
        }

        def show(ptr Point*) {
            var h Hash dyn = cast[Hash dyn](ptr)
        }
        """,
    )
    for needle in [
        '"type": "TraitDecl"',
        '"type": "TraitImplDecl"',
        '"selfType": "Point"',
        '"targetType": "Hash dyn"',
        '"declaredType": "Hash dyn"',
        '"receiverAccess": "set"',
    ]:
        assert_contains(json_out, needle, label="trait surface json")


def test_trait_impl_body_syntax_does_not_break_plain_ir_lowering(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_impl_body_only.lo",
        """
        trait Hash {
            def hash() u64
        }

        struct Point {
            value i32

            def hash() u64 {
                ret cast[u64](self.value)
            }
        }

        impl Hash for Point {
            def hash() u64 {
                ret self.hash()
            }
        }

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait impl body ir")


def test_trait_v0_impl_for_body_supports_member_static_and_dyn_calls(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_impl_for_body.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.value + 1
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var view Hash dyn = cast[Hash dyn](&point)
            ret point.hash() + Hash.hash(&point) + view.hash() - 126
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait impl-for ir")
    assert_regex(
        ir,
        r"call i32 @.*Point\.__trait__\..*Hash\.hash\(ptr ",
        label="trait impl-for ir",
    )
    assert_contains(ir, "@__lona_trait_witness__", label="trait impl-for ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait impl-for ir",
    )


def test_trait_v0_static_qualified_calls_and_impl_validation(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_static_dispatch.lo",
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

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var ptr Point* = &point
            ret Hash.hash(&point) + Hash.hash(ptr) - 42
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait static dispatch ir")
    assert_regex(
        ir,
        r"call i32 @.*Point\.hash\(ptr ",
        label="trait static dispatch ir",
    )

    failures = [
        (
            "trait_static_receiver_pointer_required.lo",
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

            impl Hash for Point {
                def hash() i32 {
                    ret self.hash()
                }
            }

            def main() i32 {
                var point = Point(value = 41)
                ret Hash.hash(point)
            }
            """,
            [
                "trait-qualified receiver must be passed as an explicit self pointer",
                "Write `Trait.method(&value, ...)`",
            ],
        ),
        (
            "trait_impl_missing_method.lo",
            """
            trait Hash {
                def hash() i32
            }

            struct Point {
                value i32
            }

            impl Hash for Point {
            }

            def main() i32 {
                ret 0
            }
            """,
            ["is missing method `hash`"],
        ),
        (
            "trait_impl_receiver_mismatch.lo",
            """
            trait Hash {
                set def hash() i32
            }

            struct Point {
                value i32

                def hash() i32 {
                    ret self.value
                }
            }

            impl Hash for Point {
                def hash() i32 {
                    ret self.value
                }
            }

            def main() i32 {
                ret 0
            }
            """,
            ["receiver access mismatch for `hash`"],
        ),
        (
            "trait_impl_param_type_mismatch.lo",
            """
            trait Hash {
                def hash(value i32) i32
            }

            struct Point {
                value i32

                def hash(value i64) i32 {
                    ret self.value + cast[i32](value)
                }
            }

            impl Hash for Point {
                def hash(value i64) i32 {
                    ret self.value + cast[i32](value)
                }
            }

            def main() i32 {
                ret 0
            }
            """,
            ["parameter type mismatch for `hash` at index 0"],
        ),
        (
            "trait_impl_duplicate_visible.lo",
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

            impl Hash for Point {
                def hash() i32 {
                    ret self.value
                }
            }
            impl Hash for Point {
                def hash() i32 {
                    ret self.value
                }
            }

            def main() i32 {
                ret 0
            }
            """,
            ["duplicate visible impl for trait"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_trait_v0_static_getters_accept_const_self_pointers(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_static_const_getter.lo",
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

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            const point = Point(value = 41)
            var ptr Point const* = &point
            ret Hash.hash(&point) + Hash.hash(ptr) - 84
        }
        """,
    )
    assert_regex(ir, r"call i32 @.*Point\.hash\(ptr ", label="trait static const getter ir")
    assert_contains(ir, "define i32 @main()", label="trait static const getter ir")


def test_trait_v0_allows_same_module_struct_types_in_trait_signatures(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_same_module_struct_signature.lo",
        """
        struct Big {
            value i32
        }

        trait Factory {
            def make() Big
            def score(item Big) i32
        }

        struct Maker {
            seed i32

            def make() Big {
                ret Big(value = self.seed + 1)
            }

            def score(item Big) i32 {
                ret item.value + self.seed
            }
        }

        impl Factory for Maker {
            def make() Big {
                ret self.make()
            }
            def score(item Big) i32 {
                ret self.score(item)
            }
        }

        def main() i32 {
            var maker = Maker(seed = 20)
            var item = Factory.make(&maker)
            ret Factory.score(&maker, item)
        }
        """,
    )
    assert_regex(ir, r"%.*Big = type \{ i32 \}", label="trait same-module struct ir")
    assert_regex(ir, r"call i32 @.*Maker\.score\(ptr ", label="trait same-module struct ir")


def test_trait_v0_qualified_calls_bind_named_args_from_trait_signatures(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_named_args.lo",
        """
        trait Add {
            def add(x i32) i32
        }

        struct Point {
            value i32

            def add(y i32) i32 {
                ret self.value + y
            }
        }

        impl Add for Point {
            def add(x i32) i32 {
                ret self.value + x
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            ret Add.add(&point, x = 1)
        }
        """,
    )
    assert_regex(
        ir,
        r"call i32 @.*Point\.__trait__\..*Add\.add\(ptr [^,]+, i32 1\)",
        label="trait named args ir",
    )


def test_trait_v0_supports_multiple_traits_with_same_method_name(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_same_method_name_multi_impl.lo",
        """
        trait Hash {
            def hash() i32
        }

        trait Metric {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }
        impl Metric for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var h Hash dyn = cast[Hash dyn](&point)
            var m Metric dyn = cast[Metric dyn](&point)
            ret Hash.hash(&point) + Metric.hash(&point) + h.hash() + m.hash() - 168
        }
        """,
    )
    assert_contains(ir, "@__lona_trait_witness__", label="trait same method name ir")
    assert_regex(ir, r"call i32 @.*Point\.hash\(ptr ", label="trait same method name ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait same method name ir",
    )

    _expect_ir_failure(
        compiler,
        "trait_same_method_name_different_signatures_bad.lo",
        """
        trait Hash {
            def hash() i32
        }

        trait Metric {
            def hash(step i32) i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.value + 1
            }
        }
        impl Metric for Point {
            def hash() i32 {
                ret self.value + 2
            }
        }

        def main() i32 {
            ret 0
        }
        """,
        [
            "parameter count mismatch for `hash`",
        ],
    )


def test_trait_v0_trait_impl_bodies_support_same_name_with_explicit_receiver_paths(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_impl_body_same_method_name.lo",
        """
        trait Hash {
            def read() i32
        }

        trait Metric {
            def read() i32
        }

        struct Point {
            value i32
        }

        impl Hash for Point {
            def read() i32 {
                ret self.value + 1
            }
        }

        impl Metric for Point {
            def read() i32 {
                ret self.value + 2
            }
        }

        def main() i32 {
            var point = Point(value = 40)
            var h Hash dyn = cast[Hash dyn](&point)
            var m Metric dyn = cast[Metric dyn](&point)
            ret Hash.read(&point) + Metric.read(&point) +
                point.Hash.read() + point.Metric.read() +
                h.read() + m.read() - 249
        }
        """,
    )
    assert_contains(ir, "@__lona_trait_witness__", label="trait impl body same-name ir")
    assert_contains(ir, "__trait__", label="trait impl body same-name ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait impl body same-name ir",
    )

    _expect_ir_failure(
        compiler,
        "trait_impl_body_same_method_name_ambiguous.lo",
        """
        trait Hash {
            def read() i32
        }

        trait Metric {
            def read() i32
        }

        struct Point {
            value i32
        }

        impl Hash for Point {
            def read() i32 {
                ret self.value + 1
            }
        }

        impl Metric for Point {
            def read() i32 {
                ret self.value + 2
            }
        }

        def main() i32 {
            var point = Point(value = 40)
            ret point.read()
        }
        """,
        [
            "ambiguous trait method `read`",
            "point.Hash.read(...)",
            "point.Metric.read(...)",
        ],
    )


def test_trait_v0_inherent_methods_still_win_over_trait_impl_body_methods(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_impl_body_inherent_precedence.lo",
        """
        trait Hash {
            def read() i32
        }

        struct Point {
            value i32

            def read() i32 {
                ret self.value + 100
            }
        }

        impl Hash for Point {
            def read() i32 {
                ret self.value + 1
            }
        }

        def main() i32 {
            var point = Point(value = 40)
            ret point.read() + point.Hash.read() + Hash.read(&point) - 222
        }
        """,
    )
    assert_contains(ir, "__trait__", label="trait inherent precedence ir")
    assert_regex(
        ir,
        r"call i32 @.*Point\.read\(ptr ",
        label="trait inherent precedence ir",
    )


def test_trait_v0_explicit_receiver_trait_paths_cover_pointer_and_mixed_impl_forms(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_receiver_path_mixed_impls.lo",
        """
        trait Hash {
            def read() i32
        }

        trait Metric {
            def read() i32
        }

        struct Point {
            value i32

            def read() i32 {
                ret self.value + 1
            }
        }

        impl Hash for Point {
            def read() i32 {
                ret self.read()
            }
        }

        impl Metric for Point {
            def read() i32 {
                ret self.value + 2
            }
        }

        def main() i32 {
            var point = Point(value = 40)
            var ptr Point* = &point
            var h Hash dyn = cast[Hash dyn](&point)
            var m Metric dyn = cast[Metric dyn](&point)
            ret ptr.Hash.read() + ptr.Metric.read() +
                Hash.read(&point) + Metric.read(&point) +
                h.read() + m.read() - 249
        }
        """,
    )
    assert_contains(ir, "@__lona_trait_witness__", label="trait receiver path mixed impls ir")
    assert_contains(ir, "__trait__", label="trait receiver path mixed impls ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait receiver path mixed impls ir",
    )


def test_trait_v0_explicit_receiver_trait_path_does_not_shadow_real_members(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_receiver_path_member_shadowing.lo",
        """
        trait Hash {
            def read() i32
        }

        struct View {
            value i32

            def read() i32 {
                ret self.value + 1
            }
        }

        struct Holder {
            Hash View
        }

        struct Point {
            value i32
        }

        impl Hash for Point {
            def read() i32 {
                ret self.value + 2
            }
        }

        def main() i32 {
            var holder = Holder(Hash = View(value = 30))
            var point = Point(value = 40)
            ret holder.Hash.read() + point.Hash.read() - 73
        }
        """,
    )
    assert_regex(
        ir,
        r"call i32 @.*View\.read\(ptr ",
        label="trait receiver path member shadowing ir",
    )
    assert_regex(
        ir,
        r"call i32 @.*Point\.__trait__\..*Hash\.read\(ptr ",
        label="trait receiver path member shadowing ir",
    )


def test_trait_v0_explicit_receiver_trait_path_reports_unknown_methods_and_writable_setters(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "trait_receiver_path_unknown_method_bad.lo",
            """
            trait Hash {
                def read() i32
            }

            struct Point {
                value i32
            }

            impl Hash for Point {
                def read() i32 {
                    ret self.value + 1
                }
            }

            def main() i32 {
                var point = Point(value = 40)
                ret point.Hash.missing()
            }
            """,
            [
                "unknown trait method `Hash.missing`",
            ],
        ),
        (
            "trait_receiver_path_setter_const_bad.lo",
            """
            trait CounterLike {
                set def bump(step i32) i32
            }

            struct Counter {
                value i32
            }

            impl CounterLike for Counter {
                set def bump(step i32) i32 {
                    self.value = self.value + step
                    ret self.value
                }
            }

            def main() i32 {
                const counter = Counter(value = 40)
                ret counter.CounterLike.bump(1)
            }
            """,
            [
                "set trait method `bump` requires a writable receiver",
                "Setter trait calls through `value.Trait.method(...)` require a writable receiver",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_trait_v0_allows_local_trait_dyn_fields_before_trait_declaration(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_local_dyn_field.lo",
        """
        struct Holder {
            hash Hash dyn
        }

        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value
            }
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var hash Hash dyn = cast[Hash dyn](&point)
            var holder = Holder(hash = hash)
            ret holder.hash.hash()
        }
        """,
    )
    assert_regex(ir, r"%.*Holder = type \{ \{ ptr, ptr \} \}", label="trait local dyn field ir")
    assert_contains(ir, "@__lona_trait_witness__", label="trait local dyn field ir")


def test_trait_v0_allows_self_and_forward_local_trait_dyn_signatures(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_local_dyn_signatures.lo",
        """
        trait Hash {
            def merge(other Hash dyn) i32
        }

        trait UseLater {
            def connect(other Later dyn) i32
        }

        trait Later {
            def hash() i32
        }

        struct Point {
            value i32

            def merge(other Hash dyn) i32 {
                ret self.value + 1
            }

            def connect(other Later dyn) i32 {
                ret other.hash()
            }

            def hash() i32 {
                ret self.value
            }
        }

        impl Hash for Point {
            def merge(other Hash dyn) i32 {
                ret self.merge(other)
            }
        }
        impl UseLater for Point {
            def connect(other Later dyn) i32 {
                ret self.connect(other)
            }
        }
        impl Later for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def main() i32 {
            var point = Point(value = 41)
            var hash Hash dyn = cast[Hash dyn](&point)
            var later Later dyn = cast[Later dyn](&point)
            ret Hash.merge(&point, hash) + UseLater.connect(&point, later)
        }
        """,
    )
    assert_contains(ir, "@__lona_trait_witness__", label="trait local dyn signatures ir")
    assert_regex(ir, r"call i32 @.*Point\.merge\(ptr ", label="trait local dyn signatures ir")
    assert_contains(ir, "call i32 %trait.slot(ptr %trait.data)", label="trait local dyn signatures ir")


def test_trait_v0_dyn_objects_support_casts_calls_and_signature_positions(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_dyn_dispatch.lo",
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

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def forward(value Hash dyn) Hash dyn {
            ret value
        }

        def invoke(value Hash dyn) i32 {
            ret value.hash()
        }

        def main() i32 {
            var point = Point(value = 41)
            var h Hash dyn = cast[Hash dyn](&point)
            ret invoke(forward(h))
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait dyn dispatch ir")
    assert_contains(ir, "@__lona_trait_witness__", label="trait dyn dispatch ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait dyn dispatch ir",
    )

    failures = [
        (
            "trait_dyn_non_addressable_bad.lo",
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

            impl Hash for Point {
                def hash() i32 {
                    ret self.hash()
                }
            }

            def main() i32 {
                var h Hash dyn = cast[Hash dyn](&Point(value = 41))
                ret 0
            }
            """,
            [
                "trait object construction expects an addressable source",
                "Temporaries cannot become `Trait dyn`.",
            ],
        ),
        (
            "trait_dyn_missing_impl_bad.lo",
            """
            trait Hash {
                def hash() i32
            }

            struct Point {
                value i32
            }

            def main() i32 {
                var point = Point(value = 41)
                var h Hash dyn = cast[Hash dyn](&point)
                ret 0
            }
            """,
            [
                "does not implement trait",
                "before constructing `",
                "Hash dyn`",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_trait_v0_accepts_pointer_backed_trait_object_sources(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_dyn_pointer_backed_sources.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }

            set def borrow_hash() Hash dyn {
                ret cast[Hash dyn](&self)
            }
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def invoke(value Hash dyn) i32 {
            ret value.hash()
        }

        def main() i32 {
            var point = Point(value = 41)
            var ptr Point* = &point
            var from_ptr Hash dyn = cast[Hash dyn](&ptr)
            var from_self Hash dyn = point.borrow_hash()
            ret invoke(from_ptr) + invoke(from_self) - 42
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait dyn pointer-backed ir")
    assert_contains(ir, "@__lona_trait_witness__", label="trait dyn pointer-backed ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait dyn pointer-backed ir",
    )


def test_trait_v0_dyn_mutability_tracks_readonly_and_writable_receivers(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_dyn_mutability.lo",
        """
        trait CounterLike {
            def read() i32
            set def bump(step i32) i32
        }

        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }

        impl CounterLike for Counter {
            def read() i32 {
                ret self.read()
            }
            set def bump(step i32) i32 {
                ret self.bump(step)
            }
        }

        def read_value(view CounterLike const dyn) i32 {
            ret view.read()
        }

        def main() i32 {
            var counter = Counter(value = 40)
            var writable CounterLike dyn = cast[CounterLike dyn](&counter)
            const frozen = Counter(value = 7)
            const readonly CounterLike const dyn = cast[CounterLike dyn](&frozen)
            ret writable.bump(2) + read_value(readonly)
        }
        """,
    )
    assert_contains(ir, "@__lona_trait_witness__", label="trait dyn mutability ir")
    assert_regex(
        ir,
        r"call i32 %trait\.slot\(ptr %trait\.data, i32 2\)",
        label="trait dyn mutability ir",
    )
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait dyn mutability ir",
    )

    failures = [
        (
            "trait_dyn_readonly_setter_bad.lo",
            """
            trait CounterLike {
                def read() i32
                set def bump(step i32) i32
            }

            struct Counter {
                value i32

                def read() i32 {
                    ret self.value
                }

                set def bump(step i32) i32 {
                    self.value = self.value + step
                    ret self.value
                }
            }

            impl CounterLike for Counter {
                def read() i32 {
                    ret self.read()
                }
                set def bump(step i32) i32 {
                    ret self.bump(step)
                }
            }

            def main() i32 {
                const counter = Counter(value = 41)
                const view CounterLike const dyn = cast[CounterLike dyn](&counter)
                ret view.bump(1)
            }
            """,
            [
                "set trait method `bump` requires a writable receiver",
                "Read-only trait objects can only call get-only methods",
            ],
        ),
        (
            "trait_static_setter_const_receiver_bad.lo",
            """
            trait CounterLike {
                set def bump(step i32) i32
            }

            struct Counter {
                value i32

                set def bump(step i32) i32 {
                    self.value = self.value + step
                    ret self.value
                }
            }

            impl CounterLike for Counter {
                set def bump(step i32) i32 {
                    ret self.bump(step)
                }
            }

            def main() i32 {
                const counter = Counter(value = 41)
                ret CounterLike.bump(&counter, 1)
            }
            """,
            [
                "set trait method `bump` requires a writable receiver",
                "Static trait setter calls require a writable self pointer",
            ],
        ),
        (
            "trait_dyn_const_source_setter_bad.lo",
            """
            trait CounterLike {
                set def bump(step i32) i32
            }

            struct Counter {
                value i32

                set def bump(step i32) i32 {
                    self.value = self.value + step
                    ret self.value
                }
            }

            impl CounterLike for Counter {
                set def bump(step i32) i32 {
                    ret self.bump(step)
                }
            }

            def invoke(value Counter const) i32 {
                ret cast[CounterLike dyn](&value).bump(1)
            }

            def main() i32 {
                var counter = Counter(value = 41)
                ret invoke(counter)
            }
            """,
            [
                "set trait method `bump` requires a writable receiver",
                "Read-only trait objects can only call get-only methods",
            ],
        ),
        (
            "trait_dyn_const_source_into_writable_binding_bad.lo",
            """
            trait CounterLike {
                def read() i32
            }

            struct Counter {
                value i32

                def read() i32 {
                    ret self.value
                }
            }

            impl CounterLike for Counter {
                def read() i32 {
                    ret self.read()
                }
            }

            def main() i32 {
                const counter = Counter(value = 41)
                var view CounterLike dyn = cast[CounterLike dyn](&counter)
                ret view.read()
            }
            """,
            [
                "writable `trait_dyn_const_source_into_writable_binding_bad.CounterLike dyn` cannot receive a read-only trait object",
                "borrowed from a const receiver",
            ],
        ),
        (
            "trait_dyn_const_source_into_writable_param_bad.lo",
            """
            trait CounterLike {
                def read() i32
            }

            struct Counter {
                value i32

                def read() i32 {
                    ret self.value
                }
            }

            impl CounterLike for Counter {
                def read() i32 {
                    ret self.read()
                }
            }

            def invoke(view CounterLike dyn) i32 {
                ret view.read()
            }

            def main() i32 {
                const counter = Counter(value = 41)
                ret invoke(cast[CounterLike dyn](&counter))
            }
            """,
            [
                "writable `trait_dyn_const_source_into_writable_param_bad.CounterLike dyn` cannot receive a read-only trait object",
                "borrowed from a const receiver",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)

    readonly_ir = _emit_ir(
        compiler,
        "trait_dyn_readonly_copy.lo",
        """
        trait CounterLike {
            def read() i32
        }

        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }
        }

        impl CounterLike for Counter {
            def read() i32 {
                ret self.read()
            }
        }

        def main() i32 {
            const frozen = Counter(value = 41)
            var a = cast[CounterLike dyn](&frozen)
            var b = a
            ret b.read()
        }
        """,
    )
    assert_contains(readonly_ir, "@__lona_trait_witness__", label="trait dyn readonly copy ir")

    _expect_ir_failure(
        compiler,
        "trait_dyn_readonly_copy_setter_bad.lo",
        """
        trait CounterLike {
            def read() i32
            set def bump(step i32) i32
        }

        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }

        impl CounterLike for Counter {
            def read() i32 {
                ret self.read()
            }
            set def bump(step i32) i32 {
                ret self.bump(step)
            }
        }

        def main() i32 {
            const frozen = Counter(value = 41)
            var a = cast[CounterLike dyn](&frozen)
            var b = a
            ret b.bump(1)
        }
        """,
        [
            "set trait method `bump` requires a writable receiver",
            "Read-only trait objects can only call get-only methods",
        ],
    )

    _expect_ir_failure(
        compiler,
        "trait_dyn_const_legacy_order_bad.lo",
        """
        trait CounterLike {
            def read() i32
        }

        def main(view CounterLike dyn const) i32 {
            ret view.read()
        }
        """,
        [
            "read-only trait objects use `Trait const dyn`, not `Trait dyn const`",
            "Write `Hash const dyn` instead of `Hash dyn const`.",
        ],
    )

    _expect_ir_failure(
        compiler,
        "trait_dyn_const_source_into_writable_return_bad.lo",
        """
        trait CounterLike {
            def read() i32
        }

        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }
        }

        impl CounterLike for Counter {
            def read() i32 {
                ret self.read()
            }
        }

        def freeze() CounterLike dyn {
            const counter = Counter(value = 41)
            ret cast[CounterLike dyn](&counter)
        }

        def main() i32 {
            ret freeze().read()
        }
        """,
        [
            "writable `trait_dyn_const_source_into_writable_return_bad.CounterLike dyn` cannot receive a read-only trait object",
            "borrowed from a const receiver",
        ],
    )


def test_trait_v0_explicit_readonly_dyn_casts_work_in_params_returns_and_fields(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_explicit_readonly_dyn.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Holder {
            view Hash const dyn
        }

        struct Point {
            value i32

            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def freeze(value Hash const dyn) Hash const dyn {
            ret value
        }

        def main() i32 {
            var point = Point(value = 41)
            var view Hash const dyn = cast[Hash const dyn](&point)
            var holder = Holder(view = view)
            ret freeze(holder.view).hash()
        }
        """,
    )
    assert_regex(ir, r"%.*Holder = type \{ \{ ptr, ptr \} \}", label="trait explicit readonly dyn ir")
    assert_contains(ir, "@__lona_trait_witness__", label="trait explicit readonly dyn ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait explicit readonly dyn ir",
    )


def test_trait_v0_readonly_trait_object_pointers_support_getters_and_reject_setters(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_readonly_dyn_pointer_getter.lo",
        """
        trait CounterLike {
            def read() i32
            set def bump(step i32) i32
        }

        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }

        impl CounterLike for Counter {
            def read() i32 {
                ret self.read()
            }
            set def bump(step i32) i32 {
                ret self.bump(step)
            }
        }

        def invoke(ptr CounterLike const dyn*) i32 {
            ret ptr.read()
        }

        def main() i32 {
            const frozen = Counter(value = 41)
            var view CounterLike const dyn = cast[CounterLike dyn](&frozen)
            var ptr CounterLike const dyn* = &view
            ret invoke(ptr)
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait readonly dyn ptr getter ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait readonly dyn ptr getter ir",
    )

    _expect_ir_failure(
        compiler,
        "trait_readonly_dyn_pointer_setter_bad.lo",
        """
        trait CounterLike {
            def read() i32
            set def bump(step i32) i32
        }

        struct Counter {
            value i32

            def read() i32 {
                ret self.value
            }

            set def bump(step i32) i32 {
                self.value = self.value + step
                ret self.value
            }
        }

        impl CounterLike for Counter {
            def read() i32 {
                ret self.read()
            }
            set def bump(step i32) i32 {
                ret self.bump(step)
            }
        }

        def invoke(ptr CounterLike const dyn*) i32 {
            ret ptr.bump(1)
        }

        def main() i32 {
            const frozen = Counter(value = 41)
            var view CounterLike const dyn = cast[CounterLike dyn](&frozen)
            var ptr CounterLike const dyn* = &view
            ret invoke(ptr)
        }
        """,
        [
            "set trait method `bump` requires a writable receiver",
            "Read-only trait objects can only call get-only methods",
        ],
    )


def test_trait_v0_trait_object_pointers_reuse_implicit_deref_for_calls(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_dyn_pointer_call.lo",
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

        impl Hash for Point {
            def hash() i32 {
                ret self.hash()
            }
        }

        def invoke(ptr Hash dyn*) i32 {
            ret ptr.hash()
        }

        def main() i32 {
            var point = Point(value = 41)
            var h Hash dyn = cast[Hash dyn](&point)
            var ptr Hash dyn* = &h
            ret invoke(ptr)
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait dyn ptr call ir")
    assert_contains(
        ir,
        "call i32 %trait.slot(ptr %trait.data)",
        label="trait dyn ptr call ir",
    )


def test_trait_v0_reports_targeted_diagnostics_for_unsupported_bodies_and_fields(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "trait_field_member_bad.lo",
            """
            trait Hash {
                value i32
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "trait `Hash` cannot declare field `value`",
                "Trait v0 only allows method signatures inside trait bodies.",
            ],
        ),
        (
            "trait_method_body_bad.lo",
            """
            trait Hash {
                def hash() i32 {
                    ret 1
                }
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "trait method `hash` cannot have a body in trait v0",
                "Keep only the method signature inside the trait.",
            ],
        ),
        (
            "trait_local_var_bad.lo",
            """
            trait Hash {
                var tmp i32 = 0
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "trait `Hash` cannot declare local variable `tmp`",
                "Trait bodies describe interfaces only.",
            ],
        ),
        (
            "trait_global_bad.lo",
            """
            trait Hash {
                global tmp i32 = 0
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "trait `Hash` cannot declare global `tmp`",
                "Move globals to module scope.",
            ],
        ),
        (
            "trait_ret_bad.lo",
            """
            trait Hash {
                ret 0
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "trait `Hash` cannot contain executable statements",
                "Trait v0 only allows method signatures inside trait bodies.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_trait_impl_for_body_supports_applied_self_types(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_impl_body_applied_self.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Box[T] {
            value T
        }

        impl Hash for Box[i32] {
            def hash() i32 {
                ret self.value + 1
            }
        }

        def main() i32 {
            var box = Box[i32](value = 41)
            var view Hash dyn = cast[Hash dyn](&box)
            ret box.hash() + Hash.hash(&box) + view.hash() - 126
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait applied self ir")
    assert_contains(
        ir,
        "@__lona_trait_witness__",
        label="trait applied self ir",
    )


def test_trait_impl_for_body_supports_generic_self_types(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_impl_body_generic_self.lo",
        """
        trait Hash {
            def hash() i32
        }

        struct Num {
            value i32
        }

        impl Hash for Num {
            def hash() i32 {
                ret self.value + 1
            }
        }

        struct Box[T] {
            value T
        }

        impl[T Hash] Hash for Box[T] {
            def hash() i32 {
                ret Hash.hash(&self.value) + 1
            }
        }

        def main() i32 {
            var box = Box[Num](value = Num(value = 40))
            var view Hash dyn = cast[Hash dyn](&box)
            ret box.hash() + Hash.hash(&box) + view.hash() - 126
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait generic self ir")
    assert_contains(
        ir,
        "@__lona_trait_witness__",
        label="trait generic self ir",
    )


def test_plain_dot_lookup_keeps_generic_trait_impl_methods_ambiguous(
    compiler: CompilerHarness,
) -> None:
    _expect_ir_failure(
        compiler,
        "trait_impl_body_generic_self_ambiguous_plain_dot.lo",
        """
        trait Hash {
            def hash() i32
        }

        trait Metric {
            def hash() i32
        }

        struct Num {
            value i32
        }

        impl Hash for Num {
            def hash() i32 {
                ret self.value + 1
            }
        }

        impl Metric for Num {
            def hash() i32 {
                ret self.value + 2
            }
        }

        struct Box[T] {
            value T
        }

        impl[T Hash] Hash for Box[T] {
            def hash() i32 {
                ret Hash.hash(&self.value)
            }
        }

        impl[T Metric] Metric for Box[T] {
            def hash() i32 {
                ret Metric.hash(&self.value)
            }
        }

        def main() i32 {
            var box = Box[Num](value = Num(value = 40))
            ret box.hash()
        }
        """,
        [
            "ambiguous trait method `hash`",
            "box.Hash.hash(...)",
            "box.Metric.hash(...)",
        ],
    )
