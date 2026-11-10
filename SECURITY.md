# Security policy

Northstar Web Browser is a small independent web browser. Security fixes ship from
`main`; only the latest tagged release is supported.

## Reporting

Report security issues by e-mail to:  andreas.rosdal@hotmail.com

Please include the version (shown in the About Northstar Web Browser dialog),
your OS, a minimal reproducer (URL or self-contained HTML), and your
assessment of the impact.

## Threat model

The attacker controls fetched HTML, CSS, JavaScript, images, fonts,
WebAssembly and media. The user, the kernel, and the local filesystem
outside the sandbox allow-list are trusted.

**In scope:** memory-safety bugs in our C code; Linux Landlock/seccomp,
macOS Seatbelt and Windows process-mitigation bypasses; same-origin,
iframe-sandbox, cookie and HTTP-cache partitioning bypasses; HSTS,
mixed-content and CSP bypasses for the parts enforced below; URL-bar
spoofing.

**Out of scope:** bugs in third-party libraries (libcurl, GTK 4, GLib,
lexbor, QuickJS, Wuffs, FreeType, codecs, …) — report upstream; features
we don't implement (WebGL, WebGPU, WebRTC, EME/DRM, JIT); Spectre-class
side channels; attacks that already need local code execution; the
*Known gaps* below.

## Architecture

The browser is **single-process**. The HTML/CSS/JS/layout engine runs on
a thread of the GTK shell process; media decoding and audio run on
worker threads of the same process. There is no renderer process, so the
sandboxes below contain the whole browser, not a renderer, and a
memory-safety bug in the engine is not contained between pages.

A GUI launch also starts a watchdog supervisor (`src/watchdog.c`) that
restarts the browser on a crash or hang. It loads no web content and is
not sandboxed. `--no-watchdog` or `NS_NO_WATCHDOG` disables it.

## Hardening

- **Compiler and linker** (`meson.build`): PIE, full RELRO, non-executable
  stack, `-fstack-protector-strong`, `-fstack-clash-protection`,
  `-fcf-protection=full`, `_FORTIFY_SOURCE=3`, `-Wformat=2`. There is no
  JIT: JavaScript runs on the QuickJS interpreter and WebAssembly on the
  WAMR fast interpreter.
- **Heap:** on glibc, `M_PERTURB` poisons freed and new allocations
  (`harden_allocator`, `NS_NO_HARDEN_ALLOC`).
- **Privileges** (`src/security.c`): Linux and macOS refuse to run as
  root (exit status 77). Windows relaunches an elevated instance with the
  shell's medium-integrity token, or asks before running elevated.
  `NS_ALLOW_ROOT=1` overrides both.

### Linux

Installed from `main()` before any content is parsed, in GUI and headless
runs alike. Both layers **fail open** on kernels that lack them.

- **Landlock.** Read+execute: `/usr`, `/lib*` and the executable's own
  directory. Read-only: `/etc`, `/proc`, `/sys`, `/run`, font/theme
  directories, the XDG config, data and cache directories, and the
  browser's data files. Read+write: the browser's own
  `~/.config|.local/share|.cache/northstar`, the download directory,
  `/dev/null`, `/dev/shm`, GPU, sound and video device nodes. The rest of
  `$HOME` (`~/.ssh`, documents, …) is unreachable, `file:` URLs included.
  No writable path is executable, and symlink creation is denied.
- **seccomp-bpf.** A default-deny allow-list of 266 syscalls
  (`ns_seccomp_allowed_names[]`), applied to every thread with TSYNC and
  `PR_SET_NO_NEW_PRIVS`. No `execve`, `ptrace`, `bpf`, `mount`,
  `io_uring`, `userfaultfd`, `perf_event_open` or module syscalls. A
  second filter refuses `TIOCSTI`/`TIOCLINUX`, namespace `clone` flags,
  `unshare`/`setns`, `SHM_EXEC`, and signals to other processes.
