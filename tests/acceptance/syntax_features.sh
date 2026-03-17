#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

milestone_json_in="$(new_tmp_file milestone-json)"
milestone_json_out="$(new_tmp_file milestone-json-out)"
legacy_cast_in="$(new_tmp_file legacy-cast)"
legacy_cast_out="$(new_tmp_file legacy-cast-out)"
float_in="$(new_tmp_file float)"
float_out="$(new_tmp_file float-out)"
numeric_convert_in="$(new_tmp_file numeric-convert)"
numeric_convert_out="$(new_tmp_file numeric-convert-out)"
tobits_in="$(new_tmp_file tobits)"
tobits_out="$(new_tmp_file tobits-out)"
tobits_infer_in="$(new_tmp_file tobits-infer)"
tobits_infer_out="$(new_tmp_file tobits-infer-out)"
wide_bits_in="$(new_tmp_file wide-bits)"
wide_bits_out="$(new_tmp_file wide-bits-out)"
implicit_numeric_in="$(new_tmp_file implicit-numeric)"
implicit_numeric_out="$(new_tmp_file implicit-numeric-out)"
mixed_numeric_op_in="$(new_tmp_file mixed-numeric-op)"
mixed_numeric_op_out="$(new_tmp_file mixed-numeric-op-out)"
numeric_cross_bad_in="$(new_tmp_file numeric-cross-bad)"
numeric_cross_bad_out="$(new_tmp_file numeric-cross-bad-out)"
injected_member_bad_in="$(new_tmp_file injected-member-bad)"
injected_member_bad_out="$(new_tmp_file injected-member-bad-out)"
float_literal_target_bad_in="$(new_tmp_file float-literal-target-bad)"
float_literal_target_bad_out="$(new_tmp_file float-literal-target-bad-out)"
float_nan_cmp_in="$(new_tmp_file float-nan-cmp)"
float_nan_cmp_out="$(new_tmp_file float-nan-cmp-out)"
bool_bytecopy_bad_in="$(new_tmp_file bool-bytecopy-bad)"
bool_bytecopy_bad_out="$(new_tmp_file bool-bytecopy-bad-out)"
string_placeholder_in="$(new_tmp_file string-placeholder)"
string_placeholder_out="$(new_tmp_file string-placeholder-out)"
initial_list_hidden_in="$(new_tmp_file initial-list-hidden)"
initial_list_hidden_out="$(new_tmp_file initial-list-hidden-out)"
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
tuple_array_in="$(new_tmp_file tuple-array)"
tuple_array_out="$(new_tmp_file tuple-array-out)"
array_init_in="$(new_tmp_file array-init)"
array_init_out="$(new_tmp_file array-init-out)"
array_group_in="$(new_tmp_file array-group)"
array_group_out="$(new_tmp_file array-group-out)"
array_mixed_in="$(new_tmp_file array-mixed)"
array_mixed_out="$(new_tmp_file array-mixed-out)"
array_value_init_in="$(new_tmp_file array-value-init)"
array_value_init_out="$(new_tmp_file array-value-init-out)"
array_overflow_in="$(new_tmp_file array-overflow)"
array_overflow_out="$(new_tmp_file array-overflow-out)"
array_bad_dim_in="$(new_tmp_file array-bad-dim)"
array_bad_dim_out="$(new_tmp_file array-bad-dim-out)"
array_bad_shape_in="$(new_tmp_file array-bad-shape)"
array_bad_shape_out="$(new_tmp_file array-bad-shape-out)"
array_bad_depth_in="$(new_tmp_file array-bad-depth)"
array_bad_depth_out="$(new_tmp_file array-bad-depth-out)"
struct_init_in="$(new_tmp_file struct-init)"
struct_init_out="$(new_tmp_file struct-init-out)"
named_call_bad_in="$(new_tmp_file named-call-bad)"
named_call_bad_out="$(new_tmp_file named-call-bad-out)"
named_call_mix_in="$(new_tmp_file named-call-mix)"
named_call_mix_out="$(new_tmp_file named-call-mix-out)"
named_call_order_in="$(new_tmp_file named-call-order)"
named_call_order_out="$(new_tmp_file named-call-order-out)"
ctor_unknown_field_in="$(new_tmp_file ctor-unknown-field)"
ctor_unknown_field_out="$(new_tmp_file ctor-unknown-field-out)"

