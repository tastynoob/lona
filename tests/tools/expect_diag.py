#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Assert that a lona input fails with a diagnostic substring.")
    parser.add_argument("input", help="Input .lo file")
    parser.add_argument("substring", help="Expected diagnostic substring")
    parser.add_argument("output_file", nargs="?", default="", help="Optional captured output path")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    compiler = root / "build" / "lona-ir"
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        print(f"missing compiler binary: {compiler}", file=sys.stderr)
        return 1

    if args.output_file:
        output_path = Path(args.output_file)
    else:
        fd, path = tempfile.mkstemp(suffix=".txt", prefix="lona-diag-", dir=os.environ.get("TMPDIR", "/tmp"))
        os.close(fd)
        output_path = Path(path)

    result = subprocess.run(
        [str(compiler), "--emit", "ir", args.input],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output_path.write_text(result.stdout, encoding="utf-8")

    if result.returncode == 0:
        print(f"expected compile failure for {args.input}", file=sys.stderr)
        return 1
    if args.substring not in result.stdout:
        sys.stderr.write(result.stdout)
        return 1

    print(f"PASS {args.input} produced expected diagnostic")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

