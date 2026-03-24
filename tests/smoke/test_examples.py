from __future__ import annotations

from pathlib import Path

from tests.harness.compiler import CompilerHarness


def test_examples_compile_to_verified_ir(compiler: CompilerHarness, repo_root: Path) -> None:
    for sample in [
        repo_root / "example" / "algorithms_suite.lo",
        repo_root / "example" / "c_ffi_linked_list.lo",
        repo_root / "example" / "data_model_suite.lo",
        repo_root / "example" / "function_pointer_suite.lo",
        repo_root / "example" / "syntax_suite.lo",
        repo_root / "example" / "modules" / "main.lo",
    ]:
        compiler.emit_ir(sample).expect_ok()


def test_c_ffi_linked_list_example_runs(compiler: CompilerHarness, repo_root: Path) -> None:
    input_path = repo_root / "example" / "c_ffi_linked_list.lo"
    build_result, exe_path = compiler.build_system_executable(input_path, output_name="c_ffi_linked_list")
    build_result.expect_ok()
    compiler.run_executable(exe_path).expect_exit_code(0)

