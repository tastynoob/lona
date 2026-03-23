#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

ref_local_in="$(new_tmp_file ref-local)"
ref_local_out="$(new_tmp_file ref-local-out)"
ref_param_in="$(new_tmp_file ref-param)"
ref_param_out="$(new_tmp_file ref-param-out)"
ref_param_named_in="$(new_tmp_file ref-param-named)"
ref_param_named_out="$(new_tmp_file ref-param-named-out)"
ref_param_missing_ref_bad_in="$(new_tmp_file ref-param-missing-ref-bad)"
ref_param_missing_ref_bad_out="$(new_tmp_file ref-param-missing-ref-bad-out)"
ref_param_rvalue_bad_in="$(new_tmp_file ref-param-rvalue-bad)"
ref_param_rvalue_bad_out="$(new_tmp_file ref-param-rvalue-bad-out)"
ref_param_addr_in="$(new_tmp_file ref-param-addr)"
ref_param_addr_out="$(new_tmp_file ref-param-addr-out)"
ref_local_type_bad_in="$(new_tmp_file ref-local-type-bad)"
ref_local_type_bad_out="$(new_tmp_file ref-local-type-bad-out)"
ref_param_const_view_in="$(new_tmp_file ref-param-const-view)"
ref_param_const_view_out="$(new_tmp_file ref-param-const-view-out)"
ref_param_drop_const_bad_in="$(new_tmp_file ref-param-drop-const-bad)"
ref_param_drop_const_bad_out="$(new_tmp_file ref-param-drop-const-bad-out)"
ref_method_temp_in="$(new_tmp_file ref-method-temp)"
ref_method_temp_out="$(new_tmp_file ref-method-temp-out)"

cat >"$ref_local_in" <<'EOF'
def main() i32 {
    var x i32 = 3
    ref alias i32 = x
    alias = 9
    ret x
}
EOF
"$BIN" --emit-ir --verify-ir "$ref_local_in" >"$ref_local_out"
if [ "$(grep -c 'alloca i32' "$ref_local_out")" -ne 2 ]; then
    echo 'expected ref local binding to reuse the source storage without adding another alloca' >&2
    exit 1
fi
grep -q 'store i32 9, ptr %1' "$ref_local_out"

cat >"$ref_param_in" <<'EOF'
def set7(ref x i32) i32 {
    x = 7
    ret x
}

def main() i32 {
    var x i32 = 1
    ret set7(ref x)
}
EOF
"$BIN" --emit-ir --verify-ir "$ref_param_in" >"$ref_param_out"
grep -q '^define i32 @set7(ptr ' "$ref_param_out"
grep -q 'call i32 @set7(ptr ' "$ref_param_out"

cat >"$ref_param_named_in" <<'EOF'
def set7(ref slot i32) i32 {
    slot = 7
    ret slot
}

def main() i32 {
    var x i32 = 1
    ret set7(ref slot = x)
}
EOF
"$BIN" --emit-ir --verify-ir "$ref_param_named_in" >"$ref_param_named_out"
grep -q '^define i32 @set7(ptr ' "$ref_param_named_out"
grep -q 'call i32 @set7(ptr ' "$ref_param_named_out"

cat >"$ref_param_missing_ref_bad_in" <<'EOF'
def set7(ref slot i32) i32 {
    slot = 7
    ret slot
}

def main() i32 {
    var x i32 = 1
    ret set7(x)
}
EOF
expect_emit_ir_failure "$ref_param_missing_ref_bad_in" "$ref_param_missing_ref_bad_out" 'expected ref parameter call without explicit ref marker to fail'
grep -q 'reference parameter `slot` must be passed with `ref`' "$ref_param_missing_ref_bad_out"

cat >"$ref_param_rvalue_bad_in" <<'EOF'
def set7(ref x i32) i32 {
    x = 7
    ret x
}

def main() i32 {
    ret set7(ref 1 + 2)
}
EOF
expect_emit_ir_failure "$ref_param_rvalue_bad_in" "$ref_param_rvalue_bad_out" 'expected ref parameter call with rvalue argument to fail'
grep -q 'reference parameter `x` expects an addressable value' "$ref_param_rvalue_bad_out"

cat >"$ref_param_addr_in" <<'EOF'
def poke(ref x i32) i32 {
    var p i32* = &x
    *p = 9
    ret x
}

def main() i32 {
    var x i32 = 1
    ret poke(ref x)
}
EOF
"$BIN" --emit-ir --verify-ir "$ref_param_addr_in" >"$ref_param_addr_out"
poke_body="$(sed -n '/^define i32 @poke(/,/^}/p' "$ref_param_addr_out")"
echo "$poke_body" | grep -q '^define i32 @poke(ptr %0)'
if [ "$(echo "$poke_body" | grep -c 'alloca i32')" -ne 1 ]; then
    echo 'expected ref parameter address-of to reuse the incoming pointee slot without allocating a wrapper i32 slot' >&2
    exit 1
fi
echo "$poke_body" | grep -Eq 'store ptr %0, ptr %'

cat >"$ref_local_type_bad_in" <<'EOF'
def main() i32 {
    var x i32 = 3
    ref alias i64 = x
    ret 0
}
EOF
expect_emit_ir_failure "$ref_local_type_bad_in" "$ref_local_type_bad_out" 'expected mismatched ref local binding type to fail'
grep -q 'reference binding type mismatch for `alias`: expected i64, got i32' "$ref_local_type_bad_out"

cat >"$ref_param_const_view_in" <<'EOF'
def read(ref x i32 const) i32 {
    ret x
}

def main() i32 {
    var x i32 = 3
    ret read(ref x)
}
EOF
"$BIN" --emit-ir --verify-ir "$ref_param_const_view_in" >"$ref_param_const_view_out"
grep -q '^define i32 @read(ptr ' "$ref_param_const_view_out"
grep -q 'call i32 @read(ptr ' "$ref_param_const_view_out"

cat >"$ref_param_drop_const_bad_in" <<'EOF'
def bump(ref x i32) i32 {
    x = x + 1
    ret x
}

def main() i32 {
    var x i32 const = 3
    ret bump(ref x)
}
EOF
expect_emit_ir_failure "$ref_param_drop_const_bad_in" "$ref_param_drop_const_bad_out" 'expected ref const drop program to fail'
grep -q 'reference parameter `x` type mismatch: expected i32, got i32 const' "$ref_param_drop_const_bad_out"

cat >"$ref_method_temp_in" <<'EOF'
struct Counter {
    value i32

    def bump() i32 {
        self.value = self.value + 1
        ret self.value
    }
}

def main() i32 {
    ret Counter(1).bump()
}
EOF
"$BIN" --emit-ir --verify-ir "$ref_method_temp_in" >"$ref_method_temp_out"
grep -Eq '^define i32 @.*Counter\.bump\(ptr ' "$ref_method_temp_out"
grep -Eq 'call i32 @.*Counter\.bump\(ptr ' "$ref_method_temp_out"
