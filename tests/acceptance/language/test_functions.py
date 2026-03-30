from __future__ import annotations

from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def _expect_ir_failure(compiler: CompilerHarness, name: str, source: str, needles: list[str]) -> None:
    result = compiler.write_source(name, source)
    failed = compiler.emit_ir(result).expect_failed()
    for needle in needles:
        assert_contains(failed.stderr, needle, label=f"{name} diagnostic")


def test_function_pointer_lowering_variants(compiler: CompilerHarness) -> None:
    cases = [
        (
            "func_ptr.lo",
            """
            def foo(v i32) i32 {
                ret v
            }

            def hold() i32 {
                var cb (i32: i32) = foo&<i32>
                ret 0
            }
            """,
            [("define i32 @foo", False), ("define i32 @hold", False), ("store ptr @foo", False)],
        ),
        (
            "func_ptr_void.lo",
            """
            def ping() {}

            def hold() i32 {
                var cb (:) = ping&<>
                cb()
                ret 0
            }
            """,
            [("define void @ping", False), ("store ptr @ping", False), (r"call void %.*\(\)", True)],
        ),
        (
            "func_ptr_ptr.lo",
            """
            def foo(v i32) i32 {
                ret v
            }

            def hold() i32 {
                var cb (i32: i32) = foo&<i32>
                var slot (i32: i32)* = &cb
                ret (*slot)(7)
            }
            """,
            [("define i32 @foo", False), ("store ptr @foo", False), (r"call i32 %.*\(i32 7\)", True)],
        ),
        (
            "func_ptr_array.lo",
            """
            def ping() i32 {
                ret 7
            }

            def hold() i32 {
                var table (: i32)[1] = {ping&<>}
                ret table(0)()
            }
            """,
            [("define i32 @ping", False), ("store [1 x ptr]", False), (r"call i32 %.*\(\)", True)],
        ),
        (
            "func_ptr_ref.lo",
            """
            def set7(ref v i32) i32 {
                v = 7
                ret v
            }

            def hold() i32 {
                var cb (ref i32: i32) = set7&<ref i32>
                var x i32 = 1
                ret cb(ref x)
            }
            """,
            [("define i32 @set7(ptr ", False), ("store ptr @set7", False), (r"call i32 %.*\(ptr ", True)],
        ),
        (
            "func_ptr_derived_uninit.lo",
            """
            def hold() i32 {
                var slot (i32: i32)*
                ret 0
            }
            """,
            [("alloca ptr", False)],
        ),
    ]

    for name, source, expectations in cases:
        ir = compiler.emit_ir(compiler.write_source(name, source)).expect_ok().stdout
        for needle, is_regex in expectations:
            if is_regex:
                assert_regex(ir, needle, label=name)
            else:
                assert_contains(ir, needle, label=name)


