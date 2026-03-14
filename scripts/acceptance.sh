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
missing_return_in="$(mktemp "$TMPDIR_LOCAL/lona-missing-return-XXXXXX.lo")"
json_feature_in="$(mktemp "$TMPDIR_LOCAL/lona-json-feature-XXXXXX.lo")"
json_feature_out="$(mktemp "$TMPDIR_LOCAL/lona-json-feature-XXXXXX.json")"
bool_in="$(mktemp "$TMPDIR_LOCAL/lona-bool-XXXXXX.lo")"
bool_out="$(mktemp "$TMPDIR_LOCAL/lona-bool-XXXXXX.ll")"
pointer_in="$(mktemp "$TMPDIR_LOCAL/lona-pointer-XXXXXX.lo")"
pointer_out="$(mktemp "$TMPDIR_LOCAL/lona-pointer-XXXXXX.ll")"
func_param_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-param-bad-XXXXXX.lo")"
func_param_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-param-bad-XXXXXX.txt")"
func_local_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-local-bad-XXXXXX.lo")"
func_local_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-local-bad-XXXXXX.txt")"
func_top_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-top-bad-XXXXXX.lo")"
func_top_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-top-bad-XXXXXX.txt")"
func_inferred_local_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-local-bad-XXXXXX.lo")"
func_inferred_local_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-local-bad-XXXXXX.txt")"
func_inferred_top_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-top-bad-XXXXXX.lo")"
func_inferred_top_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-top-bad-XXXXXX.txt")"
grammar_subset_in="$(mktemp "$TMPDIR_LOCAL/lona-grammar-subset-XXXXXX.lo")"
grammar_subset_out="$(mktemp "$TMPDIR_LOCAL/lona-grammar-subset-XXXXXX.ll")"
cleanup() {
    rm -f "$json_out" "$ir_out" "$debug_out" "$missing_return_in" \
        "$json_feature_in" "$json_feature_out" "$bool_in" "$bool_out" \
        "$pointer_in" "$pointer_out" "$func_param_bad_in" "$func_param_bad_out" \
        "$func_local_bad_in" "$func_local_bad_out" "$func_top_bad_in" "$func_top_bad_out" \
        "$func_inferred_local_bad_in" "$func_inferred_local_bad_out" \
        "$func_inferred_top_bad_in" "$func_inferred_top_bad_out" \
        "$grammar_subset_in" "$grammar_subset_out"
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

missing_return_source='def bad(a i32) i32 {
    if a < 1 {
        ret 1
    }
}
'
printf '%s' "$missing_return_source" >"$missing_return_in"
if "$BIN" --emit-ir "$missing_return_in" >/dev/null 2>&1; then
    echo 'expected missing return program to fail' >&2
    exit 1
fi

json_feature_source='def walk(limit i32) {
    var i i32 = 0
    for i < limit {
        i = i + 1
    }
    ret
}
'
printf '%s' "$json_feature_source" >"$json_feature_in"
"$BIN" "$json_feature_in" >"$json_feature_out"
grep -q '"type": "Program"' "$json_feature_out"
grep -q '"type": "For"' "$json_feature_out"
grep -q '"cond": {' "$json_feature_out"
grep -q '"body": {' "$json_feature_out"
grep -q '"type": "Return"' "$json_feature_out"
grep -q '"value": null' "$json_feature_out"

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
grep -q 'alloca i1' "$bool_out"
grep -q 'store i1 true' "$bool_out"
grep -q 'store i1 false' "$bool_out"
grep -q 'load i1, ptr ' "$bool_out"
grep -q 'ret i1 %' "$bool_out"

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

func_param_bad_source='def bad_callback(cb () i32) i32 {
    ret 0
}
'
printf '%s' "$func_param_bad_source" >"$func_param_bad_in"
if "$BIN" --emit-ir "$func_param_bad_in" >"$func_param_bad_out" 2>&1; then
    echo 'expected bare function parameter program to fail' >&2
    exit 1
fi
grep -q 'unsupported bare function parameter type for `cb` in `bad_callback`: () i32' "$func_param_bad_out"

func_local_bad_source='def bad_local() i32 {
    var cb () i32
    ret 0
}
'
printf '%s' "$func_local_bad_source" >"$func_local_bad_in"
if "$BIN" --emit-ir "$func_local_bad_in" >"$func_local_bad_out" 2>&1; then
    echo 'expected bare function local variable program to fail' >&2
    exit 1
fi
grep -q 'unsupported bare function variable type for `cb`: () i32' "$func_local_bad_out"

func_top_bad_source='var cb () i32
'
printf '%s' "$func_top_bad_source" >"$func_top_bad_in"
if "$BIN" --emit-ir "$func_top_bad_in" >"$func_top_bad_out" 2>&1; then
    echo 'expected bare function top-level variable program to fail' >&2
    exit 1
fi
grep -q 'unsupported bare function variable type for `cb`: () i32' "$func_top_bad_out"

func_inferred_local_bad_source='def foo() i32 {
    ret 1
}

def bad_inferred_local() i32 {
    var cb = foo
    ret 0
}
'
printf '%s' "$func_inferred_local_bad_source" >"$func_inferred_local_bad_in"
if "$BIN" --emit-ir "$func_inferred_local_bad_in" >"$func_inferred_local_bad_out" 2>&1; then
    echo 'expected inferred bare function local variable program to fail' >&2
    exit 1
fi
grep -q 'unsupported bare function variable type for `cb`: () i32' "$func_inferred_local_bad_out"

func_inferred_top_bad_source='def foo() i32 {
    ret 1
}

var cb = foo
'
printf '%s' "$func_inferred_top_bad_source" >"$func_inferred_top_bad_in"
if "$BIN" --emit-ir "$func_inferred_top_bad_in" >"$func_inferred_top_bad_out" 2>&1; then
    echo 'expected inferred bare function top-level variable program to fail' >&2
    exit 1
fi
grep -q 'unsupported bare function variable type for `cb`: () i32' "$func_inferred_top_bad_out"

grammar_subset_source='struct Name {
    a i32
    b i32
}

def make_name(a i32, b i32) Name {
    var out Name
    out.a = a
    out.b = b
    ret out
}

var sample = make_name(1, 2)
'
printf '%s' "$grammar_subset_source" >"$grammar_subset_in"
"$BIN" --emit-ir --verify-ir "$grammar_subset_in" >"$grammar_subset_out"
grep -q '^%Name = type { i32, i32 }' "$grammar_subset_out"
grep -q '^define %Name @make_name' "$grammar_subset_out"

printf 'acceptance checks passed\n'
