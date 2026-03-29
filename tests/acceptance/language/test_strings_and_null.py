from __future__ import annotations

from tests.acceptance.language._syntax_helpers import _emit_ir, _emit_json, _expect_ir_failure
from tests.harness import assert_contains, assert_not_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_string_and_null_semantics(compiler: CompilerHarness) -> None:
    string_view_ir = _emit_ir(
        compiler,
        "string_view.lo",
        """
        def second(msg u8 const[*]) u8 {
            ret msg(1)
        }

        def main() i32 {
            var msg = "hello"
            ret second(msg)
        }
        """,
    )
    for needle in [
        'private constant [6 x i8] c"hello\\00", align 1',
        "alloca ptr",
        "call i8 @second(ptr",
    ]:
        assert_contains(string_view_ir, needle, label="string view ir")
    for needle in ["alloca [5 x i8]", "store [5 x i8]"]:
        assert_not_contains(string_view_ir, needle, label="string view ir")

    literal_index_ir = _emit_ir(
        compiler,
        "string_literal_index.lo",
        """
        def main() u8 {
            ret "hi"(1)
        }
        """,
    )
    assert_contains(literal_index_ir, 'private constant [3 x i8] c"hi\\00", align 1', label="string literal index ir")
    assert_regex(
        literal_index_ir,
        r"load i8, ptr getelementptr inbounds \(i8, ptr @\.lona\.bytes\.[0-9]+, i32 1\)",
        label="string literal index ir",
    )

    string_escape_ir = _emit_ir(
        compiler,
        "string_escape.lo",
        """
        def main() i32 {
            var bytes = "A\\x42\\x4\\0"
            ret bytes(1)
        }
        """,
    )
    assert_contains(string_escape_ir, 'private constant [5 x i8] c"AB\\04\\00\\00", align 1', label="string escape ir")

    empty_string_ir = _emit_ir(
        compiler,
        "empty_string.lo",
        """
        def main() u8 {
            ret ""(0)
        }
        """,
    )
    assert_contains(
        empty_string_ir,
        "private constant [1 x i8] zeroinitializer, align 1",
        label="empty string ir",
    )

    null_ir = _emit_ir(
        compiler,
        "null_pointer.lo",
        """
        def maybe(flag bool) u8 const[*] {
            if flag {
                ret null
            }
            ret "ok"
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
            "string_const_view_bad.lo",
            """
            def main() i32 {
                var bytes = "hi"
                bytes(0) = 0
                ret 0
            }
            """,
            ["assignment target contains read-only storage: u8 const"],
        ),
        (
            "string_address_removed.lo",
            """
            def main() i32 {
                var bytes = &"hi"
                ret bytes(0)
            }
            """,
            ["address-of expects an addressable value"],
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
