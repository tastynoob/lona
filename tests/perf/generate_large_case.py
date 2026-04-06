#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

GENERATOR_VERSION = 4
FEATURE_BLOCK_COUNT = 3_124
EXPECTED_LINE_COUNT = 100_000
BLOCK_LINE_COUNT = 32


def require_block_length(name: str, lines: list[str]) -> list[str]:
    if len(lines) != BLOCK_LINE_COUNT:
        raise RuntimeError(
            f"{name} should render {BLOCK_LINE_COUNT} lines, got {len(lines)}"
        )
    return lines


def render_arithmetic_block(index: int) -> list[str]:
    type_name = f"Node_{index:05d}"
    apply_name = f"apply_{index:05d}"
    kernel_name = f"kernel_{index:05d}"
    seed = (index % 11) + 1
    delta = (index % 5) + 1
    return require_block_length(
        type_name,
        [
            f"struct {type_name} {{",
            "    left i32",
            "    right i32",
            "",
            "    set def bump(delta i32) i32 {",
            "        self.left = self.left + delta",
            "        self.right = self.right + delta",
            "        ret self.left + self.right",
            "    }",
            "}",
            "",
            f"def {apply_name}(ref slot i32, extra i32) i32 {{",
            "    slot = slot + extra",
            "    ret slot",
            "}",
            "",
            f"def {kernel_name}(seed i32) i32 {{",
            f"    var base i32 = -seed + {seed}",
            "    var data i32[4] = {base, 2, 3, 4}",
            "    var ptr i32[4]* = &data",
            "    (*ptr)(1) = data(0) + data(2)",
            f"    var node = {type_name}(left = data(1), right = data(3))",
            f"    var total i32 = node.bump(delta = {delta})",
            "    ref alias i32 = total",
            f"    var cb (ref i32, i32: i32) = @{apply_name}",
            "    cb(ref alias, data(0))",
            "    var pair <i32, bool> = (alias, true)",
            "    if pair._2 {",
            "        ret pair._1 + data(1)",
            "    }",
            "    ret 0",
            "}",
        ],
    )


def render_loop_block(index: int) -> list[str]:
    type_name = f"LoopPair_{index:05d}"
    adjust_name = f"adjust_{index:05d}"
    kernel_name = f"kernel_{index:05d}"
    return require_block_length(
        type_name,
        [
            f"struct {type_name} {{",
            "    left i32",
            "    right i32",
            "",
            "    set def mix(scale i32) i32 {",
            "        self.left = self.left + scale",
            "        self.right = self.right ^ scale",
            "        ret self.left | self.right",
            "    }",
            "}",
            "",
            f"def {adjust_name}(v i32) i32 {{",
            "    ret (v << 1) + 1",
            "}",
            "",
            f"def {kernel_name}(limit i32) i32 {{",
            "    var i i32 = 0",
            f"    var acc i32 = {adjust_name}(limit)",
            "    var pair <i32, bool> = (limit, limit > 0)",
            "    for i < 3 {",
            "        i += 1",
            "        acc = acc + i",
            "    }",
            f"    var item = {type_name}(left = acc, right = ~limit)",
            "    var score i32 = item.mix(scale = i)",
            "    if pair._2 && ((score > 0) || (limit == 0)) {",
            "        ret score + pair._1",
            "    }",
            "    acc = score & 7",
            "    ret acc",
            "}",
            "",
        ],
    )


def render_pointer_block(index: int) -> list[str]:
    type_name = f"Cell_{index:05d}"
    patch_name = f"patch_{index:05d}"
    read_name = f"read_{index:05d}"
    kernel_name = f"kernel_{index:05d}"
    return require_block_length(
        type_name,
        [
            f"struct {type_name} {{",
            "    set value i32",
            "",
            "    set def store(next i32) i32 {",
            "        self.value = next",
            "        ret self.value",
            "    }",
            "}",
            "",
            f"def {patch_name}(ref slot i32, delta i32) i32 {{",
            "    slot = slot + delta",
            "    ret slot",
            "}",
            "",
            f"def {read_name}(v i32) i32 {{",
            "    ret v",
            "}",
            "",
            f"def {kernel_name}(seed i32) i32 {{",
            "    var local i32 = seed + 1",
            "    var ptr i32* = &local",
            "    *ptr = *ptr + 2",
            f"    var cell = {type_name}(value = local)",
            "    var wrote i32 = cell.store(next = *ptr)",
            "    ref alias i32 = cell.value",
            f"    var cb (ref i32, i32: i32) = @{patch_name}",
            "    cb(ref alias, wrote)",
            "    var arr i32[2] = {alias, local}",
            "    var arr_ptr i32[2]* = &arr",
            "    (*arr_ptr)(1) = (*arr_ptr)(0) + wrote",
            f"    ret {read_name}(cell.value + arr(1))",
            "}",
        ],
    )


