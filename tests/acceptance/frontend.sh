#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

INPUT="$FIXTURES_DIR/acceptance_main.lo"

json_out="$(new_tmp_file json)"
ir_out="$(new_tmp_file ir)"
obj_out="$(new_tmp_file obj)"
darwin_obj_out="$(new_tmp_file darwin-obj)"
opt_ir_out="$(new_tmp_file opt-ir)"
debug_out="$(new_tmp_file debug)"
missing_return_in="$(new_tmp_file missing-return)"
json_feature_in="$(new_tmp_file json-feature)"
json_feature_out="$(new_tmp_file json-feature-out)"
cast_json_in="$(new_tmp_file cast-json)"
cast_json_out="$(new_tmp_file cast-json-out)"
func_ptr_json_in="$(new_tmp_file func-ptr-json)"
func_ptr_json_out="$(new_tmp_file func-ptr-json-out)"
null_json_in="$(new_tmp_file null-json)"
null_json_out="$(new_tmp_file null-json-out)"
bool_in="$(new_tmp_file bool)"
bool_out="$(new_tmp_file bool-out)"
pointer_in="$(new_tmp_file pointer)"
pointer_out="$(new_tmp_file pointer-out)"
byte_json_in="$(new_tmp_file byte-json)"
byte_json_out="$(new_tmp_file byte-json-out)"
entry_ir_in="$(new_tmp_file entry-ir)"
entry_bare_ir_out="$(new_tmp_file entry-bare-ir-out)"
entry_alt_bare_ir_out="$(new_tmp_file entry-alt-bare-ir-out)"
entry_system_ir_out="$(new_tmp_file entry-system-ir-out)"
c_abi_only_in="$(new_tmp_file c-abi-only)"
c_abi_only_obj_out="$(new_tmp_file c-abi-only-obj)"

"$BIN" "$INPUT" >"$json_out"
grep -q '"type": "Program"' "$json_out"
grep -q '"type": "FieldCall"' "$json_out"

"$BIN" --emit ir --verify-ir "$INPUT" >"$ir_out"
grep -Eq '^define i64 @.*Complex\.add\(ptr [^,]+, i64 [^)]+\)' "$ir_out"
grep -q '^define i32 @fibo' "$ir_out"
if grep -q 'llvm.dbg.declare' "$ir_out"; then
    echo 'unexpected debug metadata in non-debug IR' >&2
    exit 1
fi

"$BIN" --emit obj --verify-ir "$INPUT" "$obj_out"
test -s "$obj_out"
if [ "$(od -An -t x1 -N 4 "$obj_out" | tr -d ' \n')" != "7f454c46" ]; then
    echo 'unexpected object file header for --emit obj output' >&2
    exit 1
fi
nm -a "$obj_out" | grep -Fq '__lona_native_abi_v0_0'
strings "$obj_out" | grep -Fq 'lona.native_abi=v0.0'

"$BIN" --emit obj --target x86_64-apple-darwin --verify-ir "$INPUT" "$darwin_obj_out"
test -s "$darwin_obj_out"
strings "$darwin_obj_out" | grep -Fq 'lona.native_abi=v0.0'

"$BIN" --emit ir --verify-ir -O3 "$INPUT" >"$opt_ir_out"
grep -Eq '^define i64 @.*Complex\.add\(ptr [^,]+, i64 [^)]+\)' "$opt_ir_out"
grep -q '^define i32 @fibo' "$opt_ir_out"

"$BIN" --emit ir --verify-ir -g "$INPUT" >"$debug_out"
grep -q 'llvm.dbg.declare' "$debug_out"
grep -q '!llvm.dbg.cu' "$debug_out"
grep -q '!DISubprogram' "$debug_out"

cat >"$entry_ir_in" <<'EOF'
def run() i32 {
    ret 7
}

ret run()
EOF
"$BIN" --emit ir --target x86_64-none-elf --verify-ir "$entry_ir_in" >"$entry_bare_ir_out"
grep -q 'target triple = "x86_64-none-unknown-elf"' "$entry_bare_ir_out"
grep -q '^define i32 @__lona_main__()' "$entry_bare_ir_out"
grep -q '^define i32 @run()' "$entry_bare_ir_out"
! grep -q '^define i32 @main()' "$entry_bare_ir_out"
! grep -q '^define i32 @main(i32' "$entry_bare_ir_out"
! grep -q '^@__lona_argc =' "$entry_bare_ir_out"
! grep -q '^@__lona_argv =' "$entry_bare_ir_out"

"$BIN" --emit ir --target x86_64-unknown-none-elf --verify-ir "$entry_ir_in" >"$entry_alt_bare_ir_out"
grep -q 'target triple = "x86_64-unknown-none-elf"' "$entry_alt_bare_ir_out"
grep -q '^define i32 @__lona_main__()' "$entry_alt_bare_ir_out"
grep -q '^define i32 @run()' "$entry_alt_bare_ir_out"
! grep -q '^define i32 @main()' "$entry_alt_bare_ir_out"
! grep -q '^define i32 @main(i32' "$entry_alt_bare_ir_out"
! grep -q '^@__lona_argc =' "$entry_alt_bare_ir_out"
! grep -q '^@__lona_argv =' "$entry_alt_bare_ir_out"

"$BIN" --emit ir --target x86_64-unknown-linux-gnu --verify-ir "$entry_ir_in" >"$entry_system_ir_out"
grep -q 'target triple = "x86_64-unknown-linux-gnu"' "$entry_system_ir_out"
grep -q '^define i32 @__lona_main__()' "$entry_system_ir_out"
grep -q '^define i32 @run()' "$entry_system_ir_out"
grep -q '^define i32 @main(i32' "$entry_system_ir_out"
grep -q '^@__lona_argc =' "$entry_system_ir_out"
grep -q '^@__lona_argv =' "$entry_system_ir_out"

cat >"$c_abi_only_in" <<'EOF'
extern "C" def add(a i32, b i32) i32 {
    ret a + b
}
EOF
"$BIN" --emit obj --target x86_64-unknown-linux-gnu --verify-ir "$c_abi_only_in" "$c_abi_only_obj_out"
! nm -a "$c_abi_only_obj_out" | grep -Fq '__lona_native_abi_'
! strings "$c_abi_only_obj_out" | grep -Fq 'lona.native_abi='

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

cat >"$null_json_in" <<'EOF'
def main() i32 {
    var ptr i32* = null
    ret 0
}
EOF
"$BIN" "$null_json_in" >"$null_json_out"
grep -Fq '"declaredType": "i32*"' "$null_json_out"
grep -Fq '"value": null' "$null_json_out"

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
"$BIN" --emit ir --verify-ir "$bool_in" >"$bool_out"
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
"$BIN" --emit ir --verify-ir "$pointer_in" >"$pointer_out"
grep -q '^define i32 @pointer_roundtrip' "$pointer_out"
grep -q 'alloca ptr' "$pointer_out"
grep -q 'store ptr ' "$pointer_out"
grep -q 'load ptr, ptr ' "$pointer_out"
grep -q 'store i32 %' "$pointer_out"
