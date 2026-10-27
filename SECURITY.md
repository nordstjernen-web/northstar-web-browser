# Security policy

Northstar Web Browser is a small independent web browser. Security fixes ship from
`main`; only the latest tagged release is supported.

## Reporting
Report security issues by e-mail to:  andreas.rosdal@hotmail.com

Please include the version (shown in the About Northstar Web Browser dialog),
your OS, a minimal reproducer (URL or self-contained HTML), and your
assessment of the impact.

## Threat model

Northstar Web Browser treats the Internet with the utmost suspicion. The attacker
controls fetched HTML, CSS, JavaScript, images, fonts, WebAssembly and
media. The user, the kernel, and the local filesystem outside the sandbox
allow-list are trusted.

**In scope**

- Memory-safety bugs in our C code: out-of-bounds reads/writes,
  use-after-free, integer overflow, format-string.
- Linux sandbox escapes — both layers: the Landlock filesystem
  allow-list and the seccomp-bpf syscall allow-list.
- macOS Seatbelt write-confinement bypass.
- Windows process-mitigation bypass — the policies set via
  `SetProcessMitigationPolicy` at startup (forced image relocation, strict
  handle checks, extension-point disable, image-load restrictions,
  child-process block, and in headless runs the dynamic-code ban).
- Same-origin, iframe-sandbox, cookie, and HTTP-cache partitioning bypass.
- HSTS, mixed-content, and CSP enforcement bypass, for the parts this
  document says are enforced.
- URL-bar spoofing (IDN homograph, userinfo, scheme confusion, etc.).

**Out of scope**

- Bugs in third-party libraries (libcurl, GTK 4, GLib, lexbor, QuickJS,
  Wuffs, …). Report upstream; we update when fixes ship.
- Features we deliberately don't implement: WebGL, WebGPU, WebRTC,
  EME/DRM, JIT, and "AI" web APIs.
- CPU-level side channels (Spectre-class).
- Attacks that already require local code execution as the same user.
- The gaps listed under *Known gaps* below, which are known and tracked.

## Defenses

This minimalist edition runs **single-process**: the HTML/CSS/JS/layout
engine parses and renders untrusted content in the GTK shell process
itself, on a dedicated engine thread. There is no separate renderer
executable and no per-tab renderer process — every page shares one OS
process and one address space. The audio decoders and SDL2 mixer also run
in that browser process on a worker thread (see *Media*, below).

Because there is no privilege boundary between pages, the layered
defenses below are **hardening and containment for the whole process**,
not an inter-process sandbox around a compromised renderer: they shrink
what a memory-safety bug in the engine can reach (filesystem, syscalls,
network protocols), but a bug in the engine is not confined to a
subordinate process the way it would be in a multi-process browser. A
true per-page/per-origin process sandbox is not part of this edition;
treat process isolation as **absent**, and the defenses here as
defence-in-depth around a single trusted-code / untrusted-data boundary.

A normal GUI launch has one more process: the watchdog supervisor
(`src/watchdog.c`), which starts the browser as a child and restarts it on
a crash or hang. The supervisor loads no page content and initialises no
network or UI, and it is **not** sandboxed — it spawns the browser before
any Landlock, seccomp or mitigation policy is applied, so that the child
applies them to itself. `--no-watchdog` or `NS_NO_WATCHDOG` runs the
browser without it; headless and tooling runs never use it.

### Compile-time hardening (`meson.build`)

PIE, full RELRO, non-executable stack, separate-code segments,
`-fstack-protector-strong`, `-fstack-clash-protection`,
`-fcf-protection=full` (Intel CET / AMD IBT), `_FORTIFY_SOURCE=3`
(`=2` fallback), `-Wformat=2 -Wformat-security`. No JIT is used or
linked — JavaScript runs on the QuickJS interpreter and WebAssembly on
WAMR's fast interpreter — so the engine never needs executable memory it
creates itself. On Linux the kernel enforces that wherever page content
is parsed and run: the engine thread installs a second seccomp filter on
itself, inherited by every thread it starts (media decoding, audio,
workers), that refuses an anonymous or shared executable `mmap` and any
`mprotect`/`pkey_mprotect` adding `PROT_EXEC` with `EACCES`, and denies
`memfd_create`, `memfd_secret` and `remap_file_pages`; download threads
install the same filter, and headless and tooling runs install it for
the whole process. The GUI's main thread is left out because GPU drivers
such as Mesa's llvmpipe compile shaders to machine code there. Headless
runs on Windows refuse dynamic code too (see *Windows process
mitigations*); the Windows and macOS GUIs do not. A private file-backed
executable mapping is still allowed, since shared libraries are loaded
that way, so code written to a file in a writable directory can still be
mapped executable; and because the filter is per thread, it is a
hardening measure rather than a boundary between threads of one process.