cat >"$milestone_json_in" <<'EOF'
struct Complex {
    real i32
    img i32
}

def mix(x i32, y i32) i32 {
    ret x + y
}

def show() {
    var pair <i32, bool> = (1, true)
    var matrix i32[4][5] = {1, 2, 3, 4}
    var c = Complex(real = 1, img = 2)
    var p = math.Point(x = 1)
    ret mix(y = c.img, x = c.real)
}
EOF
"$BIN" "$milestone_json_in" >"$milestone_json_out"
grep -Fq '"declaredType": "<i32, bool>"' "$milestone_json_out"
grep -Fq '"type": "TupleLiteral"' "$milestone_json_out"
grep -Fq '"declaredType": "i32[4][5]"' "$milestone_json_out"
grep -Fq '"type": "BraceInit"' "$milestone_json_out"
grep -Fq '"type": "FieldCall"' "$milestone_json_out"
grep -Fq '"type": "NamedCallArg"' "$milestone_json_out"
grep -Fq '"kind": "positional"' "$milestone_json_out"
grep -Fq '"name": "math"' "$milestone_json_out"

cat >"$legacy_cast_in" <<'EOF'
def bad() i32 {
    var x i32 = i32 1
    ret x
}
EOF
expect_emit_ir_failure "$legacy_cast_in" "$legacy_cast_out" 'expected legacy cast syntax program to fail'
grep -Fq "lona doesn't support C-style cast syntax like \`i32 1\`." "$legacy_cast_out"
grep -Fq '.toi32()' "$legacy_cast_out"

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

cat >"$numeric_convert_in" <<'EOF'
def main() i32 {
    var base i32 = 1.5.toi32()
    var sample f32 = base.tof32()
    var promoted f64 = sample
    ret promoted.toi32()
}
EOF
"$BIN" --emit-ir --verify-ir "$numeric_convert_in" >"$numeric_convert_out"
grep -q 'fptosi double' "$numeric_convert_out"
grep -q 'sitofp i32' "$numeric_convert_out"
grep -q 'fpext float' "$numeric_convert_out"

cat >"$tobits_in" <<'EOF'
def main() i32 {
    var v i8 = -1
    var raw u8[1] = v.tobits()
    var wide i32 = raw.toi32()
    ret wide
}
EOF
"$BIN" --emit-ir --verify-ir "$tobits_in" >"$tobits_out"
grep -q 'insertvalue \[1 x i8\]' "$tobits_out"
grep -q 'extractvalue \[1 x i8\]' "$tobits_out"
grep -q 'zext i8' "$tobits_out"
! grep -q 'sext i8' "$tobits_out"

cat >"$tobits_infer_in" <<'EOF'
def main() i32 {
    var raw = 7.tobits()
    ret raw.toi32()
}
EOF
"$BIN" --emit-ir --verify-ir "$tobits_infer_in" >"$tobits_infer_out"
grep -q 'alloca \[4 x i8\]' "$tobits_infer_out"
grep -q 'store \[4 x i8\] c"\\07\\00\\00\\00"' "$tobits_infer_out"
grep -q 'extractvalue \[4 x i8\]' "$tobits_infer_out"

cat >"$wide_bits_in" <<'EOF'
def main() i32 {
    var raw u8[256] = {}
    ret raw.toi32()
}
EOF
"$BIN" --emit-ir --verify-ir "$wide_bits_in" >"$wide_bits_out"
extract_count="$(grep -c 'extractvalue \[256 x i8\]' "$wide_bits_out")"
if [ "$extract_count" -ne 4 ]; then
    echo "expected wide raw-toi32 program to extract exactly 4 bytes, got $extract_count" >&2
    exit 1
fi
! grep -q 'i2048' "$wide_bits_out"

cat >"$implicit_numeric_in" <<'EOF'
def widen(v u8) u16 {
    ret v
}

