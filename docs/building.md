# Building and running Northstar Web Browser

Northstar Web Browser builds with **meson + ninja** and a C compiler (GCC or Clang).
The primary target is Linux; macOS and Windows are also supported. The
project declares C++ as well, so meson wants a C++ compiler present even
though no C++ source is built.

## Linux dependencies

Debian / Ubuntu:

```sh
sudo apt install build-essential git pkg-config meson ninja-build cmake \
    libgtk-4-dev libcurl4-openssl-dev libssl-dev libuchardet-dev \
    libharfbuzz-dev libfribidi-dev libcairo2-dev libfontconfig-dev \
    libfreetype-dev libpsl-dev libsqlite3-dev libseccomp-dev libsdl2-dev \
    zlib1g-dev
```

Fedora / RHEL:

```sh
sudo dnf install gcc gcc-c++ git pkgconf meson ninja-build cmake gtk4-devel \
    libcurl-devel openssl-devel uchardet-devel harfbuzz-devel \
    fribidi-devel cairo-devel fontconfig-devel freetype-devel \
    libpsl-devel sqlite-devel libseccomp-devel SDL2-devel zlib-devel
```

openSUSE:

```sh
sudo zypper install gcc gcc-c++ git pkgconf meson ninja cmake gtk4-devel \
    libcurl-devel libopenssl-devel libuchardet-devel harfbuzz-devel \
    fribidi-devel cairo-devel fontconfig-devel freetype2-devel \
    libpsl-devel sqlite3-devel libseccomp-devel libSDL2-devel zlib-devel
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

**Optional, auto-detected:** `libavif-dev` (AVIF images — it pulls in a
full AV1 decoder for a format that is rare on the web, so
`-Davif=disabled` drops it), `libenchant-2-dev` (+ a dictionary such as
`hunspell-en-us`) for on-screen spell-checking, `libopusfile-dev` /
`libvorbis-dev` (which carries `vorbisfile`) for native Ogg Opus/Vorbis
decode in the in-process mixer,
and `libthai-dev` for Thai word breaking. The build works without them.

## macOS dependencies

With Homebrew:

```sh
brew install meson ninja pkg-config cmake gtk4 curl openssl@3 uchardet libpsl \
    sqlite sdl2 zlib
```

Export `PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"`
before configuring. The macOS build uses the same setup and compile commands
shown below. The optional libraries are the `libavif`, `opusfile`,
`libvorbis` and `enchant` formulae.

With MacPorts (verified on macOS 27.0.1, Apple Silicon, Xcode 27 /
Apple clang 21):

```sh
sudo port install meson ninja pkgconfig cmake gtk4 +quartz curl openssl3 \
    uchardet libpsl sqlite3 libsdl2 zlib
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
  file or directory`. Delete `builddir/src/wamr/libwamr.a` and rebuild.
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
| [lexbor](https://github.com/lexbor/lexbor) — HTML parser and WHATWG URL module | `v3.0.1` | A system lexbor ≥ 3.0.0 is used when `pkg-config` or CMake finds one; otherwise the wrap is cloned and its static library built through meson's CMake module. |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) — JavaScript | master commit `9b06171` | A system quickjs-ng first, of any version; the wrap as fallback. |
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
quickjs-ng) as a syntax and sanity check (`scripts/verify-polyfills.py`).
A `-Djs_engine=quickjs` build skips the check: that subproject builds no
tools, and a `qjsc` from Bellard's QuickJS on the `PATH` takes different
options.

`./scripts/dev.sh build` runs `meson setup` (only when needed) and
`meson compile -C builddir` in one step.

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

The vendored copies are updated by replacing the files. Wuffs
(`subprojects/wuffs/wuffs-v0.4.c`) and minimp3 (`src/audio/minimp3.h`) are
byte-identical to their upstream releases, so syncing either is a copy.
pl_mpeg carries one Northstar Web Browser change — a bounds fix for half-pel motion
compensation, which read a row past the plane — marked in place in the
vendored header; keep it when syncing. WAMR (`src/wamr/`) is a subset of
upstream: `core/` plus `ns_wamr.c`, the narrow accessors the WebAssembly
JS API needs, which are Northstar Web Browser's own.
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
| `audio` | `auto` | Enable in-process audio playback (needs SDL2). |
| `avif` | `auto` | Decode AVIF through libavif. Disabling drops a full AV1 decoder; AVIF images then fail to decode. |
| `build_date` | *(configure date)* | Build-date stamp shown in the About dialog. |

Set with `-Dname=value`, e.g. `meson setup builddir -Dwasm=disabled`.

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
| `--window-size=WxH` | Initial window size. |
| `--gsk-renderer=gl\|ngl\|vulkan\|cairo` | GTK renderer. |
| `--no-watchdog` | Run without the watchdog. |
| `--print-config` | Print the effective configuration and exit. |
| `--trace=FILE` | Write a Chrome trace-event JSON of frame phases, relayouts, scripts, fetches and decodes, for Perfetto (ui.perfetto.dev) or `chrome://tracing`. Works in the GUI and headless. |

## Headless mode (scripting / testing)

The browser can render without a display, which is how the fixtures are
exercised and how behaviour can be scripted. Any of `--headless`,
`--dump=`, `--eval=`, `--act=`, `--inspect=`, `--inspect-at=` or `--wpt`
selects it; headless runs are never supervised by the watchdog.

```sh
# Dump a page to PNG
./builddir/src/gtk/northstar --headless \
    --url="https://example.com/" --dump="png:/tmp/out.png" --viewport=1000

# Evaluate JavaScript against a loaded page and print the result
./builddir/src/gtk/northstar --headless \
    --url="file://$HOME/Downloads/page.html" --eval="document.title"
```

