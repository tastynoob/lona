#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/lona"
INPUT="$ROOT/tests/fixtures/acceptance_main.lo"
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
func_ptr_in="$(mktemp "$TMPDIR_LOCAL/lona-func-ptr-XXXXXX.lo")"
func_ptr_out="$(mktemp "$TMPDIR_LOCAL/lona-func-ptr-XXXXXX.ll")"
func_ptr_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-ptr-bad-XXXXXX.lo")"
func_ptr_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-ptr-bad-XXXXXX.txt")"
func_ptr_uninit_in="$(mktemp "$TMPDIR_LOCAL/lona-func-ptr-uninit-XXXXXX.lo")"
func_ptr_uninit_out="$(mktemp "$TMPDIR_LOCAL/lona-func-ptr-uninit-XXXXXX.txt")"
func_array_uninit_in="$(mktemp "$TMPDIR_LOCAL/lona-func-array-uninit-XXXXXX.lo")"
func_array_uninit_out="$(mktemp "$TMPDIR_LOCAL/lona-func-array-uninit-XXXXXX.txt")"
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
func_inferred_method_local_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-method-local-bad-XXXXXX.lo")"
func_inferred_method_local_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-method-local-bad-XXXXXX.txt")"
func_inferred_method_top_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-method-top-bad-XXXXXX.lo")"
func_inferred_method_top_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-inferred-method-top-bad-XXXXXX.txt")"
func_method_return_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-method-return-bad-XXXXXX.lo")"
func_method_return_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-method-return-bad-XXXXXX.txt")"
func_method_arg_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-method-arg-bad-XXXXXX.lo")"
func_method_arg_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-method-arg-bad-XXXXXX.txt")"
func_method_expr_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-method-expr-bad-XXXXXX.lo")"
func_method_expr_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-method-expr-bad-XXXXXX.txt")"
func_method_top_expr_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-func-method-top-expr-bad-XXXXXX.lo")"
func_method_top_expr_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-func-method-top-expr-bad-XXXXXX.txt")"
call_arg_type_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-call-arg-type-bad-XXXXXX.lo")"
call_arg_type_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-call-arg-type-bad-XXXXXX.txt")"
call_arg_count_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-call-arg-count-bad-XXXXXX.lo")"
call_arg_count_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-call-arg-count-bad-XXXXXX.txt")"
return_type_bad_in="$(mktemp "$TMPDIR_LOCAL/lona-return-type-bad-XXXXXX.lo")"
return_type_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-return-type-bad-XXXXXX.txt")"
syntax_diag_in="$(mktemp "$TMPDIR_LOCAL/lona-syntax-diag-XXXXXX.lo")"
syntax_diag_out="$(mktemp "$TMPDIR_LOCAL/lona-syntax-diag-XXXXXX.txt")"
semantic_diag_in="$(mktemp "$TMPDIR_LOCAL/lona-semantic-diag-XXXXXX.lo")"
semantic_diag_out="$(mktemp "$TMPDIR_LOCAL/lona-semantic-diag-XXXXXX.txt")"
duplicate_param_in="$(mktemp "$TMPDIR_LOCAL/lona-duplicate-param-XXXXXX.lo")"
duplicate_param_out="$(mktemp "$TMPDIR_LOCAL/lona-duplicate-param-XXXXXX.txt")"
method_self_in="$(mktemp "$TMPDIR_LOCAL/lona-method-self-XXXXXX.lo")"
method_self_out="$(mktemp "$TMPDIR_LOCAL/lona-method-self-XXXXXX.ll")"
top_level_mix_in="$(mktemp "$TMPDIR_LOCAL/lona-top-level-mix-XXXXXX.lo")"
top_level_mix_out="$(mktemp "$TMPDIR_LOCAL/lona-top-level-mix-XXXXXX.ll")"
import_dir="$(mktemp -d "$TMPDIR_LOCAL/lona-import-XXXXXX")"
import_dep_in="$import_dir/math.lo"
import_main_in="$import_dir/main.lo"
import_main_out="$(mktemp "$TMPDIR_LOCAL/lona-import-main-XXXXXX.ll")"
import_type_out="$(mktemp "$TMPDIR_LOCAL/lona-import-type-XXXXXX.ll")"
import_mid_in="$import_dir/mid.lo"
import_leaf_in="$import_dir/leaf.lo"
import_transitive_main_in="$import_dir/transitive_main.lo"
import_transitive_ok_out="$(mktemp "$TMPDIR_LOCAL/lona-import-transitive-ok-XXXXXX.ll")"
import_transitive_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-import-transitive-bad-XXXXXX.txt")"
import_transitive_type_bad_out="$(mktemp "$TMPDIR_LOCAL/lona-import-transitive-type-bad-XXXXXX.txt")"
import_exec_dep_in="$import_dir/bad_dep.lo"
import_exec_main_in="$import_dir/bad_main.lo"
import_exec_out="$(mktemp "$TMPDIR_LOCAL/lona-import-exec-XXXXXX.txt")"
large_struct_return_in="$(mktemp "$TMPDIR_LOCAL/lona-large-struct-return-XXXXXX.lo")"
large_struct_return_out="$(mktemp "$TMPDIR_LOCAL/lona-large-struct-return-XXXXXX.ll")"
grammar_subset_in="$(mktemp "$TMPDIR_LOCAL/lona-grammar-subset-XXXXXX.lo")"
grammar_subset_out="$(mktemp "$TMPDIR_LOCAL/lona-grammar-subset-XXXXXX.ll")"
cleanup() {
    rm -f "$json_out" "$ir_out" "$debug_out" "$missing_return_in" \
        "$json_feature_in" "$json_feature_out" "$bool_in" "$bool_out" \
        "$pointer_in" "$pointer_out" "$func_ptr_in" "$func_ptr_out" \
        "$func_ptr_bad_in" "$func_ptr_bad_out" \
        "$func_ptr_uninit_in" "$func_ptr_uninit_out" \
        "$func_array_uninit_in" "$func_array_uninit_out" \
        "$func_param_bad_in" "$func_param_bad_out" \
        "$func_local_bad_in" "$func_local_bad_out" "$func_top_bad_in" "$func_top_bad_out" \
        "$func_inferred_local_bad_in" "$func_inferred_local_bad_out" \
        "$func_inferred_top_bad_in" "$func_inferred_top_bad_out" \
        "$func_inferred_method_local_bad_in" "$func_inferred_method_local_bad_out" \
        "$func_inferred_method_top_bad_in" "$func_inferred_method_top_bad_out" \
        "$func_method_return_bad_in" "$func_method_return_bad_out" \
        "$func_method_arg_bad_in" "$func_method_arg_bad_out" \
        "$func_method_expr_bad_in" "$func_method_expr_bad_out" \
        "$func_method_top_expr_bad_in" "$func_method_top_expr_bad_out" \
        "$call_arg_type_bad_in" "$call_arg_type_bad_out" \
        "$call_arg_count_bad_in" "$call_arg_count_bad_out" \
        "$return_type_bad_in" "$return_type_bad_out" \
        "$syntax_diag_in" "$syntax_diag_out" \
        "$semantic_diag_in" "$semantic_diag_out" \
        "$duplicate_param_in" "$duplicate_param_out" \
        "$method_self_in" "$method_self_out" \
        "$top_level_mix_in" "$top_level_mix_out" \
        "$import_main_out" "$import_type_out" \
        "$import_transitive_ok_out" "$import_transitive_bad_out" "$import_transitive_type_bad_out" \
        "$import_exec_dep_in" "$import_exec_main_in" "$import_exec_out" \
        "$large_struct_return_in" "$large_struct_return_out" \
        "$grammar_subset_in" "$grammar_subset_out"
    rm -rf "$import_dir"
}
trap cleanup EXIT