- **Content-thread filter.** The engine thread, every thread it starts,
  download threads, and the whole process in headless runs also refuse
  executable anonymous or shared mappings, `PROT_EXEC` via `mprotect`,
  `memfd_create` and new `AF_UNIX` sockets (so no D-Bus, `ssh-agent` or
  X server). The GUI main thread is exempt, because GPU drivers JIT shaders and
  Wayland/D-Bus need sockets, and so is the audio output thread, which is
  started before the engine thread. The filter is per thread, so this is
  hardening, not a boundary.

`NS_NO_SANDBOX=1` disables Landlock, seccomp and the macOS profile;
`NS_NO_SECCOMP=1` disables seccomp alone. Other security-relevant
variables are documented in `docs/building.md`.

### macOS

A Seatbelt profile (`sandbox_init`) denies filesystem writes except to
temporary and runtime directories, `/dev`, the browser's own directories
and Downloads. Reads are not restricted.

### Windows

`SetProcessMitigationPolicy` at startup, best-effort: forced ASLR
relocation, strict handle checks, extension-point disable (AppInit DLLs,
LSPs), image-load restrictions (no remote or low-IL DLLs, prefer
System32), and no child processes. Headless runs also ban dynamic code;
the GUI does not, because of GPU drivers and shell extensions. There is no
per-path filesystem sandbox (no AppContainer). `NS_NO_WIN32_MITIGATIONS=1`
disables the suite.

## Network

- libcurl with peer and host verification, `http`/`https`/`ftp` only, no
  HTTPS→HTTP redirect downgrade, at most 10 redirects, and a response-size
  cap. HSTS is persisted, and top-level navigations try HTTPS first.
  EventSource and WebSocket get the same protocol limits and HSTS upgrade.
- Certificate errors: with `tls_allow_insecure_override` (off by default)
  or `--ignore-certificate-errors` only a top-level navigation may proceed,
  never for an HSTS host; it is marked not secure and the unverified
  request sends and stores no cookies. With the command-line option, later
  requests to that host skip verification for the rest of the run, are
  never cached, and do not pin HSTS.
- Every subresource goes through `ns_fetch_policy_check`
  (`src/fetch_policy.c`), on the request and at every redirect hop:
  `file:` only from `file:` documents; mixed content blocked, except that
  images and media are upgraded; then CSP by destination. Frames' requests
  carry the frame's own policy.

## Origin isolation

- **Partitioning.** Cookies and the HTTP cache are partitioned per
  top-level site (via the Public Suffix List). Requests inside a frame are
  keyed by the page's site; third-party cookies are blocked by default
  (`cookie_policy`), for network requests and `document.cookie` alike.
  Cache file names are `SHA-256(URL || partition)`.
- **CSP** (`src/csp.c`), from headers and `<meta>`: `default-src`,
  `script-src` (nonces, hashes, `'strict-dynamic'`), `connect-src`,
  `frame-src`/`child-src`, `worker-src`, `img-src`, `media-src`,
  `font-src`, `object-src`, `frame-ancestors`, `base-uri`, `form-action`,
  and `style-src` for script-inserted sheets. Inline handlers and
  `javascript:` need `'unsafe-inline'`; `eval` needs `'unsafe-eval'`. Not
  supported: `script-src-attr`, `style-src-attr`,
  `upgrade-insecure-requests`, `sandbox`, report-only and reporting.
- **SRI** is checked for scripts and `<link>` stylesheets.
- **Address bar** shows the WHATWG-serialised URL: IDN hosts as punycode,
  no `user:password@`.
- **`about:` pages** with settings are reachable only from the browser
  UI, change state only by POST, and cannot be navigated to by web
  content.
- **`file:`** documents are opaque origins; `http(s)` pages cannot
  navigate to or play `file:` URLs.
- **Service workers** need a secure context and stay within the script's
  origin and directory (`Service-Worker-Allowed` is not supported).
- **Camera** permission is per page origin; frames share it only through
  same-origin chains or `allow="camera"`.
- **Extensions** are local code the user installed. Their resources
  resolve under the extension root, storage is per extension, and only
  `web_accessible_resources` are exposed to pages (the MV3 `matches`
  restriction is not applied). Content scripts run in the page's main
  world.
