from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_json_milestone_nodes_and_legacy_cast_rejection(compiler: CompilerHarness) -> None:
    json_out = _emit_json(
        compiler,
        "milestone_json.lo",
        """
        struct Complex {
            real i32
            img i32
        }

        def mix(x i32, y i32) i32 {
            ret x + y
        }

        def show() {
            var pair <i32, bool> = (1, true)
            var matrix i32[4][5] = {1, 2, 3, 4}
            var c = Complex(real = 1, img = 2)
            var p = math.Point(x = 1)
            ret mix(y = c.img, x = c.real)
        }
        """,
    )
    for needle in [
        '"declaredType": "<i32, bool>"',
        '"type": "TupleLiteral"',
        '"declaredType": "i32[4][5]"',
        '"type": "BraceInit"',
        '"type": "FieldCall"',
        '"type": "NamedCallArg"',
        '"kind": "positional"',
        '"name": "math"',
    ]:
        assert_contains(json_out, needle, label="milestone json")

    _expect_ir_failure(
        compiler,
        "legacy_cast.lo",
        """
        def bad() i32 {
            var x i32 = i32 1
            ret x
        }
        """,
        ["syntax error: I couldn't parse this statement:"],
    )

def test_multiline_parentheses_calls_and_operator_continuations(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "multiline_layout.lo",
        """
        def add(
            a i32,
            b i32
        ) i32 {
            ret a +
                b
        }

        def ping(
        ) i32 {
            ret 1
        }

        def main() i32 {
            var grouped i32 =
                (
                    1 +
                    2
                )
            var total i32 = add(
                grouped,
                ping(
                )
            )
            ret total
        }
        """,
    )
    assert_contains(ir, "define i32 @add(i32", label="multiline layout ir")
    assert_contains(ir, "define i32 @ping()", label="multiline layout ir")
    assert_contains(ir, "define i32 @main()", label="multiline layout ir")

def test_multiline_bracket_and_tuple_type_layouts(compiler: CompilerHarness) -> None:
    ir = _emit_ir(
        compiler,
        "multiline_types.lo",
        """
        def make_pair() <
            i32,
            bool
        > {
            ret (
                1,
                true
            )
        }

        def main() usize {
            var data i32[
                2
            ] = {}
            var pair <
                i32,
                bool
            > = make_pair()
            var size usize = sizeof[
                i32[
                    2
                ]
            ]()
            ret size + cast[usize](pair._1) + cast[usize](data(0))
        }
        """,
    )
    assert_contains(ir, "define i64 @make_pair()", label="multiline type ir")
    assert_contains(ir, "alloca { i32, i8 }", label="multiline type ir")
    assert_contains(ir, "define i64 @main()", label="multiline type ir")