"$BIN" "$INPUT" >"$json_out"
grep -q '"type": "Program"' "$json_out"
grep -q '"type": "FieldCall"' "$json_out"

"$BIN" --emit-ir --verify-ir "$INPUT" >"$ir_out"
grep -Eq '^define %.*Complex @.*Complex\.add' "$ir_out"
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

func_ptr_source='def foo(v i32) i32 {
    ret v
}

def hold() i32 {
    var cb (i32)* i32 = foo&<i32>
    ret 0
}
'
printf '%s' "$func_ptr_source" >"$func_ptr_in"
"$BIN" --emit-ir --verify-ir "$func_ptr_in" >"$func_ptr_out"
grep -q '^define i32 @foo' "$func_ptr_out"
grep -q '^define i32 @hold' "$func_ptr_out"
grep -q 'store ptr @foo' "$func_ptr_out"

func_ptr_bad_source='def foo(v i32) i32 {
    ret v
}

def hold() i32 {
    var cb = foo&<bool>
    ret 0
}
'
printf '%s' "$func_ptr_bad_source" >"$func_ptr_bad_in"
if "$BIN" --emit-ir "$func_ptr_bad_in" >"$func_ptr_bad_out" 2>&1; then
    echo 'expected mismatched function pointer reference program to fail' >&2
    exit 1
