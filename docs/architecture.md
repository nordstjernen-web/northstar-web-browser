# Northstar Web Browser architecture

![Northstar Web Browser architecture diagram](architecture.svg)

This document maps the Northstar Web Browser codebase: the process and thread model,
the page-load pipeline, and which source file owns which job. It describes
the **minimalist GPL edition**, which is single-process and hand-written
(no forked browser engine). For the security posture of each layer see
[`../SECURITY.md`](../SECURITY.md).

## Process model

This edition runs the engine **in the shell process**. There is no
per-tab or per-origin renderer process; every page shares one address
space.

```
 watchdog supervisor  (watchdog.c, GUI launches only)
    │  re-executes the binary with --watchdog-child,
    │  restarts it on crash or hang, restores the session
    ▼
 browser process  (appmain.c → src/gtk/)
    ├─ GTK main thread: window, omnibox, menus, page view   (src/gtk/*.c)
    ├─ engine thread "ns-engine", own GMainContext         (enginethread.c)
    │     fetch → parse → style → layout → paint, page JS, timers
    ├─ curl multi thread + fetch tasks                      (net.c)
    ├─ background compile of large scripts (≤ 2)           (bytecode_cache.c)
    ├─ image decode tasks (MPEG-1 opened here)              (image.c, video.c)
    ├─ one video decode thread per MSE stream               (videoworker.c)
    │     └─ dav1d / libvpx / OpenH264 worker threads      (av1.c, vp9.c, h264.c)
    ├─ JS worker threads: Web Workers, service workers      (js.c)
    ├─ WebSocket and EventSource threads                    (ws.c, eventsource.c)
    ├─ audio mixer thread → SDL2                            (src/audio/audio.c)
    └─ download threads                                     (procwindow.c)

 headless / tooling  (--headless, --dump=, --eval=, --act=, --wpt, …)
    the same binary, never supervised, no GTK window       (headless.c)
```

- **Entry point** (`appmain.c`) — handles `--version`, refuses to run
  privileged, loads the UI catalogue and configuration, decides whether to
  supervise, applies the sandbox, parses the remaining flags and starts
  either the GTK shell or the headless driver.
- **Watchdog supervisor** (`watchdog.c`) — a normal GUI launch first
  becomes a small parent that initialises no network, sandbox or UI and
  runs the real browser as a child (`--watchdog-child`). The child beats
  a counter from the GTK main loop every two seconds; a thread in the
  child exits it when the beat stops for 60 seconds plus the configured
  JavaScript budget. The supervisor restarts a crashed or hung child with
  the saved session, and gives up after more than five failures within a
  minute. It is on by default (`--no-watchdog`, `NS_NO_WATCHDOG` or the
  `watchdog_enabled` setting turn it off); headless and tooling runs are
  never supervised. A hang on the engine thread alone does not stop the
  heartbeat, since it is the GTK loop that beats.
- **GTK shell** (`src/gtk/procwindow.c`) — the window chrome: toolbar,
  omnibox, bookmarks, history, downloads, find bar, settings, printing,
  zoom and full screen. *New Window* opens another window in the same
  process; each window shows one page — there are no tabs. Its own
  icons are the bundled SVGs drawn by the in-engine SVG renderer
  (`src/gtk/icons.c`), so they never depend on a GdkPixbuf SVG loader.
- **Page view** (`src/gtk/procview.c`) — the widget that shows a page.
  It turns input into typed requests (`REQ_LOAD`, `REQ_RENDER`,
  `REQ_CLICK`, `REQ_KEY`, `REQ_SCROLL`, `REQ_PRINT`, …), posts each as a
  job to the engine thread (`enginethread.c`), and receives the matching
  response — a frame surface plus title, URL, cursor, download and camera
  side information — back on the GTK main loop. Both ends are in
  this process; nothing is serialised.
