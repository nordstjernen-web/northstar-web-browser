#!/usr/bin/env bash
# Build a redistributable Northstar Web Browser Linux x86_64 release: the stripped,
# LTO-optimised northstar binary plus the data files it needs at runtime,
# zipped under dist/.
set -euo pipefail

log() { printf '[pack-linux] %s\n' "$*" >&2; }
trap 'rc=$?; printf "[pack-linux] FAILED (exit %s) at line %s: %s\n" \
    "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR
[ -n "${NS_DEBUG:-}" ] && set -x

ROOT=$(cd "$(dirname "$0")/.." && pwd)
bash "$ROOT/scripts/check-subprojects.sh"
BUILDDIR=${BUILDDIR:-$ROOT/release-build}
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
ARCH=$(uname -m)
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
SLUG="northstar-${FSVERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"
ZIP="$ROOT/dist/${SLUG}.zip"

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release -Db_lto="${NS_BUILD_LTO:-true}" \
        -Db_ndebug=true --strip \
        ${NS_BUILD_DATE:+-Dbuild_date="$NS_BUILD_DATE"}
fi
meson compile -C "$BUILDDIR" ${NS_BUILD_JOBS:+-j "$NS_BUILD_JOBS"}
strip --strip-all "$BUILDDIR/src/gtk/northstar"

LOADER=$(ldd "$BUILDDIR/src/gtk/northstar" 2>/dev/null \
    | grep -m1 -oE 'ld-(musl|linux)[^ ]*' || true)
if printf '%s' "$LOADER" | grep -q 'ld-musl'; then
    LIBC=musl
else
    LIBC=glibc
fi
[ -n "$LOADER" ] || LOADER="ld-$LIBC"

if [ "$LIBC" = musl ]; then
    LIBC_REQ='- musl libc (Alpine 3.19+ era and later)'
    RUNTIME_INSTALL='    sudo apk add gtk4.0 libcurl uchardet libavif dav1d libvpx opus libthai \
        libseccomp libpsl sqlite-libs ca-certificates font-dejavu sdl2 # Alpine (musl)'
else
    # Don't guess the glibc floor — read it from the binary we just built.
    GLIBC_MIN=$(objdump -T "$BUILDDIR/src/gtk/northstar" 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -t. -k1,1V -k2,2n -u \
        | tail -1 | cut -d_ -f2)
    LIBC_REQ="- glibc ${GLIBC_MIN:-2.38}+ (the build container's generation; check with: ldd --version)"
    RUNTIME_INSTALL='    sudo apt   install libgtk-4-1 libcurl4 libuchardet0 \
        libavif16 libpsl5 libseccomp2 libsqlite3-0 libsdl2-2.0-0   # Debian/Ubuntu
    sudo dnf   install gtk4 libcurl libuchardet \
        libavif libpsl libseccomp sqlite-libs SDL2                 # Fedora/RHEL
    sudo zypper install libgtk-4-1 libcurl4 libuchardet0 \
        libavif16 libpsl5 libseccomp2 libsqlite3-0 libSDL2-2_0-0   # openSUSE'
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/data/icons/hicolor/scalable/apps"
cp "$BUILDDIR/src/gtk/northstar" "$STAGE/"
cp "$ROOT"/data/icons/hicolor/scalable/apps/northstar*.svg \
   "$ROOT"/data/icons/hicolor/scalable/apps/northstar.gif \
   "$STAGE/data/icons/hicolor/scalable/apps/"
cp "$ROOT/data/northstar.desktop" "$STAGE/data/"

cp "$ROOT/README.md" "$STAGE/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/"
cp "$ROOT/LICENSE" "$STAGE/"

cat > "$STAGE/INSTALL.md" <<EOF
# Northstar Web Browser ${VERSION} — Linux ${ARCH} binary

Stripped, LTO-optimised build. The browser engine itself (lexbor for
HTML, quickjs for JavaScript, wuffs for image decoding, wamr for
WebAssembly) is statically linked into the binary. The GTK 4 desktop
stack stays dynamic because it expects to find pixbuf loaders, IM
modules and font/theme data on the host at runtime — fully-static GTK
isn't practical.

## Runtime requirements

This is a ${LIBC} build (linked against ${LOADER}). It will not run
against the other C library — pick the matching download.

${LIBC_REQ}
- GTK 4.6+, with gio, gobject, pango, cairo
- libcurl with a TLS backend; OpenSSL 3 (libcrypto)
- libuchardet
- libavif 16 (AVIF images — only in recent distro releases)
- libpsl, libseccomp, libsqlite3
- SDL2 (in-process audio playback)
- fontconfig + a font set; harfbuzz; freetype; libstdc++
- ca-certificates (TLS trust store)
- An X11 or Wayland session

Distro install commands:

${RUNTIME_INSTALL}

For Linux distros without modern GTK 4, use the AppImage
(scripts/pack-appimage.sh) instead.

## Run

    ./northstar https://example.com

## Install on user path

    install -Dm755 northstar ~/.local/bin/northstar
    ln -sf northstar ~/.local/bin/open-internet-browser
    install -Dm644 data/icons/hicolor/scalable/apps/northstar.svg \\
        ~/.local/share/icons/hicolor/scalable/apps/northstar.svg

## License

Free software under the GNU General Public License, version 3 or later.
Copyright 2026 Andreas Røsdal. See LICENSE and README.md for details.
EOF

log "staged: $(cd "$STAGE" && find . -type f | sort | tr '\n' ' ')"
log "zip -> $ZIP"
rm -f "$ZIP"
( cd "$ROOT/dist" && zip -r "$(basename "$ZIP")" "$(basename "$STAGE")" >/dev/null )
[ -s "$ZIP" ] || { log "ERROR: $ZIP missing or empty after zip"; exit 1; }

zip_size=$(du -h "$ZIP" 2>/dev/null | cut -f1 || echo '?')
bin_size=$(du -h "$STAGE/northstar" 2>/dev/null | cut -f1 || echo '?')
echo "Built: $ZIP ($zip_size)"
echo "Binary size: $bin_size"
echo
echo "Smoke test: ./dist/${SLUG}/northstar --headless --url=https://example.com --dump=text"
