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
LTO_MODE="${LTO_MODE:-off}"
KEEP_TEMP=0
OPT_LEVEL=0

usage() {
    cat <<'EOF'
Usage: scripts/lac.sh [options] <input.lo> <output>

Options:
  -O <0-3>       Forward optimization level to lona-ir
  --target <triple>
                 Target triple for hosted builds
  --lto <off|full>
                 Link-time optimization mode
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
        --lto)
            LTO_MODE="$2"
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

case "$LTO_MODE" in
    off|full)
        ;;
    *)
        echo "unknown lto mode: $LTO_MODE" >&2
        usage >&2
        exit 1
        ;;
esac

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

if [ -z "$NM_BIN" ] || [ ! -x "$NM_BIN" ]; then
    cat >&2 <<EOF
cannot build system executable from $INPUT
help: hosted builds require \`nm\` so lac can validate \`main\` / \`__lona_main__\` symbols safely
help: install \`nm\`, set NM_BIN, or build a non-hosted artifact instead
EOF
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

OBJECTS=()
if [ "$LTO_MODE" = "full" ]; then
    FINAL_OBJECT="$TMPDIR_LOCAL/program.lto.o"
    "$LONA_IR_BIN" --emit obj --lto full --target "$TARGET_TRIPLE" --verify-ir -O "$OPT_LEVEL" \
        "$INPUT" "$FINAL_OBJECT"
    OBJECTS=("$FINAL_OBJECT")
else
    MANIFEST_PATH="$TMPDIR_LOCAL/objects.manifest"
    CACHE_DIR="$TMPDIR_LOCAL/objects"
    "$LONA_IR_BIN" --emit objects --target "$TARGET_TRIPLE" --verify-ir -O "$OPT_LEVEL" \
        --cache-dir "$CACHE_DIR" \
        "$INPUT" "$MANIFEST_PATH"

    while IFS=$'\t' read -r KIND ROLE PATH_VALUE; do
        if [ "$KIND" = "object" ] && [ -n "${PATH_VALUE:-}" ]; then
            OBJECTS+=("$PATH_VALUE")
        fi
    done < "$MANIFEST_PATH"

    MODULE_SYMBOLS="$("$NM_BIN" -g "${OBJECTS[@]}")"
    MODULE_DEFINED_SYMBOLS="$("$NM_BIN" -g --defined-only "${OBJECTS[@]}")"
    if ! grep -Eq ' [TW] __lona_main__$' <<<"$MODULE_DEFINED_SYMBOLS"; then
        cat >&2 <<EOF
cannot build system executable from $INPUT
help: the linked object does not expose __lona_main__()
help: define root-level executable statements in the root module
EOF
        exit 1
    fi
    if grep -Eq ' [UTW] main$' <<<"$MODULE_SYMBOLS"; then
        cat >&2 <<EOF
cannot build system executable from $INPUT
help: this program already declares or imports a non-entry symbol named \`main\`
help: rename that symbol or build a non-hosted artifact instead of a system executable
EOF
        exit 1
    fi

    ENTRY_OBJECT="$TMPDIR_LOCAL/lona-hosted-entry.o"
    "$LONA_IR_BIN" --emit entry --target "$TARGET_TRIPLE" "$ENTRY_OBJECT"
    OBJECTS+=("$ENTRY_OBJECT")
fi

if [ "${#OBJECTS[@]}" -eq 0 ]; then
    cat >&2 <<EOF
cannot build system executable from $INPUT
help: lona-ir did not emit any linkable object files
help: this looks like a compiler multi-object emission bug rather than a user program error
EOF
    exit 1
fi

ALL_SYMBOLS="$("$NM_BIN" -g "${OBJECTS[@]}")"
DEFINED_SYMBOLS="$("$NM_BIN" -g --defined-only "${OBJECTS[@]}")"
if ! grep -Eq ' [TW] __lona_main__$' <<<"$DEFINED_SYMBOLS"; then
    cat >&2 <<EOF
cannot build system executable from $INPUT
help: the linked object does not expose __lona_main__()
help: define root-level executable statements in the root module
EOF
    exit 1
fi
if ! grep -Eq ' [TW] main$' <<<"$DEFINED_SYMBOLS"; then
    cat >&2 <<EOF
cannot build system executable from $INPUT
help: the hosted system wrapper \`main(argc, argv)\` was not generated
help: this looks like a compiler entry-wrapping bug rather than a user program error
EOF
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
"$CC_BIN" "${OBJECTS[@]}" -o "$OUTPUT"
