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

    def compile(
        self,
        input_path: Path,
        verify_ir: bool = True,
        output_mode: str = "llvm_ir",
        lto: str = "off",
        include_paths: list[Path] | None = None,
    ) -> dict:
        command = {
            "command": "compile",
            "input": str(input_path),
            "output_mode": output_mode,
            "verify_ir": verify_ir,
            "lto": lto,
        }
        if include_paths is not None:
            command["include_paths"] = [str(path) for path in include_paths]
        return self._send(command)

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


def expect_object_compile_ok(
    result: dict,
    compiled: int,
    reused: int,
    emitted_bitcode: int,
    reused_bitcode: int,
    emitted_objects: int,
    reused_objects: int,
) -> None:
    expect_equal(result["exit_code"], 0, "compile exit code")
    expect_equal(result["stderr"], "", "diagnostics")
    expect(result["stdout_size"] > 0, "expected non-empty object output")
    stats = result["stats"]
    expect_equal(stats["compiled_modules"], compiled, "compiled module count")
    expect_equal(stats["reused_modules"], reused, "reused module count")
    expect_equal(
        stats["emitted_module_bitcode"], emitted_bitcode, "emitted module bitcode count"
    )
    expect_equal(
        stats["reused_module_bitcode"], reused_bitcode, "reused module bitcode count"
    )
    expect_equal(
        stats["emitted_module_objects"], emitted_objects, "emitted module object count"
    )
    expect_equal(
        stats["reused_module_objects"], reused_objects, "reused module object count"
    )


def expect_bundle_compile_ok(
    result: dict,
    compiled: int,
    reused: int,
    emitted_objects: int,
    reused_objects: int,
) -> None:
    expect_equal(result["exit_code"], 0, "compile exit code")
    expect_equal(result["stderr"], "", "diagnostics")
    expect(
        "format\tlona-artifact-bundle-v1" in result["stdout"],
        "expected bundle manifest output",
    )
    expect("kind\tobj" in result["stdout"], "expected object bundle manifest kind")
    stats = result["stats"]
    expect_equal(stats["compiled_modules"], compiled, "compiled module count")
    expect_equal(stats["reused_modules"], reused, "reused module count")
    expect_equal(stats["emitted_module_bitcode"], 0, "emitted module bitcode count")
    expect_equal(stats["reused_module_bitcode"], 0, "reused module bitcode count")
    expect_equal(
        stats["emitted_module_objects"], emitted_objects, "emitted module object count"
    )
    expect_equal(
        stats["reused_module_objects"], reused_objects, "reused module object count"
    )


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
        "    ret Box(data = matrix)\n"
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


def trait_dependency_text(
    seed_value: int, *, extra_arg: bool = False, include_impl: bool = True
) -> str:
    method_sig = "def hash(step i32) i32" if extra_arg else "def hash() i32"
    method_def = (
        "    def hash(step i32) i32 {\n"
        "        ret self.value + step\n"
        "    }\n"
        if extra_arg
        else "    def hash() i32 {\n"
        "        ret self.value + 1\n"
        "    }\n"
    )
    text = (
        "trait Hash {\n"
        f"    {method_sig}\n"
        "}\n\n"
        "struct Point {\n"
        "    value i32\n\n"
        f"{method_def}"
        "}\n\n"
    )
    if include_impl:
        text += "impl Point: Hash\n\n"
    text += (
        "def make() Point {\n"
        f"    ret Point(value = {seed_value})\n"
        "}\n"
    )
    return text


