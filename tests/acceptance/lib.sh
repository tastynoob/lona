#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
FIXTURES_DIR="$ROOT/tests/fixtures"
TMPDIR_LOCAL="${TMPDIR:-/tmp/claude-1000}"

if [ ! -d "$TMPDIR_LOCAL" ]; then
    TMPDIR_LOCAL="/tmp/lona-acceptance"
fi
mkdir -p "$TMPDIR_LOCAL"

if [ ! -x "$BIN" ]; then
    echo "missing compiler binary: $BIN" >&2
    exit 1
fi

ACCEPTANCE_TMPDIR="$(mktemp -d "$TMPDIR_LOCAL/lona-acceptance-XXXXXX")"

cleanup_acceptance_tmpdir() {
    rm -rf "$ACCEPTANCE_TMPDIR"
}
trap cleanup_acceptance_tmpdir EXIT

new_tmp_file() {
    local prefix="$1"
    mktemp "$ACCEPTANCE_TMPDIR/${prefix}-XXXXXX"
}

new_tmp_dir() {
    local prefix="$1"
    mktemp -d "$ACCEPTANCE_TMPDIR/${prefix}-XXXXXX"
}

expect_emit_ir_failure() {
    local input="$1"
    local output="$2"
    local message="$3"

    if "$BIN" --emit-ir "$input" >"$output" 2>&1; then
        echo "$message" >&2
        exit 1
    fi
}