- **Safe browsing** (`src/safebrowsing*.c`): a local host blocklist plus
  Google Safe Browsing v5 hash-prefix lists. It fails open.
- **Downloads** get an origin mark (quarantine xattr, `user.xdg.origin.url`,
  `Zone.Identifier`), and `<a download>` needs a recent user gesture.
- **On disk**, profile directories are `0700` on Unix and files the
  browser writes are `0600`. Private mode keeps service workers and
  extension storage in memory.

## JavaScript and frames

- QuickJS runtime per top-level document, so no JS state survives a
  navigation (except the back/forward cache, up to 4 pages). Memory cap
  `js_memory_cap_mb` (2 GiB default; workers at most 512 MiB), 5 MiB
  stack. The DOM bridge revalidates node handles on every call.
- Each frame gets its own realm **inside the page's runtime**. Cross-origin
  and opaque-sandboxed frames see restricted window proxies (`postMessage`,
  `location` setter, `closed`, `length`, navigation helpers; anything else
  throws `SecurityError`), and `contentDocument` is `null`. Natives
  resolve the calling realm past page-realm polyfill wrappers
  (`ns_js_calling_realm()`), so handlers, promise jobs, `postMessage`
  sources and named frame access run against the caller's own frame. A
  frame whose document was replaced has its old realm treated as opaque.
- `sandbox` is enforced and intersected down nested frames:
  `allow-scripts`, `allow-same-origin` (storage throws, cookies empty),
  `allow-forms`, `allow-modals` and `allow-popups`.
- `postMessage` checks `targetOrigin` against the recipient's origin, and
  delivery is asynchronous.
- `document.cookie` reads and writes the site's libcurl jar under one
  lock. `HttpOnly` cookies stay invisible, and names that collide with
  them are refused. The setter caps input at 4 KiB, enforces `Secure`,
  `__Secure-`/`__Host-`, refuses public-suffix `Domain`s, and treats
  cookies as `SameSite=Lax` (libcurl lacks `SameSite`).
- Internal `data-nd-*` attributes are hidden from script, author CSS
  selectors and `attr()`.

## Parsers

HTML and URLs: lexbor. Images: Wuffs (PNG/APNG, GIF, BMP, JPEG, WebP).
ICO is unwrapped in-tree, SVG is rendered in-engine, and AVIF goes to
libavif (`-Davif=disabled` removes it); there is no other image decoder.
Fonts: FreeType. Charset: uchardet. XML is parsed in-tree (`src/xml.c`),
with entity expansion capped at 1 MiB. CSS and layout recursion are
depth-capped, image and video dimensions are clamped before
multiplication, and blur radii are clamped.

Media is attacker-controlled bytes parsed in-process: in-tree MP4, WebM
and MSE demuxers; MPEG-1/MP2 (pl_mpeg), MP3 (minimp3), in-tree AAC,
Opus/Vorbis; for video, GStreamer by default on Linux, Media Foundation
on Windows, and dav1d/libvpx/OpenH264 otherwise (`NS_VIDEO_DECODER`).
Media is fetched through `net.c` under the page's policy, and decoded
from memory.

## Known gaps

- **No process isolation** between pages, or between a page and the
  browser.
- **Frame isolation is JS-level.** Frames share the page's runtime and
  DOM prototypes. The engine keeps one current URL, so `location` read
  from a page callback during a frame's turn can report the frame's URL,
  and a frame's `performance` entries include the page's.
- **`style-src` is not enforced** for parser-inserted `<link>`,
  `@import`, inline `<style>` and `style=` attributes.
- **Web fonts are registered process-wide** with fontconfig under the
  page's family name, so one site can affect how another renders it.
- **Code loaded before the sandbox.** GStreamer plugins (Linux) and Media
  Foundation DLLs (Windows) load before Landlock/seccomp or the mitigation
  policies are applied.
- **Linux read access is wider than needed:** all of `~/.config`,
  `~/.local/share` and `~/.cache` are readable, other applications'
  profiles included.
- **Windows** has no filesystem sandbox, and the GUI allows dynamic code.
- **The cookie jar is not locked between browser instances** that share a
  profile.