def trait_dyn_program_text(module_name: str, *, extra_arg: bool = False,
                           arg_value: int = 2) -> str:
    call = f"h.hash({arg_value})" if extra_arg else "h.hash()"
    return (
        f"import {module_name}\n\n"
        "def main() i32 {\n"
        f"    var point {module_name}.Point = {module_name}.make()\n"
        f"    var h {module_name}.Hash dyn = cast[{module_name}.Hash dyn](&point)\n"
        f"    ret {call}\n"
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


def run_trait_dyn_signature_interface_hash_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    seed_value = rng.randint(30, 70)
    arg_value = rng.randint(2, 9)

    write_file(dep_path, trait_dependency_text(seed_value, extra_arg=False))
    write_file(app_path, trait_dyn_program_text("dep", extra_arg=False))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, trait_dependency_text(seed_value, extra_arg=True))
    result = runner.compile(app_path)
    expect_compile_failure(
        result,
        1,
        0,
        "call argument count mismatch: expected 1, got 0",
    )

    write_file(
        app_path,
        trait_dyn_program_text("dep", extra_arg=True, arg_value=arg_value),
    )
    result = runner.compile(app_path)
    expect_compile_ok(result, compiled=1, reused=1)
    expect(
        "call i32 %trait.slot(ptr %trait.data, i32 " in result["stdout"],
        "expected trait dyn signature change to force recompilation of the caller",
    )


def run_trait_dyn_impl_header_interface_hash_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    seed_value = rng.randint(30, 70)

    write_file(dep_path, trait_dependency_text(seed_value, include_impl=True))
    write_file(app_path, trait_dyn_program_text("dep", extra_arg=False))
    expect_compile_ok(runner.compile(app_path), compiled=2, reused=0)

    write_file(dep_path, trait_dependency_text(seed_value, include_impl=False))
    result = runner.compile(app_path)
    expect_compile_failure(
        result,
        1,
        0,
        "does not implement trait `dep.Hash`",
    )

    write_file(dep_path, trait_dependency_text(seed_value, include_impl=True))
    result = runner.compile(app_path)
    expect_compile_ok(result, compiled=1, reused=1)
    expect(
        "@__lona_trait_witness__dep_2eHash__dep_2ePoint" in result["stdout"],
        "expected restoring the visible impl header to rebuild the caller dyn witness path",
    )


def run_same_module_generic_runtime_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    app_path = root / "app.lo"
    first_bonus = rng.randint(1, 4)
    second_bonus = first_bonus + rng.randint(1, 4)
    point_value = rng.randint(20, 40)

    write_file(
        app_path,
        (
            "trait Hash {\n"
            "    def hash() i32\n"
            "}\n\n"
            "struct Point {\n"
            "    value i32\n\n"
            "    def hash() i32 {\n"
            "        ret self.value + 1\n"
            "    }\n"
            "}\n\n"
            "impl Point: Hash\n\n"
            "struct Box[T] {\n"
            "    value T\n\n"
            "    def get() T {\n"
            "        ret self.value\n"
            "    }\n\n"
            "    def hash() i32 {\n"
            f"        ret Hash.hash(&self.value) + {first_bonus}\n"
            "    }\n"
            "}\n\n"
            "impl[T Hash] Box[T]: Hash\n\n"
            "def id[T](value T) T {\n"
            "    ret value\n"
            "}\n\n"
            "def hash_one[T Hash](value T) i32 {\n"
            "    ret Hash.hash(&value)\n"
            "}\n\n"
            "def main() i32 {\n"
            f"    var point Point = Point(value = {point_value})\n"
            "    var box Box[Point] = Box[Point](value = point)\n"
            "    var out i32 = id[i32](1) + id(2)\n"
            "    out = out + box.get().hash()\n"
            "    out = out + hash_one(point)\n"
            "    ret out + Hash.hash(&box)\n"
            "}\n"
        ),
    )
    first = runner.compile(app_path)
    expect_compile_ok(first, compiled=1, reused=0)
    expect(
        "id__inst__i32" in first["stdout"],
        "expected same-module generic function instantiation in incremental smoke",
    )
    expect(
        "Box[" in first["stdout"],
        "expected concrete applied generic struct layout in incremental smoke",
    )

    second = runner.compile(app_path)
    expect_compile_ok(second, compiled=0, reused=1)

    write_file(
        app_path,
        (
            "trait Hash {\n"
            "    def hash() i32\n"
            "}\n\n"
            "struct Point {\n"
            "    value i32\n\n"
            "    def hash() i32 {\n"
            "        ret self.value + 1\n"
            "    }\n"
            "}\n\n"
            "impl Point: Hash\n\n"
            "struct Box[T] {\n"
            "    value T\n\n"
            "    def get() T {\n"
            "        ret self.value\n"
            "    }\n\n"
            "    def hash() i32 {\n"
            f"        ret Hash.hash(&self.value) + {second_bonus}\n"
            "    }\n"
            "}\n\n"
            "impl[T Hash] Box[T]: Hash\n\n"
            "def id[T](value T) T {\n"
            "    ret value\n"
            "}\n\n"
            "def hash_one[T Hash](value T) i32 {\n"
            "    ret Hash.hash(&value)\n"
            "}\n\n"
            "def main() i32 {\n"
            f"    var point Point = Point(value = {point_value})\n"
            "    var box Box[Point] = Box[Point](value = point)\n"
            "    var out i32 = id[i32](1) + id(2)\n"
            "    out = out + box.get().hash()\n"
            "    out = out + hash_one(point)\n"
            "    ret out + Hash.hash(&box)\n"
            "}\n"
        ),
    )
    third = runner.compile(app_path)
    expect_compile_ok(third, compiled=1, reused=0)


