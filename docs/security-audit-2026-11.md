# Security audit, November 2026

A source review of Northstar Web Browser for security vulnerabilities, made
on 10 November 2026 against `main` at commit `c6aec52` (1.0.23-dev). It
follows the threat model in [`../SECURITY.md`](../SECURITY.md): the attacker
controls every fetched byte and any network server; the user, the kernel
and the local filesystem outside the sandbox allow-list are trusted.
Third-party library internals (libcurl, GTK, lexbor, QuickJS, Wuffs, WAMR,
pl_mpeg, minimp3) were not reviewed, only our code that calls them.

**Result: no memory-safety vulnerability was found. Twenty-two findings
are listed: one high, six medium, the rest low or informational.** The
serious ones are web-security policy gaps (document MIME handling, cookie
scoping for cross-site frames) and sandbox rules that do not hold as
written. None of the findings has been fixed by this document.

## Method

- **Code reading** of the first-party C sources (about 220,000 lines in
  `src/`, excluding `src/wamr/` and `src/web-text-lib/`), the build files,
  CI workflows, installers and the npm package.
- **Dynamic testing** with an AddressSanitizer + UndefinedBehaviorSanitizer
  build (`-Db_sanitize=address,undefined`, quickjs-ng, Ubuntu 24.04,
  GTK 4.14, libcurl 8.5) driven in `--headless` mode against a local HTTP
  server and, through `--proxy=`, a local proxy that answers for arbitrary
  host names. Sandboxing was active (Landlock and seccomp).

Each finding carries a status:

| Status | Meaning |
|---|---|
| **Reproduced** | Observed in the sanitizer build with a test page. |
| **Confirmed** | Established by reading the code path end to end; not run. |
| **Likely** | The code supports it, but one link depends on behaviour that was not tested (a library, the kernel, or an unread caller). |
| **Unverified** | A plausible hazard; reachability was not established. |

## Summary

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | Top-level documents of any MIME type are parsed as HTML; `nosniff` and `Content-Disposition` ignored | High | Reproduced |
| 2 | A cross-site frame's requests to its embedder's site carry the embedder's cookies | Medium | Reproduced |
| 3 | seccomp `socket(AF_UNIX)` denial compares 64 bits of a 32-bit argument | Medium | Confirmed (rule); kernel truncation not tested |
| 4 | "Signals only to this process" is bypassable through other allowed syscalls | Medium | Confirmed |
| 5 | Windows URL handler: option injection through `"%1"` | Medium | Likely |
| 6 | Policy-blocked synchronous XHR runs other page tasks inside `send()` | Medium | Reproduced |
| 7 | Certificate-error override retries without a prompt and sends cookies | Medium (opt-in only) | Confirmed |
| 8 | EventSource: protocol limits are dead code, no HSTS, body parsed before open | Low | Confirmed (`#ifdef`); script impact not tested |
| 9 | `doh_url` / `NS_DOH_URL` is never applied | Low | Confirmed |
| 10 | Cross-origin script errors are not muted in `window.onerror` | Low | Reproduced |
| 11 | `document.cookie` accepts a `Domain` attribute on IP-literal hosts | Low | Reproduced |
| 12 | Page-set `Authorization` survives cross-origin redirects; preflights may carry cookies | Low | Confirmed |
| 13 | Landlock leaves socket/FIFO creation unhandled; config file writable in the sandbox | Low | Confirmed |
| 14 | `document.cookie` on an `http:` page can replace a `Secure` cookie | Low | Likely |
| 15 | HTTP cache ignores credentials mode; `Cache-Control` matched case-sensitively | Low | Confirmed |
| 16 | Hardening flags do not reach the fallback subprojects | Low | Confirmed |
| 17 | Bytecode cache key does not separate classic scripts from modules | Low | Confirmed (key); effect not tested |
| 18 | Wrapper finalizer disarms the node-invalidate hook | Low | Unverified |
| 19 | Frame realms are never released before page teardown | Info | Confirmed |
| 20 | Per-response memory budget has no global cap | Info | Confirmed |
| 21 | Short cache key from a corrupted database is indexed unchecked | Info | Confirmed |
| 22 | Smaller items (address bar, downloads, `about:start`, npm checksums, logging) | Info | See text |

## Findings

### 1. Top-level documents of any MIME type are parsed as HTML

