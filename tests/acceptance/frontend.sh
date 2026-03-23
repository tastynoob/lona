#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

INPUT="$FIXTURES_DIR/acceptance_main.lo"

json_out="$(new_tmp_file json)"
ir_out="$(new_tmp_file ir)"
debug_out="$(new_tmp_file debug)"
missing_return_in="$(new_tmp_file missing-return)"
json_feature_in="$(new_tmp_file json-feature)"
json_feature_out="$(new_tmp_file json-feature-out)"
cast_json_in="$(new_tmp_file cast-json)"
cast_json_out="$(new_tmp_file cast-json-out)"
func_ptr_json_in="$(new_tmp_file func-ptr-json)"
func_ptr_json_out="$(new_tmp_file func-ptr-json-out)"
bool_in="$(new_tmp_file bool)"
bool_out="$(new_tmp_file bool-out)"
pointer_in="$(new_tmp_file pointer)"
pointer_out="$(new_tmp_file pointer-out)"
byte_json_in="$(new_tmp_file byte-json)"
byte_json_out="$(new_tmp_file byte-json-out)"

"$BIN" "$INPUT" >"$json_out"
grep -q '"type": "Program"' "$json_out"
grep -q '"type": "FieldCall"' "$json_out"

"$BIN" --emit-ir --verify-ir "$INPUT" >"$ir_out"
grep -Eq '^define i64 @.*Complex\.add\(ptr [^,]+, i64 [^)]+\)' "$ir_out"
grep -q '^define i32 @fibo' "$ir_out"
if grep -q 'llvm.dbg.declare' "$ir_out"; then
    echo 'unexpected debug metadata in non-debug IR' >&2
    exit 1
fi

"$BIN" --emit-ir --verify-ir -g "$INPUT" >"$debug_out"
grep -q 'llvm.dbg.declare' "$debug_out"
grep -q '!llvm.dbg.cu' "$debug_out"
grep -q '!DISubprogram' "$debug_out"

cat >"$missing_return_in" <<'EOF'
def bad(a i32) i32 {
    if a < 1 {
        ret 1
    }
}
EOF
expect_emit_ir_failure "$missing_return_in" "$ir_out" 'expected missing return program to fail'

cat >"$json_feature_in" <<'EOF'
def walk(limit i32) {
    var i i32 = 0
    for i < limit {
        i = i + 1
    }
    ret
}
EOF
"$BIN" "$json_feature_in" >"$json_feature_out"
grep -q '"type": "Program"' "$json_feature_out"
grep -q '"type": "For"' "$json_feature_out"
grep -q '"cond": {' "$json_feature_out"
grep -q '"body": {' "$json_feature_out"
grep -q '"type": "Return"' "$json_feature_out"
grep -q '"value": null' "$json_feature_out"

cat >"$cast_json_in" <<'EOF'
def widen(v <i32, i32>) <i32, i32> {
    ret cast[<i32, i32>](v)
}
EOF
"$BIN" "$cast_json_in" >"$cast_json_out"
grep -q '"type": "CastExpr"' "$cast_json_out"
grep -q '"targetType": "<i32, i32>"' "$cast_json_out"

cat >"$func_ptr_json_in" <<'EOF'
def hold() {
    var slot (i32: i32)*
    var table (: i32* const)[1] const
    ret
}
EOF
"$BIN" "$func_ptr_json_in" >"$func_ptr_json_out"
grep -Fq '"declaredType": "(i32: i32)*"' "$func_ptr_json_out"
grep -Fq '"declaredType": "(: i32* const)[1] const"' "$func_ptr_json_out"

cat >"$byte_json_in" <<'EOF'
def main() {
    var raw = "\xFF\n"
    ret
}
EOF
"$BIN" "$byte_json_in" >"$byte_json_out"
grep -Fq '"value": "\\xFF\\n"' "$byte_json_out"

cat >"$bool_in" <<'EOF'
def local_bool(a i32) bool {
    var ok bool = true
    if a > 3 {
        ok = false
    }
    if ok {
        ret true
    }
    ret false
}
EOF
"$BIN" --emit-ir --verify-ir "$bool_in" >"$bool_out"
grep -q '^define i8 @local_bool' "$bool_out"
grep -q 'alloca i8' "$bool_out"
grep -q 'store i8 1' "$bool_out"
grep -q 'store i8 0' "$bool_out"
grep -q 'load i8, ptr ' "$bool_out"
grep -q 'ret i8 %' "$bool_out"

cat >"$pointer_in" <<'EOF'
def pointer_roundtrip(a i32) i32 {
    var value i32 = a
    var ptr i32* = &value
    *ptr = *ptr + 1
    ret value
}
EOF
"$BIN" --emit-ir --verify-ir "$pointer_in" >"$pointer_out"
grep -q '^define i32 @pointer_roundtrip' "$pointer_out"
grep -q 'alloca ptr' "$pointer_out"
grep -q 'store ptr ' "$pointer_out"
grep -q 'load ptr, ptr ' "$pointer_out"
grep -q 'store i32 %' "$pointer_out"
