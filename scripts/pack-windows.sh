#!/usr/bin/env bash
# Build a redistributable Northstar Web Browser Windows bundle: a root launcher plus the
# MSYS2 DLLs and GTK runtime data under app/ so it runs outside MSYS2.
#
# Builds (or reuses) a separate --buildtype=release tree in $BUILDDIR so the
# shipped binary has NDEBUG defined — third-party assertions in vendored deps
# like quickjs-ng are compiled out, and the optimiser runs at -O3.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
bash "$ROOT/scripts/check-subprojects.sh"
BUILDDIR=${BUILDDIR:-$ROOT/builddir-release}
OUT=${OUT:-$ROOT/dist/northstar-win64}
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
SLUG="northstar-${VERSION}-windows-x86_64"
ZIP="$ROOT/dist/${SLUG}.zip"
APP=$OUT/app
BIN_SRC=$BUILDDIR/src/gtk/northstar.exe
LAUNCHER_SRC=$BUILDDIR/src/northstar-launcher.exe
BROWSER_EXE=northstar-ui.exe
EXTRA_MESON_SETUP_ARGS=()
if [ -n "${NS_MESON_SETUP_ARGS:-}" ]; then
    EXTRA_MESON_SETUP_ARGS=($NS_MESON_SETUP_ARGS)
fi

resolve_mingw_prefix() {
    for cand in "${MINGW_PREFIX:-}" /c/msys64/ucrt64 "C:/msys64/ucrt64" \
                /ucrt64 /c/msys64/mingw64 "C:/msys64/mingw64" /mingw64; do
        [ -n "$cand" ] || continue
        [ -d "$cand/bin" ] || continue
        [ -f "$cand/bin/libgtk-4-1.dll" ] || continue
        echo "$cand"; return
    done
    return 1
}

MINGW_PREFIX=$(resolve_mingw_prefix) || {
    echo "pack-windows: could not find the MSYS2 ucrt64/bin or mingw64/bin; set MINGW_PREFIX." >&2
    exit 1
}

declare -A mingw_dlls
for dll in "$MINGW_PREFIX"/bin/*.dll; do
    [ -f "$dll" ] || continue
    dll_name=${dll##*/}
    mingw_dlls[${dll_name,,}]=${dll}
done

if [ -z "${CC_LD:-}" ] && command -v ld.lld >/dev/null 2>&1 &&
   ${CC:-cc} --version 2>/dev/null | grep -q clang; then
    export CC_LD=lld CXX_LD=lld
