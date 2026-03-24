from __future__ import annotations

from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def _emit_ir(compiler: CompilerHarness, name: str, source: str) -> str:
    return compiler.emit_ir(compiler.write_source(name, source)).expect_ok().stdout


def _emit_json(compiler: CompilerHarness, name: str, source: str) -> str:
    return compiler.emit_json(compiler.write_source(name, source)).expect_ok().stdout


def _expect_ir_failure(compiler: CompilerHarness, name: str, source: str, needles: list[str]) -> str:
    result = compiler.emit_ir(compiler.write_source(name, source)).expect_failed()
    for needle in needles:
        assert_contains(result.stderr, needle, label=f"{name} diagnostic")
    return result.stderr


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


def test_builtin_casts_and_cast_restrictions(compiler: CompilerHarness) -> None:
    numeric_ir = _emit_ir(
        compiler,
        "cast_numeric.lo",
        """
        def main() i32 {
            var base i32 = 1
            var sample f32 = cast[f32](base)
            var widened f64 = cast[f64](sample)
            ret cast[i32](widened)
        }
        """,
    )
    for needle in ["sitofp i32", "fpext float", "fptosi double"]:
        assert_contains(numeric_ir, needle, label="numeric cast ir")

    pointer_ir = _emit_ir(
        compiler,
        "cast_pointer.lo",
        """
        def main() i32 {
            var bytes u8[2] = {7, 9}
            var raw u8* = &bytes(0)
            var view u8[*] = cast[u8[*]](raw)
            ret view(1)
        }
        """,
    )
    for needle in ["ptrtoint ptr %", "inttoptr i64 %"]:
        assert_contains(pointer_ir, needle, label="pointer cast ir")
    assert_regex(pointer_ir, r"getelementptr( inbounds)? i8, ptr %", label="pointer cast ir")

    pointer_rebind_ir = _emit_ir(
        compiler,
        "cast_pointer_rebind.lo",
        """
        struct Box {
            value i32
        }

        def main() i32 {
            var box Box
            var bytes u8[*] = cast[u8[*]](&box)
            var view Box[*] = cast[Box[*]](bytes)
            view(0).value = 7
            ret box.value
        }
        """,
    )
    for needle in ["define i32 @main", "ptrtoint ptr %", "inttoptr i64 %"]:
        assert_contains(pointer_rebind_ir, needle, label="pointer rebind cast ir")

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