def render_conversion_block(index: int) -> list[str]:
    type_name = f"Window_{index:05d}"
    fold_name = f"lift_{index:05d}"
    kernel_name = f"kernel_{index:05d}"
    return require_block_length(
        type_name,
        [
            f"struct {type_name} {{",
            "    left i32",
            "    right i32",
            "",
            "    set def fold(extra i32) i32 {",
            "        self.left = self.left + extra",
            "        ret self.left + self.right",
            "    }",
            "}",
            "",
            f"def {fold_name}(v i32) i32 {{",
            "    ret v + 1",
            "}",
            f"def {kernel_name}(seed i32) i32 {{",
            "    var sample f32 = cast[f32](seed)",
            "    sample = sample + 1.0",
            "    var bits u8[4] = sample.tobits()",
            "    sample = bits.tof32()",
            "    var tupled <i32, bool> = (seed, sample >= 1.0)",
            f"    var win = {type_name}(left = tupled._1, right = 2)",
            "    var value i32 = win.fold(extra = 2)",
            "    if tupled._2 {",
            "        value = value + win.left",
            "    } else {",
            "        value = value - 1",
            "    }",
            "    var matrix i32[2][3] = {{1}, {2}}",
            "    matrix(1)(2) = value",
            "    var grid i32[3, 2] = {{1}, {2}}",
            "    grid(1, 1) = matrix(1)(2)",
            f"    ret {fold_name}(grid(1, 1))",
            "}",
        ],
    )


def render_feature_block(index: int) -> list[str]:
    block_kind = index % 4
    if block_kind == 0:
        return render_arithmetic_block(index)
    if block_kind == 1:
        return render_loop_block(index)
    if block_kind == 2:
        return render_pointer_block(index)
    return render_conversion_block(index)


def render_footer() -> list[str]:
    return require_block_length(
        "footer",
        [
            "def main() i32 {",
            "    var acc i32 = kernel_00000(1)",
            "    acc = acc + kernel_00001(2)",
            "    acc = acc + kernel_00002(3)",
            "    acc = acc + kernel_00003(4)",
            "    acc = acc + kernel_00500(5)",
            "    acc = acc + kernel_01000(6)",
            "    acc = acc + kernel_01500(7)",
            "    acc = acc + kernel_02000(8)",
            "    acc = acc + kernel_02500(9)",
            "    acc = acc + kernel_03000(10)",
            "    ret acc",
            "}",
            "",
            "var seed0 i32 = main()",
            "var seed1 i32 = seed0 + 1",
            "var seed2 i32 = seed1 + 1",
            "var seed3 i32 = seed2 + 1",
            "var seed4 i32 = seed3 + 1",
            "var seed5 i32 = seed4 + 1",
            "var seed6 i32 = seed5 + 1",
            "var seed7 i32 = seed6 + 1",
            "var seed8 i32 = seed7 + 1",
            "var seed9 i32 = seed8 + 1",
            "var seed10 i32 = seed9 + 1",
            "var seed11 i32 = seed10 + 1",
            "var seed12 i32 = seed11 + 1",
            "var seed13 i32 = seed12 + 1",
            "var seed14 i32 = seed13 + 1",
            "var seed15 i32 = seed14 + 1",
            "var seed16 i32 = seed15 + 1",
            "var sink i32 = seed16",
        ],
    )


def render_source() -> str:
    lines: list[str] = []
    for index in range(FEATURE_BLOCK_COUNT):
        lines.extend(render_feature_block(index))
    lines.extend(render_footer())

    if len(lines) != EXPECTED_LINE_COUNT:
        raise RuntimeError(
            f"expected {EXPECTED_LINE_COUNT} lines, got {len(lines)}"
        )
    return "\n".join(lines) + "\n"


def build_metadata(source: str) -> dict[str, object]:
    encoded = source.encode("utf-8")
    return {
        "generator_version": GENERATOR_VERSION,
        "feature_block_count": FEATURE_BLOCK_COUNT,
        "line_count": len(source.splitlines()),
        "byte_count": len(encoded),
        "sha256": hashlib.sha256(encoded).hexdigest(),
    }


def validate_manifest(metadata: dict[str, object], manifest_path: Path) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for key in ("generator_version", "feature_block_count", "line_count", "sha256"):
        if manifest.get(key) != metadata.get(key):
            raise RuntimeError(
                f"manifest mismatch for {key}: expected {manifest.get(key)!r}, got {metadata.get(key)!r}"
            )


def write_case(output: Path, manifest_path: Path | None = None) -> dict[str, object]:
    source = render_source()
    metadata = build_metadata(source)
    if metadata["line_count"] != EXPECTED_LINE_COUNT:
        raise RuntimeError(
            f"expected {EXPECTED_LINE_COUNT} source lines, got {metadata['line_count']}"
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(source, encoding="utf-8")

    if manifest_path is not None:
        validate_manifest(metadata, manifest_path)
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--print-metadata", action="store_true")
    args = parser.parse_args()

    metadata = write_case(args.output, args.manifest)
    if args.print_metadata:
        print(f"generator-version: {metadata['generator_version']}")
        print(f"feature-block-count: {metadata['feature_block_count']}")
        print(f"source-lines: {metadata['line_count']}")
        print(f"source-bytes: {metadata['byte_count']}")
        print(f"source-sha256: {metadata['sha256']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
