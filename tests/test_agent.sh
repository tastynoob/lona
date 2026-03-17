#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${OUT_DIR:-/tmp/lona-ai-tests}"
MODEL="${CODEX_MODEL:-}"
FOCUS="${*:-generate 3 positive cases and 1 diagnostic case that cover different documented syntax areas}"

mkdir -p "$OUT_DIR"

PROMPT=$(cat <<EOF
Follow the workflow in $ROOT/tests/test_skill.md.

Repository root: $ROOT
Output directory for generated tests: $OUT_DIR

Requirements:
- Read docs/README.md and tests/README.md first.
- Generate small Lona test cases under $OUT_DIR.
- Use bash tests/tools/compile_case.sh for every positive case.
- Use bash tests/tools/expect_diag.sh for every negative case.
- Do not modify existing tracked source files unless explicitly needed for adding curated tests.
- Report which syntax areas were covered and which commands were run.

Additional focus:
$FOCUS
EOF
)

if [ -n "$MODEL" ]; then
    exec codex exec --full-auto --sandbox workspace-write --ask-for-approval never -m "$MODEL" -C "$ROOT" "$PROMPT"
fi

exec codex exec --full-auto --sandbox workspace-write --ask-for-approval never -C "$ROOT" "$PROMPT"
