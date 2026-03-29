from __future__ import annotations

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


def test_imported_root_execution_and_name_conflicts_are_rejected(compiler: CompilerHarness) -> None:
    compiler.write_source("import_exec/bad_dep.lo", "var x i32 = 1\n")
    main_path = compiler.write_source(
        "import_exec/main.lo",
        """
        import bad_dep

        def main() i32 {
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(main_path).expect_failed()
    assert_contains(
        result.stderr,
        f"imported file `{main_path.parent / 'bad_dep.lo'}` cannot contain top-level executable statements",
        label="imported executable diagnostic",
    )
    assert_contains(
        result.stderr,
        "help: Move this statement into a function, or keep top-level execution only in the root file.",
        label="imported executable diagnostic",
    )

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