fi
grep -q 'function reference parameter type mismatch at index 0 for `foo`: expected i32, got bool' "$func_ptr_bad_out"

func_ptr_uninit_source='def bad_holder() i32 {
    var cb (i32)* i32
    ret 0
}
'
printf '%s' "$func_ptr_uninit_source" >"$func_ptr_uninit_in"
if "$BIN" --emit-ir "$func_ptr_uninit_in" >"$func_ptr_uninit_out" 2>&1; then
    echo 'expected uninitialized function pointer variable program to fail' >&2
    exit 1
fi
grep -q 'function-related variable type for `cb` requires initializer: (i32) i32*' "$func_ptr_uninit_out"

func_array_uninit_source='def bad_table() i32 {
    var table ()[] i32
    ret 0
}
'
printf '%s' "$func_array_uninit_source" >"$func_array_uninit_in"
if "$BIN" --emit-ir "$func_array_uninit_in" >"$func_array_uninit_out" 2>&1; then
    echo 'expected uninitialized function array variable program to fail' >&2
    exit 1
fi
grep -Fq 'function-related variable type for `table` requires initializer: () i32[]' "$func_array_uninit_out"

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

func_inferred_method_local_bad_source='struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def Complex(a i32, b i32) Complex {
    var out Complex
    out.real = a
    out.imag = b
    ret out
}

def bad_method_local() i32 {
    var c = Complex(1, 2)
    var cb = c.add
    ret 0
}
'
printf '%s' "$func_inferred_method_local_bad_source" >"$func_inferred_method_local_bad_in"
if "$BIN" --emit-ir "$func_inferred_method_local_bad_in" >"$func_inferred_method_local_bad_out" 2>&1; then
    echo 'expected inferred bare method local variable program to fail' >&2
    exit 1
fi
grep -Eq 'unsupported bare function variable type for `cb`: \([^)]*Complex\) [^ ]*Complex' "$func_inferred_method_local_bad_out"

func_inferred_method_top_bad_source='struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def Complex(a i32, b i32) Complex {
    var out Complex
    out.real = a
    out.imag = b
    ret out
}

var sample = Complex(1, 2)
var cb = sample.add
'
printf '%s' "$func_inferred_method_top_bad_source" >"$func_inferred_method_top_bad_in"
if "$BIN" --emit-ir "$func_inferred_method_top_bad_in" >"$func_inferred_method_top_bad_out" 2>&1; then
    echo 'expected inferred bare method top-level variable program to fail' >&2
    exit 1
fi
grep -Eq 'unsupported bare function variable type for `cb`: \([^)]*Complex\) [^ ]*Complex' "$func_inferred_method_top_bad_out"

func_method_return_bad_source='struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def Complex(a i32, b i32) Complex {
    var out Complex
    out.real = a
    out.imag = b
    ret out
}

