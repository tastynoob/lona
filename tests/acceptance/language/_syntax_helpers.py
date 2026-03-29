from __future__ import annotations

from tests.harness import assert_contains
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
