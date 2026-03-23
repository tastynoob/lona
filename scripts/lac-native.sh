#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_ROOT="$(cd "$SCRIPT_DIR/../share/lona" 2>/dev/null && pwd || true)"
ASSET_ROOT="$ROOT"
if [ ! -f "$ASSET_ROOT/runtime/bare_x86_64/lona_start.S" ] && [ -n "$INSTALL_ROOT" ]; then
    ASSET_ROOT="$INSTALL_ROOT"
fi

DEFAULT_LONA_IR_BIN="$ROOT/build/lona-ir"
if [ -x "$SCRIPT_DIR/lona-ir" ]; then
    DEFAULT_LONA_IR_BIN="$SCRIPT_DIR/lona-ir"
fi

LONA_IR_BIN="${LONA_IR_BIN:-${LONA_BIN:-$DEFAULT_LONA_IR_BIN}}"
LLC_BIN="${LLC_BIN:-$(command -v llc-18 || command -v llc || true)}"
CC_BIN="${CC_BIN:-cc}"
LD_BIN="${LD_BIN:-ld}"
STARTUP_SRC="${STARTUP_SRC:-$ASSET_ROOT/runtime/bare_x86_64/lona_start.S}"
LINKER_SCRIPT="${LINKER_SCRIPT:-$ASSET_ROOT/runtime/bare_x86_64/lona.ld}"
TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-pc-linux-gnu}"
KEEP_TEMP=0
OPT_LEVEL=0

usage() {
    cat <<'EOF'
Usage: scripts/lac-native.sh [options] <input.lo> <output>

Options:
  -O <0-3>       Forward optimization level to lona-ir
  --keep-temp    Keep intermediate .ll/.o files
  -h, --help     Show this help
EOF
}

ARGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        -O)
            OPT_LEVEL="$2"
            shift 2
            ;;
        --keep-temp)
            KEEP_TEMP=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                ARGS+=("$1")
                shift
            done
            ;;
        -*)
            echo "unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

if [ "${#ARGS[@]}" -ne 2 ]; then
    usage >&2
    exit 1
fi

INPUT="${ARGS[0]}"
OUTPUT="${ARGS[1]}"

if [ ! -x "$LONA_IR_BIN" ]; then
    echo "lona-ir compiler not found or not executable: $LONA_IR_BIN" >&2
    exit 1
fi

if [ -z "$LLC_BIN" ] || [ ! -x "$LLC_BIN" ]; then
    echo "llc not found; set LLC_BIN or install llc-18" >&2
    exit 1
fi

if [ ! -f "$STARTUP_SRC" ]; then
    echo "startup assembly not found: $STARTUP_SRC" >&2
    exit 1
fi

if [ ! -f "$LINKER_SCRIPT" ]; then
    echo "linker script not found: $LINKER_SCRIPT" >&2
    exit 1
fi

TMPDIR_LOCAL="$(mktemp -d "${TMPDIR:-/tmp}/lona-native-XXXXXX")"
cleanup() {
    if [ "$KEEP_TEMP" -eq 0 ]; then
        rm -rf "$TMPDIR_LOCAL"
    else
        echo "kept temp files in $TMPDIR_LOCAL" >&2
    fi
}
trap cleanup EXIT

IR_PATH="$TMPDIR_LOCAL/program.ll"
PROGRAM_OBJ="$TMPDIR_LOCAL/program.o"
STARTUP_OBJ="$TMPDIR_LOCAL/lona_start.o"

"$LONA_IR_BIN" --emit ir --verify-ir -O "$OPT_LEVEL" "$INPUT" "$IR_PATH"

if ! grep -q '^define i32 @__lona_entry__()' "$IR_PATH"; then
    cat >&2 <<EOF
cannot build executable from $INPUT
help: the linked IR does not expose __lona_entry__()
help: define a root-level top-level program or a zero-argument 'def main() i32'
EOF
    exit 1
fi

"$LLC_BIN" -filetype=obj -relocation-model=static -mtriple="$TARGET_TRIPLE" \
    "$IR_PATH" -o "$PROGRAM_OBJ"
"$CC_BIN" -c "$STARTUP_SRC" -o "$STARTUP_OBJ"
mkdir -p "$(dirname "$OUTPUT")"
"$LD_BIN" -m elf_x86_64 -nostdlib -z noexecstack -T "$LINKER_SCRIPT" \
    -o "$OUTPUT" "$STARTUP_OBJ" "$PROGRAM_OBJ"