def run_imported_generic_owner_context_invalidation_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    helper_path = root / "helper.lo"
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"

    write_file(
        helper_path,
        (
            "struct Box[T] {\n"
            "    value T\n"
            "}\n"
        ),
    )
    write_file(
        dep_path,
        (
            "import helper\n\n"
            "def make_helper_ptr() helper.Box[i32]* {\n"
            "    ret null\n"
            "}\n\n"
            "def take_helper_ptr[T](value helper.Box[T]*) helper.Box[T]* {\n"
            "    ret value\n"
            "}\n"
        ),
    )
    write_file(
        app_path,
        (
            "import dep\n\n"
            "def main() i32 {\n"
            "    var out = dep.take_helper_ptr(dep.make_helper_ptr())\n"
            "    ret 0\n"
            "}\n"
        ),
    )

    first = runner.compile(app_path, output_mode="object_bundle")
    expect_bundle_compile_ok(first, compiled=3, reused=0, emitted_objects=3, reused_objects=0)

    second = runner.compile(app_path, output_mode="object_bundle")
    expect_bundle_compile_ok(second, compiled=0, reused=3, emitted_objects=0, reused_objects=3)

    write_file(
        helper_path,
        (
            "struct Box[T] {\n"
            "    value T\n"
            "}\n\n"
            "struct Marker {\n"
            f"    value i32\n"
            "}\n"
        ),
    )

    third = runner.compile(app_path, output_mode="object_bundle")
    expect_bundle_compile_ok(third, compiled=3, reused=0, emitted_objects=3, reused_objects=0)