def test_controlflow_braces_must_stay_on_same_line_as_if_and_else(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "if_brace_next_line.lo",
            """
            def bad(flag bool) i32 {
                if flag
                {
                    ret 1
                }
                ret 0
            }
            """,
            ["syntax error:"],
        ),
        (
            "else_brace_next_line.lo",
            """
            def bad(flag bool) i32 {
                if flag {
                    ret 1
                }
                else
                {
                    ret 2
                }
                ret 0
            }
            """,
            ["syntax error:"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)

def test_unary_operators_cannot_continue_onto_next_line(
    compiler: CompilerHarness,
) -> None:
    failures = [
        (
            "unary_minus_next_line.lo",
            """
            def bad() i32 {
                ret -
                    1
            }
            """,
            ["syntax error:"],
        ),
        (
            "unary_deref_next_line.lo",
            """
            def bad(ptr i32*) i32 {
                ret *
                    ptr
            }
            """,
            ["syntax error:"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


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

        impl Point: Hash

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


def test_trait_header_only_syntax_does_not_break_plain_ir_lowering(
    compiler: CompilerHarness,
) -> None:
    ir = _emit_ir(
        compiler,
        "trait_header_only.lo",
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

        impl Point: Hash

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(ir, "define i32 @main()", label="trait header ir")


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

        impl Point: Hash

        def main() i32 {
            var point = Point(value = 41)
            ret Hash.hash(point)
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
            "trait_impl_missing_method.lo",
            """
            trait Hash {
                def hash() i32
            }

            struct Point {
                value i32
            }

            impl Point: Hash

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

            impl Point: Hash

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

            impl Point: Hash

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

            impl Point: Hash
            impl Point: Hash

            def main() i32 {
                ret 0
            }
            """,
            ["duplicate visible impl for trait"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


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

        impl Maker: Factory

        def main() i32 {
            var maker = Maker(seed = 20)
            var item = Factory.make(maker)
            ret Factory.score(maker, item)
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

        impl Point: Add

        def main() i32 {
            var point = Point(value = 41)
            ret Add.add(point, x = 1)
        }
        """,
    )
    assert_regex(ir, r"call i32 @.*Point\.add\(ptr [^,]+, i32 1\)", label="trait named args ir")


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

        impl Point: Hash

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

        impl Point: Hash
        impl Point: UseLater
        impl Point: Later

        def main() i32 {
            var point = Point(value = 41)
            var hash Hash dyn = cast[Hash dyn](&point)
            var later Later dyn = cast[Later dyn](&point)
            ret Hash.merge(point, hash) + UseLater.connect(point, later)
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

        impl Point: Hash

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

            impl Point: Hash

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
        (
            "trait_dyn_set_receiver_bad.lo",
            """
            trait Hash {
                set def hash() i32
            }

            struct Point {
                value i32

                set def hash() i32 {
                    self.value = self.value + 1
                    ret self.value
                }
            }

            impl Point: Hash

            def main() i32 {
                var point = Point(value = 41)
                var h Hash dyn = cast[Hash dyn](&point)
                ret 0
            }
            """,
            [
                "is not dyn-compatible in trait v0",
                "Dynamic trait objects currently support get-only methods only",
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

        impl Point: Hash

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
            "trait_impl_body_bad.lo",
            """
            trait Hash {
                def hash() i32
            }

            struct Point {
                value i32
            }

            impl Point: Hash {
            }

            def main() i32 {
                ret 0
            }
            """,
            [
                "trait impl bodies are not supported in trait v0",
                "Declare only `impl Type: Trait` headers for now.",
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

    failures = [
        (
            "cast_pointer_nested_const_bad.lo",
            """
            def main() i32 {
                var bytes u8 const[2] = {1, 2}
                var view u8 const[*] = &bytes(0)
                var slot u8 const[*]* = &view
                var bad u8[*][*] = cast[u8[*][*]](slot)
                ret bad(0)(0)
            }
            """,
            ["unsupported builtin cast from `u8 const[*]*` to `u8[*][*]`"],
        ),
        (
            "cast_bad.lo",
            """
            def main() i32 {
                var value i32 = 1
                var raw u8* = cast[u8*](value)
                ret 0
            }
            """,
            ["unsupported builtin cast from `i32` to `u8*`"],
        ),
        (
            "cast_tuple_bad.lo",
            """
            def main() i32 {
                var pair <i32, i32> = (1, 2)
                var copy <i32, i32> = cast[<i32, i32>](pair)
                ret copy._1
            }
            """,
            [
                "unsupported builtin cast from `<i32, i32>` to `<i32, i32>`",
                "Builtin cast only supports builtin scalar and pointer types.",
            ],
        ),
        (
            "cast_struct_bad.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            def main() i32 {
                var pair = Pair(left = 1, right = 2)
                var copy Pair = cast[Pair](pair)
                ret copy.left
            }
            """,
            [
                "unsupported builtin cast from `",
                "Pair` to `",
                "Builtin cast only supports builtin scalar and pointer types.",
            ],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
