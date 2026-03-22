#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

method_self_in="$(new_tmp_file method-self)"
method_self_out="$(new_tmp_file method-self-out)"
method_self_mutate_in="$(new_tmp_file method-self-mutate)"
method_self_mutate_out="$(new_tmp_file method-self-mutate-out)"
top_level_mix_in="$(new_tmp_file top-level-mix)"
top_level_mix_out="$(new_tmp_file top-level-mix-out)"
field_method_chain_in="$(new_tmp_file field-method-chain)"
field_method_chain_out="$(new_tmp_file field-method-chain-out)"
import_dir="$(new_tmp_dir import)"
import_dep_in="$import_dir/math.lo"
import_main_in="$import_dir/main.lo"
import_main_out="$(new_tmp_file import-main-out)"
import_type_out="$(new_tmp_file import-type-out)"
import_chain_out="$(new_tmp_file import-chain-out)"
import_module_call_bad_out="$(new_tmp_file import-module-call-bad-out)"
import_unknown_member_out="$(new_tmp_file import-unknown-member-out)"
import_namespace_value_bad_out="$(new_tmp_file import-namespace-value-bad-out)"
import_function_value_bad_out="$(new_tmp_file import-function-value-bad-out)"
import_local_shadow_out="$(new_tmp_file import-local-shadow-out)"
import_top_level_shadow_out="$(new_tmp_file import-top-level-shadow-out)"
import_type_shadow_out="$(new_tmp_file import-type-shadow-out)"
import_mid_in="$import_dir/mid.lo"
import_leaf_in="$import_dir/leaf.lo"
import_transitive_main_in="$import_dir/transitive-main.lo"
import_transitive_ok_out="$(new_tmp_file import-transitive-ok-out)"
import_transitive_bad_out="$(new_tmp_file import-transitive-bad-out)"
import_transitive_type_bad_out="$(new_tmp_file import-transitive-type-bad-out)"
import_exec_dep_in="$import_dir/bad_dep.lo"
import_exec_main_in="$import_dir/bad_main.lo"
import_exec_out="$(new_tmp_file import-exec-out)"
import_conflict_dep_in="$import_dir/conflict_dep.lo"
import_conflict_main_in="$import_dir/conflict_main.lo"
import_conflict_out="$(new_tmp_file import-conflict-out)"
large_struct_return_in="$(new_tmp_file large-struct-return)"
large_struct_return_out="$(new_tmp_file large-struct-return-out)"
grammar_subset_in="$(new_tmp_file grammar-subset)"
grammar_subset_out="$(new_tmp_file grammar-subset-out)"
import_named_method_dir="$(new_tmp_dir import-named-method)"
import_named_method_dep_in="$import_named_method_dir/dep.lo"
import_named_method_main_in="$import_named_method_dir/main.lo"
import_named_method_out="$(new_tmp_file import-named-method-out)"
import_c_abi_dir="$(new_tmp_dir import-c-abi)"
import_c_abi_dep_in="$import_c_abi_dir/dep.lo"
import_c_abi_main_in="$import_c_abi_dir/main.lo"
import_c_abi_out="$(new_tmp_file import-c-abi-out)"
import_c_repr_dir="$(new_tmp_dir import-c-repr)"
import_c_repr_dep_in="$import_c_repr_dir/dep.lo"
import_c_repr_main_in="$import_c_repr_dir/main.lo"
import_c_repr_out="$(new_tmp_file import-c-repr-out)"
import_c_native_ptr_bad_out="$(new_tmp_file import-c-native-ptr-bad-out)"
import_mutating_method_dir="$(new_tmp_dir import-mutating-method)"
import_mutating_method_dep_in="$import_mutating_method_dir/dep.lo"
import_mutating_method_main_in="$import_mutating_method_dir/main.lo"
import_mutating_method_out="$(new_tmp_file import-mutating-method-out)"
import_packed_aggregate_dir="$(new_tmp_dir import-packed-aggregate)"
import_packed_aggregate_dep_in="$import_packed_aggregate_dir/dep.lo"
import_packed_aggregate_main_in="$import_packed_aggregate_dir/main.lo"
import_packed_aggregate_out="$(new_tmp_file import-packed-aggregate-out)"
import_direct_return_dir="$(new_tmp_dir import-direct-return)"
import_direct_return_dep_in="$import_direct_return_dir/dep.lo"
import_direct_return_main_in="$import_direct_return_dir/main.lo"
import_direct_return_out="$(new_tmp_file import-direct-return-out)"