**High. Reproduced.**
`src/libnorthstar.c:1487-1520` (`browser_prepare_document_response`),
`:1809-1825`; `src/html.c:93-97`.

A top-level navigation rewrites only `image/*`, JSON and non-HTML
`text/*` responses. Every other type falls through to the HTML parser.
`Content-Disposition: attachment` is captured (`src/net.c:2967`) but only
exposed to script, and `X-Content-Type-Options` is consulted only for
scripts and styles (`src/engine.c:268`, `src/js.c:58597`).

Observed in the test build — a response whose body is
`<p id=x>inert</p><script>…</script>`:

| Response headers | Result |
|---|---|
| `application/octet-stream`, `nosniff`, `Content-Disposition: attachment` | script ran in the serving origin |
| `application/pdf`, `nosniff` | script ran |
| `application/zip` | script ran |
| no `Content-Type`, `nosniff` | script ran |
| `text/plain` | shown as text |
| `application/json` | shown as text |
| `image/png`, `nosniff` | treated as an image |

Any site that lets users upload files and serves them back as an
attachment or as `application/octet-stream` — the standard defence — is
exposed to stored cross-site scripting in this browser. Only the
top-level path was tested; frame loads go through `src/js.c` and should be
checked for the same behaviour.

**Fix.** Allow-list document types (`text/html`, XHTML/XML, `text/*`,
images, JSON). Send everything else, and anything with
`Content-Disposition: attachment`, to the download path. Never treat a
response as HTML when `nosniff` is present and the type is not an HTML
type.

### 2. A cross-site frame's requests to the embedder's site carry cookies

**Medium. Reproduced.** `src/net.c:5642-5698`.

The first-party check compares the request target with the top-level
page, not with the document making the request, and the unsafe-method
check runs only when `initiator` is set, which is only for top-level
navigations:

```c
if (cookies_allowed && cookie_policy == NS_COOKIE_FIRST_PARTY &&
    top_url && !ns_url_is_same_site(url, site_top_url))
    cookies_allowed = FALSE;
```

Observed: `http://shop.example.com/` sets `session=…` and embeds
`<iframe src="http://ads.evil.net/…">`. Inside the frame,
`fetch('http://shop.example.com/echo', {credentials:'include'})` and a
credentialed `XMLHttpRequest` POST both arrived with the shop's `session`
cookie. SECURITY.md states that every cookie is treated as
`SameSite=Lax` and that third-party requests get none; a request from a
cross-site frame is third-party by initiator and should not carry them.
An embedded advertisement or widget can therefore make state-changing
requests to its host site as the logged-in user.

**Fix.** For non-navigation requests, also require the requesting
document to be same-site with the target, and apply the unsafe-method rule
using the requesting document.

### 3. seccomp `socket(AF_UNIX)` denial compares all 64 bits

**Medium (post-compromise). Rule confirmed; kernel behaviour not tested.**
`src/security.c:1152-1155`.

```c
(void)seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EACCES), socket_nr, 1,
                       SCMP_A0(SCMP_CMP_EQ, AF_UNIX));
```

The `family` argument is a 32-bit `int`, but seccomp compares the whole
64-bit register. A raw `socket` syscall with the upper half of the first
argument non-zero does not match the rule, and the kernel then truncates
the value to `AF_UNIX`. The same file handles this correctly for `ioctl`
at `:1012` with `SCMP_CMP_MASKED_EQ, 0xffffffffu`. The rule is the
documented mitigation that keeps a compromised content thread off D-Bus,
`systemd --user`, ssh-agent and gpg-agent.

**Fix.** `SCMP_A0(SCMP_CMP_MASKED_EQ, 0xffffffffu, AF_UNIX)`; better,
allow only `AF_INET` and `AF_INET6` on content threads, and on Landlock
ABI 6 add `LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET`.

### 4. Signal restriction is bypassable

**Medium (post-compromise). Confirmed.** `src/security.c:1045-1053`
(deny rules), `:852-853`, `:892`, `:897`, `:967` (allow-list).

