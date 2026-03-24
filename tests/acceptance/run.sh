#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PYTHON_BIN="${PYTHON:-python3}"

"$PYTHON_BIN" -m pytest -q "$ROOT"/tests/acceptance/test_*.py

for script in \
    functions.sh \
    references.sh \
    modules.sh \
    operators.sh \
    syntax_features.sh
do
    bash "$ROOT/tests/acceptance/$script"
done

printf 'acceptance checks passed\n'
