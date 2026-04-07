from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_tuples_small_aggregates_and_method_abi_shapes(compiler: CompilerHarness) -> None:
    tuple_ir = _emit_ir(
        compiler,
        "tuple.lo",
        """
        def bad() i32 {
            var pair <i32, bool> = (1, true)
            ret 0
        }
        """,
    )
    for needle in ["alloca { i32, i8 }", "store { i32, i8 } { i32 1, i8 1 }"]:
        assert_contains(tuple_ir, needle, label="tuple ir")

    tuple_flow_ir = _emit_ir(
        compiler,
        "tuple_flow.lo",
        """
        def echo(pair <i32, bool>) <i32, bool> {
            ret pair
        }

        def main() i32 {
            var pair <i32, bool> = (1, true)
            var out <i32, bool> = echo(pair)
            ret 0
        }
        """,
    )
    assert_regex(tuple_flow_ir, r"^define i64 @echo\(i64 [^)]+\)", label="tuple flow ir")
    assert_regex(tuple_flow_ir, r"call i64 @echo\(i64 %", label="tuple flow ir")

    small_struct_ir = _emit_ir(
        compiler,
        "small_struct_packed.lo",
        """
        struct Pair {
            left i32
            right i32
        }

        def echo(v Pair) Pair {
            ret v
        }

        def main() i32 {
            var pair = Pair(left = 1, right = 2)
            ret echo(pair).right
        }
        """,
    )
    assert_regex(small_struct_ir, r"^define i64 @echo\(i64 [^)]+\)", label="small struct packed ir")
    assert_regex(small_struct_ir, r"call i64 @echo\(i64 %", label="small struct packed ir")
    assert_not_contains(small_struct_ir, "llvm.memcpy", label="small struct packed ir")

    medium_struct_ir = _emit_ir(
        compiler,
        "medium_struct_direct_return.lo",
        """
        struct Triple {
            a i32
            b i32
            c i32
        }

        def echo(v Triple) Triple {
            ret v
        }

        def main() i32 {
            var triple = Triple(a = 1, b = 2, c = 3)
            ret echo(triple).c
        }
        """,
    )
    assert_regex(medium_struct_ir, r"^%.*Triple = type \{ i32, i32, i32 \}", label="medium struct direct return ir")
    assert_regex(medium_struct_ir, r"^define %.*Triple @echo\(ptr [^)]+\)", label="medium struct direct return ir")
    assert_regex(medium_struct_ir, r"call %.*Triple @echo\(ptr %", label="medium struct direct return ir")
    assert_not_contains(medium_struct_ir, "sret", label="medium struct direct return ir")

    small_array_ir = _emit_ir(
        compiler,
        "small_array_packed.lo",
        """
        def echo(v i32[2]) i32[2] {
            ret v
        }

        def main() i32 {
            var row i32[2] = {1, 2}
            ret echo(row)(1)
        }
        """,
    )
    assert_regex(small_array_ir, r"^define i64 @echo\(i64 [^)]+\)", label="small array packed ir")
    assert_regex(small_array_ir, r"call i64 @echo\(i64 %", label="small array packed ir")

    method_abi_ir = _emit_ir(
        compiler,
        "method_abi.lo",
        """
        struct Pair {
            left i32
            right i32

            def swap(extra i32) Pair {
                ret Pair(left = self.right + extra, right = self.left + extra)
            }
        }

        def main() i32 {
            var pair = Pair(left = 1, right = 2)
            var out = pair.swap(3)
            ret out.left
        }
        """,
    )
    assert_regex(method_abi_ir, r"^define i64 @.*Pair\.swap\(ptr [^,]+, i32 [^)]+\)", label="method abi ir")
    assert_regex(method_abi_ir, r"call i64 @.*Pair\.swap\(ptr [^,]+, i32 3\)", label="method abi ir")

    method_direct_return_ir = _emit_ir(
        compiler,
        "method_direct_return.lo",
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

        def main() i32 {
            var triple = Triple(a = 1, b = 2, c = 3)
            ret triple.shift(4).b
        }
        """,
    )
    assert_regex(method_direct_return_ir, r"^%.*Triple = type \{ i32, i32, i32 \}", label="method direct return ir")
    assert_regex(method_direct_return_ir, r"^define %.*Triple @.*Triple\.shift\(ptr [^,]+, i32 [^)]+\)", label="method direct return ir")
    assert_regex(method_direct_return_ir, r"call %.*Triple @.*Triple\.shift\(ptr [^,]+, i32 4\)", label="method direct return ir")
    assert_not_contains(method_direct_return_ir, "sret", label="method direct return ir")

    tuple_field_ir = _emit_ir(
        compiler,
        "tuple_field.lo",
        """
        def echo(pair <i32, bool>) <i32, bool> {
            ret pair
        }

        def main() i32 {
            var pair <i32, bool> = (1, true)
            pair._1 = 7
            if echo(pair)._2 {
                ret echo(pair)._1
            }
            ret 0
        }
        """,
    )
    for needle in ["getelementptr inbounds { i32, i8 }", "load i8, ptr", "load i32, ptr", "store i32 7"]:
        assert_contains(tuple_field_ir, needle, label="tuple field ir")

    tuple_no_context_ir = _emit_ir(
        compiler,
        "tuple_no_context.lo",
        """
        def main() i32 {
            var pair = (1, true)
            if pair._2 {
                ret pair._1
            }
            ret 0
        }
        """,
    )
    for needle in ["alloca { i32, i8 }", "store { i32, i8 } { i32 1, i8 1 }"]:
        assert_contains(tuple_no_context_ir, needle, label="tuple no context ir")

    tuple_array_ir = _emit_ir(
        compiler,
        "tuple_array.lo",
        """
        def main() i32 {
            var items <i32, bool>[2] = {(1, true), (2, false)}
            if items(0)._2 {
                ret items(0)._1
            }
            ret items(1)._1
        }
        """,
    )
    for needle in ["alloca [2 x { i32, i8 }]", "store [2 x { i32, i8 }]"]:
        assert_contains(tuple_array_ir, needle, label="tuple array ir")

    _expect_ir_failure(
        compiler,
        "tuple_field_bad.lo",
        """
        def main() i32 {
            var pair <i32, bool> = (1, true)
            ret pair._3
        }
        """,
        ["unknown tuple field `_3`", "Tuple fields are named `_1`, `_2` in declaration order."],
    )


def test_struct_fields_allow_forward_local_types_but_reject_by_value_cycles(
    compiler: CompilerHarness,
) -> None:
    input_path = compiler.write_source(
        "forward_struct_field.lo",
        """
        struct Outer {
            inner Inner
        }

        struct Inner {
            value i32
        }

        def make() Outer {
            ret Outer(inner = Inner(value = 7))
        }

        ret make().inner.value
        """,
    )
    build_result, exe_path = compiler.build_system_executable(
        input_path, output_name="forward_struct_field"
    )
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(7)

    _expect_ir_failure(
        compiler,
        "recursive_struct_cycle.lo",
        """
        struct Node {
            next Node
        }

        ret 0
        """,
        [
            "struct `Node` forms a recursive by-value cycle through field `next`",
            "Use pointers for recursive links instead of embedding the recursive struct by value.",
        ],
    )


def test_array_forms_views_and_array_diagnostics(compiler: CompilerHarness) -> None:
    success_cases = [
        (
            "array_init.lo",
            """
            def bad() i32 {
                var matrix i32[4][5] = {{1}, {2}}
                matrix(1)(2) = 7
                ret matrix(1)(2)
            }
            """,
            [
                ("alloca [5 x [4 x i32]]", False),
                ("getelementptr inbounds [5 x [4 x i32]]", False),
                ("[4 x i32] [i32 1, i32 0, i32 0, i32 0]", False),
                ("[4 x i32] [i32 2, i32 0, i32 0, i32 0]", False),
                ("store i32 7", False),
                ("ret i32", False),
            ],
        ),
        (
            "array_group.lo",
            """
            def bad() i32 {
                var matrix i32[5, 4] = {{1, 2}, {3}}
                matrix(1, 2) = 9
                ret matrix(1, 2)
            }
            """,
            [
                ("alloca [5 x [4 x i32]]", False),
                ("getelementptr inbounds [5 x [4 x i32]]", False),
                ("[4 x i32] [i32 1, i32 2, i32 0, i32 0]", False),
                ("[4 x i32] [i32 3, i32 0, i32 0, i32 0]", False),
                ("store i32 9", False),
            ],
        ),
        (
            "array_mixed.lo",
            """
            def bad() i32 {
                var tensor i32[3][4, 5] = {}
                tensor(1, 2)(0) = 11
                ret tensor(1, 2)(0)
            }
            """,
            [("alloca [4 x [5 x [3 x i32]]]", False), ("getelementptr inbounds [4 x [5 x [3 x i32]]]", False), ("store i32 11", False)],
        ),
        (
            "array_value_init.lo",
            """
            def main() i32 {
                var row i32[4] = {1, 2}
                ret row(0) + row(1) + row(2) + row(3)
            }
            """,
            [("store [4 x i32] [i32 1, i32 2, i32 0, i32 0]", False)],
        ),
        (
            "array_value_init_trailing_comma_inline.lo",
            """
            def main() i32 {
                var row i32[4] = {1, 2,}
                ret row(0) + row(1) + row(2) + row(3)
            }
            """,
            [("store [4 x i32] [i32 1, i32 2, i32 0, i32 0]", False)],
        ),
        (
            "array_value_init_trailing_comma_multiline.lo",
            """
            def main() i32 {
                var row i32[4] = {
                    1,
                    2,
                }
                ret row(0) + row(1) + row(2) + row(3)
            }
            """,
            [("store [4 x i32] [i32 1, i32 2, i32 0, i32 0]", False)],
        ),
        (
            "array_value_init_multiline_legacy.lo",
            """
            def main() i32 {
                var row i32[4] = {
                    1
                    2
                }
                ret row(0) + row(1) + row(2) + row(3)
            }
            """,
            [("store [4 x i32] [i32 1, i32 2, i32 0, i32 0]", False)],
        ),
        (
            "array_infer.lo",
            """
            def main() i32 {
                var row = {1, 2}
                ret row(0) + row(1)
            }
            """,
            [("alloca [2 x i32]", False), ("store [2 x i32] [i32 1, i32 2]", False)],
        ),
        (
            "array_infer_nested.lo",
            """
            def main() i32 {
                var matrix = {{1, 2}, {3, 4}}
                ret matrix(1)(0)
            }
            """,
            [("alloca [2 x [2 x i32]]", False), ("store [2 x [2 x i32]] [[2 x i32] [i32 1, i32 2], [2 x i32] [i32 3, i32 4]]", False)],
        ),
        (
            "array_ptr.lo",
            """
            def main() i32 {
                var row i32[4] = {1, 2, 3, 4}
                var p i32[4]* = &row
                (*p)(2) = 9
                ret row(2)
            }
            """,
            [("alloca [4 x i32]", False), ("alloca ptr", False), ("store ptr %1, ptr %2", False), ("getelementptr inbounds [4 x i32], ptr %3, i32 0, i32 2", False), ("store i32 9", False)],
        ),
        (
            "array_view_fixed_elem.lo",
            """
            def main() i32 {
                var row i32[4] = {1, 2, 3, 4}
                var slot i32[4]* = &row
                var mixed i32[4][*] = slot
                mixed(0)(2) = 9
                ret row(2)
            }
            """,
            [("alloca ptr", False), ("getelementptr inbounds [4 x i32], ptr ", False), ("store i32 9", False)],
        ),
        (
            "array_view_ptr.lo",
            """
            def main() i32 {
                var raw u8[4] = {1, 2, 3, 4}
                var bytes u8* = &raw(0)
                var view u8[*] = bytes
                view(1) = 9
                ret raw(1)
            }
            """,
            [("alloca [4 x i8]", False), ("getelementptr inbounds i8, ptr ", False), ("store i8 9", False)],
        ),
        (
            "array_view_nested_view.lo",
            """
            def main() i32 {
                var raw u8[4] = {1, 2, 3, 4}
                var bytes u8* = &raw(0)
                var view u8[*] = bytes
                var views u8[*][1] = {view}
                var first u8[*]* = &views(0)
                var nested u8[*][*] = first
                nested(0)(2) = 7
                ret raw(2)
            }
            """,
            [("alloca [1 x ptr]", False), ("getelementptr inbounds ptr, ptr ", False), ("getelementptr inbounds i8, ptr ", False), ("store i8 7", False)],
        ),
        (
            "array_view_nested_ptr.lo",
            """
            def main() i32 {
                var row i8[8] = {1, 2, 3, 4, 5, 6, 7, 8}
                var slot i8[8]* = &row
                var slots i8[8]*[1] = {slot}
                var first i8[8]** = &slots(0)
                var mixed i8[8]*[*] = first
                var picked i8[8]* = mixed(0)
                (*picked)(3) = 11
                ret row(3)
            }
            """,
            [("alloca [1 x ptr]", False), ("getelementptr inbounds ptr, ptr ", False), ("getelementptr inbounds [8 x i8], ptr ", False), ("store i8 11", False)],
        ),
        (
            "array_view_eq.lo",
            """
            def main() i32 {
                var raw u8[4] = {1, 2, 3, 4}
                var bytes u8* = &raw(0)
                var view u8[*] = bytes
                if bytes == view {
                    ret 1
                }
                ret 0
            }
            """,
            [("icmp eq ptr", False)],
        ),
    ]
    for name, source, expectations in success_cases:
        ir = _emit_ir(compiler, name, source)
        for needle, is_regex in expectations:
            if is_regex:
                assert_regex(ir, needle, label=name)
            else:
                assert_contains(ir, needle, label=name)

    failures = [
        (
            "array_view_nested_ptr_bad.lo",
            """
            def main() i32 {
                var row i8[8] = {1, 2, 3, 4, 5, 6, 7, 8}
                var slot i8[8]* = &row
                var mixed i8[8]*[*] = slot
                ret 0
            }
            """,
            ["initializer type mismatch for `mixed`: expected i8[8]*[*], got i8[8]*"],
        ),
        (
            "array_legacy_indexable_bad.lo",
            """
            def main() i32 {
                var raw u8[4] = {1, 2, 3, 4}
                var bytes u8* = &raw(0)
                var view u8[]* = bytes
                ret 0
            }
            """,
            [
                "explicit unsized array type syntax is not allowed inside pointer declarations: u8[]*",
                "Use `T[*]` instead, for example `u8[*]`. `[]` is not a user-writable type declaration syntax.",
            ],
        ),
        (
            "array_fixed_ptr_bad.lo",
            """
            def main() i32 {
                var raw i32[1] = {1}
                var p i32* = &raw(0)
                var fake i32[4]* = p
                ret 0
            }
            """,
            ["initializer type mismatch for `fake`: expected i32[4]*, got i32*"],
        ),
        (
            "array_decay_bad.lo",
            """
            def take(p i32[4]*) i32 {
                ret (*p)(0)
            }

            def main() i32 {
                var row i32[4] = {1, 2, 3, 4}
                ret take(row)
            }
            """,
            ["call argument type mismatch at index 0: expected i32[4]*, got i32[4]"],
        ),
        (
            "array_unsized_bad.lo",
            """
            def main() i32 {
                var row i32[] = {1, 2}
                ret 0
            }
            """,
            [
                "explicit unsized array type syntax is not allowed: i32[]",
                "Use fixed explicit dimensions like `i32[2]`. If you want inferred array dimensions, write `var a = {1, 2}`. If you need an indexable pointer, write `T[*]`.",
            ],
        ),
        (
            "array_bad_dim.lo",
            """
            def bad() i32 {
                var matrix i32[0][5] = {}
                ret 0
            }
            """,
            ["fixed-dimension arrays require positive integer literal sizes"],
        ),
        (
            "array_overflow.lo",
            """
            def bad() i32 {
                var a i32[2] = {1, 2, 3}
                ret 0
            }
            """,
            ["array initializer has too many elements"],
        ),
        (
            "array_bad_shape.lo",
            """
            def bad() i32 {
                var a i32[4][5] = {1, 2}
                ret 0
            }
            """,
            ["array initializer expects a nested brace group at index 0"],
        ),
        (
            "array_bad_depth.lo",
            """
            def bad() i32 {
                var a i32[1] = {{1}}
                ret 0
            }
            """,
            ["array initializer nesting is deeper than the array shape"],
        ),
        (
            "call_trailing_comma_bad.lo",
            """
            def foo(a i32, b i32) i32 {
                ret a + b
            }

            def main() i32 {
                ret foo(1, 2,)
            }
            """,
            ["unexpected ')'"],
        ),
        (
            "tuple_literal_trailing_comma_bad.lo",
            """
            def main() i32 {
                var pair <i32, bool> = (1, true,)
                ret 0
            }
            """,
            ["unexpected ')'"],
        ),
        (
            "param_trailing_comma_bad.lo",
            """
            def sum(a i32, b i32,) i32 {
                ret a + b
            }
            """,
            ["unexpected ')'; expected identifier, ref, newline."],
        ),
        (
            "array_dim_trailing_comma_bad.lo",
            """
            def main() i32 {
                var row i32[4,] = {1, 2, 3, 4}
                ret row(0)
            }
            """,
            ["unexpected ']'"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
