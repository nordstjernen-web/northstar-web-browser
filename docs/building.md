# Building and running Northstar Web Browser

Northstar Web Browser builds with **meson + ninja** and a C compiler (GCC or Clang).
The primary target is Linux; macOS and Windows are also supported. The
project is C only: no C++ compiler is needed, though WebGL links the C++
runtime that glslang (a C++ library with a C interface) depends on.

## Linux dependencies

Debian / Ubuntu:

```sh
sudo apt install build-essential git pkg-config meson ninja-build \
    libgtk-4-dev libcurl4-openssl-dev libssl-dev \
    libharfbuzz-dev libfribidi-dev libcairo2-dev libfontconfig-dev \
    libfreetype-dev libpsl-dev libsqlite3-dev libseccomp-dev libsdl3-dev \
    zlib1g-dev
```

Fedora / RHEL:

```sh
sudo dnf install gcc git pkgconf meson ninja-build gtk4-devel \
    libcurl-devel openssl-devel harfbuzz-devel \
    fribidi-devel cairo-devel fontconfig-devel freetype-devel \
    libpsl-devel sqlite-devel libseccomp-devel SDL3-devel zlib-devel
```

openSUSE:

```sh
sudo zypper install gcc git pkgconf meson ninja gtk4-devel \
    libcurl-devel libopenssl-develel harfbuzz-devel \
    fribidi-devel cairo-devel fontconfig-devel freetype2-devel \
    libpsl-devel sqlite3-devel libseccomp-devel SDL3-devel zlib-devel
```

`libseccomp` is required on Linux — `meson setup` fails without it. On
macOS and Windows it is unused and the syscall filter is a no-op.

The text stack has version floors set by web-text-lib: GLib ≥ 2.80,
HarfBuzz ≥ 8.3, fontconfig ≥ 2.15, Cairo ≥ 1.18 and FriBidi ≥ 1.0.6.
GTK must be ≥ 4.14 (≥ 4.22.1 on Windows; elsewhere the build warns below
4.22, which it prefers for the newer GSK renderers) and libcurl ≥ 8.5.
These are what Ubuntu 24.04 ships;
Debian 13 and Fedora 40 meet them too. Older releases — Ubuntu 22.04 and
20.04, Debian 12 — cannot build Northstar Web Browser from their own packages: their
GTK 4 is too old or absent, alongside the text-stack libraries.
`meson setup` checks every floor before configuring web-text-lib and
lists each library that is missing or too old in a single error.
web-text-lib is linked into the same process as GTK, so it cannot fall back to building
GLib or Cairo from source; on an older release, build inside an Ubuntu
24.04 (or newer) container instead.
With a libcurl older than 8.11 the build warns that WebSocket is
unavailable unless that libcurl was built with WebSocket support.

**Optional, auto-detected:** `libgstreamer1.0-dev` and
`libgstreamer-plugins-base1.0-dev` (`gstreamer-devel` /
`gstreamer-plugins-base-devel` on Fedora and openSUSE) for GStreamer
video decoding, the Linux default; install decoder plugins such as
`gstreamer1.0-plugins-good`, `-bad` and `gstreamer1.0-libav` to play
VP9, AV1 and H.264. `libavif-dev` (AVIF images — it pulls in a
full AV1 decoder for a format that is rare on the web, so
`-Davif=disabled` drops it), `libenchant-2-dev` (+ a dictionary such as
`hunspell-en-us`) for on-screen spell-checking, `libopusfile-dev` /
`libvorbis-dev` (which carries `vorbisfile`) for native Ogg Opus/Vorbis
decode in the in-process mixer, `libepoxy-dev` and `glslang-dev`
(`libepoxy-devel` / `glslang-devel` on Fedora and openSUSE) for WebGL,
`libthai-dev` for Thai word breaking, `libdav1d-dev`, `libvpx-dev` and
OpenH264 (where the distribution packages it) for the built-in AV1, VP9
and H.264 decoders that stand in where GStreamer has no decoder,
`libopus-dev` for Opus sound in WebM, and `libbrotli-dev` and
`libzstd-dev` for Brotli and zstd `Content-Encoding`. The build works
without them.

### Installing SDL3

On distributions without SDL3 package available, such as Ubuntu 24 and
Debian 12, it is possible to build without it and have no audio output;
or, you can build it from source and install manually.