On glibc, `mallopt(M_PERTURB)` fills freed and freshly allocated heap
memory with a pattern (`src/security.c`), in the GUI and in headless runs,
so a use-after-free or an uninitialised read sees garbage instead of
stale data; the `harden_allocator` setting (`NS_NO_HARDEN_ALLOC`) turns
it off.

### Privilege drop (`src/security.c`)

- Linux/macOS: refuses to start as root — prints a diagnostic and exits
  with status 77 before any page is loaded.
- Windows: when launched elevated it *drops* Administrator rights by
  relaunching itself with the desktop shell's medium-integrity token
  (`CreateProcessWithTokenW`) and exiting the elevated instance, so the
  browsing session runs unprivileged. It verifies the shell token is not
  itself elevated first, so a fully elevated session cannot loop. When
  de-elevation is impossible it falls back to a plain-language dialog
  offering to quit (default) or run as Administrator anyway. Elevation is
  detected with `CheckTokenMembership` against the Built-in
  Administrators SID.
- `NS_ALLOW_ROOT=1` overrides on every platform — keep the elevated
  process and skip the de-elevation and the prompt.
- Sets `PR_SET_NO_NEW_PRIVS` before installing the seccomp filter, so
  setuid binaries cannot be used to gain privileges after a compromise.

### Linux sandbox

Two confinement layers exist, both default-deny and installed from
`main()` in `src/appmain.c` (via `src/security.c`) before any HTML or
audio is parsed:

| Run mode | Landlock | seccomp-bpf | `PR_SET_NO_NEW_PRIVS` |
|----------|:--------:|:-----------:|:---------------------:|
| **Interactive GUI** (the normal browser) | ✅ | ✅ | ✅ |
| Headless / `--dump` / `--eval` / WPT tooling | ✅ | ✅ | ✅ |
| Watchdog supervisor (GUI launches only) | — | — | — |

Both layers **fail open**: on a kernel without Landlock, or when the
seccomp filter cannot be loaded, the browser logs the failure and runs
unconfined.

