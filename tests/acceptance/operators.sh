#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

operator_ir_in="$(new_tmp_file operator-ir)"
operator_ir_out="$(new_tmp_file operator-ir-out)"
operator_bad_in="$(new_tmp_file operator-bad)"
operator_bad_out="$(new_tmp_file operator-bad-out)"
short_circuit_in="$(new_tmp_file short-circuit)"
short_circuit_bin="$(new_tmp_file short-circuit-bin)"
mixed_sign_in="$(new_tmp_file mixed-sign)"
mixed_sign_out="$(new_tmp_file mixed-sign-out)"
mixed_sign_bin="$(new_tmp_file mixed-sign-bin)"

cat >"$operator_ir_in" <<'EOF'
def compute(a i32, b i32, flag bool) bool {
    var mix i32 = a % b
    mix = mix + (a << 1)
    mix = mix - (b >> 1)
    mix = mix ^ ~a
    mix = mix | (a & b)
    ret (mix <= a) || (flag && (b >= a))
}

def float_cmp(a f32, b f32) bool {
    ret (a + b) >= (a - b)
}

def unsigned_mix(a u32, b u32) u32 {
    ret (a / b) + (a % b) + (a >> b)
}

def bool_bits(a bool, b bool) bool {
    ret (a & b) | (a ^ b)
}

def ptr_same(v i32) bool {
    var value i32 = v
    var ptr i32* = &value
    ret ptr == ptr
}

def main() i32 {
    var ok bool = compute(9, 4, false)
    var cmp bool = float_cmp(3.0, 1.0)
    var bits bool = bool_bits(true, false)
    var same bool = ptr_same(7)
    if ok || cmp || bits || same {
        ret 1
    }
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$operator_ir_in" >"$operator_ir_out"
grep -Fq 'srem i32' "$operator_ir_out"
grep -Fq 'udiv i32' "$operator_ir_out"
grep -Fq 'urem i32' "$operator_ir_out"
grep -Fq 'shl i32' "$operator_ir_out"
grep -Fq 'ashr i32' "$operator_ir_out"
grep -Fq 'lshr i32' "$operator_ir_out"
grep -Fq 'and i32' "$operator_ir_out"
grep -Fq 'and i1' "$operator_ir_out"
grep -Fq 'xor i32' "$operator_ir_out"
grep -Fq 'xor i1' "$operator_ir_out"
grep -Fq 'or i32' "$operator_ir_out"
grep -Fq 'or i1' "$operator_ir_out"
grep -Fq 'icmp sle' "$operator_ir_out"
grep -Fq 'icmp sge' "$operator_ir_out"
grep -Fq 'icmp eq ptr' "$operator_ir_out"
grep -Fq 'phi i1' "$operator_ir_out"
grep -Fq 'fadd float' "$operator_ir_out"
grep -Fq 'fsub float' "$operator_ir_out"
grep -Fq 'fcmp oge float' "$operator_ir_out"

cat >"$operator_bad_in" <<'EOF'
def bad(a f32, b f32) f32 {
    ret a & b
}
EOF
expect_emit_ir_failure "$operator_bad_in" "$operator_bad_out" 'expected invalid float bitwise program to fail'
grep -Fq 'operator `&` doesn'"'"'t support `f32` and `f32`' "$operator_bad_out"

cat >"$short_circuit_in" <<'EOF'
def mark(ptr i32*, value i32) bool {
    *ptr = value
    ret true
}

def main() i32 {
    var state i32 = 0
    false && mark(&state, 1)
    true || mark(&state, 2)
    ret state
}
EOF
bash "$ROOT/scripts/lac.sh" "$short_circuit_in" "$short_circuit_bin"
set +e
"$short_circuit_bin"
short_circuit_status=$?
set -e
if [ "$short_circuit_status" -ne 0 ]; then
    echo "expected short-circuit program to exit with 0, got $short_circuit_status" >&2
    exit 1
fi

cat >"$mixed_sign_in" <<'EOF'
def less(a i64, b u8) bool {
    ret a < b
}

def divide(a i64, b u8) i64 {
    ret a / b
}

def shift(a i64, b u8) i64 {
    ret a >> b
}

def main() i32 {
    var a i64 = -1
    var b u8 = 1
    if less(a, b) && divide(a, b) == -1 && shift(a, b) == -1 {
        ret 0
    }
    ret 1
}
EOF
"$BIN" --emit-ir --verify-ir "$mixed_sign_in" >"$mixed_sign_out"
grep -Fq 'icmp slt i64' "$mixed_sign_out"
grep -Fq 'sdiv i64' "$mixed_sign_out"
grep -Fq 'ashr i64' "$mixed_sign_out"
bash "$ROOT/scripts/lac.sh" "$mixed_sign_in" "$mixed_sign_bin"
set +e
"$mixed_sign_bin"
mixed_sign_status=$?
set -e
if [ "$mixed_sign_status" -ne 0 ]; then
    echo "expected mixed signedness program to exit with 0, got $mixed_sign_status" >&2
    exit 1
fi
