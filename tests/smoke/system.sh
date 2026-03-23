#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/lona-ir"
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
bad_main_program="$WORKDIR/bad_main.lo"
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
list_crud_program="$WORKDIR/list_crud.lo"
list_crud_exe="$WORKDIR/list_crud"
linked_list_program="$ROOT/example/c_ffi_linked_list.lo"
linked_list_exe="$WORKDIR/linked_list"
main_exe="$WORKDIR/return_9"
main_obj="$WORKDIR/return_9.o"
top_level_exe="$WORKDIR/top_level"
lona_import_c_exe="$WORKDIR/import_abs"
bad_main_exe="$WORKDIR/bad_main"
bad_main_log="$WORKDIR/bad_main.log"

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

cat >"$bad_main_program" <<'EOF'
def main(argc i32) i32 {
    ret argc
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

cat >"$list_crud_program" <<'EOF'
repr("C") struct Slot {
    used i32
    value i32
    next i32
}

extern "C" def malloc(size u64) Slot*
extern "C" def free(p Slot*)
extern "C" def puts(msg i8*) i32

def list_init(slots Slot[*], ref free_head i32, capacity i32) {
    free_head = 0
    var i i32 = 0
    for i < capacity {
        slots(i).used = 0
        slots(i).value = 0
        if i + 1 < capacity {
            slots(i).next = i + 1
        } else {
            slots(i).next = -1
        }
        i = i + 1
    }
}

def take_slot(slots Slot[*], ref free_head i32) i32 {
    if free_head < 0 {
        ret -1
    }
    var idx i32 = free_head
    free_head = slots(idx).next
    slots(idx).used = 1
    slots(idx).next = -1
    ret idx
}

def release_slot(slots Slot[*], ref free_head i32, idx i32) {
    slots(idx).used = 0
    slots(idx).value = 0
    slots(idx).next = free_head
    free_head = idx
}

def list_append(slots Slot[*], ref head i32, ref free_head i32, value i32) i32 {
    var idx i32 = take_slot(slots, ref free_head)
    if idx < 0 {
        ret -1
    }
    slots(idx).value = value
    slots(idx).next = -1
    if head < 0 {
        head = idx
        ret idx
    }
    var cur i32 = head
    for slots(cur).next >= 0 {
        cur = slots(cur).next
    }
    slots(cur).next = idx
    ret idx
}

def list_find(slots Slot[*], head i32, value i32) i32 {
    var cur i32 = head
    for cur >= 0 {
        if slots(cur).used != 0 && slots(cur).value == value {
            ret cur
        }
        cur = slots(cur).next
    }
    ret -1
}

def list_update(slots Slot[*], head i32, old_value i32, new_value i32) i32 {
    var idx i32 = list_find(slots, head, old_value)
    if idx < 0 {
        ret 0
    }
    slots(idx).value = new_value
    ret 1
}

def list_remove(slots Slot[*], ref head i32, ref free_head i32, value i32) i32 {
    var prev i32 = -1
    var cur i32 = head
    for cur >= 0 {
        if slots(cur).used != 0 && slots(cur).value == value {
            var next i32 = slots(cur).next
            if prev < 0 {
                head = next
            } else {
                slots(prev).next = next
            }
            release_slot(slots, ref free_head, cur)
            ret 1
        }
        prev = cur
        cur = slots(cur).next
    }
    ret 0
}

def list_len(slots Slot[*], head i32) i32 {
    var cur i32 = head
    var count i32 = 0
    for cur >= 0 {
        if slots(cur).used != 0 {
            count = count + 1
        }
        cur = slots(cur).next
    }
    ret count
}

def list_sum(slots Slot[*], head i32) i32 {
    var cur i32 = head
    var total i32 = 0
    for cur >= 0 {
        if slots(cur).used != 0 {
            total = total + slots(cur).value
        }
        cur = slots(cur).next
    }
    ret total
}

def main() i32 {
    var slots Slot[*] = malloc(64)
    var head i32 = -1
    var free_head i32 = -1
    var status i32 = 0

    list_init(slots, ref free_head, 4)

    if list_append(slots, ref head, ref free_head, 10) < 0 {
        status = 11
    }
    if status == 0 && (list_append(slots, ref head, ref free_head, 20) < 0) {
        status = 12
    }
    if status == 0 && (list_append(slots, ref head, ref free_head, 30) < 0) {
        status = 13
    }
    if status == 0 && (list_len(slots, head) != 3) {
        status = 14
    }
    if status == 0 && (list_find(slots, head, 20) < 0) {
        status = 15
    }
    if status == 0 && (list_update(slots, head, 20, 25) == 0) {
        status = 16
    }
    if status == 0 && (list_find(slots, head, 25) < 0) {
        status = 17
    }
    if status == 0 && (list_remove(slots, ref head, ref free_head, 10) == 0) {
        status = 18
    }
    if status == 0 && (list_find(slots, head, 10) >= 0) {
        status = 19
    }
    if status == 0 && (list_append(slots, ref head, ref free_head, 40) < 0) {
        status = 20
    }
    if status == 0 && (list_len(slots, head) != 3) {
        status = 21
    }
    if status == 0 && (list_sum(slots, head) != 95) {
        status = 22
    }

    if status == 0 {
        var msg i8[8] = {108, 105, 115, 116, 32, 111, 107, 0}
        puts(&msg(0))
    }

    free(slots)
    ret status
}
EOF

bash "$BUILD_SYSTEM" "$main_program" "$main_exe"
bash "$BUILD_SYSTEM" "$top_level_program" "$top_level_exe"
bash "$BUILD_SYSTEM" "$lona_import_c_program" "$lona_import_c_exe"
if bash "$BUILD_SYSTEM" "$bad_main_program" "$bad_main_exe" >"$bad_main_log" 2>&1; then
    echo "expected lac.sh to reject non-host-compatible main(argc) entry" >&2
    exit 1
fi
grep -Fq 'cannot build system executable from' "$bad_main_log"
grep -Fq 'zero-argument '\''def main() i32'\''' "$bad_main_log"
"$BIN" --emit obj --verify-ir "$main_program" "$main_obj"
nm -g --defined-only "$main_obj" | grep -Eq ' [TW] __lona_entry__$'
nm -g --defined-only "$main_obj" | grep -Eq ' [TW] main$'
bash "$COMPILE_CASE" "$export_add_program" "$export_add_ir" "$export_add_obj" >/dev/null
"$CC_BIN" -Werror "$export_add_harness_c" "$export_add_obj" -o "$export_add_exe"
bash "$COMPILE_CASE" "$repr_point_program" "$repr_point_ir" "$repr_point_obj" >/dev/null
"$CC_BIN" -Werror "$repr_point_harness_c" "$repr_point_obj" -o "$repr_point_exe"
bash "$BUILD_SYSTEM" "$list_crud_program" "$list_crud_exe"
bash "$BUILD_SYSTEM" "$linked_list_program" "$linked_list_exe"

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
"$list_crud_exe"
list_crud_status=$?
"$linked_list_exe"
linked_list_status=$?
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

if [ "$list_crud_status" -ne 0 ]; then
    echo "expected system malloc/free/puts list CRUD program to exit with 0, got $list_crud_status" >&2
    exit 1
fi

if [ "$linked_list_status" -ne 0 ]; then
    echo "expected system self-pointer linked-list CRUD program to exit with 0, got $linked_list_status" >&2
    exit 1
fi

echo "system smoke passed"
