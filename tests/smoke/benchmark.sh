#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
TMPDIR_LOCAL="${TMPDIR:-/tmp/claude-1000}"

if [ ! -d "$TMPDIR_LOCAL" ]; then
    TMPDIR_LOCAL="/tmp/lona-bench"
fi
mkdir -p "$TMPDIR_LOCAL"

if [ ! -x "$BIN" ]; then
    echo "missing compiler binary: $BIN" >&2
    exit 1
fi

bench_dir="$(mktemp -d "$TMPDIR_LOCAL/lona-bench-XXXXXX")"
synthetic_in="$bench_dir/synthetic.lo"
trap 'rm -rf "$bench_dir"' EXIT

{
    echo 'def seed() i32 {'
    echo '    ret 0'
    echo '}'
    echo
    for i in $(seq 0 199); do
        if [ "$i" -eq 0 ]; then
            echo "def chain0(v i32) i32 {"
            echo "    ret v + 1"
            echo '}'
            echo
            continue
        fi
        prev=$((i - 1))
        echo "def chain$i(v i32) i32 {"
        echo "    ret chain$prev(v) + 1"
        echo '}'
        echo
    done
    echo 'def main() i32 {'
    echo '    ret chain199(seed())'
    echo '}'
} >"$synthetic_in"

run_case() {
    local name="$1"
    local input="$2"
    local log="$bench_dir/$name.log"
    local start end elapsed_ms

    start=$(date +%s%N)
    "$BIN" --emit ir --verify-ir --stats "$input" >/dev/null 2>"$log"
    end=$(date +%s%N)
    elapsed_ms=$(((end - start) / 1000000))

    echo "[$name]"
    echo "wall-ms: $elapsed_ms"
    cat "$log"
    echo
}

run_case "function-pointer" "$ROOT/example/function_pointer_suite.lo"
run_case "module-import" "$ROOT/example/modules/main.lo"
run_case "synthetic-chain" "$synthetic_in"
