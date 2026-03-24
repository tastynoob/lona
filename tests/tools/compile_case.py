#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run_command(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile a lona case to verified IR and object output.")
    parser.add_argument("input", help="Input .lo file")
    parser.add_argument("output_ll", nargs="?", default="", help="Optional output .ll path")
    parser.add_argument("output_obj", nargs="?", default="", help="Optional output .o path")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    compiler = root / "build" / "lona-ir"
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        print(f"missing compiler binary: {compiler}", file=sys.stderr)
        return 1

    input_path = Path(args.input)
    if args.output_ll:
        ir_output = Path(args.output_ll)
    else:
        fd, path = tempfile.mkstemp(suffix=".ll", prefix="lona-case-", dir=os.environ.get("TMPDIR", "/tmp"))
        os.close(fd)
        ir_output = Path(path)

    if args.output_obj:
        obj_output = Path(args.output_obj)
    else:
        fd, path = tempfile.mkstemp(suffix=".o", prefix="lona-case-", dir=os.environ.get("TMPDIR", "/tmp"))
        os.close(fd)
        obj_output = Path(path)

    ir_result = run_command(
        [
            str(compiler),
            "--emit",
            "ir",
            "--target",
            "x86_64-unknown-linux-gnu",
            "--verify-ir",
            str(input_path),
        ],
        cwd=root,
    )
    if ir_result.returncode != 0:
        sys.stderr.write(ir_result.stderr)
        return ir_result.returncode
    ir_output.write_text(ir_result.stdout, encoding="utf-8")

    obj_result = run_command(
        [
            str(compiler),
            "--emit",
            "obj",
            "--target",
            "x86_64-unknown-linux-gnu",
            "--verify-ir",
            str(input_path),
            str(obj_output),
        ],
        cwd=root,
    )
    if obj_result.returncode != 0:
        sys.stderr.write(obj_result.stderr)
        return obj_result.returncode

    print(f"PASS {input_path} -> {ir_output} -> {obj_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

