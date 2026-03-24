from __future__ import annotations

import re

from tests.harness import assert_contains, assert_regex
from tests.harness.compiler import CompilerHarness


def test_ref_local_binding_reuses_source_storage(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_local.lo",
        """
        def main() i32 {
            var x i32 = 3
            ref alias i32 = x
            alias = 9
            ret x
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert ir.count("alloca i32") == 2, (
        "expected ref local binding to reuse the source storage without adding another alloca\n" + ir
    )
    assert_contains(ir, "store i32 9, ptr %1", label="ref local ir")


def test_ref_parameter_lowers_to_pointer_call(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param.lo",
        """
        def set7(ref x i32) i32 {
            x = 7
            ret x
        }

        def main() i32 {
            var x i32 = 1
            ret set7(ref x)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^define i32 @set7\(ptr ", label="ref parameter ir")
    assert_contains(ir, "call i32 @set7(ptr ", label="ref parameter ir")


def test_named_ref_argument_lowers_to_pointer_call(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param_named.lo",
        """
        def set7(ref slot i32) i32 {
            slot = 7
            ret slot
        }

        def main() i32 {
            var x i32 = 1
            ret set7(ref slot = x)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^define i32 @set7\(ptr ", label="named ref parameter ir")
    assert_contains(ir, "call i32 @set7(ptr ", label="named ref parameter ir")


def test_ref_parameter_requires_explicit_ref_marker(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param_missing_ref.lo",
        """
        def set7(ref slot i32) i32 {
            slot = 7
            ret slot
        }

        def main() i32 {
            var x i32 = 1
            ret set7(x)
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "reference parameter `slot` must be passed with `ref`",
        label="missing ref marker diagnostic",
    )


def test_ref_parameter_rejects_rvalues(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param_rvalue.lo",
        """
        def set7(ref x i32) i32 {
            x = 7
            ret x
        }

        def main() i32 {
            ret set7(ref 1 + 2)
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "reference parameter `x` expects an addressable value",
        label="ref rvalue diagnostic",
    )


def test_ref_parameter_address_of_reuses_incoming_slot(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param_addr.lo",
        """
        def poke(ref x i32) i32 {
            var p i32* = &x
            *p = 9
            ret x
        }

        def main() i32 {
            var x i32 = 1
            ret poke(ref x)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    match = re.search(r"^define i32 @poke\(ptr %0\)(.*?)^}", ir, re.MULTILINE | re.DOTALL)
    assert match is not None, f"failed to locate @poke body\n{ir}"
    poke_body = match.group(0)
    assert poke_body.count("alloca i32") == 1, (
        "expected ref parameter address-of to reuse the incoming pointee slot without allocating a wrapper i32 slot\n"
        + poke_body
    )
    assert_regex(poke_body, r"store ptr %0, ptr %", label="poke body")


def test_ref_local_type_mismatch_is_rejected(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_local_type_bad.lo",
        """
        def main() i32 {
            var x i32 = 3
            ref alias i64 = x
            ret 0
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "reference binding type mismatch for `alias`: expected i64, got i32",
        label="ref local type mismatch diagnostic",
    )


def test_ref_const_view_parameter_accepts_mutable_argument(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param_const_view.lo",
        """
        def read(ref x i32 const) i32 {
            ret x
        }

        def main() i32 {
            var x i32 = 3
            ret read(ref x)
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^define i32 @read\(ptr ", label="const view ir")
    assert_contains(ir, "call i32 @read(ptr ", label="const view ir")


def test_ref_parameter_cannot_drop_constness(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_param_drop_const.lo",
        """
        def bump(ref x i32) i32 {
            x = x + 1
            ret x
        }

        def main() i32 {
            var x i32 const = 3
            ret bump(ref x)
        }
        """,
    )
    result = compiler.emit_ir(input_path).expect_failed()
    assert_contains(
        result.stderr,
        "reference parameter `x` type mismatch: expected i32, got i32 const",
        label="ref const drop diagnostic",
    )


def test_method_call_on_temporary_still_uses_pointer_receiver(compiler: CompilerHarness) -> None:
    input_path = compiler.write_source(
        "ref_method_temp.lo",
        """
        struct Counter {
            value i32

            def bump() i32 {
                self.value = self.value + 1
                ret self.value
            }
        }

        def main() i32 {
            ret Counter(1).bump()
        }
        """,
    )
    ir = compiler.emit_ir(input_path).expect_ok().stdout
    assert_regex(ir, r"^define i32 @.*Counter\.bump\(ptr ", label="method temp ir")
    assert_regex(ir, r"call i32 @.*Counter\.bump\(ptr ", label="method temp ir")