`kill` and `tgkill` are restricted to the browser's own pid, but the
allow-list also contains `tkill`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo`,
`pidfd_open` and `pidfd_send_signal`, none of which is constrained. A
compromised browser can signal any process the user owns, including the
unsandboxed watchdog supervisor.

**Fix.** Remove `tkill`, `rt_sigqueueinfo` and `pidfd_send_signal` from
the allow-list, or constrain their pid argument to `getpid()`; on Landlock
ABI 6 add `LANDLOCK_SCOPE_SIGNAL`.

### 5. Windows URL handler: option injection

**Medium (Windows only). Likely — not tested on Windows.**
`data/installer/northstar.nsi:132`; `src/appmain.c:561-564`, `:601-701`;
`src/watchdog.c:119-123`.

The handler is registered as `"northstar.exe" "%1"`, there is no `--`
terminator, and options are accepted after the URL. A caller that passes a
URL containing an unescaped `"` to the shell can append options:
`--proxy=` (all traffic through an attacker's proxy),
`--ignore-certificate-errors`, `--trace=PATH` and
`--watchdog-session=PATH` (truncate or overwrite a user file before the
sandbox is applied), `--dump=`, `--eval=`. The Linux desktop entry
(`Exec=northstar %U`) is not affected.

**Fix.** Register `… -- "%1"` and stop option parsing at `--`; accept the
`--watchdog-*` options only together with the supervisor's child flag;
reject any option that follows the first positional argument.

### 6. Blocked synchronous XHR runs page tasks inside `send()`

**Medium. Reproduced.** `src/js.c:19276-19282`, `:19000-19019`.

The normal synchronous path pumps an isolated `GMainContext` and its
comment explains why. The branch taken when the fetch policy refuses the
URL pumps the shared engine context instead and does not set `in_pump`:

```c
if (blocked) {
    ns_js_attach_idle(st->js, ns_xhr_emit_blocked_idle, st);
    ...
    if (ns_xhr_is_sync(ctx, this_val))
        ns_xhr_pump_until_done(ctx, this_val);
    return JS_UNDEFINED;
}
```

Observed: on a page with `connect-src 'none'`, a `setTimeout(…, 0)`
callback ran before a synchronous `x.send()` returned. Timers, message
tasks, lifecycle ticks and image-ready callbacks of every page on the
engine thread can therefore run nested inside a script that the rest of
the engine assumes runs to completion. No memory-safety consequence was
demonstrated; host commands are gated on `g_main_depth() == 0`
(`src/gtk/enginethread.c:32-35`), so page teardown cannot happen there.

**Fix.** Complete a blocked synchronous request inline without pumping,
or give it the same isolated context. As hardening, make
`ns_js_process_pending_iframes` and `ns_js_lifecycle_tick` bail when
`dispatch_depth` or `callback_depth` is non-zero.

### 7. Certificate-error override retries silently and sends cookies

**Medium, but only with `tls_allow_insecure_override=true` or
`--ignore-certificate-errors`. Confirmed.** `src/net.c:6042-6079`.

With the setting on, a failed certificate check on a top-level navigation
to a non-HSTS host is retried with verification disabled, with the cookie
share attached and with no prompt. An on-path attacker receives the
user's cookies for that site, `Secure` ones included; the only signal is
the address-bar icon afterwards. SECURITY.md describes the setting as
letting the user "proceed past" an error, which implies a choice.

**Fix.** Show a per-host interstitial before the retry and do not attach
cookies to an unverified request.

### 8. EventSource: missing protocol limits and HSTS

**Low. `#ifdef` confirmed; script-visible impact not tested.**
`src/eventsource.c:385-392`, `:341-349`, `:289-333`.

```c
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#endif
```

`CURLOPT_*` names are enum constants, not preprocessor macros, so these
blocks are never compiled (checked against libcurl 8.5's `curl.h`).
Consequences: redirects follow libcurl's default redirect protocols
rather than `http,https`, and the HTTPS-only clamp for `https:` sources
is absent; there is no HSTS upgrade (WebSocket has one at
`src/js.c:21726`); and the body of a 3xx response without `Location` is
fed to the parser without the CORS and content-type checks.

**Fix.** Remove the `#ifdef`s (the build floor is libcurl 8.5), feed the
parser only after the stream is open, treat any final non-200 status as
fatal, and apply the HSTS upgrade.

### 9. DNS-over-HTTPS setting is never applied

**Low (privacy). Confirmed.** `src/net.c:2497-2501`; same cause at `:2466`
for `CURLOPT_TLS13_CIPHERS`.

The same `#ifdef CURLOPT_…` mistake as finding 8. A user who sets
`doh_url` or `NS_DOH_URL` still resolves names through the system
resolver, and the TLS 1.3 cipher list is not set.

**Fix.** Remove the `#ifdef`s.

### 10. Cross-origin script errors are not muted

**Low. Reproduced.** No "muted errors" handling exists; the only
`"Script error."` string is a conversion fallback at `src/js.c:57453`.

A classic `<script src>` loaded from another origin without CORS reported
its full exception text, file URL and line to the embedding page's
`window.onerror`
(`ReferenceError: undefined_function_SECRETLEAK is not defined @http://localhost:8765/err.js`).
The same detail reached a worker through `importScripts`. Error messages
can embed data from the cross-origin response, which is why browsers
replace them with `Script error.`.

**Fix.** Mark scripts fetched cross-origin without CORS as muted and
report `"Script error."`, an empty file name and zero line and column for
them.

### 11. `Domain` cookies on IP-literal hosts

**Low. Reproduced.** `src/net.c:1612-1621`, `:1873-1886`.

`cookie_domain_matches` does a suffix match without first excluding IP
addresses. A page at `http://127.0.0.1:8765/` ran
`document.cookie = 'sup=1; domain=0.1'` and the cookie was stored as a
domain cookie for `.0.1`. A page on one address can thus plant cookies
that are sent to other addresses sharing trailing octets — for example
from `10.0.0.1` to a router at `192.168.0.1`. A related item, not tested:
`src/net.c:1089-1101` derives the "site" of an IP host from
`psl_registrable_domain`, so unrelated addresses may be treated as
same-site.

With host names the checks held: `Domain=com`, `Domain=co.uk`,
`Domain=other.com` and a sibling subdomain were all refused.

**Fix.** When the host is an IPv4 or IPv6 literal, accept a `Domain`
attribute only if it equals the host, store the cookie host-only, and
treat the whole address as its site.

### 12. Headers and cookies on cross-origin redirects and preflights

**Low. Confirmed.** `src/net.c:6634-6638`, `:6703-6715`, `:6506-6508`.

A page-supplied `Authorization` header is re-sent on every redirect hop,
including to a different origin (the target is preflighted, but the
target itself answers the preflight). The Fetch standard removes
`Authorization` on a cross-origin redirect. Preflight `OPTIONS` requests
are sent without the internal omit-cookies marker.

**Fix.** Drop `Authorization` when a hop changes origin and mark
preflights as credential-less.

### 13. Landlock rule set gaps

**Low (post-compromise). Confirmed.** `src/security.c:525-541`,
`:599-603`; `src/config.c:152-185`.

`handled_access_fs` omits `MAKE_SOCK`, `MAKE_FIFO`, `MAKE_BLOCK`,
`MAKE_CHAR` and `IOCTL_DEV`. Rights that are not handled are allowed
everywhere, so a compromised process can create socket or FIFO inodes
outside the write allow-list. `~/.config/northstar` is read-write inside
the sandbox, so the same process can rewrite `northstar.conf`
(`https_proxy`, `tls_allow_insecure_override`, `safe_browsing`,
`home_url`) and keep its foothold across restarts. The extensions
directory was deliberately placed out of reach; the configuration was
not.

**Fix.** Add the `MAKE_*` rights to the handled set. Write settings from
outside the sandbox, or require confirmation in the UI before a
security-lowering key takes effect.

### 14. `Secure` cookie can be replaced from an `http:` page

**Low. Likely — depends on how libcurl treats `CURLOPT_COOKIELIST`
lines; not tested (needs an HTTPS origin).** `src/net.c:1903-1913`.

Before a script-set cookie is injected, only a collision with an
`HttpOnly` cookie is checked. A page on `http://example.com`, or content
injected into it by a network attacker, may be able to overwrite a
`Secure` cookie of the same name. The HttpOnly protection itself was
tested and holds: script could neither read, create nor overwrite an
`HttpOnly` cookie.

**Fix.** Refuse the write when the document is not HTTPS and a `Secure`
cookie with the same name and a matching domain exists.

### 15. HTTP cache keying and `Cache-Control` parsing

**Low. Confirmed.** `src/cache.c:448-450`, `:648-649`;
`src/net.c:5712-5731`, `:6199-6240`.

`no-store` and `private` are found with a case-sensitive `strstr`, so
`No-Store` or `PRIVATE` is stored. The key is URL, top-site partition and
a limited `Vary`; a response fetched with cookies or a script-set
`Authorization` is reused for a `credentials: 'omit'` request in the same
partition.

**Fix.** Parse directives case-insensitively as tokens, do not store
responses to requests that carried `Authorization`, and include the
credentials mode in the key.

### 16. Hardening flags do not reach the fallback subprojects

**Low. Confirmed.** `meson.build:50-74`;
`subprojects/packagefiles/lexbor/meson.build:25`,
`subprojects/packagefiles/quickjs/meson.build:16-20`,
`subprojects/wuffs/meson.build:47`.

The hardening flags are added with `add_project_arguments`, which does
not apply to subprojects. When lexbor, QuickJS, Wuffs and pl_mpeg are
built from the bundled wraps — the code that parses most untrusted
input — they get no `-fstack-protector-strong`,
`-fstack-clash-protection`, `-fcf-protection` or `_FORTIFY_SOURCE`. There
is also no `-mbranch-protection` on aarch64.

**Fix.** Use `add_global_arguments`, or pass the flags through each
subproject's `c_args`.

### 17. Bytecode cache key ignores the script kind

**Low. Key collision confirmed; runtime effect not tested.**
`src/bytecode_cache.c:107-121`; `src/js.c:57415-57424`, `:57940-57955`.

The key is SHA-256 of site, name and source. The same URL and bytes
loaded once as a classic script and once as a module share an entry. The
module path checks the tag and recompiles; the classic path evaluates
whatever it read. This is confusion between two compilations of the same
attacker-visible source, not attacker-chosen bytecode.

**Fix.** Mix the compile kind and flags into the hash. Two hardening
notes: the on-disk files carry no integrity tag (only a local writer could
abuse that, which is out of scope, but a keyed MAC is cheap), and
`write_disk` uses a fixed `<path>.tmp` name that two processes sharing a
profile can interleave.

### 18. Wrapper finalizer disarms the node-invalidate hook

**Low. Unverified — no reachable path was found.**
`src/js.c:3708-3716`, `:3836-3877`, `:59427-59497`.

`ns_element_finalizer` clears `n->js_invalidate` unconditionally.
`ns_invalidate_wrapper` is the only place that scrubs native per-node
state (focus, image loads, listeners, popover and media state) when a
node is freed, so a node whose wrapper was collected and that is later
freed by the orphan sweep would not be scrubbed. Today this is masked:
main-document wrappers are pinned, and replaced frame documents stay
alive because their realms are never freed (finding 19).

**Fix.** Clear only `js_wrapper` in the finalizer, or have the sweep
scrub every node it frees. This must be fixed before finding 19 is.

### 19. Frame realms are never released before page teardown

**Info (memory growth). Confirmed.** `src/js.c:45605-45610`; freed only
at `:56458` and `:57385`. A page that reloads a frame in a loop
accumulates realms and documents up to the runtime memory cap.

### 20. No global cap on response memory

**Info (denial of service). Confirmed.** `src/net.c:2734-2742`, `:2812`.
Each response may use up to half of available memory, capped at 4 GiB;
parallel large responses or decompression output can exhaust memory.

### 21. Short cache key from a corrupted database

**Info (local tampering only). Confirmed.** `src/cache.c:119-129`,
`:494-498`, `:778-784`. A key read from the SQLite database is indexed at
`key[0]`, `key[1]` and `key + 2` without a length check.

### 22. Smaller items

- **Address bar.** A plain left-anchored entry with no emphasis on the
  registrable domain (`src/gtk/procwindow.c:516-545`); a long
  `login.bank.com.<padding>.evil.tld` shows only its trusted-looking
  prefix. Confirmed.
- **Downloads.** Page-triggered downloads are saved to `~/Downloads`
  without a prompt and with the page-chosen extension; an
  exists-then-write race lets two same-name downloads overwrite each other
  (`src/gtk/procwindow.c:972-1003`). Confirmed.
- **`about:start`** is readable by any page through `fetch()`, exposing
  the configured search-engine URL (`src/js.c:8881-8883`). Confirmed.
- **npm package.** The macOS path verifies the DMG against a
  `SHA256SUMS` file fetched from the same release
  (`npm/lib/install.js:120-141`); this detects corruption, not a replaced
  release. Confirmed.
- **File permissions.** `northstar.conf` and extension storage are
  written with the umask and then `chmod`ed (`src/config.c:505-506`,
  `src/ext.c:327-328`); the 0700 directory covers the window. Confirmed.
- **Logging on Windows.** The debug event log is written to disk by
  default (`src/debuglog.c:34-41`); whether private-mode URLs reach it was
  not traced. Unverified.
- **`document.domain`** accepts any value, including a public suffix
  (`document.domain = 'com'` read back as `com`), but the value had no
  effect on frame access in testing. Reproduced; cosmetic.
- **Crash recovery.** The session file is in `$XDG_RUNTIME_DIR`, which
  Landlock grants read-only, so recovery may not work under the sandbox.
  Functional rather than a security issue. Likely.

## SECURITY.md statements that do not match the code

| Statement | Reality | Finding |
|---|---|---|
| Only `http`, `https` and `ftp`; redirects clamped to HTTPS | Not for EventSource | 8 |
| `NS_DOH_URL` decides where DNS goes | Never applied | 9 |
| `kill`/`tgkill` at other processes is refused | Other allowed syscalls deliver signals | 4 |
| Content threads refuse `socket(AF_UNIX, …)` | Bypassable with high argument bits | 3 |
| Every cookie is `SameSite=Lax`; third-party requests get none | Not for a cross-site frame's requests to the embedder's site | 2 |
| `tls_allow_insecure_override` lets the user proceed | No interaction; automatic retry with cookies | 7 |
| HSTS is enforced | Not for EventSource | 8 |
| Compile-time hardening list | Own objects only, not the fallback subprojects | 16 |
| Known gap: `document.cookie` in a frame uses the frame's own site | Stale — `js_cookie_site` (`src/net.c:1707-1718`) uses the top-level site; the code is stricter than the text | — |

## What was tested and held

**Sanitizer runs.** About thirty hostile-page cases ran with no
AddressSanitizer or UBSan report: ArrayBuffer detachment during
`TextDecoder`, `crypto.getRandomValues`, `crypto.subtle.digest` and
`putImageData`; out-of-range `getImageData`, `createImageData` and
`ImageData` arguments; node removal inside event listeners,
MutationObserver, NodeIterator and TreeWalker callbacks, custom-element
reactions and attribute `toString` coercion; Range use after its
container was emptied; a 20,000-deep DOM tree through `innerHTML`,
`cloneNode`, `querySelectorAll` and layout; 5,000-deep `calc()` and
`:is()` nesting; and a table of 1,200 cells with `colspan=100
rowspan=100`. This is a smoke test, not a fuzzing campaign.

**Same-origin policy** (page on `127.0.0.1`, other origin `localhost`;
and `a.example.com` / `b.example.com` / `victim.com` through the proxy):

- Cross-origin frame: `contentDocument` is `null`; `contentWindow`
  access to `document`, variables, `eval`, `Function`, `localStorage`,
  `fetch` and `location.href` throws `SecurityError`; a `javascript:` URL
  assigned to its `location` did not run.
- `fetch` and `XMLHttpRequest` cross-origin without CORS are blocked;
  `no-cors` gives an opaque, empty response; a same-origin redirect to
  another origin is blocked; a redirect to `file:` is refused.
- `file:` URLs are refused for fetch, XHR, images, scripts and frames
  from an `http:` page.
- Canvas tainting: `getImageData` and `toDataURL` throw after drawing a
  cross-origin image.
- Cross-origin stylesheets do not expose their rules.
- Workers and service workers from another origin are refused.
- `sandbox` frames: scripts blocked without `allow-scripts`; with
  `allow-scripts` alone the frame has an opaque origin, no cookies, no
  storage and no parent access. Top navigation is enforced at
  `src/js.c:56073`.
- Popups without user interaction are blocked.
- Cookies: `HttpOnly` is invisible to and cannot be created or
  overwritten by script; `Secure`, `__Secure-` and `__Host-` are refused
  on `http:`; public-suffix, foreign and sibling `Domain` values are
  refused; no cookies go to unrelated sites.
- Request headers containing CR/LF and forbidden methods (`TRACE`) are
  rejected.

**Parsers and decoders, by reading:** `src/mp4.c` (all of it),
`src/audio/aac.c` (all of it), `src/image_wuffs.c` (all of it),
`src/webm.c` (EBML readers, lacing and block handling), the PCM growth
paths of `src/audio/audio.c`, the table column, rowspan and grid-track
indexing in `src/layout.c`, recursion limits in `src/dom.c`,
`src/html_lexbor.c`, `src/layout.c` and `src/html.c`, and every fixed-size
stack buffer and `memcpy` in `src/svg.c`, `src/css*.c`, `src/dom.c`,
`src/paint.c`, `src/encoding.c`, `src/font.c`, `src/mathml.c` and
`src/anim.c`. All are bounds-checked; cursor-style readers with sticky
overrun flags are used consistently, sizes are checked before
multiplication, and no `strcpy`, `strcat`, `sprintf` or `alloca` appears.

**Network, shell and sandbox, by reading:** TLS verification on every
handle; per-hop redirect checks on the main fetch path; CR/LF rejection in
headers; chrome-only gating of `about:settings`, `about:config`,
`about:history` and `view-source:`; escaping in generated internal pages;
response header and content-decoding limits; WebSocket framing limits; CSP
and CORS preflight logic; Safe Browsing response parsing; extension
loading and resource confinement; download file-name sanitising; process
spawning (argv only, no shell); sandbox ordering, `NO_NEW_PRIVS`, TSYNC
and the absence of `execve`, `ptrace`, `bpf`, `mount`, `unshare` and
`io_uring` from the allow-list; CI workflows (actions pinned to commit
hashes, read-only token, no `pull_request_target`, no event data
interpolated into `run:`); pinned wraps and container digests.

**JS bindings, by reading:** canvas `putImageData`, `getImageData`,
`ImageData` and `createImageBitmap`; `crypto.getRandomValues`; structured
clone of buffers; OfflineAudioContext rendering; worker message
transport; IndexedDB value serialisation; bytecode cache location and
permissions.

## Not covered

This review does not show the absence of vulnerabilities in the
following, which were not read or only sampled:

- **Most of `src/js.c`** (61,700 lines): about 130 `JS_GetOpaque` call
  sites, Fetch/Response body handling, Blob/File/FileReader, the zlib
  streams, `document.write`, and the raw `ns_node *` fields of
  `struct ns_js` other than `focused_node` and `js_image_loads`. The
  delegated pass over this file ended early; the sanitizer cases above
  cover some of the same ground at run time only.
- **Not opened:** `src/wasm.c`, `src/webcrypto.c`, `src/js_webcrypto.c`,
  `src/js_encoding.c`, `src/js_intl.c`, `src/js_date.c`, `src/js_media.c`,
  `src/js_mse.c`, `src/js_perf.c`, `src/js_receiver.c`, `src/js_realm.c`,
  `src/quickjs_compat.c`, `src/webaudio.c`, `src/forms.c`, `src/xml.c`,
  `src/mse.c`, `src/video.c`, `src/h264.c`, `src/av1.c`,
  `src/gstvideo.c`, `src/mfvideo.c`, `src/web-text-lib/`.
- **Sampled only:** `src/css.c` (35,800 lines) and the bulk of
  `src/layout.c` and `src/paint.c`; `src/idb.c`, `src/engine.c`,
  `src/page_session.c`; `src/headless.c`, `src/print.c`, most of
  `src/gtk/procview.c` and `src/gtk/procwindow.c`.
- **Threading:** worker threads against engine-thread state,
  `SharedArrayBuffer`, the curl-multi thread and request coalescing in
  `src/net.c`, and whether any thread exists before
  `landlock_restrict_self` (Landlock has no TSYNC).
- **Policy paths not exercised:** `postMessage` target-origin matching,
  `window.open`/`opener` with a user gesture, `X-Frame-Options` and
  `frame-ancestors`, HSTS and mixed content (no HTTPS origin in the test
  setup), frame-level MIME handling for finding 1, and URL-bar display in
  the GTK shell.
- **Platforms:** macOS Seatbelt and the Windows mitigation policies were
  read but not run; findings 3 and 5 were not tested on a live kernel or
  on Windows.

## Suggested order of work

1. Finding 1 (document MIME handling) — the only one a hostile page can
   use directly against an unmodified third-party site.
2. Finding 2 (cross-site frame cookies).
3. Findings 3, 4 and 13 together (sandbox rules), then finding 5.
4. Findings 8 and 9: delete the `#ifdef CURLOPT_…` guards.
5. Findings 6, 10, 11, 12, 14 and 15.
6. Update SECURITY.md for the statements in the table above.
7. A fuzzing pass over the JS bindings and the uncovered files, since
   that is where this review is thinnest.