def main() i32 {
    var x u8 = 1
    ret widen(x)
}
EOF
"$BIN" --emit-ir --verify-ir "$implicit_numeric_in" >"$implicit_numeric_out"
grep -q 'store i8 1' "$implicit_numeric_out"
grep -q 'zext i8' "$implicit_numeric_out"
grep -q 'zext i16' "$implicit_numeric_out"

cat >"$mixed_numeric_op_in" <<'EOF'
def main() i32 {
    var a u8 = 1
    var b i32 = 2
    ret a + b
}
EOF
"$BIN" --emit-ir --verify-ir "$mixed_numeric_op_in" >"$mixed_numeric_op_out"
grep -q 'zext i8' "$mixed_numeric_op_out"
grep -q 'add i32' "$mixed_numeric_op_out"

cat >"$numeric_cross_bad_in" <<'EOF'
def main() i32 {
    var x f32 = 1
    ret 0
}
EOF
expect_emit_ir_failure "$numeric_cross_bad_in" "$numeric_cross_bad_out" 'expected implicit int-to-float program to fail'
grep -Fq 'initializer type mismatch for `x`: expected f32, got i32' "$numeric_cross_bad_out"
grep -Fq '.toXXX()' "$numeric_cross_bad_out"

cat >"$injected_member_bad_in" <<'EOF'
def main() i32 {
    var x = 1.tof32
    ret 0
}
EOF
expect_emit_ir_failure "$injected_member_bad_in" "$injected_member_bad_out" 'expected non-call injected member program to fail'
grep -Fq 'injected member `tof32` can only be used as a direct call callee' "$injected_member_bad_out"

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

cat >"$initial_list_hidden_in" <<'EOF'
def bad() i32 {
    var x initial_list
    ret 0
}
EOF
expect_emit_ir_failure "$initial_list_hidden_in" "$initial_list_hidden_out" 'expected internal initial_list type use to fail'
grep -Fq '`initial_list` is a compiler-internal initialization interface' "$initial_list_hidden_out"
grep -Fq 'Use brace initialization like `{1, 2, 3}` instead.' "$initial_list_hidden_out"

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

cat >"$tuple_array_in" <<'EOF'
def main() i32 {
    var items <i32, bool>[2] = {(1, true), (2, false)}
    if items(0)._2 {
        ret items(0)._1
    }
    ret items(1)._1
}
EOF
"$BIN" --emit-ir --verify-ir "$tuple_array_in" >"$tuple_array_out"
grep -Fq 'alloca [2 x { i32, i1 }]' "$tuple_array_out"
grep -Fq 'store [2 x { i32, i1 }]' "$tuple_array_out"

cat >"$array_init_in" <<'EOF'
def bad() i32 {
    var matrix i32[4][5] = {{1}, {2}}
    matrix(1)(2) = 7
    ret matrix(1)(2)
}
EOF
"$BIN" --emit-ir --verify-ir "$array_init_in" >"$array_init_out"
grep -Fq 'alloca [5 x [4 x i32]]' "$array_init_out"
grep -Fq 'getelementptr inbounds [5 x [4 x i32]]' "$array_init_out"
grep -Fq '[4 x i32] [i32 1, i32 0, i32 0, i32 0]' "$array_init_out"
grep -Fq '[4 x i32] [i32 2, i32 0, i32 0, i32 0]' "$array_init_out"
grep -Fq 'store i32 7' "$array_init_out"
grep -Fq 'ret i32' "$array_init_out"

cat >"$array_group_in" <<'EOF'
def bad() i32 {
    var matrix i32[5, 4] = {{1, 2}, {3}}
    matrix(1, 2) = 9
    ret matrix(1, 2)
}
EOF
"$BIN" --emit-ir --verify-ir "$array_group_in" >"$array_group_out"
grep -Fq 'alloca [5 x [4 x i32]]' "$array_group_out"
grep -Fq 'getelementptr inbounds [5 x [4 x i32]]' "$array_group_out"
grep -Fq '[4 x i32] [i32 1, i32 2, i32 0, i32 0]' "$array_group_out"
grep -Fq '[4 x i32] [i32 3, i32 0, i32 0, i32 0]' "$array_group_out"
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
def main() i32 {
    var row i32[4] = {1, 2}
    ret row(0) + row(1) + row(2) + row(3)
}
EOF
"$BIN" --emit-ir --verify-ir "$array_value_init_in" >"$array_value_init_out"
grep -Fq 'store [4 x i32] [i32 1, i32 2, i32 0, i32 0]' "$array_value_init_out"

