#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
INPUT="${1:?usage: compile_case.sh <input.lo> [output.ll] [output.o]}"
IR_OUTPUT="${2:-}"
OBJ_OUTPUT="${3:-}"

if [ ! -x "$BIN" ]; then
    echo "missing compiler binary: $BIN" >&2
    exit 1
fi

if [ -z "$IR_OUTPUT" ]; then
    IR_OUTPUT="$(mktemp "${TMPDIR:-/tmp}/lona-case-XXXXXX.ll")"
fi

if [ -z "$OBJ_OUTPUT" ]; then
    OBJ_OUTPUT="$(mktemp "${TMPDIR:-/tmp}/lona-case-XXXXXX.o")"
fi

"$BIN" --emit ir --verify-ir "$INPUT" "$IR_OUTPUT"
"$BIN" --emit obj --verify-ir "$INPUT" "$OBJ_OUTPUT"

printf 'PASS %s -> %s -> %s\n' "$INPUT" "$IR_OUTPUT" "$OBJ_OUTPUT"
