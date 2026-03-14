#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/lona"
INPUT="$ROOT/test.lo"
TMPDIR_LOCAL="${TMPDIR:-/tmp/claude-1000}"
if [ ! -d "$TMPDIR_LOCAL" ]; then
    TMPDIR_LOCAL="/tmp/claude-1000"
fi

json_out="$(mktemp "$TMPDIR_LOCAL/lona-json-XXXXXX.txt")"
ir_out="$(mktemp "$TMPDIR_LOCAL/lona-ir-XXXXXX.ll")"
debug_out="$(mktemp "$TMPDIR_LOCAL/lona-debug-XXXXXX.ll")"
bad_out="$(mktemp "$TMPDIR_LOCAL/lona-bad-XXXXXX.lo")"
bool_in="$(mktemp "$TMPDIR_LOCAL/lona-bool-XXXXXX.lo")"
bool_out="$(mktemp "$TMPDIR_LOCAL/lona-bool-XXXXXX.ll")"
pointer_in="$(mktemp "$TMPDIR_LOCAL/lona-pointer-XXXXXX.lo")"
pointer_out="$(mktemp "$TMPDIR_LOCAL/lona-pointer-XXXXXX.ll")"
func_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-bad-XXXXXX.lo")"
func_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-bad-XXXXXX.txt")"
cleanup() {
    rm -f "$json_out" "$ir_out" "$debug_out" "$bad_out" "$bool_in" "$bool_out" \
        "$pointer_in" "$pointer_out" "$func_bad_in" "$func_bad_out"
}
trap cleanup EXIT

"$BIN" "$INPUT" >"$json_out"
grep -q '"type": "Program"' "$json_out"
grep -q '"type": "FieldCall"' "$json_out"

"$BIN" --emit-ir --verify-ir "$INPUT" >"$ir_out"
grep -q '^define %Complex @Complex.add' "$ir_out"
grep -q '^define i32 @fibo' "$ir_out"
if grep -q 'llvm.dbg.declare' "$ir_out"; then
    echo 'unexpected debug metadata in non-debug IR' >&2
    exit 1
fi

"$BIN" --emit-ir --verify-ir -g "$INPUT" >"$debug_out"
grep -q 'llvm.dbg.declare' "$debug_out"
grep -q '!llvm.dbg.cu' "$debug_out"
grep -q '!DISubprogram' "$debug_out"

auto_bad_source='def bad(a i32) i32 {
    if a < 1 {
        ret 1
    }
}
'
printf '%s' "$auto_bad_source" >"$bad_out"
if "$BIN" --emit-ir "$bad_out" >/dev/null 2>&1; then
    echo 'expected missing return program to fail' >&2
    exit 1
fi

bool_source='def local_bool(a i32) bool {
    var ok bool = true
    if a > 3 {
        ok = false
    }
    if ok {
        ret true
    }
    ret false
}
'
printf '%s' "$bool_source" >"$bool_in"
"$BIN" --emit-ir --verify-ir "$bool_in" >"$bool_out"
grep -q '^define i1 @local_bool' "$bool_out"
grep -q 'store i1 true' "$bool_out"
grep -q 'ret i1 false' "$bool_out"

pointer_source='def pointer_roundtrip(a i32) i32 {
    var value i32 = a
    var ptr i32* = &value
    *ptr = *ptr + 1
    ret value
}
'
printf '%s' "$pointer_source" >"$pointer_in"
"$BIN" --emit-ir --verify-ir "$pointer_in" >"$pointer_out"
grep -q '^define i32 @pointer_roundtrip' "$pointer_out"
grep -q 'alloca ptr' "$pointer_out"
grep -q 'store ptr ' "$pointer_out"
grep -q 'load ptr, ptr ' "$pointer_out"
grep -q 'store i32 %' "$pointer_out"

func_bad_source='def bad_callback(cb () i32) i32 {
    ret 0
}
'
printf '%s' "$func_bad_source" >"$func_bad_in"
if "$BIN" --emit-ir "$func_bad_in" >"$func_bad_out" 2>&1; then
    echo 'expected bare function parameter program to fail' >&2
    exit 1
fi
grep -q 'unsupported bare function parameter type for `cb` in `bad_callback`: () i32' "$func_bad_out"

printf 'acceptance checks passed\n'
