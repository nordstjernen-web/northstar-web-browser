#!/bin/sh
# ci-musl.sh: build and smoke-test Northstar Web Browser on musl libc, inside an Alpine container with the source tree mounted at /src.
set -eu

apk add --no-cache \
    build-base clang ccache pkgconf meson ninja git cmake python3 \
    gtk4.0-dev curl-dev openssl-dev uchardet-dev libpsl-dev \
    harfbuzz-dev fribidi-dev cairo-dev fontconfig-dev freetype-dev \
    sqlite-dev zlib-dev libavif-dev sdl2-dev libseccomp-dev linux-headers \
    dav1d-dev libvpx-dev openh264-dev opus-dev libthai-dev \
    opusfile-dev libvorbis-dev enchant2-dev \
    fontconfig font-dejavu ca-certificates file

git config --global --add safe.directory '*'

export CC="ccache clang"
ccache --max-size=200M
ccache --zero-stats

meson setup builddir -Db_lto=false
meson compile -C builddir
file builddir/src/gtk/northstar
ccache --show-stats

export NS_ALLOW_ROOT=1
ldd builddir/src/gtk/northstar | grep -i musl || true
./builddir/src/gtk/northstar \
    --headless --url=https://example.com --dump=text > smoke.txt
head -40 smoke.txt
grep -qi example smoke.txt
