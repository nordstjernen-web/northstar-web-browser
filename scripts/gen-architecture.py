#!/usr/bin/env python3
# gen-architecture.py — regenerate docs/architecture.svg, the diagram in docs/architecture.md.
import html
import os

W = 1200
out = []


def esc(t):
    return html.escape(t, quote=True)


def rect(x, y, w, h, cls, rx=8):
    out.append(f'<rect class="{cls}" x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}"/>')


def text(x, y, t, cls, anchor="start"):
    out.append(f'<text class="{cls}" x="{x}" y="{y}" text-anchor="{anchor}">{esc(t)}</text>')


def module(x, y, w, h, name, desc, accent):
    rect(x, y, w, h, "mod")
    out.append(f'<rect class="bar {accent}" x="{x}" y="{y}" width="5" height="{h}" rx="2"/>')
    text(x + 14, y + 20, name, "file")
    lines = desc if isinstance(desc, list) else [desc]
    for i, line in enumerate(lines):
        text(x + 14, y + 37 + i * 15, line, "desc")


def group(x, y, w, h, title, cls, dashed=False):
    rect(x, y, w, h, cls + (" dash" if dashed else ""), 12)
    text(x + 16, y + 26, title, "gtitle")


def arrow(x1, y1, x2, y2, cls="arr"):
    out.append(f'<line class="{cls}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" marker-end="url(#ah)"/>')


def chip(x, y, w, name, cls="chip"):
    rect(x, y, w, 30, cls, 6)
    text(x + w / 2, y + 20, name, "chiptext", "middle")


H = 1336
out.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" font-family="DejaVu Sans, Verdana, sans-serif">')
out.append("""<title>Northstar Web Browser architecture</title>
<defs>
<marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#445"/></marker>
<style>
.bg{fill:#fbfaf7}
.proc{fill:none;stroke:#333;stroke-width:1.6}
.dash{stroke-dasharray:7 5}
.g-shell{fill:#eef4fb;stroke:#3a78c2;stroke-width:1.4}
.g-engine{fill:#eef8f2;stroke:#2e9a61;stroke-width:1.4}
.g-js{fill:#fdf7ea;stroke:#d39a1c;stroke-width:1.4}
.g-thr{fill:#f5f1fb;stroke:#7a5bb5;stroke-width:1.4}
.g-store{fill:#f1f7ee;stroke:#4f8a3a;stroke-width:1.4}
.g-sec{fill:#fcf0f0;stroke:#c24a4a;stroke-width:1.4}
.g-ext{fill:#f2f2f2;stroke:#777;stroke-width:1.4}
.mod{fill:#fff;stroke:#b9b9b9;stroke-width:1}
.bar{stroke:none}
.a-shell{fill:#3a78c2}.a-engine{fill:#2e9a61}.a-js{fill:#d39a1c}.a-thr{fill:#7a5bb5}.a-store{fill:#4f8a3a}.a-sec{fill:#c24a4a}.a-ext{fill:#777}
.chip{fill:#fff;stroke:#999;stroke-width:1}
.title{font-size:26px;font-weight:bold;fill:#222}
.sub{font-size:13px;fill:#555}
.gtitle{font-size:15px;font-weight:bold;fill:#222}
.ptitle{font-size:13px;font-weight:bold;fill:#222}
.file{font-family:"DejaVu Sans Mono",Menlo,Consolas,monospace;font-size:13px;font-weight:bold;fill:#222}
.desc{font-size:11px;fill:#555}
.chiptext{font-size:12px;fill:#333}
.lbl{font-size:11px;fill:#444}
.arr{stroke:#445;stroke-width:1.5;fill:none}
.arrd{stroke:#445;stroke-width:1.5;fill:none;stroke-dasharray:6 4}
</style>
</defs>""")
rect(0, 0, W, H, "bg", 0)
text(W / 2, 40, "Northstar Web Browser — software architecture", "title", "middle")
text(W / 2, 62, "Hand-written HTML/CSS/JS engine in C · GTK 4 shell · libcurl · one process · no Gecko, WebKit or Blink", "sub", "middle")

