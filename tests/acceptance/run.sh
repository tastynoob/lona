#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PYTHON_BIN="${PYTHON:-python3}"

"$PYTHON_BIN" -m pytest -q "$ROOT"/tests/acceptance/test_*.py

printf 'acceptance checks passed\n'
