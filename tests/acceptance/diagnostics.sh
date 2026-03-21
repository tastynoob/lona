#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

syntax_diag_in="$(new_tmp_file syntax-diag)"
syntax_diag_out="$(new_tmp_file syntax-diag-out)"
semantic_diag_in="$(new_tmp_file semantic-diag)"
semantic_diag_out="$(new_tmp_file semantic-diag-out)"
duplicate_param_in="$(new_tmp_file duplicate-param)"
duplicate_param_out="$(new_tmp_file duplicate-param-out)"
missing_return_in="$(new_tmp_file missing-return)"
missing_return_out="$(new_tmp_file missing-return-out)"
missing_ret_value_in="$(new_tmp_file missing-ret-value)"
missing_ret_value_out="$(new_tmp_file missing-ret-value-out)"
import_diag_dir="$(new_tmp_dir import-diag)"
import_diag_dep_in="$import_diag_dir/bad_dep.lo"
import_diag_main_in="$import_diag_dir/main.lo"
import_diag_out="$(new_tmp_file import-diag-out)"

cat >"$syntax_diag_in" <<'EOF'
def bad() i32 {
    var x i32 =
    ret 0
}
EOF
expect_emit_ir_failure "$syntax_diag_in" "$syntax_diag_out" 'expected syntax diagnostic program to fail'
grep -Fq "syntax error: I couldn't parse this statement: unexpected newline." "$syntax_diag_out"
grep -Fq " --> $syntax_diag_in:2:16" "$syntax_diag_out"
grep -Fq ' 2 |     var x i32 =' "$syntax_diag_out"
grep -Fq 'help: Check for a missing separator, unmatched delimiter, or mistyped keyword near here.' "$syntax_diag_out"

cat >"$semantic_diag_in" <<'EOF'
def bad() i32 {
    ret foo
}
EOF
expect_emit_ir_failure "$semantic_diag_in" "$semantic_diag_out" 'expected semantic diagnostic program to fail'
grep -Fq 'semantic error: undefined identifier `foo`' "$semantic_diag_out"
grep -Fq " --> $semantic_diag_in:2:9" "$semantic_diag_out"
grep -Fq ' 2 |     ret foo' "$semantic_diag_out"
grep -Fq 'help: Declare it with `var` before using it, or check the spelling.' "$semantic_diag_out"

cat >"$duplicate_param_in" <<'EOF'
def bad(a i32, a i32) i32 {
    ret a
}
EOF
expect_emit_ir_failure "$duplicate_param_in" "$duplicate_param_out" 'expected duplicate parameter program to fail'
grep -Fq 'semantic error: duplicate function parameter `a`' "$duplicate_param_out"
grep -Fq " --> $duplicate_param_in:1:16" "$duplicate_param_out"
grep -Fq 'help: Rename one of the parameters so each binding is unique.' "$duplicate_param_out"

cat >"$missing_return_in" <<'EOF'
def bad(a i32) i32 {
    if a < 1 {
        ret 1
    }
}
EOF
expect_emit_ir_failure "$missing_return_in" "$missing_return_out" 'expected all-paths missing return program to fail'
grep -Fq 'semantic error: missing return value' "$missing_return_out"
grep -Fq " --> $missing_return_in:1:5" "$missing_return_out"
grep -Fq ' 1 | def bad(a i32) i32 {' "$missing_return_out"

cat >"$missing_ret_value_in" <<'EOF'
def bad() i32 {
    ret
}
EOF
expect_emit_ir_failure "$missing_ret_value_in" "$missing_ret_value_out" 'expected bare ret in non-void function to fail'
grep -Fq 'semantic error: missing return value' "$missing_ret_value_out"
grep -Fq " --> $missing_ret_value_in:2:5" "$missing_ret_value_out"
grep -Fq ' 2 |     ret' "$missing_ret_value_out"

cat >"$import_diag_dep_in" <<'EOF'
def bad(flag bool) i32 {
    if flag {
        ret 1
    }
}
EOF
cat >"$import_diag_main_in" <<'EOF'
import bad_dep

def main() i32 {
    ret bad_dep.bad(false)
}
EOF
expect_emit_ir_failure "$import_diag_main_in" "$import_diag_out" 'expected imported implementation diagnostic to fail in defining module'
grep -Fq 'semantic error: missing return value' "$import_diag_out"
grep -Fq " --> $import_diag_dep_in:1:5" "$import_diag_out"
grep -Fq ' 1 | def bad(flag bool) i32 {' "$import_diag_out"
