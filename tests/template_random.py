#!/usr/bin/env python3

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from shutil import which

sys.dont_write_bytecode = True

from pass_fail import TestSuite, expect, expect_contains


@dataclass
class GeneratedCase:
    name: str
    root_file: str
    files: dict[str, str]
    expected_ir: list[str]


def run_command(cmd: list[str], *, cwd: Path | None = None, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def write_case(root: Path, case: GeneratedCase) -> Path:
    for relative_path, content in case.files.items():
        path = root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    return root / case.root_file


def compile_case(compiler: Path, clang: str, root: Path, input_path: Path) -> str:
    ir_path = root / f"{input_path.stem}.ll"
    obj_path = root / f"{input_path.stem}.o"

    compile_result = run_command(
        [str(compiler), "--emit", "ir", "--verify-ir", str(input_path)]
    )
    expect(
        compile_result.returncode == 0,
        f"compile failed for {input_path.name}:\n{compile_result.stderr}",
    )
    ir_path.write_text(compile_result.stdout, encoding="utf-8")

    clang_result = run_command(
        [clang, "-Werror", "-Wno-override-module", "-x", "ir", "-c", str(ir_path), "-o", str(obj_path)]
    )
    expect(
        clang_result.returncode == 0,
        f"clang rejected generated IR for {input_path.name}:\n{clang_result.stderr}",
    )
    return compile_result.stdout


def render_import_block(case_id: int, delta: int) -> tuple[dict[str, str], list[str]]:
    module_name = f"dep_{case_id}"
    return (
        {
            f"{module_name}.lo": (
                f"def imported_add_{case_id}(v i32) i32 {{\n"
                f"    ret v + {delta}\n"
                "}\n\n"
                f"struct ImportedPoint{case_id} {{\n"
                "    x i32\n"
                "}\n"
            )
        },
        [module_name],
    )


def generate_case(rng: random.Random, case_id: int) -> GeneratedCase:
    features = [
        "bool",
        "pointer",
        "float",
        "tuple",
        "array",
        "operators",
        "struct",
        "func_ptr",
        "import",
    ]
    chosen = set(rng.sample(features, rng.randint(3, 5)))

    files: dict[str, str] = {}
    imports: list[str] = []
    defs: list[str] = []
    main_body: list[str] = [f"    var score i32 = {rng.randint(1, 9)}"]
    expected_ir = ["define i32 @main"]

    def add_definition(text: str) -> None:
        defs.append(text.strip() + "\n")

    add_definition(
        f"""
def step_{case_id}(v i32) i32 {{
    ret v + {rng.randint(1, 6)}
}}
"""
    )
    main_body.append(f"    score = step_{case_id}(score)")
    expected_ir.append(f"@step_{case_id}")

    if "bool" in chosen:
        threshold = rng.randint(2, 12)
        add_definition(
            f"""
def allow_{case_id}(v i32) bool {{
    ret v > {threshold}
}}
"""
        )
        main_body.extend(
            [
                f"    if allow_{case_id}(score) {{",
                f"        score = score + {rng.randint(1, 4)}",
                "    }",
            ]
        )
        expected_ir.append(f"@allow_{case_id}")

    if "pointer" in chosen:
        add_definition(
            f"""
def pointer_bump_{case_id}(v i32) i32 {{
    var value i32 = v
    var ptr i32* = &value
    *ptr = *ptr + {rng.randint(1, 4)}
    ret value
}}
"""
        )
        main_body.append(f"    score = pointer_bump_{case_id}(score)")
        expected_ir.append(f"@pointer_bump_{case_id}")

    if "float" in chosen:
        bias_a = rng.randint(1, 4)
        bias_b = rng.randint(1, 4)
        add_definition(
            f"""
def float_mix_{case_id}(v f32) f32 {{
    var base f32 = {bias_a}.0 + {bias_b}.0
    ret v + base
}}
"""
        )
        main_body.append(f"    var sample_{case_id} f32 = float_mix_{case_id}(1.0)")
        expected_ir.append(f"@float_mix_{case_id}")

        main_body.extend(
            [
                f"    var conv_src_{case_id} i32 = {rng.randint(1, 9)}",
                f"    var conv_mid_{case_id} f32 = cast[f32](conv_src_{case_id})",
                f"    var conv_dst_{case_id} f64 = conv_mid_{case_id}",
                f"    score = score + cast[i32](conv_dst_{case_id})",
                f"    var bits_{case_id} u8[4] = conv_mid_{case_id}.tobits()",
                f"    score = score + bits_{case_id}.toi32()",
            ]
        )

    if "tuple" in chosen:
        add_definition(
            f"""
def tuple_make_{case_id}(v i32) <i32, bool> {{
    var pair <i32, bool> = (v, true)
    ret pair
}}

def tuple_keep_{case_id}(pair <i32, bool>) <i32, bool> {{
    ret pair
}}
"""
        )
        main_body.extend(
            [
                f"    var pair_{case_id} <i32, bool> = tuple_make_{case_id}(score)",
                f"    var kept_{case_id} <i32, bool> = tuple_keep_{case_id}(pair_{case_id})",
                f"    kept_{case_id}._1 = kept_{case_id}._1 + {rng.randint(1, 4)}",
                f"    if kept_{case_id}._2 {{",
                f"        score = score + kept_{case_id}._1",
                f"    }}",
            ]
        )
        expected_ir.extend([f"@tuple_make_{case_id}", f"@tuple_keep_{case_id}"])

    if "array" in chosen:
        matrix_rows = rng.randint(2, 4)
        matrix_cols = rng.randint(2, 5)
        vector_rows = rng.randint(2, 4)
        vector_cols = rng.randint(2, 5)
        add_definition(
            f"""
def array_use_{case_id}() i32 {{
    var matrix_{case_id} i32[{matrix_cols}][{matrix_rows}] = {{{{1, 2}}}}
    matrix_{case_id}(1)(1) = {rng.randint(3, 9)}
    var grid_{case_id} i32[{vector_rows}, {vector_cols}] = {{{{1}}, {{2}}}}
    grid_{case_id}(1, 1) = matrix_{case_id}(1)(1)
    ret grid_{case_id}(1, 1)
}}
"""
        )
        main_body.append(f"    score = score + array_use_{case_id}()")
        expected_ir.append(f"@array_use_{case_id}")

    if "operators" in chosen:
        add_definition(
            f"""
def operator_mix_{case_id}(a i32, b i32, flag bool) i32 {{
    var mix i32 = a % b
    mix = mix + (a << 1)
    mix = mix - (b >> 1)
    mix = mix ^ ~a
    if (mix <= a) || (flag && (b >= a)) {{
        mix = mix | (a & b)
    }}
    ret mix
}}
"""
        )
        main_body.append(
            f"    score = score + operator_mix_{case_id}(score + {rng.randint(3, 8)}, {rng.randint(2, 5)}, true)"
        )
        expected_ir.append(f"@operator_mix_{case_id}")

    if "struct" in chosen:
        add_definition(
            f"""
struct Counter{case_id} {{
    value i32

    def bump(step i32) i32 {{
        ret self.value + step
    }}
}}
"""
        )
        main_body.extend(
            [
                f"    var counter_{case_id} = Counter{case_id}(score)",
                f"    score = counter_{case_id}.bump({rng.randint(1, 5)})",
            ]
        )
        expected_ir.append(f"Counter{case_id}.bump")

    if "func_ptr" in chosen:
        add_definition(
            f"""
def callback_{case_id}(v i32) i32 {{
    ret v + {rng.randint(1, 6)}
}}

def apply_{case_id}(v i32, cb (i32: i32)) i32 {{
    ret cb(v)
}}
"""
        )
        main_body.append(f"    score = apply_{case_id}(score, callback_{case_id}&<i32>)")
        expected_ir.extend([f"@callback_{case_id}", f"@apply_{case_id}"])

    if "import" in chosen:
        module_delta = rng.randint(1, 7)
        import_files, module_names = render_import_block(case_id, module_delta)
        files.update(import_files)
        imports.extend(module_names)
        module_name = module_names[0]
        main_body.extend(
            [
                f"    score = {module_name}.imported_add_{case_id}(score)",
                f"    var imported_point_{case_id} = {module_name}.ImportedPoint{case_id}(x = score)",
                f"    score = imported_point_{case_id}.x",
            ]
        )
        expected_ir.extend([f"@{module_name}.imported_add_{case_id}", f"%{module_name}.ImportedPoint{case_id}"])

    main_body.append("    ret score")

    import_lines = [f"import {module_name}\n" for module_name in imports]
    root_source = "\n".join(import_lines + defs + ["def main() i32 {", *main_body, "}"]) + "\n"
    root_name = f"case_{case_id}.lo"
    files[root_name] = root_source

    return GeneratedCase(
        name=f"random-template-{case_id}",
        root_file=root_name,
        files=files,
        expected_ir=expected_ir,
    )


def run_case(case: GeneratedCase, compiler: Path, clang: str, root: Path) -> None:
    input_path = write_case(root, case)
    ir = compile_case(compiler, clang, root, input_path)
    for needle in case.expected_ir:
        expect_contains(ir, needle, f"{case.name} IR")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--cases", type=int, default=8)
    args = parser.parse_args()
    if which(args.clang) is None:
        fallback = "clang-18" if which("clang-18") is not None else "clang"
        args.clang = fallback

    rng = random.Random(args.seed)
    print(f"seed: {args.seed}")

    suite = TestSuite("template random")
    with tempfile.TemporaryDirectory(prefix="lona_template_random_") as temp_dir:
        root = Path(temp_dir)
        for case_id in range(args.cases):
            case = generate_case(rng, case_id)
            suite.add(case.name, lambda case=case: run_case(case, args.compiler, args.clang, root / case.name))
        return suite.execute()


if __name__ == "__main__":
    raise SystemExit(main())
