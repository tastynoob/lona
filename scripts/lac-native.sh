#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASSET_ROOT="$ROOT"

DEFAULT_LONA_IR_BIN="$ROOT/build/lona-ir"
if [ -x "$SCRIPT_DIR/lona-ir" ]; then
    DEFAULT_LONA_IR_BIN="$SCRIPT_DIR/lona-ir"
fi

LONA_IR_BIN="${LONA_IR_BIN:-${LONA_BIN:-$DEFAULT_LONA_IR_BIN}}"
CC_BIN="${CC_BIN:-cc}"
LD_BIN="${LD_BIN:-ld}"
NM_BIN="${NM_BIN:-$(command -v nm || true)}"
STARTUP_SRC="${STARTUP_SRC:-$ASSET_ROOT/runtime/bare_x86_64/lona_start.S}"
LINKER_SCRIPT="${LINKER_SCRIPT:-$ASSET_ROOT/runtime/bare_x86_64/lona.ld}"
TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-none-elf}"
LTO_MODE="${LTO_MODE:-off}"
KEEP_TEMP=0
OPT_LEVEL=0
STATS=0
DEFAULT_CACHE_ROOT="${LONA_CACHE_DIR:-${TMPDIR:-/tmp}/lona-cache}"
CACHE_ROOT="$DEFAULT_CACHE_ROOT"

sanitize_path_component() {
    printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'
}

usage() {
    cat <<'EOF'
Usage: scripts/lac-native.sh [options] <input.lo> <output>

Options:
  -O <0-3>       Forward optimization level to lona-ir
  -I <dir>       Add module include search directory (repeatable)
  --target <triple>
                 Target triple for bare builds
  --lto <off|full>
                 Link-time optimization mode
  --cache-dir <dir>
                 Persistent artifact cache root (default: ${TMPDIR:-/tmp}/lona-cache)
  --stats        Forward compile statistics from lona-ir
  --keep-temp    Keep intermediate .ll/.o files
  -h, --help     Show this help
EOF
}

ARGS=()
INCLUDE_ARGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        -O)
            OPT_LEVEL="$2"
            shift 2
            ;;
        -I)
            INCLUDE_ARGS+=("-I" "$2")
            shift 2
            ;;
        -I*)
            INCLUDE_ARGS+=("-I" "${1#-I}")
            shift
            ;;
        --include-dir)
            INCLUDE_ARGS+=("-I" "$2")
            shift 2
            ;;
        --include-dir=*)
            INCLUDE_ARGS+=("-I" "${1#--include-dir=}")
            shift
            ;;
        --target)
            TARGET_TRIPLE="$2"
            shift 2
            ;;
        --lto)
            LTO_MODE="$2"
            shift 2
            ;;
        --cache-dir)
            CACHE_ROOT="$2"
            shift 2
            ;;
        --cache-dir=*)
            CACHE_ROOT="${1#--cache-dir=}"
            shift
            ;;
        --stats)
            STATS=1
            shift
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

if [ ! -f "$STARTUP_SRC" ]; then
    cat >&2 <<EOF
startup assembly not found: $STARTUP_SRC
help: \`make install\` does not install bare runtime assets
help: run this script from a repository checkout, or pass STARTUP_SRC explicitly
EOF
    exit 1
fi

if [ ! -f "$LINKER_SCRIPT" ]; then
    cat >&2 <<EOF
linker script not found: $LINKER_SCRIPT
help: \`make install\` does not install bare runtime assets
help: run this script from a repository checkout, or pass LINKER_SCRIPT explicitly
EOF
    exit 1
fi

if [ -z "$NM_BIN" ] || [ ! -x "$NM_BIN" ]; then
    echo "nm not found; set NM_BIN or install binutils nm" >&2
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

TARGET_CACHE_KEY="$(sanitize_path_component "$TARGET_TRIPLE")"
PERSISTENT_CACHE_ROOT="$CACHE_ROOT/native/$TARGET_CACHE_KEY"
OBJECT_CACHE_DIR="$PERSISTENT_CACHE_ROOT/object-bundle"
LINKED_BITCODE_CACHE_DIR="$PERSISTENT_CACHE_ROOT/linked-bitcode"
STATS_ARGS=()
if [ "$STATS" -eq 1 ]; then
    STATS_ARGS+=(--stats)
fi

STARTUP_OBJ="$TMPDIR_LOCAL/lona_start.o"
OBJECTS=()
if [ "$LTO_MODE" = "full" ]; then
    FINAL_OBJECT="$TMPDIR_LOCAL/program.lto.o"
    "$LONA_IR_BIN" --emit linked-obj --lto full --target "$TARGET_TRIPLE" --verify-ir -O "$OPT_LEVEL" \
        "${STATS_ARGS[@]}" \
        --cache-dir "$LINKED_BITCODE_CACHE_DIR" \
        "${INCLUDE_ARGS[@]}" \
        "$INPUT" "$FINAL_OBJECT"
    OBJECTS=("$FINAL_OBJECT")
else
    MANIFEST_PATH="$TMPDIR_LOCAL/objects.manifest"
    "$LONA_IR_BIN" --emit obj --target "$TARGET_TRIPLE" --verify-ir -O "$OPT_LEVEL" \
        "${STATS_ARGS[@]}" \
        "${INCLUDE_ARGS[@]}" \
        --cache-dir "$OBJECT_CACHE_DIR" \
        "$INPUT" "$MANIFEST_PATH"

    while IFS=$'\t' read -r RECORD_KIND ARTIFACT_KIND ROLE PATH_VALUE; do
        if [ "$RECORD_KIND" = "artifact" ] && [ "$ARTIFACT_KIND" = "obj" ] && [ -n "${PATH_VALUE:-}" ]; then
            OBJECTS+=("$PATH_VALUE")
        fi
    done < "$MANIFEST_PATH"
fi

if [ "${#OBJECTS[@]}" -eq 0 ]; then
    cat >&2 <<EOF
cannot build executable from $INPUT
help: lona-ir did not emit any linkable object files
help: this looks like a compiler multi-object emission bug rather than a user program error
EOF
    exit 1
fi

if ! "$NM_BIN" -g --defined-only "${OBJECTS[@]}" | grep -Eq ' [TW] __lona_main__$'; then
    cat >&2 <<EOF
cannot build executable from $INPUT
help: the emitted object bundle does not expose __lona_main__()
help: define root-level executable statements in the root module
EOF
    exit 1
fi

"$CC_BIN" -c "$STARTUP_SRC" -o "$STARTUP_OBJ"
mkdir -p "$(dirname "$OUTPUT")"
"$LD_BIN" -m elf_x86_64 -nostdlib -z noexecstack -T "$LINKER_SCRIPT" \
    -o "$OUTPUT" "$STARTUP_OBJ" "${OBJECTS[@]}"
