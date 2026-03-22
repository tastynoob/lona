#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_NATIVE="$ROOT/scripts/lac-native.sh"
TMPDIR_LOCAL="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMPDIR_LOCAL/lona-native-smoke-XXXXXX")"

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

main_program="$WORKDIR/return_42.lo"
top_level_program="$WORKDIR/top_level.lo"
ref_local_program="$WORKDIR/ref_local_address.lo"
ref_param_program="$WORKDIR/ref_param_address.lo"
array_ptr_program="$WORKDIR/array_pointer.lo"
method_self_program="$WORKDIR/method_self.lo"
method_temp_program="$WORKDIR/method_temp.lo"
struct_fields_program="$WORKDIR/struct_fields.lo"
small_struct_program="$WORKDIR/small_struct_packed.lo"
medium_struct_program="$WORKDIR/medium_struct_direct_return.lo"
main_exe="$WORKDIR/return_42"
top_level_exe="$WORKDIR/top_level"
ref_local_exe="$WORKDIR/ref_local_address"
ref_param_exe="$WORKDIR/ref_param_address"
array_ptr_exe="$WORKDIR/array_pointer"
method_self_exe="$WORKDIR/method_self"
method_temp_exe="$WORKDIR/method_temp"
struct_fields_exe="$WORKDIR/struct_fields"
small_struct_exe="$WORKDIR/small_struct_packed"
medium_struct_exe="$WORKDIR/medium_struct_direct_return"

cat >"$main_program" <<'EOF'
def main() i32 {
    ret 42
}
EOF

cat >"$top_level_program" <<'EOF'
var x = 1
x = x + 1
EOF

cat >"$ref_local_program" <<'EOF'
def main() i32 {
    var x i32 = 1
    ref alias i32 = x
    var p i32* = &alias
    *p = 11
    ret x
}
EOF

cat >"$ref_param_program" <<'EOF'
def poke(ref x i32) i32 {
    var p i32* = &x
    *p = 9
    ret x
}

def main() i32 {
    var x i32 = 1
    ret poke(ref x)
}
EOF

cat >"$array_ptr_program" <<'EOF'
def main() i32 {
    var row i32[4] = {1, 2, 3, 4}
    var p i32[4]* = &row
    (*p)(2) = 13
    ret row(2)
}
EOF

cat >"$method_self_program" <<'EOF'
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

cat >"$method_temp_program" <<'EOF'
struct Counter {
    value i32

    def bump(step i32) i32 {
        self.value = self.value + step
        ret self.value
    }
}

def main() i32 {
    ret Counter(1).bump(2)
}
EOF

cat >"$struct_fields_program" <<'EOF'
def inc(v i32) i32 {
    ret v + 1
}

struct Mixed {
    flag bool
    ratio f32
    bits u8[4]
    pair <i32, bool>
    ptr i32*
    cb (i32)* i32
}

def main() i32 {
    var x i32 = 41
    var raw u8[4] = 1.tof32().tobits()
    var pair <i32, bool> = (1, true)
    var mixed = Mixed(flag = true, ratio = 1.tof32(), bits = raw, pair = pair, ptr = &x, cb = inc&<i32>)
    if mixed.flag && mixed.pair._2 && (mixed.ratio >= 1.tof32()) {
        mixed.bits(0) = 1
        ret mixed.cb(*mixed.ptr) + mixed.bits(0) + mixed.pair._1
    }
    ret 0
}
EOF

cat >"$small_struct_program" <<'EOF'
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

def main() i32 {
    var p = Pair(left = 1, right = 2)
    var out = echo(p).swap(3)
    ret out.left + out.right
}
EOF

cat >"$medium_struct_program" <<'EOF'
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

def main() i32 {
    var triple = Triple(a = 1, b = 2, c = 3)
    var out = echo(triple).shift(4)
    ret out.a + out.b + out.c
}
EOF

bash "$BUILD_NATIVE" "$main_program" "$main_exe"
bash "$BUILD_NATIVE" "$top_level_program" "$top_level_exe"
bash "$BUILD_NATIVE" "$ref_local_program" "$ref_local_exe"
bash "$BUILD_NATIVE" "$ref_param_program" "$ref_param_exe"
bash "$BUILD_NATIVE" "$array_ptr_program" "$array_ptr_exe"
bash "$BUILD_NATIVE" "$method_self_program" "$method_self_exe"
bash "$BUILD_NATIVE" "$method_temp_program" "$method_temp_exe"
bash "$BUILD_NATIVE" "$struct_fields_program" "$struct_fields_exe"
bash "$BUILD_NATIVE" "$small_struct_program" "$small_struct_exe"
bash "$BUILD_NATIVE" "$medium_struct_program" "$medium_struct_exe"

set +e
"$main_exe"
main_status=$?
"$top_level_exe"
top_level_status=$?
"$ref_local_exe"
ref_local_status=$?
"$ref_param_exe"
ref_param_status=$?
"$array_ptr_exe"
array_ptr_status=$?
"$method_self_exe"
method_self_status=$?
"$method_temp_exe"
method_temp_status=$?
"$struct_fields_exe"
struct_fields_status=$?
"$small_struct_exe"
small_struct_status=$?
"$medium_struct_exe"
medium_struct_status=$?
set -e

if [ "$main_status" -ne 42 ]; then
    echo "expected native main program to exit with 42, got $main_status" >&2
    exit 1
fi

if [ "$top_level_status" -ne 0 ]; then
    echo "expected top-level native program to exit with 0, got $top_level_status" >&2
    exit 1
fi

if [ "$ref_local_status" -ne 11 ]; then
    echo "expected local ref address-of program to exit with 11, got $ref_local_status" >&2
    exit 1
fi

if [ "$ref_param_status" -ne 9 ]; then
    echo "expected ref parameter address-of program to exit with 9, got $ref_param_status" >&2
    exit 1
fi

if [ "$array_ptr_status" -ne 13 ]; then
    echo "expected array pointer program to exit with 13, got $array_ptr_status" >&2
    exit 1
fi

if [ "$method_self_status" -ne 5 ]; then
    echo "expected method self mutation program to exit with 5, got $method_self_status" >&2
    exit 1
fi

if [ "$method_temp_status" -ne 3 ]; then
    echo "expected temporary receiver method program to exit with 3, got $method_temp_status" >&2
    exit 1
fi

if [ "$struct_fields_status" -ne 44 ]; then
    echo "expected heterogeneous struct fields program to exit with 44, got $struct_fields_status" >&2
    exit 1
fi

if [ "$small_struct_status" -ne 9 ]; then
    echo "expected small aggregate ABI program to exit with 9, got $small_struct_status" >&2
    exit 1
fi

if [ "$medium_struct_status" -ne 18 ]; then
    echo "expected medium aggregate direct-return program to exit with 18, got $medium_struct_status" >&2
    exit 1
fi

echo "native smoke passed"
