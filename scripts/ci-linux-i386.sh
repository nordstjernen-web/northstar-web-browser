#!/bin/bash
# Build and smoke-test Northstar Web Browser on 32-bit x86 Linux.
#
# Runs inside a 32-bit Debian 13 (trixie) container -- a native 32-bit
# userland, as on antiX or Debian i386 -- with the source tree mounted
# at /src. GitHub's JavaScript actions cannot run in a 32-bit
# container, so the workflow starts this script with `docker run`:
#
#   bash scripts/ci-pull-image.sh debian:trixie linux/386
#   docker run --rm --pull=never --platform linux/386 \
#       --security-opt seccomp=unconfined -v "$PWD:/src" -w /src \
#       debian:trixie bash scripts/ci-linux-i386.sh
#
# Set CCACHE_DIR to a mounted directory to keep ccache across runs.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
echo 'Acquire::Retries "5";' > /etc/apt/apt.conf.d/80retries
apt-get update -qq --error-on=any
apt-get install -y -qq --no-install-recommends \
    ccache gcc g++ make cmake pkg-config meson ninja-build git file \
    libgtk-4-dev libcurl4-openssl-dev libssl-dev \
    libharfbuzz-dev libfribidi-dev libcairo2-dev \
    libfontconfig-dev libfreetype-dev \
    libuchardet-dev libpsl-dev libsqlite3-dev \
    libavif-dev libsdl2-dev libseccomp-dev libenchant-2-dev \
    libopusfile-dev libvorbis-dev zlib1g-dev \
    fonts-dejavu-core ca-certificates >/dev/null

# The checkout is owned by the runner user, not root.
git config --global --add safe.directory '*'

test "$(getconf LONG_BIT)" = 32
gcc -dumpmachine
gcc --version | head -1
meson --version
pkg-config --modversion gtk4

export CC="ccache gcc" CXX="ccache g++"
ccache --max-size=200M >/dev/null
ccache --zero-stats >/dev/null

builddir=${BUILDDIR:-builddir-i386}
meson setup "$builddir" --werror
meson compile -C "$builddir" 2>&1 | tee build-i386.log
ccache --show-stats

bin=$builddir/src/gtk/northstar
file "$bin"
file "$bin" | grep -q 'ELF 32-bit'

export NS_ALLOW_ROOT=1
"$bin" --headless --dump=text about:start > about.txt
head -3 about.txt
grep -q nordstjernen.org about.txt

"$bin" --headless --dump=text --settle-ms=300 \
    'data:text/html,<p id="a">before</p><script>var a=document.getElementById("a");a.textContent="js-dom-ok "+(6*7)+" "+(2n**65n)+" "+structuredClone([1,[2]])[1][0];setTimeout(function(){a.textContent+=" js-timer-ok"},50);</script>' \
    > js-page.txt
cat js-page.txt
grep -q 'js-dom-ok 42 36893488147419103232 2' js-page.txt
grep -q 'js-timer-ok' js-page.txt

# Thousands of elements matched by the same selectors go through the
# selector cache, whose hash is pointer-width sensitive.
"$bin" --headless --dump=text --settle-ms=300 \
    "data:text/html,<style>.c p{color:red}.c>p+p{color:blue}div.c p:first-child{font-weight:bold}</style><div class=c id=x></div><script>var h='';for(var i=0;i<2000;i++)h+='<p>p'+i+'</p>';document.getElementById('x').innerHTML=h;document.title='n='+document.querySelectorAll('.c p').length;</script>" \
    > selectors.txt
grep -q 'p1999' selectors.txt
