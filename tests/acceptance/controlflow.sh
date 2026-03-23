#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/lib.sh"

json_in="$(new_tmp_file controlflow-json)"
json_out="$(new_tmp_file controlflow-json-out)"
natural_else_in="$(new_tmp_file natural-else)"
natural_else_bin="$(new_tmp_file natural-else-bin)"
break_else_in="$(new_tmp_file break-else)"
break_else_bin="$(new_tmp_file break-else-bin)"
continue_else_in="$(new_tmp_file continue-else)"
continue_else_bin="$(new_tmp_file continue-else-bin)"
nested_break_in="$(new_tmp_file nested-break)"
nested_break_bin="$(new_tmp_file nested-break-bin)"
terminating_for_else_in="$(new_tmp_file terminating-for-else)"
terminating_for_else_out="$(new_tmp_file terminating-for-else-out)"
break_bad_in="$(new_tmp_file break-bad)"
break_bad_out="$(new_tmp_file break-bad-out)"
continue_bad_in="$(new_tmp_file continue-bad)"
continue_bad_out="$(new_tmp_file continue-bad-out)"
break_else_bad_in="$(new_tmp_file break-else-bad)"
break_else_bad_out="$(new_tmp_file break-else-bad-out)"

cat >"$json_in" <<'EOF'
def walk(limit i32) i32 {
    var i i32 = 0
    for i < limit {
        i = i + 1
        if i < limit {
            continue
        }
        break
    } else {
        ret 7
    }
    ret 0
}
EOF
"$BIN" "$json_in" >"$json_out"
grep -q '"type": "For"' "$json_out"
grep -q '"type": "Break"' "$json_out"
grep -q '"type": "Continue"' "$json_out"
grep -q '"else": {' "$json_out"

cat >"$natural_else_in" <<'EOF'
def main() i32 {
    var i i32 = 0
    var out i32 = 0
    for i < 3 {
        i = i + 1
    }
    else {
        out = 7
    }
    ret out
}
EOF
bash "$ROOT/scripts/lac.sh" "$natural_else_in" "$natural_else_bin"
set +e
"$natural_else_bin"
natural_else_status=$?
set -e
if [ "$natural_else_status" -ne 7 ]; then
    echo "expected naturally exhausted for-else program to exit with 7, got $natural_else_status" >&2
    exit 1
fi

cat >"$break_else_in" <<'EOF'
def main() i32 {
    var i i32 = 0
    var out i32 = 0
    for i < 3 {
        break
    } else {
        out = 9
    }
    ret out
}
EOF
bash "$ROOT/scripts/lac.sh" "$break_else_in" "$break_else_bin"
set +e
"$break_else_bin"
break_else_status=$?
set -e
if [ "$break_else_status" -ne 0 ]; then
    echo "expected break to skip for-else block and exit with 0, got $break_else_status" >&2
    exit 1
fi

cat >"$continue_else_in" <<'EOF'
def main() i32 {
    var i i32 = 0
    var out i32 = 0
    for i < 3 {
        i = i + 1
        if i < 3 {
            continue
        }
        out = 5
    } else {
        out = out + 2
    }
    ret out
}
EOF
bash "$ROOT/scripts/lac.sh" "$continue_else_in" "$continue_else_bin"
set +e
"$continue_else_bin"
continue_else_status=$?
set -e
if [ "$continue_else_status" -ne 7 ]; then
    echo "expected continue to preserve eventual for-else execution and exit with 7, got $continue_else_status" >&2
    exit 1
fi

cat >"$nested_break_in" <<'EOF'
def main() i32 {
    var outer i32 = 0
    for outer < 1 {
        outer = outer + 1
        var inner i32 = 0
        for inner < 2 {
            inner = inner + 1
            break
        } else {
            ret 1
        }
    } else {
        ret 0
    }
    ret 2
}
EOF
bash "$ROOT/scripts/lac.sh" "$nested_break_in" "$nested_break_bin"
set +e
"$nested_break_bin"
nested_break_status=$?
set -e
if [ "$nested_break_status" -ne 0 ]; then
    echo "expected inner break to skip only the inner else and let outer for-else finish with 0, got $nested_break_status" >&2
    exit 1
fi

cat >"$terminating_for_else_in" <<'EOF'
def choose(v bool) i32 {
    for v {
        ret 1
    } else {
        ret 2
    }
}

def main() i32 {
    ret choose(false)
}
EOF
"$BIN" --emit ir --verify-ir "$terminating_for_else_in" >"$terminating_for_else_out"
grep -q '^define i32 @choose' "$terminating_for_else_out"

cat >"$break_bad_in" <<'EOF'
def main() {
    break
}
EOF
expect_emit_ir_failure "$break_bad_in" "$break_bad_out" 'expected break outside loop program to fail'
grep -Fq '`break` can only appear inside `for` loops' "$break_bad_out"

cat >"$continue_bad_in" <<'EOF'
def main() {
    continue
}
EOF
expect_emit_ir_failure "$continue_bad_in" "$continue_bad_out" 'expected continue outside loop program to fail'
grep -Fq '`continue` can only appear inside `for` loops' "$continue_bad_out"

cat >"$break_else_bad_in" <<'EOF'
def main() {
    for false {
    } else {
        break
    }
}
EOF
expect_emit_ir_failure "$break_else_bad_in" "$break_else_bad_out" 'expected break inside for-else block program to fail'
grep -Fq '`break` can only appear inside `for` loops' "$break_else_bad_out"

echo "controlflow acceptance passed"
