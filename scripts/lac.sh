#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

DEFAULT_LONA_IR_BIN="$ROOT/build/lona-ir"
if [ -x "$SCRIPT_DIR/lona-ir" ]; then
    DEFAULT_LONA_IR_BIN="$SCRIPT_DIR/lona-ir"
fi

if [ -x /usr/lib/llvm-18/bin/clang ]; then
    DEFAULT_CLANG_BIN=/usr/lib/llvm-18/bin/clang
else
    DEFAULT_CLANG_BIN="$(command -v clang-18 || command -v clang || true)"
fi

LONA_IR_BIN="${LONA_IR_BIN:-${LONA_BIN:-$DEFAULT_LONA_IR_BIN}}"
CLANG_BIN="${CLANG_BIN:-$DEFAULT_CLANG_BIN}"
KEEP_TEMP=0
OPT_LEVEL=0

usage() {
    cat <<'EOF'
Usage: scripts/lac.sh [options] <input.lo> <output>

Options:
  -O <0-3>       Forward optimization level to lona-ir and clang
  --keep-temp    Keep intermediate .ll file
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

if [ -z "$CLANG_BIN" ] || [ ! -x "$CLANG_BIN" ]; then
    echo "clang not found; set CLANG_BIN or install clang-18" >&2
    exit 1
fi

TMPDIR_LOCAL="$(mktemp -d "${TMPDIR:-/tmp}/lona-system-XXXXXX")"
cleanup() {
    if [ "$KEEP_TEMP" -eq 0 ]; then
        rm -rf "$TMPDIR_LOCAL"
    else
        echo "kept temp files in $TMPDIR_LOCAL" >&2
    fi
}
trap cleanup EXIT

IR_PATH="$TMPDIR_LOCAL/program.ll"

"$LONA_IR_BIN" --emit-ir --verify-ir -O "$OPT_LEVEL" "$INPUT" "$IR_PATH"

if ! grep -q '^define i32 @main()' "$IR_PATH"; then
    cat >&2 <<EOF
cannot build system executable from $INPUT
help: the linked IR does not expose a host-compatible main()
help: define a root-level top-level program or a zero-argument 'def main() i32'
EOF
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
"$CLANG_BIN" -O"$OPT_LEVEL" -Wno-override-module -x ir "$IR_PATH" -o "$OUTPUT"