group(20, 84, 360, 92, "Watchdog supervisor — GUI launches only", "g-ext", dashed=True)
module(36, 118, 328, 48, "watchdog.c", "no network, sandbox or UI · restarts on crash or hang", "a-ext")
group(820, 84, 360, 92, "Headless / tooling — same binary", "g-ext", dashed=True)
module(836, 118, 328, 48, "headless.c", "--dump= · --eval= · --act= · --wpt · --inspect=", "a-ext")
text(600, 112, "one executable: northstar", "ptitle", "middle")
text(600, 132, "GUI, supervisor and headless modes", "lbl", "middle")
text(600, 148, "are chosen by command-line flags", "lbl", "middle")
arrow(200, 176, 200, 200)
text(210, 194, "runs the browser as --watchdog-child", "lbl")

rect(14, 204, 1172, 908, "proc dash", 14)
text(30, 226, "Browser process — GTK shell + page engine in one address space", "ptitle")

group(30, 236, 1140, 100, "GTK main thread  (src/gtk/)", "g-shell")
module(46, 270, 330, 56, "appmain.c", ["flags · refuse root · i18n · config", "watchdog decision · sandbox → shell or headless"], "a-shell")
module(392, 270, 380, 56, "procwindow.c", ["window chrome: omnibox · menus · bookmarks", "history · downloads · print · settings · zoom"], "a-shell")
module(788, 270, 366, 56, "procview.c", ["page-view widget: input → typed requests,", "frame surface → screen · find · devtools"], "a-shell")

arrow(700, 336, 700, 382)
arrow(760, 382, 760, 336)
text(690, 356, "requests (REQ_LOAD, REQ_CLICK, REQ_KEY, …)", "lbl", "end")
text(690, 372, "posted as jobs to the engine thread", "lbl", "end")
text(770, 356, "responses back on the GTK main loop:", "lbl")
text(770, 372, "frame + title, URL, cursor, audio commands", "lbl")

EX, EY, EW, EH = 30, 382, 790, 460
group(EX, EY, EW, EH, "Engine thread “ns-engine” — its own GMainContext  (enginethread.c, mainctx.c)", "g-engine")
module(46, 418, 240, 50, "page_session.c", "open page · bfcache (4) · last frame", "a-engine")
module(318, 418, 240, 50, "libnorthstar.c", "ns_browser: document, runtime, input", "a-engine")
module(590, 418, 214, 50, "engine.c · render.c", "fetch · cascade · layout pass", "a-engine")
arrow(286, 443, 316, 443)
arrow(558, 443, 588, 443)

text(46, 494, "Rendering pipeline", "ptitle")
stages = [
    ("net.c · cache.c", "libcurl fetch · HSTS · cookies · HTTP cache"),
    ("html.c · html_lexbor.c · xml.c", "charset (uchardet) · lexbor parse"),
    ("dom.c", "node tree + mutation API"),
    ("css*.c · anim.c · font.c", "cascade · media/container queries · @font-face"),
    ("layout.c · mathml.c", "block · inline · flex · grid · table · multicol"),
    ("paint.c · svg.c · image.c", "box tree in stacking order → Cairo"),
]
sy = 504
for i, (n, d) in enumerate(stages):
    module(46, sy, 380, 42, n, d, "a-engine")
    if i < len(stages) - 1:
        arrow(400, sy + 42, 400, sy + 52)
    sy += 52
text(46, sy + 12, "print.c paginates the same box tree onto sheets", "lbl")

