from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


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

    const_binding_json = _emit_json(
        compiler,
        "const_binding_json.lo",
        """
        def main() i32 {
            const value = 1
            ret value
        }
        """,
    )
    for needle in ['"type": "VarDef"', '"readOnlyBinding": true', '"field": "value"']:
        assert_contains(const_binding_json, needle, label="const binding json")

    typed_const_binding_json = _emit_json(
        compiler,
        "typed_const_binding_json.lo",
        """
        def main() i32 {
            const ptr u8 const[*] = "hi"
            ret 0
        }
        """,
    )
    for needle in ['"type": "VarDef"', '"readOnlyBinding": true', '"field": "ptr"', '"declaredType": "u8 const[*]"']:
        assert_contains(typed_const_binding_json, needle, label="typed const binding json")

    const_materialize_ir = _emit_ir(
        compiler,
        "const_materialize.lo",
        """
        def main() i32 {
            var bytes u8 const[2] = {1, 2}
            var copy = bytes
            copy(0) = 7

            const ptr = &copy(0)
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
            "const_binding_assign_bad.lo",
            """
            def main() i32 {
                const value = 1
                value = 2
                ret 0
            }
            """,
            ["assignment target contains read-only storage: i32 const"],
        ),
        (
            "const_binding_rewrap_once_bad.lo",
            """
            def main() i32 {
                const a = 1
                const b = a
                b = 2
                ret 0
            }
            """,
            ["assignment target contains read-only storage: i32 const"],
        ),
        (
            "val_binding_syntax_removed.lo",
            """
            def main() i32 {
                val value = 1
                ret value
            }
            """,
            ["syntax error:"],
        ),
        (
            "var_top_level_const_bad.lo",
            """
            def main() i32 {
                var value u8 const = 1
                ret 0
            }
            """,
            ["variable `value` cannot use a top-level const storage type: u8 const"],
        ),
        (
            "var_top_level_const_ptr_bad.lo",
            """
            def main() i32 {
                var raw u8* const = null
                ret 0
            }
            """,
            [
                "variable `raw` cannot use a top-level const storage type: u8* const",
                "Use `const raw = ...` or `const raw T = ...` for a read-only binding",
            ],
        ),
        (
            "const_array_assign_bad.lo",
            """
            def main() i32 {
                var a u8 const[2] = {1, 2}
                var b u8 const[2] = {3, 4}
                a = b
                ret 0
            }
            """,
            ["assignment target contains read-only storage: u8 const[2]"],
        ),
        (
            "const_tuple_assign_bad.lo",
            """
            def main() i32 {
                var pair <i32 const, i32> = (1, 2)
                var next <i32 const, i32> = (3, 4)
                pair = next
                ret 0
            }
            """,
            ["assignment target contains read-only storage: <i32 const, i32>"],
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
