#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

func_ptr_in="$(new_tmp_file func-ptr)"
func_ptr_out="$(new_tmp_file func-ptr-out)"
func_ptr_void_in="$(new_tmp_file func-ptr-void)"
func_ptr_void_out="$(new_tmp_file func-ptr-void-out)"
func_ptr_ref_in="$(new_tmp_file func-ptr-ref)"
func_ptr_ref_out="$(new_tmp_file func-ptr-ref-out)"
func_ptr_ref_bad_in="$(new_tmp_file func-ptr-ref-bad)"
func_ptr_ref_bad_out="$(new_tmp_file func-ptr-ref-bad-out)"
func_ptr_bad_in="$(new_tmp_file func-ptr-bad)"
func_ptr_bad_out="$(new_tmp_file func-ptr-bad-out)"
func_ptr_uninit_in="$(new_tmp_file func-ptr-uninit)"
func_ptr_uninit_out="$(new_tmp_file func-ptr-uninit-out)"
func_ptr_small_agg_in="$(new_tmp_file func-ptr-small-agg)"
func_ptr_small_agg_out="$(new_tmp_file func-ptr-small-agg-out)"
func_array_uninit_in="$(new_tmp_file func-array-uninit)"
func_array_uninit_out="$(new_tmp_file func-array-uninit-out)"
func_name_conflict_in="$(new_tmp_file func-name-conflict)"
func_name_conflict_out="$(new_tmp_file func-name-conflict-out)"
struct_name_conflict_in="$(new_tmp_file struct-name-conflict)"
struct_name_conflict_out="$(new_tmp_file struct-name-conflict-out)"
type_member_bad_in="$(new_tmp_file type-member-bad)"
type_member_bad_out="$(new_tmp_file type-member-bad-out)"
func_param_bad_in="$(new_tmp_file func-param-bad)"
func_param_bad_out="$(new_tmp_file func-param-bad-out)"
func_local_bad_in="$(new_tmp_file func-local-bad)"
func_local_bad_out="$(new_tmp_file func-local-bad-out)"
func_top_bad_in="$(new_tmp_file func-top-bad)"
func_top_bad_out="$(new_tmp_file func-top-bad-out)"
func_inferred_local_bad_in="$(new_tmp_file func-inferred-local-bad)"
func_inferred_local_bad_out="$(new_tmp_file func-inferred-local-bad-out)"
func_inferred_top_bad_in="$(new_tmp_file func-inferred-top-bad)"
func_inferred_top_bad_out="$(new_tmp_file func-inferred-top-bad-out)"
func_inferred_method_local_bad_in="$(new_tmp_file func-inferred-method-local-bad)"
func_inferred_method_local_bad_out="$(new_tmp_file func-inferred-method-local-bad-out)"
func_inferred_method_top_bad_in="$(new_tmp_file func-inferred-method-top-bad)"
func_inferred_method_top_bad_out="$(new_tmp_file func-inferred-method-top-bad-out)"
func_method_return_bad_in="$(new_tmp_file func-method-return-bad)"
func_method_return_bad_out="$(new_tmp_file func-method-return-bad-out)"
func_method_arg_bad_in="$(new_tmp_file func-method-arg-bad)"
func_method_arg_bad_out="$(new_tmp_file func-method-arg-bad-out)"
func_method_expr_bad_in="$(new_tmp_file func-method-expr-bad)"
func_method_expr_bad_out="$(new_tmp_file func-method-expr-bad-out)"
func_method_top_expr_bad_in="$(new_tmp_file func-method-top-expr-bad)"
func_method_top_expr_bad_out="$(new_tmp_file func-method-top-expr-bad-out)"
call_arg_type_bad_in="$(new_tmp_file call-arg-type-bad)"
call_arg_type_bad_out="$(new_tmp_file call-arg-type-bad-out)"
call_arg_count_bad_in="$(new_tmp_file call-arg-count-bad)"
call_arg_count_bad_out="$(new_tmp_file call-arg-count-bad-out)"
return_type_bad_in="$(new_tmp_file return-type-bad)"
return_type_bad_out="$(new_tmp_file return-type-bad-out)"

