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


def expect_compile_failure(result: dict, compiled: int, reused: int, needle: str) -> None:
    expect(result["exit_code"] != 0, "expected compile failure")
    expect(needle in result["stderr"], f"expected diagnostic containing {needle!r}")
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


def method_dependency_text(dx_name: str, dy_name: str) -> str:
    return (
        "struct Vec2 {\n"
        "    x i32\n"
        "    y i32\n\n"
        f"    def add({dx_name} i32, {dy_name} i32) i32 {{\n"
        f"        ret self.x + self.y + {dx_name} + {dy_name}\n"
        "    }\n"
        "}\n"
    )


def method_program_text(module_name: str, dx_name: str, dy_name: str) -> str:
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        f"    var v = {module_name}.Vec2(x = 1, y = 2)\n"
        f"    ret v.add({dx_name} = 3, {dy_name} = 4)\n"
        "}\n"
    )


def indexable_pointer_dependency_text(type_name: str) -> str:
    return (
        f"def first(ptr {type_name}[*]) {type_name} {{\n"
        "    ret ptr(0)\n"
        "}\n"
    )


def indexable_pointer_program_text(module_name: str) -> str:
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        "    var raw u8[1] = {1}\n"
        "    var ptr u8* = &raw(0)\n"
        f"    ret {module_name}.first(ptr)\n"
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


def run_named_method_interface_hash_case(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    first_dx = f"dx_{rng.randint(10, 99)}"
    first_dy = f"dy_{rng.randint(10, 99)}"
    second_dx = f"left_{rng.randint(10, 99)}"
    second_dy = f"right_{rng.randint(10, 99)}"

    write_file(dep_path, method_dependency_text(first_dx, first_dy))
    write_file(app_path, method_program_text("dep", first_dx, first_dy))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, method_dependency_text(second_dx, second_dy))
    result = runner.compile(app_path)
    expect_compile_failure(
        result,
        1,
        0,
        f"unknown parameter `{first_dx}` for function call",
    )


def run_indexable_pointer_interface_hash_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"

    write_file(dep_path, indexable_pointer_dependency_text("u8"))
    write_file(app_path, indexable_pointer_program_text("dep"))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, indexable_pointer_dependency_text("i32"))
    result = runner.compile(app_path)
    expect_compile_failure(
        result,
        1,
        0,
        "call argument type mismatch at index 0: expected i32[*], got u8*",
    )


def ref_signature_dependency_text(use_ref: bool) -> str:
    binding = "ref v i32" if use_ref else "v i32"
    body = (
        "    v = v + 1\n"
        "    ret v\n"
        if use_ref
        else "    ret v + 1\n"
    )
    return (
        f"def set7({binding}) i32 {{\n"
        f"{body}"
        "}\n"
    )


def ref_signature_program_text(module_name: str, use_ref: bool) -> str:
    call_arg = "ref x" if use_ref else "x"
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        "    var x i32 = 1\n"
        f"    ret {module_name}.set7({call_arg})\n"
        "}\n"
    )


def constructor_dependency_text(field_name: str) -> str:
    return (
        "struct Point {\n"
        f"    {field_name} i32\n"
        "}\n"
    )


def constructor_program_text(module_name: str, field_name: str) -> str:
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        f"    var p = {module_name}.Point({field_name} = 1)\n"
        f"    ret p.{field_name}\n"
        "}\n"
    )


def run_ref_signature_interface_hash_case(rng: random.Random, runner: SessionRunner,
                                          root: Path) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"

    write_file(dep_path, ref_signature_dependency_text(False))
    write_file(app_path, ref_signature_program_text("dep", False))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, ref_signature_dependency_text(True))
    result = runner.compile(app_path)
    expect_compile_failure(
        result,
        1,
        0,
        "reference parameter `v` must be passed with `ref`",
    )

    write_file(app_path, ref_signature_program_text("dep", True))
    result = runner.compile(app_path)
    expect_compile_ok(result, compiled=1, reused=1)
    expect("call i32 @dep.set7(ptr " in result["stdout"],
           "expected ref signature change to force recompilation of the caller")


def run_imported_constructor_interface_hash_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    first_field = f"x_{rng.randint(10, 99)}"
    second_field = f"y_{rng.randint(10, 99)}"

    write_file(dep_path, constructor_dependency_text(first_field))
    write_file(app_path, constructor_program_text("dep", first_field))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, constructor_dependency_text(second_field))
    result = runner.compile(app_path)
    expect_compile_failure(
        result,
        1,
        0,
        f"unknown field `{first_field}` for constructor `dep.Point`",
    )

    write_file(app_path, constructor_program_text("dep", second_field))
    expect_compile_ok(runner.compile(app_path), compiled=1, reused=1)


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
                "named-method-interface-hash-invalidation",
                lambda: run_named_method_interface_hash_case(
                    rng, runner, root / "named_method_interface"
                ),
            )
            suite.add(
                "indexable-pointer-interface-hash-invalidation",
                lambda: run_indexable_pointer_interface_hash_case(
                    rng, runner, root / "indexable_pointer_interface"
                ),
            )
            suite.add(
                "ref-signature-interface-hash-invalidation",
                lambda: run_ref_signature_interface_hash_case(
                    rng, runner, root / "ref_signature_interface"
                ),
            )
            suite.add(
                "imported-constructor-interface-hash-invalidation",
                lambda: run_imported_constructor_interface_hash_case(
                    rng, runner, root / "imported_constructor_interface"
                ),
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
