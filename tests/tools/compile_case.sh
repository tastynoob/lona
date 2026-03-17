#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
if [ -n "${CLANG:-}" ]; then
    CLANG_BIN="$CLANG"
elif command -v clang-18 >/dev/null 2>&1; then
    CLANG_BIN="clang-18"
else
    CLANG_BIN="clang"
fi
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

"$BIN" --emit-ir --verify-ir "$INPUT" >"$IR_OUTPUT"
"$CLANG_BIN" -Werror -Wno-override-module -x ir -c "$IR_OUTPUT" -o "$OBJ_OUTPUT"

printf 'PASS %s -> %s -> %s\n' "$INPUT" "$IR_OUTPUT" "$OBJ_OUTPUT"