def bad_method_return() Complex {
    var c = Complex(1, 2)
    ret c.add
}
'
printf '%s' "$func_method_return_bad_source" >"$func_method_return_bad_in"
if "$BIN" --emit-ir "$func_method_return_bad_in" >"$func_method_return_bad_out" 2>&1; then
    echo 'expected bare method return program to fail' >&2
    exit 1
fi
grep -q 'method selector can only be used as a direct call callee' "$func_method_return_bad_out"

func_method_arg_bad_source='struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def Complex(a i32, b i32) Complex {
    var out Complex
    out.real = a
    out.imag = b
    ret out
}

def take(v Complex) Complex {
    ret v
}

def bad_method_arg() Complex {
    var c = Complex(1, 2)
    ret take(c.add)
}
'
printf '%s' "$func_method_arg_bad_source" >"$func_method_arg_bad_in"
if "$BIN" --emit-ir "$func_method_arg_bad_in" >"$func_method_arg_bad_out" 2>&1; then
    echo 'expected bare method argument program to fail' >&2
    exit 1
fi
grep -q 'method selector can only be used as a direct call callee' "$func_method_arg_bad_out"

func_method_expr_bad_source='struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def Complex(a i32, b i32) Complex {
    var out Complex
    out.real = a
    out.imag = b
    ret out
}

def bad_method_expr() i32 {
    var c = Complex(1, 2)
    c.add
    ret 0
}
'
printf '%s' "$func_method_expr_bad_source" >"$func_method_expr_bad_in"
if "$BIN" --emit-ir "$func_method_expr_bad_in" >"$func_method_expr_bad_out" 2>&1; then
    echo 'expected bare method expression-statement program to fail' >&2
    exit 1
fi
grep -q 'method selector can only be used as a direct call callee' "$func_method_expr_bad_out"

func_method_top_expr_bad_source='struct Complex {
    real i32
    imag i32

    def add(a Complex) Complex {
        var out Complex
        out.real = self.real + a.real
        out.imag = self.imag + a.imag
        ret out
    }
}

def Complex(a i32, b i32) Complex {
    var out Complex
    out.real = a
    out.imag = b
    ret out
}

var sample = Complex(1, 2)
sample.add
'
printf '%s' "$func_method_top_expr_bad_source" >"$func_method_top_expr_bad_in"
if "$BIN" --emit-ir "$func_method_top_expr_bad_in" >"$func_method_top_expr_bad_out" 2>&1; then
    echo 'expected bare top-level method expression program to fail' >&2
    exit 1
fi
grep -q 'method selector can only be used as a direct call callee' "$func_method_top_expr_bad_out"

call_arg_type_bad_source='def foo(v i32) i32 {
    ret v
}

def bad_call_type() i32 {
    ret foo(true)
}
'
printf '%s' "$call_arg_type_bad_source" >"$call_arg_type_bad_in"
if "$BIN" --emit-ir "$call_arg_type_bad_in" >"$call_arg_type_bad_out" 2>&1; then
    echo 'expected call argument type mismatch program to fail' >&2
    exit 1
fi
grep -q 'call argument type mismatch at index 0: expected i32, got bool' "$call_arg_type_bad_out"

call_arg_count_bad_source='def foo(v i32) i32 {
    ret v
}

def bad_call_count() i32 {
    ret foo()
}
'
printf '%s' "$call_arg_count_bad_source" >"$call_arg_count_bad_in"
if "$BIN" --emit-ir "$call_arg_count_bad_in" >"$call_arg_count_bad_out" 2>&1; then
    echo 'expected call argument count mismatch program to fail' >&2
    exit 1
fi
grep -q 'call argument count mismatch: expected 1, got 0' "$call_arg_count_bad_out"

return_type_bad_source='def bad() i32 {
    ret true
}
'
printf '%s' "$return_type_bad_source" >"$return_type_bad_in"
if "$BIN" --emit-ir "$return_type_bad_in" >"$return_type_bad_out" 2>&1; then
    echo 'expected return type mismatch program to fail' >&2
    exit 1
fi
grep -q 'return type mismatch: expected i32, got bool' "$return_type_bad_out"

