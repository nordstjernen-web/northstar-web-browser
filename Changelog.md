Changelog:
=========
Significant changes in each release:

1.0.19:
=======
* Input: a mouse drag ends where the button is released. `pointerup`
  and `mouseup` went to the element under the press point with the
  press coordinates, `pointermove`/`mousemove` reported `buttons: 0`
  while the button was held, and `click` went to the pressed element
  even when the release was elsewhere. chess.com reads the drop square
  from `pointerup`, so dragging a piece put it back where it started.
  The release now targets the element under the pointer, `click` goes
  to the nearest common ancestor of the press and release targets, and
  a cancelled `mousedown` no longer starts a text selection while
  dragging.
* Scripts: scripts in content added to the document while a script was
  being fetched now run. Every external script fetch runs a nested event
  loop, and anything inserted during it, including the streamed rest of
  the page, was skipped for good. On chess.com, the guest page's
  `pubsub-connection` module at the end of `<body>` never ran, so
  `PubSub.client` was missing, the seek failed with "PubSub.client
  script timeout after 60000ms", and guests were never paired with an
  opponent.
* Modules: a relative `import()` from code without a script URL (eval,
  `javascript:` URLs, event-handler attributes) resolves against the
  document base URL instead of throwing "module specifier must be
  absolute http/https URL".
* Local files: a `file:` URL the Linux sandbox will not open now gets an
  error page that says so. The page said "this account is not allowed to
  read it" even when the account could, because Landlock only lets the
  browser read a few folders (`~/Downloads`, its own profile, system
  folders such as `/usr`), so `file:///` or a page saved in `~/Documents`
  looked like a file-permission problem (#125). When the file's own
  permissions would allow reading it, the page now names the sandbox,
  explains why it keeps the rest of the home directory closed and points
  to the download directory, where local pages can be opened.
* JavaScript: `Function.prototype.caller` returns the calling function
  for sloppy-mode functions, as in V8 and SpiderMonkey, and `null` when
  the caller is top-level code, a builtin or strict. It always returned
  `undefined`, which freecivweb.com's anti-cheat check treats as a
  forged packet: singleplayer games stopped sending anything after
  "Server refused unauthorized packet.", and multiplayer would have
  disbanded the player's units.
* Web Storage: `sessionStorage` lives for the window, per origin, as the
  standard requires. Every navigation used to start with an empty
  store, so a sign-in that saves its OIDC state on one origin, visits
  another and comes back (BankID's app.bankid.no callback) found its
  state gone and rejected the login.
* Networking: response bodies are decoded by the browser instead of
  libcurl (gzip including multi-member streams, zlib-wrapped and raw
  deflate, Brotli, zstd and stacked codings). A `Content-Encoding` value
  the browser does not know now passes the body through unchanged, as
  other browsers do; libcurl failed the whole request with "Unrecognized
  content encoding type", which broke BankID sign-in because its
  callback server sends `Content-Encoding: ISO-8859-1`. Decoded output
  is held to the response budget and `Accept-Encoding` advertises only
  what can be decoded; libbrotlidec and libzstd are optional
  dependencies.
* DOM: shadow roots and their contents no longer show up in light-DOM
  walks. `document.querySelectorAll('*')`, `getElementsByTagName('*')`,
  NodeIterator and TreeWalker returned the shadow root (a node without a
  `tagName`) and the elements inside it. Cloudflare Turnstile walks the
  document this way and failed with "Cannot read properties of undefined
  (reading 'toLowerCase')", which broke chess.com's guest login.
* Messaging: a message a page posts to a cross-origin frame keeps its
  `event.source`. It arrived as `null`, so a frame that replied with
  `e.source.postMessage()` threw, and `e.source === parent` was false.
  Turnstile's challenge frame answers the widget's heartbeat that way.

1.0.18:
=======
* Browser pages: Settings, History and Page Source open again from the
  menu, the keyboard shortcuts and the address bar. Every load passed the
  page on screen as its initiator, so a load started from the browser UI
  looked like web content asking for the page and was refused with
  "about: pages are not available to web content" (or "view-source: is
  not available here"). A user-activated load now carries the current
  page as initiator only when it is the navigation that page asked for.
  The History link on the start page works too.
* Web compatibility (chess.com): a script that walks `window.frames`
  recursively no longer hangs the page. A same-document iframe's window
  reported the top window's `length` and indexed frames, so every iframe
  looked like it contained the whole frame tree; the page's JavaScript ran
  until the 60-second watchdog stopped it. The iframe window now reports
  its own child frames.
* CSS: nested math functions in values that use `var()` resolve `rem`
  against the root font size instead of 16px, so
  `max(26.4rem, min(...))` and `round(down, ..., .8rem)` give the right
  sizes on pages whose root font size is not 16px. On chess.com the board
  was too small and overlapped the navigation.
* Audio: up to 64 sounds can be loaded at once, up from 16. chess.com
  preloads about 50 sound effects, and those past the 16th failed with
  `too-many-players`.
* Build: `-Djs_engine=quickjs` builds no longer fail when Bellard's `qjs`
  and `qjsc` are installed. The build-time polyfill check found them on
  the `PATH` and passed `qjsc` quickjs-ng options (`-C`, `-P`, `-n`) that
  it rejects. The check now runs only on quickjs-ng builds, with
  quickjs-ng's own tools.
* Packaging: Northstar can be installed from npm as `northstar-browser`
  (`npm/`, documented in `docs/npm.md`). Installing builds the release
  source with meson on Linux, macOS, the BSDs and Windows (in MSYS2), after
  a check that names every missing or too-old build tool and library and
  the command that installs it. The build shows its progress, keeps a log,
  and checks the source tarball against a checksum recorded in the package.
* Packaging: the npm installer sets the executable bits on the installed
  `bin/` and `libexec/` programs, and restores them on an existing install
  when `northstar` starts, so a tree that lost them still launches. The
  `northstar` launcher script is executable in the repository too.

1.0.17:
=======
* Packaging: an Arch Linux package, `northstar-browser`, in
  `packaging/arch/` (PKGBUILD and .SRCINFO, ready for the AUR), documented
  in `docs/Arch.md`. It builds release 1.0.16 in a clean Arch container and
  passes namcap.
* Security: the bundled lexbor HTML tokenizer no longer writes past
  its temporary buffer when a named character reference ends exactly
  at the buffer's end. `&nGt;` and `&nLt;` decode to more bytes than
  their names take, so 16379 bytes of text followed by `&nGt;x` (in
  text, an attribute value or RCDATA) overflowed the heap. The patch
  is `subprojects/packagefiles/lexbor-named-charref-bound.patch`.
  Builds against a system lexbor need the same fix from their
  distribution.
* Security: a module graph in which a dependency failed to load, and
  other modules reached the failed module through an import cycle, no
  longer crashes the page. The bundled quickjs-ng freed the failed module
  while the cycle still pointed at it, and the next script that imported
  into the graph used the freed memory. Such a module now stays
  registered, and every later import of it, static or `import()`,
  rejects with a `TypeError`. The patch is
  `subprojects/packagefiles/quickjs-ng-module-resolve-failed.patch`.
* Security: `select.add()` of an element that contains the select, such
  as the `<option>` around it, throws `HierarchyRequestError` instead of
  linking the tree into a cycle that hung the page. An argument that is
  neither an `<option>` nor an `<optgroup>` throws `TypeError`, and adding
  an option before itself does nothing.
* Security: Safe Browsing now checks every redirect hop of a page
  navigation and the target of a form POST. A link to a clean URL that
  redirected to a listed host used to load the listed page without a
  warning, and a form posted to a listed host sent its data. The
  navigation now stops at the hop that would reach the listed host,
  without contacting it, and shows the warning page.
* Security: HSTS entries now expire at the end of the max-age the site
  sent. The engine read curl's HSTS file without the expiry stamps, so a
  host that once sent `Strict-Transport-Security` was forced to HTTPS,
  and refused the certificate-warning override, for as long as its line
  stayed in the file.
* Security: `<link rel=stylesheet integrity=...>` is now checked. Only
  scripts were verified against their Subresource Integrity hash, so a
  stylesheet pinned by hash on a third-party CDN was applied whatever it
  contained. A sheet that does not match is now skipped, logged as
  "SRI mismatch: stylesheet", and kept out of CSSOM.
* Security: nested blocks whose `min-width` or `max-width` is an
  intrinsic keyword (`min-content`, `max-content`, `fit-content`) no
  longer take exponential time to lay out. Each level measured its
  subtree twice, so 24 nested `min-width: min-content` blocks in a float
  took 10 seconds and a few more levels stalled the page for minutes.
  The measurement is now reused; layout results are unchanged.
* `localStorage` keeps entries whose keys contain `=`, a line break or
  leading or trailing spaces, an empty key, and values with lone
  surrogates. The on-disk key file used storage keys as key-file keys:
  such keys were refused with GLib criticals, and a key with a line
  break corrupted the file, so the next session lost every entry of
  the origin. Keys and values are now stored percent-escaped; existing
  files still load.
* CSS animations run at the display's frame rate again. The window
  capped page animations at 60 frames a second by dropping any frame
  that came less than exactly 1/60 s after the last one, but a 60 Hz
  display's frame times jitter around that value, so about every other
  frame was dropped and an animation ran at around 40 frames a second.
  The cap now allows a quarter frame of slack.
* HTTPS connections no longer re-read the CA bundle each time on
  distributions whose libcurl has a default CA path (Debian, Ubuntu).
  libcurl caches the parsed CA store only when it comes from a CA file
  alone, so every new connection re-parsed all ~150 certificates on its
  handshake's critical path. The browser now clears the CA path next to
  the bundle it sets.
* Security: a page and a frame that load the same stylesheet no longer
  crash the browser. The stylesheet caches re-parse a sheet when the
  frame's viewport changes which of its media queries match, and the
  re-parse freed the copy the page's style pass was still using. A sheet
  the caches drop during a style pass is now freed when the pass ends.
  Linked sheets and `@import`ed sheets were both affected (1.0.14 to
  1.0.16).
* Headless `--act`: `click` and `rightclick` now take a `sel=CSS` point,
  as `docs/building.md` documents and `move` and `down` already did.
  They used to ignore any point that was not `X,Y`.

1.0.16:
=======
* Text layout moved from the ns-pango subproject to web-text-lib,
  vendored in `src/web-text-lib/` from Nordstjernen Sol and built with
  `subdir()`, so `meson setup` no longer fetches it and offline and
  distro builds get it from the source tarball. The engine reaches it
  through `src/ns_pango.h`. Three functions Sol had trimmed and Northstar
  calls (word spacing, line spacing, log attributes) are restored from
  upstream. CJK text now sits on the same baseline as the text around it.
* about:start shows the animated N logo and the heading "Nordstjernen
  Browser" above the search box.
* Nordstjernen Luna is renamed Nordstjernen Web Browser in every
  user-visible place, and the repository moves to
  <https://github.com/nordstjernen-web/nordstjernen-web-browser>. The
  Windows installer removes the old Nordstjernen Luna Start-menu folder
  and desktop shortcut on upgrade.
* The project now follows a no-AI policy: contributions must be written
  by people, not generated by AI tools. `CLAUDE.md`, `AGENTS.md` and the
  `.claude/` and `.agents/` assistant configuration are removed, and the
  README states the policy.
* Security: seven crash fixes backported from Nordstjernen's
  memory-safety review, each with a `Security-Fix:` trailer.
  `element.attributes` crashed when page script run while it was built
  (a redefined `Attr` prototype or an `Array.prototype` setter) removed
  the attribute being converted; the custom element upgrade walk read a
  registry slot freed by a nested `customElements.define()`;
  `navigation.entries()` read a freed history entry when an
  `Object.prototype` setter called `history.pushState()`;
  `select.add(option)` left the option registered as an orphan node
  while the select owned it; the `document.body` setter could link a
  node under its own descendant, after which tree walks overflowed the
  stack; and SVG path data with a number after closepath (`M0 0Z1`)
  looped until memory ran out, both in `getTotalLength()` and when the
  path was painted.
* Security: four more crash fixes backported from Nordstjernen.
  `getAttributeNames()` and `querySelectorAll()` (its class, tag and key
  index paths) stored results into the returned array while still
  walking the attribute list or index bucket, so a setter the page
  defined on `Array.prototype` could remove the attribute or call
  `document.open()` and free what the walk was reading; reading an SVG
  length such as `rect.width` let an `Object.prototype` setter free the
  attribute value still in use; and a WebAssembly call with an
  `externref` argument followed by a numeric one whose `valueOf` made a
  nested call could free the externref's box before the call used it.
* Northstar is renamed Nordstjernen Luna: the window title, About
  dialog, `about:` pages, error and warning pages, desktop entry,
  AppStream metadata, installer, packages and documentation use the new
  name. The executable, application ID, settings and profile locations,
  the `User-Agent` and client-hint brand, and the Windows registry keys
  keep the Northstar identifiers, so existing installs, profiles and
  default-browser settings carry over.
* The repository moved to
  <https://github.com/nordstjernen-web/nordstjernen-luna-browser>; the
  README badges, AppStream metadata, Debian packaging and man page link
  there.
* Linux: the browser starts again on NVIDIA's proprietary driver under
  X11. The Landlock sandbox let the GUI read `/dev/dri` but not open the
  render nodes read-write, and gave no access to `/dev/nvidia*` at all,
  so GTK's OpenGL and Vulkan drivers were refused the GPU ("Permission
  denied" on `/dev/dri/renderD128`) and the window died with an X
  `BadDrawable` error; the watchdog restarted it until it gave up. In GUI
  mode the sandbox now lets those device nodes be opened read-write;
  headless runs keep them read-only. Backported from Nordstjernen.
* `open-internet-browser` is a second name for the browser. `meson
  install` links it to `northstar` in the binary directory; the `.deb`
  and `.rpm` packages register it with `update-alternatives`, so
  Northstar installs beside Nordstjernen and Southstar and
  `update-alternatives --config open-internet-browser` picks which one
  the name starts; the Windows installer adds it as an App Paths alias
  and the MSIX package as an app execution alias.
* The animated cartoon splash is gone from `about:start`, which now
  opens on the search row. `data/splash.gif` is no longer installed or
  packaged, and `scripts/gen-splash.py` and `scripts/gen-splash.sh`,
  which drew it, are removed.

1.0.15:
=======
* Browser identity and engine behaviour that bot detection (Google's
  BotGuard among them) compares with the browser the user agent names,
  backported from Nordstjernen:
  - `Sec-CH-UA` and `navigator.userAgentData` report `"Northstar"` instead
    of claiming to be Chromium and Google Chrome; `getHighEntropyValues`
    answers only the hints it is asked for, with the real architecture,
    bitness and version.
  - Every top-level navigation sends `Sec-Fetch-User: ?1`; a navigation
    without it reads as automation.
  - Built-in functions print as `function f() { [native code] }` on one
    line and bound functions as `function () { [native code] }`. Engine
    scripts are compiled without their source text, so the getters,
    methods and helpers the browser writes in JavaScript print as native
    code under their member name instead of showing their source.
  - `Error.prototype.stack` starts with the `Name: message` line and lists
    frames as V8 does: an anonymous function or script top level as
    `at url:line:col`, code run by `eval` with an empty file name.
  - TypeErrors use V8's wording: `Cannot read properties of null (reading
    'x')`, `Cannot set properties of undefined (setting 'x')`,
    `obj.f is not a function`, `X is not a constructor`
    (`quickjs-ng-v8-type-errors.patch`).
  - `Intl`, its constructors' prototypes, `localStorage`/`sessionStorage`
    (`Storage`), `performance.timing` (`PerformanceTiming`) and
    `performance.navigation` (`PerformanceNavigation`) carry their
    `Symbol.toStringTag`; `Function.prototype` loses quickjs-ng's
    non-standard `fileName`, `lineNumber` and `columnNumber`.
  - `SharedArrayBuffer` is not exposed to pages that are not cross-origin
    isolated, as HTML requires; `navigator.pdfViewerEnabled` is `false`,
    matching the empty `navigator.plugins` of an edition without a PDF
    viewer.
  - `localeCompare` and `Intl.Collator` compare letters before case and
    accents, so `'a'.localeCompare('B')` is `-1` on Windows too, where
    GLib fell back to code-point order.
  - Due 0-1 ms timers run only between tasks, never inside a frame load,
    a script or another callback.
  - Node, Document, DocumentFragment and the character-data interfaces
    keep only the members their WebIDL defines, and Element and
    HTMLElement only theirs: a document or text node no longer has
    `click()` or `style`, and a `<div>` no `href`.
* Security: the Landlock sandbox no longer opens the whole home directory
  when `$XAUTHORITY` is `~/.Xauthority`, as LightDM sets it on X11
  sessions (Linux Mint, Xubuntu). The sandbox granted read access to the
  directory holding that file, which was then `$HOME`, so `~/.ssh`,
  `~/.aws` and every other file there were readable from the browser
  process. It now grants read access to the `$XAUTHORITY` file alone.
* Security: the kernel now refuses executable memory wherever page content
  runs. Northstar has no JIT, yet its seccomp filter let any thread map
  memory executable, so a memory-safety bug in the engine, the media
  demuxers or the decoders could make a buffer executable and run it. The
  engine thread installs a second filter on itself, inherited by the
  threads it starts, that refuses an anonymous executable `mmap` and any
  `mprotect`/`pkey_mprotect` adding `PROT_EXEC`; headless and tooling
  runs install it for the whole process, as headless Windows runs already
  refuse dynamic code. The GUI's main thread keeps executable memory for
  GPU drivers that compile shaders (Mesa's llvmpipe). The main filter now
  allows `seccomp(SECCOMP_SET_MODE_FILTER)` so a thread can add its own.
* `document.cookie` follows the cookie store. It was a copy taken when
  the page loaded and replaced only when script wrote it, so a cookie set
  or deleted by a `fetch()` or XHR response never appeared, which broke
  sites that copy a CSRF token from a cookie into a request header. The
  store now counts its writes, and a read takes the cookies again when
  the count has changed. Same fix as Nordstjernen's and Southstar's.
* Security: a local HTML file can no longer read other local files
  through a frame or a canvas. `ns_js_urls_same_origin` compared non-HTTP
  URLs by scheme alone, so it called any two `file:` URLs same-origin: a
  page opened from the downloads folder could embed another program's
  file under `~/.config` in an iframe and read its `contentDocument`, and
  a local image from another folder drawn into a canvas did not taint it.
  Every `file:` URL is now its own origin there too, as `fetch()` and
  `XMLHttpRequest` already treated it. A local page still loads its own
  scripts, style sheets and images. Nordstjernen and Southstar take the
  same fix, where `fetch()` and XHR could read the files directly.
* A source-wide sweep for unused, duplicated and bloated code removes
  about 2,250 lines from the engine without changing what pages see.
  - A link with section garbage collection found 24 functions nothing
    called, and 183 functions were exported through headers but only
    used in their own file (117 of them canvas internals that
    `js_internal.h` still declared from before `js_canvas.c` was split
    out of `js.c`). The dead ones are gone and the rest are static.
    Fields and counters that were written and never read, 27 functions'
    unused parameters, a no-op `ns_idb_db_close()` called 19 times and
    the native `__ndMseRanges` binding no script called went with them.
  - Shared helpers replace copies: one SQLite open/harden/exec helper
    for the cache, history and IndexedDB; one ASCII-whitespace test for
    eleven; one implementation each of `addEventListener` and
    `removeEventListener` for elements, documents and windows; one
    directory-listing page for `file:` and `ftp:`; one Wuffs decoder
    setup for still and animated images; and single copies of the
    iframe location/history shim, the `<progress>`/`<meter>` state
    algorithms, the meta-CSP walk and the recursive delete.
  - The HTML charset prescan used its own outdated copy of the Encoding
    Standard's label table and now uses the full one in `encoding.c`, so
    labels it lacked, such as `unicode-1-1-utf-8`, `ucs-2` and
    `csunicode`, are recognised.
  - Caret placement, word selection and selection extents no longer
    create and destroy a throwaway Cairo surface on every call.
  - The microphone module only held stubs that produced the same
    analyser output as no microphone, and is removed. Granting a page
    the camera no longer opens `/dev/video0` and starts V4L2 streaming,
    since no frame was ever read from it; device enumeration and the
    per-site permission are unchanged. The `camera_enabled` config key,
    written on every grant and never read, is gone.
* A second pass removes about 1,550 more lines, again without changing
  what pages see: every step was compared with the previous build's
  rendering, layout dumps and script output.
  - The 152 comments inside functions, structs and headers that the
    source policy does not allow are gone; each file keeps its one
    header comment.
  - Reflected DOM accessors are table-driven: the anchor and `Location`
    URL parts, boolean content attributes such as `noshade` and
    `defaultChecked`, the console methods windows and workers share, and
    the `SubtleCrypto` methods each go through one function.
  - Code kept in two to eleven copies now exists once: the CSS
    inline-declaration, at-rule descriptor and quote-skipping scanners,
    the shorthand serializers' longhand collection, the inline Pango
    layout setup that layout and paint both built, layout's hit-test
    entry and blockified-child handling, IndexedDB's argument handling,
    the argument conversion of `before`/`after`/`append`/`prepend`/
    `replaceWith`, listener retirement, selector-list parsing and worker
    teardown, among others.

* Security: the media demuxers no longer read past a buffer, hang or
  exhaust memory on hostile files.
  - The progressive MP4 box walk read an 8-byte box header beyond the
    response body when a 206 response stopped short and the next box
    began in the file's last 8 to 15 bytes, where no further range was
    requested. It now gives up there.
  - A 64-bit box size near 2^64 wrapped the walk's file offset back to
    an earlier box, and two such boxes looped forever on the main thread.
  - The sample index of an ordinary MP4 was capped at 2^21 samples only
    after every track had been parsed, so 64 small `trak` boxes could
    each allocate 84 MB first, about 5 GB in all, which aborted the
    browser on 32-bit systems. The cap now applies as each track is
    read, and a 65th track is refused before it is parsed.
  - A fragment's `trun` for a track the movie does not declare walked
    up to 2^18 samples one by one without recording any, so a `moof`
    of millions of such runs held the main thread for hours. Its extent
    is now computed in one step.

* Security: on musl systems such as Alpine, an `<img>` whose SVG nests
  about 250 groups no longer crashes the browser. Images decode on GLib
  worker threads, which get the C library's default thread stack, and
  musl's is 128 KiB, less than the SVG renderer's 256 allowed levels need.
  The Linux binary now asks for 2 MiB default thread stacks, which musl
  honours and glibc, already at 8 MiB, ignores.

* Security: `WebAssembly.Memory`'s `buffer` can no longer be moved out
  with `ArrayBuffer.prototype.transfer()`. The buffer views the
  instance's linear memory, which the engine does not own; a transfer of
  the same length handed that address to a new buffer that nothing
  detached, so after `memory.grow()` remapped the memory, or the instance
  was freed, the page read and wrote freed memory. Both engines now refuse
  to transfer a buffer whose memory they do not own, with the `TypeError`
  other browsers throw (`quickjs-ng-external-buffer-transfer.patch`,
  `quickjs-external-buffer-transfer.patch`), and
  `scripts/check-subprojects.sh` now also fails when a checkout lacks one
  of its wrap's patches, as one downloaded before a patch was added does:
  delete it and download it again.

* Security: `structuredClone()` and `postMessage()` no longer copy an
  `ArrayBuffer` from freed memory. The clone took the buffer's data
  pointer and then read its `resizable` property, which a page can
  redefine to detach the buffer, so the clone held whatever the heap had
  put there since. The pointer is now taken after every getter and the
  constructor have run.
* Security: the native MediaSource hooks no longer use a SourceBuffer
  that page script removed while its arguments were converted. A
  `valueOf` passed to `__ndMseAppend`, `__ndMseRemove` or
  `__ndMseSetDuration` could call `__ndMseRemoveBuffer` between the
  lookup and the append, and `__ndMseRemoveBuffer` converted its id twice,
  so it could free one buffer and unregister another. Every argument is
  now converted before the lookup, and the append result is built without
  running property setters.

* Security: a 2D canvas context no longer draws through a freed Cairo
  context. Changing a canvas's `width` or `height` replaces its Cairo
  surface and context the next time the context is used, and a page
  could do that from inside a call already drawing, through a
  `valueOf` argument or a `globalAlpha` or `globalCompositeOperation`
  getter, so `fillRect`, `drawImage`, `clearRect`, `moveTo` and others
  carried on with the destroyed one. Every `CanvasRenderingContext2D`
  method now keeps a replaced context alive until the outermost canvas
  call returns.
* Security: an `AudioBufferSourceNode` with an infinite `playbackRate`
  no longer indexes its buffer with a NaN position, which C leaves
  undefined and 32-bit builds could turn into an out-of-bounds read.
* Alpine Linux builds install the full dependency set. The musl CI
  build now also compiles AV1 (dav1d), VP9 (libvpx), H.264 (OpenH264)
  and Opus video sound, Thai word breaking and zlib, and the nightly
  Alpine package installs the HarfBuzz, FriBidi, Cairo and FreeType
  headers that ns-pango needs, plus the AV1, VP9 and Opus decoders,
  instead of the system Pango it no longer uses. The musl package's
  README lists the matching runtime packages.
* The Windows release packaging (zip, installer and MSIX) links with lld
  when it builds with clang, since clang's link-time optimisation could
  not load the GNU linker plugin it needs in MSYS2's MINGW64.

1.0.14:
=======
* Security: fixes from a source-wide review on 2026-10-10. Each was
  reachable from an ordinary page.
  - The cascade gathers matches for an element and each of its nine
    pseudo-elements in one pass, ten destinations in all, but the
    per-rule record of which destinations matched and at what
    specificity held only nine. A page whose style sheets used every
    pseudo-element, ending with `::file-selector-button`, wrote past the
    end of that record into the next rule's, or past the end of the heap
    block for the last rule, which AddressSanitizer reported as a heap
    buffer overflow. The record now has a slot for every destination.
  - A value that `attr()` substitutes is tainted, and a tainted value
    inside a URL-taking function makes the declaration invalid, so that
    markup a site lets users author cannot make the browser fetch an
    arbitrary URL. The check for URL functions in the substituted text
    matched only lower-case `url(`, `src(` and `image-set(`, while CSS
    function names are case-insensitive, so `data-bg="URL(http://…)"`
    under `background-image: attr(data-bg type(*))` was fetched. The
    check now recognises every spelling of every URL-taking function.
  - Any page could read another site's responses with that site's
    cookies. `fetch()` took the request's origin, which decides the
    cookies sent and whether the response may be read, from
    `this.location.href`, so `fetch.call({location: {href:
    "https://bank.example/"}}, "https://bank.example/account")` was sent
    with the bank's cookies and resolved with its body. The origin now
    comes from the document that owns the `fetch` function: the page's
    own, or, for a frame, the URL the engine loaded into that frame.
  - `XMLHttpRequest.send()` sent the header lines and method it found in
    the request object's `_headers` and `_method` properties, which page
    script can rewrite after `setRequestHeader()` and `open()` have
    checked them. A page could add `Cookie`, `Host`, `Origin`,
    `Sec-Fetch-*` and the engine's internal `X-ND-Navigate` header,
    which made a redirected cross-site request travel as a navigation
    with the target's cookies, or send a `TRACE` request. `send()` now
    checks every header line and the method again. `X-Requested-With:
    XMLHttpRequest` is added only to same-origin requests, since sites
    read it as proof that a request came from their own script.
  - A cross-origin frame could reach the page that embedded it through a
    frame of its own. Nested frames load after their parent, when the
    engine's current URL is the top page's again, and an `about:blank`
    or `srcdoc` frame took that URL as its origin, so a `srcdoc` frame
    inside a frame from another site was treated as same-origin with the
    top page and could read its DOM. A frame's embedder is now the
    document that contains its element.
* A table caption's and a table cell's `::before` and `::after` are
  built as boxes the way a block's are. A caption turned every
  pseudo-element into inline text, so a floated one took no room (a
  `float: left; width: 40px` `::before` did not push the caption's text
  aside), a `display: block` one with empty content made no block, and
  an absolutely positioned one was never laid out. A cell dropped its
  pseudo-elements altogether, so `td::before { content: attr(data-label) }`
  showed nothing.
* `<link rel="preload" as="style">` (and `as="script"`) fetches its
  resource and fires `load` or `error` when the fetch ends, and a
  `<link>` whose `rel`, `href` or `as` changes is processed again: one
  that becomes `rel="stylesheet"` applies its sheet and fires `load`,
  and a stylesheet whose `href` changes loads the new sheet. The common
  asynchronous CSS pattern `<link rel="preload" as="style"
  onload="this.rel='stylesheet'">` never fired its handler, so its
  sheet was never applied.
* `JSON.parse()` reads decimals correctly when the operating system uses
  a decimal comma. GTK sets every locale category from the environment
  as the window starts, and the JSON parser converts numbers with the C
  library's `strtod()`, which follows `LC_NUMERIC`: under a Norwegian,
  German or French locale `JSON.parse("[1.5, 2.25e1]")` gave `[1, 2]`
  in the browser window (headless runs never set the locale and were
  unaffected). The window now puts `LC_NUMERIC` back to `"C"` after GTK
  starts.
* Constructed style sheets adopted by a shadow root apply again. The
  `adoptedStyleSheets` polyfill scoped each sheet to its host by giving
  the host a `data-nd-host` attribute from script, which `setAttribute()`
  has since refused for engine-internal names, so every adopted sheet
  matched nothing: the WebComponents TodoMVC rendered as unstyled text.
  The engine now tags the host itself, and each sheet is materialized
  once per document and shared by every host that adopts it, so a
  hundred custom elements adopting the same two sheets parse two sheets,
  not two hundred. `adoptedStyleSheets.push()` takes effect, as the
  observable array it is in browsers.
* Performance, measured with Speedometer 3.1's TodoMVC workloads
  (`scripts/speedometer-bench.sh`):
  - The ancestor filter that lets the cascade skip a descendant
    selector without walking up the tree also records each ancestor's
    `[name=value]` attributes, so a selector such as
    `[data-k="a"] .item` is dismissed at once below every element whose
    attribute differs. Shadow-root style sheets are scoped to their host
    that way, so a page with a hundred shadow hosts no longer matches
    every element against every host's rules: restyling the
    WebComponents complex-DOM workload after it adds its items drops
    from 1.8 s to 0.3 s.
  - The key that lets elements with the same matched rules share one
    computed style is written in three copies per declaration instead
    of six appends, into a buffer sized once. Elements under rule sets
    with hundreds of custom properties, as Spectrum's are, built keys
    tens of kilobytes long field by field: the Lit and WebComponents
    workloads run 35-45% faster.
  - Event listeners are indexed by their target. Adding, removing and
    dispatching to a listener used to walk every listener on the page,
    reading each one's `signal.aborted` from script on every
    `addEventListener()`, and the pass that frees removed listeners ran
    after every dispatch step whether or not any had been removed. With a
    hundred list items carrying a few listeners each, that walk was a
    third of the time Vue spends handling a click.
  - A page whose text field shows a value or placeholder longer than its
    `size` laid out the whole document twice on every relayout: once to
    learn the field's width and again with the column count that width
    allows. Layout now starts from the counts the previous layout of the
    document settled on and repeats only when they change, so every
    TodoMVC page, whose "What needs to be done?" placeholder is 22
    characters against the default 20, lays out once per frame.
  - Adding or removing a shadow root's style sheets no longer restyles
    the whole page. Sheets that can only match inside one shadow host
    are kept apart from the page's sheets, left out of the signature
    that decides whether the last cascade's styles can be reused, and
    gathered only for elements inside that host; when one of them
    changes, only its host is restyled. The Lit workload, whose hundred
    `<todo-item>` shadow roots each carry their own `<style>`, restyled
    7,600 elements every time an item was added or removed.
  - A regular-expression search tries a match only at positions holding
    a character the pattern can start with, when every alternative
    begins with a literal (`quickjs-ng-regexp-first-char.patch`). The
    engine used to push a backtracking state at every character of the
    subject: jQuery's `/<script|<style|<link/i` test of a 23 KB list
    took 1.6 ms and now takes 0.14 ms, and the jQuery TodoMVC runs about
    30% faster.
  - `JSON.parse()` converts a number of up to 15 significant digits
    without an exponent directly, as the exact quotient of two doubles,
    and hands anything else to quickjs-ng's own `js_atod()` instead of
    the C library's `strtod()` (a new wrap patch,
    `quickjs-ng-json-number.patch`). MinGW's `strtod()` takes a lock and
    runs gdtoa for every number, a fifth of `JSON.parse()` time in the
    TodoMVC workloads that keep their list as JSON, and it reads digits
    by the current `LC_NUMERIC`. Parsing a hundred 13-digit ids is 4x
    faster; 200,000 random numbers parse bit-identically to `Number()`.
  - A plain CSS time such as `130ms` or `.2s` is read directly instead
    of through the `calc()` evaluator, which copied the text, printed the
    number back out and parsed it again under the C library's locale
    lock. Elements with transitions read their durations and delays on
    every relayout, and Spectrum gives thousands of them one.
* JavaScript passes every test262 test that quickjs-ng's own
  `test262.conf` enables: 0 failures in 82,220 runs, against 49 on
  v0.17.0. quickjs-ng moves from the v0.17.0 release to master
  `9b06171`, the newest code (no newer release exists), which already
  fixes the evaluation order of computed-property and destructuring
  assignments, `Reflect.set` with an undefined receiver and template
  literal substitution. A new wrap patch,
  `quickjs-ng-test262.patch`, fixes the rest:
  - `f() = 1`, `f()++` and `for (f() in o)` in sloppy code throw a
    ReferenceError when they run, as browsers do, instead of failing to
    parse; a tagged template there is still a SyntaxError.
  - `a + [] = 0` and `#x in {} = 0` are SyntaxErrors. A `{…}` or `[…]`
    followed by `=` was taken as a destructuring assignment anywhere,
    even as the right operand of another operator.
  - A computed object-literal key is converted to a property key before
    its value is evaluated.
  - `get` or `set` followed by a generator on the next line, and
    `static` followed by `=` or `;` on the next line, are class fields.
  - Inside `with`, reading and writing a binding asks the object again
    whether it still has the property, and `var` destructuring resolves
    the target before it reads the source property.
  - `"NaN"` is a canonical numeric string, so assigning to it through a
    typed array on the prototype chain defines nothing.
  - A sync iterator wrapped for `for await` or `yield*` is closed when
    the value it yields is a rejected promise, and a `for await` loop
    no longer calls `return()` when `next()` itself throws or rejects.
  - A module that re-exports a namespace it imported, through `export *`
    from two modules, is no longer ambiguous.
  - A module-level `using` declaration can be exported.
  - A module without top-level await that finished as part of an async
    graph is marked done, so a later `import()` of a module that depends
    on it no longer waits forever; an async module rejects its own
    promise before its importers'.
  The interrupt callback now receives the context rather than the
  runtime, the cached bytecode format version moves on, and a wasm
  import that fails without an exception of its own throws a
  `WebAssembly.RuntimeError`, since quickjs-ng now makes `InternalError`
  uncatchable. Bellard's QuickJS stays on its 2026-06-04 release: its
  newer commits are unreleased.
* Google Maps loads its full interface: the place card, sign-in button,
  layer switcher, zoom and location controls, Street View bar, footer and
  scale. Two engine gaps stopped its start-up half way and showed "an
  error occurred":
  - XPath 1.0: `document.evaluate()`, `createExpression()`,
    `createNSResolver()`, `XPathEvaluator`, `XPathExpression` and
    `XPathResult`, with every axis, node test, operator and core function,
    HTML-document name matching, namespace prefixes through a resolver, and
    all ten result types.
  - A dedicated worker's global object is now a `DedicatedWorkerGlobalScope`
    inheriting from `WorkerGlobalScope` and `EventTarget` (a service
    worker's, a `ServiceWorkerGlobalScope`), so `self instanceof
    DedicatedWorkerGlobalScope` holds, as worker scripts check before
    running.
* The mouse cursor follows a page that changes `cursor` from its own
  pointer handlers. The cursor was looked up in the styles of the last
  frame, so a drag that set `cursor: move` on `mousedown` showed it a
  move late, and nothing looked again when the button came up, so the
  page's reset on `mouseup` never showed and dragging the Google Maps map
  left the move cursor behind. Pending style changes are now applied
  before the lookup, and a button release looks the cursor up again.
* An absolutely positioned box resolves percentages in its `max-width`,
  `min-width`, margins and padding against its containing block. They
  were resolved against the box's own shrink-to-fit width, so
  `max-width: calc(100% - 740px)` went negative and was dropped, and
  `max-width: 42%` took 42% of the content's width. Google Maps' row of
  category chips beside the search box showed one clipped chip instead
  of five.
* A `gap`, `margin-block`, `margin-inline`, `padding-block`,
  `padding-inline`, `inset-block`, `inset-inline`,
  `border-block-*`/`border-inline-*` width, style or color, `overflow`
  or `inset` declaration with too many values, or with one invalid
  value, is ignored as a whole, as CSS Syntax requires, instead of
  applying the values that parsed. `gap: 1px 2px 3px` used to set
  `row-gap` to 1px.
* Headless `--act=` gains pointer actions: `move X,Y` hovers (dispatching
  pointerover/mouseover, pointermove/mousemove and setting `:hover`),
  and `down X,Y` / `up` press and release, with `move`s between them for
  a drag. A point may be given as `sel=CSS [dx=N] [dy=N]`, resolved to
  the matching element's centre when the action runs, so scripted
  interactions survive layout changes.
* Media no longer starts by itself: a new `autoplay_enabled` setting,
  off by default, makes `play()` reject with `NotAllowedError` unless the
  user clicked or pressed a key in the last five seconds, and ignores the
  `autoplay` attribute, as browsers' "block audio and video" option
  does. Video then starts when the user asks, after the page has
  settled, instead of competing with its layout.
* getVideoPlaybackQuality() counts the frames a Media Source video
  showed and the ones it decoded too late to show, and
  MediaCapabilities.decodingInfo() reports a video configuration as smooth
  only when its pixel rate fits what the decoder has sustained (1080p at
  30 frames a second before it has measured anything). It also answers
  for Media Source video, which it always called unsupported. Pages such
  as YouTube lower the quality they stream when frames are dropped; they
  were told none ever were.
* `--trace` also records the media pipeline: video decoding and feeding,
  the picture picked for each view frame, audio device fills and
  feeding, and MSE appends.
* Video decoding runs on a thread of its own. An MSE video stream hands
  coded frames (references to the buffered samples) to a decode thread
  that decodes and converts to BGRA up to four frames ahead; the
  animation tick only picks the decoded frame for the playback clock and
  keeps the queue topped up, restarting it from a keyframe on seeks and
  after `remove()`. Main-thread time per video tick drops from 9-44 ms
  (1080p H.264, AV1) to about 0.3 ms.
* libdav1d and libvpx decode with up to four threads (one per core),
  still one picture out per sample in. With the decode thread, 1080p AV1
  plays all 150 of 150 frames of a 5 s test stream where it showed 95.
* The view composites playing Media Source Extensions videos (and MP4 or
  WebM files in `<video src>`) itself. The page frame keeps a transparent
  hole where the picture goes, and at each tick of its own frame clock the
  view picks the decoded frame for the playback clock and draws it under
  the page, so overlays stay on top. YouTube's player rereads its size
  twice per frame while its controls show, and each read relaid the whole
  watch page (about 25 ms), so the engine thread managed 13 frames a
  second and a 25 fps video slowed down whenever the mouse was over it;
  it now plays at its full rate, and new pictures no longer repaint the
  page. The decoder is handed about three seconds of coded frames and
  keeps up to 48 MB of decoded pictures ready, so the picture also rides
  out engine stalls of a few seconds, and it never steps back except on
  a seek.
* A video's picture follows its sound smoothly and never jumps back. The
  stream clock follows the audio device's sample clock, anchored where
  the device asks for more data, so it advances evenly however the device
  schedules its callbacks (PulseAudio and PipeWire top up deep buffers in
  bursts), and it never steps back between seeks. Drift is slewed out by
  moving the frame timeline a little each poll (at most a 5% rate change);
  a picture ahead of a starved sound holds instead of jumping back, and
  only one more than half a second behind jumps forward. The picture used
  to fall behind the sound and then jump 100 ms forward every 20 seconds
  or so, or jump back whenever the sound ran ahead of it.
* A video that switches sources no longer loses its sound, and no longer
  freezes at the next pause and play, when the old source's close reaches
  the audio thread after the new stream has opened. This is how YouTube
  froze when the content came on after an ad.
* A playing video no longer stops for 100-450 ms every second or so.
  When the view had already shown the frame the engine was aiming at, the
  engine took it for lost and restarted the decoder from the last keyframe,
  up to 4 s back.
* `<video>` plays ordinary MP4 (H.264, AV1, VP9 with AAC or Opus) and
  WebM files, not only MPEG-1, and `canPlayType("video/mp4")` and
  `"video/webm"` answer "maybe" (or "probably" for codecs Northstar
  decodes). The file is fetched with HTTP range requests: the `moov`
  is found wherever it sits (BitChute's files keep it at the end), its
  sample tables are indexed, and the samples for the next half minute
  are fed through the Media Source buffers, so decoding, sound,
  `buffered`, `readyState`, `seekable` and seeking work as they do for
  streaming sites; memory holds about a minute of the file whatever
  its length. A seek restarts the download at the keyframe before the
  target. WebM and fragmented MP4 files are read from the start, and a
  server that ignores ranges sends the whole file at once. Video.js
  on BitChute had refused the source as unsupported.
* `<video>` and `<audio>` play Media Source Extensions streams, as
  YouTube and other streaming sites send them: `MediaSource`,
  `SourceBuffer` (`appendBuffer`, `remove`, `abort`, `timestampOffset`,
  sequence mode, append windows, `buffered`) and
  `MediaSource.isTypeSupported` for AV1/VP9 video and AAC/Opus audio in
  fragmented MP4 or WebM. Buffered ranges, `readyState`, `waiting`,
  `canplay` and `playing` follow what has been appended, and appending
  past a 300 MB (video) / 48 MB (audio) quota throws
  `QuotaExceededError` so pages evict old data. Sound streams into the
  audio mixer as it decodes, and the picture follows its clock.
* A playing `<video>` or `<audio>` holds at its start, waiting, until the
  document has fired its `load` event, instead of starting while the
  page is still loading and laying itself out.
* H.264 (AVC) video decoder over Cisco's OpenH264, an optional
  dependency behind the new `-Dh264` feature (on when `openh264` is
  present). `src/h264.c` rewrites length-prefixed MP4 samples to Annex B
  and feeds the `avcC` parameter sets before the first frame and whenever
  they change. Each picture comes back right after its own access unit,
  in decode order: OpenH264's display-order reordering keys on the
  wrapping picture order count and stalls on x264 streams with B-frames.
  Only OpenH264 binaries built by Cisco are covered by the H.264 patent
  license Cisco pays for; distribution builds from source are not.
* Media Source Extensions play H.264 when OpenH264 is built in:
  `isTypeSupported()` accepts `avc1`/`avc3` Baseline, Main and High
  (8-bit 4:2:0) in MP4, which is what hls.js asks before playing
  PeerTube. Each coded frame keeps the codec configuration it arrived
  with, so the decoder gets the right SPS/PPS after a quality switch or a
  seek back into an earlier rendition. With B-frames the frame for a
  time is found by presentation time among the frames stored in decode
  order, pictures decoded before their turn are kept until shown, and an
  append that continues a coded frame group no longer deletes the
  group's reordered frames it overlaps.
* A SourceBuffer whose initialization segment carries both an audio and
  a video track, as hls.js creates for muxed HLS renditions (PeerTube,
  Twitch), plays both. Only the first decodable track was kept: PeerTube
  played without sound, and Twitch, whose segments list the audio track
  first, without picture. The buffer's `buffered` ranges are the
  intersection of its two tracks.
* `MediaSource.setLiveSeekableRange()` and `clearLiveSeekableRange()`.
  The media element's `seekable` for a MediaSource follows Media Source
  Extensions: 0 to the duration for a finite one, and for a live
  (infinite) duration the live seekable range joined with the buffered
  ranges. Twitch's player calls it after its first append and reported
  a MediaSource error ("not a function") on every live stream.
* After a seek into a part of a Media Source that is not buffered yet,
  the sound starts at the seek target instead of at the next buffered
  audio frame, which could lie anywhere later in the stream; the
  picture follows the sound's clock, so playback jumped back to an old
  buffered range.
* Incremental demuxers for fragmented MP4 (ISO BMFF: init segment, then
  `moof`/`mdat` fragments) and WebM (Matroska EBML: tracks, clusters,
  SimpleBlocks and BlockGroups). Both accept bytes in arbitrary chunks and
  yield samples with decode and presentation times, keyframe flags and
  the codec configuration (`av1C`, `vpcC`, `esds`, `dOps`/OpusHead).
* Video and audio decoders for the formats streaming sites serve: AV1
  through libdav1d and VP9 through libvpx (both optional, `-Dav1` and
  `-Dvp9`, on when the library is present), Opus through libopus when
  present, and AAC-LC through a new in-tree decoder written from ISO/IEC
  14496-3. Decoded pictures (8 to 16 bit, 4:2:0/4:2:2/4:4:4, BT.601 or
  BT.709, limited or full range) convert to BGRA textures in `src/yuv.c`.
  `src/videodec.c` and `src/audiodec.c` pick the decoder from the codec
  string.
* Script that changes an attribute layout cannot see no longer lays the
  page out again: SVG content attributes repaint their `<svg>`, `dir`
  only relayouts when the text's base direction changes, an `<a>`'s href
  only when it gains or loses one (clicks read the current href), a form
  control's class only when it gains or loses one, and attributes of
  undisplayed scripts and templates not at all. Changes made from timers,
  messages and events now also restyle without a relayout when they can,
  as changes from animation frames already did.
* Each relayout spends less time around layout itself: it looks for
  transitions and animations only on elements whose style has some, no
  longer copies every `@keyframes` rule again, and finds the page's style
  sheets with fewer string comparisons. The style pass after the cascade
  takes about a third less time on YouTube.
* Script that adds or removes nodes where layout does not go (inside an
  element that is not displayed, in an `<svg>`'s content, in `<head>`, or
  in a tree not yet in the document) restyles the page instead of laying
  it out again, unless the change brings new style sheets, a new viewport
  or a new `<base>`. Changes inside an `<svg>` repaint it. `--trace` now
  names what asked for each relayout.
* A running CSS animation or transition of opacity, transforms or colours
  repaints only where its element painted before and after each frame,
  instead of the whole page, including elements in inline-blocks. The
  Twitch landing page, whose carousel keeps an animation running,
  repainted the whole window some 50 times a second and now spends about
  a fifth of the time painting.
* `--trace` also records timers, events, animation frame callbacks,
  microtasks and garbage collections, forced reflows with the API that
  asked for them, and restyles.
* A change to a class, an inline style or an attribute that only
  selectors read restyles the page without rebuilding and laying out the
  box tree when the new computed styles differ only in what paint reads
  from the box at use time (transforms, opacity, colours, shadows,
  background position, filters) or move an absolutely positioned box by
  one inset. Geometry reads and getComputedStyle after such a change no
  longer force a relayout of the whole page: YouTube's progress bar,
  which writes transforms and then reads the player's size twice per
  frame while the controls show, cost two 20-30 ms relayouts per frame
  and now mostly costs one 6 ms restyle. When a relayout is needed after
  all, it reuses the cascade the restyle already computed, and the
  animation observer skips styles without animations or transitions.
  `NS_LAYOUT_VERIFY=1` lays the page out again after every such restyle
  and reports the first difference.
* A restyle keeps the layout when an inline box or text run gets a new
  computed style with the same values; it used to relay out the whole
  page. On YouTube the scrubber's pull indicator took such a style with
  every progress update, so half the restyles became 15-20 ms relayouts.
* A restyle that keeps the layout repaints only where the restyled boxes
  painted before and paint after (through transforms and scroll offsets),
  not the whole page. On YouTube a progress bar update repaints a
  115x17 px strip in about 5 ms instead of the whole view in 21 ms.
* An element whose style is recomputed only because a :has() rule might
  match differently keeps its style when it matches the same declarations
  under the same parent. YouTube's root element, with 1,700 custom
  properties, was rebuilt on every restyle.
* Media queries are evaluated again only when the viewport, the media
  device, print, colour scheme or reduced motion change. Before, every
  cascade reparsed the queries of every cached style sheet.
* A restyle computes and compares only the elements whose style changed,
  rather than building and comparing a table of every element's style.
  On YouTube, a progress bar update costs a 1.4 ms restyle instead of
  2 ms.
* A change to aria-label or title restyles the page instead of laying it
  out again, except on a <button> that layout may label with it. YouTube
  rewrites the time display's aria-label every second, which relaid the
  whole watch page each time.
* A restyled element whose computed values did not change keeps its
  style, and the restyle reuses the styles of elements below it whose
  matched rules did not change. Hovering a video in YouTube's sidebar
  restyled 1,250 elements in 22-27 ms; it now takes 8-12 ms.
* An element that fades in with a CSS transition stays visible when the
  transition ends. YouTube's player controls faded in on hover and then
  vanished at once.
* An animation tick stops running queued tasks once its 16 ms budget is
  spent; it checked the budget only after up to 64 tasks, so one frame on
  YouTube could wait more than a second for timers and forced relayouts.
* IndexedDB index lookups by a single key (`index.get`, `getKey`,
  `getAll`, `count`) and unique-index checks on `put`/`add` read only the
  matching rows, and uniqueness checks skip the stored values. Every
  index query read and deserialised the whole index, which was the
  largest script-side cost on a YouTube watch page (14.6% of main-thread
  time, now 0.9%).
* The compiled-JavaScript cache keeps large scripts (up to 48 MB of
  bytecode per entry) and bounds its disk use to 256 MB, dropping the
  least recently used entries. YouTube's main bundle and player were
  over the old 4 MB limit and were re-parsed on every visit; a reload
  now evaluates them in about 0.45 s and 55 ms instead of 1.2 s and
  250 ms.
* Large external scripts compile on worker threads while the page loads:
  when a preloaded script of 64 KB or more is not in the bytecode cache,
  it is compiled in the background and the main thread picks up the
  result instead of compiling it again. On a first visit to a YouTube
  watch page, scripts block the HTML parser for about 1.1 s instead of
  1.9-2.5 s.
* The background bytecode precompiler gives QuickJS a NUL-terminated
  copy of the script. QuickJS's tokenizer reads up to the terminating
  NUL, so it read past the end of the buffer (found with
  AddressSanitizer on Twitch).
* Restyling after DOM changes touches only what can have changed. A
  single `:has()` the restyle index could not key switched incremental
  restyle off for the whole page, and most changes re-cascaded their
  whole subtree. Attribute, class, child-list and hover/focus changes
  now restyle the changed element, re-match descendants only through
  invalidation sets built from the stylesheets, and stop at children
  whose parent's computed style came out unchanged. On a YouTube watch
  page the style cascade drops from 5.6 s to 2.0 s over the first 15
  seconds (about 400 elements restyled per pass instead of 4,500), and
  video ads stutter much less.
* DOM and form-state changes made through `textContent`, `innerText`,
  fragment insertion, `<select>` and `<option>` APIs, input values, form
  reset, exclusive `<details>`, checkbox/radio activation,
  `setCustomValidity` and the table section APIs mark the affected
  elements for restyle; `:empty`, `:checked` and positional selectors
  could go stale after them.
* Full style passes are about 28% cheaper: rule-index candidates carry
  what selector matching needs to reject them early, declarations whose
  value uses `var()` are parsed once per substituted value per pass
  instead of once per element, and shared style clones keep their inline
  and `currentColor` bookkeeping, so unchanged elements stop looking
  changed to incremental restyle.
* Restyle invalidation for `:has()` and child-list changes is cheap: key
  sets are checked by looking up the node's own tag, id and classes, the
  ancestor walk for `:has()` anchors stops at ancestors already walked
  since the last style pass, and a child-list change walks the ancestors
  once instead of once per child. On a YouTube watch page this cut
  main-thread CPU by about 30%.
* Painting skips fully transparent subtrees and keeps opacity groups
  element-sized: YouTube's ~20 invisible hover overlays each allocated
  and composited a viewport-sized offscreen surface every frame. A full
  repaint of a YouTube watch page drops from about 39 ms to 13 ms, and
  damage repaints cover only the damaged columns instead of the whole
  viewport width.
* Parsed stylesheets are reused across window size changes unless one
  of their own `@media` conditions changes result. They were cached per
  exact viewport size, so every resize step, and the vertical scrollbar
  appearing while a page loads, re-parsed all CSS and rebuilt every rule
  index: about 0.7 s on a YouTube watch page, now about 0.1 s.
* Incremental restyle reuses untouched subtrees wholesale: elements off
  the path to any changed element skip per-element dirty checks, the
  ancestor filter and invalidation-set matching. Small restyles are
  about 25% cheaper.
* The invalidation keys incremental restyle uses are computed once per
  stylesheet and merged when the set of sheets changes, instead of being
  rebuilt from every rule of every sheet each time a `<style>` element
  is added. YouTube's full style passes during load cost about half as
  much (412 ms to 227 ms).
* The address bar and window title follow a page that changes its
  address with `history.pushState()` or its title from script. They were
  only read when a navigation finished, so single-page sites (YouTube)
  kept showing the first page's address.
* Back, Forward and Reload follow the address a page moves to by itself
  (history.pushState, replaceState, a new fragment): after clicking from
  one YouTube video to another and following a link off the site, Back
  returns to the second video and Reload stays on it, where they used to
  load the address the page was first opened at.
* Space no longer reloads the page. Toolbar buttons took the keyboard
  focus when clicked, and a new window could start with it on one, so
  Space pressed Reload instead of reaching the page (YouTube's
  play/pause). Toolbar buttons no longer take focus on click, and a
  finished page load hands the keyboard to the page unless the address
  bar is being typed in.
* A page lays itself out for the window's size when the window was
  resized while the page loaded. It kept the size the window had when the
  load began, leaving the rest of the window blank (the Twitch landing
  page, maximised during its load, stayed 1014 pixels wide).
* Interface-specific DOM members live on the prototypes of the interfaces
  that define them (`href` and the URL parts on `HTMLAnchorElement` and
  `HTMLAreaElement`, `src` on the media, image, script and frame
  interfaces, `value`/`checked`/`form`/`disabled` on the form controls,
  `length` and the data methods on `CharacterData`, and so on), and
  `<video>`/`<audio>` inherit from `HTMLMediaElement`. They all sat on
  one prototype shared by every node, so a `<div>`, an unknown element,
  a text node and the document each reported `text`, `href`, `src`,
  `type`, `checked` and hundreds more. Feature tests such as
  `'text' in el` went wrong: YouTube's Polymer sanitizer replaced every
  `text` binding with "zClosurez", leaving the consent dialog, video
  title, description and related videos blank. `document.open()` works
  again; the element `open` accessor had shadowed it.
* `<noscript>` is never rendered when scripting is enabled, even when
  an author rule gives it a display.
* `<link rel=stylesheet>` elements in the parsed document fire `load`
  and `error`, so the `media="print" onload="this.media='all'"`
  asynchronous stylesheet pattern applies its stylesheet.
* Headless settling no longer ends while a script is blocked fetching a
  module or resource, which cut single-page apps off before their first
  XHRs completed.
* Scripts and speculative preloads resolve against the document's
  `<base href>`. Parser-inserted `<script src>` used the document URL,
  so sites built with Angular CLI (PeerTube) fetched their scripts from
  the wrong path and never started.
* A `blob:` URL has the origin of the page that created it, as the URL
  standard defines, so a worker started from one sends that `Origin`
  and passes CORS checks against it. Twitch's player worker failed its
  cross-origin `fetch()` of the WebAssembly player
  (`Access-Control-Allow-Origin: https://www.twitch.tv`) and never
  asked for the stream's playlist.
* Assigning any value to an error's `stack` stores it as an own data
  property, as other engines do. quickjs-ng's `Error.prototype.stack`
  setter throws "expects a string" for anything else, which broke
  Twitch's Kasada script; Northstar replaces the setter in every new
  context.
* A request that carries a `Range` header bypasses the HTTP cache in
  both directions. The cache is keyed by URL, so a stored 206 answered
  later requests for other byte ranges of the same file with the wrong
  bytes.
* A same-origin `<iframe>` no longer sees the parent page's own global
  variables as its own. Its window was seeded with every property of the
  parent window, including those the page's scripts had defined; it now
  receives only the browser's built-in globals, as cross-origin frames
  already did.
* A frame's `window.name` is its `<iframe name>` attribute. Same-origin
  frames reported the parent window's name, so scripts that find a
  sibling frame through `parent.frames[name]` (reCAPTCHA's challenge
  frame does this) found nothing.
* Inside an `<iframe>`, a bare name resolves to an element with that id
  in the frame's own document, not in the parent page's. Frames shared
  the parent window's named-properties object, so on Google's "unusual
  traffic" page (which has `<div id="recaptcha">`) the reCAPTCHA frames
  saw that div as their `recaptcha` namespace, and the checkbox spun
  forever.
* `history.pushState()` and its siblings taken from an iframe and called
  on another window's `history` act on that history, as in browsers. The
  iframe's stand-in history ignored the receiver, so YouTube, which takes
  these functions from a hidden iframe, changed pages without ever
  changing the address.
* An iframe shows the document a server sends with an HTTP error status
  (404, 429, 500 ...), as browsers do; only `<object>` falls back to its
  content on an error. Twitch's anti-bot challenge page arrives as a
  429 in an iframe and was dropped as "load failed".
* An iframe whose `src` is a `javascript:` URL keeps an empty
  about:blank document instead of fetching "javascript:;" from the
  network.
* Changing an iframe's `sandbox` attribute takes effect at the frame's
  next navigation, as the HTML standard specifies; the loaded document
  keeps its origin. Twitch's ad verification adds `sandbox` after the
  frame loaded and then writes into its `contentDocument`, which
  Northstar had already made null.
* `window.onload` and the other window event-handler properties fire
  in iframes. The dispatcher read the handler from the top-level
  window, so a frame's own `window.onload = ...` never ran (and the
  top page's handler ran for the frame's events). BitChute's embedded
  player starts from `window.onload` and stayed a black box.
* Fixed an intermittent crash while pages load. `getComputedStyle()`
  and similar style flushes replaced the style table without relaying
  out, and two such flushes between relayouts could free styles the
  current layout still painted with; the engine crashed in painting and
  the watchdog reloaded the page.
* The window's scroll range follows the page when its size changes after
  loading. It kept the height the page had when it opened, so pages that
  build themselves with JavaScript (YouTube's default watch layout)
  could not be scrolled until a resize, zoom or `scrollTo()` happened to
  update it.
* The layout oscillation dampener only engages for layouts that really
  alternate (A, B, A). It also counted relayouts that produced the same
  layout as before, so a page making many layout-neutral mutations while
  loading (YouTube) engaged it, and every later content change waited
  0.4-1 s to appear.
* A positioned box with `z-index: auto` no longer hides the z-indexed
  boxes inside it from clicks. Hit testing kept such a box's z-indexed
  descendants in a scope of their own, so they lost to any later sibling
  layer with a lower z-index: YouTube's consent dialog (z-index 2202,
  inside an absolutely positioned app shell) lost every click to its
  backdrop (z-index 2201) and could not be accepted or rejected. Those
  descendants now compete in the enclosing stacking context, as in CSS.
* Positioned descendants of a positioned box with `z-index: auto` are
  painted in the enclosing stacking context, ordered by z-index and tree
  order, as CSS specifies. They were painted inside that box as if it
  were a layer of its own, so a `position: fixed; z-index: 2202` dialog
  inside an absolutely positioned app shell drew below a
  `z-index: 2201` backdrop elsewhere, and deep high-z descendants drew
  below outer boxes with a lower z-index.
* Rounded borders whose sides differ in color or width keep their
  rounded corners. Only a uniform solid border followed `border-radius`;
  any other was drawn as four straight lines, so the reCAPTCHA spinner
  (a circle with two transparent sides) appeared as a square corner.
* An SVG element whose `transform` cannot be inverted (for example a
  hidden shape with `scale(0)`) is not rendered, per the SVG spec. It was
  passed to cairo, which put the whole drawing context into a permanent
  error state, so everything painted after it vanished: YouTube's player
  volume icon carries such a path, and the watch page drew only the
  player while its consent dialog was up.
* Clicking a page after a script read `getComputedStyle()` no longer
  crashes in SVG painting: inline SVG boxes kept a pointer to a style
  table that the style flush had already replaced and freed.
* `overflow: hidden` (or `clip`, `scroll`, `auto`) clips absolutely
  positioned descendants whose containing block is inside the clipping
  box. They were painted after the clip was lifted, so a positioned
  child escaped any clipping parent: YouTube's collapsed volume slider
  drew its knob over the time display. Descendants positioned against a
  box outside the clipping element still escape it, as in CSS.
* An auto-width block with `overflow: hidden` inside an explicitly
  zero-width parent clips its content to nothing. The painter skipped
  clipping for any zero-width box without its own explicit `width`, so a
  collapsed slider's contents spilled over its neighbours.
* A percentage `top` or `bottom` on a relatively positioned box resolves
  against a containing block whose height comes from flex stretching,
  even through `height: 100%` children of the stretched item. It
  resolved to 0, so YouTube's autoplay toggle (`top: 50%;
  transform: translateY(-50%)`) sat half its height above its pill.
* A line holding inline-blocks or inline images is sized and aligned
  with the CSS strut: its box reaches the larger of the strut's and the
  inline-blocks' ascents above the baseline and descents below it,
  instead of centring the line in `line-height`. Small inline-blocks
  sit on the baseline in tall lines (they were painted at the top of
  the line), and text stays on the same baseline as a taller
  inline-block beside it (it was centred a few pixels below). Painting
  and hit testing now place inline-blocks where layout does.
* An inline `<img>`, `<svg>` or `<video>` with padding, borders or
  margins counts them in the width of the line, so a shrink-to-fit
  parent (inline-block, flex item, float) wraps the whole box. Only the
  content width was measured, so YouTube's 24px speaker icon with 12px
  padding sat in a 24px button and stuck out of its pill.
* An absolutely positioned box resolves its percentage `min-height`
  against the height of its containing block even when that block's
  height is `auto`. Twitch's player (`top: 0; min-height: 100%` inside
  an aspect-ratio box) was laid out 0 px tall, so a playing live stream
  showed only the channel's offline picture behind it.
* A grid that places its items column by column (`grid-auto-flow: column`)
  and sizes itself to its content is as wide as all its columns plus the
  gaps between them, where it used to be as wide as its widest item: the
  tabs of a Twitch channel no longer pile up on top of each other.
* Security: a page could free the address the engine was still reading.
  Laying out a page fetches its stylesheets and images synchronously,
  and the engine waited for them by spinning its own main context, so
  page script queued there (deferred module scripts, microtasks,
  `AbortSignal.timeout()` callbacks) ran in the middle of layout. A
  `history.replaceState()` from that script freed the page URL that the
  stylesheet walk passed to every later request, a heap use-after-free
  that AddressSanitizer caught on github.com. The engine now waits for
  those fetches on a private context that only the fetch itself can
  wake, and layout works on its own copy of the page URL.
* Security: moving focus held the old and new elements only as raw
  pointers while it fired `blur`, `focusout`, `focus` and `focusin`, and
  each of those handlers can remove either element. A removed element
  stays alive while script can still reach it, so no page was found that
  turns this into a use-after-free today, but nothing in the focus code
  guaranteed it: freeing a node never cleared the focused-node pointer,
  and an element a `blur` handler removed still received focus. Focus
  now holds a reference to both elements across the dispatches, declines
  to focus an element a handler has removed, gives the later events a
  null `relatedTarget` when the old element is gone, and clears the
  focused-node pointer when that node is freed.
* Security: fixes from Nordstjernen's 2026-10-04 audit, ported where
  the same code exists here. Each was reachable from an ordinary page.
  - `font-family: random-item(…)` validated every option by
    canonicalizing it again, with no limit on how deeply the functions
    nested, so a value nested a few thousand levels deep cost quadratic
    memory and could overflow the stack. A `random-item()` nested more
    than 16 deep is now invalid.
  - An `@supports (prop: value)` test parses the declaration as a
    stylesheet of its own, and an `@supports` hidden in braces inside
    the value was evaluated by that inner parse, which tested its own
    declaration the same way, each round starting a fresh parse that no
    depth limit could see. A declaration more than 32 such rounds deep
    is now unsupported.
  - Matching a complex selector recursed once per compound, and neither
    the number of compounds nor a run of siblings has a limit, so
    `element.matches()` with a selector of a few hundred thousand
    `b ~ b ~ …` compounds over as many sibling elements overflowed the
    stack and crashed. A selector chain more than 1024 compounds deep
    now fails to match.
  - Flattening nested CSS rules substitutes `:is(<parent>)` for every `&`
    in a child selector, so the selector text grew multiplicatively with
    each level: `a{&&&&&&&&{&&&&&&&&{…}}}` ten levels deep, about a
    hundred bytes of CSS, made the browser allocate until it aborted.
    Flattening now has a budget of 16 MiB plus 16 bytes per byte of
    stylesheet for the selector text it builds and copies, and drops the
    remaining nested rules once that is spent; real stylesheets stay far
    below it.
  - `grid-template-columns` and `grid-template-rows` keep at most 24
    tracks, but a `repeat(auto-fill, …)` or `repeat(auto-fit, …)` that
    did not fit recorded its full track count, so grid layout read
    track sizes from past the end of the stored track list. The count is
    now clamped to the tracks actually stored, both when the value is
    parsed and when layout expands it.
  - The style engine keeps the hovered, pressed, focused and
    focus-visible elements as plain pointers, and nothing cleared them
    when the element was freed. A page that replaced a pressed element
    while the mouse button was still down could have it freed, after
    which the next restyle walked freed memory. Freeing a node now
    clears every such pointer that refers to it.
  - A text input whose value is longer than its `size` is widened on a
    second layout pass to the number of characters its CSS width can
    show, and that count was not bounded: `width: 1e10px` asked for
    hundreds of millions of padding characters and aborted the browser
    when the allocation failed, and a still larger width converted a
    double that does not fit in an `int`. The count is now worked out
    in floating point and clamped to 4096 columns.
  - Canvas `shadowBlur` accepts any finite number, and the blur radius
    was converted to an `int` before it was limited, which is undefined
    for a value such as `1e300`. It is now clamped to the blur's
    64-pixel maximum first.
  - SVG: an arc whose start point had overflowed to infinity (relative
    path commands add finite coordinates up past the largest double)
    turned its segment count into NaN before converting it to an
    `int`, and an image with `width="1e308in"` or a `viewBox` whose
    aspect ratio overflows sized its raster from an infinite or NaN
    dimension. Both conversions are undefined behaviour; the segment
    count is now clamped in floating point, and a non-finite intrinsic
    size falls back to the default raster size.
  - Printing walked a multi-line text box in line-height steps, adding a
    break span per line. A box placed a quintillion pixels down the page
    never advanced, because adding one line height no longer changed the
    position, and a very tall box asked for a span per line of its
    height, so printing such a page spun until memory ran out. The walk
    now stops when a step no longer moves and is capped at about a
    million spans.
  - Opening a `northstar-unsafe-continue:` link, the safe-browsing
    interstitial's way through, stripped the prefix and called itself
    again, copying the rest of the URL at every level, so a link made
    of thousands of repeated prefixes cost quadratic memory and could
    overflow the stack. The prefixes are now skipped in a loop and the
    remaining URL is opened once.
  - `DecompressionStream` inflated each chunk into one buffer outside
    the JavaScript heap with no limit, so a small, highly compressible
    gzip body made the browser allocate gigabytes, and past 4 GiB it
    aborted. A single chunk or flush that would produce more than
    256 MiB now fails the stream with a `RangeError`.
  - Following a same-page `#fragment` link handed the page URL itself to
    the `hashchange` event as `newURL`, but setting `oldURL` first could
    run a page-defined setter, and a setter that called
    `history.replaceState()` freed that URL before it was read. The
    event now gets a copy.
  - Building an `Attr` wrapper ran page script (a getter on
    `window.Attr`, setters on `Attr.prototype`) and then went on reading
    the attribute it was wrapping, so a script that removed the
    attribute meanwhile had its freed value copied into a new Text node.
    The wrapper now reads its own copies. An `Attr` attached to a second
    element from an `attributeChangedCallback` is detached from the first
    one, instead of being listed twice and hanging the page's teardown.
  - `history.go()` converted its argument with a plain C cast, undefined
    for values such as `1e10` or `Infinity`, and added it to the history
    position in `int`, so `history.go(2147483647)` overflowed before the
    range check. It now converts with ToInt32 and checks the target in
    64 bits.
  - `addEventListener()` reads `signal.aborted` of every listener already
    registered, and a page getter there could remove that listener: it
    was freed while the loop still compared its event type. Listeners
    are now kept until the loop ends, and one removed by the getter is
    skipped.
  - `insertRow()` and `insertCell()` collected the existing rows or cells
    before converting the index argument, whose `valueOf()` runs page
    script. A `valueOf()` that moved the table into a `<template>` and
    reset the template's `innerHTML` freed those rows, which were then
    used as the insertion point. The argument is now converted first,
    and a throwing `valueOf()` propagates.
  - `replaceChildren()`, `textContent`/`innerHTML` replacement and
    clearing walked the old children through a `next` pointer saved
    before `disconnectedCallback` ran page script. A callback that moved
    a sibling left it pointing at a parent that no longer listed it, and
    could make a node being inserted an ancestor of the parent. The loops
    now take the first child afresh each time, and `replaceChildren()`
    skips a node that has become the parent or one of its ancestors.
    Setting a `<template>`'s `innerHTML` orphans the old content instead
    of freeing nodes page script may still hold, and SVG
    `beginElement()` passes a copy of its `to` value.
  - Looking up a custom element's class read a property that a page
    getter on `Function.prototype` could intercept, and a getter that
    called `customElements.define()` for the same name freed the
    registry entry the lookup then returned. The upgrade itself borrowed
    the class across its constructor and callbacks the same way. Both
    now hold their own reference, and a deep chain of not-yet-upgraded
    custom elements is bounded by the 512-level limit instead of
    restarting it at every level.
* Packaging refuses to build from a stale subproject checkout. Meson
  never moves a git wrap's checkout after the `.wrap` pin changes, so
  the first 1.0.13 Windows packages were built with quickjs-ng 0.16.1
  while the wrap pinned v0.17.0. `scripts/check-subprojects.sh`
  compares each checkout with its pin, and `pack-windows.sh`,
  `pack-linux.sh`, `pack-appimage.sh` and `pack-srpm.sh` run it before
  building.
* The Windows build moves to MSYS2's UCRT64 environment. MSYS2 no
  longer publishes GTK 4 for MINGW64, so the Windows CI job stopped
  before compiling anything. CI now installs the
  `mingw-w64-ucrt-x86_64-*` packages; the `_msys_*` helper scripts,
  `run-windows.ps1` and `smoke-windows.ps1` use UCRT64 and fall back to
  MINGW64 only on a machine where that alone has GTK 4 installed, and
  `pack-windows.sh` and `pack-windows-installer.sh` look in UCRT64
  first.
* The musl and i386 CI jobs no longer fail when one registry throttles
  GitHub's shared runners. ECR Public, where both images came from since
  Docker Hub answered 429 Too Many Requests, began refusing them with
  "Data limit exceeded". `scripts/ci-pull-image.sh` now takes the
  official image from Docker Hub, `mirror.gcr.io` or ECR Public,
  whichever serves it, and the musl build runs in that container through
  `scripts/ci-musl.sh`, as the i386 build already did.

1.0.13:
=======
* WebAssembly runs on WAMR's fast interpreter instead of the classic one.
  It is still an interpreter, with no JIT and no writable-and-executable
  memory, and large modules run several times faster.
* WebAssembly: a JavaScript exception thrown into a wasm call that
  JavaScript made from inside another wasm call now reaches that
  JavaScript caller. It used to surface only at the outermost call, so
  the `try`/`catch` around the inner call never ran. Emscripten's
  `setjmp`/`longjmp` and C++ exceptions rely on catching it there.
* `new ImageData(data, width[, height])` and `new ImageData(width,
  height)` construct real `ImageData` objects, validating the sizes as
  the HTML standard requires. The constructor was a placeholder that
  ignored its arguments, so `putImageData` of such an object drew
  nothing. Objects from `getImageData` and `createImageData` are now
  `instanceof ImageData`.
* `window.scrollX`, `scrollY`, `pageXOffset` and `pageYOffset` were
  registered as QuickJS getters with a four-argument function, a
  mismatched call that only worked by accident.
* `<video>` plays the sound of an MPEG-1 program stream. The MP2 track
  plays in the audio mixer from the bytes the video already downloaded,
  `play()`, `pause()`, seeking, `volume`, `muted` and `loop` drive both,
  and the picture follows the sound's clock (measured within 15 ms). Like
  `<audio>`, a video with sound autoplays only when muted or after a user
  gesture.
* Closing the window no longer crashes. The address bar's suggestion
  popover was unparented from the window's destroy handler, after GTK had
  already finalized the address entry it belonged to; it is now
  unparented when the entry itself is destroyed.
* `<video controls>` and `<audio controls>` show working playback
  controls: play/pause, a seek bar that seeks on click or drag, the
  current time and duration, and a mute button. `<audio controls>` drew
  an inert strip and `<video controls>` drew nothing.
* `<video>` decodes as it plays instead of decoding the whole clip up
  front. A clip was cut off at 256 MB of decoded frames (11 s at 640x360,
  21 s at 480x270); memory now holds one frame whatever the length, and
  the page no longer waits for the whole decode before the first frame.
* Security: a source-wide audit fixed several memory-safety and
  denial-of-service bugs reachable from an ordinary web page.
  - WebAssembly: an exported function that returned an `externref` not
    held by a table or global freed the host box before converting it to
    a JavaScript value, so the 64th such call dereferenced freed memory
    and crashed. The periodic externref reclaim now runs after the
    results are converted.
  - `IntersectionObserver`: the observer tick held a raw pointer into the
    targets array across the `DOMRect` construction it triggers, so a page
    that replaced `window.DOMRect` and called `observe()` from it
    reallocated the array and left the tick writing through freed memory.
    The tick now addresses targets by index and re-checks bounds.
  - `setAttributeNS(ns, "class", …)` fed the class-token cache but only
    `class` in the null namespace cleared it, so overwriting a namespaced
    `class` attribute freed the backing string while the cache still
    pointed into it. Both namespaced mutators now clear the cache.
  - Inserting an element more than 512 levels above its target parent
    escaped the depth-capped ancestor check and formed a parent-pointer
    cycle, which hung the browser on the next tree walk. The cycle check
    is no longer depth-limited.
  - Deeply nested `display:grid` boxes recursed layout until the stack
    overflowed; layout recursion is now depth-bounded.
  - A `conic-gradient()` with an enormous `from` angle span-looped
    forever during paint; the angle is now normalised with `floor`.
  - An enormous `column-count` tried to allocate a multi-gigabyte column
    array and aborted the process; the used column count is clamped.
  - `Intl.DateTimeFormat` formatting a non-finite time value is handled
    as an invalid date instead of converting an infinity to `time_t`.
  - Media event tasks already in flight are now scrubbed when their
    element is released, closing a dangling-pointer window in the task
    queue.
* A grid item can span more than 24 rows. `span N` in `grid-row`,
  `grid-row-start`, `grid-row-end` and `grid-area` was cut to 24, the
  limit on explicit tracks in a template, although implicit rows go up
  to 4096. Google's image results lay their tiles out as a masonry of
  5px auto rows with `grid-row-start: span 25` to `span 100`, so every
  tile came out 120px tall and its caption was drawn over the picture.
  Spans now go up to the implicit row limit; a column span is still
  clamped to the 24 column tracks the layout holds. Nordstjernen fixed
  the same bug; this fix is written from the bug and the CSS Grid
  specification.
* An absolutely positioned grid whose height comes from `top` and
  `bottom` gives its `fr` rows that height to share. The row sizing read
  only the `height` property, so `grid-template-rows: repeat(2,
  minmax(0, 1fr))` on a `position: absolute; inset: 0` grid resolved to
  two empty rows and the items were only as tall as their content. The
  picture mosaic in a Google results knowledge panel is such a grid and
  showed one picture beside an empty tile. Written independently of
  Nordstjernen's fix for the same bug.
* `round()`, `mod()`, `rem()` and `abs()` with a percentage argument
  resolve it at layout time against the property's real basis, as
  `min()`, `max()` and `clamp()` already did. They were folded into a
  pixel length when the declaration was parsed, taking the percentage of
  the viewport width, so `width: round(nearest, 100%, 1px)` -- which
  Google's results page puts on every top-level section -- made each
  section as wide as the window and ran it over the right-hand column.
  `round()` with a length and no step, or with more than two values, is
  now rejected as the specification requires. One of these functions
  nested inside `calc()`, `min()`, `max()` or `clamp()` still uses the
  viewport-based value. Written independently of Nordstjernen's fix for
  the same bug.
* The methods and attributes of `Node` and every interface that
  inherits from it throw `TypeError: Illegal invocation` when they are
  called on something that is not a node, as WebIDL requires and Chrome
  and Firefox do; promise-returning operations (`play()`, `decode()`,
  `requestFullscreen()`, ...) reject instead, and the
  `[LegacyLenientThis]` event handlers return `undefined`. They used to
  return `null`, `0` or `false` for a plain object or an
  `Object.create(Element.prototype)`: only 76 of 2,897 such members threw,
  now 2,848 do and the rest are the rejecting, lenient and `EventTarget`
  members. Google's sign-in page probes exactly this and sends a browser
  that does not throw to "This browser or app may not be secure"; Gmail
  cannot be reached without passing it. Each checked call costs one
  extra native call, about 15 ns. Written independently of Nordstjernen's
  fix, which patches its in-tree JavaScript engine; Northstar carries no
  engine fork, so the check wraps the members instead.
* Assigning to a property of a custom element's prototype
  (`MyElement.prototype.async = function () {...}`) no longer throws
  `Illegal invocation` when Northstar's shared element prototype carries
  an accessor of that name. Chrome keeps such accessors on the specific
  interface (`async` lives only on `HTMLScriptElement`), so the
  assignment creates an own property there; the checked setters now do
  the same for an ordinary object that inherits from `Node.prototype`,
  while a setter called directly on a foreign object still throws.
  YouTube's Polymer runtime assigns `async` this way and stopped at its
  loading skeleton after the receiver checks landed; it boots again.
* `window.postMessage()`, `MessagePort.postMessage()`, a port's queued
  messages when it starts, and `BroadcastChannel` deliver their messages
  as tasks, as HTML specifies, instead of on the microtask queue. A
  message overtook the promise callbacks queued before it, so a script
  that posted to a port and to the window, queued a promise callback and
  set a zero-delay timer saw "port, window, timeout, microtask"; it now
  sees "microtask, port, window, timeout", as in Chrome. Google's Closure
  library (`goog.async.nextTick`) and React's scheduler both use
  `MessageChannel` to yield a task. Each turn delivers the messages
  queued before it began, with a microtask checkpoint after each one; a
  page whose handler keeps posting to itself continues at idle priority,
  so timers, network completions and repaints still get their turn, and
  delivery waits while a synchronous load pumps the main loop. Written
  independently of Nordstjernen's fix for the same bug.
* A page that registers a custom property with `@property` or
  `CSS.registerProperty()` no longer copies every inherited custom
  property for each element that declares one of its own. The
  registered path built a flat table of the whole inherited chain per
  such element and then walked all of it again to compute registered
  values; YouTube registers one property and carries about 1,300
  variables, so every forced layout of a watch page spent most of its
  cascade copying them. An element's own table now holds its
  declarations and the registered properties whose value differs from
  what it would inherit, on top of the parent's chain, as it already did
  without registrations. Restyling 800 elements under 1,361 variables
  and one registration goes from 300-400 ms to 13-17 ms; computed
  values are unchanged. Written independently of Nordstjernen's fix for
  the same problem.
* An image or inline block wider than a centred, right-aligned or
  end-aligned line starts at the line's start edge and overflows the end,
  as CSS Text requires, instead of hanging off both sides. Pango centres
  an overflowing line with a negative offset, so the left part of an
  oversized picture in a `text-align: center` box -- reCAPTCHA's image
  challenge tiles, for one -- was cut off. A layout whose lines all
  overflow is now laid out, hit-tested and painted start-aligned.
  Written independently of Nordstjernen's fix for the same bug.
* `position: relative` moves inline images, inline blocks and inline
  flex or grid boxes, and every relatively positioned element inside
  one. The offset pass never entered the boxes a line holds, and the
  paint, hit test and geometry code placed those boxes where the text
  put them, so a relative offset on or inside an inline-block was
  dropped. reCAPTCHA's image challenge shows each tile as a window on
  one large picture moved with percentage `top` and `left`, and every
  tile showed the same corner of it. Painting, hit testing,
  `getBoundingClientRect()` and `offsetLeft`/`offsetTop` now include the
  offset, as in Chrome. Written independently of Nordstjernen's fix for
  the inline-box case; the nested case was not part of it.
* Clicks go to the positioned box that is painted on top when several
  share a stack level. The box tree puts absolutely and fixed positioned
  boxes after their in-flow siblings, and hit testing broke ties by that
  order while painting uses document order, so a `position: relative`
  box drawn over an earlier fixed overlay lost its clicks to the
  overlay. reCAPTCHA puts exactly such an overlay under its challenge to
  close it on an outside click, and every click on the challenge closed
  it. Ties now go by document order, and a positioned sibling ranks above
  a non-positioned one, as in painting. Written independently of
  Nordstjernen's fix for the same bug.
* `window.setTimeout()` and `window.setInterval()` called by the page
  while a frame is loading -- from a frame's script calling into its
  parent, or from the frame's `load` handler -- are no longer dropped.
  The check for a detached window compared the window's document with
  the document currently being run, which is the frame's at that
  moment, and took the page's own window for a detached one. A window
  now counts as detached only when its document is outside the page's
  tree. reCAPTCHA continues its work in such timers, so
  `grecaptcha.execute()` never produced a token. Written independently
  of Nordstjernen's fix for the same bug.
* `Event.prototype` no longer carries an `isTrusted` getter, which
  WebIDL makes an unforgeable own property of each event, so assigning
  `isTrusted` on an object derived from `Event.prototype` works as in
  Chrome instead of throwing. Events the engine fires take the prototype
  of the interface their type belongs to -- `click` a `PointerEvent`,
  `mousedown` a `MouseEvent`, `focus` a `FocusEvent`, `input` an
  `InputEvent`, `message` a `MessageEvent`, `hashchange`, `popstate`,
  `storage`, `pageshow`, animation, transition, clipboard and toggle
  events theirs -- instead of all being plain `Event`s, so
  `e instanceof MouseEvent` and `e instanceof MessageEvent` hold; port
  messages, which had no event prototype at all, get the same. Both
  were found by Nordstjernen while tracing Google's reCAPTCHA frames;
  this fix is written independently of its change.
* A message posted between a page and its frames arrives as objects of
  the receiving window: `e.data instanceof Array`, `Object`, `Date`,
  `Map`, `Set`, `RegExp`, `Number`, a typed array or an error class, and
  `e instanceof MessageEvent`, now hold in the receiver. The structured
  clone ran once, in the sender's realm, and recognised dates, maps,
  sets, regular expressions, data views and boxed primitives with
  `instanceof` against that realm's constructors. It now recognises them
  by their engine class, and a window message is cloned again into the
  receiving realm when it is delivered. Ladybird reported this class of
  bug as what kept Google reCAPTCHA from passing; Nordstjernen fixed it
  too, and this fix is written independently of its change.
* A window's child frames can be reached by index and by name, also
  from inside a frame: `window.kid` and `parent['kid']` return the frame
  named `kid`, `parent.frames.length` and `parent.frames[0]` work from a
  frame's own script, and a cross-origin frame can reach its siblings
  through `parent.frames[name]` and `parent[i]` and post messages to
  them, which HTML allows across origins; the parent's document and its
  elements stay out of reach. The page's window resolved indices,
  `length` and names against whichever document was running, which is
  the frame's own while frame code runs, and looked names up only as
  element ids. reCAPTCHA's challenge frames find each other this way and
  failed with "Cannot read properties of undefined (reading
  'postMessage')". Written independently of Nordstjernen's fix.
* A document is styled with its own style sheets only. A frame's
  document sits inside the page's tree, and every sheet of the page and
  of its frames was matched against every element: the page's
  `body { height: 1200px }` made the body of each of its frames 1200px
  tall, and a sheet a frame linked styled the page. Only a frame's
  `<style>` elements were kept to it. The sheet collector now records the
  document each sheet belongs to, and the cascade matches an element
  only against its own document's sheets. Gmail, Google sign-in and
  reCAPTCHA all run in frames. Written independently of Nordstjernen's
  fix for the same bug.
* Reading a property of a `<form>` no longer walks the document. A form's
  named properties override its own members, so every read -- `form.action`
  and `form.method` included -- ran the named-property hook, which built
  a fresh `form.elements` collection and searched it. The form's controls
  and an index of their names and ids are now kept until the DOM next
  changes. On a form with 300 controls, 40,000 reads of `action` and
  `method` take 29 ms instead of 1.3 s, and 20,000 reads of a named
  control 9 ms instead of 640 ms; Google's results page reads its search
  form this way while it settles. Written independently of
  Nordstjernen's fix for the same problem.
* Pages with container queries are laid out once per relayout when no
  container changed size, instead of always twice. Every relayout of such
  a page ran the cascade and the layout without container sizes, measured
  the containers and ran both again with the sizes; Google's results page
  is one, and relaid out dozens of times while it loads. The relayout now
  starts from the container sizes the previous relayout of the same
  document measured, and only cascades and lays out again when the new
  layout's containers differ, repeating until they agree (at most three
  extra passes). Incremental restyling stays in use while the container
  sizes are unchanged, and the selector match cache is only kept when a
  second pass is likely. Inline-block and other atomic inline containers
  are now measured at all, and a container whose size depends on an
  outer container's query gets the size it really has rather than one
  from the first pass: 300 cards in a `container-type` wrapper resize
  correctly across breakpoints, and 30 forced relayouts take 443 ms
  instead of 788 ms. Written independently of Nordstjernen's work on the
  same problem.
* A node counts as connected when it is in the page, not only when it is
  under the document whose code is running. While a frame's script runs
  -- or the page's code runs on its behalf, as when a frame calls a
  function of its parent -- the engine's current document is the frame's,
  so a script or iframe the page inserted into its own document then
  never ran or loaded, and custom elements, form submission and
  `scrollend` treated page nodes as disconnected too. Gmail and Google's
  sign-in pages drive their frames this way. Written independently of
  Nordstjernen's fix for the same bug.
* `event.source` of a message between a page and its frame is the
  window as the receiver sees it: in the page, the frame's
  `contentWindow` (also for a cross-origin frame), and in the frame, its
  `parent`. A message the page sent through `frame.contentWindow.postMessage()`
  claimed to come from the frame itself, with the frame's origin, and a
  message from a cross-origin frame inserted by script carried the
  frame's own global rather than the parent's view of it, so
  `e.source === iframe.contentWindow` checks -- with which Google's
  frame APIs and reCAPTCHA authenticate messages -- failed. Written
  independently of Nordstjernen's fixes in the same area.
* The toolbar, address-bar and logo icons render on systems without
  librsvg's GdkPixbuf loader (Debian/Ubuntu `librsvg2-common`), which a
  KDE or minimal desktop often lacks. GTK hands full-colour SVG icons to
  GdkPixbuf, so without that loader every icon was GTK's image-missing
  placeholder, with no warning. The shell now draws its bundled icons
  through the in-engine SVG renderer (`src/gtk/icons.c`), at the
  screen's own scale, and sets the window icon only when GTK can load
  it. Fixes #16.
* The standards-conformance backport released in 1.0.12 is withdrawn.
  Thirteen of its commits ported work that outside contributors wrote
  for Nordstjernen under the Nordstjernen Source License, which this GPL
  edition may not distribute without their permission: frame named
  access, realms, messaging and event dispatch (`a41234d`, `37a597c`,
  `68d06b8`, `293b46f`), focus tracking, nested-frame parents,
  `MessagePort` transfer, browser-fired event flags and shadow-tree
  separation (`93901bf` to `d0b0e7a`), and the CSS-wide animation
  keyword and `SourceBuffer.appendBuffer` fixes (`acc38d3`, `b27c3f4`).
  Those areas behave as in 1.0.11 again. The bytecode cache format moves
  on, so no script compiled by 1.0.11 or 1.0.12 is reused.
* `animation` and `transition` set to a CSS-wide keyword (`inherit`,
  `initial`, `unset`, `revert`) set every longhand to that keyword, as
  the background shorthand already did. The shorthand expansion read the
  keyword as an animation list and crashed about a third of the loads of
  a page that used one. The 1.0.12 fix was among the withdrawn commits;
  this one is written anew.
* Web and service workers run on threads with an 8 MiB stack on every
  POSIX platform, like the engine thread. They had the platform's default
  thread stack, 512 KiB on macOS and 128 KiB with musl, under a 5 MiB
  JavaScript stack limit, so deep recursion in a worker crashed the
  browser instead of throwing `RangeError`. 1.0.12 fixed this another
  way in a withdrawn commit; this fix is written anew.
* `prepend()` no longer links a node to itself when it is handed the
  parent's current first child. It took the reference child before
  moving the nodes, so that node ended up as its own next sibling and the
  next walk over the children never finished; example.com's script does
  exactly this for English-language visitors, and the page hung, which is
  also what kept the musl CI smoke test running until it was cancelled.
  Ported from open-internet-navigator.
* Builds on the original QuickJS (`-Djs_engine=quickjs`) no longer crash
  when a page loads a module script from the bytecode cache. A cached
  module was evaluated without resolving its imports first. quickjs-ng
  resolves them on evaluation; the original engine dereferenced the
  missing import and crashed. In practice, any site built from ES modules
  crashed the second time it was opened, once its modules were cached.
* `docs/js-engine-comparison.md` measures the quickjs-ng and the original
  QuickJS builds against each other: build cost, startup, Octane, DOM and
  built-in micro-benchmarks, Speedometer 3.1, web-platform-tests and
  language features.
* `docs/building.md` documents a MacPorts build on macOS beside the
  Homebrew one.
* The README's project-home link at the bottom points at Northstar's own
  page, <https://nordstjernen.org/northstar-browser/>, instead of the
  Nordstjernen front page, and its links to the Nordstjernen repository
  use that repository's current name, `nordstjernen-browser`.
* `meson setup` checks every system-library version floor -- GLib 2.80,
  Cairo 1.18, HarfBuzz 8.3, FriBidi 1.0.6, fontconfig 2.15, GTK 4.14 and
  libcurl 8.5 -- before it configures ns-pango, and names each library
  that is missing or too old in one error. On a release older than
  Ubuntu 24.04 the build used to stop inside the ns-pango subproject on
  whichever floor it met first, as a bare GLib version mismatch, with
  nothing saying that GTK and the rest of the text stack were too old as
  well (#13). The Debian and RPM build dependencies carry the same
  floors, and `docs/building.md` says which releases meet them.
* 32-bit builds no longer put every cached selector match in one hash
  bucket. The selector-cache hash folded a pointer-sized value with
  `h >> 32`, which is undefined where pointers are 32 bits wide. On x86
  it shifted by zero and the hash came out as 0 for every key, so
  each style lookup scanned the whole cache. GCC warned about it on
  i386 (`-Wshift-count-overflow`, #15). The fold is now done in 64 bits.
* The i386 GCC build links cleanly with `--werror`. GCC's link-time
  optimiser could not see that the MP3 decoder's short-block scalefactor
  loop stays inside its 40-entry array and reported
  `-Wstringop-overflow` (#15); the loop now carries that bound
  explicitly. A new `linux-i386` CI workflow builds and smoke-tests
  Northstar in a 32-bit Debian 13 container on every push, using
  `scripts/ci-linux-i386.sh`.
* Icon-font ligatures that start with "s" (search, settings, star,
  share, ...) render as icons again instead of as letters. ns-pango is
  updated to a revision that no longer hands HarfBuzz made-up glyph ids,
  whose high bits HarfBuzz's coverage cache dropped, so shaping a space
  in an icon font marked glyph 32 as uncovered for the life of the face.
* The Windows CI build compiles again with clang 23, whose new
  `-Wunused-but-set-global` warning stopped it on a watchdog flag that
  was set and never read. The flag is gone.
* Grid rows are sized from single-row items first and then from
  spanning items in order of increasing span, as CSS Grid's track sizing
  algorithm requires. Items were taken in document order, so an item
  spanning several auto rows that came before the items of those rows
  spread its height over rows that were still empty. On Wikipedia's
  Vector skin the Appearance column, which spans the title, toolbar and
  article rows, added about 135px of blank space under the title and
  again under the Article/Talk tabs.

1.0.12:
=======
* A page with a few hundred nested elements, or a script that recurses
  deeply, no longer crashes the browser on macOS. Pages run on the
  engine thread, which was created with the system's default thread
  stack -- 512 KB on macOS and 128 KB with musl -- while layout needs a
  few KB per nesting level and page JavaScript is allowed 5 MB of stack.
  Around 200 nested `<div>` elements, or `function f(){return f()}`,
  overflowed it instead of being laid out or throwing `RangeError`. The
  engine thread now gets an 8 MB stack on every platform, the same as
  the main thread.
* The Linux build instructions list `git`, which `meson setup` needs to
  fetch the lexbor, QuickJS and ns-pango subprojects.
* Shadow roots behave as document fragments: nodes directly inside one
  have a `null` `parentElement`, the root's `nodeName` is
  `#document-fragment`, it is not `instanceof Element`, and `closest()`,
  `contains()` and `compareDocumentPosition()` stop at the shadow
  boundary. Selectors with an id run inside a shadow root, such as
  `shadowRoot.querySelector('p#x')`, find their element; they used to
  look it up in the document and come back empty.
* A frame starts with its own `onload`, `onclick` and other window event
  handlers set to `null`. It used to start with copies of its parent's,
  so the page's `window.onload` could run again for the frame.
* Nested frames know their place: a frame inside a frame has the middle
  frame as its `parent`, so `parent.frames`, `parent.document` and
  `parent.name` refer to it, while `top` stays the page. `parent.location`
  and `top.location` read in a frame give that window's URL instead of
  the frame's own, setting `parent.location.hash` fires `hashchange` at
  the page, and a frame's `document.referrer` is the URL of the document
  holding it, cut down by the referrer policy as in other browsers.
* Focus follows the HTML focus update steps. `blur` and `focusout` run
  while no element has focus, `relatedTarget` names the other element,
  and the windows get `blur` and `focus` when focus moves between a page
  and its frames, by script or by a click. `document.activeElement` is
  the iframe, not the element inside it, when a frame has focus, so a
  page can no longer see which element of a cross-origin frame is
  focused. `document.hasFocus()` is true only for the focused document
  and the documents containing it, and `focus()` does nothing on an
  element that cannot take focus or is not in the document. Clicking
  plain content inside a frame focuses the frame's document, as in other
  browsers, instead of the iframe element around it.
* Events the browser fires carry the flags other browsers give them:
  `focus`, `blur`, `readystatechange`, element `scroll` and media events
  no longer bubble, and `DOMContentLoaded`, `input`, `change`, `scroll`
  and `focusin`/`focusout` can no longer be cancelled. `invalid` can be.
* Window events look as they do in other browsers: `load` reaches only
  window listeners, with the document as its target; `hashchange` and
  `popstate` are fired at the window and do not bubble; `pageshow` and
  `pagehide` have the document as their target; `window.onload`,
  `window.onmessage` and the other window handlers see `eventPhase` and
  `currentTarget`; and `<body onload>`-style handlers run with the window
  as `this`. Scrolling the page fires one `scroll` event instead of two,
  and `resize` is fired at the window only. `message` and `storage`
  events have the window as their target; they used to report the
  document.
* Transferring a `MessagePort` moves it: the receiver gets a new port
  object, references to the port inside the message point to that new
  port, and the sender's port stops sending and receiving. Messages
  already queued for the port go with it. Transferring a port twice,
  transferring the port that sends the message, or listing a port twice
  throws `DataCloneError`, and a message that fails to clone leaves its
  ports in place. `window.postMessage` transfers its ports even when the
  target origin does not match, as in other browsers.
* A frame's origin comes from the URL the engine loaded it from, not from
  its `location.origin`, which the frame could redefine to send messages
  under another origin and receive messages posted to that origin.
* A frame's document no longer shows through its iframe element:
  `textContent`, `innerHTML`, `outerHTML`, `getHTML()`, `XMLSerializer`,
  `TreeWalker`, `hasChildNodes()` and `cloneNode(true)` treat the iframe as
  having only its fallback content, as `childNodes` already did. They used
  to include the frame's text and markup, even for a cross-origin frame.
* Deep recursion in a worker throws a stack overflow error (`RangeError`
  on quickjs-ng, `InternalError` on the original QuickJS) instead of
  crashing the browser: the worker's JavaScript stack limit now fits its
  thread's stack, which on macOS is 512 KiB.
* Message ports work across frames: a frame that receives a transferred
  `MessagePort` gets its message events and `ports` in its own realm,
  `addEventListener` on a port accepts `{handleEvent}` objects, and the
  handlers run against the frame's document. Exceptions thrown by port
  handlers and by window `on<event>` handlers now reach `onerror` and
  `error` listeners instead of being dropped.
* `XMLHttpRequest`, `FileReader`, `Worker`, `WebSocket` and `EventSource`
  events, observer callbacks and the load events of images created from
  script run against the document of the frame whose handler they call,
  so a frame's relative URLs no longer resolve against the parent page.
  Promise reactions keep running against the document that is current
  when the microtask queue drains: neither stock quickjs-ng nor the
  original QuickJS tells which realm a pending job belongs to.
* Events inside a frame reach that frame's window: window listeners and
  `window.onclick`-style handlers see clicks, pointer and key events,
  `DOMContentLoaded` and bubbling custom events, and a frame's `load`
  event fires once with the document as its target. A frame's events
  no longer run the parent window's `on<event>` handlers.
* When the browser dispatches an event, such as a user click or a port
  message, microtasks queued by one listener run before the next
  listener, as in other browsers.
* Trusted mouse and pointer events are `MouseEvent`/`PointerEvent`s of the
  target's realm, with frame-relative `clientX`/`clientY`, `pageX`/`pageY`,
  `offsetX`/`offsetY`, `view` and `timeStamp`.
* The window keeps being ticked while a `requestIdleCallback`, a posted
  message or a script-started image load is waiting.
* `window.postMessage` throws `SyntaxError` for an unparseable target
  origin and defaults to `"/"` when none is given; messages to and from a
  frame sandboxed without `allow-same-origin` use the opaque origin
  `"null"`; message events target the receiving window and carry a
  frozen `ports` array from the receiving realm.
* `window.postMessage(message, [port])` with an array as the second
  argument follows the `(message, options)` overload, as WebIDL overload
  resolution requires: the array is read as an options dictionary, so
  nothing is transferred. It was taken as a transfer list, so a library
  that posts `postMessage(token, [channel.port2])` from code shared with
  workers handed its own port to every `message` listener on the page.
  Any second argument that is neither an object nor `undefined`/`null`
  is the target origin, and `postMessage()` with no arguments throws
  `TypeError`.
* Aliased built-ins keep their spec names (`String.prototype.trimLeft`
  is the function named `trimStart`, `URL` stays `URL` beside
  `webkitURL`), and `Function.prototype.name` is empty again. The `URL`
  interface is installed after `URLSearchParams`, which it needs.
* `postMessage` and `MessagePort` messages are delivered as tasks after
  the sender's microtasks, as in other browsers. They ran as microtasks,
  so a message arrived before promise callbacks queued ahead of it, and a
  handler that posted back to itself kept `setTimeout` callbacks from
  ever running.
* A frame's `WindowProxy` stays the same object across its first
  navigation. A `contentWindow` read while an iframe still showed its
  initial `about:blank` now reaches the document that loads into it:
  same-origin frames reuse the initial window, as the HTML spec
  requires, and messages posted through the early reference to a
  cross-origin frame are delivered. The link between a frame's outer
  window and its realm global moved out of JavaScript-visible
  properties, which had let a cross-origin frame reach its embedder's
  window.
* Structured data sent between windows, frames and `MessageChannel`
  ports arrives as objects of the receiving realm: `e.data instanceof
  Array`, `Map`, `Date` and `Uint8Array` hold in the receiver. Messages
  crossing a frame boundary carried the sender's objects, and `Map`,
  `Set`, `Date`, `RegExp`, `DataView` and boxed primitives from another
  realm degraded to plain objects. The original QuickJS gains
  `JS_IsDate`, `JS_IsRegExp`, `JS_IsMap`, `JS_IsSet`, `JS_IsDataView` and
  `JS_IsProxy` in `src/quickjs_compat.c`, and both engines
  `JS_GetBoxedPrimitiveKind`, all from class ids learned on the first
  context.
* Frames have their own document lifecycle and geometry. Each frame's
  `document.readyState` runs `loading` -> `interactive` -> `complete`
  with `readystatechange` at each step instead of reporting the top
  page's state. `innerWidth`/`innerHeight` in a laid-out frame are the
  frame's size rather than the top window's, element rects and
  `elementFromPoint` inside a frame are measured from the iframe's
  content box rather than its border box.
* A message from a cross-origin frame to its parent has the frame's
  WindowProxy as `event.source`, the same object as
  `iframe.contentWindow`, so pages can tell which frame spoke.
* Rounded solid borders whose sides differ in color, or have some sides
  transparent, follow the corner radius instead of being stroked as
  straight lines with square corners.
* Events carry the interface their type implies: window, port and
  worker messages are `MessageEvent`s, and `error`, `hashchange`,
  `popstate`, `storage`, `pageshow`/`pagehide` and promise rejection
  events get their own interfaces. Port messages were plain objects.
  `isTrusted` lives on each event, not on `Event.prototype`, so objects
  deriving from `Event.prototype` can define their own.
* Objects the engine hands to pages inherit from their WebIDL interface
  and report its name: `new FileReader() instanceof FileReader`,
  `new AbortController()`, `FormData` and `AudioContext` instances, and
  `Object.prototype.toString.call(localStorage)` is `[object Storage]`.
  Canvas contexts, `TextMetrics`, `ImageData`, `MediaQueryList`,
  `FontFaceSet`, `location`, `screen` and the `navigator` sub-objects
  follow suit, and `Location`, `Screen`, `BarProp`,
  `CustomElementRegistry` and the other interfaces they belong to exist
  as globals. Constructors made by `ns_make_ctor` give a plain object
  they return `new.target.prototype`, through a trampoline instead of an
  engine change.
* Functions the engine implements in JavaScript print as native code,
  `function animate() { [native code] }`, like every other built-in, and
  carry their member name instead of an internal one (`elementAnimate`).
  About a thousand engine functions printed their source. Engine
  scripts are compiled without source text through
  `JS_EvalHidingSource`/`JS_CompileHidingSource` in
  `src/quickjs_compat.c`: quickjs-ng round-trips the compiled script
  through `JS_WriteObject` with `JS_WRITE_OBJ_STRIP_SOURCE`, the original
  QuickJS compiles it under `JS_STRIP_SOURCE`. Page scripts, inline
  handlers, string timers and `new Function` keep their source. The
  compiled-script cache format version is bumped.
* Inside a frame, `window`, `location` and `history` report themselves
  as `[object Window]`, `[object Location]` and `[object History]`, as on
  the top-level page.
* `performance.getEntries`, `getEntriesByType` and `getEntriesByName`
  return empty lists in workers instead of throwing, and `clearMarks` and
  `clearMeasures` exist. A frame's document has `domain`, `timeline`,
  `pictureInPictureEnabled`, `adoptedStyleSheets`, `xmlEncoding` and
  `xmlStandalone` like the top-level document.
* Dedicated workers have `Intl` and `crossOriginIsolated`. A worker that
  read its time zone or formatted a number threw `ReferenceError`.
* Numbers the engine formats keep a `.` decimal point under locales that
  use a comma, such as Turkish or Norwegian. The engine runs inside the
  GTK shell, whose startup applies the OS locale, so `Accept-Language`
  went out as `tr-TR,tr;q=0,9`, an invalid header.
* Northstar can be built on Fabrice Bellard's original QuickJS instead of
  quickjs-ng: `meson setup builddir -Djs_engine=quickjs`. quickjs-ng stays
  the default. The original is fetched from its 2026-06-04 release and
  built from a meson file Northstar supplies; the quickjs-ng API calls it
  lacks are provided in `src/quickjs_compat.c` over its public API. Each
  engine keeps its compiled-script cache apart, and `about:northstar`
  names the engine in use. `docs/quickjs.md` lists what differs.
* A module script that imports other modules runs again after the page
  has created an event, posted a message, opened IndexedDB or fetched
  something. Native code that built those objects assigned `isTrusted`
  over the read-only accessor on `Event.prototype`, and a fetched
  response's `body` over the read-only `Response.prototype.body`, ignoring
  the failure; quickjs-ng kept the error pending and reported it from the
  next module evaluation as `TypeError: no setter for property`, so the
  whole module was lost. Each page load also left two such errors behind
  from start-up: `Event.prototype` was being made its own prototype, and
  `new URL()` built its `searchParams` before `URLSearchParams` existed.
  Trusted events keep `isTrusted` true through the accessor.
* On the original QuickJS, `Array.prototype.sort` calls the comparator for
  identical values, as every other engine does. Without that, jQuery 4's
  `uniqueSort` kept duplicates, so `$(a).add(a)` and `.closest()` returned
  the same element twice. The adapter also pads short argument lists to
  `JS_NewTypedArray`, whose constructor reads three arguments regardless,
  and learns each typed-array class on its own instead of assuming they
  are consecutive.
* The Windows build compiles against newer GLib, whose `g_renew` declares
  a local that shadowed one in the CSS candidate collector under
  `-Werror=shadow`.
* `window[name]` returns the WindowProxy of the child frame whose `name`
  attribute matches, before falling back to an element id, as the HTML
  named-access rules specify. From inside a frame, `parent.frames.length`
  counted the calling frame's own children and `parent[i]`,
  `parent.frames[name]` and `parent.foo` looked in the frame's document,
  so sibling frames could not reach each other to `postMessage`. Named
  and indexed access now resolves in the document of the window being
  read: `frames[0].foo` finds the frame's element, and a frame's
  `window.foo` never finds one in its parent. A cross-origin parent
  exposes its child frames by index and name, each behind the same
  cross-origin WindowProxy, while every other property still throws
  `SecurityError`.
* Inline event-handler content attributes inside a frame, such as
  `<body onload="...">`, are compiled and run in that frame's realm. They
  ran in the top-level page's realm, so they could not see functions
  declared by the frame's own scripts and failed with `ReferenceError`.
  A frame whose markup carries such handlers gets its own realm even
  without a `<script>`, and an `<iframe onload="...">` attribute runs in
  the page that holds the iframe.
* A frame's `window.name` starts as its `<iframe name>`, and a same-origin
  frame's `window.frameElement` is its `<iframe>`. Both were empty, so a
  frame could not find itself in `parent.frames`.
* `postMessage` into a same-origin frame reports the caller as
  `event.source`. It reported the receiving frame itself, so replying to
  `event.source` sent the answer back into the frame instead of to its
  parent. A message from another origin hands the frame a cross-origin
  WindowProxy as `event.source` rather than the sender's own window.
* `SourceBuffer.appendBuffer()` accepts data again. It still called a
  helper that the Blob rewrite removed, so every append threw
  `ReferenceError` and no byte reached the native stream. It now copies
  the `ArrayBuffer` or view it is given and throws `TypeError` for
  anything else, before the state checks, as WebIDL requires.
* `animation` and `transition` set to a CSS-wide keyword (`inherit`,
  `initial`, `unset`, `revert`, `revert-layer`) give that keyword to
  every longhand. The shorthand expansion read the keyword as an
  animation list, which crashed the renderer on pages such as YouTube
  and made `animation: inherit` compute to `none`.

1.0.11:
=======
* The Microsoft Store package is built under the Northstar Browser
  product identity (`29567TheFreecivProject.NorthstarBrowser`, published
  by Nordstjernen), without the phone identity of the old reservation.

1.0.10:
=======
* `align-content` works on block containers, as CSS Box Alignment
  specifies: `center`, `end` and their `safe`/`unsafe` forms move the
  content of a block with a fixed height. It only worked in flex and grid
  containers, so a vertically centered block kept its text at the top.
* A block-level `<textarea>` with a CSS height is sized by that height.
  Its text run was sized as the whole control, padding included, so a
  single-row field overflowed its own content box: it drew a scrollbar
  and reported a `scrollHeight` 20px too tall, which auto-growing chat
  inputs copied into their height.
* A focused, empty `<textarea>` keeps showing its placeholder, with the
  caret in front of it, as `<input>` already did. Placeholder text is no
  longer spell-checked.
* Web fonts whose files carry a family name other than the `@font-face`
  family are registered under the CSS family with each file's own
  weight. A family split over Regular, Medium and Bold files used to be
  aliased to whichever file loaded first, so normal text could render
  bold.
* Northstar builds again against a shared lexbor library, as MSYS2
  ships it. The character-encoding tables were placed in static
  initializers, which a DLL-imported symbol cannot be, so the build
  stopped; they are now looked up when first needed.
* A change to a page no longer breaks every paragraph into lines again.
  The layout tree is rebuilt on each change, and with it every text
  run's measurement, so a counter updating on a page of 400 cards spent
  40 ms per update re-measuring text that had not changed. Text
  measurements now outlive the rebuild, keyed by everything they depend
  on; the same update takes 6.5 ms, and `NS_LAYOUT_CACHE=verify` checks
  every reused measurement.
* Animated images, canvas drawing, video frames, element scrolling and
  the blinking caret repaint only the part of the page they change, and
  scrolling the page shifts the previous frame and paints only the rows
  that came into view. Every one of these used to repaint and copy the
  whole viewport: a 64-pixel animated GIF on a long page of cards kept
  the engine thread busy 80% of the time. The window copies only the
  changed rectangles too. Changes the engine cannot place -- relayouts,
  restyles, CSS animations, content under a transform -- still repaint
  everything, and `NS_DAMAGE=verify` checks each partial frame against a
  full repaint.
* Blurred `box-shadow`s are cached, so a page of cards with the same
  shadow blurs it once instead of once per card per frame, and shadows
  outside the repainted area are skipped. Painting such a page fell from
  72 ms to 3 ms a frame.
* Text crossing the top or bottom edge of the window no longer loses the
  outermost row of its glyphs' antialiasing.
* A cross-origin frame can no longer read the embedding site's images
  through a canvas. Whether an image was cross-origin was judged against
  the top-level page even when a frame's script had loaded it, because
  the frame's canvas calls run through the page's own functions: a frame
  could draw and read back images from the embedding page's origin, and
  got a `SecurityError` for images from its own. An image is now judged
  against the document that requested it.
* A deep Web Audio graph no longer crashes the browser when an
  `OfflineAudioContext` renders it. Each node with inputs takes a scratch
  buffer the length of the render, one per level of the graph, and a
  failed allocation aborted the process: a long context feeding an
  oscillator through thirty gain nodes took the browser down even within
  the new buffer limits. A level whose buffer cannot be allocated is now
  rendered silent instead.
* Drawing an image into a canvas is back to its old speed. The
  cross-origin check parsed the image's URL and the page's URL on every
  `drawImage` call, which nearly doubled its cost for sprite-heavy pages
  and games; URLs that share the page's scheme and host now pass without
  parsing.
* Cross-origin images requested with `crossorigin` and served with an
  `Access-Control-Allow-Origin` header that admits the page no longer
  taint a canvas. They were treated like any other cross-origin image, so
  a site that loads its images from a CDN with CORS enabled and reads
  them back -- image editors, thumbnailers, charting and map libraries --
  got a `SecurityError` from `getImageData` or `toDataURL`. The header is
  checked the same way `fetch()` checks it; without the attribute, or
  with a header naming another origin, the image still taints.
* An `<img>` in the page's markup whose same-origin address redirects to
  another site now taints a canvas it is drawn into. Images the page loads
  up front were stored without the address the redirect ended at, so they
  were judged by the address the markup asked for and their pixels stayed
  readable through `getImageData` and `toDataURL`. Images created from
  script already recorded it.
* `OffscreenCanvas.convertToBlob()` rejects with a `SecurityError` when
  a cross-origin image has been drawn into the canvas. `getImageData`,
  `toDataURL` and `toBlob` already refused a tainted canvas, but
  `convertToBlob` still resolved with a PNG of its pixels, so a page could
  read another site's image through an OffscreenCanvas.
* `structuredClone()` and `postMessage()` keep a `DOMException`: the
  copy is a `DOMException` with the same `name`, `message` and `code`,
  where it used to arrive as an empty plain object.
* A media query containing `/*` inside a quoted string no longer crashes
  the browser. Stripping comments from a query called the matcher again
  on the result, and a `/*` inside quotes survives the stripping, so
  `matchMedia("(x: '/*')")` or `@media (x: "/*") {}` recursed until the
  stack overflowed. The comments are stripped once and the result is
  evaluated directly.
* A frame's `window` event listeners hear only its own document's
  events. Listeners added through any window were kept in one list and
  every one of them ran for every window-level event, so a frame --
  cross-origin included -- that listened for `click` or `keydown` on its
  window received the embedding page's clicks and key presses, with the
  page's elements as `event.target`. Each listener now belongs to the
  document of the window (or document) it was added to, window-level
  dispatch runs only the listeners of the event's document with that
  document's window as `currentTarget`, and a frame's window gets its own
  `load` event when the frame finishes loading.
* A cross-origin frame no longer reaches the embedding page through what
  the two share in the one QuickJS runtime. Its global object received a
  copy of every global on the parent's window, the page's own variables
  included, so `window.someState` or any stored reference to `document`
  handed the frame the parent's DOM and cookies; `new Text()`,
  `new Comment()` and `new Range()` produced nodes of the parent's
  document; `customElements.define()` in the frame upgraded the parent's
  elements with the frame's class; and the `cookieStore` shim returned
  the parent's cookies. A cross-origin frame now gets only the browser's
  own globals, captured before the page's scripts run, and none of the
  parent-bound ones; nodes and ranges a frame creates belong to its own
  document; each document has its own custom element registry, as HTML
  specifies; and `self.origin` reports the frame's origin.
* Documents without a browsing context -- from `DOMParser`,
  `createHTMLDocument` or `cloneNode` -- return an empty `document.cookie`,
  as HTML specifies, instead of the page's cookies, and the
  `Document.prototype` cookie accessor no longer hands the page's cookies
  to another document it is called on.
* A frame whose URL redirects to another site takes the origin of the
  page it actually loads. It kept the URL it was requested with, so a
  same-origin address that redirected elsewhere gave the other site's
  document the embedding page's origin: it could read the parent's DOM
  and cookies, and the parent could read it.
* `postMessage`'s `targetOrigin` is checked against the origin of the
  window the message goes to. The top-level window's origin was read from
  a URL that is switched to the running frame's while the frame's script
  runs, so when a frame posted to its parent the parent seemed to have
  the frame's origin: a message addressed to the parent's real origin was
  dropped and one addressed to any other origin -- the case `targetOrigin`
  exists to stop -- was delivered, and replies carried the wrong origin.
* `crypto.subtle` supports Ed448 and X448 from the Secure Curves draft,
  which used to reject with NotSupportedError. Both generate keys,
  import and export them as `raw`, `spki`, `pkcs8` and JWK (`kty`
  "OKP", `crv` "Ed448"/"X448", `alg` "Ed448"), and follow the usage and
  extractable rules of Ed25519 and X25519. Ed448 signs and verifies with
  114-byte signatures and, like Ed25519, refuses small-order public keys
  and signature points, which OpenSSL alone would accept. Its optional
  `context` must be a BufferSource of at most 255 bytes (an
  OperationError otherwise); a non-empty context needs OpenSSL 3.2 or
  newer and is a NotSupportedError with older libraries, rather than
  being dropped. X448 `deriveBits` and `deriveKey` reject an all-zero
  shared secret with OperationError.
* `window.scrollTo()`, `scroll()`, `scrollBy()` and setting
  `document.documentElement.scrollTop` or `scrollLeft` scroll the page.
  They used to change only what `scrollY` reported while the view stayed
  where it was, so "back to top" buttons, scroll restoration and
  script-driven anchor links did nothing. The position is clamped to the
  scrollable range, `scrollBy()` called without a receiver starts from
  the real position instead of 0, and `scrollY` no longer flickers back
  to the old position while the view catches up.
* `<audio>` and `<video>` are real media elements now. Each has a player
  object (`src/js_media.c`) with the HTML state machine: `networkState`
  and `readyState` move through loading, metadata and playable,
  `duration` is known once the file has decoded, `currentTime` is read
  from the mixer or the frame timeline instead of a stored guess, and the
  events arrive in order -- `loadstart`, `durationchange`,
  `loadedmetadata`, `loadeddata`, `canplay`, `canplaythrough`, `play`,
  `playing`, `timeupdate` every 250 ms, `pause`, `ended`, `error`.
  `play()` returns a promise that resolves when playback starts and
  rejects with `NotSupportedError` or `AbortError` when it cannot; before,
  it resolved at once, a missing file failed silently and `ended` never
  fired for audio. The mixer reports load, position, end and errors back
  through `ns_audio_context_status`, and commands go straight to it
  instead of waiting for the next rendered frame. `loop` is honoured for
  audio and video, a video without `loop` stops at its end instead of
  repeating forever, and a video without `autoplay` waits on its first
  frame until `play()`. `autoplay` starts audio only when it is muted or
  the user has interacted with the page, `muted` and `volume` apply
  (`volume` outside 0-1 throws `IndexSizeError`), `videoWidth` and
  `videoHeight` report the clip's size, a new `src` restarts loading, an
  element removed from the page pauses, a page left in the back/forward
  cache goes quiet, and a `new Audio(url)` that nothing references keeps
  playing to the end. A `<source>` whose type cannot be played is skipped
  instead of fetched.
* Every subresource request now passes one policy check, in the network
  layer, before it is sent and again at each redirect. The request
  carries what it is for (image, stylesheet, font, media, frame, worker,
  script or connection) and the policy of the page that asked for it, and
  the check applies the `file:` rule, mixed content and
  Content-Security-Policy together. What this changes for pages:
  `img-src`, `media-src` and `font-src` are enforced (they were parsed and
  ignored); an `https:` page's `http:` stylesheets, fonts and frames are
  blocked and its `http:` images and media are upgraded to `https:`, where
  before all of them loaded over plain HTTP; `connect-src` and mixed
  content are checked at every redirect hop instead of after the whole
  response had been downloaded; `sendBeacon` obeys `connect-src`; and
  `http://localhost` is no longer treated as mixed content. `<audio>` is
  now fetched through the same network stack as the rest of the page,
  with the page's cookie partition, HSTS and the policy check, instead of
  a separate libcurl handle with its own User-Agent, and it is decoded
  from memory instead of being written to `~/.cache/northstar/msaudio`.
* An idle page no longer costs a frame every 16 ms. The window used to
  ask the engine for a frame at 60 Hz whenever the page had any timer,
  fetch or socket outstanding, and most of those frames came back
  unchanged; a page with a 50 ms `setInterval` that never touches the
  DOM made 443 frame requests in eight seconds and now makes one. The
  engine now wakes the window when there is something to show -- a DOM
  change, a `requestAnimationFrame`, an image or canvas update, a scroll
  or a navigation -- and the 60 Hz loop runs only while
  something animates. `requestAnimationFrame` timestamps and CSS
  animations follow the display's frame clock (callbacks are 16.7 ms
  apart instead of wherever the engine thread happened to run), canvas
  drawing repaints without relaying out the page, and wheel scrolls and
  window resizes that arrive while the engine is busy are merged instead
  of queued one by one. Element `scrollTop` and paint-only `<img>` changes
  made from a timer now repaint at once; they were drawn only when
  something else happened to repaint.
* Forms submitted from pages in a legacy encoding send their fields in
  that encoding (or the first valid `accept-charset` label) as the
  Encoding Standard's encoders produce them. A single character the
  encoding could not represent -- a snowman on a Shift_JIS page, `Ā` on
  a windows-1252 page -- made the whole field fall back to UTF-8; it is
  now sent as `%26%23NNNN%3B` and the rest of the field keeps its
  encoding. Link queries use the same encoders, which fixes the
  Shift_JIS NEC/IBM duplicates (U+2170 is `FA40`, not `EEEF`), the
  GB18030-2022 mappings in gb18030 and GBK, and the ISO-2022-JP escape
  that must close a JIS X 0208 run before an unencodable character.
* `--trace=FILE` writes a Chrome trace-event file of where the engine's
  time goes -- each frame's tick, paint and copy, every cascade and
  layout, script evaluations, fetches, image decodes and the GTK thread's
  present -- for Perfetto or `chrome://tracing`, in the GUI as well as
  headless. `scripts/sample-profile.sh` now samples the engine thread
  instead of the GTK main thread, which in the GUI only ever waits.
* The Windows process mitigations are the ones intended. The policies
  were passed as bare numbers, and two were wrong: the call meant for
  ASLR set DEP (always on for 64-bit) and the one meant for the
  dynamic-code ban set Control Flow Guard (which cannot be enabled after
  start), so neither took effect. The policies are now named, forced
  image relocation applies everywhere, and headless and tooling runs --
  which load no GPU driver or shell extension -- also refuse to create
  executable memory.
* Images and MPEG-1 video that a page fetches are decoded on a worker
  thread. They were decoded on the engine thread as each download
  finished, so a video clip froze the page's scripts, timers and
  rendering for as long as it took to decode every frame -- over half a
  second for three seconds of 720p.
* The audio worker checks `file:` URLs itself. It opened any `file://`
  path it was handed, relying on the script bindings alone to refuse
  local files to `http(s)` pages; the page view now tells it whether the
  page is a `file:` document, and the worker refuses local paths
  otherwise. It also decodes the URL properly, so a local file whose name
  holds a space or other escaped character plays instead of failing to
  open.
* `OfflineAudioContext` and `createBuffer` refuse a channel count outside
  1-32, a sample rate outside 3000-768000 Hz, or more than 64 Mi samples
  in all, with the `NotSupportedError` the Web Audio specification names.
  A page could ask for `new OfflineAudioContext(1, 2e9, 44100)`, or raise
  `length` before `startRendering()`, and the renderer's 8 GB allocation
  aborted the whole browser.
* `canPlayType`, `navigator.mediaCapabilities` and
  `MediaSource.isTypeSupported` answer from one table of what the build
  can decode (`src/media_types.c`). `decodingInfo` claimed WebM, VP8, VP9,
  WAV and Opus played and MPEG and MP3 did not -- the reverse of the
  truth -- and `encodingInfo` claimed the same though nothing encodes.
  `canPlayType` now answers for the element it is called on (a `<video>`
  does not play an MP3), says "probably" to `mp2v` no longer (pl_mpeg is
  MPEG-1 only), and a build without the SDL2 mixer no longer advertises
  any audio type.
* A build configured with `-Daudio=disabled` links again. The audio stub
  that replaces the SDL2 mixer lacked `ns_audio_context_dispatch_blob`,
  which the page view calls for `blob:` media, so the final link failed.
* Links in pages that use a legacy encoding put non-ASCII query text
  into the URL in that encoding, as HTML's URL parsing requires: on a
  windows-1252 page `<a href="?q=é">` now reads back and navigates as
  `?q=%E9` rather than `?q=%C3%A9`, Shift_JIS, EUC-KR or Big5 pages
  send their own multi-byte sequences, and a character the encoding
  cannot represent becomes `%26%23NNNN%3B`. The fragment and every
  other part of the URL stay UTF-8. A link in an iframe uses the
  iframe document's encoding, and one in a document made by
  `new Document()`, `createHTMLDocument()` or `DOMParser` uses that
  document's (UTF-8), even before it is inserted.
* `crypto.subtle` follows the Web Cryptography API's algorithm
  normalization and error rules. Algorithm dictionaries are read the
  WebIDL way (a missing member or an out-of-range length is a
  TypeError, an unknown algorithm or hash a NotSupportedError), usages
  that do not fit the key are a SyntaxError, bad key material a
  DataError and a key used for the wrong algorithm, usage or key type
  an InvalidAccessError, where many of these used to come back as
  OperationError or as a plain Error. Keys are real `CryptoKey` objects
  whose `algorithm` carries `length`, `hash`, `modulusLength`,
  `publicExponent` or `namedCurve` as the key type requires; JWK import
  checks `kty`, `use`, `key_ops`, `ext`, `alg` and `crv` and rejects
  mismatched EC and Ed25519 key pairs; JWK export sets `alg`;
  `deriveBits` honours an absent, zero or non-byte length; Ed25519
  verification refuses small-order keys and signatures; compressed EC
  points import and export uncompressed; `wrapKey`/`unwrapKey` with JWK
  no longer fail to parse the unwrapped text; and `structuredClone()`
  copies a `CryptoKey`.
* `crypto.getRandomValues()` throws the errors the Web Cryptography API
  specifies: a `TypeMismatchError` DOMException for a `DataView` (it
  was a plain TypeError) and a `QuotaExceededError` for more than 65536
  bytes (it was a RangeError).
* `XMLHttpRequest.responseText` decodes the body in the charset named by
  `overrideMimeType()` or the response's `Content-Type`, and a byte-order
  mark overrides both, as the XHR standard specifies. The body was
  always read as UTF-8, so Shift_JIS or windows-1252 text came back as
  mojibake; `x-user-defined` now maps bytes 0x80-0xFF to U+F780-U+F7FF
  as the Encoding Standard defines.
* `TextDecoder` accepts every label of the Encoding Standard, not just
  UTF-8, UTF-16 and windows-1252, so `new TextDecoder("shift_jis")` or
  `"gbk"` no longer throws a RangeError, and it decodes with the same
  decoders as documents: partial sequences of any encoding carry over
  between `{stream: true}` calls and invalid input is replaced the way
  the standard specifies. `TextDecoder` and `TextEncoder` are real
  interfaces with `encoding`, `fatal`, `ignoreBOM` and the methods on
  their prototypes, `TextEncoder.encodeInto()` is native and counts
  what it read in UTF-16 units, `TextDecoderStream` reports the
  decoder's canonical encoding name and its `fatal`/`ignoreBOM`, and
  `TextEncoderStream` joins a surrogate pair split across two chunks.
* Pages in the legacy East Asian encodings -- Big5, EUC-JP, Shift_JIS,
  ISO-2022-JP, EUC-KR, GBK and gb18030 -- decode as the Encoding
  Standard specifies. The whole document went through iconv in one
  call, so a single sequence iconv's table lacked (Big5 has hundreds of
  them, and Shift_JIS pages use NEC and IBM rows) failed the conversion
  and the page was read as UTF-8 or windows-1252 instead. Each encoding
  now has its own decoder that follows the standard's state machine,
  looks characters up in the standard's index tables (those lexbor
  already ships, so decoding no longer depends on the platform's iconv)
  and puts U+FFFD where a sequence is invalid. Single-byte encodings
  decode the same way, so a windows-1252 page with a byte like 0x81 no
  longer falls back to another encoding, and the labels that name the
  replacement encoding give a single U+FFFD rather than decoded text.
* A frame sandboxed without `allow-same-origin` can no longer reach the
  page that embeds it. A `srcdoc` or same-origin frame sandboxed that way
  got the embedding page's real window as `parent` and `top`, so its
  script could read and change the parent's DOM, `document.cookie` and
  `localStorage`; it now gets the same restricted window proxy as a
  cross-origin frame, where anything but `postMessage`, the `location`
  setter and a few navigation properties throws `SecurityError`.
* `--dump=print:FILE` works under the Linux sandbox. The output directory
  of a print dump was not made writable the way a `png:` or `pdf:` dump's
  is, so the pagination check `docs/building.md` describes failed with
  "failed to create PDF surface" and wrote nothing.
* The documentation matches the source again. `docs/architecture.md`
  describes the threads, the two headless paths, printing and
  diagnostics as they are, and its diagram is regenerated from
  `scripts/gen-architecture.py` instead of the old picture of a render
  protocol that no longer exists. `SECURITY.md` lists the Windows
  mitigations that are actually applied, the real Landlock paths, which
  CSP directives, mixed-content rules and SRI checks are enforced, how
  the address bar shows internationalised hosts, and the known gaps.
  `docs/compliance.md` leads with the latest reading per area and drops
  gaps that have closed, `docs/building.md` documents every command-line
  option and environment variable, and the manual page does the same.
* `reportError(value)` reports the value the way an uncaught exception
  is reported: it fires a cancelable `error` event at the window with
  the caller's file, line and column and `event.error` set to the value,
  and logs it only if no listener cancels the event. It used to print
  the value to the console and nothing else, so error trackers never
  saw it. The `error` events for uncaught exceptions are now
  `ErrorEvent` instances.
* `Object.prototype.toString` names the natively implemented interfaces:
  a `MessagePort`, `XMLHttpRequest`, `MessageChannel`, `DOMParser` and
  the like read `[object MessagePort]` and so on instead of
  `[object Object]`, which scripts use to tell platform objects apart.
* URL setters behave as the URL Standard describes where the parser
  library does not: `url.host = "example.com:"` or `"example.com:abc"`
  changes the host and keeps the port, an out-of-range port still sets
  the host, clearing the host of a non-special URL with credentials or a
  port is refused, and a URL with an empty host cannot gain a username,
  password or port. `new URL("??a=b").searchParams` keeps the second
  `?`. Links (`<a>`, `<area>`) whose `href` does not parse now report
  `":"` as the protocol and ignore setters instead of working on the raw
  string, and an `href` containing a NUL character is no longer cut
  short there.
* An iframe whose `load` handler navigates it again (for example to
  `about:blank`) no longer freezes the page. Each reload ran inside the
  same loop that processed the previous one, so timers and rendering
  never got a turn; the next load now waits for the following frame.
* `structuredClone()` and `postMessage()` follow the HTML serialization
  rules more closely. Transferring an `ArrayBuffer` detaches it (a
  detached one, a duplicate, or an object that cannot be transferred
  throws `DataCloneError`), resizable buffers keep their
  `maxByteLength`, views of one buffer still share a buffer in the
  copy, sparse arrays keep their length, `BigInt` wrappers survive, and
  errors keep only an own `message` and a standard name. A page that
  replaces `window.structuredClone` no longer changes what
  `postMessage()` sends.
* `Blob` and `File` follow the File API. The constructors accept any
  iterable of parts and reject strings, numbers and plain objects,
  honour `endings: "native"`, read their options in the specified
  order, drop a `type` with non-printable characters and copy
  `ArrayBuffer` parts (a detached one is empty); `size`, `type`, `name`
  and `lastModified` are prototype getters, `slice()` clamps like other
  browsers and validates its content type, `blob.bytes()` exists, and
  `String(blob)` is `[object Blob]`. Calling `Blob()` without `new`
  throws.
* Setting `meta.content`, `textarea.rows` or `frameset.rows` from script
  changes the attribute; the assignments were silently ignored.
  `role`, `ariaLabel`, `ariaBusy` and the other ARIA properties read
  `null` when the attribute is absent (as in other browsers) instead of
  an empty string or a default such as `"false"`, setting them to
  `null` removes the attribute, and the numeric and text ARIA
  properties such as `ariaValueNow` and `ariaLevel` exist at all.
  `font.size` is a string, `textarea.cols = 0` falls back to the
  default, `progress.max` ignores non-positive values and parses its
  attribute with the HTML number rules (`"5%"` is 5), assigning `null`
  to the legacy colour and margin attributes clears them, `object.data`
  resolves an empty value against the base URL, and setting `label` or
  `defaultValue` is seen by mutation observers.
* `innerText` and `textContent` of a shadow host no longer include the
  shadow tree's text, text inside an inline `<svg>`'s `<text>` elements
  is part of `innerText`, a `visibility: hidden` paragraph or `<br>`
  adds no line breaks of its own, and an `<optgroup>` outside a
  `<select>` keeps its text. Setting `innerText` or `outerText` to a
  string with a NUL character keeps the text after it instead of
  cutting it off there.
* A `fit-content(<length>)` grid track sizes to its content up to the
  limit. It behaved like `minmax(auto, <length>)` and always grew to
  the limit, so `fit-content(70px)` holding 30px of text was 70px wide
  and an empty one kept its full size instead of collapsing.
* Rows repeated by `grid-template-rows: repeat(auto-fit, ...)` that no
  item occupies collapse to zero height, as auto-fit columns already
  did, instead of keeping their size like `auto-fill`.
* Line names inside `repeat(auto-fill, ...)` and `repeat(auto-fit,
  ...)` are repeated with the tracks. They were kept only once and the
  names after the repetition kept their unrepeated line numbers, so
  `grid-column: b 3` and names following the `repeat()` placed items on
  the wrong lines, and the resolved `grid-template-columns` of such a
  grid dropped every line name.
* `getComputedStyle()` reports `grid-template-columns` and
  `grid-template-rows` of an element that is not a grid, and
  `grid-auto-columns` and `grid-auto-rows` of any element, as the
  computed track list: `repeat()`, `minmax()`, `fit-content()` and line
  names stay as written and only lengths become pixels, so `[a] 1em
  repeat(2, 2em [b] 3em)` reads back as `[a] 16px repeat(2, 32px [b]
  48px)` instead of an expanded list with the functions dropped.
* Flexible grid tracks honour their fixed minimums. `minmax(40px, 1fr)
  minmax(0, 1fr)` in a 60px grid gave the second column 30px and
  overflowed; a track whose share falls below its minimum now keeps the
  minimum and the others split what is left (40px and 20px). Space that
  `fr` factors summing below 1 leave over stretches `auto` tracks, and
  in a grid with a fixed height a `minmax(0, 1fr)` row no longer grows
  to fit its content, nor does an item spanning several rows including a
  flexible one stretch them.
* `calc()`, `min()`, `max()` and `clamp()` values set from script read
  back simplified the way CSS Values 4 serializes them: terms of the
  same unit are combined, absolute units become `px`, and a sum lists
  its number, then its percentage, then its dimensions sorted by unit,
  so `calc(1vh + 2px + 3%)` reads back as `calc(3% + 2px + 1vh)` and
  `min(1px + 1%)` as `calc(1% + 1px)`. Only a value whose rounded
  six-digit form is exact is rewritten, so `calc(100% / 3)` keeps its
  full precision for layout.
* `grid-template-rows: repeat(auto-fill, ...)` repeats its rows. Only
  columns expanded an automatic repetition; rows kept a single copy of
  the pattern. Rows now repeat as many times as fit the grid's height,
  or its `max-height` or `min-height` when the height is not fixed.
* `repeat(auto-fill, ...)` and `repeat(auto-fit, ...)` count their
  repetitions from the space the other tracks and gaps leave, and size a
  `minmax()` track by its fixed maximum; `10px 20% repeat(auto-fill,
  35px)` in a 200px grid made five repetitions that overflowed instead
  of four. Columns an item adds past the explicit grid take their size
  from `grid-auto-columns` instead of always being `auto`.
* Math functions keep the sign of zero. `calc(-0)` was rewritten to
  `calc(0)` before it was evaluated, so `1 / sign(calc(-0))` came out
  as `infinity` instead of `-infinity`; `min()`, `max()` and `clamp()`
  now order `-0` below `0`, `round()` with an infinite step, `mod()`
  and `rem()` return the signed zero CSS Values 4 specifies, and `-0`
  still reads back as `0` through `element.style`.
* A `calc()` that evaluates to NaN or an infinity no longer produces
  NaN geometry. `width: calc(NaN * 1px)` laid out and read back as
  `nanpx` and `calc(infinity * 1px)` as an unusable infinite length;
  computed values now clamp NaN to 0 and infinities to the largest
  (or most negative) representable length, as CSS Values 4 specifies,
  for lengths, percentages, numbers and the `scale` and `translate`
  properties.
* Grid items that span several `auto`, `min-content` or `max-content`
  columns size those columns. Only single-column items were measured,
  so a heading spanning two content-sized columns could overflow them;
  a spanning item's minimum and maximum content widths are now spread
  over the columns it crosses (narrower spans first, respecting each
  column's maximum), as the grid sizing algorithm specifies.
* `grid-template-columns/-rows: subgrid` checks its line-name list:
  only `[names]` groups and `repeat(N | auto-fill, [names]...)` may
  follow `subgrid` (so `subgrid 1px` or `subgrid repeat(2, 1px)` is
  dropped), the names -- including empty `[]` groups -- are kept for
  `getComputedStyle()`, and `subgrid` on an element whose parent is not
  a grid reports `none`, as it is laid out as an ordinary grid.
* A grid with only a `min-height` grows its `fr` rows to fill it. The
  common page skeleton `min-height: 100vh; grid-template-rows: auto 1fr
  auto` sized the middle row to its content and left the footer halfway
  up the screen; the flexible rows now share the space the minimum
  height leaves, as they already did for a fixed `height`.
* Grids that use `grid-template-areas` are laid out by the full grid
  algorithm. They went through a separate, reduced code path that gave
  `fr` rows no share of a fixed container height (a header / `1fr` /
  footer page left the footer under the header instead of at the
  bottom), sized every column the areas added as `1fr` instead of by
  `grid-auto-columns`, and ignored `align-content`, `align-items` and
  `grid-auto-rows`. Area names also resolve as line names, so
  `grid-row: main` and `grid-column: main-start / main-end` place items
  on a named area.
* `getComputedStyle()` reads the `grid-area`, `grid-row` and
  `grid-column` shorthands (they read empty), and `grid-template`
  reports a declared track list as declared, falling back to the
  laid-out tracks only when no template is set.
* `align-self` and `justify-self` values `self-start` and `self-end` on
  a grid item use the item's own writing mode. A `vertical-rl` item
  aligned with `justify-self: self-start` sits at the right edge of its
  area, and a vertical item with `direction: rtl` aligns to the bottom;
  only the item's `direction` was considered before, as if every item
  were horizontal.
* The `grid` and `grid-template` shorthands follow their grammar and
  set every longhand they cover. Rows written in the template form
  without a size (`"a a" "b b" 1fr`) are `auto`, where the first size
  given was applied to the first row; line names between rows merge
  (`"a" [x] [y] "b"` names one line `x y`); omitted longhands are reset,
  so `grid-template: auto / 1fr 1fr` clears an earlier
  `grid-template-areas` and `grid: auto-flow / ...` resets
  `grid-template-rows`; and invalid values (`grid-template: 10px`,
  `"a" 10px 10px`, `none / "a"`) are dropped instead of partially
  applied. `grid-auto-flow` rejects `auto` and repeated keywords.
  `element.style` and `getComputedStyle()` read both shorthands back,
  composed from their longhands, and a track list keeps its
  `repeat()` and `fit-content()` when read back from a stylesheet or a
  style attribute.
* `grid-row`, `grid-column`, `grid-area` and their `-start`/`-end`
  longhands follow the `<grid-line>` grammar. Values such as `0`,
  `span`, `span -2`, `1 2`, `auto 1` or a fifth `grid-area` part were
  accepted and could override a valid earlier declaration; they are now
  dropped. Omitted parts are filled in as the spec says (a named line
  repeats, anything else becomes `auto`), so `grid-row: 2` also resets
  an earlier `grid-row-end`, and `element.style` reads the values back
  in their shortest canonical form (`2 i span` reads `span 2 i`, `1 /
  auto` reads `1`). Line and area names keep their case, as custom
  identifiers are case-sensitive.
* Viewport units inside an iframe measure the iframe. `vw`, `vh`,
  `vmin` and the `sv*`/`lv*`/`dv*` variants in a frame's document
  resolved against the top-level window unless the frame's size came
  from its `style` attribute; the frame's own computed width and height
  now define its viewport. A `calc()`, `min()`, `max()` or `clamp()`
  mixing viewport units with other units also recomputes them for the
  current viewport, where it used to keep the size the window had when
  the stylesheet was first parsed -- so `calc(100vh - 60px)` follows a
  window resize and resolves per frame.
* Every radio button in a required group reports `valueMissing` while
  none is checked, not only the one carrying `required`; required
  checkboxes, file inputs and selects report it even when disabled;
  and `input.validity` identifies itself as a `ValidityState`.
* Submit buttons take part in form validation (`willValidate` is true
  and a custom validity message on one blocks submission), and a
  `readonly` input of any type is left out of it, as HTML specifies.
* Tab continues from where you clicked: after clicking text or any
  other non-focusable spot, Tab moves to the next focusable element
  after it and Shift+Tab to the one before, instead of starting over
  at the top of the page.
* The `--wpt` runner delivers `test_driver` clicks, pointer actions and
  key presses as trusted input through the same paths as the shell, so
  light dismiss, Escape, Tab, keyboard activation and click-to-focus
  are exercised as a user triggers them; WebDriver key codes are no
  longer typed into text fields as raw characters.
* Hiding a popover no longer hands focus back to an element that a
  script moved inside the popover while it was open; that element is
  hidden along with it.
* Popover and dialog light dismiss runs before the page receives the
  `pointerdown`/`pointerup` that triggers it, so `beforetoggle` reaches
  the page ahead of its `pointerup` listeners and a listener that
  rearranges the page cannot change which popover was clicked.
* A popover opened inside a modal `<dialog>` can be clicked. Hit
  testing only looked inside the dialog's own box and missed the
  popover drawn above it, so clicks on it landed on the dialog.
* `:focus` and `:focus-visible` are up to date when a `focus` listener
  reads styles: `getComputedStyle()` inside the handler described the
  element as unfocused, a focused button input switched to a text
  input did not start matching `:focus-visible`, and headless
  rendering matched `:focus` against the last field it had typed into
  instead of the page's focused element.
* Links, buttons, checkboxes, radio buttons and `<summary>` respond to
  the keyboard: Enter activates a focused link, button or summary, and
  Space (on release) a focused button, checkbox, radio button or
  summary, with a trusted `click` like a mouse press. Enter on a
  focused link or summary used to do nothing, Enter on a button
  submitted its form without a click, and Space scrolled the page.
* `String(link)` and `link + ""` give the URL of an `<a>` or `<area>`
  element, as their `href` stringifier requires, instead of
  "[object HTMLAnchorElement]".
* Client-side image maps work. Clicking, hovering or calling
  `elementFromPoint()` over an `<img usemap>` lands on the `<area>`
  under the pointer (rectangles, circles, polygons with the even-odd
  rule and `default`, with `coords` parsed as HTML's list of numbers,
  garbage and all), and clicking an area follows its link. Areas used
  to be invisible to the pointer, so the whole image acted as one
  unlinked picture.
* A `<script>` that is inserted empty runs once it gets text or a
  `src`, as HTML's "prepare the script element" requires: a script
  created, attached and only then filled through `textContent`,
  `appendChild` or `src` used to be marked as already started and
  never ran. A `src=""` now fires `error` instead of being ignored.
* The `autofocus` attribute works on page load: the first focusable
  element carrying it in the document gets focus at the next rendering
  update (or before `load` at the latest), unless something is
  already focused or the URL fragment points at an element. Search
  boxes and login fields marked `autofocus` used to wait for a click.
* `<dialog>` follows the HTML standard's show, close and request-close
  steps. The `close` event fired synchronously and even for a dialog
  that was never open, `close()` on a closed dialog still changed its
  `returnValue`, `show()` on a modal dialog did not throw, a modal
  dialog opened from a document that is not displayed did not throw,
  the dialog on top was whichever came last in the document rather
  than the one opened last, and focus went back to whatever was focused
  before the first modal dialog. Dialogs now fire `beforetoggle` and a
  queued `toggle`, queue `close`, remember the focus they took and hand
  it back when they close, pick their initial focus from `autofocus`
  or the first keyboard-focusable descendant, honour `closedby`
  (`closedBy` reflects it) for Escape and clicks outside, run
  `requestClose()` through a cancelable `cancel` event exactly once,
  hide unrelated popovers when opened, and stop being modal when
  removed from the document. `:modal` matches every open modal dialog.
* `srcset` is parsed as the HTML standard describes. The old splitter
  cut every candidate at its first comma, so a `data:` URL or any URL
  with a comma in it was truncated, descriptors in parentheses or with
  junk after them were not rejected, and a `src` lost to a `1x`
  candidate. An image chosen from a `2x` or `w` candidate is now laid
  out at its density-corrected size (a `srcset="big.jpg 800w"
  sizes="400px"` image is 400 pixels wide, not 800), `naturalWidth`
  and `naturalHeight` report the same corrected size, and a
  `<picture>` only considers the `<source>` elements before its
  `<img>`.
* Popovers and invoker commands work as the HTML standard describes.
  `showPopover()` only showed an element and never closed anything, a
  `popover=auto` stayed open when another opened, `popovertarget`
  buttons toggled even inside a form, and nothing ever dismissed a
  popover; an open popover also stayed `display: none` until something
  else restyled the page. Auto and hint popovers now form a stack that
  closes unrelated popovers, clicking outside or pressing Escape closes
  the topmost one (Escape also cancels a modal dialog), the `popover`,
  `popoverTargetAction`, `popoverTargetElement`, `command` and
  `commandForElement` properties reflect their attributes, removing or
  retyping an open popover hides it, `beforetoggle` fires synchronously
  and `toggle` once per task as a `ToggleEvent` with its `source`, and
  `<button commandfor command>` fires a cancelable `CommandEvent` and
  runs the popover and dialog commands. An open popover is centred in
  the viewport with the standard UA popover box style.
* `sibling-index()` and `sibling-count()` in a container size query
  resolve against the container element, instead of always counting 1.
* Flexbox follows `writing-mode`: in a vertical container `row` runs
  along the vertical inline axis and `column` along the horizontal block
  axis, and vertical items take a central baseline for baseline
  alignment, so vertical-rl/vertical-lr flex layouts no longer lay out
  as if they were horizontal.
* In a column flexbox an image, video or SVG stretches across the
  container like any other item (keeping its aspect ratio, capped by
  `max-width`), and a stretched item's percentage `max-width` resolves
  against the container instead of being applied a second time to the
  item's own width.
* Scripts that wait for web fonts measure the page in those fonts.
  `document.fonts.ready` could resolve while an `@font-face` font was
  still being fetched -- or after it had arrived but before the page was
  laid out again -- so `offsetWidth` and friends in its callback still
  reported the fallback font's metrics. A font that finishes loading now
  makes the next layout query lay the page out again, the fonts the
  page's stylesheets reference are requested before `ready` is decided,
  `document.fonts.status` reads `loading` meanwhile, and
  `document.fonts.load()` and `FontFace.load()` wait the same way.
* Headless `--wpt`, `--inspect` and image dumps load the document's
  images before its scripts run and fire their `load` events, so tests
  and scripts that measure images at `load` see their real sizes.
* `element.style.color` and the other colour properties read a colour
  back in the form CSS Color 4 and 5 give it. `lab(20 0 10/50%)` came
  back exactly as typed and `color(srgb 10% 10% 10%)` kept its
  percentages; they now read `lab(20 0 10 / 0.5)` and
  `color(srgb 0.1 0.1 0.1)`. A `color-mix()` lists its colours and
  percentages in normalised form, a relative colour keeps its `from`
  form around a normalised origin, and a `calc()` inside a colour reads
  back as its simplified value.
* `color-mix()` and relative colors that use `currentcolor` paint in
  the element's own text colour. The colour parser knew nothing of
  `currentcolor` inside a function, so a Tailwind-style
  `background-color: color-mix(in oklab, currentColor 10%, transparent)`
  was thrown away and the box stayed transparent. Such a value now keeps
  its `currentcolor` and is worked out for each element, so a child that
  inherits it under a different `color` gets its own shade, and
  `color: currentcolor` takes the parent's colour instead of the default.
  Colours are also kept at full precision in the space they are written
  in: a `none` component is filled from the other colour of a mix
  instead of reading as zero, `color-mix()` takes a percentage before
  the colour, any number of colours and every hue interpolation method,
  and `getComputedStyle` reports `lab()`, `oklch()`,
  `color(display-p3 ...)` and mixed colours in their own notation rather
  than rounding them to `rgb()`.
* An `<iframe>` whose source is an image shows it in an image document,
  as a top-level navigation does, instead of parsing the image bytes as
  HTML text.
* Images in the page start downloading as soon as the document is laid
  out, not at the first paint; each `<img>` fires its `load` or `error`
  event when its image arrives, `complete` and `naturalWidth` report
  it, and the window `load` event waits for the page's (non-lazy)
  images, as HTML says, instead of firing before any of them loaded.
* Restyling after a script change no longer gives an element the
  styles of a stranger's child. Elements whose parents have the same
  computed style share one computed style, and parents were told apart
  by a number that restarted with every style pass, so after a class,
  id or attribute change a freshly styled parent could get the same
  number as an untouched one elsewhere, and their children swapped
  inherited values -- custom properties, colours, fonts -- until the
  next full restyle. Parents that were never numbered, such as those
  using `attr()`, all counted as the same parent even on a first load.
  Every computed style now gets its own number that is never reused.
* Elements whose interface is plain `HTMLElement` (`article`,
  `section`, `b`, `abbr`, `nav`, `summary`, ...) and valid custom element
  names are no longer `HTMLUnknownElement` instances; truly unknown tags
  report `[object HTMLUnknownElement]`, and `listing` / `xmp` are
  `HTMLPreElement`s.
* `relList.supports()` answers per element: `<link>` reports the link
  types it acts on (stylesheet, icon, preload, modulepreload, ...),
  while `<a>`, `<area>` and `<form>` report only `noopener`,
  `noreferrer` and `opener`; `<form>` now has a `relList`.
* Absolutely positioned children of flex containers land where they
  should. One with `margin: auto` and no `left`/`right` sat 40px in from
  its static position instead of at it (auto margins only centre a box
  between two insets), `align-self: baseline` and `last baseline` put it
  at the wrong edge of a `wrap-reverse` line, and in a wrapping container
  whose lines are stretched by `align-content`, `flex-end` and `center`
  items stayed at the top of the taller line.
* A float in a column too narrow for it is as wide as its longest word
  or widest unbreakable child, like in other browsers, instead of being
  squeezed below it so that its content spills out of its border. A
  child with `width: 0` also counts as zero wide when its parent shrinks
  to fit, rather than as wide as its content.
* `min-content`, `max-content`, `fit-content` and `stretch` work in
  `min-width`/`max-width` and `min-height`/`max-height` as well as in
  `width`/`height`, on inline-blocks, floats, flex items and absolutely
  positioned boxes. The width keywords measured a box that had a pixel
  `width` of its own as that width -- so `width: 0; min-width:
  min-content` stayed 0 and `width: 500px; max-width: max-content`
  stayed 500px -- the height keywords were ignored, and a flex item's
  `width: min-content` or `max-width: fit-content` was sized from its
  max-content width. A flex item with a `min-content` or `max-content`
  height is no longer stretched to the line.
* A `<fieldset>`'s `<legend>` sits in the frame's top border, the way
  every browser draws it, instead of being a full-width line of text
  inside the frame. The legend is shrink-wrapped to its text whatever
  its `display`, its border box is centred on the top border, the
  border is left out behind it, and the fieldset's content starts below
  whichever of the two reaches further down. `align="center"` /
  `"right"` (mapped to `justify-self`), `justify-self` and auto margins
  place it along the border, and a right-to-left fieldset starts it on
  the right. Content written before the legend in the source now joins
  the text after it, and a fieldset, like its rendered legend, contains
  its floats.
* A type selector that follows another simple selector in a compound
  (`[foo]i`, `.a*`) is a parse error instead of silently matching, and
  `selectorText` / `cssText` drop comments and write an attribute
  selector's case flag as ` i]` / ` s]`.
* A one-line text input (`text`, `search`, `tel`, `url`, `email`,
  `password`) never uses a line height smaller than `normal`, as HTML
  requires, so `input { line-height: 1px }` no longer squashes the field
  and clips its text; `getComputedStyle` reports the used value.
* The user-agent sheet hides `area` and `base`, leaves `source` and
  `track` at `display: inline`, keeps `input type=hidden` and a `form`
  the parser left inside a table part hidden even against author
  `!important`, and lays out `embed[hidden]` as a 0x0 inline box, as
  HTML's rendering section lists.
* Setting `document.title` on a page without a `<title>` creates one
  that later reads (and `getElementsByTagName`) can find, reports the
  change to mutation observers, and does nothing when there is no
  `<head>`, as HTML says.
* WebCrypto AES-CTR honours the `length` parameter: a length outside
  1..128 is an `OperationError`, the counter wraps within its low
  `length` bits instead of carrying into the nonce, and a message long
  enough to reuse a counter block is refused.
* A comment or a `display: none` element (a `<script>`, a hidden
  `<span>`) between pieces of inline content no longer ends the line:
  `foo<!-- -->bar` and `a<script></script>b` lay out as one line of text
  instead of two.
* `display: contents` on an element that cannot be unboxed (`img`,
  `input`, `iframe`, `video`, `br`, an outermost `svg` and the other
  replaced elements and form controls CSS Display lists) computes to
  `display: none`.
* Documents in quirks mode get the extra user-agent rules HTML lists
  for them: tables reset font, line-height, white-space and text-align
  instead of inheriting them, forms keep a 1em bottom margin, stray list
  items put their marker inside, text inputs and textareas size with
  `border-box`, and left- or right-aligned images get a 3px gap.
* In quirks mode a `<td nowrap>` or `<th nowrap>` that also has a
  fixed pixel `width` wraps normally, as HTML says legacy pages expect.
* `getComputedStyle().transform` resolves only the `transform`
  property; the individual `translate`, `rotate` and `scale` properties
  stay separate as CSS Transforms 2 says, instead of being folded into
  the reported matrix.
* In a `<picture>`, the `width` and `height` of the `<source>` that is
  selected size the `<img>`, as HTML's dimension attribute source rule
  says, so art-directed images with different proportions per
  breakpoint reserve the right box.
* An `<img>`, `<video>` or `<input type=image>` with `width` and
  `height` attributes computes `aspect-ratio: auto W / H`, as the HTML
  rendering rules map them, so a responsive image styled `width: 100%;
  height: auto` reserves its height before it loads; once it has loaded
  its own ratio takes over, as the `auto` says.
* An `<iframe>` has the 2px inset border the rendering section gives it,
  which `frameborder="0"` (or any value that is not a non-zero integer)
  removes, and `<video>` computes `object-fit: contain`.
* `<fieldset>` has the rendering section's default style -- a 2px
  groove border, 2px inline margins, `0.35em 0.625em` block and `0.75em`
  inline padding and a `min-content` minimum inline size -- instead of a
  1px solid border and 8px margins, and the deprecated `ThreeDFace`
  system colour resolves to `ButtonBorder` as CSS Color 4 maps it.
* Form controls compute `appearance: auto` as the HTML rendering rules
  give them (`none` for hidden, file and image inputs), and
  `getComputedStyle` answers for prefixed aliases such as
  `-webkit-appearance` and `-webkit-border-radius` -- they were parsed as
  aliases but read back as empty strings, which feature-detection code
  takes to mean the property is unsupported.
* Legacy table borders follow the HTML rendering rules. `<table
  border>` draws an outset border of that width in the text colour (1px
  when the value does not parse) and inset 1px cell borders, instead of a
  fixed `#888` solid line; `frame` picks which sides are outset or
  hidden; `rules` collapses the borders, hides the table's own and draws
  the rules on cells, rows or groups as specified; `bordercolor` sets the
  border colour; `cellspacing` and `cellpadding` accept any non-negative
  integer; and `align=middle`/`absmiddle` centres a row or cell.
* `innerHTML`, `outerHTML`, `insertAdjacentHTML` and
  `Range.createContextualFragment` parse markup in the context
  element's namespace. Inside an `<svg>` every element came out as an
  HTML element with a lower-cased name -- `<linearGradient>` became an
  HTMLUnknownElement called `lineargradient` -- so gradients, filters and
  shapes that D3, icon libraries and chart code insert that way were
  never drawn.
* `:focus-visible` is its own pseudo-class instead of another name for
  `:focus`. Clicking a button or link no longer puts it in the
  `:focus-visible` state, so pages that draw focus rings only for
  keyboard users -- the usual `:focus:not(:focus-visible) { outline: 0 }`
  -- stop showing a ring after every click, while Tab, access keys, focus
  moved by script without a preceding click, and any text field still
  match, as the Selectors spec suggests.
* In nested CSS, declarations that follow a nested rule apply after it,
  as the CSS Nesting spec's nested-declarations rule requires. They were
  gathered into one rule at the top of the parent, so
  `.card { padding: 1px; @media (...) { padding: 20px } padding-top: 3px }`
  ended with a 20px top padding instead of 3px.
* Switching the desktop between light and dark, or turning animations
  off, now reaches `prefers-color-scheme` and `prefers-reduced-motion`
  rules in style sheets that were already loaded. Media queries are
  evaluated when a sheet is parsed, and the parsed-sheet caches were
  only cleared for print, so a page reloaded after a theme change kept
  the rules chosen for the old one.
* Invalid style rules are dropped the way CSS Syntax requires. After a
  stray `;` or `}` between rules the parser resumed at the next rule,
  and a selector list with an empty item, a trailing comma or trailing
  junk (`.a, {`, `.b) {`) was applied from whatever parsed; other
  browsers read the stray token as the start of the next rule's
  selector, which then fails, and drop the whole rule. Northstar now
  does the same, so a stylesheet that relies on those errors to hide a
  rule from browsers no longer shows it.
* A comment inside a media query no longer disables it. `@media
  (min-width: 100px) /* desktop */ { ... }` was never applied, and the
  same went for `<link media>`, `<style media>` and `matchMedia()`; the
  media query parser now treats a comment as whitespace, as `@supports`
  already did.
* `<meta charset="utf-16">` no longer turns a page into CJK mojibake. A
  document that declares UTF-16 in a meta tag is necessarily ASCII-
  compatible, and the HTML encoding sniffing rules read it as UTF-8, but
  the declaration was handed straight to the converter. Declared labels
  now go through the WHATWG encoding table for decoding too, so an
  unknown label is ignored instead of guessed at, `euc-kr` decodes as
  the Windows-949 superset the web means by it, and `iso-8859-8-i` is
  understood.
* The rest of the default stylesheet follows the HTML rendering rules
  instead of Northstar's own taste. Text is `CanvasText` (black, not
  `#1a1a1a`), links are `LinkText`/`VisitedText` (not a Wikipedia blue),
  `<code>`, `<kbd>` and `<samp>` no longer get a grey background, padding,
  a border or `pre-wrap` -- the padding also slipped a stray space into
  the text around every inline code span, and the background showed
  through dark `pre code` themes -- `<pre>` has 1em margins and a normal
  line height, `<small>`/`<big>` are `smaller`/`larger`, `<address>` is
  not grey, `<legend>` is not bold, a non-modal `<dialog open>` is
  positioned out of flow and centred rather than pushing the page down,
  and form controls no longer inherit `letter-spacing`, `line-height`,
  `text-transform`, `text-indent` or `text-shadow` from the text around
  them.
* A page can hide its own iframes again, and `<object>` shows its
  fallback content. The default stylesheet hid every frame, object and
  embed with `!important` and forced a loaded iframe to `display: block
  !important` -- and a user-agent `!important` beats anything an author
  writes, so an OAuth or payment helper iframe styled `display: none`
  was laid out in the page once it loaded, and `<object data=x.swf><p>Get
  Flash</p></object>` showed nothing at all. Those rules are ordinary
  defaults now; an `<object>` that has not loaded a document renders the
  content inside it, as HTML specifies.
* A `<summary>` draws its disclosure triangle as a list marker, the way
  the HTML rendering rules style it, so `list-style: none`, `display:
  block` or `::marker { content: "" }` removes it as in other browsers;
  it was inserted into the text itself and could not be removed. The
  summary is no longer bold, and the default stylesheet no longer pushes
  every paragraph, list, table and heading inside a `<details>` 16px to
  the right -- once per nesting level. Any element with `display:
  list-item` now gets a marker, not just `<li>`.
* More of the legacy presentational attributes work the way the HTML
  rendering rules map them. `<font size>` accepts `+n` and `-n` relative
  to size 3 and maps to the absolute font-size keywords, so `size="+1"`
  is 18px rather than 10px, `size="-1"` is no longer ignored, and nested
  `<font>` elements no longer compound. `<body>`'s `marginheight`/
  `topmargin` and `marginwidth`/`leftmargin` set its margins, falling back
  to the containing iframe's `marginheight`/`marginwidth`; `background`
  sets the background image
  of a body, table, row or cell; `<br clear>` moves what follows below
  the floats; `<caption align>` places and aligns the caption; `<nobr>`
  does not wrap; and `<marquee>` is shown as a (still) inline-block
  instead of being hidden with its text.
* Italics, underlines, strike-throughs and superscripts come from CSS
  rather than from the tag name. The text collector slanted every `<i>`,
  `<em>`, `<cite>` and `<dfn>` whatever its `font-style` said -- so Font
  Awesome's `<i class="fa">` icons were drawn slanted -- and a
  `font-style: normal` span inside italic text stayed italic. `<sup>` and
  `<sub>` were shrunk twice (the stylesheet's size and then a fixed 0.75
  scale) and raised a fixed 4px, while `vertical-align: super` on any
  other element did nothing. Now `font-style` alone decides, in both
  directions; `vertical-align: super`, `sub` and lengths raise or lower
  text by the amounts other browsers use, relative to the parent's font
  size; a `position: relative` inline element's `top`/`bottom` moves its
  text; and the default stylesheet gives `<u>`, `<ins>`, `<s>`, `<del>`
  and `<strike>` their decorations, so `getComputedStyle` reports them.
  `<ins>` and `<del>` are no longer tinted green and red.
* `<hr>` is drawn by its borders, as the HTML rendering rules style it:
  a 1px inset rule in grey with `0.5em auto` margins. It used to be a
  1px grey background with a line of its own painted 4px below the top
  whenever it had no border -- so Bootstrap's `border: 0; border-top:
  1px solid` rule showed a second, grey line under the first, and an
  `<hr>` restyled as a coloured bar had a stripe drawn across it. The
  `color`, `noshade` and `size` attributes follow the rendering rules
  too: `size` is the rule's full height, including its borders.
* Style matching skips rules whose ancestors cannot be there. While the
  style pass walks the document it keeps a small counting filter of the
  tag names, ids and classes of the current element's ancestors, and a
  rule such as `ul li.menu a[href]` is dropped at once for a link with
  no `li.menu` above it instead of walking the ancestor chain. The style
  pass on a page of 16,000 elements and 3,000 rules takes 195 ms
  instead of 2 seconds. Rules inside `@scope`, and matching relative to
  a scope, still take the full path.
* The render pipeline's zoom factor scales every element's font size
  exactly once. Elements that share one computed style value -- five
  identical list items, say -- had that value multiplied once per
  element, so at 150% the fifth sibling's text came out 7.6 times too
  big. The pipeline is only driven at 100% today, so no page rendered
  differently yet.
* Numbering a long ordered list is linear again. Each marker counted
  every `<li>` before it, reading their `value` attributes, so a list of
  4,000 references cost 8 million sibling steps per layout with inside
  markers and again on every paint; a layout or paint pass now numbers a
  list's items in one sweep the first time a marker asks. Laying out
  4,000 items with `list-style-position: inside` takes 157 ms instead of
  253 ms.
* An image with a CSS `filter` is filtered once, not on every paint.
  The filtered copy was rebuilt from the full-resolution pixels each
  time the image was drawn -- once per `<img>` when a page repeats an
  image -- and is now kept with the decoded image until the filter
  changes. A page showing one 800x800 image forty times in grayscale
  renders in 2.0 s instead of 4.0 s, as fast as without the filter.
* The CSS `font-family` list of a text run is turned into a font name
  once rather than three or four times per run on every layout and
  paint. The answer is remembered per family list until the system font
  set changes or a web font finishes loading; laying out a page of 3,000
  paragraphs with long font stacks is 12% faster.
* Relayouts no longer redo work on unchanged style sheets. Each one
  re-resolved every `url()` in every cached sheet, copied every linked
  sheet to scan it for viewport media queries, and built the lookup key
  for the page's inline styles by copying all of their text; on a page
  with 1.5 MB of CSS that was 100 ms per relayout and is now 9 ms. The
  inline-style cache now also includes the page's base URL, so a page
  whose `<style>` text matches one visited earlier no longer loads
  `url()` images relative to the earlier page.
* A page that declares `container-type` but has no `@container` rules
  and no container units no longer styles itself twice on every layout.
  The second, container-aware style pass ran whenever any element was a
  container, and threw its result away; on a 12,000-element page with
  3,000 rules that cost 700 ms per relayout. It now runs only when a
  sheet has `@container` rules or a container unit was resolved, and a
  `@container` condition is parsed once per rule instead of once per
  rule and element.
* Removing or inserting children no longer counts the node's position
  among its siblings when no `Range` exists. The bookkeeping that keeps
  live ranges pointing at the right offsets measured the index of every
  moved node by walking its previous siblings, so emptying a 20,000-item
  list from the end took 13 seconds and 5,000 insertions into the middle
  of a 10,000-child element took 4; they now take 29 ms and 41 ms. Pages
  with live ranges keep the exact same range updates.
* Changing an element's `class` or `id` to a name no style sheet
  mentions no longer restyles everything inside it. Toggling a theme
  class on `<body>` that no selector uses re-ran the whole cascade --
  900 ms on a page of 12,000 elements and 3,000 rules -- and now leaves
  the styles alone (6 ms). Names that appear anywhere in a selector,
  including inside `:is()`, `:not()`, `:has()`, `:nth-child(... of S)`
  and `@scope`, or any `[class]`/`[id]` attribute selector or
  `:target`, still restyle as before.
* `getElementById` answers repeat lookups of a duplicated id at once.
  When two elements shared an id every call walked the whole document to
  find the first one, so 2,000 lookups on a 60,000-node page took 4
  seconds; the answer is now remembered until an element with that id is
  added, removed or renamed, and the same loop takes 4 ms. The tag and
  class indexes behind `getElementsByTagName`, `getElementsByClassName`
  and simple `querySelectorAll` calls no longer search a whole list to
  drop or place one element: a large list keeps a set of its members and
  is put back in document order the next time it is read.
* A style sheet pulled in with `@import` is parsed once, like a
  `<link>` sheet, instead of again on every layout. Every relayout
  re-parsed each imported sheet from its bytes, so a page that imports
  a 2,000-rule sheet and changes its DOM twenty times spent 4.6 seconds
  in the parser; it now takes 0.14 seconds, the same as linking the
  sheet. The cached sheet is keyed by its address, the layer it is
  imported into and the viewport, and is re-parsed when the fetched
  bytes change.
* `:nth-child()`, `:nth-last-child()`, `:nth-of-type()` and
  `:nth-last-of-type()` no longer slow down with the square of the list
  length. Each test counted the element's siblings from scratch, so a
  zebra-striped list of 40,000 rows took 14 seconds to style; the style
  pass now numbers all the children of a parent in one sweep the first
  time one of them is asked about, and the same page loads in about a
  second. `:nth-child(... of S)` still counts the matching siblings each
  time.
* A long descendant selector no longer hangs the browser on a deep page.
  Matching `.nomatch div div div span` retried every ancestor at every
  step, so the work grew with the depth of the tree raised to the number
  of compounds: 3 seconds for that selector over a 120-deep tree, over a
  minute with one more `div`. A step that has already searched every
  ancestor up to the root now tells the steps before it to stop, as
  other engines' selector checkers do, and the same selectors match in
  under a millisecond.
* Matching a selector against an element no longer looks up the
  element's namespace unless the selector names one, and a type selector
  compares the tag name against a lowercase copy made when the sheet is
  parsed instead of case-folding both names on every test. The style
  pass on a page of 16,000 elements and 3,000 rules takes a quarter less
  time, and a long descendant selector over a deep tree a third.
* Each `<style>` element is its own style sheet again. Adjacent inline
  sheets were joined into one text before parsing, so a sheet that ended
  inside an unclosed block, string or comment swallowed every sheet after
  it; such a sheet is now parsed on its own, where the end of the sheet
  closes whatever it left open. `<style type="text/foo">` and a
  `<link rel=stylesheet>` whose `type` is not CSS no longer apply, a
  `<link>` with the `disabled` attribute is not loaded, and
  `styleEl.disabled = true` or `sheet.disabled = true` switches a sheet
  off -- the getter was a stub that always said `false`.
* The `dir` attribute sets the CSS `direction`, as the HTML rendering
  rules map it. Only the text shaper read the attribute, so on a
  right-to-left page the words ran right to left but a table still put
  its first column on the left and a flex row still started at the left
  edge. `dir=rtl`, `dir=ltr`, `dir=auto` and `<bdi>` now reach the
  cascade through `:dir()`, and list markers sit on the right of a
  right-to-left list item instead of being painted off its left edge.
* Lists follow the HTML rendering rules. A nested `<ul>` draws a
  circle and the next level a square instead of a disc at every depth; a
  list inside a list has no block margins of its own; `<dir>` and `<menu>`
  are block lists indented 40px like `<ul>`; and `ul`/`li` `type="none"`
  hides the marker. The marker is chosen from the computed
  `list-style-type` rather than from whether the parent is an `<ol>`, so
  `ol { list-style-type: disc }` draws bullets. The 2px margin the
  default stylesheet put around every `<li>`, and the bold and extra top
  margin on every `<dt>`, are gone -- no browser has them.
* `width`, `height`, `hspace` and `vspace` attributes are read with the
  HTML rules for parsing dimension values. They went through `strtod`,
  so `width="+200"` and `width=".5"` produced a width where no browser
  gives one, and `width="20.25e2"` came out 2025px wide instead of
  20.25px. A table's or a cell's `width="0"` is now ignored, as the
  "ignoring zero" rule requires; `hspace`/`vspace` accept a percentage
  and reach `<embed>`, `<object>`, `<marquee>` and `<input type=image>`,
  not only `<img>`; and an image's `border` attribute is no longer capped
  at 100px and applies to `<object>` and image buttons too.
* A `min()`, `max()` or `clamp()` inside `calc()` resolves its
  percentages against the box it sits in. The nested function was
  reduced to a number up front, with any percentage taken of the window
  width, so `width: calc(min(100%, 800px))` in a 500px column came out
  800px wide. Additions and multiplications around the function, as in
  `calc(100% - min(2rem, 5%))`, are now folded into it instead.
* A declaration whose `var()` cannot be substituted -- the variable is
  undefined and there is no fallback, or what it holds does not parse
  for that property -- leaves the property `unset`, as the spec's
  "invalid at computed-value time" rule says, instead of vanishing and
  letting an earlier declaration win. `color: red` followed by
  `color: var(--undefined)` now inherits the parent's colour, as in
  every other browser, rather than staying red.
* A declaration that uses `var()` keeps its place among the other
  declarations of its rule. Such declarations are set aside until the
  element's custom properties are known, and were then ranked after
  every plain declaration of the rule, so
  `padding: var(--gap); padding-top: 3px` ended up with the variable's
  padding on top instead of 3px.
* An element's own `style` attribute outranks cascade layers, for
  `!important` declarations as well as normal ones. Layers were
  compared before the inline flag, so
  `@layer base { .x { color: red !important } }` beat
  `style="color: green !important"`.
* A translation that mixes a percentage with a length, such as
  `translateX(calc(-50% + 10px))`, moves by both. The percentage was
  dropped whenever a length was present, which left centred pop-ups and
  tooltips off by half their width. `em` and `rem` in `translate()`, its
  siblings and the `translate` property are measured against the
  element's own font size and the root's, not a fixed 16px, and a
  transition between a percentage and a length translation moves
  through both instead of treating the percentage as pixels.
* A transition between `transform: none` and a transform plays, and the
  element keeps its transform when it ends. Building the identity
  transform to animate from wrote zeros into the target value itself --
  the one the element's computed style holds -- so a hover that slid or
  scaled something from `none` left it where it was, for good.
* `text-shadow` is inherited, as CSS Text Decoration specifies, so a
  shadow set on a container reaches the text of the paragraphs, list
  items and inline-blocks inside it rather than only the container's
  own loose text. `orphans`, `widows` and `dominant-baseline` are
  inherited too, as their specs say.
* Every layer of a multi-image `background` in an external stylesheet
  is fetched relative to that stylesheet. Only the first `url()` was
  resolved against the sheet's address and the rest against the page,
  so the second image of `url(img/a.png), url(img/b.png)` in
  `/css/site.css` was requested from `/img/b.png`.
* `font-weight: bolder` and `lighter` are worked out from the parent's
  weight, using the table in CSS Fonts 4, when the style is computed.
  They were kept as keywords and measured later against a fixed 400
  with the old thresholds, so `bolder` inside bold text stayed at 700
  instead of 900, `lighter` inside bold fell to 100 instead of 400, and
  children inherited the keyword rather than the weight.
* `color: currentColor` takes the parent's colour, which is what the
  keyword means on the `color` property itself. It was left unresolved,
  so it was handed down as a word, getComputedStyle reported
  "currentcolor", and a border that takes its colour from the text was
  drawn black.
* `initial` on an inherited property means that property's initial
  value -- black text, a 16px font, normal weight and style,
  `line-height: normal` and so on -- instead of acting like `inherit`.
  The cascade stored nothing for the keyword, and an inherited property
  with nothing stored takes its parent's value, so `color: initial`
  inside red text stayed red and `all: initial` barely reset anything a
  reader could see.
* `rem` in the root element's own `font-size` is measured against the
  initial 16px, as the spec requires, instead of against the size it is
  in the middle of computing. `html { font-size: 1.25rem }` came out at
  25px rather than 20px, and a fluid
  `clamp(1rem, 0.9rem + 0.5vw, 1.25rem)` settled on the wrong size --
  which then scaled every other `rem` on the page with it.
* A percentage `line-height` is worked out once, on the element that
  sets it, and descendants inherit the resulting length. It was handed
  down as a percentage and each child resolved it again against its own
  font size, so a heading inside `font-size: 14px; line-height: 150%`
  got a 48px line instead of 21px -- the opposite of what the spec (and
  every browser) does, and the reason a percentage is not the same as a
  plain number there.
* A grid row with a fixed size keeps it. Rows declared as `20px` (or a
  resolvable percentage, or `minmax()` of two fixed sizes) grew to fit
  taller content like `auto` rows do, pushing every later row down;
  now the content overflows the row, and an item spanning a fixed row
  and an `auto` one grows only the `auto` row.
* Grid items given both a row and a column claim their cell before the
  automatically placed items flow in, as the grid placement algorithm
  orders it. An item pinned to row 1, column 1 that came later in the
  source used to land on top of whichever auto-placed item had already
  taken that cell.
* A `position: fixed` box inside a transformed element belongs to that
  element: it is placed against it and scrolls with it, as CSS
  Transforms says, instead of being pinned to the window. Slide-in menus
  and modals built inside a `transform`ed wrapper now open where the
  page puts them.
* `<center>` and `align="center"` (or `"right"`) line up the blocks
  inside them, not just images and tables: `<center><div
  style="width: 200px">` is centred, as is a table nested in a
  `<td align="center">`. A block with a margin of its own or an `auto`
  margin keeps the position those give it, and plain `text-align:
  center` still moves only inline content.
* A table's `width` includes its border and padding, as HTML's default
  stylesheet makes tables `box-sizing: border-box` and table layout now
  honours box-sizing. `<table style="width: 100%; border: 1px solid">`
  no longer sticks out of the page by its border, and a `width="600"`
  table is 600px wide overall.
* A percentage height inside a `box-sizing: border-box` parent is a
  share of that parent's content box. It was taken from the border-box
  height instead, so `height: 50%` inside a 100px-tall parent with 10px
  of padding came out 50px rather than 40px.
* An absolutely positioned box without `top` sits where it would have
  flowed, not at the bottom of its parent. Finding that static position
  only settled when the walk reached the next element after the box, by
  which point the whole parent had been counted, so a positioned first
  child of a 30px block landed 30px too low; and boxes inside earlier
  positioned boxes were counted as if they took up space in the flow.
* A child's bottom margin stays inside a parent it must not escape. It
  collapsed through any parent without bottom padding or border, so an
  `overflow: hidden` box, a float or an inline-block lost its last
  child's bottom margin from its own height, and a parent with a fixed
  `height` pushed the next block down by that margin as if it were its
  own. Margins now only collapse through a parent whose height is
  `auto` and whose `min-height` is zero, and never through one that
  starts a new formatting context or the root element -- so the
  document is as tall as the body's margins say, as in other browsers.
* An absolutely positioned box sized by its content is as wide as that
  content again: its own padding and border were being taken out of the
  measured width, so a box with `padding: 10px; border: 5px` around a
  100px child came out 70px wide inside. Its height now also honours
  `min-height` and `max-height` -- `top: 0; bottom: 0; max-height: 50px`
  was as tall as the containing block, and `height: 10px; min-height:
  60px` stayed 10px.
* Deeply nested flex rows lay out in a blink instead of seconds. Every
  row laid each item out once to measure it and again in place, and each
  of those layouts did the same for the row inside, so the work doubled
  with every level: 22 nested `display: flex` boxes took three and a
  half seconds. An item whose size did not change between the two passes
  is now moved into place rather than laid out again, and a row with a
  definite height hands its stretched items that height on the first
  pass, so the same page takes 0.2s. Moving a grid container now also
  moves its track positions, which absolutely positioned grid children
  are placed against.
* Shrink-to-fit boxes -- inline-blocks, floats, flex items sized by
  their content -- no longer come out wider than what they hold. A
  child with a pixel `width` and `box-sizing: border-box` was measured
  at its border-box width and then had its padding and border added a
  second time, and every float inside was counted as at least 60px
  wide, so an inline-block around a 16px floated icon was 60px wide.
* Floated columns with `box-sizing: border-box` sit side by side again.
  Deciding where a float fits counted its padding and border twice for
  a border-box width, so the Bootstrap 3 grid -- `*{box-sizing:
  border-box}` and two `float: left; width: 50%; padding: 0 15px`
  columns -- dropped its second column below the first.
* A flex item with a height of its own keeps it in a row. Stretching
  ignored whether the item's height was `auto`, so two 20px-tall items
  in a 100px-tall row both came out 100px tall -- and the second layout
  that stretch triggered sized them from their `width` again instead of
  the flexed width, so two `width: 200px` items squeezed into 300px
  overlapped. Only an item with an automatic height stretches now, the
  stretched height respects its `min-height` and `max-height`, and the
  flexed width survives the relayout.
* A multi-column block splits a list, not just a run of siblings. The
  column code distributed a container's own children and gave up when
  there were fewer than two, so `column-width` on a wrapper holding a
  single `<ol>` -- which is exactly how a Wikipedia reference list is
  built -- laid the whole list out in one column. A lone in-flow block
  child is now looked through and its children are distributed instead,
  and the balance point is measured from that child's content rather
  than the wrapper's outer height.
* A multi-column block establishes a block formatting context, as the
  spec says it does, so it sits beside a float instead of running
  underneath it.
* A block that establishes a formatting context is placed clear of every
  float it spans, not just the ones beside its top edge. It was narrowed
  against the float band at its first line and kept that width all the
  way down, so a taller float lower on the page -- a second stacked
  thumbnail, say -- ended up overlapping it. Where the finished box turns
  out to reach such a float it is laid out once more against the widest
  intrusion over its own height.
* `content: '[' / ''` renders just the bracket. The alternative text a
  `content` value may carry after a slash, for a screen reader to read in
  place of the glyphs, was being drawn as part of the text, so
  MediaWiki's section-edit links came out as `[/ edit ]/`.
* A list item styled `display: inline-block` or `display: block` no
  longer draws a bullet. Only a `list-item` display generates a marker;
  the painter went by the tag name, so an `<li>` that a page had made
  into something else -- a thumbnail in a MediaWiki gallery, say --
  carried a bullet no browser would show.
* A `<th>` no longer paints a grey background of its own, and a
  `<caption>` is no longer bold with padding under it. No browser's
  default stylesheet has either, so a table that sets its own colours --
  a Wikipedia infobox, for one -- showed its header cells in a shade the
  page never asked for.
* An inline-block whose width is a percentage no longer drags the
  intrinsic width of whatever contains it up to the width of the page. A
  percentage is indefinite while intrinsic sizes are being measured, but
  the atomic was laid out against the containing block anyway, so a table
  cell holding one reported a minimum as wide as the container and the
  table grew to match. It is measured against its own content instead.
* `align` and `valign` on a `<tr>`, `<thead>`, `<tbody>`, `<tfoot>`,
  `<col>` or `<colgroup>` now reach the cells, and a cell's
  `vertical-align` is inherited rather than pinned to `middle` by the
  default stylesheet, so a row can set the alignment for its cells the
  way the HTML rendering rules say it can. Cells still centre by default,
  because the row groups carry that default and the cells inherit it.
* `<figcaption>` is no longer italic and smaller than its figure, and a
  `<figure>`'s and a `<dl>`'s default margins match the HTML rendering
  rules. The italics in particular showed on every Wikipedia thumbnail
  caption.
* A style rule that names a pseudo-element is no longer thrown away
  because of what follows the pseudo-element's name. The selector parser
  treated any character after `::before` or `::after` as a syntax error,
  and the whole rule went with it -- so `li::after { ... }` survived only
  when the brace sat tight against the selector, and a selector list like
  `dd::after, li::after { ... }` never survived at all. That is how
  Wikipedia's horizontal lists are punctuated, so an infobox's platform
  list or a navbox's links ran together as one unbreakable word, which in
  turn forced the column measures wrong. Only something that really
  continues the compound selector is an error now; whitespace, a comma, a
  combinator and the block's brace merely end it.
* A table no longer squeezes its columns below the width their contents
  need. When a table asked for a width smaller than the sum of its
  columns' minimums -- an infobox at `width: 22em` whose labels do not
  fit, say -- every column was scaled down proportionally and the text
  ran out over the cell beside it. The table now grows to that sum, as
  CSS 2.1 requires.
* `min-width` and `max-width` on a table cell now take part in the
  column measures. The auto layout read only the cell's `width`, so a
  cell asking for `width: 50px; min-width: 150px` stayed at 50px and a
  `max-width` never clamped anything. A cell's contribution is now
  clamped the way every other box's is -- `max-width` first, then
  `min-width` -- in the min-content floor, the max-content measure and
  the specified width alike.
* A table cell inherits `text-align` from the table or the row again.
  The default stylesheet pinned `td, th` to `text-align: left`, which no
  browser's does, so `text-align: center` set on a `<table>` or a `<tr>`
  reached the caption and nothing else: a Wikipedia navbox title, a
  sidebar heading and every `align="center"`-era table layout came out
  left-aligned. `th` still centres on its own account.
* A table column with a specified width is no longer narrower than its
  cells need. The auto table layout took a cell's own `width` as that
  cell's minimum, so the width won outright: `width: 1%` on a heading
  cell -- the idiom Wikipedia's navboxes, and countless other tables,
  use to shrink a column to its label -- left the column one per cent of
  the table wide and its text ran across the cell beside it. A cell's
  floor is now its min-content width, measured without its own `width`,
  and a percentage `width` no longer counts as a definite minimum
  anywhere intrinsic sizes are measured, since it resolves against a
  basis that is not yet known.

1.0.9:
======
* Placeholder text in an `<input>` or `<textarea>` is no longer
  spell-checked. The layout already withheld spell ranges from a
  placeholder, but the painter's fallback underlined every run inside an
  editable host, so an empty search box showed a red squiggle under its
  own hint; the fallback now covers only `contenteditable` hosts, whose
  runs carry no explicit ranges. The README describes the engine thread
  in place of the removed renderer processes and request protocol, the
  typed `calc()` math, the Cache API and the site-partitioned cookie
  store, and its screenshot is recaptured from the 1.0.9 window.
* Text layout is ns-pango `3c6adba`, which merges upstream Pango 1.58.2.
  Of upstream's changes, four reach code the fork carries: an overline or
  strikethrough now spans the wider of a run's ink and logical extents,
  as the underline already did, so a decorated run whose glyphs are
  narrower than their advance no longer shows a shorter line above or
  through it than under it; a run with no font has its offsets zeroed
  rather than left uninitialised; the variant-to-feature mapping moves
  into shared helpers; and two zero-length memcpy and qsort calls that
  UBSan flags are guarded. The fork keeps its Ubuntu 24.04 dependency
  floors rather than upstream's new HarfBuzz 11 and fontconfig 2.17
  requirements, since nothing in the merged code needs them.
* Text layout is ns-pango `5a49882`, and a paragraph now comes out the
  same wherever it sits in its text. The fork's itemisation cache keyed a
  paragraph on its bytes and the layout's attribute list but not on the
  paragraph's offset, and attributes are ranges over the whole text: in a
  textarea, a `<pre>` or a pre-line paragraph whose second line repeated
  the first, a bold or a font on the first line was served to the second.
  Its shaping cache copied a font-feature range as an absolute offset into
  the paragraph, so a word shaped once under `font-feature-settings` on
  the word before it kept that shaping when it turned up elsewhere -- a
  kerning pair lost, or a ligature kept, on a word the feature never
  covered. Both keys carry what they lacked, and the fork's harness gains
  a `position` mode that fails on the old caches. The same pin fixes a
  heap overflow in the attribute-list deserialiser on a lone quote, two
  latent out-of-bounds reads inherited from upstream Pango, the fontmap
  serial not moving when a font file is added, and compares the item
  cache's attribute lists in linear rather than quadratic time. The fork
  now asks for stack protectors, stack-clash and control-flow protection
  and `_FORTIFY_SOURCE=3` by name, so a clang build carries them too, and
  runs its harness under AddressSanitizer and UBSan in its CI.
* Entering full screen now shows a notice. When the window goes full
  screen -- through the View menu, the shortcut or any other path -- a
  dark banner at the top of the page reads "<host> is now full screen.
  Press Esc to exit." for five seconds, and disappears again the moment
  the window leaves full screen. Without the banner a page could pass off
  its own drawing of an address bar and a sign-in form as the browser's
  chrome once the real toolbar was hidden (reported by Muhammad Wishal as
  fullscreen address-bar spoofing). `Element.requestFullscreen()` also
  now rejects with a `TypeError`, as the Fullscreen API specifies when
  `document.fullscreenEnabled` is false, instead of resolving as if the
  page had been granted full screen; page scripts cannot put the window
  into full screen in this edition and are no longer told otherwise.
* The address bar suggests as you type. A list drops down under the
  field with matching bookmarks, history entries ranked by visit count
  and recency, and a "Search for …" row that comes first when the text
  looks like a query and last when it looks like an address. Up and Down
  walk the list and fill the field with the highlighted address, Enter
  or a click opens it, and Escape or leaving the field closes the list.
  The history store gains a substring search over URLs and titles for
  this.
* The navigation toolbar is laid out like Mozilla 1.0 and Netscape
  Communicator. It sits on a bright silver-blue face with a white top
  highlight and a dark bottom groove and shows Back, Forward, Reload,
  Stop, Home, Print, Downloads and Bookmarks as icon-over-label buttons;
  they are flat until hovered, raise on hover and sink when pressed, and
  Stop stays in place greyed out while nothing is loading. The sunken
  location field with its page proxy icon takes the full width between
  the groove separators; the "Location:" label, the "Go" button and the
  toolbar grippy are gone, since Enter already goes and nothing was
  dragged. A Downloads button opens the downloads window from the
  toolbar. A new faceted printer icon and a downloads-tray icon join the
  icon set. The hamburger menu button carries a "Menu" label under its
  icon and keeps every window action with its shortcut, including Quit.
* The `about:start` splash is redrawn as a flat, sunny xkcd-style comic:
  black ink on white paper with only a few flat colours -- a yellow sun
  with wobbling rays, blue wave lines, a tan-planked ark and a red-banded
  lighthouse. The rain, lightning, umbrella, night sky and perspective
  road are gone; the animal pairs walk a plain ground line toward Noah in
  side view, and at the head of the queue two little browser windows on
  stick legs wait their turn beneath the speech bubble "Two of each. Yes,
  even browsers." A star pennant flutters on the ark's roof, Noah's wife
  waves from the deck, gulls circle the lighthouse, the keeper waves from
  the gallery and the dove carries its olive sprig. The 32 frames at 80 ms
  animate the sun's rays, drifting clouds, waves, the rocking ark, walking
  legs, hopping rabbits and kangaroos, the flapping dove and gulls and a
  whale surfacing to spout, on one 256-colour palette rendered at three
  times supersampling. `scripts/gen-splash.py` prefers its explicit font
  paths over `fc-match`, so the lettering stays comic on Windows. The
  README screenshot is updated with a full browser window capture,
  showing the navigation toolbar above the sunny about:start comic.
* The navigation toolbar is shorter. The icon-over-label buttons drop
  from 44 to 34 pixels with 20-pixel icons and a smaller label, the
  location field, Go button and throbber shrink to match, and the
  toolbar's own padding and margins tighten, so the whole bar is about a
  quarter less tall. The toolbar icons are redrawn in a style between the
  earlier flat gradient glyphs and the faceted 3D set: each keeps a
  single smooth gradient and a thin dark outline, and gains a soft drop
  shadow and a glossy top highlight, without the split facets and bevel
  edges. Stop is a red button with a white cross again, Reload a plain
  circular arrow, Home a blue-roofed house with door and windows, Print a
  printer with paper in and out, and Bookmarks a blue ribbon whose gold
  star fills in when the page is saved.
* `<textarea rows>` and `cols` are parsed as bounded non-negative
  integers (1 to 1000, defaulting to 2 and 20), so an attribute like
  `rows="2000000000"` no longer makes layout build billions of
  placeholder lines and exhaust memory.
* SVG rendering bounds the element tree it walks to 256 levels in every
  recursive pass -- rendering, `id` indexing for `url(#...)` references and
  locating the root of an SVG image -- so a document nested tens of
  thousands of `<g>` or `<div>` elements deep can no longer overflow the
  stack. Masks are budgeted at 256 MB of live surfaces across nesting.
  The SVG property resolver keeps its scratch buffers and the computed-style
  table in the per-render context instead of process-wide statics, so
  SVG images decoding on worker threads no longer race the main thread's
  inline SVG paint.
* XML internal entities expand to at most 1 MB per document; a nested
  entity chain (the "billion laughs" pattern) now makes the document
  not well-formed instead of exhausting memory.
* `<audio>` and `<video>` elements on `http(s)` documents can no longer
  name a `file:` URL, matching the rule the stream path already
  applied; a `file:` document may still play local media.
* Animation timelines: `currentTime` seeks on audio and animated images
  clamp non-finite and out-of-range values before converting to frame
  indices, per-frame delays are capped at ten minutes and total durations
  accumulate in 64 bits, so a crafted APNG or a huge `currentTime` cannot
  overflow the integer arithmetic.
* The JavaScript engine is quickjs-ng v0.17.0 (from v0.16.2), which adds
  `Iterator.zip`, `Array.fromAsync` and the upstream fixes of that
  release. The compatibility shims and the Windows link patch apply
  unchanged.
* The address bar shows the URL as serialised instead of percent-decoding
  it, and drops any `user:password@` part of the authority, so
  `https://bank.example%2Flogin@evil.example/` can no longer read as a
  page on the first host.
* Downloads triggered by `<a download>` require a recent user gesture, so
  a page cannot drop files into the download directory from a timer or
  from `click()`; names beginning with a dot or containing a path
  separator fall back to `download`.
* `navigator.clipboard.writeText()` and `execCommand("copy")` require a
  recent user gesture and reject with `NotAllowedError` otherwise, so a
  page cannot replace the clipboard contents while the user is elsewhere.
* The camera permission bar names the requesting origin, or "this page"
  when there is none, and never renders a raw `data:` or `blob:` URL in
  the prompt.
* After a crash or hang the supervisor restores the previous session once;
  if the restored session fails again before it has run for five minutes
  the next start is a clean one, so a page that wedges the browser cannot
  keep it in a restart loop.
* The in-process audio mixer accepts only `http(s):`, `data:` and `file:`
  URLs from the engine's side channel; anything else is refused instead
  of being treated as a local path. Side-channel headers (`X-Nav`,
  `X-Download`, `X-Audio`) that exceed their length limit are dropped
  whole rather than truncated to a different URL or command, and a queued
  audio command is only appended when it fits.
* Subresource redirects keep their initiator. A redirected image,
  stylesheet, script or fetch no longer loses its top-level document
  after the first hop; later hops keep the initiator's cookie and cache
  partition, the same-site cookie rule and the real `Sec-Fetch-Site`,
  `Sec-Fetch-Mode` and `Sec-Fetch-Dest` values instead of being treated as
  a fresh navigation, and the WebExtension request-block check runs on
  every hop.
* The `about:settings-save` and `about:settings-clear` endpoints require
  POST and read only the request body, a request without a top URL is
  trusted only when it is a navigation, and web pages can no longer
  navigate to `about:` pages other than `about:blank` and the start page.
  Home page and search engine URLs saved from `about:settings` must be
  http(s) (the home page may also be an `about:` page), free of control
  characters and at most 2048 bytes, and the cookie policy must be a known
  value.
* `document.cookie` refuses a name that collides with an `HttpOnly`
  cookie for the host in the site's network jar, so a server session
  cookie is never replaced by or sent alongside a script-set value.
* A `file:` page can still embed local images, scripts, stylesheets and
  frames, but `fetch()` and `XMLHttpRequest` receive only an opaque
  response for `file:` URLs, and directory listings are synthesized only
  for navigations.
* The Linux sandbox probes the Landlock ABI at startup, never grants
  symlink creation, grants truncate (ABI 3+) only where writes are
  allowed, and requests the link/rename right only on ABI 2+ so the
  ruleset also builds on older kernels. CSP scheme-only, `*` and host
  sources compare the parsed scheme, host and port of the resource URL
  and fail closed on a malformed URL; numeric render-IPC headers reject
  negative values.
* Option selectedness no longer rewrites the `selected` attribute.
  `option.selected`, `select.value`, `select.selectedIndex` and the
  dropdown and listbox picking paths change an internal selectedness flag
  (like checkbox checkedness) instead of the content attribute, so
  `defaultSelected` keeps reflecting the markup, `form.reset()` restores
  the default option, and `new Option(text, value, defaultSelected,
  selected)` maps its arguments correctly.
* Real clicks on checkboxes and radios follow the legacy-pre-activation
  rules: a mouse click toggles the control before the `click` event,
  reverts when the event is cancelled, and fires `input` and `change`
  afterwards, matching `element.click()`; clicking a `<label>` dispatches
  a synthetic `click` on its labeled control instead of toggling it
  silently, and a label whose `for` names a missing element no longer
  falls back to a descendant control.
* `<base href>` applies to images loaded through `img.src`, `new Image()`
  and srcset rescans and to `img.currentSrc`; `base.href` itself resolves
  against the document's fallback base URL.
* The `formdata` event fires when `new FormData(form)` is constructed and
  on every real form submission, so listeners can append fields, and the
  browser serializes that entry list. Real submissions validate like
  `checkValidity()`: `minlength` and `maxlength` only block after a user
  edit and `pattern` uses the JavaScript regex matcher.
* `range.value`, `stepUp()` and `stepDown()` produce the shortest
  round-trip decimal (`1234567`, `0.3`) instead of `1.23457e+06` or
  `0.10000000000000001`, and number and range sanitization enforce the
  HTML valid floating-point number grammar.
* `focus()` ignores elements that are not focusable areas; `select.add(el,
  index)` indexes across optgroups and inserts beside the reference;
  `img.complete` stays false while a `srcset`-only image is loading;
  `textarea.rows` reflects as a positive number with a fallback of 2; and
  the `form` attribute only selects a form owner while the control is
  connected.
* Canvases are origin-tainted. Drawing a cross-origin `<img>` (including
  one that redirected to another origin), a pattern made from one, an
  ImageBitmap created from one or an already tainted canvas clears the
  canvas's origin-clean flag, and `getImageData`, `toDataURL` and
  `toBlob` then throw a `SecurityError` DOMException instead of leaking
  the pixels; resizing the canvas restores the flag, and `data:`, `blob:`
  and same-origin images stay clean. `getImageData` checks its
  pixel-buffer size in 64-bit arithmetic before allocating.
* Cross-origin frames no longer expose their DOM. `iframe.contentDocument`
  and `getSVGDocument()` return `null` and `contentWindow` returns a
  restricted window proxy (`postMessage`, the `location` setter,
  `closed`, `length`, `window`/`self`/`frames`/`parent`/`top`/`opener`,
  `close`/`focus`/`blur`; everything else throws `SecurityError`) when the
  frame's origin differs from the embedding document or the frame is
  sandboxed without `allow-same-origin`. Same-origin, `about:blank` and
  `srcdoc` frames are unaffected.
* `postMessage` matches `targetOrigin` exactly: the value is parsed to
  scheme, host and port (a full URL collapses to its origin, the scheme
  is case-folded, default ports drop) and compared for equality with the
  target window's origin; `/` means the caller's own origin and `*`
  still matches everything.
* The CSS component-value parser bounds nesting at 512 levels and the
  `@container` condition parser at 32, so a style sheet or a
  `style.setProperty()` value made of hundreds of thousands of `(` no
  longer overflows the stack.
* An unterminated CSS string ends at the next newline as a bad-string
  token, as css-syntax-3 requires, so `content:"x;` only invalidates its
  own declaration instead of swallowing the rest of the block. `:not()`
  and `:has()` with an empty argument are selector parse errors and drop
  their rule; `p:not(){}` no longer matches every `p`. A pseudo-element
  ends its compound selector: `div::before span`, `::before.x` and
  `::before#id` are parse errors, while the user-action pseudo-classes
  (`::before:hover`, `::after:focus-visible`) still follow it.
* Custom properties are substituted at declaration time: with
  `.a{--x:var(--y);--y:red} .b{--y:blue}` a `.b` inside `.a` inherits the
  red `--x` computed on `.a`, and `getPropertyValue('--x')` returns `red`
  rather than `var(--y)`. Registered properties are type-checked on the
  substituted value. A `var()` cycle such as `--a:var(--b);--b:var(--a)`
  makes every member guaranteed-invalid so fallbacks apply instead of
  leaking the literal `var()` text once the depth cap was hit, and a
  declaration whose substitution leaves a top-level `!important` is
  dropped rather than kept with the keyword stripped.
* `color-mix()` scales the result's alpha by the percentage sum when
  both percentages are given and add up to less than 100%, and `in
  oklch` interpolates in polar form with the hue along the shorter arc.
* Shorthands reset the longhands they omit: `text-decoration` emits
  line, style and colour (so `text-decoration:underline` after
  `underline dotted red` renders solid in the current colour), `outline`
  and `column-rule` reset to medium/none/currentcolor, `flex-flow` to
  row/nowrap, `columns` to auto/auto and `border-block`/`border-inline`
  reset width, style and colour on both sides. A shorthand containing a
  token that does not parse is dropped whole -- `padding:10px auto` no
  longer sets the top and bottom. `outline-style: auto` is accepted.
* `-webkit-device-pixel-ratio`, `-webkit-min-device-pixel-ratio` and
  `-webkit-max-device-pixel-ratio` are aliases of `resolution` in dppx,
  and `@import` honours a `supports(...)` condition after its `layer()`
  part.
* `min()`, `max()` and `clamp()` keep `em`, `rem` and `lh` terms per
  argument and resolve them against the element at computed-value time
  instead of baking 16px and 19.2px at parse time, so
  `font-size:clamp(1.6rem,4vw,2.4rem)` under `html{font-size:62.5%}`
  clamps to 16-24px and `padding:max(1em,8px)` at 20px gives 20px.
* Grid containers no longer cap auto-placement at 24 rows: the occupancy
  map and row-size arrays grow with the content (up to 4096 implicit
  rows), so long grid lists lay out one item per row instead of piling
  everything past row 24 onto the last row.
* Floats are placed at their margin-box top; a float with `margin-top`
  previously landed a full margin lower than its siblings. Floats paint
  above the backgrounds of in-flow blocks that follow them, `z-index`
  applies to flex and grid items even when they are not positioned, and
  positioned boxes with a negative `z-index` paint immediately after the
  background of the stacking context that owns them.
* A block whose first in-flow child has a top margin grows its own top
  margin instead of keeping the collapsed margin inside its content box
  (CSS 2.2 section 8.3.1), so a background on the parent no longer shows a
  band above the first child and the collapse propagates through nested
  ancestors. The root element and flow-root, flex, grid, float and
  positioned boxes still keep their children's margins inside.
* Table column counting and width measurement account for cells occupied
  by a rowspan from an earlier row, a cell spanning several columns only
  widens them when they add up to less than its own width (distributing
  the excess proportionally), and `<col span width>` gives its width to
  each covered column.
* `text-align` no longer centres or right-aligns block-level tables,
  images, video or SVG; auto margins, the table `align` attribute and the
  legacy `<center>` / `align=` ancestry keep working. Wrapping flex
  containers honour `margin-left:auto` / `margin-right:auto` on their
  items, replaced elements respect `box-sizing:border-box`, and
  `position:relative` offsets accept `calc()`, math functions and every
  length unit.
* The `:checked` selector on options and the listbox highlight read the
  same selectedness the DOM uses, so an option chosen by script in a
  `<select multiple>` matches `:checked` and paints selected.
* Continuous integration builds with the optional Ogg Opus/Vorbis and
  Enchant libraries on every platform, so the in-process Ogg decode and
  spell-checking paths compile on Linux, Alpine, macOS and Windows; the
  Linux job also builds on Ubuntu 26.04, the Windows job uploads its
  binary, and Dependabot keeps the GitHub Actions versions current.
* The shell calls the page engine directly. The internal HTTP/JSON
  request protocol that carried every page load, event and frame over a
  socketpair between two halves of the same process is gone, together
  with the fork-and-exec spawners for a renderer executable that was
  never built and the Task Manager that listed this one process. The
  engine now runs on a dedicated thread with its own GLib main context,
  so page timers, fetch completions and settle loops no longer share the
  GTK main loop and the window stays responsive while a page loads;
  audio, navigation, camera and download signals come back as plain
  fields of a rendered frame instead of length-capped headers.
* The `about:start` splash animation and the logo are read from
  `share/northstar/splash.gif` and the installed `northstar.gif` instead
  of being compiled in as base64 arrays, which takes 660 KB out of the
  binary and a second copy off the heap on every start page; the
  unreferenced `data/splash.png` is gone and `scripts/gen-splash.py`
  writes `data/splash.gif` directly.
* Dead JavaScript bindings are gone: the C `Element.animate()` that
  resolved every animation as finished before the polyfill replaced it,
  the fake `getSelection()` and `Range` stubs whose twenty methods did
  nothing, and the polyfill copies of `URLSearchParams`,
  `AbortController`, `AbortSignal`, `XMLSerializer` and
  `structuredClone` that never ran because the native binding wins; the
  polyfill verifier no longer checks for those natively provided
  constructors. Three unreferenced engine functions are removed with
  them.
* `document.cookie` and network requests share one cookie store per
  site partition, owned by libcurl. Each partition has a cookie-sharing
  libcurl share and a holder handle that loads and writes the Netscape
  jar file; requests attach to that share instead of loading and
  rewriting the file per handle, script writes go in through
  `CURLOPT_COOKIELIST` and reads come out of `CURLINFO_COOKIELIST`. The
  separate `.js.txt` sidecar jar, the hand-written file parser and
  rewriter and the clobbering race between concurrent handles that
  motivated the sidecar are gone; an existing sidecar is folded into the
  jar on first use. Scripts still cannot read or overwrite `HttpOnly`
  cookies.
* One `calc()` evaluator. The CSS engine's math parser now carries a
  value kind (number, length, angle, time, resolution, flex) through
  every operation and function, so the five private evaluators that
  colour channels, `image-set()` resolutions, time properties, media
  queries and `sin()`/`cos()`/`tan()` arguments each kept are deleted,
  along with the text rewriters that turned angle and time units into
  bare numbers. `atan()`, `asin()`, `acos()` and `atan2()` now yield
  angles, so `rotate(atan(1))` rotates by 45deg instead of being
  dropped, `min()`, `max()` and `clamp()` compare angles, times and
  resolutions, and a length property no longer accepts `calc(10deg)`
  as zero.
* The Cache API is real. `caches.open()`, `has()`, `delete()`, `keys()`
  and `match()` and the `Cache` methods `match()`, `matchAll()`,
  `add()`, `addAll()`, `put()`, `delete()` and `keys()` store request
  and response pairs per site partition in IndexedDB and follow the
  Service Workers specification: URL fragments are ignored,
  `ignoreSearch`, `ignoreMethod`, `ignoreVary` and `cacheName` are
  honoured, `Vary` headers are matched against the stored request,
  a non-GET request, a non-http(s) URL, a 206 response, a `Vary: *`
  response or a used body rejects with a `TypeError`, and `addAll()`
  stores nothing when any fetch fails. The same storage is visible from
  a page and its service worker, so a worker that fills a cache at
  install time serves it offline. Until now every method resolved with
  nothing.
* Style recalculation reads each element's selector keys once. To find
  the rules that might match an element, the cascade looks it up in
  every stylesheet's index by id, by each of its classes, by tag name
  and by each attribute name -- and it re-derived those keys for every
  sheet it consulted, so an element was scanned for its `id` and `class`
  attributes, had its class list re-split into tokens with a copy per
  token, and had every attribute name measured and lower-cased once per
  stylesheet rather than once. The keys are now built a single time per
  element and handed to each sheet, the class tokens come from the
  parsed class set the element already caches for `ns_node_has_class`,
  and the tag and attribute names are folded to lower case once instead
  of at every lookup. Nothing about which rules match changed; the same
  page simply reaches the same answer with a fraction of the string
  work, which is most of what style recalculation was doing on a large
  document.
* Looking an element up by id no longer walks the document when there is
  no such element. The document keeps an id index, but a lookup that
  missed fell through to a full depth-first walk, because nothing
  recorded whether the index could be trusted to be complete. Every read
  of a global that a page has not defined goes through this path -- the
  window object consults its named properties whenever ordinary property
  lookup fails -- so a single `typeof someGlobal` cost a walk of every
  element in the document, and feature detection in a loop cost one per
  test. Writing an `id` attribute now updates the index from the one
  place every id write passes through, rather than from the scripting
  layer alone, so the index holds every id in the document and a miss
  can answer immediately -- the same trust the document's tag index
  already enjoys. A lookup scoped to a subtree, such as one inside a
  shadow root whose ids the document index deliberately does not hold,
  and a duplicated id both still take the walk. Three thousand reads of
  an undefined global on a document of eight thousand elements fall from
  420 ms to 0.5 ms.
* Declaring a transition no longer costs a style recalculation every
  frame. The animation layer keeps a record for each element that names
  a `transition` or an `animation`, and creating that record barred the
  element from reusing its computed style -- necessarily, because while
  an animation runs it writes interpolated values straight into that
  style, and reusing it next frame would hand the cascade a value it
  never computed. But the bar was raised as soon as the record existed,
  whether or not anything was animating, and a barred element drags its
  whole subtree with it. On a page whose stylesheet puts a `transition`
  on most of its elements -- which is to say a modern page -- that
  turned an incremental restyle back into a full one: on the Speedometer
  3.1 TodoMVC pages roughly two thirds of the document was recomputed on
  every frame with nothing on the page having changed. The bar is now
  raised and lowered by the code that actually writes the values, so it
  covers exactly the elements that are mid-animation, and an element is
  recomputed once more as its animation ends so it never keeps an
  interpolated value. A declared transition that is not running now
  costs nothing.

1.0.8:
======
* When several options of a single-choice `<select>` carry the
  `selected` attribute, the last one wins, as the HTML selectedness
  setting algorithm requires; the first used to win, so `<option
  selected>` appended after another selected option (by the parser,
  `appendChild` or `innerHTML`) did not become the value.
* `HTMLOptionsCollection` exposes `selectedIndex`, and every event the
  engine dispatches carries a `composed` flag (true for the UI event
  types, false otherwise) instead of leaving the property undefined on a
  `change` event.
* `transform` is validated function by function against css-transforms:
  each function checks its argument count and types, so `translate(1px,
  2px, 3px)`, `scale(6, 7, 8)`, `skewX(0, 0)` and `translateX(3%) none`
  are rejected, and the specified value serialises canonically --
  percentages in `scale()` become numbers, `rotate(0)` reads
  `rotate(0deg)`, `0` lengths read `0px`, and function names other than
  the `translate` family are lowercased. The `scale`, `rotate` and
  `translate` properties get the same treatment: `scale: 100% 100%` reads
  `1`, `translate: 100px 0px` reads `100px`, `rotate: 400grad x` and
  `rotate: 0.5 0 0 400grad` are accepted and read `x 400grad`, and a
  negative axis vector folds its sign into the angle. `transform-origin`
  and `perspective-origin` follow the position grammar (`top center`
  reads `center top`, `1px left` and `top bottom` are rejected,
  `perspective-origin` takes the edge-offset form `right 20px bottom
  30px`), `perspective: 1000` without a unit is rejected, and
  `transform-box` is a property. WPT css/css-transforms/parsing: 190 ->
  318 of 336.
* `border-image` is implemented. The five longhands (`border-image-source`,
  `-slice`, `-width`, `-outset`, `-repeat`) parse and validate against the
  css-backgrounds grammar, the shorthand splits `source || slice [ / width
  | / width? / outset ] || repeat` and resets what it leaves out, and
  `border` resets all five as the specification requires. Paint draws the
  nine-slice border from a `url()` image, which the loader now fetches like
  a background image, or from a gradient rendered to the border area, with
  `stretch`, `repeat`, `round` and `space` edges, `fill` for the middle and
  `outset` enlarging the area; `border-image: linear-gradient(...) 1`, the
  common gradient-border idiom, now shows a gradient frame.
* `-webkit-border-radius` and the `-webkit-border-*-radius` corners are
  aliases of the unprefixed properties, `border-radius` rejects a fifth
  value, a negative radius or a second slash, a corner rejects a third
  value, and the shorthand's specified value collapses each half as a
  quad (`1px 1px 1px 2% / 1px 2% 1px 2%` reads `1px 1px 1px 2% / 1px 2%`).
  `em` and `rem` corner radii written as `h / v` pairs resolve against the
  font size in the computed style.
* The CSSOM rebuilds `style.border`, `style.borderTop` and the other
  sides, `style.borderRadius` and `style.borderImage` from their
  longhands, so `border: 1px solid #fff` reads back
  `1px solid rgb(255, 255, 255)` and `border-top: 2px` after `border:
  1px` reads `2px 1px 1px` through `border-width`. `cssText` prefers the
  `border` shorthand when its seventeen longhands agree, then the
  `border-width`/`-style`/`-color` quads, then a side, then
  `border-image` and `border-radius`, following the CSSOM
  serialization order; `style.length` counts every longhand a `border`
  shorthand sets. The `border` shorthand rejects `auto`, a second width
  or a negative length, and a shorthand that carries `var()` is kept
  whole rather than expanded into guessed longhands. `getComputedStyle`
  resolves `border` and the side shorthands from their computed
  longhands instead of echoing the specified text.
* `background-position-x`/`-y` accept `x-start`, `x-end`, `y-start`,
  `y-end` and an edge with an offset (`right 10px`, `top -20%`).
* A colour in the specified style serialises the way CSS Color 4
  requires: a keyword keeps its spelling in lowercase (`ActiveText` reads
  back `activetext`), and a legacy `#hex`, `rgb()`, `hsl()` or `hwb()`
  value reads back as `rgb()`/`rgba()`. The deprecated CSS2 system
  colours (`Menu`, `ButtonShadow`, `ThreeDFace`, `WindowFrame` and the
  rest) map to their CSS Color 4 replacements instead of being invalid.
  A comma-separated layer list with one invalid layer is now rejected as a
  whole, as the grammar requires, rather than keeping the valid layers.
* The `background` shorthand is parsed layer by layer against the
  css-backgrounds grammar. Every comma-separated layer sets all eight
  longhands -- image, position, size after the slash, repeat, attachment,
  origin and clip, with the colour on the final layer -- and a longhand the
  layer leaves out resets to its initial value, so `background: red` no
  longer keeps an earlier `background-image`. The old parser read one
  layer, dropped the origin, clip and attachment keywords and did not know
  `background-attachment` at all; that property now exists, and
  `background-clip`, `background-origin` and `background-attachment` take
  a comma-separated list like the other layered longhands. Paint resolves
  origin and clip per layer, clips the colour by the last layer's clip,
  and positions a `background-attachment: fixed` layer against the
  viewport. `background-position` keeps the keywords it was written with
  (`left top`, `center center`) in the specified style, `background-repeat`
  rejects a third keyword, and `background-clip` accepts `text`,
  `border-area` and `border-area text`. `style.background` and
  `cssText` rebuild the shorthand from the longhands in canonical order,
  omitting initial values, and `style.length` counts the nine longhands
  a shorthand sets. WPT css/css-backgrounds parsing/background-*: 168 ->
  330 of 350 subtests.
* The CSSOM keeps the keyword a shorthand was written with: after
  `border-color: red yellow` or `border: thin dotted blue`,
  `style.borderTopColor` reads `red` and `style.borderTopWidth` reads
  `thin` rather than `rgb(255, 0, 0)` and `1px`, and a multi-layer value
  such as `background-image: url(a), url(b)` serialises every layer
  instead of the first. Setting or removing a property now follows the
  CSSOM algorithm for overlapping declarations: a shorthand replaces the
  longhands and shorthands it covers, and clearing one longhand of a
  stored shorthand expands only that shorthand, so `style.length` goes
  back to zero once every longhand of `border-color` is removed and
  `style.border` no longer leaves an earlier `border-width` behind. WPT
  css/css-backgrounds parsing/border-*-shorthand: 27 -> 89 of 96.
* `box-shadow` and `text-shadow` serialise their specified value in
  canonical order (colour, offsets, blur, spread, `inset`) with `0`
  written as `0px`, and reject the invalid forms the grammar excludes:
  a lone length, a fifth length, two colours, `inset` twice, a negative
  blur, a percentage, or a colour splitting the lengths. The parser now
  keeps a `calc()` or `em` length until the computed style resolves it
  against the element's font size, and a shadow without a colour takes
  `currentcolor` from the computed `color` instead of a fixed
  half-opaque black. `rgb(0, 255, 0)` and other colours with spaces
  inside the parentheses were split into separate tokens and lost. WPT
  css/css-backgrounds parsing/box-shadow-*: 25 -> 82 of 82.
* lexbor is v3.0.1, up from v3.0.0. The release fixes the URL parser's
  buffer growth: a component longer than its stack buffer was copied into
  fresh heap storage without the bytes already written, and the list of
  search parameters kept a stale tail pointer, so a long path, query or
  host could lose its prefix on the way through lxb_url_parse. IDNA
  ASCII conversion had the same missing copy, an unfinished :contains()
  selector leaked and could be freed twice, and the multi-byte decoders
  returned a short-buffer status rather than writing past the end of a
  full output buffer. quickjs-ng stays at v0.16.2; the documentation now
  says so.
* A table's max-content and min-content widths are measured column by
  column, as css-tables-3 §4.4 requires: each column takes the widest
  cell it holds (or the cell's specified width, floored at its
  min-content), the columns are summed once with the border spacing, and
  captions widen the result. They used to be the sum of every row, so a
  table nested in a cell, a floated infobox with multi-paragraph cells,
  and a table inside a flex or grid item reported several times their
  real width; cells and captions now measure like blocks (widest child)
  instead of summing their children. WPT css/css-tables: 330 -> 334 of
  787 on a 2026-09 checkout.
* position: fixed elements stay anchored to the viewport while the page
  scrolls. Layout placed them against the initial containing block and
  nothing translated them by the scroll offset, so a fixed header, cookie
  bar or modal overlay scrolled away with the document. The paint walk
  now offsets a fixed box by the viewport origin (and culls its subtree
  against the offset bounds), hit-testing applies the same offset so
  clicks land on the fixed element, and mouse events carry
  viewport-relative clientX/clientY with document coordinates in
  pageX/pageY.
* position: sticky boxes are hit-tested where they are painted: clicking
  a stuck header or navigation bar used to fall through to whatever
  lay beneath it. One shared ns_box_sticky_offset serves paint, hit
  testing and getBoundingClientRect; it resolves percentage and calc()
  insets against the scrollport, and measures a sticky box inside an
  overflow container against that container's padding box rather than
  the cairo clip, so a partly scrolled-out scroller no longer drags its
  sticky children to the viewport edge.
* Fixed a use-after-free in the MutationObserver delivery loop: the job
  queued raw observer pointers, so a callback that disconnected and
  dropped a later observer left the loop reading and calling through
  freed memory. The queue now holds a reference on each observer's
  wrapper for the duration of the drain, and slots waiting for a
  slotchange event are scrubbed when their node is destroyed by an
  earlier listener rather than dispatched to a dangling node.
* OfflineAudioContext.startRendering() bounds the graph walk to 4096
  node renders; a page that wired a node into its own inputs several
  times over could make the recursion exponential and hang the browser.
* docs/cve-2026-85046.md records why the actively exploited V8 JIT type
  confusion CVE-2026-85046 does not apply to Northstar: the engine is the
  quickjs-ng interpreter with no optimizing compiler, no per-array
  elements kind, no write barriers, and a generic Array.prototype.sort
  that stores back through the checked property path.
* Raw pointers into page-owned ArrayBuffers are no longer held across a
  call back into JavaScript. putImageData, AnalyserNode's byte getters and
  the AudioBufferSourceNode renderer read the page's properties first and
  fetch the backing bytes last, so a getter that transfers or resizes the
  buffer meanwhile cannot leave the engine writing through a freed
  pointer (the type-confusion class behind CVE-2026-85046). WebAssembly
  externref boxes release their JavaScript value through the runtime
  rather than the context they were created in.
* Flex layout resolves flexible lengths the way css-flexbox-1 §9.7
  describes: one implementation shared by row, wrapping-row and column
  containers distributes free space with the item freezing loop, so flex
  factors below one scale the free space, flex-shrink is weighted by the
  flex base size, and min/max violations are frozen and re-distributed.
  The automatic minimum size of a flex item is min(content size,
  specified size) rather than the specified size, so width: 200px in a
  100px container shrinks as browsers do; min-content and max-content
  minimums are honoured, and a percentage size on a replaced element or
  text control counts as zero for the specified size suggestion.
* Column flex containers wrap: flex-wrap: wrap and wrap-reverse break
  items into lines against the definite main size, align-content places
  the lines (start/end resolve in the inline axis; space-around and
  space-evenly fall back to start when the lines overflow), a wrapping
  container with a single line is still multi-line, rtl mirrors the
  cross axis, column-reverse packs from the main end, auto margins in
  the main axis absorb free space, and an indefinite-height column sizes
  itself from its items' content contributions so flex: 1 items no
  longer collapse.
* Negative free space overflows in the right direction: space-between,
  space-around and space-evenly fall back to start, flex-end and center
  overflow the start edge, and a scroll container packs overflowing
  content toward its start so it stays reachable; row wrap-reverse
  mirrors lines against the container's definite height.
* scrollWidth and scrollHeight include the scroll container's end
  padding and, in rtl, overflow to the left. offsetTop and offsetLeft
  round negative values to nearest and flush pending layout before
  locating the offset parent.
* The static position of an absolutely positioned flex child honours
  start, end, left and right on justify-content and align-self as
  writing-mode-relative keywords, self-start/self-end use the child's
  own direction, and last baseline aligns to the cross end.
* align-items, align-self, align-content, justify-content, justify-items
  and justify-self parse first baseline, last baseline and the safe and
  unsafe prefixes.
* WPT css/css-flexbox: 1465 -> 1997 of 3670 subtests on a 2026-09
  checkout of the horizontal-writing-mode suites.
* An absolutely positioned box whose containing block is a grid
  container takes that block from its grid-column and grid-row lines,
  as css-grid-1 §9 requires: a line inside the explicit grid resolves to
  the edge of the adjacent track, a line outside it, an unknown name or
  a span resolves to the padding edge after the start/end swap, and
  offsets, percentages and shrink-to-fit sizes resolve against that
  area. A shrink-to-fit abspos box no longer squeezes below its
  min-content width when the area is narrower than its content.
* align-content: stretch on a grid container distributes free block
  space only to rows whose max track size is auto; fixed-length rows
  kept their length.
* An absolutely positioned element with an inline-level display that
  follows inline content takes its static position from the line it
  would have occupied, after the preceding text, instead of the top of
  the block.
* document.fonts.ready waits for the web fonts the page needs: it
  flushes style so pending @font-face loads are requested, resolves
  once the loader is idle and marks the document for relayout;
  fonts.status reports loading meanwhile.
* Alignment properties keep their full specified keyword: safe and
  unsafe prefixes, legacy left/center/right, first baseline (computed
  as baseline) and last baseline parse and serialize, and the
  place-self, place-items and place-content shorthands split two-word
  values and serialize a repeated value once.
* offsetTop/offsetLeft flush pending layout before locating the offset
  parent, so a first read during parsing no longer returns viewport
  coordinates.
* Grid containers with direction: rtl lay their columns out from the
  right, and grid-placed absolutely positioned boxes mirror with them.
* repeat(auto-fit, ...) collapses the repeated tracks that no in-flow
  item occupies, so a card grid with fewer cards than columns stretches
  the remaining fr tracks as browsers do.
* A grid item's percentage height resolves against its grid area when
  the rows it spans have definite track sizes, and an absolutely
  positioned child of a grid container with auto offsets takes its
  static position from its grid area, aligned by justify-self and
  align-self.
* getComputedStyle on a grid container returns the used track sizes for
  grid-template-columns and grid-template-rows, with explicit line names
  in place, as CSSOM requires.
* Track lists resolve em against the element's own font size and calc()
  percentages against the track axis; an auto track grows to its
  max-content contribution before fr tracks share the remainder, fr rows
  fill a definite container height, over-constrained minmax() rows
  shrink toward their minimum, percentage rows in an indefinite-height
  grid re-resolve against the final height, and an auto column measures
  its content with real text metrics.
* The grid track-list parser rejects negative sizes, stray commas,
  consecutive or trailing-only line-name lists, reserved words as line
  names, a second auto-repeat and non-fixed tracks beside one, and
  supports line names inside repeat().
* Fixed a hang in the CSS parser on an at-rule inside a declaration
  list (div { @foo {} color: green }), in stylesheets and inline styles
  alike; the at-rule is skipped as CSS Syntax requires.
* font-family values are canonicalized: quoted strings and ident
  sequences are told apart, ident-like strings are unquoted, generic
  families are lowercased and reserved single idents are rejected. The
  font shorthand is validated against its full grammar before any
  longhand is written, resets the longhands it does not mention, accepts
  math functions for the size and line-height, expands the system font
  keywords, and rolls back entirely on an invalid family;
  getComputedStyle().font is composed from the longhands.
* "prop in element.style" is true only for supported properties, and a
  longhand written after a later shorthand that covers it lands after
  the shorthand so it wins.
* Grid items honour justify-self and align-self self-start, self-end,
  start, end, left and right with the grid's and the item's direction,
  minmax(auto, X) tracks take their intrinsic minimum, min-width and
  max-width keywords (min-content, max-content, fit-content) resolve on
  grid items, an explicit-width item under justify-self normal is
  start-aligned in its area (right edge in rtl) and "safe" alignment
  falls back to start when the item overflows, and aspect-ratio sizes
  replaced elements.
* Gradients are parsed against the CSS Images 4 grammar: to-side and
  corner directions (the corner angle follows the box), radial shapes,
  size keywords and explicit radii, conic from-angles, positions in all
  forms, colour interpolation methods ("in oklch longer hue"), two-
  position colour stops and interpolation hints. Invalid preludes,
  empty arguments and stray hints are rejected. Specified and computed
  values serialize canonically, ellipse gradients paint as ellipses and
  closest/farthest side and corner sizes are honoured.
* background-position and object-position validate the position
  grammar (3-value syntax only for background-position) and serialize
  horizontal-first with a single keyword expanded to its pair.
* scrollWidth and scrollHeight extend the in-flow content's margin
  boxes by the container's end padding instead of every descendant's
  border box, so negative margins and margins that collapse through
  the container report no phantom overflow; in rtl and in the flipped
  cross axis of wrap-reverse the region is measured from the far edge.
  Block children in rtl containers that over-constrain the line keep
  their right margin and overflow to the left.
* getComputedStyle exposes -webkit- prefixed aliases through their
  camelCase names (webkitAppearance).
* The content property is validated against its grammar (strings,
  quotes, images, attr(), counter()/counters() with symbols(), an
  alt-text list after a slash) and serializes canonically: double-quoted
  strings, decimal counter styles omitted, the symbolic system dropped.
* Container queries evaluate the full condition grammar: not/and/or
  with nesting, size features in plain, boolean and range form
  (double-sided ranges and math functions included), aspect-ratio and
  orientation, unknown features that make the enclosing condition
  false, comma-separated condition lists, name-only rules and vertical
  writing-mode containers. Invalid preludes are dropped from the engine
  and the CSSOM, conditionText serializes canonically, container-name
  and the container shorthand validate their values, and
  CSSContainerRule exposes containerName, containerQuery and
  conditions. In headless mode a style flush after a mutation performs
  the container-aware relayout so getComputedStyle sees query results.
* image-set() and -webkit-image-set() are validated option by option
  (images, x/dppx/dpi/dpcm resolutions or math functions resolving to a
  resolution, type()) and serialize canonically in specified and
  computed style; unicode-range descriptors are validated per CSS
  Syntax 3 and serialize as uppercase U+XXXX or U+XXXX-YYYY.
* JavaScript can be turned off: an "Enable JavaScript" toggle in
  Settings parses pages with scripting disabled (so noscript content
  renders) and skips script execution entirely — the user decides what
  runs, in the old Mozilla tradition.
* about:mozilla shows the maroon page every browser of this lineage
  owes its readers, and about:config opens the settings page.
* The status bar reads "Done" for a moment when a page finishes
  loading, as it always did.
* Fixed a use-after-free of the session URL: timer, event-dispatch and
  requestAnimationFrame callbacks saved the current URL pointer and restored
  it unconditionally after the callback, so a handler that navigated (a
  fragment click, location.hash =, history.pushState) left the engine
  reading and double-freeing a freed URL. The URL is now restored only when
  it was actually swapped for an iframe realm.
* Added view-source: — Ctrl+U / "Page Source" in the menu shows the current
  page's HTML with classic syntax highlighting. Only chrome-initiated
  navigations can use the scheme; web content is refused.
* input.showPicker() and select.showPicker() are implemented per spec:
  InvalidStateError on disabled or readonly controls, NotAllowedError
  without a user gesture, and a successful call consumes the activation.
  WPT show-picker suites pass 129/129.
* stepUp()/stepDown() follow the spec: they throw InvalidStateError on
  non-numeric input types and step="any", honor the per-type default
  step and scale, round to the step grid, clamp to min/max, and
  serialize date, month, week, time and datetime-local values back to
  their canonical strings. WPT input-stepup 53/53 and time 32/32.
* The color input sanitizes through the CSS color parser: keywords,
  rgb() and #rgb shorthand normalize to lowercase six-digit hex, and
  surrounding whitespace is stripped.
* Label association follows the spec: label.control resolves the for
  attribute against the label's own tree to the first element with that
  id (null when it is not labelable or the attribute is empty),
  label.form returns the associated control's form owner, and .labels
  is a live NodeList.
* The WPT harness's testdriver bridge grants real user activation for
  simulated gestures, and the __nsWpt* hooks are inert outside the
  harness.
* Form constraint validation follows the HTML spec much more closely:
  ValidityState flags are computed for disabled and readonly controls
  (bars from validation affect willValidate, not the flags), valueMissing
  is suppressed on disabled/readonly controls, the pattern attribute
  compiles as a JavaScript regular expression with the v flag (invalid
  patterns are ignored) and applies to each address of a multiple email
  input, tooLong/tooShort fire only after a user edit as the spec's dirty
  value flag requires, and willValidate is false inside a datalist.
  WPT form-validation: patternMismatch, tooLong, tooShort, typeMismatch
  and badInput suites now fully pass.
* Live HTMLCollection/NodeList property semantics follow WebIDL: silent
  sloppy-mode failures and strict TypeErrors for read-only indexed and
  named properties, spec-compliant descriptors, expando support, and
  Object.keys listing only indices; moveBefore() is ParentNode-only;
  replaceChildren() queues a single mutation record; Node.isConnected is
  true for any node whose root is a document.
* document.getElementById respects shadow boundaries and duplicate-id
  document order after moveBefore/append reorderings.
* Popup blocking, in the Firefox 1 tradition: window.open only navigates
  when called within five seconds of a real user gesture (click, key
  press, touch), consumes that activation, and logs blocked attempts to
  the console. navigator.userActivation now reports the live activation
  state instead of constants.
* Ctrl+Enter in the address bar completes a bare name to www.name.com,
  Shift+Enter to .net and Ctrl+Shift+Enter to .org, as in classic
  Firefox.
* Ctrl+Shift+R and Ctrl+F5 reload the page bypassing the HTTP cache.
* Ctrl+D bookmarks the current page, with a matching "Bookmark This Page"
  menu entry, in the classic browser tradition.
* about:book — every browser of the lineage carries its Book.
* Updated the ns-pango subproject pin to the latest upstream commit.
* Optimized event dispatch throughput by lazily evaluating composedPath()
  arrays on demand rather than eagerly creating them on every event dispatch.
* Added node-level listener filtering with NS_NODE_HAS_LISTENERS flag, skipping
  listener array iterations on intermediate DOM tree nodes without listeners.
* Bound standard Event prototype methods (preventDefault, stopPropagation,
  stopImmediatePropagation, cancelBubble, composedPath) directly on Event.prototype.
* Added instant early-return checks to MutationObserver record dispatch when no
  observers are active, bypassing unnecessary node invalidations.
* Accelerated document.createElement with lowercase ASCII fast paths and
  ASCII-first element name validation.
* Optimized inline style property conversions (camel_to_kebab) and empty initial
  inline style updates avoiding intermediate GString allocations.
* Optimized dataset property lookups by matching target attribute names directly
  without repeated per-attribute string allocations.
* Bypassed redundant storage allocations and event dispatches when setting
  identical localStorage/sessionStorage values.
* Added HTMLDialogElement.showModal() standards compliance validating open and
  connected document state.
* Aligned structuredClone with specification requirements (argument validation
  and transfer options support).
* Optimized CSS.escape for simple identifiers and bounded selector and form
  ancestor traversals.
* Optimized DOM textContent and innerText get/set paths with zero-allocation
  string returns for empty and single text child nodes.
* Optimized DOMTokenList (classList) add, remove, and toggle with single-token
  fast paths avoiding token array and parser allocations.
* Accelerated DOM hierarchy validation in pre-insert checks and ancestor-or-self
  queries with O(1) leaf child checks and bounded traversals.
* Optimized DOM attribute operations (getAttribute, setAttribute, hasAttribute,
  toggleAttribute, removeAttribute) with zero-allocation lowercase ASCII fast paths
  and single-pass string conversion.
* Fast-path selector matching in Element.matches() and Element.closest() for simple
  class (.cls), tag (tag), and id (#id) selectors, avoiding full CSS selector AST
  allocations on delegated event lookups.
* Subtree getElementById queries leverage document root ID indexes and bloom filters
  prior to falling back to full recursive DOM tree traversal.
* CSS line-height unit calculations now cover all root font relative units
  (rem, rlh, rex, rch, rcap, ric), element font relative units (lh, ex, ch,
  cap, ic), viewport percentage units (vh, vw, vmin, vmax, vi, vb, svh, svw,
  lvh, lvw, dvh, dvw), and container query units (cqw, cqh, cqi, cqb, cqmin,
  cqmax) across text layout and painting.
* DOMTokenList (classList) optimizes add and remove operations to skip
  redundant attribute re-serializations and DOM mutation dispatches when the
  underlying token set is unchanged.
* AbortSignal spec compliance: AbortSignal.abort() and AbortSignal.timeout()
  generate standard DOMException instances (AbortError, TimeoutError),
  AbortSignal.any() accepts any iterable signal collection, and prototype
  chains correctly inherit from EventTarget.
* Norwegian regional Accept-Language configuration adds complete fallbacks
  across Bokmål (nb), Nynorsk (nn), and generic Norwegian (no) locales.
* Parser and layout loop safety improvements: bounded counter formatting
  buffers, guaranteed forward pointer progress in pseudo content resolution,
  unified depth-bounded ns_node_root tree traversals, and cycle-protected
  document order comparisons.
* Selection highlights the text it is actually on. The highlight was
  painted as one flat pass over the finished page, in document
  coordinates, after everything else had been drawn — so inside a
  scrolling box it landed wherever the text would have been unscrolled,
  spilled past the box it belonged to, and ignored the transforms and
  clips the glyphs themselves were drawn under. It is now drawn where the
  text is drawn, from a range table computed once per frame and looked up
  per box, so it inherits that box's scroll offset, transform and clip by
  construction. It also goes down before the glyphs rather than over them,
  which is what lets `::selection` carry an opaque background without
  washing the letters out. Selection hit-testing gained the two things the
  click path already had: it adds a scrolling ancestor's scroll offset as
  it descends, so a click inside a scrolled `<div>` selects the line under
  the pointer instead of the line that would be there at scroll zero, and
  it stops at a box that clips its children, so a point below an
  `overflow: hidden` container no longer reaches the content clipped out
  of it.
* `user-select` and `::selection` are read from where the style is. Both
  were looked up on the inline box's own `style`, which inline boxes do
  not carry — the style lives on an ancestor — so the pointer was always
  null and both properties were silently ignored: `user-select: none` text
  copied anyway, and a page's `::selection` colours never appeared. Both
  now walk to the nearest ancestor that has a style, the same way the text
  painter finds its font.
* Copied text reads like the page. Every inline box ended with a newline,
  so a paragraph broken into runs by a `<b>` or an inline-block came out
  one word per line; a `<br>` — which layout carries as U+2028 — came out
  as nothing at all. A line break is now emitted where a block boundary
  is, U+2028 and U+2029 become newlines, and the zero-width characters
  `<wbr>` leaves behind are dropped. Text under `user-select: none` is no
  longer collected at the ends of a range, only in the middle of one.
* Double-click selects a word, triple-click selects the block. Both
  gestures previously did what a single click did. Word edges come from
  the shaper's own break attributes rather than an ASCII rule, so they
  hold for scripts that do not put spaces between words. Shift-click
  extends the existing selection from its anchor instead of dropping it,
  and a drag that selected text no longer activates the link it ended on —
  releasing the mouse after selecting a sentence containing a link used to
  navigate away.
* A page can see and set the selection it is showing. `getSelection()`
  reported an empty, collapsed selection no matter what was selected on
  screen, because nothing ever told the JavaScript engine what the
  selection was; `document.execCommand` answered false to everything and
  `navigator.clipboard.write` was absent. The page selection now flows
  into the engine on every change, so `toString()`, `type`,
  `isCollapsed`, `rangeCount` and the range's `getBoundingClientRect()`
  describe the real one; `execCommand` performs `copy`, `cut`,
  `selectAll` and `unselect`, with `queryCommandSupported` and
  `queryCommandEnabled` agreeing about them; and `clipboard.writeText`
  and `clipboard.write` reach the system clipboard through a side channel
  on the render response, the same route downloads and audio commands
  already take. Reading the clipboard stays refused — there is no
  permission prompt behind which to put it.
* A custom property can say what it holds. `@property` was parsed for its
  `inherits` and `initial-value` descriptors and nothing else: the `syntax`
  descriptor was read past, so the one thing the rule exists to declare —
  the grammar its value has to match — was never checked, and
  `CSS.registerProperty` was absent entirely, which is how most of the
  libraries that use registered properties reach for them. Both now go
  through one grammar: `src/css_prop_syntax.c` parses a `<syntax>` string
  into its alternatives and multipliers and matches a value against it,
  including the arithmetic, so `calc(7in - 12px)` is a `<length>` and
  `calc(5px + 10%)` is not, and so is the computational-independence rule
  that makes `10em` a legal length in a stylesheet but not as an initial
  value. A rule missing `syntax` or `inherits`, or carrying an
  `initial-value` its own syntax rejects, is now dropped rather than half
  honoured, and a declaration whose value does not match the registered
  syntax falls back to the initial or inherited value instead of being
  taken at face value. `CSS.registerProperty` throws the errors the API is
  specified to throw — `SyntaxError` for a name, syntax or initial value it
  cannot accept, `InvalidModificationError` for a second registration — and
  the CSSOM grew `CSSPropertyRule`, so `name`, `syntax`, `inherits` and
  `initialValue` read back off the rule.
* Viewport units inside a frame measure that frame. `vw`, `vh`, `vmin`,
  `vmax` and their small/large/dynamic spellings resolved against the
  top-level window wherever they appeared, so a 200x100 iframe laid its
  `100vw` box out at the width of the whole browser. The cascade now
  swaps in the frame's own viewport while it walks a nested document,
  the way the media-query evaluation already did, and only when the
  frame's size is actually known — from its `width`/`height` attributes,
  an inline size, or the last layout — so a frame whose size has not been
  measured yet keeps the behaviour it had rather than guessing at
  300x150. `data/fixtures` aside, a 200x100 `<iframe>` whose content asks
  for `100vw` by `50vh` now lays that box out at 200x50.
* A registered custom property computes its value instead of carrying the
  text it was written with. `<length>` arrives in pixels whatever unit it
  was authored in and whatever the element's font size is, a
  `<length-percentage>` that mixes the two serializes as the `calc()` the
  CSSOM specifies, `<angle>` lands in degrees, `<time>` in seconds,
  `<resolution>` in `dppx`, `<integer>` rounded, `<color>` as `rgb()` with
  `currentcolor` resolved against the element's own colour, `<string>`
  requoted, and a `<transform-function>` with its arguments computed the
  same way. The computation runs once the element's font metrics are
  known, so `--x: 14em` on a ten-pixel element is `140px` and an inherited
  value keeps the number its parent computed.
* A property registered through `CSS.registerProperty` now restyles the
  page. Incremental restyle skips a pass when no stylesheet has changed,
  and a registration changes no stylesheet, so a property registered from
  script had no effect until something else happened to dirty the tree.
  The registration is part of the signature that decides whether the pass
  can be skipped.
* `@counter-style` rules with a name no counter style may take — `none`, a
  CSS-wide keyword, or one of the six predefined styles the spec forbids
  overriding — are dropped instead of entering the stylesheet, and
  `CSSCounterStyleRule` reports its `name`.
* The CSS tokenizer decides what starts an identifier the way the Syntax
  specification does. A `-` was treated as the beginning of a name whatever
  followed it, so the subtraction in `calc(7in - 12px)` tokenized as an
  identifier rather than an operator; a hyphen now only starts a name when
  a name character, a second hyphen or an escape follows it, and the same
  rule governs the unit after a number and the name after `@`.
* The page itself snaps. `scroll-snap-type` reached 1.0.7 on scroll
  containers only, which left out the arrangement almost every page that
  asks for snapping actually uses: full-height sections down the document,
  with the property on `html` or `body` and nothing overflowing in between.
  The document scroller is the one scroller that is not a box — the shell
  owns its offsets in a `GtkAdjustment`, and the engine only learns them as
  the coordinates it is asked to paint from — so the snap positions its
  descendants offer were never consulted. The solver no longer derives the
  snapport from the scrolling box: it takes one, so the viewport can supply
  its own, and the renderer resolves the proposed offset against the root
  element's `scroll-snap-type` on the way into a frame and hands the snapped
  one back to the shell, which is the same channel `scrollIntoView` and
  fragment navigation already return a scroll position through. Every source
  of document scrolling therefore snaps — the wheel, the scrollbar, the
  keyboard — because each of them ends in a frame rendered at a new offset.
  `scroll-padding` on the root insets the viewport snapport as it insets a
  box's, `mandatory` and `proximity` keep their meanings, and the horizontal
  axis rides back alongside the vertical one. The root element is `html`,
  and `body` is honoured as a source too, which is how the root's `overflow`
  is already read here and what the pages that set it there expect.
  Verified on `data/render-tests/scroll-snap-viewport.html`, four
  hundred-viewport-height sections in a 955-pixel viewport: proposed offsets
  of 100, 400, 600, 1000 and 1400 all resolve to 955, 2000 to 1910, and
  anything past the end to 2865, while `scroll-snap.html`, whose snapping is
  all inside boxes, and the ordinary layout pages resolve to no snap at all
  and scroll exactly as before.

* Transitions and animations run on every property and show in style.
  The animation engine keeps one run per animation-name entry, so an
  element plays several @keyframes animations at once; iteration counts
  are fractional; a missing or empty @keyframes rule behaves as the spec
  says and a rule added after load is picked up on the next cascade; a
  @keyframes rule that omits its 0% or 100% frame interpolates from the
  element's underlying value, with transform: none as the identity; and
  the timing function applies per keyframe interval. Transitions start
  from a property that had no cascaded value (its initial value) and
  from the previous computed style when the transition and the change
  land in the same flush, interpolate keyword-stored numbers and lengths
  such as font-weight and vertical-align, clip rectangles and
  space-separated pairs such as two-value border radii, are cancelled
  when the property leaves transition-property or the element (or an
  ancestor) becomes display: none, shorten by the eased progress when
  reversed, and a child whose value is inherited from a transitioning
  ancestor follows the ancestor's animated value instead of starting its
  own transition.
* The animation and transition longhands are real properties:
  animation-name, animation-timing-function, animation-iteration-count,
  animation-direction, animation-fill-mode, animation-play-state,
  animation-composition, transition-property, transition-timing-function
  and transition-behavior (allow-discrete lets any property transition
  discretely), plus animation-timeline, animation-range,
  animation-range-start and animation-range-end as parse-only
  scroll-driven-animation properties. The shorthands expand into the
  longhands in the cascade and in the inline style object, so
  e.style.animation sets and clears the longhands and reads back the
  shortest serialisation; the shorthand grammar is strict (a bare number
  is an iteration count, a time never lacks its unit, a negative
  duration, a second timing function or a second name rejects the
  declaration, a transition without a property means all);
  animation-duration accepts auto; keyframe names given as strings are
  unescaped and serialised as identifiers; and the CSSOM rejects
  @keyframes rules named none, default, a CSS-wide keyword or a number.
  getComputedStyle serialises every longhand and both shorthands from the
  effective lists.
* A Web Animations surface backs the engine: Element.animate() drives the
  same keyframe runs from script (array and property-indexed keyframes,
  offsets, camelCase names, duration/delay/iterations/direction/fill/
  easing options) and its Animation reaches finish and cancel through
  the promises and events; element.getAnimations() and
  document.getAnimations() return CSSTransition, CSSAnimation and
  script Animation objects in spec order with currentTime, startTime,
  playState, play, pause, finish, cancel and reverse-safe seeking;
  AnimationEffect.getTiming and getComputedTiming follow the phase rules
  for fill, direction, iterations and zero durations;
  KeyframeEffect.getKeyframes returns the computed keyframes of a CSS
  animation with per-keyframe easing and composite; play() and pause()
  take precedence over an unchanged animation-play-state; and
  AnimationEvent and TransitionEvent have real constructors. A run
  starts on the first animation frame and the timeline is frozen
  between frames, so what a script sets is what it reads back, and the
  requestAnimationFrame timer no longer fires alongside the host frame
  loop.
* Animation and transition events follow the Web Animations phase model:
  animationstart, animationend, animationiteration, transitionstart and
  transitionend are emitted when the effect crosses between the before,
  active and after phases in either direction, including after finish()
  and seeks, with the elapsed times the spec assigns, and cancel events
  carry the active time.
* attr() substitutes at cascade time on every property, per css-values-5:
  attr(name) and attr(name string) quote the attribute, raw-string keeps
  it as is, a unit name (px, %, em, deg, s ...) appends that unit to a
  number, type(*) takes the attribute as a token stream and
  type(<syntax>) checks it against a registered-property syntax; a missing
  attribute or a value that fails the type falls back to the second
  argument or makes the declaration invalid at computed-value time; and
  attr() with a bad grammar is rejected at parse time. Elements that use
  attr() are excluded from style sharing and from the incremental restyle
  cache, so an attribute change re-substitutes.
* stretch, -webkit-fill-available and -moz-available are real sizing
  keywords: width fills the containing block's content box minus margins
  for block boxes, inline-blocks, floats, replaced elements, flex items
  (as the flex base size) and grid items, and absolutely positioned boxes
  fill from their static position or their insets to the padding edge;
  height, min-height and max-height stretch against a definite containing
  block and stay indefinite against an auto-height parent (also in quirks
  mode), and box-sizing does not change the stretched border box.
* Grid items with horizontal margins were laid out against the grid area
  minus their margins and then had the margins subtracted again, so every
  margined grid item was too narrow by its margin sum.
* A canvas element's width and height attributes map to aspect-ratio, as
  the HTML rendering section says, instead of to the width and height
  properties, so a stylesheet can size a canvas in one axis and get the
  other from its bitmap ratio. aspect-ratio keeps its numerator and
  denominator (auto 16 / 9 parses, serialises and computes as written,
  16 9 and 16px / 9px are rejected); a plain ratio overrides a replaced
  element's natural ratio while auto <ratio> only fills in for one.
  Replaced elements resolve min-width and max-width: fit-content,
  min-content and max-content from their ratio, and clamping one axis no
  longer rescales an axis the author specified.
* An absolutely positioned box with both insets set honours justify-self
  and align-self: start, end and center size it to its content and place
  it inside the inset-reduced containing block, normal stretches a
  non-replaced box and fits a replaced one, and stretch fills the area.
* attr() substitutions that land inside url(), src(), image() or
  image-set(), or that carry a url() of their own, make a non-custom
  declaration invalid, as css-values-5 requires for attribute-derived
  URLs.
* Fixed a read past the end of a linked stylesheet's bytes while
  scanning it for viewport media queries.
* Serialisation fixes for the specified style: counter-reset,
  counter-increment and counter-set escape their counter names and keep
  calc() integers as authored; list-style keeps the position when the
  type is a counter style named inside or outside, and its longhands read
  from a list-style shorthand serialise the image the way the
  list-style-image property does; gradient colour stops serialise hex and
  legacy rgb()/hsl() colours as rgb() and keep named colours and modern
  colour functions as written; overflow expands in the inline style, a
  visible/scroll pair computes to auto, and overflow-clip-margin,
  counter-set and list-style are reflected by getComputedStyle.
* Transitions and animations: a transition created in a later frame gets
  a later start time and reads as pending until its first frame; steps()
  rounds its floor at the interval boundary; one animationiteration event
  fires per seek across several iterations; animation, transition,
  container, direction and unicode-bidi properties are never animated.
* Nested style rules are exposed through the CSSOM: CSSStyleRule inherits
  from CSSGroupingRule with cssRules, insertRule and deleteRule,
  selectorText on a nested rule is relative to its parent (a selector
  without & or one starting with a combinator serialises with a leading
  "& ") and is settable, cssText prints the nested rules, and "&" followed
  directly by a type selector is rejected as the spec requires.
* A declaration whose value is a single {}-block (color: {var(--x)}) is
  a declaration, not a nested rule, in the CSSOM parser, and a custom
  property keeps a {}-block anywhere in its value.
* WPT, third pass of 1.0.8 (same 37 css/ areas, 6 s timeout): 42482 ->
  46653 of 68725 subtests. css/css-transitions 149 -> 2282 of 2504,
  css/css-animations 191 -> 803 of 976, css/css-sizing 745 -> 1320 of
  2444, css/css-align 3025 -> 3247, css/css-values 3599 -> 3739,
  css/css-lists 136 -> 256 of 274, css/css-overflow 300 -> 365,
  css/css-nesting 20 -> 84 of 117, css/css-easing 32 -> 84 of 156,
  css/css-position 267 -> 297; the full table is in docs/compliance.md.

1.0.7:
======
* The `about:start` splash carries the release number and a better sky and
  earth. The sky was a two-stop vertical ramp drawn as one filled rectangle
  per scanline, which is both the slowest way to write a gradient and the
  one that bands worst; it is now a multi-stop atmosphere rendered as an
  array, with forward scattering around the sun, a haze band along the
  horizon and a scatter of stars that fades out as the sky brightens
  towards it. The north star hangs in the dark of the zenith, cool against
  the warm sun on the other side of the frame, and twinkles on its own
  period so the two never pulse together. The earth had been a soft blur:
  its terrain came from one 1024² noise field sampled through three fixed
  mip taps, so coastlines dissolved and nothing on land read as relief.
  It is built at 2048² now from a continent field, a coastline field and
  ridged noise for mountain ranges, shaded from the height gradient, and
  sampled with a real trilinear filter across the whole mip chain — so
  there are coastlines with a shallow shelf inside them, snow that sits on
  the peaks rather than across whole regions, and deserts and forests that
  follow a moisture field. Aerial perspective now decays with distance the
  way it should, which had been inverted: the haze thinned towards the
  horizon and thickened over the ground nearest the viewer. The land is
  settled, in the manner of a turn-based strategy map: capitals are
  scattered across it at a minimum separation and the ground divided
  between them by a Voronoi partition whose distances are warped by noise,
  so frontiers wander the way drawn ones do rather than meeting at
  straight bisectors. Each nation takes the lowest colour none of its
  neighbours holds, and wears it as a wash over its territory, a bolder
  line along its frontiers and a lighter one down its coast, with a
  capital and up to four towns marked inside. It rides as its own
  premultiplied overlay above the terrain and fades out with distance, so
  the markers do not turn into confetti where the texture repeats near the
  horizon, and passing cloud covers them the way cloud covers everything
  else.
* `offsetLeft` and `offsetTop` are measured from the offsetParent again.
  Both returned a document coordinate — and one built from the margin box
  rather than the border box — so an element inside any positioned
  ancestor reported where it sat on the page instead of where it sat in
  its parent. CSSOM View asks for the distance from the offsetParent's
  padding edge, with a statically positioned `body` or root the exception
  every engine makes, and that is what they return now. A great many
  pages measure this way, and so does the `checkLayout` harness that most
  of WPT's layout tests are written against: the reading of
  `css/css-flexbox` was flat at 653 subtests before the fix below and
  after it, because the fix could not be seen through this one. With both
  in, `css/css-flexbox` goes from 653 to 1437 of the same 3535 subtests,
  18.5% to 40.7%.
* An absolutely positioned child of a flex container is placed where the
  flexbox specification says. It landed at the container's content-box
  origin whatever the container asked for; CSS Flexbox 4.1 says its
  static position comes from `justify-content` and its own `align-self`,
  as though it were the only flex item, and `flex-direction: *-reverse`,
  `flex-wrap: wrap-reverse` and `direction: rtl` each turn the axis they
  govern around. Vertical writing modes are not covered — flex layout
  itself is horizontal-only here.
* `flex-wrap: wrap-reverse` puts the first line last. Lines wrapped, but
  the cross axis was never turned around, so the first line stayed at the
  top and `align-content: flex-start` stayed at the top with it. The
  lines are now mirrored within the container after `align-content` has
  placed them, and each item is mirrored within its line, which reverses
  `align-items: flex-start`/`flex-end` along with them.
* CSS Scroll Snap. `scroll-snap-type` on a scroll container, with
  `scroll-snap-align` on the things inside it, moves the container onto
  the nearest snap position once a scroll lands — from the wheel, and
  from `scrollTop`/`scrollLeft`. `scroll-padding` on the container and
  `scroll-margin` on an item inset the snapport and outset the snap area,
  both as shorthands and per side; `mandatory` always snaps, `proximity`
  only from within half a page. A wheel tick shorter than the gap between
  two snap positions still moves the reader forward rather than falling
  back to the one behind. This is scroll containers only: the document
  scroller belongs to the window, not to a box, so `scroll-snap-type` on
  `html` or `body` does nothing yet.
* The browser prints. `Ctrl+P`, or *Print…* in the menu, lays the page out
  for paper and hands the sheets to the operating system's own print
  dialog through `GtkPrintOperation` — CUPS on Linux, the Win32 printer
  dialog on Windows, and the Cocoa panel on macOS — so no printing code
  is written per platform and no new dependency is added. The engine gets
  the parts of CSS that a printer needs: `@media print` now matches (the
  media type was hardcoded to `screen`, so a page's print stylesheet was
  simply ignored), `@page` sets the sheet size from a name (`A4`,
  `letter`, `legal`, `ledger`, the A/B series), from one or two lengths,
  or from `portrait`/`landscape`, along with its margins, and
  `break-before`, `break-after` and `break-inside` — with the legacy
  `page-break-*` spellings mapping onto them, `always` becoming `page` —
  decide where a sheet may end. A sheet is cut at a forced break if there
  is one before the page is full; otherwise the cut is pulled up above
  any box it would have split, which is every leaf box, every line of a
  paragraph, and anything asking for `break-inside: avoid`. Printing
  restores the on-screen layout afterwards, so the page a reader is
  looking at does not reflow under them. `--dump=print:FILE` renders the
  same pagination to a multi-page PDF without a printer, which is how
  `data/render-tests/print-pagination.html` was checked: three A4 sheets,
  every card whole, the `@media print` paragraph swapped in, and the
  forced break starting sheet three.
* Text layout is ns-pango `2f975d8`, and a paragraph now measures the same
  whether or not another paragraph shaped its words first. The shaping
  cache decided where a run could be cut by reasoning about Unicode — a
  space either side of the boundary, or an ideograph — but Unicode does
  not know what a font does, and Liberation Sans and Liberation Serif,
  which fontconfig hands out for Arial, Helvetica and Times New Roman,
  kern the space against the letter that follows it and put the
  adjustment on the space. A cached piece ending in one therefore carried
  a width that belonged to whatever word had followed it that time, and
  served it in front of another: "Type of" came out 1024 units narrow
  once "Type A" had been laid out, and which paragraph was wrong depended
  on what the process had rendered earlier. HarfBuzz answers this
  directly — asked for unsafe-to-concat flags it marks the clusters whose
  glyphs depend on the text beyond them — so a piece is now stored only
  when the shaper cleared both of its cuts, and an item whose pieces do
  not all survive that is stored whole instead. `NS_PANGO_SHAPE_CACHE=0`
  now switches off the item and break caches along with the shape cache,
  and `verify` mode compares a served item against a fresh shaping field
  by field rather than skipping anything longer than a word. Verified
  here by rendering the 45 pages in `data/render-tests/` with the cache
  on and with it off: identical layout on every one, and no mismatch
  under `verify`.
* The build documentation says how a dependency is actually resolved and
  how to move one. `docs/building.md` and `README.md` both said `meson
  setup` fetches lexbor, quickjs-ng and ns-pango, when the first two take
  a system copy whenever the build finds one new enough and only fall
  back to the wrap — and ns-pango is the one that is always the
  subproject, because the renamed symbols are the whole reason it can sit
  beside the system Pango that GTK loads. README also listed the vendored
  components and then named only lexbor and quickjs-ng as fetched,
  leaving ns-pango in neither list. `docs/building.md` gains a section on
  updating a pin: that editing `revision =` does not move the checkout on
  its own, that `meson subprojects update --reset <name>` does and works
  on the shallow clones these wraps ask for, where the three `diff_files`
  patches live and when to regenerate them, which vendored copies are
  byte-identical to upstream and which two are not, and what to verify
  afterwards.
* The JavaScript engine is quickjs-ng v0.16.1, up from v0.15.1. The two
  patches the tree carries — the Windows link fix and the removal of the
  identical-object shortcut in `Array.prototype.sort`, which skipped a
  comparator the specification says must run — still apply and were
  regenerated against the new sources so they land without fuzz.
  `JS_NewArrayBuffer` grew a `max_len` parameter and now takes a
  reallocating callback in place of a freeing one, so the WebAssembly
  memory object passes a zero maximum and no callback, which is the
  fixed-length, externally-owned buffer it was already asking for.
* A page's ES modules load again. Reading `document.implementation`
  before `DOMImplementation` existed on the global left a TypeError
  pending on the context: the getter looked up the constructor's
  `prototype` without first checking that the constructor was there, and
  discarded the failure without clearing it. Nothing noticed until
  quickjs-ng v0.16.1 began reporting a pending exception at the next
  module boundary, at which point the stray error surfaced as the
  rejection of every `import` on the page — static, dynamic, `data:` and
  `blob:` alike — because the shape-normalising bootstrap reads that
  getter one statement before it defines the constructor. The lookup now
  guards the constructor the way every other prototype lookup in the file
  already does.
* The bytecode cache carries a new format stamp. quickjs-ng's own
  bytecode version moved with the upgrade, so entries written by an
  earlier build describe a layout the new engine will not read; they were
  already rejected safely and recompiled, but the stamp is what the cache
  has to discard them outright instead of paying for a failed read on
  every load until they age out.
* A translucent background on an inline box is translucent. Inline runs
  are painted through ns-pango attributes, and while a text colour
  carrying alpha already emitted a matching foreground-alpha attribute, a
  background colour emitted only the opaque three-channel one, so the
  alpha was dropped on the floor: `code { background: rgb(0 0 0 / 14%) }`
  painted a solid slab. The same colour on a block, an inline-block or a
  flex item was correct, which is why this survived — the wrong path is
  the one a dark theme's inline chips take. Both places that build the
  attribute list now emit `background_alpha` alongside the colour, exactly
  as the foreground path does.
* The documentation says what the code does. Several claims had outgrown
  the tree: `docs/architecture.md` gave a storage and security role to
  `secretbox.c`, a file that does not exist, and omitted ns-pango
  entirely; `docs/README.md` and `AGENTS.md` still said `<video>` laid out
  without decoding; `CLAUDE.md` still routed unrecognised images through a
  GDK-Pixbuf fallback removed in 1.0.6; `SECURITY.md` located the
  single-process startup path in `src/gtk/appmain.c`, which is
  `src/appmain.c`, and claimed no video codec attack surface at all — the
  pl_mpeg decoder is one, so it is now described with its bounds rather
  than denied. Cross-document `postMessage` is documented as working in
  both directions, with what it does and does not guarantee, now that it
  does. `THIRD-PARTY-LICENSES.md` gained the missing notice for ns-pango,
  which is LGPL and statically linked, and a list of the patches applied
  to upstream sources; it lost libepoxy from the statically-linked
  section, since nothing links it. `docs/debian.md` is gone — a dated
  packaging status and a list of accounts to register, whose one durable
  recipe already lives in `debian/README.source` — and so is
  `docs/preloading.md`, three hundred lines on the fetch path of one
  subsystem, reduced to the sentence in the pipeline table that says a
  subresource is fetched once. `AGENTS.md` is a pointer to `CLAUDE.md`
  rather than a second copy of it that drifts, and `docs/compliance.md`
  no longer repeats this changelog.
* A message a frame posts to its parent reaches the parent's listeners.
  Delivery decided whether the recipient was the top window by comparing
  it against whichever realm was executing, and while a frame's script
  runs that is the frame's own realm, so a message correctly addressed to
  the parent was judged to belong to some other window and handed to a
  dispatch path where the top window's `message` listeners are not
  registered. The message was built, cloned and queued, and then went
  nowhere; posting from the top window to itself worked, which is why
  this survived. The comparison is now against the main realm, which does
  not move. Freenet's River runs in a sandboxed frame and reaches the
  network by asking the shell around it to hold the WebSocket on its
  behalf, so every request it made was dropped in silence.
* A framed document is governed by its own Content-Security-Policy, not
  by the one that came with the page framing it. A policy was kept once
  per browser, so whatever header arrived last decided what every
  document was allowed to load. Freenet's River is served as a shell page
  whose own policy names no script host at all -- correct, because every
  script it runs is inline -- wrapped around an iframe holding the actual
  chat app, whose policy does allow its origin; the app's module was
  judged against the shell's policy and refused, and the page never got
  past the header bar. Each framed document now carries the policy from
  its own response and `<meta>`, and a resource is judged against the
  policy of the document holding it, which also survives the app adding
  a stylesheet from a timer long after the frame finished loading.
* `fetch()` resolves with a real `Response`. The object had the right
  properties and methods but not the prototype, so `instanceof Response`
  was false and `constructor.name` was `Object`. Code that branches on
  the type rather than duck-typing took the wrong path: wasm-bindgen's
  loader treats a non-`Response` as an already-compiled module and hands
  it straight to `WebAssembly.instantiate`, which is why River's
  WebAssembly failed with a `BufferSource or Module` type error instead
  of loading.
* `window` inside a frame is a `Window`. Frames get their own realm whose
  global was left an ordinary object, so `window instanceof Window` was
  false there while it was true at the top level. `web_sys::window()`
  makes exactly that test, so every Rust and WebAssembly UI framework hit
  it: Dioxus panicked with ``access to `window` `` the moment River
  started, taking the whole app down with an unreachable trap.
* `--version` prints the version and exits. The flag was never
  recognised, so it fell through to an ordinary startup: the browser
  armed the watchdog, opened a window and left the caller with exit 255
  and no version anywhere. It is now answered before the sandbox, the
  watchdog and GTK are touched, which is also what lets it work from the
  Windows bundle -- the launcher there attaches to the parent console for
  it the way it already did for `--headless` and `--print-config`.

1.0.6:
======
* A grid item placed by area name is aligned to its row. Items placed
  through `grid-template-areas` went down a layout path that never read
  `align-items` or `align-self`: each was put at the top of its row at its
  own height and left there, where the default is to stretch. lichess's
  lobby is one grid row holding the pool, the game list and the start
  buttons; the buttons' column stayed 179 pixels tall beside a 600-pixel
  neighbour and the player counts, pinned to the bottom of that column,
  came to rest on top of the buttons. A stretched item is now given its
  row height before it lays out rather than grown afterwards, so what is
  inside it lands in the right place too.
* A single flex line is as tall as the container says. A row flex
  container with a definite height has a line exactly that tall, and an
  item that stretches gets that height whether its content fits or not.
  The line was sized to the taller of the container and its content
  instead, so one over-tall item dragged the whole line past the height
  the author asked for -- lichess's 59-pixel site header held blocks that
  grew to 86 and hung out of the bar across the page behind it.
* A percentage inside `min()`, `max()` and `clamp()` is measured against
  the box rather than the viewport. These functions were folded to a
  single pixel value while the stylesheet was being parsed, when the only
  basis available was the viewport width, so the comparison ran against
  the wrong number and the answer was frozen before the element it
  belonged to was known: in a 400-pixel column `min(300px, 50%)` came out
  300 instead of 200. `width: min(100%, 60rem)` now measures what it says.
* A dialog opened from script renders, and a modal one is centred.
  `showModal()` and `show()` set the open attribute without telling the
  style engine, so the element kept the `display: none` it was matched
  with at parse time -- it reported open, matched `[open]`, and had no box
  at all. Only a dialog carrying `open` in the markup ever appeared. The
  user-agent sheet now also carries the modal rule the HTML specification
  defines, and an out-of-flow box asking for an intrinsic height is no
  longer stretched between its top and bottom offsets, so a modal lands in
  the middle of the viewport the way it does in a browser.
* A mouse or pointer event carries the window it was dispatched in.
  `UIEvent.view` is that window and every event the engine synthesised
  for a click, a drag or a hover reported null, which is a value no
  browser produces.
* A grid track can be measured in any length unit. `grid-template-columns`
  understood px, %, fr, em and rem, and quietly dropped every track it
  could not read, which moved each remaining track one place to the left.
  lichess.org asks for five columns with a `1vw` margin at either end;
  three arrived, its `<main>` landed in a column with no room in it, and
  the whole site laid out zero pixels wide down a 26000-pixel page. Track
  lengths now go through the same reader as every other length, and a
  track list that still cannot be read is discarded whole rather than
  closed up, because a missing list leaves the columns to
  `grid-template-areas` while a shifted one leaves nothing standing.
* A positioned box answers the pointer in the layer it paints in. Hit
  testing ranked a box against its own siblings and nothing else, so a
  fixed, high `z-index` overlay never rose above content in another branch
  of the page even though the painter drew it on top: lichess's game-setup
  dialog was visible and its "Play with the machine" button was not
  clickable, because the news feed behind it took the click. Positioned
  boxes are now collected and tried in the order the painter flushes them,
  and a modal dialog in the top layer is tried before the document.
* An `<svg>` that paints nothing hands the pointer to what is under it.
  SVG hit-tests as `visiblePainted` -- a shape answers where it draws and
  nowhere else -- but the engine lays an `<svg>` out as one replaced box
  and let that box answer for its whole rectangle. chess.com stretches an
  `<svg>` of rank and file labels over the board, so a piece could be
  picked up and never put down: the labels swallowed the pointerup that
  ends the drag.
* An SVG `font-size` attribute survives the cascade. A presentation
  attribute is author style at the very bottom of the cascade, so a real
  declaration beats it but inheritance must not; the renderer read the
  attribute and then overwrote it from the computed style, which always
  has a font-size because font-size is inherited. Every `<text>` drew at
  the page's font size scaled by the viewBox -- chess.com's board
  coordinates came out four times their size and spilled across the board.
* A worker shares the storage of the page that started it. The worker
  runtime was built without a storage partition, and IndexedDB reads that
  partition to find the origin's databases, so every `indexedDB` call
  inside a worker threw "Storage is unavailable" and chess.com reported
  its opening database as unusable.
* `prefers-color-scheme` reports what the desktop is actually wearing. The
  answer came from `gtk-application-prefer-dark-theme`, a GTK 3 property a
  GTK 4 dark theme does not set, so every dark desktop was told "light":
  a site's dark stylesheet never applied, and the browser's own pages sat
  white inside a dark window. The scheme is now judged from the luminance
  of the foreground colour the theme resolves, which holds for any theme
  rather than the ones that happen to set the old property, and it is
  re-evaluated when the theme changes under a running window. The internal
  pages -- start, about, settings, history and the error page -- gain the
  dark half they never had.
* An error page appears whenever a navigation has nothing to render. Only
  https:// failures got one, so a missing `file://` path came back 404 with
  an empty body and rendered as a blank white page. The page is now built
  for any scheme that ends in a transport error or a 4xx/5xx with no body
  of its own, and the classifier no longer blames the network for
  everything it fails to recognise: an unmatched message falls through to
  the status code, and a file URL is described as a file rather than as an
  unreachable server.
* The status line behaves like one. It held a permanent row beneath the
  page and read "Done" for the life of every visit; hovering a link wrote
  the URL there and leaving the link wrote nothing, so the last link the
  pointer touched stayed on screen indefinitely. It now clears when the
  pointer leaves a link and when a load finishes, and floats over the
  bottom-left corner of the page only while it has something to say.
* The toolbar reports the page zoom. Zooming said "Zoom 121%" for a moment
  and then left no trace, so a window could sit at any magnification with
  nothing admitting it -- and the steps were successive tenths, which is
  where 121% came from. A reading appears beside the address bar while the
  page is scaled, resets the zoom when clicked, and the steps follow the
  usual ladder: 90, 100, 110, 125, 150.
* The bookmark button says whether the page is bookmarked, with a star that
  fills once it is on the list; the popover's action becomes "Remove this
  bookmark" rather than silently doing nothing a second time. Escape closes
  the find bar, which its own tooltip already promised. The title bar no
  longer reads "Northstar 1.0.6 — Northstar 1.0.6" on a page with no title.
  The bookmarks popover sizes to its contents instead of reserving 280
  blank pixels, and the downloads window says when it is empty.
* Text shaping is cached a word at a time, not a run at a time, and a
  paragraph's items are cached too. The line breaker cuts a run wherever a
  line ends and shapes the piece again, so a paragraph measured
  unconstrained and then laid out at a real width shared no cache entry
  with itself -- the hit rate sat at half. Words do not move when the width
  does, which is why Firefox and Chrome both cache text a word at a time,
  and now so does this. A miss still shapes the whole run in one HarfBuzz
  call and only then divides the result, so no glyph is shaped in smaller
  company than before. Over a 200 KB page, shape-cache misses fall from
  5265 to 155 and stay there at every window width, HarfBuzz drops from
  28.6% of the process to 1.5%, and the layout phase goes from 159 ms to
  112 ms. Itemising -- bidi, script, emoji and width runs, and the font
  lookup behind each character -- is cached on the same principle, keyed on
  the context's serial so that a web font arriving invalidates it. Nothing
  moves on screen: five test pages, one of them mixing Latin, Arabic,
  Hebrew, Devanagari, Thai, CJK and emoji, render byte-identically.
* An IndexedDB write no longer walks the origin's whole storage directory.
  Every `put` recomputed the origin's quota by opening each `.sqlite` file
  beside the current one, asking it for `page_count` and closing it again --
  a directory scan and a fresh SQLite connection per record written, inside
  the write transaction. A page that stores a burst of records stalled the
  browser in the filesystem for as long as the burst lasted: starting a game
  on chess.com left the main thread inside `CreateFile` and it never came
  back. Cache the siblings' total for five seconds; the current database is
  still measured live, so the limit is enforced as before.
* The font metrics behind the `ex`, `ch`, `cap` and `ic` units are measured
  once per font rather than once per length. Resolving any of them shaped
  four probe glyphs through a fresh layout, and a stylesheet asks for the
  same font once per element its rule matches, so a page that sizes fields
  in `ch` paid for it on every one of them. The answer depends on nothing
  but the family, size, weight and slant, so it is kept until the font map
  changes underneath it -- 1.3% fewer instructions over a page whose
  elements each carry their own font, and nothing moves: the layout dump
  and the rendered PNG are byte-identical.
* A single-line text field shows as many characters as its box has room
  for. The visible window came from the `size` attribute alone, so CSS that
  widened the control -- `flex-grow`, `width: 100%` -- stretched only the
  painted frame: the start page's search box scrolled its text away after
  twenty-four characters with room for sixty. A control narrower than its
  `size` attribute clips inside its border now instead of painting past it.
* A `#fragment` stays anchored while the page finishes loading. The scroll
  offset was computed once, from whatever layout existed at navigation
  time, so images above the target had not been decoded yet and the view
  landed hundreds of pixels short of the heading it was asked for. The
  target is re-located whenever layout moves it, until the document goes
  quiet or the reader scrolls away.
* A popover menu takes the height its items need. GTK builds a section's
  separator after the popover has negotiated its size, so the surface came
  out one separator short and the scrolled window inside it swallowed the
  difference -- the last item of the context menu was always clipped.
* The toolbar menu covers what a reader reaches for: New Window, Zoom In,
  Zoom Out, Reset Zoom, Full Screen, History and the two save entries join
  it, grouped into window, view, page and tools sections, each naming its
  keyboard shortcut. Reload, a toolbar button one pixel away, leaves.
* Escape in the address bar restores the URL of the page on screen. It used
  to return focus to the page but leave whatever had been typed in the bar,
  where it named a page the window was not showing. With the page already
  focused, Escape stops the load. The connection indicator moves inside the
  entry, where the padlock belongs.
* Text layout no longer recomputes a paragraph's line, word and sentence
  breaks every time it lays that paragraph out. ns-pango gains a cache of
  what `default_break` -- UAX #14 plus UAX #29 -- produces for a run of
  bytes, which was 12.6% of every instruction the text engine executed and
  ran once per layout: a browser builds a layout over the same paragraph
  for min-content, for max-content, for the real width and again to paint
  it. On a 200 KB page 1120 distinct paragraphs are broken once and served
  2492 more times from the cache. Asking the shape cache no longer
  allocates either -- the key it was given per shaping call, hit or miss,
  cost a malloc, a free and two atomic refcounts on the font at a 98.7% hit
  rate -- and ASCII is classified for emoji segmentation without touching
  the interval tables. The whole headless page load runs 5.75% fewer
  instructions; the text engine alone, 20% fewer. Rendering is unchanged:
  the glyph-level dump and the rendered PNG of a mixed-script page are
  byte-identical either side.
* The text caches are sharded sixteen ways, so laying text out on several
  threads is worth doing. Every thread gets its own fontmap, because
  fontconfig's is unlocked, and so its own font objects -- which the cache
  keys on -- so all of them missed, all took the one lock exclusively to
  insert, and all queued behind each other: on four cores one table was
  worth 1.9x over no cache on one thread and only 1.2x on four. Sharding
  takes four-thread throughput a quarter higher, from about 79 000 to about
  98 000 layouts/s over the fork's corpus, and costs the single-threaded
  path nothing. Northstar's own layout still runs on the main loop thread,
  so this is headroom for parallel layout rather than a speedup today.
  `--debug=net` reports the break cache alongside the shape cache.
* `document.styleSheets` includes the sheets a page links to, with their
  `href` and their rules. Only inline `<style>` blocks had a populated
  `CSSStyleSheet`; a `<link rel=stylesheet>` produced one with a null href
  and an empty `cssRules`, because the sheet was built from the element's
  own text content and a link has none. nrk.no went from 13 reachable rules
  to 1895. The engine keeps a reference to the CSS it already fetched for
  the cascade, so nothing is downloaded or stored twice.
* `getComputedStyle(el).cssFloat` reports the used float. The accessor read
  the declaration block directly rather than going through whichever
  `getPropertyValue` the object carries, so on a computed style -- which has
  its own -- it always came back as the empty string, while the equivalent
  `getPropertyValue('float')` answered correctly.
* An inline-block, inline-flex or inline-grid sits on the text baseline by
  the baseline of its own last line box, the way CSS 2.1 asks, instead of
  resting its bottom margin edge there and then being placed at the top of
  the line regardless. A badge or button written inline with a sentence was
  drawn several pixels high, its own text floating above the words on either
  side, and the line box was made taller than it needed to be to cover the
  overshoot -- duck.ai's "Alle chatter er private" was the visible case.
* Instantiating a module through the `WebAssembly` JS API runs the module's
  start section and nothing else. WAMR, built for a standalone runtime, also
  called `_initialize`, `__wasm_call_ctors` and `__post_instantiate` from
  inside `wasm_runtime_instantiate` -- but on the web those are ordinary
  exports the JS glue calls itself, after it has pointed its heap views at
  the instance's memory. Running them first meant an Emscripten module tore
  down on its own first WASI call: chess.com's analysis engine died in
  `environ_sizes_get` before `new WebAssembly.Instance` had returned.
* A finished keyframe animation keeps the value `animation-fill-mode`
  says it should. The engine sampled the last keyframe correctly, then threw
  the sample away: every getter the painter calls required the animation to
  still be running, so at the moment it ended the box snapped back to its
  specified value. The common `opacity: 0` plus a `fade ... forwards`
  animation therefore faded in and vanished again within one frame, and the
  content stayed invisible for the life of the page -- chess.com's bot
  gallery, which is exactly that pattern, was a set of empty boxes.
* `AbortSignal` is an interface object, not a bare namespace. It was a plain
  object carrying `abort()`, `timeout()` and `any()`, and the signals an
  `AbortController` hands out did not inherit from it, so `signal instanceof
  AbortSignal` -- the guard every fetch wrapper writes -- threw "invalid
  'instanceof' right operand" instead of answering. chess.com's RPC client
  turned that TypeError into a 500 and never issued its first request.
  `MessagePort` gains the same treatment in the window: it existed only in
  workers, so ports came back with no prototype at all.
* A grid container's max-content width is the sum of its columns, not the
  width of its widest item. Anything that shrink-wraps a grid -- a float, a
  table cell, an inline-grid, `width: max-content` -- was sized as if the
  columns were stacked, so the tracks overflowed the box they were given.
  bbc.com's "LIVE" flag is a floated two-column grid, and the headline
  beside it started inside the flag rather than after it.
* Flex and grid items measure their intrinsic sizes in the font they will
  actually be drawn in. The base size of a flex item and the min-content
  floor of a flex or grid item were measured against the item's own style
  rather than the style its text inherits, so text in a web font was sized
  by the fallback face. An item then got a base size a pixel or two under
  what the real font needs and wrapped mid-phrase however much room the
  container had -- dn.no's nav pills broke "DN Helg" and "DN i VM" across
  two lines inside a box wide enough for either.
* An absolutely positioned box with `width: auto` gets the shrink-to-fit
  width CSS 2.1 asks for -- its max-content size clamped to the available
  space and floored at min-content -- measured by shaping the text. It used
  to be guessed from a character count at 0.65em each, so every tooltip,
  dropdown, badge and popover came out at a width unrelated to its
  contents: a seven-character label was sized 81px where the text needs 63.
* Event-listener objects follow the Web IDL callback-interface algorithm.
  `handleEvent` is looked up for every dispatch, non-callable values and
  throwing getters are reported as uncaught listener exceptions, and generic
  `EventTarget` objects no longer discard object listeners. The focused DOM
  event test moves from three passing subtests out of six to all six.
* Checkbox and radio activation keeps the state required by HTML's legacy
  pre-activation and canceled-activation steps. `indeterminate` is a real
  cloned input state, a canceled radio click restores the previously checked
  group member, synthetic `click()` events are untrusted and cannot recurse on
  the same element, and the resulting `input` and `change` events are not
  cancelable. Three focused input tests move from 52/80 to 80/80 subtests.
* `CSSStyleSheet.insertRule()` and `deleteRule()` enforce their required
  arguments, while the deprecated but web-visible `addRule()` and
  `removeRule()` methods mutate both constructed and document sheets. The
  CSSStyleSheet interface test moves from 8/17 to 17/17 subtests.
* An SVG with a `viewBox` but no width or height is sized the way CSS
  says: its ratio fitted inside the 300x150 default object size. The
  decoder used to rasterise it into a square, so the artwork was
  letterboxed and then stretched into the page's box -- Wikipedia's
  wordmark came out as a smear, and chess.com's sidebar logo under-scaled
  under `background-size: contain` and vanished entirely under `cover`.
* Workers get the APIs the polyfills already implement. A worker ran only
  the first part of the polyfill bundle, so IndexedDB, the streams, Blob
  and caches were missing inside one while the window had them all --
  thirteen globals differed between the two scopes. chess.com's opening
  book worker failed on `indexedDB is not defined` and took the play
  page's initialisation with it; that page now loads without a script
  error. `AbortController` and `AbortSignal` are installed in workers
  too, so a worker can cancel a fetch.
* A page on an origin that does not speak QUIC no longer stalls for the
  whole connect timeout. Whenever libcurl was built with HTTP/3, every
  request asked for it, so the first hop to an origin that silently drops
  UDP on 443 waited out the 15-second navigation connect timeout (6 for a
  subresource) and returned a timeout rather than falling back. The
  timeout also counted as a connection failure, which parked the host in
  the unreachable cache for two minutes and failed every subsequent
  request to it -- so acid3.acidtests.org took 15 seconds to answer and
  then lost all of its subresources. Requests now ask for HTTP/2 and are
  upgraded to HTTP/3 by the alt-svc cache, the way an origin advertises
  it; `NS_FORCE_HTTP3=1` still asks for HTTP/3 outright. Acid3 loads in
  2.2 seconds instead of 15.5, and three seconds in it has run 65 of its
  tests rather than 12.
* `min-content` and `max-content` grid tracks size to their content.
  Both parsed to the same kind as `auto`, so such a column stretched into
  the free space instead of shrinking to what it holds. Two defects sat
  behind that: a box with a definite width contributed nothing to
  min-content, because the measurement only ever looked at its children;
  and an intrinsic track was measured against the space left after every
  `minmax()` had taken its maximum, which scaled it to zero. On
  chess.com's play page the board column now has a width and the side
  panel sits beside the board rather than on top of it. A `min-content`
  track is also never scaled down to make the tracks fit, since its items
  cannot render narrower than it: the grid overflows instead, where before
  the following columns slid underneath content that kept its own width.
  A grid container's own min-content is the sum of its columns rather than
  its widest child, which is what a grid nested in a grid needs: on
  chess.com the board's inner grid measured its widest column instead of
  all four, so the board hung out of its container and over the panel.
* Grid items can be placed on named lines. A line named in the track
  list was parsed as a track, rejected, and dropped, so `grid-column:
  main` resolved to nothing and the item was auto placed. Names are now
  recorded and resolved, an area called `foo` also defines `foo-start`
  and `foo-end`, an end line repeating the start's name means the next
  line with that name, and an item with a column but no row keeps its
  column. chess.com's play page draws its board again.
* `OfflineAudioContext` renders audio instead of silence. The whole Web
  Audio surface was shape without substance: every `create*` method
  returned the same generic node, `connect()` returned its argument
  without recording an edge, `start()` and `stop()` did nothing, and
  `startRendering()` resolved a buffer of zeros. `AudioBuffer` could not
  hold samples at all -- `getChannelData` minted a fresh zeroed array on
  every call, so writing to it discarded the write. Nodes now carry their
  kind, `connect()` records the graph, sources honour their scheduled
  start and stop, buffers keep one array per channel, and the graph is
  rendered by `src/webaudio.c`: oscillators (sine, square, sawtooth,
  triangle, with detune), gain, a dynamics compressor with soft knee and
  attack/release, the RBJ biquad types, delay, wave shaping, constant
  sources and buffer playback with rate and looping. The canonical
  oscillator-into-compressor pipeline that used to sum to exactly zero
  now returns real samples. Rendering is mono, summed into every channel
  of the destination buffer, and `AudioParam` automation curves are still
  ignored -- a parameter reads as its current value for the whole render.
  The context, its nodes and its buffers also brand themselves, so
  `Object.prototype.toString` reports `OfflineAudioContext`,
  `OscillatorNode` and the rest rather than `Object`.
* The `about:start` splash is redrawn for the release: it reads
  "A fine open source web browser." beneath the version, and a small pink
  pig flies among the clouds, drifting and flapping on the same loop the
  clouds and the sun's rays already ride. `scripts/gen-splash.sh` renders
  it, and now runs where ImageMagick 7 is the only ImageMagick: the glow
  layers are flattened onto black before they are screened, so the blend
  no longer depends on how a given version treats a transparent source,
  the long MVG primitives are read from files rather than the command
  line, and `gifsicle` is optional.
* Table cells centre their content vertically again. A cell with no
  `vertical-align` of its own fell back to the initial `baseline`, so in a
  row taller than the cell's own line the text sat at the top. Every
  browser's user-agent sheet gives cells `middle`, which is what pages
  written as tables expect: on Hacker News the title beside the logo sat
  two pixels high. Cells now default to `middle`, and an author rule or a
  `valign` attribute still overrides it.
* An image is drawn inside its own borders, padding and margin. The
  painter placed the bitmap at the box's margin-box origin and gave it the
  content size, so anything between that origin and the content box was
  painted over: a bordered image covered its own top and left borders, and
  a margin shifted the picture instead of the box. On Hacker News the logo
  is an 18-pixel image with `border:1px white solid`, and its top and left
  edges were missing. Replaced content now starts where the content box
  starts, the way inline SVG and MathML already did, and the placeholder,
  alt text and drop shadow follow it.
* The toolbar reads like a browser toolbar again: back and forward are
  green, reload is blue, and a red stop button sits between reload and
  home, appearing only while a page is loading. Stop is real -- it marks
  the in-flight frame stale so the page stops changing, ends the loading
  state and drops the busy cursor -- though it does not abort the network
  request behind it.
* The window's title bar is shorter, which gives the page the height back.
  The home button is set off from the address bar rather than sitting flush
  against it, and the security shield beside the address is drawn smaller so
  it reads as an indicator rather than another toolbar button.
* `about:northstar` lists the user agent, resolved the way a request
  resolves it, so it shows what is actually sent rather than a constant.
* Dynamic `:has()` selectors now update after class, attribute and child-list
  mutations. The incremental restyle index used the selector's final subject
  as the mutation key, so `div:has(+ .test) #subject` indexed `#subject`
  instead of the `div` whose match changes. It now indexes the compound that
  owns `:has()` and invalidates its descendant and following-sibling dependent
  region. The selector-invalidation WPT subset gains 311 passing subtests with
  no regression, while typical mutations in its largest file still recompute
  only 8–10 styles rather than the whole document.
* `border-radius: 50%` -- the way a page makes a circular avatar --
  painted a 50-pixel corner. The painter read the radius out of the
  computed value and ignored its unit, so every percentage radius became
  a pixel count and a `calc()` radius was dropped altogether. Radii now
  resolve against the border box, the horizontal one against its width
  and the vertical against its height. The elliptical forms work as
  well: `border-top-left-radius: 10px 20px` used to be rejected and
  `border-radius: 10px / 20px` kept only the horizontal radii, while
  through the CSSOM the whole declaration was thrown away. A corner now
  carries both radii and the painter draws elliptical corners, with
  overlapping radii scaled down together in the proportion the spec
  prescribes.
* Relative colour syntax is implemented -- `rgb(from <color> r g b)` and
  the `from` form of `hsl()`, `hwb()`, `lab()`, `lch()`, `oklab()`,
  `oklch()` and `color()`, including `calc()` over a channel keyword, as
  in `hsl(from red calc(h + 120) s l)`. sRGB to Lab, HSL and HWB
  conversions and the inverse of each predefined-space transform are
  added; the inverses are derived from the matrices already used in the
  forward direction, so a `color(display-p3 from ...)` round trip cannot
  drift.
* The six colour functions each had their own argument loop, and each
  accepted whatever its loop happened not to reject: `rgb(1 2 3 4 5)`,
  `rgb(0,0,0,0,0)`, `rgb(10, 20 30)`, `rgb(10 20 30, 0.5)` and
  `hsl(120 50% 50% extra)` all parsed, and a colour split over two lines
  in a stylesheet failed to parse at all because the loops skipped only
  spaces. One scanner now serves all six: it enforces the legacy comma
  form and the modern whitespace-with-slash form as alternatives rather
  than a free mixture, and rejects anything after the arguments. Two
  value bugs fell out of the merge -- `lch()` scaled a percentage chroma
  by the factor `lab()` uses for its axes, so `lch(50% 20% 40)` gave
  C=25 where CSS Color 4 specifies 30, and `hwb()` had a ternary whose
  branches were identical.
* CSS tokenization closes an open function at end of input, so
  `el.style.color = "rgb(1,2,3"` sets the colour and leaves the rest of
  the inline style alone. Northstar spliced the raw text into the style
  attribute instead, where the unclosed paren swallowed every
  declaration after it: setting one property through the IDL attribute
  or through `setProperty` discarded the whole block, and `cssText`
  stored text that degraded further on each read and rewrite.
* `align-items: baseline` and `align-self: baseline` fell through to
  flex-start, so a label beside a larger heading sat with its top flush
  instead of its text on one line. An inline box now keeps the baseline
  of its first line and a flex item takes the first baseline found among
  its in-flow children, synthesizing one at the bottom margin edge when
  there is no line box in it.
* `el.style.overflow = "hidden auto"` and `el.style.gap = "10px 20px"`
  silently dropped the declaration while the same value in a stylesheet
  worked, because both names carry a property id of their own and the
  CSSOM validated them against that single-value grammar instead of the
  expansion the stylesheet parser performs. `grid-area` never set
  `grid-row-start` and its three siblings, and `overflow` with a single
  value left `overflow-x` and `overflow-y` reading `visible` on an
  element that was in fact clipping. The four-value shorthands --
  `margin`, `padding`, `border-width`, `border-color`, `border-style` --
  ignored a fifth value rather than rejecting the declaration.
* `hypot()` always returned a plain number, so `hypot(3px, 4px)` was
  rejected by every property that wants a length. An integer property
  given an overflowing calculation kept the raw double, so
  `z-index: calc(infinity)` computed to `inf` and `calc(NaN)` to `nan`;
  both now clamp as CSS Values requires. Numbers large enough to reach
  exponent notation serialized through `%g` as `1.23457e+06px`, wrong
  and lossy. A transform built from 3D functions serialized as a 2D
  `matrix()` whenever the resulting matrix happened to be flat, and the
  specified value of `translate3d()` and `scale3d()` dropped its Z
  component entirely.
* The CSS cascade indexes `:is()` and `:where()` subjects, and stops
  allocating per selector test. Profiling ten real sites put the cost of a
  page load in the cascade rather than in layout: on github.com a relayout
  spent 1.5-2.2 seconds matching selectors against 1635 elements, twice per
  relayout because container queries cascade a second time, against roughly
  0.5 s for layout itself. Counting candidates showed why -- 83% of the
  rules tested against each element came from the index's catch-all bucket,
  and the biggest group in it was rules whose subject is only `:is(...)` or
  `:where(...)`, which carry no name of their own to file under. An element
  can only match such a rule by matching one of the arms, so when every arm
  ends in an id, class or tag the rule is now filed under each arm's key
  instead of the catch-all; an arm without one (a bare pseudo-class, `*`)
  still falls back. On github.com that removes 25% of all candidate tests
  and 24% of the selector matches. Separately, the selector cache -- which
  exists so the container-query pass can reuse the first pass's results --
  allocated a key and a value for every test, about a million allocations
  per pass, none of which the first pass can ever hit, since a cascade never
  probes the same rule, selector and element twice. Entries now come from a
  bump arena with the result stored inline. On a page built to exercise
  this, 3200 elements against 3200 rules of which a third are `:is()` or
  `:where()` unions, the initial cascade falls 52% (1561 ms to 753 ms,
  median of nine) and the whole headless run 16%. Pages whose CSS does not
  use those selectors are unchanged: a text-heavy page's cascade moves from
  2.4 ms to 2.2 ms and its run time distributions overlap. Rendering is unchanged -- 32 layout and text dumps are
  byte-identical to the previous build -- and the `css` and `dom`
  web-platform-test subset gains 826 subtest passes, from tests that
  previously ran out of time, with no subtest regressing.
* Text is laid out through ns-pango, a fork of Pango carried as a meson
  subproject, instead of the system Pango. Pango keeps no cache that
  outlives a `PangoLayout`, so the same bytes were shaped by HarfBuzz
  once to measure a run and again to paint it, and a table cell was
  shaped for `min-content`, for `max-content` and once more to lay out.
  The fork adds a process-wide cache of finished glyph strings keyed on
  everything HarfBuzz reads -- font, bidi level, gravity, script,
  language, analysis and show flags, text transform, OpenType features
  and the item bytes -- and caches `pango_context_get_metrics` per font
  description, which resolving `line-height: normal` asks for on every
  inline run. Every symbol in the fork is renamed, because GTK loads
  the system Pango into the same process and GObject aborts when two
  libraries register the same type name. Layout time on a page that
  renders as well as lays out falls 25% on a text-heavy page and 38% on
  a table-heavy one; laying out alone, where nothing is shaped twice
  except for intrinsic sizing, the table page falls 29% and text is
  unchanged. Shaping results are unchanged: a corpus covering RTL and
  bidi, CJK, the white-space modes, intrinsic sizing, letter- and
  word-spacing, tabs, ellipsis, multi-column, inline atomics,
  decorations, small-caps and font features renders byte-identically,
  and the cache's own verification mode reports no difference between
  cached and freshly shaped runs anywhere in it, nor over 220 real
  web-platform-test pages. The `css` and `dom` test subset -- 1761 tests,
  35929 subtests -- reports no subtest that passed before and fails now.
* The DOM insertion methods run the insertion steps. `append`,
  `prepend`, `before`, `after`, `replaceWith` and `replaceChildren`
  moved nodes into the tree without the work `appendChild`,
  `insertBefore`, `replaceChild` and `insertAdjacentElement` already
  did, so a `<script>` inserted through any of them never executed and
  a custom element never got its `connectedCallback`. Cloning a
  `<template>`'s content and handing it to `replaceWith` -- the
  ordinary way to stamp a template -- therefore dropped every script in
  it. `innerHTML` still marks its scripts already-started, so it keeps
  not executing them.
* `getComputedStyle` resolves every property it enumerates. It listed
  218 properties but returned the empty string for 126 of them, because
  the initial-value fallback was a hand-written `strcmp` chain covering
  about fifty longhands -- so reading `flex-grow`, `align-items`,
  `max-width` or `object-fit` off an element that never set them gave
  "" rather than the initial value. Properties whose initial value is
  `currentcolor` resolve to the element's computed `color`, and an
  inherited property with no entry of its own walks up to the nearest
  styled ancestor, so elements outside the styled tree report inherited
  values instead of "".
* Computed `<position>` values are normalized. `background-position`
  and `object-position` kept their specified text, so `10% center`
  stayed `10% center` instead of resolving to `10% 50%`, and the
  four-value edge-offset form was mis-split: `right 30% top 60px`
  produced x=100% y=30% rather than x=70% y=60px. Both shorthands now
  share one splitter that resolves an edge keyword against its offset,
  and assemble their computed value from the two longhands.
* `transform-origin` and `perspective-origin` serialized as
  `translate(0%, 0%)`, which is not a valid value for either property.
  They resolve against the border box and serialize as lengths, so
  `left top` reads back as `0px 0px` and `center` on a 100x50 box as
  `50px 25px`.
* The CSS `color()` function parses. `color(srgb ...)` and the
  `srgb-linear`, `display-p3`, `a98-rgb`, `prophoto-rgb`, `rec2020`,
  `xyz`, `xyz-d50` and `xyz-d65` spaces convert to sRGB, with number,
  percentage and `none` components and an optional alpha. Previously
  the whole declaration was dropped as invalid.
* `docs/compliance.md` records where the engine stands against the HTML
  and CSS specifications, how to reproduce the web-platform-tests
  scores, and the known structural gaps.

1.0.5:
======
* Extended-container WebP images decode. Wuffs accepts only a bare
  `RIFF....WEBP` holding one `VP8 ` or `VP8L` chunk, and rejects the
  `VP8X` container outright -- which is what every encoder emits for a
  lossy image with transparency, so those failed with "could not decode
  image" rather than rendering. The container is now unwrapped to the
  bitstream Wuffs understands, and the `ALPH` chunk is decoded here:
  uncompressed alpha directly, lossless alpha by prefixing a synthesized
  `VP8L` header onto the stream and reading the green channel back out,
  then unfiltering with the horizontal, vertical or gradient predictor.
  The recovered alpha plane is bit-exact against libwebp for every
  filter and both compression methods.
* The still frame lifted out of an animated WebP decodes. It was
  reassembled without RIFF's even-size padding, so a chunk of odd length
  produced an odd `RIFF` size that Wuffs rejects before reading anything,
  and it prepended an `ALPH` chunk that Wuffs cannot parse at all.
* A single-frame MPEG-1 clip displays instead of failing. Both decode
  paths treated a frame list as an animation only when it held more than
  one frame, and discarded a shorter one to retry through the still-image
  decoder -- which has no MPEG-1 support, so a one-frame video decoded
  correctly and was then thrown away. A list of one frame is now kept and
  shown as a still; `ns_image_is_animation` still requires two, so nothing
  starts ticking for it.
* `<video>` responds to the media element API. A decoded clip reports its
  real `duration` and a `readyState` of `HAVE_ENOUGH_DATA`, `paused`
  reflects whether the frames are actually advancing, `play()` and
  `pause()` start and stop them, and assigning `currentTime` moves the
  displayed frame -- seeking to 0.20 s in a 25 fps clip shows frame 5, not
  merely a changed number. Reading `currentTime` reports the live position.
  Playback state lives on the decoded clip, which the image cache keys by
  URL, so two `<video>` elements sharing one source share playback.
  A clip still starts playing on load and loops: it is always silent, so
  this matches what Chrome allows for muted video, and Northstar has
  neither video controls nor click-to-play to start it otherwise.
* `readyState` on `<video>` and `<audio>` is no longer always zero. The
  polyfill that gives `<track>` elements a `readyState` replaced the
  property on the media prototype and delegated to the native getter it
  had captured -- but it looked that getter up as an own property of the
  element's immediate prototype, where it does not live, so the captured
  descriptor was null and every media element reported zero regardless of
  state. The lookup now walks the prototype chain.
* `<video>` plays MPEG-1. The tree already vendored pl_mpeg for its MP2
  audio decoder and switched the video half off with one call
  (`plm_set_video_enabled(plm, 0)`), so the decoder for an ISO standard
  whose patents have expired was being compiled and discarded. `video.c`
  turns it back on: an MPEG-1 Program Stream or elementary video stream is
  recognised by its start code, and every frame is decoded to the same
  `ns_image_pixel_frame` list an animated GIF produces. The image cache's
  fetch, frame timing, repaint scheduling and eviction then serve video
  with no new machinery, and `paint_video` draws the frame where it drew a
  placeholder. A `<video>` sizes to its intrinsic dimensions and keeps its
  aspect ratio when given only `width` or `height`, and `canPlayType`
  answers for `video/mpeg`. No new dependency: pl_mpeg moves from the
  audio helper's link line to the engine's.
  Decoding is up front rather than streamed, so a clip is bounded by
  `NS_VIDEO_MAX_FRAMES` and 256 MB of decoded pixels and a longer one
  plays its prefix. MPEG-1 is not a format the modern web serves; this is
  video for local and self-hosted clips, not for streaming sites.
* The vendored pl_mpeg no longer reads past a frame plane. Half-pel motion
  compensation samples `s[si + 1]`, `s[si + dw]` and `s[si + dw + 1]`, but
  `plm_video_process_macroblock` bounds only `s[si]`, so a macroblock on
  the bottom row reads up to one row plus one byte beyond the plane it
  samples -- absorbed by the next plane for interior planes, and off the
  end of the allocation for the last one. Fuzzing the decoder under
  AddressSanitizer with mutated streams reported it as a heap-buffer
  overflow read. The three frames are allocated as one chunk, which is now
  padded by that overshoot and zeroed, so the read stays inside the
  allocation and a corrupt stream decodes deterministically. Valid video is
  unaffected: no bound is tightened, so no macroblock that decoded before
  is rejected now. The overshoot was unreachable until this release
  because the video decoder was switched off.
* Animated images decode as animations on the engine's own fetch path.
  `ns_image_decode_body` routed GIF and APNG to the animation decoder, but
  the two fetch handlers in `engine.c` -- the ones headless rendering and
  the browser's own image pass use -- called `ns_image_decode_bytes`
  instead, which only ever returns a still frame. An animated GIF fetched
  through those paths therefore froze on frame one. Both now go through
  `ns_image_cache_insert_encoded`, and the still-versus-animated decision
  lives in one function rather than three copies that had already drifted.
* Animated PNG plays. Wuffs already decoded APNG frames -- the animation
  loop that GIF uses is format-agnostic -- but two things kept it from
  running: the callers only routed GIF magic to the animation path, and
  the animation decoder itself hardcoded the GIF signature check and the
  GIF decoder, so a PNG handed to it was rejected before it started. Both
  are now driven by the same format detection the still path uses. An
  APNG is recognised the way the spec defines it, by an `acTL` chunk
  appearing before the first `IDAT`, so an ordinary still PNG never pays
  for the animation decoder.

* WebP images decode. The vendored Wuffs already carried its WEBP and
  VP8 modules, and the build already enabled them, so lossy VP8,
  lossless VP8L and alpha all decode through the same memory-safe path
  as PNG, GIF, BMP and JPEG -- no new dependency, no new decoder, and
  `image/webp` now appears in the `Accept` header so content-negotiating
  servers will send it. Wuffs decodes only still WebP: an animated file
  is reduced to its first frame by walking the RIFF container for the
  first `ANMF` chunk and re-wrapping its `VP8 `/`VP8L` payload (with any
  `ALPH`) as a still image. Without that, advertising `image/webp` would
  have made pages worse, because a server picking between animated WebP
  and animated GIF on the strength of the header would have started
  sending a format that rendered as nothing.

* gdk-pixbuf no longer decodes page images. Every format the web
  actually uses is already handled in-tree -- ICO, then Wuffs for PNG,
  GIF, BMP and JPEG, then libavif, then the in-engine SVG renderer --
  so the pixbuf fallback had been reduced to TIFF, TGA, PPM and ICNS,
  none of which Chrome or Firefox render either. What it cost was the
  ability to know what parses untrusted bytes: `gdk_pixbuf_get_formats`
  enumerates loader plugins installed on the user's machine, so the set
  of decoders reachable from a web page was decided at runtime, varied
  per system, and could not be audited from the build. Those loaders
  also sit outside the memory-safety guarantee the Wuffs decoders were
  chosen for. The decode chain now ends after SVG: an unsupported
  format fails to decode instead of falling through to a plugin. GTK 4
  still depends on gdk-pixbuf for its icon theme, so a desktop build
  links it either way -- what goes away is the browser feeding it. An
  engine-only build (`-Dgtk=disabled`) now drops the dependency
  outright. `ns_image_pixbuf_supports_mime` is renamed
  `ns_image_supports_mime`, since it no longer speaks for a plugin set.

* libavif is optional. It was a hard `dependency()`, so a tree without
  it would not configure at all, even though every AVIF call site was
  already behind `NS_HAVE_AVIF` and `image_avif.c` was already compiled
  conditionally. The new `avif` meson feature defaults to `auto`, so a
  host that has libavif still decodes AVIF exactly as before;
  `-Davif=disabled` drops the dependency and AVIF images simply fail to
  decode. This matters because libavif pulls in a complete AV1 decoder
  (dav1d or libaom) for a format that is rare on the web, which is a
  large thing to require of anyone building from source.

* `var()` resolves inside SVG presentation attributes. A custom property
  set by a stylesheet rule now reaches `r="var(--radii)"` or
  `fill="var(--tint)"`, so a class can retheme an inline icon's colour
  and geometry the way it does for ordinary CSS properties.

* `marker-start`, `marker-mid` and `marker-end` draw their `<marker>` on
  path, line, polyline and polygon vertices. Vertices and their tangents
  come from the built Cairo path, so arcs and curves orient the same way
  straight segments do, and a mid vertex uses the bisector of its two
  tangents. `markerUnits="strokeWidth"` scales the marker with the
  stroke, `orient="auto"` and `auto-start-reverse` rotate it, and the
  marker viewport clips unless `overflow` says otherwise. `refX`/`refY`
  are mapped through the marker's own `viewBox` before positioning, so
  the reference point lands on the vertex.

* `mask` is honoured on SVG elements. The referenced `<mask>` renders to
  an offscreen surface whose sRGB luminance becomes the alpha the element
  is composited through, so a white mask shows the element, black hides
  it, and a gradient fades it. Group opacity and masking combine.

* A square border is painted inside its border box rather than centred
  on the edge. Each side was stroked along the border-box boundary with
  the line width set to the border width, and Cairo centres a stroke on
  its path, so every bordered element rendered half a border wider than
  it laid out on each side -- a 4px border occupied 6..9 and 60..63
  where the box model puts it at 8..11 and 58..61. Layout was always
  right; only the paint was wrong, so borders overlapped whatever sat
  next to them. Rounded borders already inset correctly and are
  unchanged.

* SVG is rendered by the engine instead of librsvg. `librsvg` is gone
  from the dependency list; `src/svg.c` walks the SVG DOM and paints it
  through the same Cairo surface, cascade and font stack that HTML uses.
  Inline `<svg>` was previously re-serialised to XML and handed to
  librsvg as an opaque raster, so the document's own stylesheet could
  never reach inside it: `fill: currentColor`, `svg .icon { fill: … }`
  and any script-driven change to SVG geometry were invisible. SVG
  elements now take part in the normal cascade, so `fill`, `stroke`,
  `stroke-width`, `stroke-dasharray`, `fill-rule`, `stop-color`,
  `text-anchor`, `paint-order` and the SVG geometry properties `x`, `y`,
  `cx`, `cy`, `r`, `rx`, `ry` are real CSS properties that inherit and
  animate like the rest. Covered: paths (including elliptical arcs and
  smooth curve continuation), rect/circle/ellipse/line/polyline/polygon,
  `viewBox` and `preserveAspectRatio`, nested `<svg>`, `<g>`, `<use>`,
  `<symbol>`, `<switch>`, `<defs>`, linear and radial gradients with
  `href` inheritance, `spreadMethod`, `gradientUnits` and
  `gradientTransform`, `clipPath`, group opacity, dashing, and `<text>`
  shaped through Pango. A standalone `.svg` document now sizes to the
  viewport rather than to a 300x150 default. `<img src="…svg">` and
  CSS `url(…svg)` go through the same renderer.

* Media queries inside a frame evaluate against the frame's own size,
  not a 300x150 guess. The viewport pushed while collecting a frame's
  stylesheets came from the frame's inline `style` attribute or its
  `width`/`height` content attributes, so a frame sized by a stylesheet
  rule -- `iframe { width: 100% }`, the common responsive-embed pattern --
  was measured as 300x150 and its `@media (min-width: ...)` blocks
  resolved against a size the frame never had. Layout now records each
  frame's content box, collection prefers it over the default, and when
  the recorded size disagrees with the one a viewport-dependent frame
  sheet was collected under, style and layout run once more so the frame
  settles on its real size. Frames whose CSS carries no width, height,
  aspect-ratio or orientation query never trigger the extra pass. Acid3
  test 46 passes as a result; Acid3 now scores 99/100, up from 98/100.
* A frame document's own stylesheet can style its root element. Sheets
  inside an iframe are rewritten to be scoped to the frame's root, and
  every selector whose subject was not literally `html` or `:root` got a
  descendant combinator — so `* { … }` or `.cls { … }` in a framed
  document matched everything inside the frame except the frame's own
  `<html>`, and `getComputedStyle` on that element reported no value for
  any property. The scope marker now also attaches directly to the
  subject compound, and lands before a pseudo-element rather than after
  it. Shadow scopes are unchanged: a shadow host is still not styled by
  its own shadow tree. Acid3 test 41 passes as a result.
* `:empty` is re-evaluated when a text node gains or loses content.
  Writing to `.data`/`.nodeValue`, or calling `appendData`,
  `insertData`, `deleteData` or `replaceData`, changes whether the node
  counts towards its parent's emptiness, but only child-list mutations
  marked the parent for restyle, so an element that became non-empty by
  having text written into an existing empty child kept its stale
  `:empty` match. Acid3 test 38 passes as a result; Acid3 now scores
  98/100, up from 96/100.
* Removed the IE-only `attachEvent` and `detachEvent`. They were exposed
  on Element, Document and Window as no-op stubs that returned true and
  registered nothing. Libraries still feature-detect them to select a
  legacy path: RequireJS, finding a native-looking `attachEvent`, bound
  its script-load callback to `onreadystatechange` instead of
  `addEventListener`, the stub swallowed it, and every module load ended
  in "Load timeout for modules". jQuery's test suite could not get past
  its RequireJS bootstrap before this.
* `DOMParser` reports the line and column of an XML parse error. The
  synthesized `parsererror` document carried the bare text "XML parsing
  error"; it now names the position the parser stopped at.
* `Array.prototype.sort` calls the comparator for identical elements.
  quickjs-ng skips the call when two slots hold the same JSValue and
  assumes the comparator would have returned 0. That is permitted by
  ECMAScript, which does not prescribe which comparisons a sort makes,
  but jQuery's `uniqueSort` learns that a collection holds duplicates
  precisely by being invoked with `a === b`, so duplicate nodes survived
  `.siblings()`, `.parents()`, `.nextAll()`, `.prevAll()`, `.closest()`
  and `.addBack()`. Carried as a wrap patch alongside the existing one,
  so the engine is still consumed unforked. jQuery's traversing module
  goes from 12 failing tests to 2.
* `getComputedStyle(el).someUnknownName` is `undefined` rather than the
  empty string. The proxy in front of a computed declaration answered
  every string key through `getPropertyValue`; its `has` trap already
  distinguished supported properties from unknown ones, and `get` now
  draws the same line.
* Fixed a double free of the response `Vary` header on the cancelled-fetch
  path in `net.c`. Cancelling a load — navigating away, pressing stop —
  freed `header_ctx.vary` twice, corrupting the heap whenever the
  response carried a `Vary`, which on the real web means most of the
  time.
* `Vary: Origin` no longer defeats the HTTP cache. `Origin` is now one of
  the headers the cache can resolve at lookup time: `net.c` computes the
  value it will send once and uses that same string both as the request
  header and as the cache selector, so the two can never disagree, and
  the absence of an `Origin` selects distinctly from any present one.
  Google serves its stylesheets `public, immutable, max-age=31536000`
  with `Vary: Origin`; those were being refetched on every load and are
  now cached.
* ES module fetches join the same request identity as every other
  subresource. The module loader passed no top-level URL, so a module was
  partitioned in the HTTP cache under its own site rather than the
  document's — two unrelated sites importing the same module shared one
  cache entry — and it neither coalesced with nor consumed the preload
  issued for the same `<script type=module src>`, since that preload
  carries the JavaScript `Accept` and the module fetch did not. It now
  passes the document URL and the script `Accept`.
* Shutting down no longer hangs a caller waiting on a coalesced fetch.
  `ns_net_drain` discarded queued fetch tasks without telling the
  coalescer, so a task that led a group left the group behind: blocking
  joiners waited on a condition nobody would signal again, and
  asynchronous joiners never had their callback run. A blocking joiner
  now also gives up when the network layer starts aborting, and in any
  case five seconds past the longest transfer timeout a leader can have,
  instead of waiting without a bound. These waits happen on worker
  threads that teardown joins, so one that never returned took the
  joining thread down with it.
* Dedicated and service workers expose the standard `Headers` interface.
  Worker-created and fetched `Request` and `Response` objects now retain
  case-insensitive header lookup and the other `Headers` methods instead of
  carrying a plain object. This lets Workbox inspect `Vary` while populating
  its caches rather than rejecting the cache operation.
* An `<iframe>` becomes visible as soon as its document loads, on a
  quiet page as well as a busy one. The UA sheet hides frames until the
  engine stamps `data-nd-frame-loaded` on them, but that stamp is
  written by the loader rather than through the scripted attribute
  path, so it never invalidated style. The frame kept the cached
  `display: none` and produced no box at all — its document parsed and
  its scripts ran, entirely unpainted — until some unrelated mutation
  happened to force a restyle. Pages with continuous script activity
  masked it; a page whose only content was a frame never showed it. The
  three places that add or remove the attribute now mark it dirty.
* Inline atomic boxes contribute their full height to the individual
  wrapped line that contains them. Multi-line form controls and table
  cells now reserve the correct vertical space instead of allowing later
  lines to overlap following content, fixing the Google footer position.
* The HTTP cache selects the right variant of a negotiated response.
  `cache.c` keyed entries on URL and partition and stored nothing about
  `Vary`, so a resource served `Vary: Accept` and referenced both as a
  stylesheet and as a script was fetched once and that single variant
  handed to both — a `<script>` element could receive CSS. Entries now
  carry the response's `Vary` and are keyed on a selector built from the
  request headers it names, with an indexed base key so a lookup can walk
  the variants stored for a URL and match the right one. `Accept`,
  `Accept-Language` and `User-Agent` are resolved; `Accept-Encoding` is
  ignored because bodies are stored decoded, which keeps the web's most
  common `Vary` from fragmenting the cache; anything else, including
  `Vary: *`, is not stored rather than stored wrongly. The preload scan
  deduplicates candidates on (URL, destination) instead of URL alone, so
  both variants are preloaded. The cache schema is versioned through
  `PRAGMA user_version` and an upgrade discards the old cache.
* The speculative preloader hands its bytes to the loader that needs
  them through a single deduplication point keyed on the request's
  identity. Preload responses used to be parked in a private store
  keyed on the bare URL and consulted ahead of the HTTP cache. That
  store ignored the cache partition, so within its 20-second window one
  site could be served bytes another site had fetched with that site's
  cookies; it ignored `no-store`; it recorded a placeholder for every
  fetch it started but only removed entries when a loader consumed one,
  so failed preloads and preloaded images — which nothing consumed —
  permanently occupied its 32 slots until the preloader silently
  stopped preloading anything. Deduplication now happens in one place.
  The in-flight coalescer keys on method, URL, cache partition and
  request headers rather than URL plus referrer, and every entry point
  joins it — `ns_net_request_async` and the blocking fetchers as well
  as `ns_net_fetch_async` — so a loader that arrives while a preload is
  still in flight waits for it instead of issuing a second request. A
  preload that finishes first is held in a preload map under that same
  key, handed over by the fetch layer itself so there is no window in
  which a resource is in neither place, and dropped when the next
  navigation begins. The preloader now sends the `Accept` header its
  consumer will send, so content-negotiated resources match. The
  separate external-script prefetcher, a third path over the same URLs,
  is gone. A page with six scripts and five stylesheets issues exactly
  one request per resource.
* Concurrent fetches of the same subresource share one network
  request. Three separate paths ask for a page's scripts and
  stylesheets — the speculative preloader, the external-script
  prefetcher, and the loader that actually consumes the bytes — and
  each issued its own request. Because they overlap, none of them
  could ever hit the HTTP cache, so an ordinary page fetched every
  script three times and every stylesheet twice, as confirmed at the
  origin. `ns_net_fetch_async` now keys uncancellable GETs on URL plus
  top-level URL, lets the first caller do the transfer, and hands each
  later caller its own copy of the response.
* `Worker.postMessage()` can transfer a `MessagePort` that also appears
  inside the message payload. The structured clone now substitutes the
  transferred endpoint during serialization, restores it in the receiving
  worker, and exposes the same port object through both `event.data` and
  `event.ports`; duplicate ports in a transfer list raise `DataCloneError`.
  This allows worker-backed consent and advertising libraries used by large
  news sites to initialize instead of failing before their message channel is
  connected.
* The root element's font reaches the rest of the page. The UA
  stylesheet declared `font-family: serif` on `html, body` together,
  and a UA declaration on `body` outranks inheritance from `html` — so
  a page styling only `html` (`html{font-family:"Helvetica Neue",
  "Segoe UI",Arial,sans-serif}` on lite.duckduckgo.com) had its font
  dropped at `body` and rendered in the UA serif default. The
  declaration now sits on `html` alone and `body` inherits it.
* A concrete font family is used when the system actually has it.
  `Arial`, `Helvetica`, `Segoe UI`, `Roboto` and the SF Pro names were
  rewritten to generic `sans-serif` unconditionally, which resolved
  through fontconfig to whatever the default sans happened to be —
  Noto Sans rather than the requested Segoe UI or Arial. Each name is
  now resolved against the installed families first, and substituted
  by `sans-serif` only when it is missing.
* Container queries no longer defeat incremental restyle. ns_css_compute
  runs twice per relayout when a page has containers, and the second
  pass — the one with the container map set — failed the incr_want test
  and then took the branch that frees the previous pass's computed
  styles. The next relayout therefore always started with an empty
  cache, so any page using `@container` re-cascaded every element from
  scratch, forever. The cache is now only discarded when incremental
  restyle is genuinely unusable, not merely because this is the
  container pass. Rules carrying a container condition also no longer
  contribute conservative invalidation keys: container_cond_matches()
  returns false whenever there is no container map, so those rules
  cannot affect the first pass, which is the only one incremental
  restyle runs in. On a 1610-element container-query page over 13
  relayouts, cascade time drops from 39ms to 8ms and style reuse goes
  from 0 to 1609 of 1610 elements per pass.
* The intrinsic width of a replaced box prefers its specified `width`
  and its decoded intrinsic size over the 200x150 placeholder an
  `<img>` without `width`/`height` attributes is given while it loads.
  Shrink-to-fit contexts — a floated `<a>` around a thumbnail above all
  — measured the placeholder, so the float reserved 200px while the
  image painted at its real width and the text ran underneath it.
* A flex item in a column container is clamped by its own `max-height`
  and floored by its own `min-height`. Only an explicit `min-height` was
  consulted, and only to keep a shrinking item from collapsing, so
  `height: calc(100% - 560px); max-height: 290px` kept the full
  calculated height and pushed everything below it down the page.
* A block that does not establish a block formatting context no longer
  grows to enclose the floats inside it. The float is registered with
  the enclosing formatting context instead, so it keeps shortening line
  boxes in the blocks that follow — a floated thumbnail followed by
  sibling `<div>`s now has the post text beside it rather than beneath
  a container stretched to the float's height.
* `window.scrollX`, `window.scrollY`, `pageXOffset` and `pageYOffset`
  are read-only accessors over the real viewport offset instead of
  writable data properties. Page script that assigned to one of them
  poisoned every reader, including `getBoundingClientRect`, which then
  reported every element at the top of the viewport.
* A flex item's content-based base size no longer has the item's own
  padding and border subtracted from it. `measure_natural_width` already
  returns a content-box size, so a padded item — a `<button>` above all —
  was assigned a base size that was short by exactly its horizontal
  padding, and its label was clipped.
* Flex items honour the automatic minimum size of CSS Flexbox §4.5: an
  item with `min-width: auto` and visible overflow never shrinks below
  its min-content width, so the last item in an over-constrained row
  keeps its label instead of being squeezed to nothing.
* `margin-left: auto` and `margin-right: auto` centre — or, on their
  own, right-align — a block-level replaced element, not just a table.
  A centred `<img>` whose used width came from `max-width` stayed
  against the left edge of its containing block.
* about:start sizes the search row to the splash image, so the two share
  the same left and right edges.
* Flex items are sized by the flex algorithm rather than by their own
  `width`. `layout_block` read `width` back out of the style and ignored
  the main size the container had assigned, so nothing ever shrank —
  `flex-shrink: 1` is the initial value, so every over-constrained flex
  row overflowed instead of fitting.
* The flex main axis is reversed when exactly one of
  `flex-direction: row-reverse` and `direction: rtl` applies, and items
  are then packed from the opposite edge, on wrapping and non-wrapping
  rows alike. `row-reverse` used to reverse the item order but still pack
  against the left edge, and `rtl` was ignored for the main axis.
* Grid row placement added the item's top margin on top of the
  margin-box origin that `ns_box.y` already means, so a negative margin
  moved the item the wrong way by twice the amount and cut the
  container's scrollable overflow to match.
* Text now wraps around floats the way CSS 2.1 §9.5 describes. Floats
  intrude into the line boxes of nested blocks in the same block
  formatting context, and an inline run that crosses the bottom of a
  float is split so the lines below it reclaim the full width — a long
  paragraph next to a short floated image no longer stays in a narrow
  column all the way down.
* Only tables, block-level replaced elements and boxes that establish a
  new block formatting context are moved aside by a float. Every other
  in-flow block keeps its containing block's width and overlaps the
  float, so backgrounds, borders and percentage widths next to a float
  resolve against the right width.
* CSS `display` is now a structured computed value (outer type, inner
  type, list-item flag, layout-internal kind) instead of a keyword string.
  Two-value syntax such as `display: flow-root list-item` reaches layout
  correctly; previously such elements lost their boxes. `-webkit-box` and
  `-webkit-inline-box` map to flex and inline-flex.
* Anonymous table boxes are generated around any run of table-internal
  siblings, so `display: table-row` and `display: table-row-group` outside
  a table lay out as tables instead of collapsing into surrounding text.
* Blockification follows the spec: the root element blockifies
  (`display: contents` on `<html>` computes to `block`) and flex and grid
  items report their blockified `display` to script, including items
  nested inside a `display: contents` wrapper.
* Cascade layers are ordered as a tree rather than by first-declaration
  order across the whole document: sublayers sort inside their parent, a
  layer's own declarations act as its implicit final sublayer, and nested
  anonymous layers stay nested instead of escaping to the top level.
* The incremental restyle pass identifies stylesheets by a parse-time
  serial instead of by address. A reparsed `<style>` reusing the freed
  block of the sheet it replaced could look unchanged and leave stale
  styles behind.
* `@scope` preludes are parsed against the grammar and invalid ones drop
  the rule; the prelude is serialized canonically.
* `StyleSheet.media` is a live `MediaList` that writes back to the owner
  node's `media` attribute.
* A unitless `0` on a length property computes to `0px`. Elements with a
  box hid this because their box edges are read back from layout; on an
  element with no box every such property reported `0`, and the inset
  resolver rejected `left: 0`, so the opposite inset of an out-of-flow
  box came back as `auto` instead of its used value.
* `getComputedStyle` returns an empty declaration list for an element
  that is not rendered — not connected, or outside the flat tree — as
  CSSOM requires, instead of a full style.
