from __future__ import annotations

import json
import subprocess
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


def test_query_can_open_syntax_suite_example(query_bin: Path, repo_root: Path) -> None:
    proc = subprocess.run(
        [
            str(query_bin),
            str(repo_root / "example"),
            "--format",
            "json",
            "--command",
            "open syntax_suite",
        ],
        cwd=repo_root,
        text=True,
        capture_output=True,
    )

    assert proc.returncode == 0, proc.stderr or proc.stdout
    response = json.loads(proc.stdout.strip())
    assert response["ok"] is True, response
    assert response["result"]["path"] == str(
        repo_root / "example" / "syntax_suite.lo"
    ), response