cat >"$func_ptr_in" <<'EOF'
def foo(v i32) i32 {
    ret v
}

def hold() i32 {
    var cb (i32)* i32 = foo&<i32>
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$func_ptr_in" >"$func_ptr_out"
grep -q '^define i32 @foo' "$func_ptr_out"
grep -q '^define i32 @hold' "$func_ptr_out"
grep -q 'store ptr @foo' "$func_ptr_out"

cat >"$func_ptr_void_in" <<'EOF'
def ping() {}

def hold() i32 {
    var cb ()* = ping&<>
    cb()
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$func_ptr_void_in" >"$func_ptr_void_out"
grep -q '^define void @ping' "$func_ptr_void_out"
grep -q 'store ptr @ping' "$func_ptr_void_out"
grep -Eq 'call void %.*\(\)' "$func_ptr_void_out"

cat >"$func_ptr_ref_in" <<'EOF'
def set7(ref v i32) i32 {
    v = 7
    ret v
}

def hold() i32 {
    var cb (ref i32)* i32 = set7&<ref i32>
    var x i32 = 1
    ret cb(ref x)
}
EOF
"$BIN" --emit-ir --verify-ir "$func_ptr_ref_in" >"$func_ptr_ref_out"
grep -q '^define i32 @set7(ptr ' "$func_ptr_ref_out"
grep -q 'store ptr @set7' "$func_ptr_ref_out"
grep -Eq 'call i32 %.*\(ptr ' "$func_ptr_ref_out"

cat >"$func_ptr_ref_bad_in" <<'EOF'
def set7(ref v i32) i32 {
    v = 7
    ret v
}

def hold() i32 {
    var cb = set7&<i32>
    var x i32 = 1
    ret cb(ref x)
}
EOF
expect_emit_ir_failure "$func_ptr_ref_bad_in" "$func_ptr_ref_bad_out" 'expected mismatched ref function pointer reference program to fail'
grep -q 'function reference parameter type mismatch at index 0 for `set7`: expected ref i32, got i32' "$func_ptr_ref_bad_out"

cat >"$func_ptr_bad_in" <<'EOF'
def foo(v i32) i32 {
    ret v
}

def hold() i32 {
    var cb = foo&<bool>
    ret 0
}
EOF
expect_emit_ir_failure "$func_ptr_bad_in" "$func_ptr_bad_out" 'expected mismatched function pointer reference program to fail'
grep -q 'function reference parameter type mismatch at index 0 for `foo`: expected i32, got bool' "$func_ptr_bad_out"

cat >"$func_ptr_uninit_in" <<'EOF'
def bad_holder() i32 {
    var cb (i32)* i32
    ret 0
}
EOF
expect_emit_ir_failure "$func_ptr_uninit_in" "$func_ptr_uninit_out" 'expected uninitialized function pointer variable program to fail'
grep -Fq 'function pointer variable type for `cb` requires initializer: (i32)* i32' "$func_ptr_uninit_out"

cat >"$func_ptr_small_agg_in" <<'EOF'
struct Pair {
    left i32
    right i32
}

def echo(v Pair) Pair {
    ret v
}

def hold() i32 {
    var cb (Pair)* Pair = echo&<Pair>
    var pair = Pair(left = 1, right = 2)
    ret cb(pair).right
}
EOF
"$BIN" --emit-ir --verify-ir "$func_ptr_small_agg_in" >"$func_ptr_small_agg_out"
grep -q 'store ptr @echo' "$func_ptr_small_agg_out"
grep -Eq '^define i64 @echo\(i64 [^)]+\)' "$func_ptr_small_agg_out"
grep -Eq 'call i64 %.*\(i64 %.*\)' "$func_ptr_small_agg_out"

cat >"$func_array_uninit_in" <<'EOF'
def bad_table() i32 {
    var table ()[] i32
    ret 0
}
EOF
expect_emit_ir_failure "$func_array_uninit_in" "$func_array_uninit_out" 'expected uninitialized function array variable program to fail'
grep -Fq 'bare function signatures are not allowed in type positions.' "$func_array_uninit_out"
grep -Fq 'Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.' "$func_array_uninit_out"

cat >"$func_name_conflict_in" <<'EOF'
struct Counter {
    value i32
}

def Counter(value i32) i32 {
    ret value
}
EOF
expect_emit_ir_failure "$func_name_conflict_in" "$func_name_conflict_out" 'expected top-level function name conflict with struct to fail'
grep -Fq 'top-level function `Counter` conflicts with struct `Counter`' "$func_name_conflict_out"
grep -Fq 'Type names reserve constructor syntax like `Counter(...)`.' "$func_name_conflict_out"

cat >"$struct_name_conflict_in" <<'EOF'
def Counter(value i32) i32 {
    ret value
}

struct Counter {
    value i32
}
EOF
expect_emit_ir_failure "$struct_name_conflict_in" "$struct_name_conflict_out" 'expected struct name conflict with top-level function to fail'
grep -Fq 'struct `Counter` conflicts with top-level function `Counter`' "$struct_name_conflict_out"
grep -Fq 'Type names reserve constructor syntax like `Counter(...)`.' "$struct_name_conflict_out"

cat >"$type_member_bad_in" <<'EOF'
struct Counter {
    value i32
}

def main() i32 {
    ret Counter.zero()
}
EOF
expect_emit_ir_failure "$type_member_bad_in" "$type_member_bad_out" 'expected static type member program to fail'
grep -Fq 'unknown type member `Counter.zero`' "$type_member_bad_out"
grep -Fq 'Static type members are not implemented yet.' "$type_member_bad_out"

cat >"$func_param_bad_in" <<'EOF'
def bad_callback(cb () i32) i32 {
    ret 0
}
EOF
expect_emit_ir_failure "$func_param_bad_in" "$func_param_bad_out" 'expected bare function parameter program to fail'
grep -Fq 'bare function signatures are not allowed in type positions.' "$func_param_bad_out"
grep -Fq 'Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.' "$func_param_bad_out"

cat >"$func_local_bad_in" <<'EOF'
def bad_local() i32 {
    var cb () i32
    ret 0
}
EOF
expect_emit_ir_failure "$func_local_bad_in" "$func_local_bad_out" 'expected bare function local variable program to fail'
grep -Fq 'bare function signatures are not allowed in type positions.' "$func_local_bad_out"
grep -Fq 'Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.' "$func_local_bad_out"

cat >"$func_top_bad_in" <<'EOF'
var cb () i32
EOF
expect_emit_ir_failure "$func_top_bad_in" "$func_top_bad_out" 'expected bare function top-level variable program to fail'
grep -Fq 'bare function signatures are not allowed in type positions.' "$func_top_bad_out"
grep -Fq 'Use an explicit function pointer type like `()*` or `(T1, T2)* Ret` instead.' "$func_top_bad_out"

cat >"$func_inferred_local_bad_in" <<'EOF'
def foo() i32 {
    ret 1
}

def bad_inferred_local() i32 {
    var cb = foo
    ret 0
}
EOF
expect_emit_ir_failure "$func_inferred_local_bad_in" "$func_inferred_local_bad_out" 'expected inferred bare function local variable program to fail'
grep -q 'unsupported bare function variable type for `cb`: () i32' "$func_inferred_local_bad_out"

cat >"$func_inferred_top_bad_in" <<'EOF'
def foo() i32 {
    ret 1
}

var cb = foo
EOF
expect_emit_ir_failure "$func_inferred_top_bad_in" "$func_inferred_top_bad_out" 'expected inferred bare function top-level variable program to fail'
grep -q 'unsupported bare function variable type for `cb`: () i32' "$func_inferred_top_bad_out"

cat >"$func_inferred_method_local_bad_in" <<'EOF'
struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def bad_method_local() i32 {
    var c = Complex(1, 2)
    var cb = c.add
    ret 0
}
EOF
expect_emit_ir_failure "$func_inferred_method_local_bad_in" "$func_inferred_method_local_bad_out" 'expected inferred bare method local variable program to fail'
grep -Eq 'unsupported bare function variable type for `cb`: \([^)]*Complex\) [^ ]*Complex' "$func_inferred_method_local_bad_out"

cat >"$func_inferred_method_top_bad_in" <<'EOF'
struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

var sample = Complex(1, 2)
var cb = sample.add
EOF
expect_emit_ir_failure "$func_inferred_method_top_bad_in" "$func_inferred_method_top_bad_out" 'expected inferred bare method top-level variable program to fail'
grep -Eq 'unsupported bare function variable type for `cb`: \([^)]*Complex\) [^ ]*Complex' "$func_inferred_method_top_bad_out"

cat >"$func_method_return_bad_in" <<'EOF'
struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def bad_method_return() Complex {
    var c = Complex(1, 2)
    ret c.add
}
EOF
expect_emit_ir_failure "$func_method_return_bad_in" "$func_method_return_bad_out" 'expected bare method return program to fail'
grep -q 'method selector can only be used as a direct call callee' "$func_method_return_bad_out"

cat >"$func_method_arg_bad_in" <<'EOF'
struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def take(v Complex) Complex {
    ret v
}

def bad_method_arg() Complex {
    var c = Complex(1, 2)
    ret take(c.add)
}
EOF
expect_emit_ir_failure "$func_method_arg_bad_in" "$func_method_arg_bad_out" 'expected bare method argument program to fail'
grep -q 'method selector can only be used as a direct call callee' "$func_method_arg_bad_out"

cat >"$func_method_expr_bad_in" <<'EOF'
struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def bad_method_expr() i32 {
    var c = Complex(1, 2)
    c.add
    ret 0
}
EOF
expect_emit_ir_failure "$func_method_expr_bad_in" "$func_method_expr_bad_out" 'expected bare method expression-statement program to fail'
grep -q 'method selector can only be used as a direct call callee' "$func_method_expr_bad_out"

cat >"$func_method_top_expr_bad_in" <<'EOF'
struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

var sample = Complex(1, 2)
sample.add
EOF
expect_emit_ir_failure "$func_method_top_expr_bad_in" "$func_method_top_expr_bad_out" 'expected bare top-level method expression program to fail'
grep -q 'method selector can only be used as a direct call callee' "$func_method_top_expr_bad_out"

cat >"$call_arg_type_bad_in" <<'EOF'
def foo(v i32) i32 {
    ret v
}

def bad_call_type() i32 {
    ret foo(true)
}
EOF
expect_emit_ir_failure "$call_arg_type_bad_in" "$call_arg_type_bad_out" 'expected call argument type mismatch program to fail'
grep -q 'call argument type mismatch at index 0: expected i32, got bool' "$call_arg_type_bad_out"

cat >"$call_arg_count_bad_in" <<'EOF'
def foo(v i32) i32 {
    ret v
}

def bad_call_count() i32 {
    ret foo()
}
EOF
expect_emit_ir_failure "$call_arg_count_bad_in" "$call_arg_count_bad_out" 'expected call argument count mismatch program to fail'
grep -q 'call argument count mismatch: expected 1, got 0' "$call_arg_count_bad_out"

cat >"$return_type_bad_in" <<'EOF'
def bad() i32 {
    ret true
}
EOF
expect_emit_ir_failure "$return_type_bad_in" "$return_type_bad_out" 'expected return type mismatch program to fail'
grep -q 'return type mismatch: expected i32, got bool' "$return_type_bad_out"
