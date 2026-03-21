#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${OUT_DIR:-/tmp/lona-ai-tests}"
MODEL="${CODEX_MODEL:-}"
FOCUS="${*:-generate 10 positive cases and 10 diagnostic case that cover different documented syntax areas}"
RESULT_FILE="$OUT_DIR/ai_test.result.txt"
LOG_FILE="$OUT_DIR/ai_test.full.log"

mkdir -p "$OUT_DIR"
rm -f "$RESULT_FILE" "$LOG_FILE"

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
- Final response must be concise and only include:
  - overall result
  - generated test files
  - syntax areas covered
  - diagnostic case result
- Do not include file editing narration, shell command logs, or intermediate progress.

Additional focus:
$FOCUS
EOF
)

CMD=(codex exec --full-auto --color never --output-last-message "$RESULT_FILE" -C "$ROOT" "$PROMPT")

if [ -n "$MODEL" ]; then
    CMD=(codex exec --full-auto --color never --output-last-message "$RESULT_FILE" -m "$MODEL" -C "$ROOT" "$PROMPT")
fi

if "${CMD[@]}" >"$LOG_FILE" 2>&1; then
    if [ -s "$RESULT_FILE" ]; then
        cat "$RESULT_FILE"
        exit 0
    fi
    echo "ai_test finished but produced no final summary. Full log: $LOG_FILE" >&2
    exit 1
fi

if [ -s "$RESULT_FILE" ]; then
    cat "$RESULT_FILE"
fi
echo "ai_test failed. Full log: $LOG_FILE" >&2
exit 1