- **Engine thread** (`src/gtk/enginethread.c`) — one dedicated thread
  with its own `GMainContext`, so page timers, fetch completions and
  nested settle loops never block the GTK main loop. Engine code attaches
  its timers and idle sources through `mainctx.c`, which resolves to that
  context in the GUI and to the default context headless.
- **Page session** (`page_session.c`) — owns the open page, a
  back/forward cache of up to four suspended pages, the pending POST body
  and the last frame. The view drives it on the engine thread.
- **Frame scheduling.** The view does not poll. A wake source on the
  engine context asks the view for a frame only when the page has
  something to show: a DOM mutation, a pending `requestAnimationFrame`, a
  loaded image, a canvas draw, a scroll or a navigation.
  The view then renders at the next GTK frame-clock tick, and it keeps a
  per-tick callback only while the page animates continuously (CSS
  animations, animated images or video, `requestAnimationFrame` loops,
  layout the dampener deferred) or a caret blinks. `requestAnimationFrame`
  and CSS animations are sampled at the frame clock's time. Canvas
  drawing repaints without a relayout, and wheel scrolls and resizes that
  arrive while one is in flight are merged into the next request.
- **Damage tracking** (`damage.c`, `page_session.c`). A canvas draw, an
  element scroll, an animated image or video frame and a caret blink
  report the element they changed, and the frame repaints only those
  boxes' rectangles; a scroll of the page shifts the previous frame and
  paints the rows that came into view plus the `position: fixed`
  elements. Anything else — a relayout, a restyle, a CSS animation, a
  generic repaint request — repaints the whole viewport, and so does a
  change under a transform or a sticky or multi-column ancestor, or on a
  page with 3D transforms; scrolling repaints everything on pages with
  sticky elements or `background-attachment: fixed`. Every frame is
  painted with 48 rows of margin beyond the viewport and each damaged
  band with the same margin, so text crossing a clip edge renders the
  same whether a pixel came from a full or a partial repaint. The view
  copies only the changed rectangles into its frame.
- **Page host** (`libnorthstar.c`) — the `ns_browser` object beneath the
  session: one document, its QuickJS runtime, input dispatch, the settle
  loop, find-in-page, printing and viewport scroll snapping. It is an
  internal interface; the embeddable library API of the full Nordstjernen
  edition is not part of this one.
- **Media controller** (`js_media.c`) — the `HTMLMediaElement` state
  machine: resource selection, `networkState`/`readyState`, `paused`,
  `ended`, `seeking`, `error`, the event sequence from `loadstart` to
  `ended`, the promise `play()` returns, `loop`, `muted`, `volume` and
  `autoplay` (muted, video, or after a user gesture). Each element gets a
  player whose backend is the audio mixer for `<audio>` and the decoded
  frame timeline for `<video>` (plus the mixer for a video's sound
  track). Commands go straight to the backend; the
  controller polls it every 50 ms while something loads or plays and
  turns what it reports into events, `timeupdate` every 250 ms. An
  element removed from the document pauses, a new `src` starts over,
  and a page parked in the back/forward cache pauses everything. The
  `controls` bar is laid out once by `ns_media_controls_layout`, which
  both `paint_media_controls` (`paint.c`) and the hit test in
  `libnorthstar.c` call, so the two cannot disagree.
- **Audio mixer** (`src/audio/audio.c`) — one worker thread per process
  fetches `<audio>` through `net.c` (or takes bytes already in hand: a
  `blob:` URL, a `<video>`'s MP2 track), decodes it into memory and mixes
  every player into the SDL2 device. Media Source Extensions players are
  streams instead: the media controller decodes AAC or Opus a couple of
  seconds ahead and pushes PCM into a persistent SDL audio stream, whose
  playback cursor sets the stream's clock (a straight line in wall time
  anchored where the device asks for more data, so it advances evenly
  however the device schedules its callbacks, and never steps back
  between seeks); headless runs use a silent
  clock-driven output instead. Each page owns an `NsAudioContext`, and
  `ns_audio_context_status` reports a player's load state, error,
  duration, position and end synchronously. Without SDL2 the build uses
  `src/audio/stub.c`, and every player fails as having no device.