cat >"$method_self_in" <<'EOF'
struct Counter {
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
EOF
"$BIN" --emit-ir --verify-ir "$method_self_in" >"$method_self_out"
grep -Eq '@.*Counter\.bump' "$method_self_out"
grep -Eq 'getelementptr inbounds %.*Counter' "$method_self_out"

cat >"$method_self_mutate_in" <<'EOF'
struct Counter {
    value i32

    def bump(step i32) i32 {
        self.value = self.value + step
        ret self.value
    }
}

def main() i32 {
    var c Counter
    c.value = 2
    c.bump(3)
    ret c.value
}
EOF
"$BIN" --emit-ir --verify-ir "$method_self_mutate_in" >"$method_self_mutate_out"
grep -Eq '^define i32 @.*Counter\.bump\(ptr ' "$method_self_mutate_out"
grep -Eq 'call i32 @.*Counter\.bump\(ptr ' "$method_self_mutate_out"
grep -Eq 'store i32 .*ptr %' "$method_self_mutate_out"

cat >"$top_level_mix_in" <<'EOF'
def inc(a i32) i32 {
    ret a + 1
}

var x i32 = 3
var y i32 = inc(x)
EOF
"$BIN" --emit-ir --verify-ir "$top_level_mix_in" >"$top_level_mix_out"
grep -q '\.main"()' "$top_level_mix_out"
grep -q '@inc' "$top_level_mix_out"

cat >"$field_method_chain_in" <<'EOF'
struct Counter {
    value i32