- **Landlock (filesystem) — applied in every browser mode.**
  - *Read + execute:* `/usr`, `/usr/local`, `/lib`, `/lib64`, the
    directory the running executable sits in, and a `../lib` or
    `../lib64` beside it.
  - *Read-only:* `/etc`, the CA-certificate and fontconfig caches,
    `/proc`, `/sys`, `/run`, `/dev/urandom`, the X11 and ICE
    socket directories, the user's font and theme directories
    (`~/.fonts`, `~/.fontconfig`, `~/.icons`, `~/.themes`), the whole
    XDG config, data and cache directories (`~/.config`,
    `~/.local/share`, `~/.cache`), the `$XAUTHORITY` file itself, and
    the data directories found near the executable
    (`../share/northstar`, or `data/` in a build tree — where the source
    tree itself, the nearest directory holding a `meson.build` up to
    three levels above the executable, is readable too). `/dev/shm` and
    `/dev/dri` are read-only in headless mode.
  - *Read only:* the per-user runtime directory (which holds the
    Wayland socket), apart from its `dconf` subdirectory, which is
    writable.
  - *Read + write:* the browser's own state under
    `~/.config/northstar`, `~/.local/share/northstar` and
    `~/.cache/northstar`, the download directory (the XDG download
    directory when it lies strictly below `$HOME` after resolving
    symlinks, otherwise `~/Downloads`), `/dev/null`, `/dev/shm` in GUI
    mode, the GPU device
    nodes `/dev/dri` and `/dev/nvidia*` in GUI mode (GTK's OpenGL and
    Vulkan drivers open them to draw the window), `/dev/snd` and any
    `/dev/video0`–`/dev/video63` node that exists. Output files named on
    the command line (`--dump=png:`, `pdf:`, `print:`, `--screenshot=`,
    `--trace=`) are opened before the sandbox starts and written through
    the open file, so no directory is granted for them.

  Landlock governs opening paths, not connecting to UNIX sockets that
  already exist, so the session bus and other per-user sockets in the
  runtime directory stay reachable. Unpacked extensions are loaded from
  `~/.local/share/northstar-extensions`, which is outside the writable
  profile directory.

  The rest of `$HOME` — `~/.ssh`, `~/.aws`, `~/.netrc`, shell history —
  is **not** reachable, also when `$XAUTHORITY` is `~/.Xauthority`. One
  thing widens that: other applications' state kept under `~/.config`,
  `~/.local/share` or `~/.cache` (other browsers' profiles among it) is
  readable. No writable directory is granted Landlock's
  execute right, and `execve` is not in the seccomp allow-list, so a
  dropped file cannot be run as a program. Symbolic-link creation
  (`LANDLOCK_ACCESS_FS_MAKE_SYM`) is handled by the ruleset and granted
  nowhere, and on Landlock ABI 3+ `truncate(2)` is handled and allowed
  only where file writes are; the ABI is probed at startup so the
  ruleset requests only rights the running kernel knows.
- **seccomp-bpf (syscalls) — applied in every browser mode.**
  Default-deny allow-list: the filter is built with
  `SCMP_ACT_ERRNO(EPERM)` as the default action and then permits only the
  266 syscalls the browser needs (`ns_seccomp_allowed_names[]` in
  `src/security.c`); every other syscall returns `EPERM`. `execve` /
  `execveat` are not on the list, so a confined process cannot pivot to
  another interpreter or binary even if Landlock would have allowed
  reading it. `ptrace`, `bpf`, `keyctl`, `mount`, `unshare`,
  `userfaultfd`, the `io_uring_*` family, `perf_event_open`, `kexec_load`,
  and the module syscalls are likewise absent. The allow-list matches
  syscall numbers only, not their arguments: `clone`/`clone3`, `ioctl`,
  `prctl`, `socket`, `mmap` and `mprotect` are allowed with any flags. A
  small filter loaded before it refuses a few argument combinations:
  `ioctl` `TIOCSTI` and `TIOCLINUX` (which could type into the terminal
  the browser was started from), `shmat` with `SHM_EXEC`, `clone` with
  any namespace flag, `unshare` and `setns`, and `kill`/`tgkill` aimed at
  any process but the browser itself; `clone3`, whose flags a filter
  cannot read, returns `ENOSYS` so that the C library falls back to
  `clone`. TSYNC
  propagates the filters to every thread. The allow-list's one argument
  rule allows `seccomp` only with `SECCOMP_SET_MODE_FILTER`, so a thread
  can add a
  filter of its own (the engine thread's executable-memory filter);
  stacked filters can only refuse more, never allow what this one
  refuses.
- **Media / audio.** Northstar Web Browser decodes audio **in-tree** in the browser
  process (`src/audio/audio.c`), not via an external player. The media
  controller (`src/js_media.c`) sends each `<audio>` element's commands
  straight to its page's audio context, which the page owns and destroys
  when it closes. An unmuted `autoplay` element starts only after a user
  gesture on the page. A dedicated worker thread fetches and decodes
  media without blocking GTK; URLs are never handed to a shell or an
  external binary. The worker fetches through `net.c` like any other
  subresource of the page — the page's cookie partition, HSTS, the
  request policy below and the response-size cap all apply — and decodes
  from memory, so nothing is written to disk. The engine has already
  checked the source against `media-src` before the command is sent. The
  mixer decodes MP3 (vendored minimp3), MP2 (vendored
  pl_mpeg), AAC-LC (an in-tree decoder, `src/audio/aac.c`, for MP4
  audio), Opus in MP4/WebM when libopus is present, and, when
  `opusfile`/`vorbisfile` are
  present, Ogg Opus/Vorbis, and outputs through SDL2. On Linux the worker inherits the
  browser's Landlock + seccomp restrictions, but codec memory corruption
  is not isolated from the browser address space. Script-supplied media
  bytes (a `MediaSource` buffer, a `blob:` URL) stay in process: the
  engine resolves them to bytes and queues those to the same worker.
- **Video.** `<video>` decodes MPEG-1 through the vendored pl_mpeg, and
  — when the libraries are present at build time — AV1 (libdav1d), VP9
  (libvpx) and H.264 (OpenH264). The containers are demuxed in-tree: an
  ISO-BMFF/MP4 (including fragmented MP4) demuxer (`src/mp4.c`), a
  WebM/Matroska demuxer (`src/webm.c`) and Media Source Extensions
  (`src/mse.c`, `src/js_mse.c`). All of it is ordinary C parsing
  attacker-controlled bytes in the browser process, with no isolation
  from it. Decoding is streamed with a bounded decode-ahead window
  (`src/video.c`: a few frames and at most 48 MB of decoded pixels);
  MPEG-1 frames are capped at 4096 pixels a side and the shared YUV
  conversion path at 8192 (`NS_YUV_MAX_DIMENSION`). There is no DRM.

`NS_NO_SANDBOX=1` disables both Landlock and seccomp (and the macOS
profile); `NS_NO_SECCOMP=1` disables seccomp alone. These and the other
boolean switches count as unset when empty or `0`, `false`, `no` or
`off`. Don't use them in normal operation. Other environment variables
that change security-relevant behaviour, all documented in
`docs/building.md`: `NS_HTTP_PROXY`/`NS_HTTPS_PROXY`/`NS_NO_PROXY` and
`NS_DOH_URL` (where traffic and DNS go), `NS_USER_AGENT`, `NS_PRIVATE`,
`NS_EXTENSIONS_DIR` (extra unpacked-extension directories),
`NS_SAFEBROWSING_LIST` (an extra blocklist, added to the bundled one),
`NS_LOG_FILE` and `NS_FRAME_DUMP` (write logs / rendered frames to disk).
`NS_EARLY_JS_FILE` and `NS_CAMERA_ALLOW` are test hooks honoured only
by headless/tooling runs.

### macOS sandbox

The macOS build applies a Seatbelt profile with `sandbox_init(3)`. It
allows reads and system interaction by default but denies filesystem
writes, then permits writes to the temporary directories, `/dev`, the
runtime directory, Northstar Web Browser's config/data/cache directories and
Downloads; files named on the command line for a headless dump are opened
before the profile is applied. This is write confinement,
not the Linux profile's read allow-list or syscall filter; all engine and
audio code still shares the browser process. Set `NS_NO_SANDBOX=1` only
for debugging.

### Windows process mitigations

Windows has no direct Landlock or seccomp-bpf equivalent that a
user-space GTK process can apply to itself. Instead the browser hardens
itself at startup via `SetProcessMitigationPolicy`, called from
`ns_security_win32_mitigations_init` in `src/security.c` before any page
is loaded. Each call is best-effort: a policy the running Windows does not
support returns `FALSE` and is skipped. The policies are named by their
`PROCESS_MITIGATION_POLICY` constants. The browser and headless/tooling
modes apply:

- **ASLR** (`ProcessASLRPolicy`, `EnableForceRelocateImages`) — relocate
  every image that carries relocations, even one built without
  `/DYNAMICBASE`, so no DLL loads at a predictable address. Stripped
  images are still allowed, since refusing them could stop a system or
  third-party DLL loading; bottom-up and high-entropy randomisation come
  from the executable's own PE header.
- **StrictHandleCheck** (`ProcessStrictHandleCheckPolicy`, flags
  `0x03`) — raise an exception on invalid handle use and lock the
  setting permanently. Catches double-close, UAF-of-handle bugs.
- **DisableExtensionPoints**
  (`ProcessExtensionPointDisablePolicy`, flags `0x01`) — block
  `AppInit_DLLs`, WinSock Layered Service Providers, Image File
  Execution Options debuggers, and a few other legacy injection
  vectors that load DLLs into every process on the box.
- **ImageLoad restrictions** (`ProcessImageLoadPolicy`, flags
  `0x07`) — `NoRemoteImages` (no DLL loads from UNC/mapped
  network drives), `NoLowMandatoryLabelImages` (no DLL loads from
  Low-IL filesystem locations), `PreferSystem32Images` (resolve
  ambiguous DLL names against system32 first). Mitigates DLL
  planting and search-order hijacks.
- **ChildProcess block** (`ProcessChildProcessPolicy`, flags
  `0x01`) — no `CreateProcess` from this process, so a compromised
  process cannot directly pivot to `cmd.exe` / `powershell` / another
  binary. The watchdog launches the browser before this policy is applied;
  the browser process itself has no legitimate child-process requirement.

Headless and tooling runs also apply **DisableDynamicCode**
(`ProcessDynamicCodePolicy`, `ProhibitDynamicCode`): the kernel refuses
any request for executable memory that was not mapped from an image, so
an exploit cannot allocate a writable-and-executable buffer. The
interactive browser does not, because it loads code it does not control
— GPU drivers that compile shaders at run time, and the shell extensions
and input methods that native file dialogs and text input bring in — and
that code may need executable memory of its own.

There is no per-path filesystem sandbox; Windows AppContainer
would provide one but requires a manifest and code-signing
integration we don't have yet. The closest equivalents — Low
Integrity Level drop and AppContainer — are tracked as future
work.

The whole mitigation suite can be disabled for debugging with
`NS_NO_WIN32_MITIGATIONS=1`. Don't use that in normal operation.

### Network

libcurl drives every page fetch with TLS verification enabled
(`CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`), `http`,
`https` and `ftp` as the only protocols, redirects clamped to HTTPS once
the initial scheme is HTTPS (and to FTP once it is FTP), at most ten
redirects, an explicit response-size cap, and `CURLOPT_NOSIGNAL`. HSTS
state is loaded and persisted via `CURLOPT_HSTS`; Alt-Svc is honoured.
Top-level navigations try HTTPS first (`https_first`, on by default).
The `tls_allow_insecure_override` setting, off by default, lets the user
proceed past a certificate error, never for an HSTS host, and only for
the top-level navigation itself, which the address bar then marks as not
secure; a subresource, frame, `fetch()`, worker or media request that
fails verification is never retried without it.

Every subresource request passes one policy check,
`ns_fetch_policy_check` (`src/fetch_policy.c`), before it is sent and
again at every redirect hop. The request carries its destination
(script, stylesheet, image, font, media, frame, worker, connection) and
the policy of the document that made it: that document's URL and a
snapshot of its Content-Security-Policy. The check applies, in order:

- **Local files.** A `file:` URL is refused unless the requesting
  document is itself a `file:` document.
- **Mixed content.** An `http:` or `ws:` URL requested by an `https:`
  document is refused, except that images and media are upgraded to
  `https:` (with no fallback if the upgrade fails), and a loopback host
  (`localhost`, `127.0.0.1`, `::1`) counts as secure.
- **CSP by destination.** `img-src`, `media-src`, `font-src`,
  `connect-src`, `worker-src`, `frame-src` and `object-src`, each with
  its CSP fallback chain, are checked against the (possibly upgraded)
  URL, as is `style-src` for stylesheets that script inserts. Scripts are
  first checked where the element is, because a nonce, a hash or
  `'strict-dynamic'` decides them; a script admitted by its URL is then
  also held to `script-src` at every redirect hop.

Requests that share an in-flight fetch or a preloaded response have the
final URL checked against their own policy when the response arrives.
Requests made by a frame's scripts carry the frame's own policy, not the
embedding page's. WebSocket, EventSource, `sendBeacon`, workers and
`importScripts()`, media the audio engine fetches, and the speculative
preload scanner (which reads the response header and `<meta>` policies
before the page's script runs) use the same check. Two exceptions: the
stylesheets the renderer loads for `<link>` and `@import` get the
`file:` and mixed-content checks but not `style-src` (see *Known gaps*),
and an extension's web-accessible resources are exempt from
`connect-src`.

### Origin isolation

- Cookies and the HTTP cache are partitioned per top-level **site**
  (scheme + registrable domain + port), where the registrable domain
  comes from the Public Suffix List via libpsl. All subdomains within
  the same registrable domain share one cookie jar and one cache
  partition; everything else is isolated. Third-party cookies are
  blocked on network requests by default (`cookie_policy`). A request
  made inside a frame is keyed by the *page's* site, not the frame's
  (`ns_fetch_policy_top_url`), so a cross-site frame gets neither its
  own first-party cookies nor its first-party cache for the resources
  it loads; the frame's URL still decides `Origin`, `Referer`,
  `Sec-Fetch-Site` and CORS. A
  subresource keeps its initiating document through every redirect hop,
  so a 302 cannot move it into the target's first-party partition or
  relabel it as a navigation in the `Sec-Fetch-*` headers.
- `about:settings`, `about:config` and `about:history` are served only
  to navigations and to requests made by those same pages (no other
  about: URL counts as a browser page), and a request with no document
  URL counts as a navigation only when it loads a document; the settings
  endpoints accept changes only by POST, validate the URLs they store,
  and web content cannot navigate to any about: page other than
  `about:blank` and the start page.
- A `file:` document may embed local files as images, scripts,
  stylesheets and frames, but every `file:` document is an opaque
  origin: `fetch()` and `XMLHttpRequest` receive only an opaque response
  for a `file:` URL, a `file:` frame is cross-origin to its embedder, a
  `file:` image drawn into a canvas taints it, directory listings are
  synthesized only for navigations, and an `http(s)` page cannot navigate
  to, or play media from, a `file:` URL.
- CSP (`src/csp.c`) is parsed from headers and `<meta>` and enforced for
  `default-src` (as the fallback), `script-src` (inline and external,
  with nonces, hashes and `'strict-dynamic'`), `connect-src`,
  `frame-src`/`child-src`, `worker-src`, `img-src`, `media-src`,
  `font-src`, `frame-ancestors`, `object-src`, `base-uri` and
  `form-action`, including on redirects; `script-src-elem` and
  `style-src-elem` stand in for `script-src` and `style-src` when those
  are absent. Inline event handlers and `javascript:` URLs need
  `'unsafe-inline'`, and `eval()`, `Function()` and string timers need
  `'unsafe-eval'`, each judged by the policy of the document the code
  belongs to. `style-src` is enforced only for stylesheets that script
  inserts: parser-inserted `<link>` and `@import` loads, inline
  `<style>` and `style=` attributes are not checked (it only withholds a
  `<link>` element's `load` event). Host
  source expressions match scheme, host (with `*.` wildcard), port
  (defaulting to the scheme's default), and path (left-anchored if the
  source ends in `/`, exact otherwise). `*` follows CSP3 semantics — it
  matches network schemes only, never `data:`, `blob:`, `filesystem:`,
  or `javascript:` — and so does `'self'`, so `data:` and `blob:` scripts,
  frames and workers need their scheme listed. `srcdoc`, `about:blank`
  and `blob:` frames get the embedder's policy plus their own `<meta>`
  policies. An import map is honoured only in the page's own document and
  only as an inline script its policy allows. Service workers run under
  the policy of the document that registered them.
- Subresource Integrity (`integrity="sha256-…"` / `sha384-` / `sha512-`)
  is verified against the response body before a script runs.
  Stylesheets' `integrity` attributes are not checked.
- The address bar shows the URL in its WHATWG-serialised form, in which
  a non-ASCII host always appears as its punycode (`xn--…`) encoding, and
  drops any `user:password@` part of the authority. A look-alike
  internationalised domain therefore never displays as the Latin name it
  imitates.
- Service workers can only be registered from a secure context (https,
  or http on loopback), and registrations and interception are restricted
  to the script's origin, with a scope under the script's directory; a
  registration can only be removed by its own origin. The
  `Service-Worker-Allowed` header, which would widen that, is not
  supported.
- Camera access is asked for per page origin. A frame shares the page's
  decision only when every frame between it and the page is same-origin
  with the page or delegates the camera with `allow="camera"`; sandboxed
  frames never do. Until the camera is granted, `enumerateDevices()`
  reports at most one camera with no id and no label.
- WebExtensions are explicitly installed local code, not capabilities
  granted to web pages. Packaged-resource paths are resolved beneath the
  extension root before loading (with symlinks resolved on Unix; on
  Windows the check is lexical), and extension storage is separated by
  extension identity. Web pages can load only the files an extension
  lists in `web_accessible_resources` (the MV3 `matches` restriction is
  not applied). Content scripts run in the page's main world — there are
  no isolated worlds — so a page that has already run can tamper with the
  JavaScript environment a `document_idle` content script sees; the
  native storage helpers are handed to the content-script shim as an
  argument and never appear on the page's global object. Each extension's
  content scripts get their own shim, whose helpers are bound to that
  extension's id, so one extension's scripts cannot reach another's
  storage.
- Safe browsing (`src/safebrowsing.c`) checks a top-level navigation's
  host against a local SHA-256 blocklist (the bundled
  `safebrowsing.list`, plus a user list in the config directory or
  `NS_SAFEBROWSING_LIST`) and shows an interstitial. The bundled list
  only holds a few test entries and nothing updates it, so in practice it
  protects only against hosts a user or distributor lists themselves. The
  interstitial's continue link is honoured only from the interstitial
  itself, and only to continue to the blocked http(s) page.
- Downloads are marked with their origin — the `com.apple.quarantine`
  attribute on macOS, `user.xdg.origin.url` on Linux, and a
  `Zone.Identifier` stream on Windows — and an `<a download>` link starts
  one only after a recent user gesture.

### On-disk state

Config, cookies, cache, HSTS, Alt-Svc, bookmarks, history,
service-worker registrations, IndexedDB, the bytecode cache and
WebExtension local storage live under the application data directories,
which are created `0700` on Unix (a directory that already exists keeps
its mode). Most files the browser writes itself are `0600`; files
created by libcurl (cookie jars, HSTS, Alt-Svc), SQLite (IndexedDB) or
GLib key files (service-worker registrations) get the process umask and
are protected by the directory mode. On Windows the HTTP cache directory
gets an explicit owner-only ACL; the other files inherit the user
profile's default ACL. Service-worker registrations are keyed by origin
and scope and are not persisted in private mode. Extension storage is
keyed by extension identity and stays memory-only in private mode. The
HTTP cache is keyed on `SHA-256(URL || partition)`, so cache filenames
never embed attacker-controlled bytes and no path-traversal is possible.

### Parsers

- HTML is parsed exclusively by [lexbor](https://github.com/lexbor/lexbor);
  there is no hand-rolled HTML tokenizer. XML and XHTML go through the
  engine's own `src/xml.c`, which caps internal-entity expansion at 1 MB
  per document.
- URL parsing routes through lexbor's WHATWG URL module.
- PNG/APNG, GIF, BMP, JPEG and WebP bytes are decoded by
  [Wuffs](https://github.com/google/wuffs) (memory-safe,
  transpiled-to-C). ICO is unwrapped in `image_ico.c` and handed to
  Wuffs; SVG is rendered in-engine; AVIF goes to libavif when the build
  has it, which is the one image path that is neither memory-safe nor
  in-tree — `-Davif=disabled` removes it. Nothing else decodes images:
  there is no GDK-Pixbuf fallback and no plugin-loaded decoder, so the
  set of parsers exposed to untrusted bytes is fixed at build time.
- Web fonts, WOFF included, are parsed by FreeType; the engine then
  re-serialises a WOFF font's tables as a plain SFNT file (`src/font.c`).
- WebAssembly modules are parsed and run by the vendored WAMR fast
  interpreter (`src/wamr/`), which is ordinary C parsing untrusted bytes.
- Charset sniffing is delegated to uchardet, not hand-rolled.
- The engine's own parsers bound attacker-controlled nesting and sizes.
  The recursive CSS parsers — selectors, `@supports`, `@media` queries,
  `var()` fallbacks, and `color-mix()` — all carry depth caps, the
  background-layer list is torn down iteratively, layout's box-tree
  walkers stop at a fixed depth, and SVG rendering stops at 256 levels,
  so a pathologically nested stylesheet or DOM cannot exhaust the stack.
  Decoded image and video dimensions are clamped before any
  `width × height` multiplication, so a crafted dimension cannot
  integer-overflow a bounds check or allocation. `filter: blur()`
  likewise clamps its radius (to 512 px, 256 for box shadows) before
  building the convolution window.

### JavaScript

JavaScript runs in [QuickJS](https://github.com/quickjs-ng/quickjs), an
interpreter — no JIT, no machine-code generation. The DOM/JS bridge
invalidates opaque pointers on node free and re-validates on every
call, so DOM mutation cannot dangle a JS-held handle. A page's runtime
is created with a memory limit (`js_memory_cap_mb`, 2 GiB by default)
and a 5 MiB stack limit; a worker's runtime gets 256 MiB, and at most
512 MiB.

All pages run in **one OS process** (single-process edition) — there is
no per-page process or address-space isolation, so this is a
JavaScript-state boundary, not a memory boundary. Every top-level
navigation builds a new document with a fresh QuickJS runtime, whatever
the origin, so attacker-controlled globals (`window.foo = secret;`),
prototype pollution, leftover module state, and any other in-memory JS
residue from a previous page cannot reach the next page's scripts. The
page being left is freed, or suspended in the back/forward cache (up to
four pages) with its runtime intact. Because
everything shares one process, a memory-safety bug in the engine is
**not** contained between pages the way it would be with separate
renderer processes; the separate runtimes defend against JS-level state
leakage, not against native memory corruption.

Iframes are rendered. An `<iframe src>` is fetched through the same
network pipeline as any other resource (TLS verification, redirect
clamp, response-size cap, CSP `frame-src`); `srcdoc` is parsed inline.
The content document is parsed by lexbor and laid out in place.

A loaded frame gets its own JavaScript realm (a `JSContext`), but
**within the parent page's single QuickJS runtime** — there is no
separate runtime or OS process per frame, and the frame shares the
parent's DOM class prototypes. The frame sees its own `window`,
`document`, `location` and `history`. A same-origin frame's `parent` and
`top` are the embedding page's real window, as the web platform
requires. A frame whose URL is cross-origin, or one sandboxed without
`allow-same-origin`, instead gets a restricted window proxy for `parent`
and `top` — `postMessage`, the `location` setter, `closed`, `length`,
`window`/`self`/`frames`/`parent`/`top` and `close`/`focus`/`blur`;
anything else throws `SecurityError` — and the embedding page sees
`contentDocument` as `null` and a restricted `contentWindow` in the
other direction. A frame's origin is that of the URL it finally loads,
after redirects.

Because every frame realm lives in the parent's runtime, the frame's
global object is assembled from the parent's: it receives copies of the
parent window's properties. For a cross-origin frame that copy is
limited to the browser's own globals, captured before the page's scripts
run, so the page's variables never reach it, and the parent-bound
`cookieStore`, `caches`, `getSelection`, `opener`, `frameElement`,
`name`, `origin` and `navigation` are replaced or left out. Nodes and
ranges a frame constructs (`new Text()`, `new Comment()`, `new Range()`,
custom elements) belong to the frame's document, and each document has
its own custom element registry, so a definition made in one document
never upgrades elements of another. Documents without a browsing context
(`DOMParser`, `createHTMLDocument`, `cloneNode`) are cookie-averse.

This is a JavaScript-level boundary for ordinary
content, **not** a hard security boundary: a memory-safety bug or a
Proxy escape in one frame is not contained from the rest of the page.
Treat frame isolation as defence-in-depth, not as an origin sandbox.

The `sandbox` attribute is parsed and enforced, with nested frames
inheriting the intersection of their ancestors' sandboxes:

- No `allow-scripts` blocks the frame's scripts from running at all.
- No `allow-same-origin` makes the frame opaque-origin: `localStorage`,
  `sessionStorage` and `indexedDB` throw `SecurityError`,
  `document.cookie` reads empty and ignores writes, and the frame and its
  embedder reach each other only through the restricted window proxies
  above.
- No `allow-forms` blocks form submission; no `allow-modals` neutralises
  `alert`/`confirm`/`prompt`/`print`; no `allow-popups` makes
  `window.open` return `null`.

A plain `<iframe>` with no `sandbox` attribute runs its scripts.

Cross-document `postMessage` is delivered in both directions between a
frame and its parent. `targetOrigin` is parsed to scheme, host and port
and compared for equality with the recipient window's origin (`/` means
the sender's own origin and `*` matches everything); the message is
dropped on a mismatch. A message sent through a restricted window proxy
carries the sender's origin and window proxy as the event's `origin` and
`source`. Delivery is asynchronous, through the job queue. What this is
not is a memory boundary: sender and recipient share one QuickJS runtime,
so `postMessage` is the ordinary channel between frames, not a
containment mechanism.

### Cookies and the `document.cookie` surface

Network cookies live in libcurl's per-site cookie jar on disk. They
are parsed, scoped, and re-sent by libcurl, honouring `HttpOnly`,
`Secure`, `Path`, `Domain`, and expiry. libcurl does not implement
`SameSite`; the browser treats every cookie as `SameSite=Lax`, so a
cross-site initiator only gets cookies sent on safe (GET/HEAD) top-level
navigations, and third-party requests get none under the default
`cookie_policy`. Requests from script follow their credentials mode:
`fetch()` sends cookies cross-origin only with `credentials: 'include'`
(and `XMLHttpRequest` only with `withCredentials`), and such a response
is readable only with the exact origin in `Access-Control-Allow-Origin`
and `Access-Control-Allow-Credentials: true`. Documents with an opaque
origin (sandboxed frames, `about:blank`, `file:`) send `Origin: null`.
At navigation the
non-`HttpOnly` cookies for the document's origin are read back out of
the jar (`ns_net_cookies_for_js`) and seeded into `document.cookie`,
and the `document.cookie` setter writes back into that same per-site
jar (`ns_net_cookie_store_from_js`) — so a cookie set from JS is sent
on the next request, and a cookie set over the network is visible to a
later `document.cookie` read. `HttpOnly` cookies are written by libcurl
with a `#HttpOnly_` line prefix that the JS read path skips, so they
stay invisible to script. Script writes and network transfers go through
one cookie store per site, guarded by a lock; only that store writes the
jar file.

The `document.cookie` setter:

- Caps input length at 4 KiB.
- Requires a non-empty `name`.
- Maintains an in-memory mirror for synchronous read-back, then
  persists to the network jar.
- Parses attributes after the first `;`. `Max-Age` (seconds) and
  `Expires` (HTTP-date, via `curl_getdate`) set the jar expiry;
  `Max-Age<=0` or a past `Expires` deletes the named cookie. `Secure`
  is rejected outright from non-HTTPS origins. `Domain` is range-checked
  against the document host before it widens scope, and a public-suffix
  `Domain` is refused; absent, the cookie is stored host-only. `Path`
  defaults to the RFC 6265 default-path of the document URL.
- Enforces the `__Secure-` and `__Host-` name-prefix rules.
- Refuses a name that collides with an `HttpOnly` cookie for the
  document host in the site's jar, so script cannot add a second value
  beside a server session cookie.

## Known gaps

- **No process isolation between pages — single-process edition.** The
  engine parses and renders all untrusted content in the one browser
  process; there is no per-tab, per-page, or per-origin renderer process.
  A memory-safety bug in the engine is therefore not contained to a
  subordinate process — the origin/cookie/CSP/cache boundaries are
  enforced in-process by the engine's own logic, and the sandbox
  (Landlock and seccomp) limits what the whole process can reach, but
  neither is a substitute for the address-space isolation a
  multi-process browser gives you. A per-page process sandbox is the
  largest single hardening this edition does not have.
- **Iframe isolation is JS-level, not a runtime or process boundary.**
  A loaded frame shares the parent page's QuickJS runtime and DOM
  prototypes; its restricted window proxies are a JavaScript boundary,
  not a true cross-origin sandbox. A per-origin/per-frame runtime would
  close this and is tracked as future work; until then, do not rely on
  a cross-origin frame being contained from the embedding origin. Known
  seams of the shared runtime: the engine keeps one "current URL", so
  while a frame's script is running a parent callback that runs in the
  same turn sees the frame's URL as its own `location`, and a frame's
  `performance` entries include the embedding page's resource timings.
- **`document.cookie` in a frame uses the frame's own site.** A
  cross-site frame reads and writes its site's first-party jar through
  `document.cookie`, so the third-party blocking that applies to network
  requests does not apply to script, and the `cookie_policy` setting is
  not consulted for script writes.
- **`style-src` for parser-inserted stylesheets and stylesheet SRI are
  not enforced**, as described under *Origin isolation*.
- **Web fonts are registered with fontconfig for the whole process**
  under the family name a page gives them, so one site's `@font-face`
  can change how another tab renders that family.
- **Internal `data-nd-*` attributes are visible to CSS selectors.** Page
  script cannot write them, but a selector such as `[data-nd-frame-opaque]`
  can observe some of the engine's bookkeeping about the page's own
  elements.
- **In-process codecs.** The MP4 and WebM demuxers, MSE, MPEG-1 video,
  AV1/VP9/H.264 (when built), MP2/MP3/AAC/Opus/Vorbis audio, AVIF (when
  built) and WebAssembly are ordinary C parsing attacker-controlled bytes
  in the browser process (see *Media*). They are bounded, but nothing
  stands between them and the rest of the process.
- **Linux read access is wider than the browser needs.** The whole XDG
  config, data and cache directories are readable.
- **Windows: no per-path filesystem sandbox, and no dynamic-code ban in
  the GUI.** The mitigation suite restricts the *process* (no remote DLL
  loads, no child processes, etc.) but does not allow-list the files the
  process can read or write the way Landlock does on Linux, and the
  interactive browser leaves executable memory allowed for GPU drivers
  and shell extensions (see *Windows process mitigations*). AppContainer
  or a Low-Integrity-Level drop would close the first; both require
  additional integration work (manifest / capability declarations /
  re-routed config paths) and are tracked as future work.
- **The cookie jar is not locked between browser instances.** Within one
  process, script and network cookie writes are serialised; two
  Northstar Web Browser processes sharing a profile can still lose each other's
  writes.
