#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

milestone_json_in="$(new_tmp_file milestone-json)"
milestone_json_out="$(new_tmp_file milestone-json-out)"
legacy_cast_in="$(new_tmp_file legacy-cast)"
legacy_cast_out="$(new_tmp_file legacy-cast-out)"
float_in="$(new_tmp_file float)"
float_out="$(new_tmp_file float-out)"
float_literal_target_bad_in="$(new_tmp_file float-literal-target-bad)"
float_literal_target_bad_out="$(new_tmp_file float-literal-target-bad-out)"
float_nan_cmp_in="$(new_tmp_file float-nan-cmp)"
float_nan_cmp_out="$(new_tmp_file float-nan-cmp-out)"
bool_bytecopy_bad_in="$(new_tmp_file bool-bytecopy-bad)"
bool_bytecopy_bad_out="$(new_tmp_file bool-bytecopy-bad-out)"
string_placeholder_in="$(new_tmp_file string-placeholder)"
string_placeholder_out="$(new_tmp_file string-placeholder-out)"
tuple_in="$(new_tmp_file tuple)"
tuple_out="$(new_tmp_file tuple-out)"
tuple_no_context_in="$(new_tmp_file tuple-no-context)"
tuple_no_context_out="$(new_tmp_file tuple-no-context-out)"
array_init_in="$(new_tmp_file array-init)"
array_init_out="$(new_tmp_file array-init-out)"
array_index_in="$(new_tmp_file array-index)"
array_index_out="$(new_tmp_file array-index-out)"
operator_placeholder_in="$(new_tmp_file operator-placeholder)"
operator_placeholder_out="$(new_tmp_file operator-placeholder-out)"

cat >"$milestone_json_in" <<'EOF'
def show() {
    var pair <i32, bool> = (1, true)
    var matrix i32[4][5] = {{}}
    ret
}
EOF
"$BIN" "$milestone_json_in" >"$milestone_json_out"
grep -Fq '"declaredType": "<i32, bool>"' "$milestone_json_out"
grep -Fq '"type": "TupleLiteral"' "$milestone_json_out"
grep -Fq '"declaredType": "i32[?][?]"' "$milestone_json_out"
grep -Fq '"type": "ArrayInit"' "$milestone_json_out"

cat >"$legacy_cast_in" <<'EOF'
def bad() i32 {
    var x i32 = i32 1
    ret x
}
EOF
expect_emit_ir_failure "$legacy_cast_in" "$legacy_cast_out" 'expected legacy cast syntax program to fail'
grep -Fq "lona doesn't support C-style cast syntax like \`i32 1\`." "$legacy_cast_out"
grep -Fq 'byte-copy semantics' "$legacy_cast_out"

cat >"$float_in" <<'EOF'
def id(v f32) f32 {
    ret v
}

def bad() f32 {
    var x f32 = 1.0 + 2.0
    ret id(-x)
}
EOF
"$BIN" --emit-ir --verify-ir "$float_in" >"$float_out"
grep -q '^define float @id' "$float_out"
grep -q '^define float @bad' "$float_out"
grep -q 'store float 3.000000e+00' "$float_out"
grep -q 'fneg float %' "$float_out"
grep -q 'call float @id(float %' "$float_out"

cat >"$float_literal_target_bad_in" <<'EOF'
def bad() u64 {
    var x u64 = 1.25
    ret x
}
EOF
expect_emit_ir_failure "$float_literal_target_bad_in" "$float_literal_target_bad_out" 'expected non-float target float literal program to fail'
grep -Fq "floating-point literal doesn't match the expected target type" "$float_literal_target_bad_out"

cat >"$float_nan_cmp_in" <<'EOF'
def bad() bool {
    var z f64 = 0.0
    var nan f64 = z / z
    ret nan != nan
}
EOF
"$BIN" --emit-ir --verify-ir "$float_nan_cmp_in" >"$float_nan_cmp_out"
grep -q 'fcmp une double' "$float_nan_cmp_out"

cat >"$bool_bytecopy_bad_in" <<'EOF'
def bad() i8 {
    var b bool = true
    var x i8 = b
    ret x
}
EOF
expect_emit_ir_failure "$bool_bytecopy_bad_in" "$bool_bytecopy_bad_out" 'expected bool byte-copy mismatch program to fail'
grep -Fq 'initializer type mismatch for `x`: expected i8, got bool' "$bool_bytecopy_bad_out"

cat >"$string_placeholder_in" <<'EOF'
def bad() i32 {
    var msg = "hello"
    ret 0
}
EOF
expect_emit_ir_failure "$string_placeholder_in" "$string_placeholder_out" 'expected string placeholder program to fail'
grep -Fq 'string literals are reserved, but runtime string semantics are not implemented yet' "$string_placeholder_out"

cat >"$tuple_in" <<'EOF'
def bad() i32 {
    var pair <i32, bool> = (1, true)
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$tuple_in" >"$tuple_out"
grep -q 'alloca { i32, i1 }' "$tuple_out"
grep -q 'store { i32, i1 } { i32 1, i1 true }' "$tuple_out"

cat >"$tuple_no_context_in" <<'EOF'
def bad() i32 {
    var pair = (1, true)
    ret 0
}
EOF
expect_emit_ir_failure "$tuple_no_context_in" "$tuple_no_context_out" 'expected context-free tuple literal program to fail'
grep -Fq 'tuple literals need an explicit tuple target type' "$tuple_no_context_out"

cat >"$array_init_in" <<'EOF'
def bad() i32 {
    var matrix i32[4][5] = {{}}
    ret 0
}
EOF
expect_emit_ir_failure "$array_init_in" "$array_init_out" 'expected array initializer placeholder program to fail'
grep -Fq 'array initializers are parsed, but fixed-dimension array semantics are not implemented yet' "$array_init_out"

cat >"$array_index_in" <<'EOF'
def use(table i32[4][5]) i32 {
    ret table(1)(2)
}
EOF
expect_emit_ir_failure "$array_index_in" "$array_index_out" 'expected array index lowering placeholder program to fail'
grep -Fq 'array indexing lowering is not implemented yet' "$array_index_out"

cat >"$operator_placeholder_in" <<'EOF'
def bad(a i32, b i32) i32 {
    ret a % b
}
EOF
expect_emit_ir_failure "$operator_placeholder_in" "$operator_placeholder_out" 'expected placeholder operator program to fail'
grep -Fq 'operator `%` is parsed, but semantic support is not implemented yet' "$operator_placeholder_out"
