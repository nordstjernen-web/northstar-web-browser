#!/usr/bin/env bash
# Build a portable Northstar Web Browser .deb by repackaging the bundle that
# pack-linux.sh produces. The binary statically links the in-tree engine
# (lexbor, quickjs, wuffs). Stable desktop deps (GTK, curl, …) are
# computed from the binary's SONAMEs with dpkg-shlibdeps, falling back to
# a hand-maintained list. The image-codec libraries whose SONAMEs differ
# per distro release (libavif and its AV1 codecs) are bundled in
# the package instead, so one .deb installs across Ubuntu/Debian releases.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")}
DEBARCH=$(dpkg --print-architecture 2>/dev/null || echo amd64)
ARCH=$(uname -m)
SLUG="northstar-${VERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"

if ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "dpkg-deb not found. Install it first:" >&2
    echo "    sudo apt install dpkg-dev            # Debian/Ubuntu" >&2
    exit 1
fi

if [ ! -x "$STAGE/northstar" ]; then
    echo "Bundle not staged yet — running pack-linux.sh first."
    "$ROOT/scripts/pack-linux.sh"
fi

PKGROOT="$ROOT/dist/debpkg"
rm -rf "$PKGROOT"
install -dm755 "$PKGROOT/DEBIAN"
install -dm755 "$PKGROOT/usr/bin"
install -dm755 "$PKGROOT/usr/share/icons/hicolor/scalable/apps"
install -dm755 "$PKGROOT/usr/share/applications"
install -dm755 "$PKGROOT/usr/share/northstar"
install -dm755 "$PKGROOT/usr/share/doc/northstar"

install -m755 "$STAGE/northstar" "$PKGROOT/usr/bin/northstar"
# All app + toolbar icons the UI and about: pages look up by name.
for icon in "$ROOT"/data/icons/hicolor/scalable/apps/northstar*.svg \
            "$ROOT"/data/icons/hicolor/scalable/apps/northstar.gif; do
    [ -e "$icon" ] && install -m644 "$icon" \
        "$PKGROOT/usr/share/icons/hicolor/scalable/apps/"
done
# Desktop file named for the GTK app-id so Wayland matches the window to it
# (otherwise the taskbar/dock icon is blank).
install -m644 "$ROOT/data/northstar.desktop" \
    "$PKGROOT/usr/share/applications/org.northstar.WebBrowser.desktop"
# about:license reads this at ../share/northstar/LICENSE relative to the
# binary (/usr/bin -> /usr/share/northstar).
install -m644 "$ROOT/LICENSE" "$PKGROOT/usr/share/northstar/LICENSE"
install -m644 "$ROOT/README.md" "$PKGROOT/usr/share/doc/northstar/"
install -m644 "$ROOT/THIRD-PARTY-LICENSES.md" "$PKGROOT/usr/share/doc/northstar/"
install -m644 "$ROOT/LICENSE" "$PKGROOT/usr/share/doc/northstar/copyright"

# Bundle the volatile image-codec libraries (libavif and the AV1
# codecs it pulls in) under /usr/lib/northstar with an $ORIGIN rpath.
# Their SONAMEs bump between Ubuntu/Debian releases and each release ships
# only one version, so depending on them as system packages makes the .deb
# installable on exactly one release. The stable desktop libs (GTK, curl,
# sqlite, …) stay as normal dependencies.
# Best-effort section: media-lib bundling and shlibdeps both shell out to
# tools that can legitimately exit non-zero (dpkg -S on an unowned path,
# SIGPIPE from head, shlibdeps warnings). Disable errexit here so a hiccup
# degrades to system deps instead of aborting the whole package build.
set +e

