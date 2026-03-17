#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from pass_fail import TestSuite, expect, expect_equal


class SessionRunner:
    def __init__(self, runner_path: Path):
        self.proc = subprocess.Popen(
            [str(runner_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def close(self) -> None:
        if self.proc.stdin is not None:
            self.proc.stdin.close()
        self.proc.wait(timeout=5)
        stderr = ""
        if self.proc.stderr is not None:
            stderr = self.proc.stderr.read()
        expect(self.proc.returncode == 0, f"session runner exited with {self.proc.returncode}: {stderr}")

    def reset(self) -> None:
        reply = self._send({"command": "reset_session"})
        expect(reply.get("ok") is True, "reset_session failed")

    def compile(self, input_path: Path, verify_ir: bool = True) -> dict:
        return self._send(
            {
                "command": "compile",
                "input": str(input_path),
                "output_mode": "llvm_ir",
                "verify_ir": verify_ir,
            }
        )

    def _send(self, command: dict) -> dict:
        expect(self.proc.stdin is not None, "session runner stdin is closed")
        expect(self.proc.stdout is not None, "session runner stdout is closed")
        self.proc.stdin.write(json.dumps(command) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        expect(bool(line), "session runner returned no reply")
        return json.loads(line)


def write_file(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def expect_compile_ok(result: dict, compiled: int, reused: int) -> None:
    expect_equal(result["exit_code"], 0, "compile exit code")
    expect_equal(result["stderr"], "", "diagnostics")
    expect(result["stdout"] != "", "expected non-empty LLVM IR output")
    stats = result["stats"]
    expect_equal(stats["compiled_modules"], compiled, "compiled module count")
    expect_equal(stats["reused_modules"], reused, "reused module count")


def program_text(module_name: str, argument: int) -> str:
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        f"    ret {module_name}.inc({argument})\n"
        "}\n"
    )


def dependency_text(delta: int) -> str:
    return (
        "def inc(a i32) i32 {\n"
        f"    ret a + {delta}\n"
        "}\n"
    )


def dependency_with_helper(delta: int, helper_name: str) -> str:
    return (
        dependency_text(delta)
        + "\n"
        + f"def {helper_name}(a i32) i32 {{\n"
        "    ret a\n"
        "}\n"
    )


def array_dependency_text(columns: int, rows: int, value: int) -> str:
    return (
        "struct Box {\n"
        f"    data i32[{columns}][{rows}]\n"
        "}\n\n"
        "def make() Box {\n"
        f"    var matrix i32[{columns}][{rows}] = {{}}\n"
        f"    matrix(0)(0) = {value}\n"
        "    var box Box\n"
        "    box.data = matrix\n"
        "    ret box\n"
        "}\n"
    )


def array_program_text(module_name: str) -> str:
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        f"    var box {module_name}.Box = {module_name}.make()\n"
        "    ret box.data(0)(0)\n"
        "}\n"
    )


def run_body_vs_interface_case(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    dep_name = "dep"
    dep_path = root / f"{dep_name}.lo"
    app_path = root / "app.lo"
    initial_delta = rng.randint(1, 7)
    body_delta = rng.randint(8, 20)
    argument = rng.randint(1, 10)
    helper_name = f"helper_{rng.randint(100, 999)}"

    write_file(dep_path, dependency_text(initial_delta))
    write_file(app_path, program_text(dep_name, argument))

    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, dependency_text(body_delta))
    expect_compile_ok(runner.compile(app_path), compiled=1, reused=1)

    write_file(dep_path, dependency_with_helper(body_delta, helper_name))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)


def run_duplicate_basename_case(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    for index in range(2):
        project = root / f"project_{index}"
        dep_path = project / "dep.lo"
        app_path = project / "app.lo"
        delta = rng.randint(10, 50)
        argument = rng.randint(1, 5)
        write_file(dep_path, dependency_text(delta))
        write_file(app_path, program_text("dep", argument))
        expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)


def run_array_interface_hash_case(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    first_cols = rng.randint(2, 4)
    second_cols = first_cols + 1
    rows = rng.randint(2, 4)
    value = rng.randint(3, 9)

    write_file(dep_path, array_dependency_text(first_cols, rows, value))
    write_file(app_path, array_program_text("dep"))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, array_dependency_text(second_cols, rows, value))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)


def run_randomized_cases(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    case_count = 3
    for index in range(case_count):
        case_root = root / f"random_case_{index}"
        run_body_vs_interface_case(rng, runner, case_root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=1337)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    print(f"seed: {args.seed}")

    suite = TestSuite("incremental smoke")
    with tempfile.TemporaryDirectory(prefix="lona_incremental_smoke_") as temp_dir:
        root = Path(temp_dir)
        runner = SessionRunner(args.runner)
        try:
            suite.add(
                "body-change-vs-interface-change",
                lambda: run_body_vs_interface_case(rng, runner, root / "body_interface"),
            )
            suite.add(
                "duplicate-basenames-across-roots",
                lambda: run_duplicate_basename_case(rng, runner, root / "duplicate_roots"),
            )
            suite.add(
                "array-interface-hash-invalidation",
                lambda: run_array_interface_hash_case(rng, runner, root / "array_interface"),
            )
            suite.add(
                "randomized-template-cases",
                lambda: run_randomized_cases(rng, runner, root / "randomized"),
            )
            exit_code = suite.execute()
        finally:
            runner.close()
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
