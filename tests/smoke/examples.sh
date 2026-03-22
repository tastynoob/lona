#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
BUILD_SYSTEM="$ROOT/scripts/lac.sh"
TMPDIR_LOCAL="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMPDIR_LOCAL/lona-example-smoke-XXXXXX")"

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

for sample in \
    "$ROOT/example/algorithms_suite.lo" \
    "$ROOT/example/c_ffi_linked_list.lo" \
    "$ROOT/example/data_model_suite.lo" \
    "$ROOT/example/function_pointer_suite.lo" \
    "$ROOT/example/syntax_suite.lo" \
    "$ROOT/example/modules/main.lo"
do
    "$BIN" --emit-ir --verify-ir "$sample" >/dev/null
done

linked_list_exe="$WORKDIR/c_ffi_linked_list"
bash "$BUILD_SYSTEM" "$ROOT/example/c_ffi_linked_list.lo" "$linked_list_exe"
set +e
"$linked_list_exe"
linked_list_status=$?
set -e
if [ "$linked_list_status" -ne 0 ]; then
    echo "expected example/c_ffi_linked_list.lo to exit with 0, got $linked_list_status" >&2
    exit 1
fi

echo "example smoke passed"
