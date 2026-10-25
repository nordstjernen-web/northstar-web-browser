#!/usr/bin/env bash
# Stage a portable Northstar Web Browser BSD release zip: the northstar binary,
# runtime data, and an INSTALL.md naming the pkg runtime
# dependencies, zipped under dist/. OS label via $1 or NS_PACK_OS.
set -euo pipefail

log() { printf '[pack-bsd] %s\n' "$*" >&2; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OS=${1:-${NS_PACK_OS:-bsd}}
BUILDDIR=${BUILDDIR:-$ROOT/builddir}
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
SLUG="northstar-${FSVERSION}-${OS}-x86_64"
STAGE="$ROOT/dist/$SLUG"
ZIP="$ROOT/dist/${SLUG}.zip"

if [ ! -x "$BUILDDIR/src/gtk/northstar" ]; then
    log "ERROR: $BUILDDIR/src/gtk/northstar not found — build first"
    exit 1
fi

strip "$BUILDDIR/src/gtk/northstar" 2>/dev/null || true

rm -rf "$STAGE"
mkdir -p "$STAGE/data/icons/hicolor/scalable/apps"
cp "$BUILDDIR/src/gtk/northstar" "$STAGE/"
cp "$ROOT"/data/icons/hicolor/scalable/apps/northstar*.svg \
   "$STAGE/data/icons/hicolor/scalable/apps/" 2>/dev/null || true
cp "$ROOT"/data/icons/hicolor/scalable/apps/northstar.gif \
   "$STAGE/data/icons/hicolor/scalable/apps/" 2>/dev/null || true
cp "$ROOT/data/northstar.desktop" "$STAGE/data/" 2>/dev/null || true
cp "$ROOT/README.md" "$STAGE/" 2>/dev/null || true
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/" 2>/dev/null || true
cp "$ROOT/LICENSE" "$STAGE/" 2>/dev/null || true

case "$OS" in
    freebsd)
        RUNTIME='    pkg install gtk4 libcurl uchardet sqlite3 libpsl libavif sdl2' ;;
    netbsd)
        RUNTIME='    pkgin install gtk4 curl uchardet sqlite3 libpsl libavif SDL2' ;;
    *)
        RUNTIME='    install gtk4 and the other runtime libraries with your package manager' ;;
esac

cat > "$STAGE/INSTALL.md" <<EOF
# Northstar Web Browser ${VERSION} — ${OS} x86_64

Portable build. The browser engine (lexbor for HTML, quickjs for
JavaScript, wuffs for image decoding, wamr for WebAssembly) is statically
linked into the binary; GTK 4 and the other shared runtime libraries come
from your system. The Linux syscall sandbox (seccomp) does not exist on
${OS}, so it is compiled out — process isolation relies on the OS.

## Runtime dependencies

${RUNTIME}

## Run

    ./northstar https://example.com
EOF

( cd "$ROOT/dist" && rm -f "${SLUG}.zip" && zip -r "${SLUG}.zip" "$SLUG" >/dev/null )
log "wrote $ZIP"
printf '%s\n' "$ZIP"