def test_float_numeric_and_tobits_paths(compiler: CompilerHarness) -> None:
    float_ir = _emit_ir(
        compiler,
        "float.lo",
        """
        def id(v f32) f32 {
            ret v
        }

        def bad() f32 {
            var x f32 = 1.0 + 2.0
            ret id(-x)
        }
        """,
    )
    for needle in ["define float @id", "define float @bad", "store float 3.000000e+00", "fneg float %", "call float @id(float %"]:
        assert_contains(float_ir, needle, label="float ir")

    numeric_convert_ir = _emit_ir(
        compiler,
        "numeric_convert.lo",
        """
        def main() i32 {
            var base i32 = cast[i32](1.5)
            var sample f32 = cast[f32](base)
            var promoted f64 = sample
            ret cast[i32](promoted)
        }
        """,
    )
    for needle in ["fptosi double", "sitofp i32", "fpext float"]:
        assert_contains(numeric_convert_ir, needle, label="numeric convert ir")

    numeric_convert_chain_ir = _emit_ir(
        compiler,
        "numeric_convert_chain.lo",
        """
        def chain(v f64) i32 {
            ret cast[i32](cast[f32](cast[i32](v)))
        }
        """,
    )
    for needle in ["fptosi double", "sitofp i32", "fptosi float"]:
        assert_contains(numeric_convert_chain_ir, needle, label="numeric convert chain ir")

    tobits_ir = _emit_ir(
        compiler,
        "tobits.lo",
        """
        def main() i32 {
            var v i8 = -1
            var raw u8[1] = v.tobits()
            var wide i32 = raw.toi32()
            ret wide
        }
        """,
    )
    for needle in ["insertvalue [1 x i8]", "extractvalue [1 x i8]", "zext i8"]:
        assert_contains(tobits_ir, needle, label="tobits ir")
    assert_not_contains(tobits_ir, "sext i8", label="tobits ir")

    tobits_infer_ir = _emit_ir(
        compiler,
        "tobits_infer.lo",
        """
        def main() i32 {
            var raw = 7.tobits()
            ret raw.toi32()
        }
        """,
    )
    for needle in ['alloca [4 x i8]', 'store [4 x i8] c"\\07\\00\\00\\00"', 'extractvalue [4 x i8]']:
        assert_contains(tobits_infer_ir, needle, label="tobits infer ir")

    wide_bits_ir = _emit_ir(
        compiler,
        "wide_bits.lo",
        """
        def main() i32 {
            var raw u8[256] = {}
            ret raw.toi32()
        }
        """,
    )
    assert wide_bits_ir.count("extractvalue [256 x i8]") == 4, wide_bits_ir
    assert_not_contains(wide_bits_ir, "i2048", label="wide bits ir")

    implicit_numeric_ir = _emit_ir(
        compiler,
        "implicit_numeric.lo",
        """
        def widen(v u8) u16 {
            ret v
        }

        def main() i32 {
            var x u8 = 1
            ret widen(x)
        }
        """,
    )
    for needle in ["store i8 1", "zext i8", "zext i16"]:
        assert_contains(implicit_numeric_ir, needle, label="implicit numeric ir")

    mixed_numeric_op_ir = _emit_ir(
        compiler,
        "mixed_numeric_op.lo",
        """
        def main() i32 {
            var a u8 = 1
            var b i32 = 2
            ret a + b
        }
        """,
    )
    for needle in ["zext i8", "add i32"]:
        assert_contains(mixed_numeric_op_ir, needle, label="mixed numeric op ir")

    float_nan_cmp_ir = _emit_ir(
        compiler,
        "float_nan_cmp.lo",
        """
        def bad() bool {
            var z f64 = 0.0
            var nan f64 = z / z
            ret nan != nan
        }
        """,
    )
    assert_contains(float_nan_cmp_ir, "fcmp une double", label="float nan cmp ir")

    failures = [
        (
            "numeric_cross_bad.lo",
            """
            def main() i32 {
                var x f32 = 1
                ret 0
            }
            """,
            ["initializer type mismatch for `x`: expected f32, got i32", "cast[T](expr)"],
        ),
        (
            "numeric_member_removed.lo",
            """
            def main() i32 {
                var x i32 = 1
                var y f32 = x.tof32()
                ret 0
            }
            """,
            ["unknown member `i32.tof32`"],
        ),
        (
            "injected_member_bad.lo",
            """
            def main() i32 {
                var x = 1.tobits
                ret 0
            }
            """,
            ["injected member `tobits` can only be used as a direct call callee"],
        ),
        (
            "float_literal_target_bad.lo",
            """
            def bad() u64 {
                var x u64 = 1.25
                ret x
            }
            """,
            ["floating-point literal doesn't match the expected target type"],
        ),
        (
            "bool_bytecopy_bad.lo",
            """
            def bad() i8 {
                var b bool = true
                var x i8 = b
                ret x
            }
            """,
            ["initializer type mismatch for `x`: expected i8, got bool"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_string_and_null_semantics(compiler: CompilerHarness) -> None:
    string_value_ir = _emit_ir(
        compiler,
        "string_value.lo",
        """
        def main() i32 {
            var msg = "hello"
            msg(0) = 72
            ret msg(0)
        }
        """,
    )
    for needle in [
        'private constant [5 x i8] c"hello", align 1',
        "alloca [5 x i8]",
        "load [5 x i8], ptr @.lona.bytes.",
        "store [5 x i8] %",
        "store i8 72",
    ]:
        assert_contains(string_value_ir, needle, label="string value ir")

    literal_index_ir = _emit_ir(
        compiler,
        "string_literal_index.lo",
        """
        def main() u8 {
            ret "hi"(1)
        }
        """,
    )
    assert_contains(literal_index_ir, 'private constant [2 x i8] c"hi", align 1', label="string literal index ir")
    assert_regex(
        literal_index_ir,
        r"load i8, ptr getelementptr inbounds \(\[2 x i8\], ptr @\.lona\.bytes\.[0-9]+, i32 0, i32 1\)",
        label="string literal index ir",
    )

    string_borrow_ir = _emit_ir(
        compiler,
        "string_borrow.lo",
        """
        def second(msg u8 const[*]) u8 {
            ret msg(1)
        }

        def main() i32 {
            var bytes = &"hi"
            ret second(bytes)
        }
        """,
    )
    for needle in [".lona.bytes.", 'private constant [2 x i8] c"hi", align 1', "call i8 @second(ptr"]:
        assert_contains(string_borrow_ir, needle, label="string borrow ir")
    assert_not_contains(string_borrow_ir, "unnamed_addr", label="string borrow ir")

    string_escape_ir = _emit_ir(
        compiler,
        "string_escape.lo",
        """
        def main() i32 {
            var bytes = "A\\x42\\0"
            ret bytes(1)
        }
        """,
    )
    assert_contains(string_escape_ir, 'private constant [3 x i8] c"AB\\00", align 1', label="string escape ir")

    null_ir = _emit_ir(
        compiler,
        "null_pointer.lo",
        """
        def maybe(flag bool) u8 const[*] {
            if flag {
                ret null
            }
            ret &"ok"
        }

        def is_missing(data u8 const[*]) bool {
            ret data == null
        }

        def main() i32 {
            var value i32 = 7
            var raw i32* = null
            if raw == null {
                raw = &value
            }

            var view i32[*] = cast[i32[*]](raw)
            if view == null {
                ret 1
            }
            if !is_missing(null) {
                ret 2
            }
            if !is_missing(maybe(true)) {
                ret 3
            }
            if is_missing(maybe(false)) {
                ret 4
            }
            ret view(0)
        }
        """,
    )
    for needle in ["store ptr null", "icmp eq ptr", "call i8 @is_missing(ptr null)"]:
        assert_contains(null_ir, needle, label="null pointer ir")

    failures = [
        (
            "string_const_borrow_bad.lo",
            """
            def main() i32 {
                var bytes = &"hi"
                bytes(0) = 0
                ret 0
            }
            """,
            ["assignment target is const-qualified: u8 const"],
        ),
        (
            "string_str_bad.lo",
            """
            def main() i32 {
                var title str = "lona"
                ret 0
            }
            """,
            ['semantic error: unknown variable type', 'var title str = "lona"'],
        ),
        (
            "null_infer_bad.lo",
            """
            def main() i32 {
                var ptr = null
                ret 0
            }
            """,
            ["cannot infer the type of `ptr` from `null`"],
        ),
        (
            "null_scalar_bad.lo",
            """
            def main() i32 {
                var value i32 = null
                ret value
            }
            """,
            ["`null` can only be used with pointer types"],
        ),
        (
            "null_cmp_bad.lo",
            """
            def main() bool {
                ret null == null
            }
            """,
            ["`null` comparison requires a concrete pointer operand"],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_const_types_materialization_and_const_rejections(compiler: CompilerHarness) -> None:
    const_json = _emit_json(
        compiler,
        "const_type_json.lo",
        """
        def main() i32 {
            var bytes u8 const[4]
            var ptr u8 const[*]
            var hold u8[*] const
            var raw u8* const
            ret 0
        }
        """,
    )
    for needle in ['"declaredType": "u8 const[4]"', '"declaredType": "u8 const[*]"', '"declaredType": "u8[*] const"', '"declaredType": "u8* const"']:
        assert_contains(const_json, needle, label="const type json")

    const_materialize_ir = _emit_ir(
        compiler,
        "const_materialize.lo",
        """
        def main() i32 {
            var bytes u8 const[2] = {1, 2}
            var copy = bytes
            copy(0) = 7

            var ptr u8* const = &copy(0)
            var next = ptr
            next = &copy(1)

            ret copy(0)
        }
        """,
    )
    for needle in ["alloca [2 x i8]", "store i8 7", "store ptr"]:
        assert_contains(const_materialize_ir, needle, label="const materialize ir")

    struct_const_ptr_field_ir = _emit_ir(
        compiler,
        "struct_const_ptr_field.lo",
        """
        struct Span {
            data u8 const[*]
        }

        def main() i32 {
            ret 0
        }
        """,
    )
    assert_contains(struct_const_ptr_field_ir, "define i32 @main", label="struct const ptr field ir")

    failures = [
        (
            "const_assign_bad.lo",
            """
            def main() i32 {
                var value u8 const = 1
                value = 2
                ret 0
            }
            """,
            ["assignment target is const-qualified: u8 const"],
        ),
        (
            "struct_const_field_bad.lo",
            """
            struct BadValue {
                value u8 const
            }
            """,
            ["struct field `value` cannot use a const-qualified storage type"],
        ),
        (
            "struct_const_array_field_bad.lo",
            """
            struct BadArray {
                bytes u8 const[4]
            }
            """,
            ["struct field `bytes` cannot use a const-qualified storage type"],
        ),
        (
            "initial_list_hidden.lo",
            """
            def bad() i32 {
                var x initial_list
                ret 0
            }
            """,
            ["`initial_list` is a compiler-internal initialization interface", "Use brace initialization like `{1, 2, 3}` instead."],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


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
                var out Pair
                out.left = self.right + extra
                out.right = self.left + extra
                ret out
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
                var out Triple
                out.a = self.b + delta
                out.b = self.c + delta
                out.c = self.a + delta
                ret out
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
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)


def test_addressing_struct_init_named_calls_and_struct_field_types(compiler: CompilerHarness) -> None:
    address_field_ir = _emit_ir(
        compiler,
        "address_field.lo",
        """
        struct Counter {
            value i32
        }

        def main() i32 {
            var counter Counter
            counter.value = 5
            var p i32* = &counter.value
            *p = 8
            ret counter.value
        }
        """,
    )
    assert_regex(address_field_ir, r"getelementptr inbounds %.*Counter, ptr %.*, i32 0, i32 0", label="address field ir")
    assert_contains(address_field_ir, "store i32 8", label="address field ir")

    address_array_ir = _emit_ir(
        compiler,
        "address_array_elem.lo",
        """
        def main() i32 {
            var row i32[4] = {}
            var p i32* = &row(1)
            *p = 6
            ret row(1)
        }
        """,
    )
    for needle in ["getelementptr inbounds [4 x i32]", "store i32 6"]:
        assert_contains(address_array_ir, needle, label="address array elem ir")

    struct_init_ir = _emit_ir(
        compiler,
        "struct_init.lo",
        """
        struct Complex {
            real i32
            img i32
        }

        def fold(real i32, img i32) i32 {
            ret real + img
        }

        def main() i32 {
            var c = Complex(img = 2, real = 1)
            ret fold(img = c.img, real = c.real)
        }
        """,
    )
    for needle in ["store %", "Complex { i32 1, i32 2 }", "call i32 @fold(i32"]:
        assert_contains(struct_init_ir, needle, label="struct init ir")

    named_call_mix_ir = _emit_ir(
        compiler,
        "named_call_mix.lo",
        """
        def mix(x i32, y i32) i32 {
            ret x + y
        }

        def main() i32 {
            ret mix(1, y = 2)
        }
        """,
    )
    assert_contains(named_call_mix_ir, "call i32 @mix(i32 1, i32 2)", label="named call mix ir")

    struct_field_types_ir = _emit_ir(
        compiler,
        "struct_field_types.lo",
        """
        def inc(v i32) i32 {
            ret v + 1
        }

        struct Mixed {
            flag bool
            ratio f32
            bits u8[4]
            pair <i32, bool>
            ptr i32*
            cb (i32: i32)
        }

        def main() i32 {
            var x i32 = 41
            var raw u8[4] = cast[f32](1).tobits()
            var pair <i32, bool> = (1, true)
            var mixed = Mixed(flag = true, ratio = cast[f32](1), bits = raw, pair = pair, ptr = &x, cb = inc&<i32>)
            if mixed.flag && mixed.pair._2 && (mixed.ratio >= cast[f32](1)) {
                mixed.bits(0) = 1
                ret mixed.cb(*mixed.ptr) + mixed.bits(0) + mixed.pair._1
            }
            ret 0
        }
        """,
    )
    assert_regex(struct_field_types_ir, r"type \{ i8, float, \[4 x i8\], \{ i32, i8 \}, ptr, ptr \}", label="struct field types ir")
    assert_regex(struct_field_types_ir, r"call i32 %.*\(i32 %.*\)", label="struct field types ir")
    assert_regex(struct_field_types_ir, r"getelementptr inbounds .* i32 0, i32 4", label="struct field types ir")
    assert_regex(struct_field_types_ir, r"getelementptr inbounds .* i32 0, i32 5", label="struct field types ir")

    failures = [
        (
            "pointer_arith_bad.lo",
            """
            def main() i32 {
                var raw u8[4] = {1, 2, 3, 4}
                var bytes u8* = &raw(0)
                var next = bytes + 1
                ret 0
            }
            """,
            ["operator `+` doesn't support `u8*` and `i32`"],
        ),
        (
            "address_expr_bad.lo",
            """
            def main() i32 {
                var x i32 = 1
                var p i32* = &(x + 1)
                ret 0
            }
            """,
            ["address-of expects an addressable value"],
        ),
        (
            "address_temp_bad.lo",
            """
            struct Point {
                x i32
                y i32
            }

            def main() i32 {
                var p Point* = &Point(1, 2)
                ret 0
            }
            """,
            ["address-of expects an addressable value"],
        ),
        (
            "ctor_ref_bad.lo",
            """
            struct Complex {
                real i32
                img i32
            }

            def bad() i32 {
                var x i32 = 1
                var c = Complex(ref real = x, img = 2)
                ret 0
            }
            """,
            ["constructor arguments do not accept `ref`", "Constructors copy field values. Remove `ref` from this argument."],
        ),
        (
            "named_call_bad.lo",
            """
            def mix(x i32, y i32) i32 {
                ret x + y
            }

            def bad() i32 {
                ret mix(x = 1)
            }
            """,
            ["missing parameter `y` for function call"],
        ),
        (
            "named_call_order.lo",
            """
            def mix(x i32, y i32) i32 {
                ret x + y
            }

            def bad() i32 {
                ret mix(x = 1, 2)
            }
            """,
            ["positional arguments must come before named arguments"],
        ),
        (
            "ctor_unknown_field.lo",
            """
            struct Complex {
                real i32
                img i32
            }

            def bad() i32 {
                var c = Complex(real = 1, phase = 2)
                ret 0
            }
            """,
            ["unknown field `phase` for constructor `", "Complex"],
        ),
        (
            "struct_ref_field_bad.lo",
            """
            struct Bad {
                ref slot i32
            }
            """,
            ["syntax error: I couldn't parse this statement: unexpected ref.", "help: Check for a missing separator, unmatched delimiter, or mistyped keyword near here."],
        ),
    ]
    for name, source, needles in failures:
        _expect_ir_failure(compiler, name, source, needles)
