#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"

for sample in \
    "$ROOT/example/algorithms_suite.lo" \
    "$ROOT/example/data_model_suite.lo" \
    "$ROOT/example/function_pointer_suite.lo" \
    "$ROOT/example/syntax_suite.lo" \
    "$ROOT/example/modules/main.lo"
do
    "$BIN" --emit-ir --verify-ir "$sample" >/dev/null
done

echo "example smoke passed"