BUNDLE_DIR="$PKGROOT/usr/lib/northstar"
# Seed libs the binary links directly whose SONAMEs bump per distro release.
SEED_RE='libavif\.so'
# Never bundle the C/C++/OpenMP runtime: universally present and ABI-stable,
# and a newer system copy stays backward-compatible with our older build.
CORE_DENY='^(ld-linux[^.]*|libc|libm|libdl|libpthread|librt|libgcc_s|libstdc\+\+|libgomp|libnuma|libatomic|libpthread)\.'
# Package-name families to drop from Depends once their libs are bundled.
# Matched against the dep name directly (not via dpkg -S, whose path lookup
# is unreliable under usrmerge), so it tracks whatever ldd linked.
STRIP_RE='^lib(avif|aom|dav1d|gav1|yuv|sharpyuv|rav1e|svtav1)'
bundled_any=0
if command -v patchelf >/dev/null 2>&1; then
    install -dm755 "$BUNDLE_DIR"
    # BFS over the dependency closure of the seed codec libs, so every backend
    # libavif pulls in (gav1, aom, dav1d, rav1e, SvtAv1, yuv, sharpyuv, …)
    # is bundled too -- no per-name allow-list to keep in sync.
    worklist=$(ldd "$PKGROOT/usr/bin/northstar" 2>/dev/null \
                 | grep -oE '/[^ ]+\.so[^ ]*' | grep -E "$SEED_RE")
    seen=""
    while [ -n "$worklist" ]; do
        next=""
        for path in $worklist; do
            base=$(basename "$path")
            case " $seen " in *" $base "*) continue ;; esac
            seen="$seen $base"
            [ -e "$path" ] || continue
            if [ ! -e "$BUNDLE_DIR/$base" ]; then
                cp -L "$path" "$BUNDLE_DIR/$base" || continue
                chmod 644 "$BUNDLE_DIR/$base"
                patchelf --set-rpath '$ORIGIN' "$BUNDLE_DIR/$base" 2>/dev/null
            fi
            for dep in $(ldd "$path" 2>/dev/null | grep -oE '/[^ ]+\.so[^ ]*'); do
                db=$(basename "$dep")
                printf '%s' "$db" | grep -Eq "$CORE_DENY" && continue
                case " $seen " in *" $db "*) continue ;; esac
                next="$next $dep"
            done
        done
        worklist=$next
    done
    if [ -n "$(ls -A "$BUNDLE_DIR" 2>/dev/null)" ]; then
        rpath_ok=1
        patchelf --set-rpath '$ORIGIN/../lib/northstar' \
            "$PKGROOT/usr/bin/northstar" 2>/dev/null || rpath_ok=0
        if [ "$rpath_ok" = 1 ]; then
            bundled_any=1
            echo "pack-deb: bundled$(ls "$BUNDLE_DIR" | sed 's/^/ /' | tr -d '\n')"
        else
            rm -rf "$BUNDLE_DIR"
        fi
    else
        rm -rf "$BUNDLE_DIR"
    fi
fi

INSTALLED_KB=$(du -sk "$PKGROOT/usr" | cut -f1)

FALLBACK_DEPS="libgtk-4-1, libcurl4 | libcurl4t64, libuchardet0, libpsl5 | libpsl5t64, libsqlite3-0, libseccomp2, libfontconfig1, libsdl2-2.0-0"

RUNTIME_DEPS=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    install -dm755 "$PKGROOT/debian"
    cat > "$PKGROOT/debian/control" <<CTL
Source: northstar

Package: northstar
Architecture: any
CTL
    scan_bins=(usr/bin/northstar)
    RUNTIME_DEPS=$(cd "$PKGROOT" \
        && dpkg-shlibdeps -O --ignore-missing-info "${scan_bins[@]}" 2>/dev/null \
        | sed -n 's/^shlibs:Depends=//p')
    rm -rf "$PKGROOT/debian"
fi
[ -n "$RUNTIME_DEPS" ] || RUNTIME_DEPS="$FALLBACK_DEPS"

set -e

if [ "$bundled_any" = 1 ] && [ -n "$RUNTIME_DEPS" ]; then
    kept=""
    OLDIFS=$IFS; IFS=','
    for dep in $RUNTIME_DEPS; do
        d=$(printf '%s' "$dep" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')
        name=${d%% *}
        if printf '%s' "$name" | grep -Eiq "$STRIP_RE"; then continue; fi
        kept="${kept:+$kept, }$d"
    done
    IFS=$OLDIFS
    RUNTIME_DEPS="$kept"
fi

cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: northstar
Version: ${VERSION}
Architecture: ${DEBARCH}
Maintainer: Andreas Røsdal <andreas.rosdal@gmail.com>
Installed-Size: ${INSTALLED_KB}
Depends: ${RUNTIME_DEPS}
Section: web
Priority: optional
Homepage: https://nordstjernen.org
Description: Northstar Web Browser — minimalist GTK 4 browser, GPL edition
 Northstar Web Browser is a small free-software web browser written from scratch in
 C with GTK 4 and libcurl. The HTML parser, CSS engine, layout, paint
 and JavaScript glue contain no third-party browser engine. SVG images
 are rendered in-engine. Licensed GPL-3.0-or-later.
EOF

cat > "$PKGROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
EOF
cp "$PKGROOT/DEBIAN/postinst" "$PKGROOT/DEBIAN/postrm"
cat >> "$PKGROOT/DEBIAN/postinst" <<'EOF'
if [ "$1" = configure ]; then
    update-alternatives --install /usr/bin/open-internet-browser open-internet-browser \
        /usr/bin/northstar 50
fi
EOF
cat > "$PKGROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = deconfigure ]; then
    update-alternatives --remove open-internet-browser /usr/bin/northstar
fi
EOF
chmod 755 "$PKGROOT/DEBIAN/postinst" "$PKGROOT/DEBIAN/postrm" \
    "$PKGROOT/DEBIAN/prerm"

DEB="$ROOT/dist/northstar_${VERSION}_${DEBARCH}.deb"
rm -f "$DEB"
dpkg-deb --root-owner-group --build "$PKGROOT" "$DEB" >/dev/null

echo
echo "Built: $DEB ($(du -h "$DEB" | cut -f1))"
echo
echo "Inspect: dpkg-deb -I $DEB"
echo "Contents: dpkg-deb -c $DEB"
echo "Install: sudo apt install $DEB"