| Option | Effect |
|--------|--------|
| `--url=URL`, or a bare URL or file path | The page to load. |
| `--dump=text\|dom\|layout\|none` | Print the page text (the default), the DOM, the box tree, or nothing. |
| `--dump=png:FILE`, `pdf:FILE`, `print:FILE` | Write a screenshot, the page as one long PDF sheet, or the page paginated for print. These also write `FILE-initial.ext`, captured at the first render; the named file follows after `--time-ms`. |
| `--viewport=W` or `WxH`, `--viewport-height=H` | Viewport size in CSS pixels (default width 1000). |
| `--settle-ms=N` | Time to let the page settle before output (default 200). |
| `--time-ms=N` | Page time between the first and final image or PDF capture (default 1000). |
| `--eval=EXPR` | Evaluate JavaScript after settling and print the result. |
| `--act='ACTION; ACTION'` | Input before output: `click X,Y`, `rightclick X,Y`, `move X,Y` (pointer motion: hover, `:hover`, mouseover/mousemove), `down X,Y` and `up` (press and release, with `move`s between them for a drag), `type TEXT`, `key NAME`, `wait MS`, `eval JS`, `evalfile PATH`, and more. Outside `--dump=png`, a point may be `sel=CSS [dx=N] [dy=N]`: the centre of the first matching element when the action runs. |
| `--inspect=SELECTOR`, `--inspect-at=X,Y` | Report the layout boxes matching a selector, or at a point. |
| `--wpt`, `--wpt-timeout-ms=N` | Run a testharness.js page and report every subtest; see [compliance.md](compliance.md). |
| `--debug[=info,warn,error,render,net,js]` | Stream engine events to stderr; `net` adds fetch, layout and shaping-cache totals at exit. |

Two traps in `--act`. Actions are split on `;`, so inline JS in an `eval`
action is cut at its first statement separator — put anything longer
than one expression in a file and use `evalfile`. And `type` inserts
text without keyboard events, so use `key` when the page's behaviour
depends on `keydown`.

Two dump modes write PDF and differ in how much they know about paper.
`--dump=pdf:PATH` writes the page as one long unpaginated sheet, the same
output as *Save Page as PDF…*. `--dump=print:PATH` runs the print path
instead — `@media print` matching, `@page` sizing, and the break
properties cutting the document into sheets — so it is how pagination is
checked without a printer attached:

```sh
./builddir/src/gtk/northstar --headless \
    --dump="print:/tmp/paged.pdf" data/render-tests/print-pagination.html
```

On Linux the sandbox confines what a headless run can read. A local
page has to live somewhere Landlock allows: a build run from
`builddir/` can read its own source tree, so the fixtures under `data/`
load as files, but a page in `/tmp` or elsewhere in `$HOME` does not —
serve it over HTTP instead, as `scripts/render-tests.sh` does. The output
directory of a PNG, PDF or print dump is added to the sandbox
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
| `NS_PRIVATE` | Start in private mode. |
| `NS_NO_WATCHDOG`, `NS_NO_CACHE`, `NS_NO_IMAGES`, `NS_NO_LOCAL_STORAGE`, `NS_NO_HTTPS_FIRST`, `NS_NO_PRELOAD_SCAN`, `NS_NO_ASYNC_IMG_DECODE`, `NS_NO_HARDEN_ALLOC` | Turn the matching feature off. |

Others the browser reads:

| Variable | Effect |
|----------|--------|
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
| `_msys_build.sh`, `_msys_env.sh`, `_msys_eval.sh`, `_msys_act.sh`, `_msys_wpt.sh`, `run-windows.ps1`, `smoke-windows.ps1` | Build, drive and smoke-test the Windows binary. |
| `pack-linux.sh`, `pack-deb.sh`, `pack-rpm.sh`, `pack-srpm.sh`, `pack-appimage.sh`, `pack-bsd.sh`, `pack-windows.sh`, `pack-windows-installer.sh`, `pack-msix.sh` | Release packaging. |
| `nightly.sh`, `nightly-distro-build.sh` | Nightly build orchestration. |
| `check-subprojects.sh` | Fail when a `subprojects/` checkout is not at the revision its `.wrap` pins; the packaging scripts run it first. |
| `ci-linux-i386.sh` | The 32-bit x86 CI build and smoke test, run inside a 32-bit Debian 13 container (`debian:trixie` with `--platform linux/386`). |
| `ci-musl.sh` | The musl CI build and smoke test, run inside an `alpine:3.24` container. |
| `ci-pull-image.sh` | Pull a Docker official image for CI from Docker Hub, `mirror.gcr.io` or ECR Public, whichever serves it. |
| `render-screenshots.sh` | Render each URL listed in `data/screenshots/sites.txt` to a PNG beside it. |
| `speedometer-bench.sh`, `speedometer4-bench.sh`, `sample-profile.sh` | Benchmarks and a sampling profiler. |
| `gen-architecture.py`, `gen-badge.sh`, `gen-windows-icon.py` | Regenerate the architecture diagram, the badge and the Windows icon. |
| `embed-text.py`, `verify-polyfills.py`, `verify-polyfills.js` | Build helpers: embed the JS polyfills and hooks, check the polyfills. |

## Definition of done

A change is complete when it (1) compiles cleanly with no new warnings
under the configured GCC/Clang flags, (2) launches and the affected UI path
works when exercised manually or headless, and (3) is committed and pushed.
See [`../README.md`](../README.md) for the project scope and the no-AI
contribution policy.
