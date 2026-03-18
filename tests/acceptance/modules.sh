#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

method_self_in="$(new_tmp_file method-self)"
method_self_out="$(new_tmp_file method-self-out)"
top_level_mix_in="$(new_tmp_file top-level-mix)"
top_level_mix_out="$(new_tmp_file top-level-mix-out)"
import_dir="$(new_tmp_dir import)"
import_dep_in="$import_dir/math.lo"
import_main_in="$import_dir/main.lo"
import_main_out="$(new_tmp_file import-main-out)"
import_type_out="$(new_tmp_file import-type-out)"
import_mid_in="$import_dir/mid.lo"
import_leaf_in="$import_dir/leaf.lo"
import_transitive_main_in="$import_dir/transitive-main.lo"
import_transitive_ok_out="$(new_tmp_file import-transitive-ok-out)"
import_transitive_bad_out="$(new_tmp_file import-transitive-bad-out)"
import_transitive_type_bad_out="$(new_tmp_file import-transitive-type-bad-out)"
import_exec_dep_in="$import_dir/bad_dep.lo"
import_exec_main_in="$import_dir/bad_main.lo"
import_exec_out="$(new_tmp_file import-exec-out)"
large_struct_return_in="$(new_tmp_file large-struct-return)"
large_struct_return_out="$(new_tmp_file large-struct-return-out)"
grammar_subset_in="$(new_tmp_file grammar-subset)"
grammar_subset_out="$(new_tmp_file grammar-subset-out)"
import_named_method_dir="$(new_tmp_dir import-named-method)"
import_named_method_dep_in="$import_named_method_dir/dep.lo"
import_named_method_main_in="$import_named_method_dir/main.lo"
import_named_method_out="$(new_tmp_file import-named-method-out)"

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
EOF
"$BIN" --emit-ir --verify-ir "$large_struct_return_in" >"$large_struct_return_out"
grep -Eq '^%.*Big = type \{ i32, i32, i32, i32, i32 \}' "$large_struct_return_out"
grep -Eq '^define void @.*Big\.add\(ptr ' "$large_struct_return_out"
grep -Eq '^define void @.*Big\(ptr ' "$large_struct_return_out"
grep -q '^define void @make_big(ptr ' "$large_struct_return_out"
grep -Eq 'call void @.*Big\(ptr ' "$large_struct_return_out"
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
grep -Eq '^define %.*Name @make_name' "$grammar_subset_out"