### Headless drivers

`headless.c` has two paths. Text, DOM and layout dumps, `--dump=none`,
`--eval` and `--act` go through `page_session.c` and `libnorthstar.c`, the
same host the GUI uses. PNG, PDF and print dumps, `--wpt` and
`--inspect`/`--inspect-at` drive `engine.c` directly: they parse, lay
out, run the page's scripts and relayout without a page session.
`NS_HEADLESS_LEGACY` sends every run down the second path.

## Page-load pipeline

Each navigation flows through these stages. `engine.c` holds the
synchronous fetch → cascade → layout → capture steps and the speculative
preload scan; `render.c` is the shared style-and-layout pass (cascade,
layout, and the container-query second pass) that the GUI and headless
drivers both call.

| Stage | File(s) | Job |
|-------|---------|-----|
| 1. Fetch | `net.c`, `cache.c`, `engine.c` | libcurl on a shared multi handle (HTTP/1.1, HTTP/2; HTTP/3 through Alt-Svc when libcurl supports it), TLS verification, redirect clamp, response-size cap, HSTS, Alt-Svc, per-site cookie jars and HTTP cache. `engine.c` scans a parsed document for its scripts and stylesheets and preloads them; a preload map, an in-flight coalescer and the HTTP cache then answer in that order, keyed on request identity rather than bare URL, so a subresource is fetched once. A preloaded script of 64 KB or more that is not in the bytecode cache is compiled on a background thread while the page parses (`bytecode_cache.c`, its own QuickJS runtime, at most two at a time); the cache keeps entries up to 48 MB within a 256 MB disk budget. `netutil.c` holds Accept-Language, search-URL and proxy helpers. |
| 2. Safety gate | `safebrowsing.c`, `fetch_policy.c`, `csp.c`, `security.c` | Top-level host checked against the local SHA-256 blocklist. Every subresource request carries its destination and its document's policy, and `ns_fetch_policy_check` applies the `file:` rule, mixed-content blocking or upgrade, and Content-Security-Policy by destination before the request and at each redirect hop. Subresource Integrity (`ns_security_sri_check`) verified for scripts. |
| 3. Parse | `html.c`, `encoding.c`, `html_lexbor.c`, `xml.c` | Charset detection (BOM, header, `<meta>` prescan, then uchardet), decoding through the WHATWG Encoding Standard decoders in `encoding.c`, and bytes → DOM via lexbor (WHATWG HTML). `xml.c` parses XHTML and other namespaced XML documents. |
| 4. DOM | `dom.c` | The document tree and its mutation API, shared by layout and the JS bridge. |
| 5. Style | `css.c`, `css_syntax.c`, `css_calc.c`, `css_media.c`, `css_prop_syntax.c`, `anim.c`, `font.c` | Stylesheet parse, selector matching, the cascade, computed values. `css_syntax.c` is the CSS Syntax tokenizer, `css_calc.c` simplifies `calc()` trees and serializes specified values, `css_media.c` the Media Queries Level 4 parser and evaluator, and `css_prop_syntax.c` the `<syntax>` grammar behind `@property` and `CSS.registerProperty`. `anim.c` runs transitions and `@keyframes` animations; `font.c` loads `@font-face` web fonts. |
| 6. Layout | `layout.c`, `mathml.c` | Box tree and fragmentation: block/inline, flex, grid, tables, multicol, positioned boxes. Text is itemized, shaped and broken into lines by web-text-lib. The box tree is rebuilt on every relayout, but a text run's measured height, max-content and min-content widths are kept across relayouts, keyed by its text, the font attributes of its inline runs, its style (whose mutation counter animations bump), the width and the viewport, so an unchanged paragraph is not broken into lines again. `mathml.c` lays out presentation MathML. |
| 7. Paint | `paint.c`, `svg.c`, `image.c`, `texture.c`, `selection.c`, `spellcheck.c` | `ns_paint` walks the box tree in stacking order and draws straight into a Cairo context; there is no intermediate display list, and the retained box tree plays that part when only part of the viewport is repainted. Blurred box shadows are cached by size, corner radii, blur and colour, so a page of identical cards blurs one shadow. `svg.c` renders inline and image SVG, `image.c` decodes images on demand into the `texture.c` pixel abstraction, and paint draws the text selection (`selection.c`) and misspelling marks (`spellcheck.c`, over Enchant) over the text. |
| 8. Present | `src/gtk/procview.c`, `headless.c`, `print.c` | The GUI draws the frame surface into the GTK widget; headless writes it to PNG or PDF or dumps a text/DOM/layout tree; printing paginates the same box tree onto sheets. |

