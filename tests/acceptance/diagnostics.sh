#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

syntax_diag_in="$(new_tmp_file syntax-diag)"
syntax_diag_out="$(new_tmp_file syntax-diag-out)"
semantic_diag_in="$(new_tmp_file semantic-diag)"
semantic_diag_out="$(new_tmp_file semantic-diag-out)"
duplicate_param_in="$(new_tmp_file duplicate-param)"
duplicate_param_out="$(new_tmp_file duplicate-param-out)"

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
