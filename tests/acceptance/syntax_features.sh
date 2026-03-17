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
tuple_flow_in="$(new_tmp_file tuple-flow)"
tuple_flow_out="$(new_tmp_file tuple-flow-out)"
tuple_field_in="$(new_tmp_file tuple-field)"
tuple_field_out="$(new_tmp_file tuple-field-out)"
tuple_field_bad_in="$(new_tmp_file tuple-field-bad)"
tuple_field_bad_out="$(new_tmp_file tuple-field-bad-out)"
tuple_no_context_in="$(new_tmp_file tuple-no-context)"
tuple_no_context_out="$(new_tmp_file tuple-no-context-out)"
array_init_in="$(new_tmp_file array-init)"
array_init_out="$(new_tmp_file array-init-out)"
array_group_in="$(new_tmp_file array-group)"
array_group_out="$(new_tmp_file array-group-out)"
array_mixed_in="$(new_tmp_file array-mixed)"
array_mixed_out="$(new_tmp_file array-mixed-out)"
array_value_init_in="$(new_tmp_file array-value-init)"
array_value_init_out="$(new_tmp_file array-value-init-out)"
array_bad_dim_in="$(new_tmp_file array-bad-dim)"
array_bad_dim_out="$(new_tmp_file array-bad-dim-out)"

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
grep -Fq '"declaredType": "i32[4][5]"' "$milestone_json_out"
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

cat >"$tuple_flow_in" <<'EOF'
def echo(pair <i32, bool>) <i32, bool> {
    ret pair
}

def main() i32 {
    var pair <i32, bool> = (1, true)
    var out <i32, bool> = echo(pair)
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$tuple_flow_in" >"$tuple_flow_out"
grep -q '^define { i32, i1 } @echo' "$tuple_flow_out"
grep -q 'call { i32, i1 } @echo' "$tuple_flow_out"

cat >"$tuple_field_in" <<'EOF'
def echo(pair <i32, bool>) <i32, bool> {
    ret pair
}

def main() i32 {
    var pair <i32, bool> = (1, true)
    pair._1 = 7
    if echo(pair)._2 {
        ret echo(pair)._1
    }
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$tuple_field_in" >"$tuple_field_out"
grep -Fq 'extractvalue { i32, i1 }' "$tuple_field_out"
grep -Fq 'getelementptr inbounds { i32, i1 }' "$tuple_field_out"
grep -Fq 'store i32 7' "$tuple_field_out"

cat >"$tuple_field_bad_in" <<'EOF'
def main() i32 {
    var pair <i32, bool> = (1, true)
    ret pair._3
}
EOF
expect_emit_ir_failure "$tuple_field_bad_in" "$tuple_field_bad_out" 'expected invalid tuple field program to fail'
grep -Fq 'unknown tuple field `_3`' "$tuple_field_bad_out"
grep -Fq 'Tuple fields are named `_1`, `_2` in declaration order.' "$tuple_field_bad_out"

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
    matrix(1)(2) = 7
    ret matrix(1)(2)
}
EOF
"$BIN" --emit-ir --verify-ir "$array_init_in" >"$array_init_out"
grep -Fq 'alloca [5 x [4 x i32]]' "$array_init_out"
grep -Fq 'getelementptr inbounds [5 x [4 x i32]]' "$array_init_out"
grep -Fq 'store i32 7' "$array_init_out"
grep -Fq 'ret i32' "$array_init_out"

cat >"$array_group_in" <<'EOF'
def bad() i32 {
    var matrix i32[5, 4] = {}
    matrix(1, 2) = 9
    ret matrix(1, 2)
}
EOF
"$BIN" --emit-ir --verify-ir "$array_group_in" >"$array_group_out"
grep -Fq 'alloca [5 x [4 x i32]]' "$array_group_out"
grep -Fq 'getelementptr inbounds [5 x [4 x i32]]' "$array_group_out"
grep -Fq 'store i32 9' "$array_group_out"

cat >"$array_mixed_in" <<'EOF'
def bad() i32 {
    var tensor i32[3][4, 5] = {}
    tensor(1, 2)(0) = 11
    ret tensor(1, 2)(0)
}
EOF
"$BIN" --emit-ir --verify-ir "$array_mixed_in" >"$array_mixed_out"
grep -Fq 'alloca [4 x [5 x [3 x i32]]]' "$array_mixed_out"
grep -Fq 'getelementptr inbounds [4 x [5 x [3 x i32]]]' "$array_mixed_out"
grep -Fq 'store i32 11' "$array_mixed_out"

cat >"$array_value_init_in" <<'EOF'
def bad() i32 {
    var matrix i32[4][5] = {{1}}
    ret 0
}
EOF
expect_emit_ir_failure "$array_value_init_in" "$array_value_init_out" 'expected explicit array value initializer program to fail'
grep -Fq 'array initializers currently only support zero-initialization placeholders' "$array_value_init_out"

cat >"$array_bad_dim_in" <<'EOF'
def bad() i32 {
    var matrix i32[0][5] = {}
    ret 0
}
EOF
expect_emit_ir_failure "$array_bad_dim_in" "$array_bad_dim_out" 'expected invalid array dimension program to fail'
grep -Fq 'fixed-dimension arrays require positive integer literal sizes' "$array_bad_dim_out"
