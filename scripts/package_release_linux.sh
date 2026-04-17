#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: scripts/package_release_linux.sh <version> <output-dir>" >&2
    exit 1
fi

VERSION="$1"
OUTPUT_DIR="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
ARCHIVE_NAME="lona-${VERSION}-linux-x86_64"
STAGE_DIR="${OUTPUT_DIR}/${ARCHIVE_NAME}"
ARCHIVE_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}.tar.gz"
CHECKSUM_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}.sha256"

rm -rf "$STAGE_DIR"

make -C "$ROOT" install DESTDIR="$STAGE_DIR" PREFIX=/

install -d "$STAGE_DIR/runtime/bare_x86_64"
install -m 644 "$ROOT/runtime/bare_x86_64/lona.ld" \
    "$STAGE_DIR/runtime/bare_x86_64/lona.ld"
install -m 644 "$ROOT/runtime/bare_x86_64/lona_start.S" \
    "$STAGE_DIR/runtime/bare_x86_64/lona_start.S"
install -m 644 "$ROOT/readme.md" "$STAGE_DIR/README.md"
install -m 644 "$ROOT/LICENSE-APACHE" "$STAGE_DIR/LICENSE-APACHE"
install -m 644 "$ROOT/LICENSE-MIT" "$STAGE_DIR/LICENSE-MIT"

rm -f "$ARCHIVE_PATH" "$CHECKSUM_PATH"
tar -C "$OUTPUT_DIR" -czf "$ARCHIVE_PATH" "$ARCHIVE_NAME"
(
    cd "$OUTPUT_DIR"
    sha256sum "$(basename "$ARCHIVE_PATH")" > "$CHECKSUM_PATH"
)
rm -rf "$STAGE_DIR"