Most computed values stay as parsed `ns_css_value`s, but `display` is
resolved once per element into an `ns_display` — the CSS Display Level 3
decomposition into an outer type, an inner type, a list-item flag, a
layout-internal kind and the box kind (`normal`, `none`, `contents`).
Layout, paint and the CSSOM read that value through predicates in `css.h`
rather than comparing keyword strings, and blockification of floated and
absolutely positioned boxes has a single implementation.

Floats are tracked per block formatting context. A block that does not
establish its own context inherits the enclosing context's floats, so a
float placed in an ancestor still shortens the line boxes of nested
content. Only tables, block-level replaced elements and boxes that
establish a new formatting context are pushed aside by a float; every
other in-flow block keeps the full containing-block width and overlaps
the float, as CSS 2.1 §9.5 requires. An inline run whose lines cross the
bottom of a float is split into fragments, so the text below the float
reclaims the full width. Anonymous table boxes are generated around any
run of table-internal siblings.

## JavaScript and web APIs

| Area | File(s) |
|------|---------|
| QuickJS binding, DOM/JS bridge, events, CSSOM, fetch/XHR, storage, most Web APIs | `js.c`, `js_internal.h` |
| Compatibility shims over the public QuickJS API, which also map the quickjs-ng API onto the original QuickJS | `quickjs_compat.c` |
| Canvas 2D, `Path2D`, `ImageBitmap`, `DOMMatrix` | `js_canvas.c` |
| `Temporal` | `js_date.c`, `datetime.c` |
| `Intl` (ECMA-402, without ICU) | `js_intl.c` |
| `performance` and `PerformanceObserver` | `js_perf.c` |
| `ShadowRealm` | `js_realm.c` |
| `crypto.subtle` (WebCrypto over OpenSSL) | `js_webcrypto.c`, `webcrypto.c` |
| Offline Web Audio graph rendering | `webaudio.c` |
| WebIDL receiver checks: node-interface members called on a non-node throw "Illegal invocation" | `js_receiver.c` |
| `TextDecoder`, `TextEncoder` (over the `encoding.c` decoders) | `js_encoding.c` |
| WebAssembly JS API (over vendored WAMR's fast interpreter; `-Dwasm`) | `wasm.c`, `src/wamr/` |
| `WebSocket`, `EventSource` | `ws.c`, `eventsource.c` |
| IndexedDB | `idb.c` |
| `getUserMedia` video: V4L2 capture (Linux) and per-site permission | `camera.c` |
| Dedicated workers and service workers, each with its own runtime on its own thread | `js.c` |
| WebExtension manifests, content scripts, resources, storage and messaging | `ext.c`, `js.c` |
| Forms: validation, serialization, submission | `forms.c` |
| Pure-JS polyfills, embedded at build time | `data/js/polyfills.js` |

Each top-level document gets its own QuickJS runtime, created when the
document is loaded and freed when the page is left — or kept suspended in
the back/forward cache. Frames get a realm inside their parent page's
runtime, not a runtime of their own (see [`../SECURITY.md`](../SECURITY.md)). Dedicated workers, service workers and the
WebExtension host each run in a separate runtime.

The DOM/JS bridge invalidates opaque node pointers on free and
re-validates them on every call, so DOM mutation cannot dangle a
JS-held handle. `data/js/polyfills.js` is embedded by `src/meson.build`
(and, when `qjs` and `qjsc` are installed, syntax-checked at build time);
some page-facing surfaces are assembled there over native hooks rather
than bound in C, which is why `navigator.mediaDevices.getUserMedia` is
the polyfill's, not the native stub's. Video capture works on Linux only.
Audio capture does not exist: `mic.c` is a stub, and a granted
microphone request resolves with a silent track.

## Storage and state

| Concern | Where |
|---------|-------|
| Configuration (`northstar.conf`, flat key/value; `--print-config` prints it) | `config.c` |
| HTTP cache (SQLite index + on-disk bodies) | `cache.c` |
| Cookie jars, HSTS and Alt-Svc files, one set per site | `net.c` |
| IndexedDB (SQLite) | `idb.c` |
| `localStorage` (one key file per origin) and service-worker registrations | `js.c` |
| Browsing history (SQLite) | `history.c` |
| Bookmarks (`bookmarks.txt`) | `bookmarks.c` |
| WebExtension local storage | `ext.c` |
| Compiled-script bytecode | `bytecode_cache.c` |

On-disk state lives under the XDG config/data/cache directories with
owner-only permissions. See [`../SECURITY.md`](../SECURITY.md) for the
partitioning and permission model.

## Images

`image.c` decodes on a worker task unless `async_image_decode` is off,
both for images the page fetches (`engine.c` hands the bytes to
`ns_image_decode_encoded` and inserts the result on the engine thread) and
for images looked up on demand; `data:` images layout needs at once, and
headless runs, decode in place. Moving sources come first: an MPEG-1
stream becomes a streaming decoder (`video.c`, see [Video](#video)), and
an animated GIF or an APNG becomes a frame list. Everything else goes
down a fixed chain:

1. **`image_ico.c`** — ICO and CUR, unwrapped and handed to Wuffs.
2. **Wuffs** (`image_wuffs.c`) — PNG, GIF, BMP, JPEG and still WebP
   (memory-safe, transpiled-to-C). An animated WebP is reduced to its
   first frame by walking the RIFF container, since Wuffs decodes only
   still WebP.
3. **libavif** (`image_avif.c`) — AVIF, when built with it.
4. **`svg.c`** — SVG, rendered in-engine onto Cairo.

Nothing follows. A format none of these cover fails to decode rather
than falling through to a plugin-loaded decoder.

## Video

`<video>` plays two kinds of source.

**MPEG-1 files.** `video.c` recognises an MPEG-1 Program Stream or
elementary video stream by its start code and opens one pl_mpeg decoder
(`ns_video_stream`) over the downloaded bytes. pl_mpeg is vendored and
already supplies the MP2 audio decoder, and MPEG-1's patents have expired.

The decoder is held by an image-cache entry like an animated GIF's frame
list, so the image cache's fetch, frame timing, repaint scheduling and
eviction serve video unchanged. Frames are decoded as the clip plays: on
each tick `ns_video_stream_show` decodes forward to the frame for the
current time, or seeks when time jumps back or more than two seconds
ahead (`plm_seek_frame` for a program stream, rewind and decode for an
elementary one), and keeps a single texture. Memory is one frame plus the
clip's bytes whatever its length; a clip larger than
`NS_VIDEO_MAX_DIMENSION` (4096) on either side is rejected. `paint_video`
draws the current frame (or a dark placeholder for a source it cannot
decode) and, for `controls`, the bar on top.

**Media Source Extensions streams**, as streaming sites send them:

| Layer | File(s) | Job |
|-------|---------|-----|
| API | `data/js/polyfills.js`, `js_mse.c` | `MediaSource`, `SourceBuffer` (appendBuffer, remove, abort, timestampOffset, sequence mode, append windows, live seekable ranges) and `isTypeSupported`, forwarded to native buffers; `blob:` URLs from `createObjectURL` bind a media element to its source. |
| Buffers | `mse.c` | One buffer per track: appended bytes are demuxed, frames kept in decode order with presentation times, buffered ranges with small gaps merged, `remove()` dropping dependent frames up to the next keyframe, quotas (300 MB video, 48 MB audio) and muxed audio+video buffers. |
| Demux | `mp4.c`, `webm.c` | Incremental fragmented MP4 (moov/moof/mdat; av1C, vpcC, avcC, esds, dOps) and WebM/Matroska (SimpleBlock, BlockGroup, unknown sizes). |
| Video decode | `videodec.c`, `av1.c`, `vp9.c`, `h264.c`, `yuv.c` | AV1 through libdav1d, VP9 through libvpx, H.264 through OpenH264 (each optional); `yuv.c` converts 8–16 bit 4:2:0/4:2:2/4:4:4 BT.601/709 pictures to BGRA. |
| Decode thread | `videoworker.c`, `video.c` | One worker thread per stream decodes from the nearest keyframe. The engine thread queues about three seconds of coded frames, and the worker converts pictures ahead of the clock while fewer than 48 MB of them (4 to 12) wait to be shown; seeks restart it from a keyframe. dav1d and libvpx use up to four threads of their own. |
| Presentation | `videolayer.c`, `paint.c`, `page_session.c`, `gtk/procview.c` | A stream's layer holds its current picture and a copy of the frame timeline. Where the video lands on the page unchanged (no transform, opacity group or filter), `paint_video` clears a transparent hole for it and records its rectangle; the view takes the picture for the timeline at each tick of its own frame clock and draws it under the page frame, so overlays such as player controls stay on top. Script, relayouts and paints on the engine thread then hold up neither the picture nor its pace, and a new picture repaints nothing on the engine thread. Elsewhere (in a group, headless, printing) the engine takes the picture on its animation tick and paints it. The picture never moves back except after a seek. |
| Audio decode | `audiodec.c`, `src/audio/aac.c` | AAC-LC (in-tree, written from ISO/IEC 14496-3) and Opus (libopus, OpusHead pre-skip) to float PCM for the mixer's streaming player. |
| Controller | `js_media.c` | `readyState` follows the data buffered ahead of the playhead (`waiting`, `canplay`, `playing`), playback stalls and resumes with the buffer, the audio track is decoded about two seconds ahead, and the picture follows the audio clock: a drift is slewed out by moving the frame timeline a quarter of it per poll, at most 2.5 ms (a 5% rate change); a picture more than 0.1 s ahead holds until the sound catches up, and only one more than half a second behind jumps forward. Playback holds until the document has loaded. |

Both kinds become an image whose frame timeline the media controller
drives: `ns_image_anim_duration`, `ns_image_anim_position`,
`ns_image_anim_set_paused`, `ns_image_anim_seek`,
`ns_image_anim_set_loop` and `ns_image_anim_ended` are the whole of the
picture's playback surface, found by looking the element's source up in
the image cache. Because that cache is keyed by URL, two `<video>`
elements with the same source share one timeline. A decoded clip waits
paused on its first frame; the controller starts it when the page calls
`play()` or `autoplay` is set and allowed, and it plays once unless
`loop` is set, then fires `ended`.

A program stream with an MP2 track also opens that track in the page's
audio context from the same bytes, so nothing downloads twice. `play()`,
`pause()`, seeking, `volume`, `muted` and `loop` drive both; the picture
waits until the sound is decoded, then is re-seeked to the mixer's
position whenever the two drift more than 0.1 s apart.

Unless the `autoplay_enabled` setting is on, media starts only within a
few seconds of a user gesture: `autoplay` is ignored and `play()` is
rejected with `NotAllowedError` otherwise, muted or not. A `<source>`
whose `type` the build cannot play is skipped, in layout and in the
controller alike, and `canPlayType` (`media_types.c`) and
`MediaSource.isTypeSupported` (`ns_mse_type_supported` in `mse.c`, then
`media_types.c`) answer from the decoders actually built in. All decoding
is software decoding on the CPU; there is no hardware video decoding.

## Printing

`print.c` paginates a laid-out page onto sheets. No printing code is
written per platform and no dependency is added: the sheets go to
`GtkPrintOperation`, which is CUPS on Linux, the Win32 printer dialog on
Windows and the Cocoa panel on macOS.

Paper is not the viewport, so the page is laid out again for it.
`ns_browser_print_pages` (`libnorthstar.c`) turns `@media print` on, sets
the viewport to a sheet's content box — A4 at 96 dpi with half-inch
margins to begin with — and relayouts. Only then can `@page` be read,
because the rule arrives through the cascade that relayout just ran; if
it changes the content width the page is laid out a second time.
Afterwards the media type, the viewport and the layout are all restored,
so the page a reader is looking at does not reflow under them.

Where the sheets are cut is decided by one walk of the box tree
(`collect_breaks`). It gathers two things: the offsets where
`break-before` or `break-after` force a cut — with the legacy
`page-break-*` spellings mapping onto them and `always` becoming `page` —
and the spans that must not be split, which are every leaf box, every
line of a paragraph (the extent divided by the computed `line-height`),
and anything asking for `break-inside: avoid`. A sheet then ends at the
first forced break inside it if there is one; otherwise the cut starts at
the bottom of the sheet and `pull_above_spans` walks it up above any span
it would have straddled, for at most eight passes. A span taller than a
sheet is dropped rather than being allowed to push the cut past the top,
which is what keeps an oversized box from stalling pagination.

`ns_print_draw_page` clips to the sheet's content box, translates by the
sheet's top offset and replays the same `ns_paint` the screen uses, so
paper and screen cannot drift apart. `--dump=print:FILE` renders the
same pagination headless — through `engine.c`, which repeats the print
relayout itself — to a multi-page PDF with no printer attached, which is
how `data/render-tests/print-pagination.html` is checked. It is distinct
from `--dump=pdf:FILE` and the *Save Page as PDF…* menu item, which write
the page as one long unpaginated sheet.

## Security-relevant modules

| File | Role |
|------|------|
| `security.c` | Refuse privileged startup, Linux Landlock + seccomp sandbox, macOS Seatbelt profile, Windows process mitigations, Subresource Integrity, the CSPRNG, allocator hardening and download-origin marking. |
| `csp.c` | Content-Security-Policy parse and enforcement, per document. |
| `fetch_policy.c` | The request policy every subresource passes: `file:` access, mixed content and CSP by destination. |
| `safebrowsing.c` | Local phishing/malware blocklist + interstitial. |
| `watchdog.c` | Supervisor that restarts the browser on crash or hang. |

## Third-party components

Fetched by `meson setup` as pinned subprojects: **lexbor** (HTML parsing
and the WHATWG URL parser — CSS is the engine's own), **quickjs-ng** (JS;
`-Djs_engine=quickjs` takes the original **QuickJS** instead). Vendored
in-tree: **web-text-lib** (text itemization, shaping and line breaking,
`src/web-text-lib/`), **Wuffs** (images), **pl_mpeg** (MPEG-1 video and
MP2 audio), **WAMR** (WebAssembly, `src/wamr/`) and **minimp3** (MP3,
`src/audio/minimp3.h`). See
[`../THIRD-PARTY-LICENSES.md`](../THIRD-PARTY-LICENSES.md).

web-text-lib is a Pango fork, shared with Nordstjernen Sol, that exists for one reason: stock Pango keeps no
cache outliving a `PangoLayout`, so the same bytes were shaped once to
measure a run and again to paint it. The fork adds a process-wide cache of
finished glyph strings and a per-context metrics cache. Every symbol in it
is renamed (`ns_pango_*`, `NsPango*`), because GTK loads the system Pango
into the same process and GObject aborts if two libraries register the same
type name — so the engine reaches it through `src/ns_pango.h` (`<web-text-lib/…>`) and the GTK shell keeps
using the system Pango for its own widgets. A run is cached only when its
shaping cannot depend on the text around it, and HarfBuzz is what decides
that: asked for unsafe-to-concat flags, it marks every cluster whose glyphs
would move if the neighbouring text changed, and a piece is stored only
when both of its cuts came back clear — a font that kerns a space against
the letter after it, as Liberation Sans and Liberation Serif do, therefore
refuses the cut that a whitespace rule would have allowed. Whitespace or a
paragraph edge still governs the item's own two ends.
`NS_PANGO_SHAPE_CACHE=verify` serves each item from the cache, shapes it
again and compares the two, warning on any difference;
`NS_PANGO_SHAPE_CACHE=0` turns the shape, break and item caches all off.

## Diagnostics

- **`--trace=FILE`** (GUI and headless, `trace.c`) writes a Chrome
  trace-event JSON array that Perfetto (ui.perfetto.dev) or
  `chrome://tracing` opens. Each frame on the engine thread shows as
  `frame` with its `tick`, `paint` and `copy frame` phases (`paint
  (damage)` and `copy damage` when only part of the viewport changed, or
  `frame (unchanged)` when nothing needed repainting), relayouts as
  `cascade`, `style pass` and `layout`, and scripts, fetches, image
  decodes and the GTK thread's `present` as their own spans. The file is
  flushed after every event and the closing bracket is optional in that
  format, so a trace survives a crash or a kill.
- **`NS_DAMAGE=verify`** repaints the whole viewport after every partial
  frame, compares the two and reports any pixel that differs, with the
  largest channel difference; `NS_DAMAGE=0` turns partial repaints and
  scroll shifting off. Scaled images can differ by one level after a
  scroll, because pixman's filter sampling depends on the absolute
  offset.
- **`NS_LAYOUT_CACHE=verify`** measures every text run the layout cache
  answers and reports any difference from the cached height, baseline or
  width; `NS_LAYOUT_CACHE=0` turns the cache off.
- **`--debug`** (headless only) streams engine events to stderr.
  `--debug=info,warn,error,render,net,js` selects levels and `--debug`
  alone selects all of them. With `net` selected, a headless run ends with
  a `[net perf]` line: fetch count and bytes, relayout time, and the
  web-text-lib shape, break and item cache hits, misses and skips.
- **`debuglog.c`** is the in-process event log. Engine code and the JS
  console emit into it and `--debug` subscribes to it. It is mirrored to
  a file when `NS_LOG_FILE` is set (to that path, or by default
  `northstar-debug.log` under the user data directory); on Windows the
  file is on unless `NS_NO_LOG_FILE` is set, and the watchdog points to
  it when the browser cannot start.
- **Developer tools** in the shell (console, network, performance, layout
  and elements panels) are served by the page view over `REQ_CONSOLE`,
  `REQ_EVAL` and `REQ_DUMP`.
- **`threaddump.c`** lists every thread's id, state, CPU time and name
  from `/proc` on `SIGQUIT`. It is Linux-only.

## Other source files

| File | Role |
|------|------|
| `i18n.c` | UI translation: English-source strings looked up in `data/i18n/*.lang` at startup, English as the fallback. No gettext. |
| `media_types.c` | The one table of media types the compiled decoders play, which `canPlayType`, `navigator.mediaCapabilities` and `MediaSource.isTypeSupported` all answer from. |
| `trace.c` | The `--trace=FILE` writer. |
| `proc_limits.h` | Shell and engine limits: maximum frame size, settle time, zoom range, script-driven redirect cap. |
| `mat4.h` | 4×4 matrices for CSS 3D transforms. |
| `version.h` | `NS_VERSION` and `NS_BUILD_DATE`. |
| `win_launcher.c` | Windows-only `northstar-launcher.exe`, which starts the browser from the bundle and reports missing runtime DLLs. |
