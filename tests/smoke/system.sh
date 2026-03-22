#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_SYSTEM="$ROOT/scripts/lac.sh"
COMPILE_CASE="$ROOT/tests/tools/compile_case.sh"
TMPDIR_LOCAL="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMPDIR_LOCAL/lona-system-smoke-XXXXXX")"

if [ -n "${CC:-}" ]; then
    CC_BIN="$CC"
elif command -v cc >/dev/null 2>&1; then
    CC_BIN="cc"
elif command -v clang-18 >/dev/null 2>&1; then
    CC_BIN="clang-18"
else
    CC_BIN="clang"
fi

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

main_program="$WORKDIR/return_9.lo"
top_level_program="$WORKDIR/top_level.lo"
lona_import_c_program="$WORKDIR/import_abs.lo"
export_add_program="$WORKDIR/export_add.lo"
export_add_harness_c="$WORKDIR/export_add_harness.c"
export_add_ir="$WORKDIR/export_add.ll"
export_add_obj="$WORKDIR/export_add.o"
export_add_exe="$WORKDIR/export_add_harness"
repr_point_program="$WORKDIR/repr_point.lo"
repr_point_harness_c="$WORKDIR/repr_point_harness.c"
repr_point_ir="$WORKDIR/repr_point.ll"
repr_point_obj="$WORKDIR/repr_point.o"
repr_point_exe="$WORKDIR/repr_point_harness"
main_exe="$WORKDIR/return_9"
top_level_exe="$WORKDIR/top_level"
lona_import_c_exe="$WORKDIR/import_abs"

cat >"$main_program" <<'EOF'
def main() i32 {
    ret 9
}
EOF

cat >"$top_level_program" <<'EOF'
var x = 1
x = x + 2
EOF

cat >"$lona_import_c_program" <<'EOF'
extern "C" def abs(v i32) i32

def main() i32 {
    ret abs(-9)
}
EOF

cat >"$export_add_program" <<'EOF'
extern "C" def lona_add(a i32, b i32) i32 {
    ret a + b
}
EOF

cat >"$export_add_harness_c" <<'EOF'
extern int lona_add(int a, int b);

int main(void) {
    return lona_add(4, 5);
}
EOF

cat >"$repr_point_program" <<'EOF'
repr("C") struct Point {
    x i32
    y i32
}

extern "C" def shift(p Point*) i32 {
    (*p).x = (*p).x + 3
    (*p).y = (*p).y + 4
    ret (*p).x + (*p).y
}
EOF

cat >"$repr_point_harness_c" <<'EOF'
struct Point {
    int x;
    int y;
};

extern int shift(struct Point *p);

int main(void) {
    struct Point p = {1, 2};
    int sum = shift(&p);
    return (sum == 10 && p.x == 4 && p.y == 6) ? 0 : 1;
}
EOF

bash "$BUILD_SYSTEM" "$main_program" "$main_exe"
bash "$BUILD_SYSTEM" "$top_level_program" "$top_level_exe"
bash "$BUILD_SYSTEM" "$lona_import_c_program" "$lona_import_c_exe"
bash "$COMPILE_CASE" "$export_add_program" "$export_add_ir" "$export_add_obj" >/dev/null
"$CC_BIN" -Werror "$export_add_harness_c" "$export_add_obj" -o "$export_add_exe"
bash "$COMPILE_CASE" "$repr_point_program" "$repr_point_ir" "$repr_point_obj" >/dev/null
"$CC_BIN" -Werror "$repr_point_harness_c" "$repr_point_obj" -o "$repr_point_exe"

set +e
"$main_exe"
main_status=$?
"$top_level_exe"
top_level_status=$?
"$lona_import_c_exe"
import_c_status=$?
"$export_add_exe"
export_add_status=$?
"$repr_point_exe"
repr_point_status=$?
set -e

if [ "$main_status" -ne 9 ]; then
    echo "expected system main program to exit with 9, got $main_status" >&2
    exit 1
fi

if [ "$top_level_status" -ne 0 ]; then
    echo "expected system top-level program to exit with 0, got $top_level_status" >&2
    exit 1
fi

if [ "$import_c_status" -ne 9 ]; then
    echo "expected system extern C import program to exit with 9, got $import_c_status" >&2
    exit 1
fi

if [ "$export_add_status" -ne 9 ]; then
    echo "expected C harness calling lona export to exit with 9, got $export_add_status" >&2
    exit 1
fi

if [ "$repr_point_status" -ne 0 ]; then
    echo "expected repr(C) struct pointer interop harness to exit with 0, got $repr_point_status" >&2
    exit 1
fi

echo "system smoke passed"