JX = 442
group(JX, 488, 362, 340, "JavaScript runtime & Web APIs", "g-js")
module(JX + 14, 520, 334, 54, "js.c", ["QuickJS bindings · DOM ↔ JS bridge · events", "fetch/XHR · CSSOM · storage · workers"], "a-js")
small = [
    ("js_canvas.c", "Canvas 2D"), ("webcrypto.c", "crypto.subtle"),
    ("js_intl.c", "Intl, ICU-free"), ("js_date.c", "Temporal"),
    ("js_perf.c", "performance.*"), ("js_realm.c", "ShadowRealm"),
    ("wasm.c", "WebAssembly (WAMR)"), ("idb.c", "IndexedDB"),
    ("webaudio.c", "Web Audio (offline)"), ("ext.c", "WebExtensions"),
]
for i, (n, d) in enumerate(small):
    cx = JX + 14 + (i % 2) * 171
    cy = 580 + (i // 2) * 46
    module(cx, cy, 163, 40, n, "", "a-js")
    text(cx + 14, cy + 34, d, "desc")
text(JX + 14, 818, "data/js/polyfills.js — embedded at build time", "lbl")

TX = 836
group(TX, EY, 334, EH, "Other threads", "g-thr")
threads = [
    ("net.c", "curl multi thread + fetch tasks"),
    ("image.c · video.c", "image decode tasks, MPEG-1 opened"),
    ("js.c", "Web Workers · service workers, own runtimes"),
    ("ws.c · eventsource.c", "WebSocket · EventSource"),
    ("audio/audio.c", "mixer: minimp3 · pl_mpeg · Ogg → SDL2"),
    ("procwindow.c", "downloads"),
    ("camera.c", "V4L2 webcam capture (Linux)"),
]
ty = 418
for n, d in threads:
    module(TX + 14, ty, 306, 44, n, d, "a-thr")
    ty += 52

group(30, 856, 1140, 118, "Storage & state — XDG config/data/cache directories, owner-only permissions", "g-store")
stores = [
    ("config.c", "northstar.conf"), ("cache.c", "HTTP cache: SQLite + files"),
    ("net.c", "cookies · HSTS · Alt-Svc per site"), ("idb.c", "IndexedDB (SQLite)"),
    ("js.c", "localStorage · SW registrations"), ("history.c", "history (SQLite)"),
    ("bookmarks.c", "bookmarks.txt"), ("bytecode_cache.c", "compiled scripts"),
]
for i, (n, d) in enumerate(stores):
    cx = 46 + (i % 4) * 280
    cy = 890 + (i // 4) * 40
    module(cx, cy, 268, 34, n, "", "a-store")
    text(cx + 14 + len(n) * 8 + 10, cy + 21, d, "desc")

group(30, 990, 1140, 110, "Security & platform", "g-sec")
secs = [
    ("security.c", ["refuse root · Landlock + seccomp", "Seatbelt · Win32 mitigations · SRI"]),
    ("csp.c", ["Content-Security-Policy", "per document"]),
    ("safebrowsing.c", ["local SHA-256 host blocklist", "+ interstitial"]),
    ("i18n.c", ["UI strings from", "data/i18n/*.lang"]),
    ("debuglog.c · threaddump.c", ["event log · --debug", "SIGQUIT thread list"]),
]
widths = [262, 190, 220, 170, 226]
cx = 46
for (n, d), w in zip(secs, widths):
    module(cx, 1024, w, 64, n, d, "a-sec")
    cx += w + 12

group(20, 1130, 1160, 96, "Pinned subprojects (fetched by meson) and vendored code — no browser-engine fork", "g-ext")
tp = [("lexbor — HTML + URL", 200), ("quickjs-ng — JS, no JIT", 200), ("web-text-lib — shaping cache", 206),
      ("Wuffs — images", 130), ("pl_mpeg — MPEG-1, MP2", 170), ("minimp3", 90), ("WAMR", 80)]
cx = 36
for name, w in tp:
    chip(cx, 1168, w, name)
    cx += w + 8
text(36, 1218, "lexbor, quickjs-ng: subprojects/*.wrap · Wuffs, pl_mpeg: subprojects/ · minimp3: src/audio/ · WAMR: src/wamr/ · web-text-lib: src/web-text-lib/", "lbl")

group(20, 1238, 1160, 76, "System libraries", "g-ext")
libs = ["GTK 4", "GLib", "Cairo", "HarfBuzz", "FriBidi", "fontconfig", "FreeType", "libcurl", "OpenSSL",
        "SQLite", "uchardet", "libpsl", "zlib", "SDL2", "libseccomp", "libavif*", "Enchant*"]
ws = [14 + len(n) * 7 for n in libs]
gap = (1128 - sum(ws)) / (len(libs) - 1)
cx = 36
for name, w in zip(libs, ws):
    chip(cx, 1272, w, name)
    cx += w + gap
text(1164, 1262, "* optional", "lbl", "end")

out.append("</svg>")
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
with open(os.path.join(root, "docs", "architecture.svg"), "w") as f:
    f.write("\n".join(out) + "\n")