def run_randomized_cases(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    case_count = 3
    for index in range(case_count):
        case_root = root / f"random_case_{index}"
        run_body_vs_interface_case(rng, runner, case_root)


def run_module_object_reuse_case(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    first_delta = rng.randint(2, 8)
    second_delta = first_delta + rng.randint(3, 9)
    argument = rng.randint(1, 10)

    write_file(dep_path, dependency_text(first_delta))
    write_file(app_path, program_text("dep", argument))
    expect_bundle_compile_ok(
        runner.compile(app_path, output_mode="object_bundle"),
        compiled=2,
        reused=0,
        emitted_objects=2,
        reused_objects=0,
    )

    expect_bundle_compile_ok(
        runner.compile(app_path, output_mode="object_bundle"),
        compiled=0,
        reused=2,
        emitted_objects=0,
        reused_objects=2,
    )

    write_file(dep_path, dependency_text(second_delta))
    expect_bundle_compile_ok(
        runner.compile(app_path, output_mode="object_bundle"),
        compiled=1,
        reused=1,
        emitted_objects=1,
        reused_objects=1,
    )


def run_module_bitcode_reuse_case(rng: random.Random, runner: SessionRunner, root: Path) -> None:
    dep_path = root / "dep.lo"
    app_path = root / "app.lo"
    first_delta = rng.randint(2, 8)
    second_delta = first_delta + rng.randint(3, 9)
    argument = rng.randint(1, 10)

    write_file(dep_path, dependency_text(first_delta))
    write_file(app_path, program_text("dep", argument))
    expect_object_compile_ok(
        runner.compile(app_path, output_mode="linked_object", lto="full"),
        compiled=2,
        reused=0,
        emitted_bitcode=2,
        reused_bitcode=0,
        emitted_objects=0,
        reused_objects=0,
    )

    expect_object_compile_ok(
        runner.compile(app_path, output_mode="linked_object", lto="full"),
        compiled=0,
        reused=2,
        emitted_bitcode=0,
        reused_bitcode=2,
        emitted_objects=0,
        reused_objects=0,
    )

    write_file(dep_path, dependency_text(second_delta))
    expect_object_compile_ok(
        runner.compile(app_path, output_mode="linked_object", lto="full"),
        compiled=1,
        reused=1,
        emitted_bitcode=1,
        reused_bitcode=1,
        emitted_objects=0,
        reused_objects=0,
    )


def run_root_vs_dependency_entry_role_case(
    rng: random.Random, runner: SessionRunner, root: Path
) -> None:
    leaf_path = root / "leaf.lo"
    mid_path = root / "mid.lo"
    app_path = root / "app.lo"
    delta = rng.randint(2, 9)

    write_file(
        leaf_path,
        (
            "def base() i32 {\n"
            f"    ret {delta}\n"
            "}\n"
        ),
    )
    write_file(
        mid_path,
        (
            "import leaf\n\n"
            "ret leaf.base()\n"
        ),
    )
    write_file(
        app_path,
        (
            "import mid\n\n"
            "ret 0\n"
        ),
    )

    standalone_mid = runner.compile(mid_path)
    expect_compile_ok(standalone_mid, compiled=2, reused=0)
    expect(
        "define i32 @__lona_main__()" in standalone_mid["stdout"],
        "expected standalone mid compile to synthesize __lona_main__",
    )

    imported_mid = runner.compile(app_path)
    expect_compile_ok(imported_mid, compiled=2, reused=1)
    expect_equal(
        imported_mid["stdout"].count("define i32 @__lona_main__()"),
        1,
        "expected importing app compile to contain exactly one language entry",
    )

    runner.reset()

    imported_first = runner.compile(app_path)
    expect_compile_ok(imported_first, compiled=3, reused=0)

    standalone_after_import = runner.compile(mid_path)
    expect_compile_ok(standalone_after_import, compiled=1, reused=1)
    expect(
        "define i32 @__lona_main__()" in standalone_after_import["stdout"],
        "expected standalone recompile to recover root language entry",
    )


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
                "trait-dyn-signature-interface-hash-invalidation",
                lambda: run_trait_dyn_signature_interface_hash_case(
                    rng, runner, root / "trait_dyn_signature_interface"
                ),
            )
            suite.add(
                "trait-dyn-impl-header-interface-hash-invalidation",
                lambda: run_trait_dyn_impl_header_interface_hash_case(
                    rng, runner, root / "trait_dyn_impl_header_interface"
                ),
            )
            suite.add(
                "same-module-generic-runtime",
                lambda: run_same_module_generic_runtime_case(
                    rng, runner, root / "same_module_generic_runtime"
                ),
            )
            suite.add(
                "imported-generic-owner-context-invalidation",
                lambda: run_imported_generic_owner_context_invalidation_case(
                    rng, runner, root / "imported_generic_owner_context"
                ),
            )
            suite.add(
                "randomized-template-cases",
                lambda: run_randomized_cases(rng, runner, root / "randomized"),
            )
            suite.add(
                "module-object-reuse",
                lambda: run_module_object_reuse_case(
                    rng, runner, root / "module_object_reuse"
                ),
            )
            suite.add(
                "module-bitcode-reuse",
                lambda: run_module_bitcode_reuse_case(
                    rng, runner, root / "module_bitcode_reuse"
                ),
            )
            suite.add(
                "module-entry-role-reuse",
                lambda: run_root_vs_dependency_entry_role_case(
                    rng, runner, root / "module_entry_role_reuse"
                ),
            )
            exit_code = suite.execute()
        finally:
            runner.close()
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