- Go to the [SDL3 releases page](https://github.com/libsdl-org/SDL/releases), download and extract source code
- Compile the libraries using CMake

```sh
cmake -S . -B build
cmake --build build
sudo cmake --install build --prefix /usr/local
```

- You might need [SDL3: Build Dependencies](https://wiki.libsdl.org/SDL3/README-linux#build-dependencies)
- Make the built libraries known to the system

```sh
sudo ldconfig
ldconfig -p
```

This updates the cache for dynamic linker `ld`, and prints the current list
of known libraries. You should see `libSDL3.so` or similar.

## macOS dependencies

With Homebrew:

```sh
brew install meson ninja pkg-config gtk4 curl openssl@3 libpsl \
    sqlite sdl3 zlib
```

Export `PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"`
before configuring. The macOS build uses the same setup and compile commands
shown below. The optional libraries are the `libavif`, `opusfile`,
`libvorbis`, `enchant`, `libepoxy` and `glslang` formulae.

With MacPorts (verified on macOS 27.0.1, Apple Silicon, Xcode 27 /
Apple clang 21):

```sh
sudo port install meson ninja pkgconfig gtk4 +quartz curl openssl3 \
    libpsl sqlite3 sdl3 zlib
```

`+quartz` builds GTK with the native macOS backend; the default `+x11`
variant needs XQuartz to run. MacPorts' `pkg-config` already
searches `/opt/local`, so no `PKG_CONFIG_PATH` is needed. The optional
libraries are the `libavif`, `opusfile`, `libvorbis`, `enchant2` and
`libthai` ports.

## Windows dependencies

Install MSYS2, then install the `mingw-w64-ucrt-x86_64-*` packages
listed by `.github/workflows/windows.yml` into its UCRT64 environment.
MSYS2 no longer publishes GTK 4 for the older MINGW64 environment. Run
Meson from the UCRT64 shell so its compiler and `pkg-config` resolve the
MinGW libraries. `scripts/_msys_build.sh` does that from any shell: it
sets up the UCRT64 environment (or MINGW64, on a machine where only that
one has GTK 4 installed), defaults `CC` to `clang`, configures
`builddir` on first use and compiles. The binary is
`builddir/src/gtk/northstar.exe`.

```sh
C:/msys64/usr/bin/bash.exe -lc "bash /c/path/to/northstar/scripts/_msys_build.sh"
```

A few traps:

- A running `northstar.exe` locks the link target, and the link fails with
  "Permission denied". Close it (`taskkill /IM northstar.exe /F`) before
  rebuilding.
- After a source file of a static library is renamed — the WAMR
  interpreter moved from `wasm_interp_classic.c` to `wasm_interp_fast.c` —
  an existing build directory can fail at `gcc-ar ... libwamr.a: No such
  file or directory`. Delete the stale `libwamr.a` under
  `builddir/subprojects/` and rebuild.
- LTO with clang needs `llvm-ar` (`mingw-w64-ucrt-x86_64-llvm-tools`);
  `meson setup` stops without it unless configured with `-Db_lto=false`.
- The Windows CI job builds with clang and `--werror`, the Linux job with
  GCC and `--werror`. Clang 23, now in MSYS2, adds
  `-Wunused-but-set-global`, so a global that is written but never read
  fails CI.

`scripts/pack-windows.sh` builds a release tree in `builddir-release` and
bundles it with its DLLs and GTK runtime data as a portable zip in
`dist/`. `scripts/pack-windows-installer.sh` wraps that bundle into a
per-user NSIS `setup.exe` (no Administrator rights; `makensis` from
`mingw-w64-ucrt-x86_64-nsis`). `scripts/pack-msix.sh` stages it as an unsigned
Microsoft Store MSIX; it needs `rsvg-convert` (`mingw-w64-ucrt-x86_64-librsvg`)
and `makeappx.exe` from the Windows SDK, or `makemsix`.

### Visual Studio

Open `northstar.sln` in Visual Studio. The project is an NMake wrapper around
the existing Meson/MSYS2 build, not an MSVC port: **Build**, **Rebuild** and
**Clean** run the corresponding operations with MSYS2, and **Start** launches
`builddir/src/gtk/northstar.exe` with `about:start`. The project expects MSYS2
at `C:\msys64` and the Windows dependencies described above. Update
`scripts/vs-build.ps1` if MSYS2 is installed elsewhere. The Visual Studio
Debug and Release configurations both use the existing `builddir` Meson
configuration; they do not select different compiler or optimization modes.

The C files are listed for browsing in Solution Explorer. In particular,
`src/appmain.c` contains the browser's `main()`; Visual Studio's MSVC debugger
does not provide full source-level debugging support for this MinGW-built
executable.

## Visual Studio Code

The repository's `.vscode/` workspace builds, runs and debugs the browser
on all three platforms; [vscode.md](vscode.md) walks through the setup.

## Build

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/gtk/northstar
```

`meson setup` resolves two upstream projects through
`subprojects/*.wrap` — lexbor and the JavaScript engine (one of the two
QuickJS rows below) — and exposes them as the `liblexbor` / `libquickjs`
dependencies:

| Dependency | Pinned to | Resolution |
|------------|-----------|------------|
| [lexbor](https://github.com/lexbor/lexbor) — HTML parser and WHATWG URL module | master commit `145872a` (after v3.0.1) | A system lexbor ≥ 3.0.0 is used when `pkg-config` finds one; otherwise the wrap is cloned and built as a static library by `subprojects/packagefiles/lexbor/meson.build`, which stands in for lexbor's CMake build. |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) — JavaScript | master commit `147d01c` | A system quickjs-ng first, of any version; the wrap as fallback. |
| [QuickJS](https://github.com/bellard/quickjs) — JavaScript, with `-Djs_engine=quickjs` | the `2026-06-04` release commit | Always the subproject, built by the meson file in `subprojects/packagefiles/quickjs/`; upstream ships none. See [quickjs.md](quickjs.md). |

`-Djs_engine=quickjs` builds on Fabrice Bellard's original QuickJS instead
of quickjs-ng; [quickjs.md](quickjs.md) covers how it is built, the adapter
and the differences between the two.

WAMR, Wuffs, pl_mpeg, minimp3 and web-text-lib (`src/web-text-lib/`, the
Pango fork the engine lays text out with, shared with Nordstjernen Sol)
are vendored in-tree and need no network. web-text-lib is trimmed to what
the engine calls; a new caller may need a function restored from upstream
Pango, renamed with `src/web-text-lib/ns-rename.py`.
No in-tree fork of any browser engine is carried.

So the first build needs network access for whichever of those two it
cannot satisfy locally, unless the system supplies them. A tarball that must build offline has to embed them;
`debian/README.source` carries the recipe.

A quickjs-ng build also compiles `data/js/polyfills.js` with quickjs-ng's
`qjs` and `qjsc` (the subproject's, or the system's alongside a system
quickjs-ng) as a syntax and sanity check (`scripts/verify-polyfills.js`).
A `-Djs_engine=quickjs` build skips the check: that subproject builds no
tools, and a `qjsc` from Bellard's QuickJS on the `PATH` takes different
options.

`./scripts/dev.sh build` runs `meson setup` (only when needed) and
`meson compile -C builddir` in one step.

## Building with muon

[muon](https://muon.build) is an implementation of meson written in C99.
It builds the browser from the same build files, with the same options,
and has a ninja implementation (samurai) built in, so neither meson nor
ninja is needed. The other dependencies are the ones listed above;
Python 3 is still needed for a build-time script. Building with muon is
tested on Linux only.

```sh
muon setup builddir
muon -C builddir samu
./builddir/src/gtk/northstar
```

Options are given as with meson (`-Dwebgl=disabled`, `-Db_lto=false`);
meson's `--werror` is `-Dwerror=true`. `muon -C builddir install` installs,
and `-d <dir>` installs into a staging directory.

muon needs its libcurl and libarchive support to fetch and unpack the
pinned tarballs (WAMR, Wuffs and PDF.js). `muon version` lists both under
"enabled features" when muon was built with them. The
[muon CI job](../.github/workflows/muon.yml) builds muon 0.7.1 from
source this way, then builds and smoke-tests the browser with it.

Where muon 0.7.1 differs from meson:

- muon applies a wrap's `diff_files` only when the wrap also has a
  `patch_directory`. `subprojects/quickjs-ng.wrap` names one,
  `packagefiles/quickjs-ng/`, for that reason. A quickjs-ng checkout
  muon made before that lacks every quickjs-ng patch, and the link fails
  on `JS_GetCallerRealmOrNull` and other functions the patches add;
  `scripts/check-subprojects.sh` names the missing patches. Delete
  `subprojects/quickjs-ng/` and set up again.
- muon compiles the browser with `-fPIE` but links it without `-pie`, and
  links a GCC LTO build with a bare `-flto`, which runs the link-time
  partitions one at a time. The build files add `-pie` and `-flto=auto`
  to the browser's link themselves, so it is PIE and links in parallel,
  as with meson. quickjs-ng's `qjs` and `qjsc`, built for the polyfill
  check, still link one partition at a time, and GCC warns "using serial
  compilation" for each.
- A muon built with its libpkgconf backend can miss every system library
  on Debian and Ubuntu, because that backend does not search the
  multiarch `pkg-config` directory: `muon setup` then reports GLib, GTK
  and the rest as not found although `pkg-config` finds them. Configure
  with `-Dmuon.pkgconfig=exec`, or build muon with `-Dlibpkgconf=disabled`.
- `muon install` has no `--skip-subprojects`; muon's meson command-line
  layer accepts the option and ignores it. A muon install therefore also
  installs quickjs-ng's `qjs`, `qjsc`, static library, headers and
  `pkg-config` file, which the distribution packages leave out. Delete
  them from the staging directory, or package from a meson build.

## Updating a pinned dependency

Editing the `revision =` line in a wrap is not enough on its own: meson
keeps the checkout it already has. Move the pin, then reset the checkout
to it.

```sh
$EDITOR subprojects/lexbor.wrap          # revision = <new tag or commit>
meson subprojects update --reset lexbor
meson compile -C builddir
```

`--reset` hard-resets the subproject to the wrap's revision and works on
the shallow clones these wraps ask for, in either direction. Deleting
`subprojects/<name>/` and reconfiguring does the same thing more slowly.

Local patches ride on top of the fetched sources, named by
`diff_files` in the wraps and living in `subprojects/packagefiles/`: a
bounds check for lexbor, a Windows link fix for quickjs-ng, and, for the
original QuickJS, a sort fix that makes `Array.prototype.sort` always call
its comparator. Regenerate
them against the new sources when a pin moves, or drop one upstream has
taken. A system lexbor or quickjs-ng does not get them.

WAMR and Wuffs are tag archives pinned by SHA-256 in `subprojects/wamr.wrap`
and `subprojects/wuffs.wrap`. Meson extracts them, applies the patches in
`diff_files` (`wamr-northstar.patch`, the local interpreter changes, and
`wuffs-jpeg-16bit-dqt.patch`), and copies the overlay from
`subprojects/packagefiles/wamr/` (the build file and `ns_wamr.c`, the narrow
accessors the WebAssembly JS API needs) or `packagefiles/wuffs/`. To move a
pin, change `directory`, `source_url`, `source_filename` and `source_hash`,
delete the old extracted directory, and refresh the patch.

The vendored copies are updated by replacing the files. minimp3
(`src/audio/minimp3.h`) is byte-identical to its upstream release, so
syncing it is a copy.
pl_mpeg carries one Northstar Web Browser change — a bounds fix for half-pel motion
compensation, which read a row past the plane — marked in place in the
vendored header; keep it when syncing.
[`../THIRD-PARTY-LICENSES.md`](../THIRD-PARTY-LICENSES.md) lists every
component and every patch, and is the file to update when any of this
moves.

Verify a dependency move the way any other change is verified: a clean
`meson compile` with no new warnings, `./scripts/dev.sh smoke` for layout
drift, and the browser launched on the paths the dependency touches. A
text-layout change wants more than that, because the shaping cache can be
wrong only for the second paragraph that shares a word with the first:
dump the layout of every page in `data/render-tests/` with the cache off
and on, require the two sets to be identical, then check that
`NS_PANGO_SHAPE_CACHE=verify` reports no mismatch over the same pages.

```sh
for f in data/render-tests/*.html; do
    n=$(basename "$f" .html)
    NS_PANGO_SHAPE_CACHE=0 ./builddir/src/gtk/northstar --headless \
        --dump=layout "$f" > "/tmp/off-$n.txt"
    ./builddir/src/gtk/northstar --headless --dump=layout "$f" > "/tmp/on-$n.txt"
    cmp -s "/tmp/off-$n.txt" "/tmp/on-$n.txt" || echo "differs: $n"
done
```

## Fast iteration

`ccache` is the biggest build-time win and meson picks it up
automatically; a warm-cache rebuild drops from ~35 s to ~1 s. Install it
once (`apt install ccache` / `dnf install ccache`). Optionally use the
`lld` linker for faster final links (`CC_LD=lld meson setup builddir`).

## Meson options

| Option | Default | Effect |
|--------|---------|--------|
| `wasm` | `auto` | Build the WebAssembly JS API over vendored WAMR. `auto` turns it off on 32-bit x86, which WAMR does not support; disable it on other platforms WAMR does not support. |
| `js_engine` | `quickjs-ng` | JavaScript engine: `quickjs-ng`, or `quickjs` for Fabrice Bellard's original QuickJS. See [quickjs.md](quickjs.md). |
| `av1`, `vp9`, `h264` | `auto` | The built-in AV1 (libdav1d), VP9 (libvpx) and H.264 (OpenH264) `<video>` decoders, each built when its library is present. They decode where GStreamer or Media Foundation is not built in or has no decoder for the codec, and with `NS_VIDEO_DECODER=native`. Only Cisco-built OpenH264 binaries are covered by the H.264 patent license Cisco pays for. |
| `mediafoundation` | `auto` | Decode `<video>` through Windows Media Foundation, the Windows default. Not built elsewhere. |
| `audio` | `auto` | Enable in-process audio playback (needs SDL3 3.2 or newer). |
| `gstreamer` | `auto` | Decode `<video>` through GStreamer decoder plugins. `auto` enables it on Linux when GStreamer (≥ 1.18, with gstreamer-app and gstreamer-video) is present, where it becomes the default video decoder; other platforms need `-Dgstreamer=enabled`. Not a required dependency: without it the native libdav1d/libvpx/OpenH264 backends decode video. |
| `avif` | `auto` | Decode AVIF through libavif. Disabling drops a full AV1 decoder; AVIF images then fail to decode. |
| `webgl` | `auto` | WebGL 1 / 2 over an offscreen OpenGL context (libepoxy), with every shader validated by glslang first. `auto` builds it when both libraries are present; `enabled` requires them; `disabled` leaves it out, and `getContext("webgl")` returns `null`. See [webgl.md](webgl.md). |
| `pdfjs` | `auto` | The PDF viewer, built on PDF.js and embedded at build time. `auto` builds it when PDF.js is available; `enabled` makes PDF.js a required dependency, so the build fails without it; `disabled` leaves the viewer out, and a PDF shows a link to the file. |
| `pdfjs_dir` | *(empty)* | Directory of an installed PDF.js to use instead of the pinned release, such as Arch's `/usr/share/pdf.js`. See below. |
| `build_date` | *(configure date)* | Build-date stamp shown in the About dialog. |

Set with `-Dname=value`, e.g. `meson setup builddir -Dwasm=disabled`.

### Where PDF.js comes from

PDF.js is never part of this repository. Without `pdfjs_dir`, meson fetches
the release pinned in `subprojects/pdfjs-dist.wrap` (by version and
SHA-256) into the git-ignored `subprojects/pdfjs-dist-<version>/`. With
`pdfjs_dir`, it uses an installed copy instead, which
`meson.build` vets at setup: it must hold the library, the
worker, the three WebAssembly image decoders and the Foxit standard fonts
(the BSD-licensed glyph data for PDFs that name a standard font without
embedding it), in either the pdfjs-dist
layout (`build/pdf.min.mjs`, `wasm/`, `standard_fonts/`) or the generic
viewer layout distributions install (`build/pdf.mjs`, `web/wasm/`,
`web/standard_fonts/`), and be PDF.js 6.5
or a later 6.x — the range the viewer in `data/js/pdf-viewer.js` is written
for. A directory that fails the check fails the build, so a distribution
package cannot silently ship a viewer against an untested PDF.js.

```sh
meson setup builddir -Dpdfjs=enabled -Dpdfjs_dir=/usr/share/pdf.js   # Arch: pacman -S pdfjs
```

`data/fixtures/compressed-text.pdf`, written by `scripts/gen-pdf-fixture.py`,
is the viewer's smoke test: its page content is compressed and holds
more text than PDF.js sends in one chunk, so the Linux CI job's text dump
shows its last marker only when the worker inflates the page and the
text stream delivers every chunk.

Debian and Ubuntu's `libjs-pdf` (2.14) is too old; Fedora, MSYS2 and
Homebrew have no package, so those builds use the pinned release. To move
the pin, change `directory`, `source_url`, `source_filename` and
`source_hash` in the wrap and the version in
`subprojects/packagefiles/pdfjs-dist/meson.build`, delete the old
`subprojects/pdfjs-dist-*/`, check the new release against the viewer,
and raise the `version_compare` range in the `pdfjs_dir` block of `meson.build` if the
supported range moves.

## Command line

`northstar [options] [URL | FILE]` opens the start page, or the given URL
or local file. A normal launch runs under a watchdog process that restarts
the browser, with its session, after a crash or hang. `debian/northstar.1`
is the manual page.

| Option | Effect |
|--------|--------|
| `--version` | Print the version and exit. |
| `--private` | Keep history, cookies, cache and site storage off disk for this session. |
| `--proxy=URL` | Use an HTTP(S) proxy. |
| `--ignore-certificate-errors` | Open pages with a self-signed or otherwise untrusted TLS certificate, such as home lab servers, after a warning dialog. The page is marked not secure; its own subresources, `fetch()` calls and WebSockets to the same host load too, and other hosts stay fully verified. See [security-model.md](security-model.md#network). |
| `--window-size=WxH` | Initial window size. |
| `--gsk-renderer=gl\|ngl\|vulkan\|cairo` | GTK renderer. |
| `--no-watchdog` | Run without the watchdog. |
| `--print-config` | Print the effective configuration and exit. |
| `--trace=FILE` | Write a Chrome trace-event JSON of frame phases, relayouts, scripts, fetches and decodes, for Perfetto (ui.perfetto.dev) or `chrome://tracing`. Works in the GUI and headless. |
| `--` | End of options: every argument after it is a URL or file, never an option. The URL and file handlers are registered this way (`northstar -- "%1"`), so a URL passed in by another program cannot add options. |

## Headless mode (scripting / testing)

The browser can render without a display, which is how the fixtures are
exercised and how behaviour can be scripted. Any of `--headless`,
`--dump=`, `--export=`, `--eval=`, `--act=`, `--inspect=`, `--inspect-at=`
or `--wpt` selects it; headless runs are never supervised by the watchdog.

```sh
# Dump a page to PNG
./builddir/src/gtk/northstar --headless \
    --url="https://example.com/" --dump="png:/tmp/out.png" --viewport=1000

# Export a page to PDF
./builddir/src/gtk/northstar \
    --url="http://localhost:8080/book.html" --export="pdf:/tmp/book.pdf"

# Evaluate JavaScript against a loaded page and print the result
./builddir/src/gtk/northstar --headless \
    --url="file://$HOME/Downloads/page.html" --eval="document.title"
```

| Option | Effect |
|--------|--------|
| `--url=URL`, or a bare URL or file path | The page to load. |
| `--dump=text\|dom\|layout\|none` | Print the page text (the default), the DOM, the box tree, or nothing. |
| `--dump=png:FILE` | Write a screenshot. This also writes `FILE-initial.png`, captured at the first render; the named file follows after `--time-ms`. |
| `--export=pdf:FILE` | Write the page as a paginated PDF, laid out with print media at the `@page` size and margins (A4 with half-inch margins by default; an explicit `--viewport` width overrides the page width). Links are clickable (`#fragment` links jump within the document) and the text stays selectable. Nothing the page does not have is added: no bookmark outline or table of contents. |
| `--viewport=W` or `WxH`, `--viewport-height=H` | Viewport size in CSS pixels (default width 1000). |
| `--settle-ms=N` | Time to let the page settle before output (default 200). |
| `--time-ms=N` | Page time between the first and final screenshot (default 1000). |
| `--eval=EXPR` | Evaluate JavaScript after settling and print the result. |
| `--act='ACTION; ACTION'` | Input before output: `click X,Y`, `rightclick X,Y`, `move X,Y` (pointer motion: hover, `:hover`, mouseover/mousemove), `down X,Y` and `up` (press and release, with `move`s between them for a drag), `type TEXT`, `key NAME`, `wait MS`, `eval JS`, `evalfile PATH`, and more. Outside `--dump=png`, a point may be `sel=CSS [dx=N] [dy=N]`: the centre of the first matching element when the action runs. |
| `--inspect=SELECTOR`, `--inspect-at=X,Y` | Report the layout boxes matching a selector, or at a point. |
| `--wpt`, `--wpt-timeout-ms=N` | Run a testharness.js page and report every subtest; see [compliance.md](compliance.md). |
| `--debug[=info,warn,error,render,net,js]` | Stream engine events to stderr; `net` adds fetch, layout and shaping-cache totals at exit. |

Two traps in `--act`. Actions are split on `;`, so inline JS in an `eval`
action is cut at its first statement separator — put anything longer
than one expression in a file and use `evalfile`. And `type` inserts
text without keyboard events, so use `key` when the page's behaviour
depends on them: `key` with a single character, or `Enter`, presses it
as the GUI does (keydown, keypress, the character typed unless a
listener cancels, keyup).

On Linux the sandbox confines what a headless run can read. A local
page has to live somewhere Landlock allows: a build run from
`builddir/` can read its own source tree, so the fixtures under `data/`
load as files, but a page in `/tmp` or elsewhere in `$HOME` does not —
serve it over HTTP instead, as `scripts/render-tests.sh` does. The output
directory of a PNG dump is added to the sandbox
automatically.

## Configuration and environment

Settings live in `~/.config/northstar/northstar.conf`, one `key = value`
per line; `--print-config` prints every key with its effective value.
These environment variables override the file:

| Variable | Effect |
|----------|--------|
| `NS_HOME_URL`, `NS_USER_AGENT`, `NS_COMPAT_MODE` | Start page, User-Agent, compatibility mode. |
| `NS_HTTP_PROXY`, `NS_HTTPS_PROXY`, `NS_NO_PROXY`, `NS_DOH_URL` | Proxies and the DNS-over-HTTPS resolver. |
| `NS_GSK_RENDERER` | GTK renderer, as for `--gsk-renderer`. |
| `NS_JS_EVAL_BUDGET_MS` | Time one script may run before it is interrupted (`js_eval_budget_ms`, default 60000, at most 3600000). JetStream 2 needs more than the default: each of its benchmarks runs as one script for minutes on an interpreter. |
| `NS_PRIVATE` | Start in private mode. |
| `NS_NO_WEBGL` | Turn WebGL off for this run, which also restores the full content-thread seccomp filter. |
| `NS_NO_WATCHDOG`, `NS_NO_CACHE`, `NS_NO_IMAGES`, `NS_NO_LOCAL_STORAGE`, `NS_NO_HTTPS_FIRST`, `NS_NO_PRELOAD_SCAN`, `NS_NO_ASYNC_IMG_DECODE`, `NS_NO_HARDEN_ALLOC` | Turn the matching feature off. |

Others the browser reads:

| Variable | Effect |
|----------|--------|
| `NS_SAFE_BROWSING_API_KEY` | Google Safe Browsing API key. Read at build time into the generated `ns_sb_key.h` (release builds get it from the repository secret); set at run time it overrides the built-in key. Without a key only the local blocklist is used. |
| `NS_SAFEBROWSING_LIST` | Blocklist file used instead of `~/.config/northstar/safebrowsing.list` and the bundled list. |
| `NS_EXTENSIONS_DIR` | WebExtensions directory (default `~/.local/share/northstar-extensions`). |
| `NS_I18N_DIR` | Directory of the `.lang` UI catalogues. |
| `NS_LOG_FILE`, `NS_NO_LOG_FILE` | Mirror the debug event log to a file (on by default only on Windows). |
| `NS_CAMERA_ALLOW` | Grant every camera request without asking. Headless/test runs only; ignored by the interactive browser. |
| `NS_EARLY_JS_FILE` | Script file evaluated before each page's own scripts. Headless/test runs only; ignored by the interactive browser. |
| `NS_FRAME_DUMP` | Directory to write every 30th rendered frame to as PNG (debugging; headless and tooling runs only). |
| `NS_FORCE_HTTP3` | Use HTTP/3 directly instead of waiting for Alt-Svc. |
| `NS_PANGO_SHAPE_CACHE` | `0` turns the web-text-lib caches off; `verify` checks every cached shape against a fresh one. |
| `NS_HEADLESS_LEGACY` | Send every headless run through the direct engine path. |

Running as `root` is refused for safety; `NS_ALLOW_ROOT=1` overrides it,
and only belongs in a throwaway container — though `scripts/dev.sh` and
the WPT runners set it by default so they work in one. `NS_NO_SANDBOX`
disables the whole sandbox and `NS_NO_SECCOMP` only the syscall filter;
they are for debugging only. Boolean switches like these and the
`NS_NO_*` feature switches count as unset when empty or `0`, `false`,
`no` or `off`.

## Smoke and render-test fixtures

`./scripts/dev.sh smoke` renders the fixture list built into
`scripts/dev.sh` — the pages in `data/fixtures/` — headless, and diffs
each against its baseline in `data/baseline/`, reporting drift. Text
fixtures use `--dump=text`; the `geo-*` ones use `--dump=layout`, which is
text-free and fixed-size so the diff is font-stable. After an intended
change, refresh with `./scripts/dev.sh baselines` (or
`dev.sh baseline <target>` for one).

`scripts/render-tests.sh [out-dir]` serves `data/render-tests/*.html` on
port 8137 and renders each to a PNG in `render-tests-out/` (800 px wide,
or `NS_TEST_VIEWPORT`) for visual inspection.

Neither is an automated test suite, and this project has none by design:
the baselines catch drift, the PNGs are read by eye, and correctness is
verified by running the browser.

## Other scripts

| Script | Purpose |
|--------|---------|
| `wpt-run.sh`, `wpt-local.sh`, `wpt-fast.sh` | Web-platform-tests runners; see [compliance.md](compliance.md). |
| `_msys_build.sh`, `_msys_env.sh`, `_msys_eval.sh`, `_msys_act.sh`, `_msys_wpt.sh`, `run-windows.ps1`, `smoke-windows.ps1`, `check-windows-bundle.ps1` | Build, drive and smoke-test the Windows binary; `check-windows-bundle.ps1` starts the packaged bundle with only the Windows system folders on PATH. |
| `pack-linux.sh`, `pack-deb.sh`, `pack-rpm.sh`, `pack-srpm.sh`, `pack-appimage.sh`, `pack-bsd.sh`, `pack-windows.sh`, `pack-windows-installer.sh`, `pack-msix.sh` | Release packaging. |
| `nightly.sh`, `nightly-distro-build.sh` | Nightly build orchestration. |
| `check-subprojects.sh` | Fail when a `subprojects/` checkout is not at the revision its `.wrap` pins; the packaging scripts run it first. |
| `ci-linux-i386.sh` | The 32-bit x86 CI build and smoke test, run inside a 32-bit Debian 13 container (`debian:trixie` with `--platform linux/386`). |
| `ci-musl.sh` | The musl CI build and smoke test, run inside an `alpine:3.24` container. |
| `ci-pull-image.sh` | Pull a Docker official image for CI from Docker Hub, `mirror.gcr.io` or ECR Public, whichever serves it. |
| `render-screenshots.sh` | Render each URL listed in `data/screenshots/sites.txt` to a PNG beside it. |
| `speedometer-bench.sh`, `speedometer4-bench.sh`, `sample-profile.sh` | Benchmarks and a sampling profiler. |
| `gen-architecture.py`, `gen-badge.sh`, `gen-windows-icon.py` | Regenerate the architecture diagram, the badge and the Windows icon. |
| `embed-text.py`, `verify-polyfills.js` | Build helpers: embed a file as a C array where the compiler lacks C23 `#embed` (before GCC 15 and Clang 19; newer compilers read the files directly through `src/embedded/embed.h.in`), and the polyfill smoke test meson runs under `qjs`. |

## Definition of done

A change is complete when it (1) compiles cleanly with no new warnings
under the configured GCC/Clang flags, (2) launches and the affected UI path
works when exercised manually or headless, and (3) is committed and pushed.
See [`../README.md`](../README.md) for the project scope.
