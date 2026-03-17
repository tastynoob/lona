#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_NATIVE="$ROOT/scripts/lac-native.sh"
TMPDIR_LOCAL="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMPDIR_LOCAL/lona-native-smoke-XXXXXX")"

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

main_program="$WORKDIR/return_42.lo"
top_level_program="$WORKDIR/top_level.lo"
main_exe="$WORKDIR/return_42"
top_level_exe="$WORKDIR/top_level"

cat >"$main_program" <<'EOF'
def main() i32 {
    ret 42
}
EOF

cat >"$top_level_program" <<'EOF'
var x = 1
x = x + 1
EOF

bash "$BUILD_NATIVE" "$main_program" "$main_exe"
bash "$BUILD_NATIVE" "$top_level_program" "$top_level_exe"

set +e
"$main_exe"
main_status=$?
"$top_level_exe"
top_level_status=$?
set -e

if [ "$main_status" -ne 42 ]; then
    echo "expected native main program to exit with 42, got $main_status" >&2
    exit 1
fi

if [ "$top_level_status" -ne 0 ]; then
    echo "expected top-level native program to exit with 0, got $top_level_status" >&2
    exit 1
fi

echo "native smoke passed"