    def read() i32 {
        ret self.value
    }
}

struct Wrapper {
    counter Counter
}

def main() i32 {
    var w = Wrapper(counter = Counter(value = 4))
    ret w.counter.read()
}
EOF
"$BIN" --emit-ir --verify-ir "$field_method_chain_in" >"$field_method_chain_out"
grep -Eq '@.*Counter\.read' "$field_method_chain_out"
grep -Eq 'call i32 @.*Counter\.read\(ptr ' "$field_method_chain_out"

cat >"$import_dep_in" <<'EOF'
def inc(v i32) i32 {
    ret v + 1
}

def helper(v i32) i32 {
    ret inc(v)
}

struct Point {
    x i32
}

struct Box {
    point Point
}
EOF
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

printf 'import math\n\ndef main() i32 {\n    ret math.Box(point = math.Point(x = 4)).point.x\n}\n' >"$import_main_in"
"$BIN" --emit-ir --verify-ir "$import_main_in" >"$import_chain_out"

cat >"$import_c_abi_dep_in" <<'EOF'
extern "C" def abs(v i32) i32

extern "C" def c_inc(v i32) i32 {
    ret abs(v) + 1
}

def native_wrap(v i32) i32 {
    ret c_inc(v)
}
EOF
cat >"$import_c_abi_main_in" <<'EOF'
import dep

def main() i32 {
    ret dep.native_wrap(-4) + dep.c_inc(-2)
}
EOF
"$BIN" --emit-ir --verify-ir "$import_c_abi_main_in" >"$import_c_abi_out"
grep -Fq 'declare i32 @abs(i32)' "$import_c_abi_out"
grep -Fq 'define i32 @c_inc(i32 ' "$import_c_abi_out"
grep -Fq 'define i32 @dep.native_wrap(i32 ' "$import_c_abi_out"
grep -Fq 'call i32 @dep.native_wrap(i32 -4)' "$import_c_abi_out"
grep -Fq 'call i32 @c_inc(i32 -2)' "$import_c_abi_out"
grep -Fq 'call i32 @c_inc(i32 %' "$import_c_abi_out"
grep -Fq 'call i32 @abs(i32 %' "$import_c_abi_out"
! grep -Fq '@dep.c_inc' "$import_c_abi_out"

cat >"$import_c_repr_dep_in" <<'EOF'
extern struct FILE

repr("C") struct Point {
    x i32
    y i32
}
EOF
cat >"$import_c_repr_main_in" <<'EOF'
import dep

extern "C" def shift(p dep.Point*, fp dep.FILE*) dep.Point*

def main() i32 {
    ret 0
}
EOF
"$BIN" --emit-ir --verify-ir "$import_c_repr_main_in" >"$import_c_repr_out"
grep -Fq 'declare ptr @shift(ptr, ptr)' "$import_c_repr_out"

cat >"$import_c_repr_dep_in" <<'EOF'
struct Pair {
    left i32
    right i32
}
EOF
cat >"$import_c_repr_main_in" <<'EOF'
import dep

extern "C" def bad(p dep.Pair*) i32
EOF
expect_emit_ir_failure "$import_c_repr_main_in" "$import_c_native_ptr_bad_out" 'expected imported native struct pointer over C FFI to fail'
grep -Fq 'semantic error: extern "C" function `bad` uses unsupported parameter `p`: dep.Pair*' "$import_c_native_ptr_bad_out"
grep -Fq 'help: Use pointers to scalars, pointers, `extern struct`, or `repr("C") struct` types. Ordinary Lona structs cannot cross the C FFI boundary.' "$import_c_native_ptr_bad_out"

cat >"$import_main_in" <<'EOF'
import math

def main() i32 {
    ret math(4)
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_module_call_bad_out" 'expected direct module call program to fail'
grep -Fq 'module `math` does not support call syntax' "$import_module_call_bad_out"
grep -Fq 'Call a concrete member like `math.func(...)` or `math.Type(...)` instead.' "$import_module_call_bad_out"

cat >"$import_main_in" <<'EOF'
import math

def main() i32 {
    ret math.missing(4)
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_unknown_member_out" 'expected unknown module member program to fail'
grep -Fq 'unknown module member `math.missing`' "$import_unknown_member_out"

cat >"$import_main_in" <<'EOF'
import math

def main() i32 {
    var ns = math
    ret 0
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_namespace_value_bad_out" 'expected bare module namespace value program to fail'
grep -Fq "module namespaces can't be used as runtime values" "$import_namespace_value_bad_out"

cat >"$import_main_in" <<'EOF'
import math

def main() i32 {
    var cb = math.inc
    ret 0
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_function_value_bad_out" 'expected imported bare function local variable program to fail'
grep -Fq 'unsupported bare function variable type for `cb`: (i32) i32' "$import_function_value_bad_out"

cat >"$import_main_in" <<'EOF'
import math

def main() i32 {
    var math i32 = 1
    ret math
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_local_shadow_out" 'expected local binding/import alias conflict to fail'
grep -Fq 'local binding `math` conflicts with imported module alias `math`' "$import_local_shadow_out"

cat >"$import_main_in" <<'EOF'
import math

def math() i32 {
    ret 0
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_top_level_shadow_out" 'expected top-level function/import alias conflict to fail'
grep -Fq 'top-level function `math` conflicts with imported module alias `math`' "$import_top_level_shadow_out"

cat >"$import_main_in" <<'EOF'
import math

struct math {
    value i32
}
EOF
expect_emit_ir_failure "$import_main_in" "$import_type_shadow_out" 'expected struct/import alias conflict to fail'
grep -Fq 'struct `math` conflicts with imported module alias `math`' "$import_type_shadow_out"

cat >"$import_named_method_dep_in" <<'EOF'
struct Vec2 {
    x i32
    y i32

    def add(dx i32, dy i32) i32 {
        ret self.x + self.y + dx + dy
    }
}
EOF
cat >"$import_named_method_main_in" <<'EOF'
import dep

def main() i32 {
    var v = dep.Vec2(x = 1, y = 2)
    ret v.add(dy = 4, dx = 3)
}
EOF
"$BIN" --emit-ir --verify-ir "$import_named_method_main_in" >"$import_named_method_out"
grep -q '^define i32 @dep.Vec2.add(ptr ' "$import_named_method_out"
grep -q 'call i32 @dep.Vec2.add(' "$import_named_method_out"

cat >"$import_mutating_method_dep_in" <<'EOF'
struct Counter {
    value i32

    def bump(step i32) i32 {
        self.value = self.value + step
        ret self.value
    }
}
EOF
cat >"$import_mutating_method_main_in" <<'EOF'
import dep

def main() i32 {
    var c dep.Counter
    c.value = 4
    c.bump(3)
    ret c.value
}
EOF
"$BIN" --emit-ir --verify-ir "$import_mutating_method_main_in" >"$import_mutating_method_out"
grep -q '^define i32 @dep.Counter.bump(ptr ' "$import_mutating_method_out"
grep -q 'call i32 @dep.Counter.bump(ptr ' "$import_mutating_method_out"
grep -Eq 'getelementptr inbounds %dep.Counter, ptr %0, i32 0, i32 0' "$import_mutating_method_out"

cat >"$import_packed_aggregate_dep_in" <<'EOF'
struct Pair {
    left i32
    right i32

    def swap(extra i32) Pair {
        var out Pair
        out.left = self.right + extra
        out.right = self.left + extra
        ret out
    }
}

def echo(v Pair) Pair {
    ret v
}
EOF
cat >"$import_packed_aggregate_main_in" <<'EOF'
import dep

def main() i32 {
    var pair = dep.Pair(left = 1, right = 2)
    ret dep.echo(pair).swap(3).left
}
EOF
"$BIN" --emit-ir --verify-ir "$import_packed_aggregate_main_in" >"$import_packed_aggregate_out"
grep -Eq '^define i64 @dep\.echo\(i64 [^)]+\)' "$import_packed_aggregate_out"
grep -Eq '^define i64 @dep\.Pair\.swap\(ptr [^,]+, i32 [^)]+\)' "$import_packed_aggregate_out"
grep -Eq 'call i64 @dep\.echo\(i64 %' "$import_packed_aggregate_out"
grep -Eq 'call i64 @dep\.Pair\.swap\(ptr [^,]+, i32 3\)' "$import_packed_aggregate_out"

cat >"$import_direct_return_dep_in" <<'EOF'
struct Triple {
    a i32
    b i32
    c i32

    def shift(delta i32) Triple {
        var out Triple
        out.a = self.b + delta
        out.b = self.c + delta
        out.c = self.a + delta
        ret out
    }
}

def echo(v Triple) Triple {
    ret v
}
EOF
cat >"$import_direct_return_main_in" <<'EOF'
import dep

def main() i32 {
    var triple = dep.Triple(a = 1, b = 2, c = 3)
    ret dep.echo(triple).shift(4).b
}
EOF
"$BIN" --emit-ir --verify-ir "$import_direct_return_main_in" >"$import_direct_return_out"
grep -Eq '^%dep\.Triple = type \{ i32, i32, i32 \}' "$import_direct_return_out"
grep -Eq '^define %dep\.Triple @dep\.echo\(ptr [^)]+\)' "$import_direct_return_out"
grep -Eq '^define %dep\.Triple @dep\.Triple\.shift\(ptr [^,]+, i32 [^)]+\)' "$import_direct_return_out"
grep -Eq 'call %dep\.Triple @dep\.echo\(ptr %' "$import_direct_return_out"
grep -Eq 'call %dep\.Triple @dep\.Triple\.shift\(ptr [^,]+, i32 4\)' "$import_direct_return_out"
! grep -q 'sret' "$import_direct_return_out"

printf 'def inc(v i32) i32 {\n    ret v + 1\n}\n\nstruct Point {\n    x i32\n}\n' >"$import_leaf_in"
printf 'import leaf\n\ndef call_leaf(v i32) i32 {\n    ret leaf.inc(v)\n}\n' >"$import_mid_in"
printf 'import mid\n\ndef main() i32 {\n    ret mid.call_leaf(4)\n}\n' >"$import_transitive_main_in"
"$BIN" --emit-ir --verify-ir "$import_transitive_main_in" >"$import_transitive_ok_out"
grep -q 'call i32 @mid.call_leaf(i32 4)' "$import_transitive_ok_out"

printf 'import mid\n\ndef main() i32 {\n    ret leaf.inc(4)\n}\n' >"$import_transitive_main_in"
expect_emit_ir_failure "$import_transitive_main_in" "$import_transitive_bad_out" 'expected transitive import function access to fail'
grep -Fq 'semantic error: undefined identifier `leaf`' "$import_transitive_bad_out"

printf 'import mid\n\ndef main() i32 {\n    var p leaf.Point\n    ret 0\n}\n' >"$import_transitive_main_in"
expect_emit_ir_failure "$import_transitive_main_in" "$import_transitive_type_bad_out" 'expected transitive import type access to fail'
grep -Fq 'semantic error: unknown variable type' "$import_transitive_type_bad_out"
grep -Fq 'var p leaf.Point' "$import_transitive_type_bad_out"

printf 'var x i32 = 1\n' >"$import_exec_dep_in"
printf 'import bad_dep\n\ndef main() i32 {\n    ret 0\n}\n' >"$import_exec_main_in"
expect_emit_ir_failure "$import_exec_main_in" "$import_exec_out" 'expected imported top-level executable statement program to fail'
grep -Fq "imported file \`$import_exec_dep_in\` cannot contain top-level executable statements" "$import_exec_out"
grep -Fq 'help: Move this statement into a function, or keep top-level execution only in the root file.' "$import_exec_out"

cat >"$import_conflict_dep_in" <<'EOF'
struct Counter {
    value i32
}

def Counter(value i32) i32 {
    ret value
}
EOF
printf 'import conflict_dep\n\ndef main() i32 {\n    ret 0\n}\n' >"$import_conflict_main_in"
expect_emit_ir_failure "$import_conflict_main_in" "$import_conflict_out" 'expected imported struct/function name conflict program to fail'
grep -Fq 'top-level function `Counter` conflicts with struct `Counter`' "$import_conflict_out"

cat >"$large_struct_return_in" <<'EOF'
struct Big {
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

def make_big(v i32) Big {
    var base = Big(v, v + 1, v + 2, v + 3, v + 4)
    ret base.add(1)
}

var sample = make_big(3)
EOF
"$BIN" --emit-ir --verify-ir "$large_struct_return_in" >"$large_struct_return_out"
grep -Eq '^%.*Big = type \{ i32, i32, i32, i32, i32 \}' "$large_struct_return_out"
grep -Eq '^define void @.*Big\.add\(ptr ' "$large_struct_return_out"
grep -q '^define void @make_big(ptr ' "$large_struct_return_out"
grep -Eq 'call void @.*Big\.add\(ptr ' "$large_struct_return_out"

cat >"$grammar_subset_in" <<'EOF'
struct Name {
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
EOF
"$BIN" --emit-ir --verify-ir "$grammar_subset_in" >"$grammar_subset_out"
grep -Eq '^%.*Name = type \{ i32, i32 \}' "$grammar_subset_out"
grep -Eq '^define i64 @make_name\(i32 [^,]+, i32 [^)]+\)' "$grammar_subset_out"
