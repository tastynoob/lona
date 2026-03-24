#!/usr/bin/env python3

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parents[1]
    out_dir = Path(os.environ.get("OUT_DIR", "/tmp/lona-ai-tests"))
    model = os.environ.get("CODEX_MODEL", "")
    focus = " ".join(argv) if argv else "generate 10 positive cases and 10 diagnostic case that cover different documented syntax areas"
    result_file = out_dir / "ai_test.result.txt"
    log_file = out_dir / "ai_test.full.log"

    out_dir.mkdir(parents=True, exist_ok=True)
    if result_file.exists():
        result_file.unlink()
    if log_file.exists():
        log_file.unlink()

    prompt = f"""Follow the workflow in {root / 'tests' / 'test_skill.md'}.

Repository root: {root}
Output directory for generated tests: {out_dir}

Requirements:
- Read docs/README.md and tests/README.md first.
- Generate small Lona test cases under {out_dir}.
- Use python3 tests/tools/compile_case.py for every positive case.
- Use python3 tests/tools/expect_diag.py for every negative case.
- Do not modify existing tracked source files unless explicitly needed for adding curated tests.
- Final response must be concise and only include:
  - overall result
  - generated test files
  - syntax areas covered
  - diagnostic case result
- Do not include file editing narration, shell command logs, or intermediate progress.

Additional focus:
{focus}
"""

    cmd = [
        "codex",
        "exec",
        "--full-auto",
        "--color",
        "never",
        "--output-last-message",
        str(result_file),
        "-C",
        str(root),
        prompt,
    ]
    if model:
        cmd = [
            "codex",
            "exec",
            "--full-auto",
            "--color",
            "never",
            "--output-last-message",
            str(result_file),
            "-m",
            model,
            "-C",
            str(root),
            prompt,
        ]

    with log_file.open("w", encoding="utf-8") as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, text=True, check=False)

    if result.returncode == 0:
        if result_file.exists() and result_file.stat().st_size > 0:
            sys.stdout.write(result_file.read_text(encoding="utf-8"))
            return 0
        print(f"ai_test finished but produced no final summary. Full log: {log_file}", file=sys.stderr)
        return 1

    if result_file.exists() and result_file.stat().st_size > 0:
        sys.stdout.write(result_file.read_text(encoding="utf-8"))
    print(f"ai_test failed. Full log: {log_file}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