syntax_diag_source='def bad() i32 {
    var x i32 =
    ret 0
}
'
printf '%s' "$syntax_diag_source" >"$syntax_diag_in"
if "$BIN" --emit-ir "$syntax_diag_in" >"$syntax_diag_out" 2>&1; then
    echo 'expected syntax diagnostic program to fail' >&2
    exit 1
fi
grep -Fq "syntax error: I couldn't parse this statement: unexpected newline." "$syntax_diag_out"
grep -Fq " --> $syntax_diag_in:2:16" "$syntax_diag_out"
grep -Fq ' 2 |     var x i32 =' "$syntax_diag_out"
grep -Fq 'help: Check for a missing separator, unmatched delimiter, or mistyped keyword near here.' "$syntax_diag_out"

semantic_diag_source='def bad() i32 {
    ret foo
}
'
printf '%s' "$semantic_diag_source" >"$semantic_diag_in"
if "$BIN" --emit-ir "$semantic_diag_in" >"$semantic_diag_out" 2>&1; then
    echo 'expected semantic diagnostic program to fail' >&2
    exit 1
fi
grep -Fq 'semantic error: undefined identifier `foo`' "$semantic_diag_out"
grep -Fq " --> $semantic_diag_in:2:9" "$semantic_diag_out"
grep -Fq ' 2 |     ret foo' "$semantic_diag_out"
grep -Fq 'help: Declare it with `var` before using it, or check the spelling.' "$semantic_diag_out"

duplicate_param_source='def bad(a i32, a i32) i32 {
    ret a
}
'
printf '%s' "$duplicate_param_source" >"$duplicate_param_in"
if "$BIN" --emit-ir "$duplicate_param_in" >"$duplicate_param_out" 2>&1; then
    echo 'expected duplicate parameter program to fail' >&2
    exit 1
fi
grep -Fq 'semantic error: duplicate function parameter `a`' "$duplicate_param_out"
grep -Fq " --> $duplicate_param_in:1:16" "$duplicate_param_out"
grep -Fq 'help: Rename one of the parameters so each binding is unique.' "$duplicate_param_out"

method_self_source='struct Counter {
    value i32

    def bump(step i32) i32 {
        ret self.value + step
    }
}

def main() i32 {
    var c Counter
    c.value = 2
    ret c.bump(3)
}
'
printf '%s' "$method_self_source" >"$method_self_in"
"$BIN" --emit-ir --verify-ir "$method_self_in" >"$method_self_out"
grep -Eq '@.*Counter\.bump' "$method_self_out"
grep -Eq 'getelementptr inbounds %.*Counter' "$method_self_out"

top_level_mix_source='def inc(a i32) i32 {
    ret a + 1
}

var x i32 = 3
var y i32 = inc(x)
'
printf '%s' "$top_level_mix_source" >"$top_level_mix_in"
"$BIN" --emit-ir --verify-ir "$top_level_mix_in" >"$top_level_mix_out"
grep -q '\.main"()' "$top_level_mix_out"
grep -q '@inc' "$top_level_mix_out"

import_dep_source='def inc(v i32) i32 {
    ret v + 1
}

def helper(v i32) i32 {
    ret inc(v)
}

struct Point {
    x i32
}
'
printf '%s' "$import_dep_source" >"$import_dep_in"
printf 'import math\n\ndef main() i32 {\n    ret math.helper(4)\n}\n' >"$import_main_in"
"$BIN" --emit-ir --verify-ir "$import_main_in" >"$import_main_out"
grep -q '^define i32 @math.inc(i32' "$import_main_out"
grep -q '^define i32 @math.helper(i32' "$import_main_out"
grep -q 'call i32 @math.helper(i32 4)' "$import_main_out"
if grep -q '^declare i32 @math.helper' "$import_main_out"; then
    echo 'expected linked module IR to resolve imported function declarations' >&2
    exit 1
fi

printf 'import math\n\ndef main() i32 {\n    var p math.Point\n    p.x = math.inc(4)\n    ret p.x\n}\n' >"$import_main_in"
"$BIN" --emit-ir --verify-ir "$import_main_in" >"$import_type_out"
grep -q '^%math.Point = type { i32 }' "$import_type_out"
grep -q 'call i32 @math.inc(i32 4)' "$import_type_out"