fi

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release "${EXTRA_MESON_SETUP_ARGS[@]}"
elif [ ${#EXTRA_MESON_SETUP_ARGS[@]} -gt 0 ]; then
    meson configure "$BUILDDIR" "${EXTRA_MESON_SETUP_ARGS[@]}"
fi
meson compile -C "$BUILDDIR"

if [ ! -x "$BIN_SRC" ]; then
    echo "pack-windows: build did not produce $BIN_SRC" >&2
    exit 1
fi
if [ ! -x "$LAUNCHER_SRC" ]; then
    echo "pack-windows: build did not produce $LAUNCHER_SRC" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$APP"
cp "$LAUNCHER_SRC" "$OUT/northstar.exe"
cp "$BIN_SRC" "$APP/$BROWSER_EXE"
validate_launcher_imports() {
    local dep key src
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        key=${dep,,}
        src=${mingw_dlls[$key]:-}
        if [ -n "$src" ]; then
            printf 'pack-windows: root launcher imports %s; keep it system-only before using the app/ layout\n' \
                "$dep" >&2
            exit 1
        fi
    done < <(objdump -p "$OUT/northstar.exe" 2>/dev/null | awk '/DLL Name:/ {print $3}')
}

validate_launcher_imports

# GLib settings schemas (compiled). Apps that GSettings-look-up a key crash without these.
mkdir -p "$APP/share/glib-2.0/schemas"
if [ -f "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" ]; then
    cp "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" "$APP/share/glib-2.0/schemas/"
fi

if [ -d "$MINGW_PREFIX/etc/fonts" ]; then
    mkdir -p "$APP/etc"
    cp -r "$MINGW_PREFIX/etc/fonts" "$APP/etc/"
fi

# GDK-PixBuf loader cache + loader DLLs for GTK's icon theme. Copied
# *before* the DLL chase so the loaders' transitive deps (notably
# librsvg-2-2.dll, pulled in only by pixbufloader_svg.dll) get bundled too.
# The browser renders SVG itself; these loaders only serve GTK's icon theme.
# GdkPixbuf loads these dynamically via loaders.cache, so their import edges
# don't appear in northstar.exe's static-import graph.
if [ -d "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" ]; then
    mkdir -p "$APP/lib"
    cp -r "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" "$APP/lib/"
fi

# Transitively resolve DLL dependencies starting from the exe and every
# pixbuf loader DLL. objdump reports import names; we look them up in the
# mingw bin dir and skip anything that resolves to a Windows system DLL.
declare -A seen
queue=("$APP/$BROWSER_EXE")
for loader in "$APP"/lib/gdk-pixbuf-2.0/*/loaders/*.dll; do
    [ -f "$loader" ] && queue+=("$loader")
done
queue_head=0
while [ "$queue_head" -lt "${#queue[@]}" ]; do
    cur=${queue[$queue_head]}
    queue_head=$((queue_head + 1))
    deps=$(objdump -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}') || true
    for dep in $deps; do
        key=${dep,,}
        if [ -n "${seen[$key]:-}" ]; then continue; fi
        seen[$key]=1
        src=${mingw_dlls[$key]:-}
        if [ -n "$src" ]; then
            cp "$src" "$APP/"
            queue+=("$APP/$(basename "$src")")
        fi
    done
done

dll_exports_symbol() {
    objdump -p "$1" 2>/dev/null | awk -v want="$2" '
        /^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]/ && $NF == want {
            found = 1
        }
        END { exit found ? 0 : 1 }
    '
}

validate_ngtcp2_ossl() {
    local ngtcp2 ssl_dep ssl
    ngtcp2=$(find "$APP" -maxdepth 1 -iname 'libngtcp2_crypto_ossl-0.dll' -print -quit 2>/dev/null || true)
    [ -n "$ngtcp2" ] || return 0
    ssl_dep=$(objdump -p "$ngtcp2" 2>/dev/null | awk '
        /DLL Name:/ { dep = $3; next }
        dep != "" && $NF == "SSL_set_quic_tls_cbs" { print dep; exit }
    ')
    [ -n "$ssl_dep" ] || return 0
    ssl=$(find "$APP" -maxdepth 1 -iname "$ssl_dep" -print -quit 2>/dev/null || true)
    if [ -z "$ssl" ]; then
        printf 'pack-windows: %s imports SSL_set_quic_tls_cbs from missing %s\n' \
            "$(basename "$ngtcp2")" "$ssl_dep" >&2
        exit 1
    fi
    if ! dll_exports_symbol "$ssl" SSL_set_quic_tls_cbs; then
        printf 'pack-windows: %s imports SSL_set_quic_tls_cbs, but bundled %s does not export it\n' \
            "$(basename "$ngtcp2")" "$(basename "$ssl")" >&2
        exit 1
    fi
}

validate_ngtcp2_ossl

# Adwaita + hicolor icons for default GTK widget glyphs (back/forward arrows, etc.).
mkdir -p "$APP/share/icons"
for theme in Adwaita hicolor; do
    if [ -d "$MINGW_PREFIX/share/icons/$theme" ]; then
        cp -r "$MINGW_PREFIX/share/icons/$theme" "$APP/share/icons/"
    fi
done

# Northstar Web Browser's own application + toolbar icons (drop into the hicolor theme
# so gtk_image_new_from_icon_name("northstar-back") and friends resolve at
# runtime, and about: pages can read the svg/gif as a data URI). The dev tree
# finds these via the ../../data/icons search path; the bundle only has
# share/icons, so every northstar-*.svg the toolbar references must be
# copied there or the header-bar buttons render blank. Refresh the hicolor
# cache so the bundled icons show up without a filesystem scan.
mkdir -p "$APP/share/icons/hicolor/scalable/apps"
cp "$ROOT"/data/icons/hicolor/scalable/apps/northstar*.svg \
   "$ROOT/data/icons/hicolor/scalable/apps/northstar.gif" \
   "$APP/share/icons/hicolor/scalable/apps/"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache --force --ignore-theme-index \
        "$APP/share/icons/hicolor" >/dev/null 2>&1 || true
fi

# Per-application data: license text. The browser reads it relative to
# the exe at runtime (see src/net.c::about_read_first).
mkdir -p "$APP/share/northstar"
cp "$ROOT/LICENSE" "$APP/share/northstar/"

# Northstar Web Browser's own GPL text plus the third-party copyright + license notices
# required by the libraries we ship, both at the root of the bundle.
cp "$ROOT/LICENSE" "$OUT/LICENSE.txt"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$OUT/"

# CA certificate bundle for libcurl HTTPS verification.
mkdir -p "$APP/etc/ssl/certs"
for ca in \
    "$MINGW_PREFIX/etc/ssl/certs/ca-bundle.crt" \
    "$MINGW_PREFIX/etc/ssl/cert.pem" \
    "$MINGW_PREFIX/ssl/certs/ca-bundle.crt"; do
    if [ -f "$ca" ]; then
        cp "$ca" "$APP/etc/ssl/certs/ca-bundle.crt"
        break
    fi
done

bundled=$(find "$APP" -maxdepth 1 -name '*.dll' | wc -l)
size=$(du -sh "$OUT" | awk '{print $1}')
printf 'pack-windows: bundled %s DLLs, total size %s, output: %s\n' "$bundled" "$size" "$OUT"

if [ -n "${NS_SKIP_ZIP:-}" ]; then
    exit 0
fi

STAGE=$ROOT/dist/$SLUG
rm -rf "$STAGE" "$ZIP"
cp -r "$OUT" "$STAGE"
if command -v zip >/dev/null 2>&1; then
    ( cd "$ROOT/dist" && zip -qr "${SLUG}.zip" "$SLUG" )
else
    powershell.exe -NoProfile -NonInteractive -Command \
        "Add-Type -AssemblyName System.IO.Compression.FileSystem; \
         [System.IO.Compression.ZipFile]::CreateFromDirectory( \
             '$(cygpath -w "$STAGE")', '$(cygpath -w "$ZIP")', \
             [System.IO.Compression.CompressionLevel]::Optimal, \$true)" >/dev/null
fi
rm -rf "$STAGE"

[ -s "$ZIP" ] || { echo "pack-windows: $ZIP missing or empty" >&2; exit 1; }
zip_size=$(du -h "$ZIP" | awk '{print $1}')
printf 'pack-windows: zip -> %s (%s)\n' "$ZIP" "$zip_size"
