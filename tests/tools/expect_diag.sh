#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
INPUT="${1:?usage: expect_diag.sh <input.lo> <substring>}"
SHIFTED=0
EXPECTED="${2:?usage: expect_diag.sh <input.lo> <substring>}"
OUTPUT_FILE="${3:-}"

if [ ! -x "$BIN" ]; then
    echo "missing compiler binary: $BIN" >&2
    exit 1
fi

if [ -z "$OUTPUT_FILE" ]; then
    OUTPUT_FILE="$(mktemp "${TMPDIR:-/tmp}/lona-diag-XXXXXX.txt")"
fi

if "$BIN" --emit ir "$INPUT" >"$OUTPUT_FILE" 2>&1; then
    echo "expected compile failure for $INPUT" >&2
    exit 1
fi

grep -Fq "$EXPECTED" "$OUTPUT_FILE"
printf 'PASS %s produced expected diagnostic\n' "$INPUT"