printf 'def inc(v i32) i32 {\n    ret v + 1\n}\n\nstruct Point {\n    x i32\n}\n' >"$import_leaf_in"
printf 'import leaf\n\ndef call_leaf(v i32) i32 {\n    ret leaf.inc(v)\n}\n' >"$import_mid_in"
printf 'import mid\n\ndef main() i32 {\n    ret mid.call_leaf(4)\n}\n' >"$import_transitive_main_in"
"$BIN" --emit-ir --verify-ir "$import_transitive_main_in" >"$import_transitive_ok_out"
grep -q 'call i32 @mid.call_leaf(i32 4)' "$import_transitive_ok_out"

printf 'import mid\n\ndef main() i32 {\n    ret leaf.inc(4)\n}\n' >"$import_transitive_main_in"
if "$BIN" --emit-ir "$import_transitive_main_in" >"$import_transitive_bad_out" 2>&1; then
    echo 'expected transitive import function access to fail' >&2
    exit 1
fi
grep -Fq 'semantic error: undefined identifier `leaf`' "$import_transitive_bad_out"

printf 'import mid\n\ndef main() i32 {\n    var p leaf.Point\n    ret 0\n}\n' >"$import_transitive_main_in"
if "$BIN" --emit-ir "$import_transitive_main_in" >"$import_transitive_type_bad_out" 2>&1; then
    echo 'expected transitive import type access to fail' >&2
    exit 1
fi
grep -Fq 'semantic error: unknown variable type' "$import_transitive_type_bad_out"
grep -Fq 'var p leaf.Point' "$import_transitive_type_bad_out"

import_exec_dep_source='var x i32 = 1
'
printf '%s' "$import_exec_dep_source" >"$import_exec_dep_in"
printf 'import bad_dep\n\ndef main() i32 {\n    ret 0\n}\n' >"$import_exec_main_in"
if "$BIN" --emit-ir "$import_exec_main_in" >"$import_exec_out" 2>&1; then
    echo 'expected imported top-level executable statement program to fail' >&2
    exit 1
fi
grep -Fq "imported file \`$import_exec_dep_in\` cannot contain top-level executable statements" "$import_exec_out"
grep -Fq 'help: Move this statement into a function, or keep top-level execution only in the root file.' "$import_exec_out"

large_struct_return_source='struct Big {
    a i32
    b i32
    c i32
    d i32
    e i32

    def add(v i32) Big {
        var out Big
        out.a = self.a + v
        out.b = self.b + v
        out.c = self.c + v
        out.d = self.d + v
        out.e = self.e + v
        ret out
    }
}

def Big(v i32) Big {
    var out Big
    out.a = v
    out.b = v + 1
    out.c = v + 2
    out.d = v + 3
    out.e = v + 4
    ret out
}

def make_big(v i32) Big {
    var base = Big(v)
    ret base.add(1)
}

var sample = make_big(3)
'
printf '%s' "$large_struct_return_source" >"$large_struct_return_in"
"$BIN" --emit-ir --verify-ir "$large_struct_return_in" >"$large_struct_return_out"
grep -Eq '^%.*Big = type \{ i32, i32, i32, i32, i32 \}' "$large_struct_return_out"
grep -Eq '^define void @.*Big\.add\(ptr ' "$large_struct_return_out"
grep -Eq '^define void @.*Big\(ptr ' "$large_struct_return_out"
grep -q '^define void @make_big(ptr ' "$large_struct_return_out"
grep -Eq 'call void @.*Big\(ptr ' "$large_struct_return_out"
grep -Eq 'call void @.*Big\.add\(ptr ' "$large_struct_return_out"

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
grep -Eq '^%.*Name = type \{ i32, i32 \}' "$grammar_subset_out"
grep -Eq '^define %.*Name @make_name' "$grammar_subset_out"

printf 'acceptance checks passed\n'