def test_function_pointer_aggregate_and_direct_return_lowering(compiler: CompilerHarness) -> None:
    packed_ir = compiler.emit_ir(
        compiler.write_source(
            "func_ptr_packed_agg.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            def echo(v Pair) Pair {
                ret v
            }

            def hold() i32 {
                var cb (Pair: Pair) = echo&<Pair>
                var pair = Pair(left = 1, right = 2)
                ret cb(pair).right
            }
            """,
        )
    ).expect_ok().stdout
    assert_contains(packed_ir, "store ptr @echo", label="packed function pointer ir")
    assert_regex(packed_ir, r"^define i64 @echo\(i64 [^)]+\)", label="packed function pointer ir")
    assert_regex(packed_ir, r"call i64 %.*\(i64 %.*\)", label="packed function pointer ir")

    direct_ir = compiler.emit_ir(
        compiler.write_source(
            "func_ptr_direct_return_agg.lo",
            """
            struct Triple {
                a i32
                b i32
                c i32
            }

            def echo(v Triple) Triple {
                ret v
            }

            def hold() i32 {
                var cb (Triple: Triple) = echo&<Triple>
                var triple = Triple(a = 1, b = 2, c = 3)
                ret cb(triple).c
            }
            """,
        )
    ).expect_ok().stdout
    assert_contains(direct_ir, "store ptr @echo", label="direct return function pointer ir")
    assert_regex(direct_ir, r"^define %.*Triple @echo\(ptr [^)]+\)", label="direct return function pointer ir")
    assert_regex(direct_ir, r"call %.*Triple %.*\(ptr %.*\)", label="direct return function pointer ir")
    assert_not_contains(direct_ir, "sret", label="direct return function pointer ir")


def test_function_pointer_related_diagnostics(compiler: CompilerHarness) -> None:
    cases = [
        (
            "func_ptr_ref_bad.lo",
            """
            def set7(ref v i32) i32 {
                v = 7
                ret v
            }

            def hold() i32 {
                var cb = set7&<i32>
                var x i32 = 1
                ret cb(ref x)
            }
            """,
            ["function reference parameter type mismatch at index 0 for `set7`: expected ref i32, got i32"],
        ),
        (
            "func_ptr_bad.lo",
            """
            def foo(v i32) i32 {
                ret v
            }

            def hold() i32 {
                var cb = foo&<bool>
                ret 0
            }
            """,
            ["function reference parameter type mismatch at index 0 for `foo`: expected i32, got bool"],
        ),
        (
            "func_ptr_uninit.lo",
            """
            def bad_holder() i32 {
                var cb (i32: i32)
                ret 0
            }
            """,
            ["function pointer variable type for `cb` requires initializer: (i32: i32)"],
        ),
        (
            "func_array_uninit.lo",
            """
            def bad_table() i32 {
                var table (: i32)[2]
                ret 0
            }
            """,
            [
                "function pointer array variable for `table` requires a full initializer: (: i32)[2]",
                "Missing elements would become null function pointers.",
            ],
        ),
        (
            "func_array_partial.lo",
            """
            def ping() i32 {
                ret 7
            }

            def bad_table() i32 {
                var table (: i32)[2] = {ping&<>}
                ret 0
            }
            """,
            [
                "function pointer arrays require full initialization: expected exactly 2 elements, got 1",
                "Missing elements would become null function pointers.",
            ],
        ),
    ]
    for name, source, needles in cases:
        _expect_ir_failure(compiler, name, source, needles)


def test_ffi_json_and_valid_signatures_lower_correctly(compiler: CompilerHarness) -> None:
    json_result = compiler.emit_json(
        compiler.write_source(
            "ffi_decl_json.lo",
            """
            #[extern "C"]
            def puts(msg i8*) i32

            struct FILE

            #[repr "C"]
            struct Point {
                x i32
                y i32
            }
            """,
        )
    ).expect_ok().stdout
    for needle in ['"abiKind": "c"', '"declKind": "opaque"', '"declKind": "repr_c"', '"body": null']:
        assert_contains(json_result, needle, label="ffi decl json")

    native_decl_ir = compiler.emit_ir(
        compiler.write_source(
            "native_decl.lo",
            """
            def add_decl(a i32, b i32) i32

            def main() i32 {
                ret add_decl(1, 2)
            }
            """,
        )
    ).expect_ok().stdout
    assert_regex(native_decl_ir, r"^declare .*i32 @add_decl\(i32, i32\)", label="native decl ir")
    assert_contains(native_decl_ir, "call i32 @add_decl(i32 1, i32 2)", label="native decl ir")
    assert_not_contains(native_decl_ir, "define i32 @add_decl", label="native decl ir")

    string_ir = compiler.emit_ir(
        compiler.write_source(
            "ffi_string.lo",
            """
            #[extern "C"]
            def inspect(msg u8 const[*]) i32

            def main() i32 {
                ret inspect("ok")
            }
            """,
        )
    ).expect_ok().stdout
    assert_contains(string_ir, 'private constant [3 x i8] c"ok\\00", align 1', label="ffi string ir")
    assert_regex(string_ir, r"^declare .*i32 @inspect\(ptr\)", label="ffi string ir")
    assert_contains(string_ir, "call i32 @inspect(ptr", label="ffi string ir")

    pointer_sig_ir = compiler.emit_ir(
        compiler.write_source(
            "ffi_pointer_sig.lo",
            """
            struct FILE

            #[repr "C"]
            struct Point {
                x i32
                y i32
            }

            #[extern "C"]
            def shift(p Point*, fp FILE*) Point*

            def main() i32 {
                ret 0
            }
            """,
        )
    ).expect_ok().stdout
    assert_regex(pointer_sig_ir, r"^declare .*ptr @shift\(ptr, ptr\)", label="ffi pointer sig ir")

    ffi_import_ir = compiler.emit_ir(
        compiler.write_source(
            "ffi_import.lo",
            """
            #[extern "C"]
            def abs(v i32) i32

            def main() i32 {
                ret 0
            }
            """,
        )
    ).expect_ok().stdout
    assert_regex(ffi_import_ir, r"^declare .*i32 @abs\(i32\)", label="ffi import ir")
    assert_not_contains(ffi_import_ir, "define i32 @abs", label="ffi import ir")

    ffi_export_ir = compiler.emit_ir(
        compiler.write_source(
            "ffi_export.lo",
            """
            #[extern "C"]
            def lona_add(a i32, b i32) i32 {
                ret a + b
            }
            """,
        )
    ).expect_ok().stdout
    assert_regex(ffi_export_ir, r"^define i32 @lona_add\(i32 [^,]+, i32 [^)]+\)", label="ffi export ir")

    ffi_export_pointer_ir = compiler.emit_ir(
        compiler.write_source(
            "ffi_export_pointer.lo",
            """
            #[repr "C"]
            struct Point {
                x i32
                y i32
            }

            #[extern "C"]
            def passthrough(p Point*) Point* {
                ret p
            }
            """,
        )
    ).expect_ok().stdout
    assert_regex(ffi_export_pointer_ir, r"^define ptr @passthrough\(ptr [^)]+\)", label="ffi export pointer ir")

    self_ptr_ir = compiler.emit_ir(
        compiler.write_source(
            "self_ptr_struct.lo",
            """
            #[repr "C"]
            struct Node {
                value i32
                set next Node*
            }

            def main() i32 {
                var node Node
                var p Node* = &node
                (*p).next = p
                ret 0
            }
            """,
        )
    ).expect_ok().stdout
    assert_regex(self_ptr_ir, r"^%.*Node = type \{ i32, ptr \}$", label="self pointer struct ir")
    assert_regex(self_ptr_ir, r"alloca %.*Node", label="self pointer struct ir")
    assert_contains(self_ptr_ir, "store ptr ", label="self pointer struct ir")

    ffi_indexable_ir = compiler.emit_ir(
        compiler.write_source(
            "ffi_indexable.lo",
            """
            #[extern "C"]
            def alloc(size u64) u8[*]

            #[extern "C"]
            def fill(buf u8[*], value u8) i32
            """,
        )
    ).expect_ok().stdout
    assert_regex(ffi_indexable_ir, r"^declare .*ptr @alloc\(i64\)", label="ffi indexable ir")
    assert_regex(ffi_indexable_ir, r"^declare .*i32 @fill\(ptr, i8\)", label="ffi indexable ir")


def test_invalid_ffi_declarations_are_rejected(compiler: CompilerHarness) -> None:
    cases = [
        (
            "ffi_abi_bad.lo",
            '#[extern "Rust"]\ndef bad(v i32) i32\n',
            ['semantic error: unsupported extern ABI `Rust`', 'help: Only `#[extern "C"]` is supported right now.'],
        ),
        (
            "ffi_extern_arg_count_bad.lo",
            '#[extern]\ndef bad(v i32) i32\n',
            [
                'semantic error: invalid arguments for tag `extern` on function `bad`: expected 1 argument, got 0',
                'help: Use syntax like `#[extern "C"]`.',
            ],
        ),
        (
            "ffi_repr_arg_type_bad.lo",
            '#[repr C]\nstruct Point {\n    x i32\n}\n',
            [
                'semantic error: invalid arguments for tag `repr` on struct `Point`: argument 0 must be a string literal',
                'help: Use syntax like `#[repr "C"]`.',
            ],
        ),
        (
            "ffi_repr_bad.lo",
            '#[repr "Rust"]\nstruct Point {\n    x i32\n}\n',
            ['semantic error: unsupported struct repr `Rust`', 'help: Only `#[repr "C"]` is supported right now.'],
        ),
        (
            "ffi_repr_func_bad.lo",
            '#[repr "C"]\ndef bad(v i32) i32\n',
            [
                'semantic error: cannot apply tag `repr` to function `bad`',
                'help: Use `#[extern "C"]` for C ABI functions. The `repr` tag only applies to struct declarations.',
            ],
        ),
        (
            "ffi_extern_global_arg_count_bad.lo",
            '#[extern "C"]\nglobal file i32\n',
            [
                'semantic error: invalid arguments for tag `extern` on global `file`: expected 0 arguments, got 1',
                'help: Use syntax like `#[extern] global name T`.',
            ],
        ),
        (
            "ffi_extern_struct_bad.lo",
            "#[extern]\nstruct FILE\n",
            [
                'semantic error: cannot apply tag `extern` to struct `FILE`',
                'help: Write `struct FILE` for an opaque type, or use `#[repr "C"] struct FILE { ... }` for a C-compatible layout. The `extern` tag only applies to function declarations.',
            ],
        ),
        (
            "ffi_repr_opaque_bad.lo",
            '#[repr "C"]\nstruct FILE\n',
            [
                'semantic error: #[repr "C"] struct `FILE` requires a body',
                'help: Use `struct FILE` for an opaque type, or add fields to `#[repr "C"] struct FILE { ... }`.',
            ],
        ),
        (
            "ffi_method_bad.lo",
            """
            struct Point {
                x i32

                #[extern "C"]
                def bad(v i32) i32 {
                    ret v
                }
            }
            """,
            [
                'semantic error: tag `extern` is only allowed on top-level declarations',
                'help: Move the tagged declaration to module scope. Tags are not supported inside functions, structs, or control-flow blocks.',
            ],
        ),
        (
            "ffi_nested_local_tag_bad.lo",
            """
            def main() i32 {
                #[extern "C"]
                def bad(v i32) i32 {
                    ret v
                }
                ret 0
            }
            """,
            [
                'semantic error: tag `extern` is only allowed on top-level declarations',
                'help: Move the tagged declaration to module scope. Tags are not supported inside functions, structs, or control-flow blocks.',
            ],
        ),
        (
            "ffi_ref_bad.lo",
            '#[extern "C"]\ndef bad(ref x i32) i32\n',
            [
                'semantic error: #[extern "C"] function `bad` parameter `x` cannot use `ref` binding',
                'help: Use an explicit pointer type like `i32*` instead.',
            ],
        ),
        (
            "ffi_callback_bad.lo",
            '#[extern "C"]\ndef bad(cb (i32: i32)) i32\n',
            [
                'semantic error: #[extern "C"] function `bad` uses unsupported parameter `cb`: (i32: i32)',
                'help: Callback support is not implemented in C FFI v0 yet.',
            ],
        ),
        (
            "ffi_callback_ptr_bad.lo",
            '#[extern "C"]\ndef bad(slot (i32: i32)*) i32\n',
            [
                'semantic error: #[extern "C"] function `bad` uses unsupported parameter `slot`: (i32: i32)*',
                'help: Callback support is not implemented in C FFI v0 yet.',
            ],
        ),
        (
            "ffi_aggregate_bad.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            #[extern "C"]
            def bad(p Pair) i32
            """,
            [
                'semantic error: #[extern "C"] function `bad` uses unsupported parameter `p`: Pair',
                'help: Pass a pointer instead. C FFI v0 does not support aggregate values at the boundary yet.',
            ],
        ),
        (
            "ffi_struct_ptr_bad.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            #[extern "C"]
            def bad(p Pair*) i32
            """,
            [
                'semantic error: #[extern "C"] function `bad` uses unsupported parameter `p`: Pair*',
                'help: Use pointers to scalars, pointers, opaque `struct` declarations, or `#[repr "C"] struct` types. Ordinary Lona structs cannot cross the C FFI boundary.',
            ],
        ),
        (
            "ffi_opaque_var_bad.lo",
            """
            struct FILE

            def main() i32 {
                var file FILE
                ret 0
            }
            """,
            [
                'semantic error: opaque struct `FILE` cannot be used by value in variable `file`',
                'help: Use `FILE*` instead. Opaque structs are only supported behind pointers.',
            ],
        ),
        (
            "ffi_opaque_ctor_bad.lo",
            """
            struct FILE

            def main() i32 {
                var file = FILE()
                ret 0
            }
            """,
            [
                'semantic error: opaque struct `',
                'FILE` cannot be constructed by value',
                'from an API that owns the storage instead. Opaque structs do not expose fields or value layout.',
            ],
        ),
        (
            "ffi_repr_field_bad.lo",
            """
            struct Pair {
                left i32
                right i32
            }

            #[repr "C"]
            struct Wrapper {
                pair Pair
            }
            """,
            [
                'semantic error: #[repr "C"] struct `Wrapper` field `pair` uses unsupported type: Pair',
                'help: Use only C-compatible field types: scalars, raw pointers, fixed arrays of C-compatible elements, or nested `#[repr "C"]` structs.',
            ],
        ),
    ]
    for name, source, needles in cases:
        _expect_ir_failure(compiler, name, source, needles)


def test_name_conflicts_bare_function_syntax_and_inferred_function_values_are_rejected(compiler: CompilerHarness) -> None:
    cases = [
        (
            "func_name_conflict.lo",
            """
            struct Counter {
                value i32
            }

            def Counter(value i32) i32 {
                ret value
            }
            """,
            ['top-level function `Counter` conflicts with struct `Counter`', 'Type names reserve constructor syntax like `Counter(...)`.'],
        ),
        (
            "struct_name_conflict.lo",
            """
            def Counter(value i32) i32 {
                ret value
            }

            struct Counter {
                value i32
            }
            """,
            ['struct `Counter` conflicts with top-level function `Counter`', 'Type names reserve constructor syntax like `Counter(...)`.'],
        ),
        (
            "type_member_bad.lo",
            """
            struct Counter {
                value i32
            }

            def main() i32 {
                ret Counter.zero()
            }
            """,
            ['unknown type member `Counter.zero`', 'Static type members are not implemented yet.'],
        ),
        (
            "func_param_bad.lo",
            "def bad_callback(cb () i32) i32 {\n    ret 0\n}\n",
            ["syntax error: I couldn't parse this statement:"],
        ),
        (
            "func_local_bad.lo",
            "def bad_local() i32 {\n    var cb () i32\n    ret 0\n}\n",
            ["syntax error: I couldn't parse this statement:"],
        ),
        (
            "func_top_bad.lo",
            "var cb () i32\n",
            ["syntax error: I couldn't parse this statement:"],
        ),
        (
            "func_inferred_local_bad.lo",
            """
            def foo() i32 {
                ret 1
            }

            def bad_inferred_local() i32 {
                var cb = foo
                ret 0
            }
            """,
            ['unsupported bare function variable type for `cb`: (: i32)'],
        ),
        (
            "func_inferred_top_bad.lo",
            """
            def foo() i32 {
                ret 1
            }

            var cb = foo
            """,
            ['unsupported bare function variable type for `cb`: (: i32)'],
        ),
    ]
    for name, source, needles in cases:
        _expect_ir_failure(compiler, name, source, needles)


def test_bare_method_values_and_selector_misuse_are_rejected(compiler: CompilerHarness) -> None:
    shared_struct = """
    struct Complex {
        real i32
        imag i32

        def add(a Complex) Complex {
            ret Complex(real = self.real + a.real, imag = self.imag + a.imag)
        }
    }
    """

    cases = [
        (
            "func_inferred_method_local_bad.lo",
            shared_struct
            + """
            def bad_method_local() i32 {
                var c = Complex(1, 2)
                var cb = c.add
                ret 0
            }
            """,
            ['unsupported bare function variable type for `cb`: (', '.Complex: ', '.Complex)'],
        ),
        (
            "func_inferred_method_top_bad.lo",
            shared_struct
            + """
            var sample = Complex(1, 2)
            var cb = sample.add
            """,
            ['unsupported bare function variable type for `cb`: (', '.Complex: ', '.Complex)'],
        ),
        (
            "func_method_return_bad.lo",
            shared_struct
            + """
            def bad_method_return() Complex {
                var c = Complex(1, 2)
                ret c.add
            }
            """,
            ['method selector can only be used as a direct call callee'],
        ),
        (
            "func_method_arg_bad.lo",
            shared_struct
            + """
            def take(v Complex) Complex {
                ret v
            }

            def bad_method_arg() Complex {
                var c = Complex(1, 2)
                ret take(c.add)
            }
            """,
            ['method selector can only be used as a direct call callee'],
        ),
        (
            "func_method_expr_bad.lo",
            shared_struct
            + """
            def bad_method_expr() i32 {
                var c = Complex(1, 2)
                c.add
                ret 0
            }
            """,
            ['method selector can only be used as a direct call callee'],
        ),
        (
            "func_method_top_expr_bad.lo",
            shared_struct
            + """
            var sample = Complex(1, 2)
            sample.add
            """,
            ['method selector can only be used as a direct call callee'],
        ),
    ]

    for name, source, needles in cases:
        result = compiler.emit_ir(compiler.write_source(name, source)).expect_failed()
        for needle in needles:
            assert_contains(result.stderr, needle, label=f"{name} diagnostic")


def test_call_argument_and_return_type_mismatches_are_rejected(compiler: CompilerHarness) -> None:
    cases = [
        (
            "call_arg_type_bad.lo",
            """
            def foo(v i32) i32 {
                ret v
            }

            def bad_call_type() i32 {
                ret foo(true)
            }
            """,
            ['call argument type mismatch at index 0: expected i32, got bool'],
        ),
        (
            "call_arg_count_bad.lo",
            """
            def foo(v i32) i32 {
                ret v
            }

            def bad_call_count() i32 {
                ret foo()
            }
            """,
            ['call argument count mismatch: expected 1, got 0'],
        ),
        (
            "return_type_bad.lo",
            """
            def bad() i32 {
                ret true
            }
            """,
            ['return type mismatch: expected i32, got bool'],
        ),
    ]
    for name, source, needles in cases:
        _expect_ir_failure(compiler, name, source, needles)
