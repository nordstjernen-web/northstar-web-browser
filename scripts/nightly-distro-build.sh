#!/usr/bin/env bash
# Build and package Northstar Web Browser inside a distro container. Invoked by
# nightly.sh via `docker run` with the source tree at the current
# directory; installs that distro's deps, builds a release binary, and
# emits a portable zip plus, where the distro has one, a native package
# (.deb or .rpm) under dist/. Argument 1 selects the distro:
# debian | ubuntu | opensuse | alpine.
set -euo pipefail

DISTRO=${1:?usage: nightly-distro-build.sh <debian|ubuntu|opensuse>}
export VERSION=${VERSION:-}
export DEBIAN_FRONTEND=noninteractive
export CC=${CC:-cc}

trap 'rc=$?; echo "nightly-distro-build($DISTRO): FAILED (exit $rc) at line $LINENO: $BASH_COMMAND" >&2; exit $rc' ERR

install_apt() {
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        build-essential clang pkg-config meson ninja-build git zip unzip curl \
        dpkg-dev patchelf ca-certificates \
        libgtk-4-dev libcurl4-openssl-dev libssl-dev libpsl-dev \
        libsqlite3-dev libseccomp-dev libavif-dev libsdl3-dev \
        libepoxy-dev glslang-dev
    # The media and optional libraries debian/control lists. Without them a
    # package built here had no GStreamer (the default <video> decoder on
    # Linux), VP9 or Opus, so YouTube played no video.
    apt-get install -y --no-install-recommends \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        libdav1d-dev libvpx-dev libopus-dev libopusfile-dev libvorbis-dev \
        libenchant-2-dev libthai-dev libbrotli-dev libzstd-dev
    apt-get install -y --no-install-recommends \
        libfontconfig-dev libpango1.0-dev || true
}

install_zypper() {
    # Tumbleweed is a rolling release and its mirrors are frequently caught
    # mid-sync, so a repo's repomd.xml can transiently reference metadata
    # files (e.g. appdata.xml.gz) not yet present on the mirror, making
    # `zypper refresh` fail. Retry a few times — the sync window resolves in
    # a minute or two and download.opensuse.org rotates mirrors on retry —
    # then fall through: `zypper install` auto-refreshes, so a still-stale
    # repo fails there with a real error instead of a single flake aborting
    # the whole nightly stage.
    local i
    for i in 1 2 3 4 5; do
        zypper --non-interactive --gpg-auto-import-keys refresh && break
        echo "zypper refresh attempt $i failed (mirror likely mid-sync); retrying in $((i * 15))s..." >&2
        sleep $((i * 15))
    done
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        gcc clang pkgconf-pkg-config meson ninja git zip unzip curl \
        rpm-build patchelf ca-certificates \
        gtk4-devel libcurl-devel libopenssl-develel libpsl-devel \
        sqlite3-devel libseccomp-devel libavif-devel SDL3-devel \
        libepoxy-devel glslang-devel
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        fontconfig-devel pango-devel || true
}

install_apk() {
    apk update -q
    apk add --no-cache \
        build-base clang pkgconf meson ninja git zip alpine-sdk \
        linux-headers gtk4.0-dev curl-dev openssl-dev libpsl-dev sqlite-dev \
        harfbuzz-dev fribidi-dev cairo-dev fontconfig-dev freetype-dev zlib-dev \
        libseccomp-dev libavif-dev sdl3-dev dav1d-dev libvpx-dev opus-dev \
        libepoxy-dev glslang-dev
    apk add --no-cache \
        libthai-dev || true
}

case "$DISTRO" in
    debian|ubuntu) install_apt ;;
    opensuse)      install_zypper ;;
    alpine)        install_apk ;;
    *) echo "unknown distro: $DISTRO" >&2; exit 2 ;;
esac

git config --global --add safe.directory "$(pwd)" || true

if [ -z "${NS_BUILD_JOBS:-}" ]; then
    mem_gb=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
    cores=$(nproc)
    jobs=$(( mem_gb / 2 ))
    [ "$jobs" -lt 2 ] && jobs=2
    [ "$jobs" -gt "$cores" ] && jobs=$cores
    export NS_BUILD_JOBS=$jobs
fi
export NS_BUILD_LTO=${NS_BUILD_LTO:-false}
echo "nightly-distro-build($DISTRO): building with -j${NS_BUILD_JOBS} lto=${NS_BUILD_LTO} (mem-bounded)"

./scripts/pack-linux.sh

case "$DISTRO" in
    debian|ubuntu) ./scripts/pack-deb.sh ;;
    opensuse)      ./scripts/pack-rpm.sh ;;
esac

echo
echo "nightly-distro-build($DISTRO): artifacts in dist/:"
ls -1 dist/*.zip dist/*.deb dist/*.rpm 2>/dev/null || true
