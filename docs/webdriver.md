# WebDriver

Northstar Web Browser has a built-in [W3C WebDriver](https://www.w3.org/TR/webdriver2/)
server, so the browser can be driven by Selenium, WebdriverIO, Nightwatch
or any other WebDriver client for automated testing. It is the classic
HTTP protocol; WebDriver BiDi is not implemented.

There is no separate driver binary: the browser itself is the driver.

```sh
./builddir/src/gtk/northstar --webdriver          # port 4444
./builddir/src/gtk/northstar --webdriver=9515     # another port
./builddir/src/gtk/northstar --webdriver=0        # a free port, printed at startup
```

The server prints one line when it is ready —
`Northstar WebDriver 1.0.23 listening on http://127.0.0.1:4444` — and runs
until it gets `SIGINT` or `SIGTERM` (or the process is killed). Pages run
headless: the same engine and page session as `--headless`, with no
window on screen. `--viewport=WxH` sets the initial window size (default
1280×800), `--settle-ms=N` how long each load settles (default 200) and
`--debug` logs every request and reply to stderr.

## Clients

Ask for `browserName: "northstar"`. With Selenium for Python:

```python
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.common.options import ArgOptions

options = ArgOptions()
options.set_capability("browserName", "northstar")
driver = webdriver.Remote(command_executor="http://127.0.0.1:4444",
                          options=options)
driver.get("https://example.com/")
print(driver.find_element(By.TAG_NAME, "h1").text)
driver.quit()
```

With WebdriverIO, set `hostname: "127.0.0.1"`, `port: 4444` and
`capabilities: { browserName: "northstar" }`. A `/wd/hub` prefix on every
path is accepted, for clients configured for a Selenium Grid URL.

## Capabilities

The new-session reply lists what the session got:

| Capability | Value |
|------------|-------|
| `browserName` | `northstar` — a request for another browser is refused |
| `browserVersion` | the release, e.g. `1.0.23`; a requested version must be a prefix of it |
| `platformName` | `linux`, `mac`, `windows`, `freebsd`, `openbsd` or `netbsd` |
| `acceptInsecureCerts` | as requested; `true` loads pages with untrusted certificates for the session, as `--ignore-certificate-errors` does |
| `pageLoadStrategy` | `normal` (wait for `readyState` `complete`, the default), `eager` (`interactive`) or `none` |
| `proxy` | `direct`, `system` or `manual` (`httpProxy` / `sslProxy`); `pac` and `autodetect` do not match |
| `timeouts` | `script` 30000, `pageLoad` 300000, `implicit` 0 ms unless requested |
| `setWindowRect` | `true` |
| `strictFileInteractability` | as requested |
| `unhandledPromptBehavior` | `dismiss and notify` (no prompt ever blocks; see below) |
| `userAgent` | the browser's User-Agent string |
| `ns:headless` | `true` |

`webSocketUrl` is accepted and ignored, so a client that asks for BiDi
falls back to the classic protocol. Other extension capabilities (any
name with a `:`) are ignored. One session runs at a time; `GET /status`
reports `ready: false` while one is open.

## Commands

Every endpoint of WebDriver Classic is routed:

- **Sessions** — New Session, Delete Session, Status, Get/Set Timeouts
  (the Selenium 3 `{"type", "ms"}` form too).
- **Navigation** — Navigate To, Get Current URL, Back, Forward, Refresh,
  Get Title. Back and Forward step through `pushState` entries of the page
  first, then through the documents the window loaded.
- **Contexts** — Get/Close/Switch To Window, Get Window Handles, New
  Window, Switch To Frame (by index, element or `null`), Switch To Parent
  Frame, Get/Set Window Rect, Maximize, Minimize and Fullscreen Window.
  Each window is its own page with its own history. The window has no
  chrome, so its size is the viewport size; it is clamped to 2560×1600,
  and Maximize and Fullscreen make it 1920×1080.
- **Elements** — Find Element(s) from the document, an element or a
  shadow root, by `css selector`, `link text`, `partial link text`,
  `tag name` and `xpath`, honouring the implicit wait; Get Active
  Element, Get Element Shadow Root, Is Element Selected, Get Element
  Attribute, Property, CSS Value, Text, Tag Name and Rect, Is Element
  Enabled, Get Computed Role and Label; plus the non-standard
  `GET .../element/{id}/displayed` that several clients still call.
- **Interaction** — Element Click (scrolled into view, checked for being
  in view and not obscured, then a real pointer press and release through
  the engine's hit testing; an `<option>` is selected as the
  specification describes), Element Clear, Element Send Keys (keydown,
  keypress, input and keyup per character, with the WebDriver key
  codepoints `U+E000`–`U+E05D` for Enter, Backspace, arrows, modifiers
  and so on).
- **Document** — Get Page Source, Execute Script and Execute Async
  Script. Scripts may return promises, which are awaited up to the script
  timeout. Elements, shadow roots, windows and frames pass both ways as
  WebDriver references; returning a cyclic object is a `javascript error`.
- **Cookies** — Get All Cookies, Get Named Cookie, Add Cookie (including
  `httpOnly`, `secure`, `domain`, `path` and `expiry`), Delete Cookie and
  Delete All Cookies, against the page's own cookie partition.
- **Actions** — Perform Actions and Release Actions with `key`, `pointer`
  (`mouse`, `pen`, `touch`), `wheel` and `none` sources: pauses, key down
  and up, pointer move (with `viewport`, `pointer` or element origins,
  interpolated over its duration), down, up and cancel, and wheel scroll.
  A second click within 500 ms at the same point also fires `dblclick`.
- **Screen capture** — Take Screenshot and Take Element Screenshot (PNG of
  the viewport, or of the element's box after scrolling it into view).
- **Print** — Print Page to a PDF, with `orientation`, `page`, `margin`
  and `pageRanges`; it uses the same pagination as `Ctrl+P` and
  `--dump=print:`.

Errors carry the WebDriver error codes and HTTP statuses (`no such
element` 404, `stale element reference` 404, `element click
intercepted` 400 and so on). A reference to an element of a document
that has since been replaced is a `stale element reference`.

## Differences and limitations

- **User prompts never block.** `alert()` writes to the console,
  `confirm()` returns `true` and `prompt()` returns its default, in
  WebDriver sessions as in every headless run, so there is never a prompt
  to accept or dismiss; the alert commands answer `no such alert`.
- **Frames are not laid out.** The engine parses and scripts a frame's
  document but does not lay it out, so inside a frame finding elements,
  reading their text and attributes and running scripts work, while a
  click or a screenshot of an element in a frame has no box to aim at
  (`element not interactable`). Scripts in a frame run in the frame's own
  realm when it has one (it runs scripts), and otherwise with `window`
  and `document` bound to the frame's.
- **File inputs** cannot be given files (the engine has no file list
  behind `<input type=file>`): sending keys to one is
  `unsupported operation`.
- **`dblclick`** from the actions API is a synthesized event
  (`isTrusted` is `false`), since the engine's pointer path has no
  double click of its own.
- **Print** ignores `scale`, `background` and `shrinkToFit` beyond
  validating them.
- **No BiDi, no browser logs endpoint**, and only one session at a time.
- `sameSite` is reported as `None` for every cookie: the cookie store
  does not keep the attribute.

## Security

The server listens on the loopback interface only (127.0.0.1, and ::1
where the machine has IPv6), and refuses:

- requests whose `Host` header is not `localhost`, `127.0.0.1` or `[::1]`
  (DNS rebinding),
- requests carrying an `Origin` header, i.e. sent by a web page,
- `POST` bodies typed `application/x-www-form-urlencoded`,
  `multipart/form-data` or `text/plain`, the types a page can send
  cross-origin without a CORS preflight.

Anyone who can open a TCP connection to the port on this machine can
drive the browser, so do not expose it through port forwarding. The
sandbox stays as in any headless run (see [SECURITY.md](../SECURITY.md)):
Landlock, seccomp and the file-reading limits apply, so a `file:` page
must sit under the source tree or Downloads. The page-side half of every
command lives in a closure held by C, never on a global, so page script
cannot reach the element registry or the frame-access natives behind it.

## How it works

`src/webdriver.c` runs a small HTTP/1.1 server: one thread accepts
connections and one thread serves each connection, handing every request
to the main thread through a queue. The main thread owns the page engine;
between requests it runs the engine's main context and renders each window
about sixty times a second, as the GUI's engine thread and frame clock
would, so timers, fetches and animations keep running.

Each window is an `ns_page_session` (`src/page_session.c`). Commands that
need the DOM call the WebDriver atoms, `data/js/webdriver.js`, which is
embedded at build time and installed into each page on first use through
`ns_js_webdriver_call()` (`src/js.c`). The atoms find elements, keep the
registry of element references, serialize values in both directions and
compute where to click; the C side turns that into real pointer and key
input through `ns_page_session_click`, `ns_page_session_key` and the rest,
so events are trusted and default actions (following links, submitting
forms, toggling checkboxes) are the engine's own. Request and reply bodies
are parsed and written by `src/json.c`.

`scripts/webdriver-test.py` is an end-to-end test with nothing but the
Python standard library: it serves fixture pages, starts
`northstar --webdriver=0` and checks every command group.

```sh
python3 scripts/webdriver-test.py builddir/src/gtk/northstar
```