cat >"$array_bad_dim_in" <<'EOF'
def bad() i32 {
    var matrix i32[0][5] = {}
    ret 0
}
EOF
expect_emit_ir_failure "$array_bad_dim_in" "$array_bad_dim_out" 'expected invalid array dimension program to fail'
grep -Fq 'fixed-dimension arrays require positive integer literal sizes' "$array_bad_dim_out"

cat >"$array_overflow_in" <<'EOF'
def bad() i32 {
    var a i32[2] = {1, 2, 3}
    ret 0
}
EOF
expect_emit_ir_failure "$array_overflow_in" "$array_overflow_out" 'expected overflowing array initializer program to fail'
grep -Fq 'array initializer has too many elements' "$array_overflow_out"

cat >"$array_bad_shape_in" <<'EOF'
def bad() i32 {
    var a i32[4][5] = {1, 2}
    ret 0
}
EOF
expect_emit_ir_failure "$array_bad_shape_in" "$array_bad_shape_out" 'expected mismatched nested array initializer program to fail'
grep -Fq 'array initializer expects a nested brace group at index 0' "$array_bad_shape_out"

cat >"$array_bad_depth_in" <<'EOF'
def bad() i32 {
    var a i32[1] = {{1}}
    ret 0
}
EOF
expect_emit_ir_failure "$array_bad_depth_in" "$array_bad_depth_out" 'expected too-deep array initializer program to fail'
grep -Fq 'array initializer nesting is deeper than the array shape' "$array_bad_depth_out"

cat >"$struct_init_in" <<'EOF'
struct Complex {
    real i32
    img i32
}

def fold(real i32, img i32) i32 {
    ret real + img
}

def main() i32 {
    var c = Complex(img = 2, real = 1)
    ret fold(img = c.img, real = c.real)
}
EOF
"$BIN" --emit-ir --verify-ir "$struct_init_in" >"$struct_init_out"
grep -Fq 'store %' "$struct_init_out"
grep -Fq 'Complex { i32 1, i32 2 }' "$struct_init_out"
grep -Fq 'call i32 @fold(i32' "$struct_init_out"

cat >"$named_call_bad_in" <<'EOF'
def mix(x i32, y i32) i32 {
    ret x + y
}

def bad() i32 {
    ret mix(x = 1)
}
EOF
expect_emit_ir_failure "$named_call_bad_in" "$named_call_bad_out" 'expected missing named argument program to fail'
grep -Fq 'missing parameter `y` for function call' "$named_call_bad_out"

cat >"$named_call_mix_in" <<'EOF'
def mix(x i32, y i32) i32 {
    ret x + y
}

def main() i32 {
    ret mix(1, y = 2)
}
EOF
"$BIN" --emit-ir --verify-ir "$named_call_mix_in" >"$named_call_mix_out"
grep -Fq 'call i32 @mix(i32 1, i32 2)' "$named_call_mix_out"

cat >"$named_call_order_in" <<'EOF'
def mix(x i32, y i32) i32 {
    ret x + y
}

def bad() i32 {
    ret mix(x = 1, 2)
}
EOF
expect_emit_ir_failure "$named_call_order_in" "$named_call_order_out" 'expected out-of-order positional/named argument program to fail'
grep -Fq 'positional arguments must come before named arguments' "$named_call_order_out"

cat >"$ctor_unknown_field_in" <<'EOF'
struct Complex {
    real i32
    img i32
}

def bad() i32 {
    var c = Complex(real = 1, phase = 2)
    ret 0
}
EOF
expect_emit_ir_failure "$ctor_unknown_field_in" "$ctor_unknown_field_out" 'expected unknown constructor field program to fail'
grep -Fq 'unknown field `phase` for type constructor `' "$ctor_unknown_field_out"
grep -Fq 'Complex' "$ctor_unknown_field_out"
