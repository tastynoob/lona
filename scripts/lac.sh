#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

DEFAULT_LONA_IR_BIN="$ROOT/build/lona-ir"
if [ -x "$SCRIPT_DIR/lona-ir" ]; then
    DEFAULT_LONA_IR_BIN="$SCRIPT_DIR/lona-ir"
fi

LONA_IR_BIN="${LONA_IR_BIN:-${LONA_BIN:-$DEFAULT_LONA_IR_BIN}}"
if [ -n "${CC:-}" ]; then
    DEFAULT_CC_BIN="$CC"
else
    DEFAULT_CC_BIN="$(command -v cc || command -v clang || true)"
fi
CC_BIN="${CC_BIN:-$DEFAULT_CC_BIN}"
NM_BIN="${NM_BIN:-$(command -v nm || true)}"
TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-unknown-linux-gnu}"
KEEP_TEMP=0
OPT_LEVEL=0

usage() {
    cat <<'EOF'
Usage: scripts/lac.sh [options] <input.lo> <output>

Options:
  -O <0-3>       Forward optimization level to lona-ir
  --target <triple>
                 Target triple for hosted builds
  --keep-temp    Keep intermediate .o file
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
        --target)
            TARGET_TRIPLE="$2"
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

if [ -z "$CC_BIN" ] || [ ! -x "$CC_BIN" ]; then
    echo "C linker driver not found; set CC_BIN or install cc/clang" >&2
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

OBJ_PATH="$TMPDIR_LOCAL/program.o"

"$LONA_IR_BIN" --emit obj --target "$TARGET_TRIPLE" --verify-ir -O "$OPT_LEVEL" \
    "$INPUT" "$OBJ_PATH"

if [ -n "$NM_BIN" ] && [ -x "$NM_BIN" ]; then
    ALL_SYMBOLS="$("$NM_BIN" -g "$OBJ_PATH")"
    DEFINED_SYMBOLS="$("$NM_BIN" -g --defined-only "$OBJ_PATH")"
    if ! grep -Eq ' [TW] __lona_main__$' <<<"$DEFINED_SYMBOLS"; then
        cat >&2 <<EOF
cannot build system executable from $INPUT
help: the linked object does not expose __lona_main__()
help: define root-level executable statements in the root module
EOF
        exit 1
    fi
    if ! grep -Eq ' [TW] main$' <<<"$DEFINED_SYMBOLS"; then
        if grep -Eq ' [UTW] main$' <<<"$ALL_SYMBOLS"; then
            cat >&2 <<EOF
cannot build system executable from $INPUT
help: this program already declares or imports a non-entry symbol named \`main\`
help: rename that symbol or build a non-hosted artifact instead of a system executable
EOF
            exit 1
        fi
        cat >&2 <<EOF
cannot build system executable from $INPUT
help: the hosted system wrapper \`main(argc, argv)\` was not generated
help: this looks like a compiler entry-wrapping bug rather than a user program error
EOF
        exit 1
    fi
fi

mkdir -p "$(dirname "$OUTPUT")"
"$CC_BIN" "$OBJ_PATH" -o "$OUTPUT"
