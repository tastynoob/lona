#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

for script in \
    frontend.sh \
    controlflow.sh \
    functions.sh \
    references.sh \
    diagnostics.sh \
    modules.sh \
    operators.sh \
    syntax_features.sh
do
    bash "$ROOT/tests/acceptance/$script"
done

printf 'acceptance checks passed\n'
