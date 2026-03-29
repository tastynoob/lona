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
    focus = (
        " ".join(argv)
        if argv
        else (
            "generate 8-12 positive cases and 8-12 diagnostic cases that focus on "
            "edge paths, adversarial syntax combinations, boundary values, and weird "
            "but documented feature interactions that are not already obviously covered "
            "by the existing acceptance tests"
        )
    )
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
- Read docs/README.md, tests/README.md, tests/acceptance/README.md, and skim tests/acceptance/**/*.py first.
- Generate small Lona test cases under {out_dir}.
- Prefer cases that are intentionally awkward:
  - boundary numeric literals, especially signed mins/maxes, mixed prefixes/suffixes, separators
  - nested pointer/indexable pointer/array/const combinations
  - odd mixtures of tuple, struct, selector, call, named arg, cast, and control-flow syntax
  - parse-valid but semantically fragile combinations
  - multi-feature interactions that a normal hand-written happy-path test would skip
- Avoid spending budget on plain happy-path duplicates of existing acceptance coverage.
- Positive cases should still be minimal, but should try to stress edge behavior rather than basic syntax.
- Negative cases should prefer diagnostics that are easy to assert via stable substrings.
- If a generated case exposes an unexpected compiler bug, keep the reproducer and clearly label it.
- Use python3 tests/tools/compile_case.py for every positive case.
- Use python3 tests/tools/expect_diag.py for every negative case.
- Do not modify existing tracked source files unless explicitly needed for adding curated tests.
- Final response must be concise and only include:
  - overall result
  - generated test files
  - syntax areas covered
  - diagnostic case result
  - any unexpected bug reproducers
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
