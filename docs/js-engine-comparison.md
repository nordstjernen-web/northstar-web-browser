# quickjs-ng vs QuickJS: measured comparison

Northstar Web Browser builds on either of two JavaScript engines (see
[quickjs.md](quickjs.md)): **quickjs-ng 0.17.0**, the default
(`subprojects/quickjs-ng.wrap`, tag `v0.17.0`), and Fabrice Bellard's
original **QuickJS 2026-06-04** (`-Djs_engine=quickjs`,
`subprojects/quickjs.wrap`, commit `3d5e064`). This
report builds both from the same tree, runs them through the same tests
and benchmarks, and compares the results.

Measured 2026-10-02 at `da7e522` (1.0.13-dev). That tree still carried
the 1.0.12 standards-conformance backport, which has since been withdrawn
(see the 1.0.13 changelog), so the web-platform-tests counts below include
work that is no longer in Northstar Web Browser.

## Verdict

**Keep quickjs-ng as the default.** Bellard's QuickJS is the faster
interpreter. It scores 11 % higher on Octane and is up to 2.2× faster
on strings, JSON and BigInt. It also has no equivalent of quickjs-ng's
integer-key `Map`/`Set` slowdown, which reaches 32×. But on real
framework pages the gap shrinks to 1.7 % (Speedometer 3.1), because most
of the time goes to Northstar Web Browser's C DOM, layout and style, which both
engines share.

quickjs-ng wins on the parts that matter more for a browser:

- **Conformance.** It passes 551 more web-platform-tests subtests and 6
  more language-feature probes, with `using`, `Array.fromAsync`,
  `Iterator.zip` and the V8-style `Error` API.
- **CI coverage.** It is the build CI exercises.
- **Maintenance.** It has a newer upstream release.
- **Stability.** The original-engine build had a crash that CI never saw
  (see below).

Startup time, memory, smoke tests and DOM-bound work are identical within
noise.

This comparison found and fixed one bug: the original-engine build
crashed when it opened an ES-module page from the bytecode cache. It also
identified a quickjs-ng performance defect worth reporting upstream (or
patching through the wrap), and a `DOMException` polyfill gap that only
the original-engine build exposes. See [Issues found](#issues-found).

## Summary

| Metric | quickjs-ng 0.17.0 (default) | QuickJS 2026-06-04 | Better |
|---|---|---|---|
| Clean build, wall / CPU | 1 m 22 s / 268 s | 1 m 07 s / 203 s | QuickJS |
| Binary size (stripped) | 10.43 MB | 10.12 MB (−3.0 %) | QuickJS |
| Headless startup, `about:start` | 262 ± 14 ms | 259 ± 10 ms | tie |
| Peak RSS: headless start / GUI on a React page | 99 / 311 MB | 98 / 311 MB | tie |
| Smoke fixtures, cute-tests | 9/9, identical output | 9/9, identical output | tie |
| Octane 2.0, geometric mean of 14 | 2251 | **2501 (+11.1 %)** | QuickJS |
| DOM-binding micro-benchmarks (8) | within ±10 % | within ±10 % | tie |
| String / JSON / BigInt / typed-array micro-benchmarks | 1.0× | **1.3–2.2× faster** | QuickJS |
| `async`/`await`, promise chains, generators | **7–14 % faster** | 1.0× | quickjs-ng |
| `Map`/`Set` with integer keys | 3–32× slower (hashing bug) | **1.0×** | QuickJS |
| Speedometer 3.1, 23 TodoMVC suites | 2.061 | **2.097 (+1.7 %)** | QuickJS (slightly) |
| web-platform-tests, 1,538 files / 111,621 subtests | **97,946 (87.75 %)** | 97,395 (87.26 %) | quickjs-ng |
| Language-feature probes (68) | **63** | 57 | quickjs-ng |
| Maximum recursion depth | 5,648 frames | **7,445 frames** | QuickJS |
| Stability during this run | no failures | crashed reopening ES-module pages (fixed here) | quickjs-ng |
| CI coverage | every workflow | none | quickjs-ng |
| Upstream release | **2026-09-18** | 2026-06-04 | quickjs-ng |
| Adapter code in Northstar Web Browser | **~210 lines** | ~560 lines | quickjs-ng |

## Test setup

- **Machine.** 4 vCPU Intel Xeon @ 2.10 GHz (x86-64), 15 GB RAM, Ubuntu
  24.04, Linux 6.18, GCC 13.3.0. No GPU, so pages render through the
  software (cairo) renderer.
- **Builds.** Two build directories from one checkout, each with the
  project's default release flags (`-O3`, LTO, PIE, `NDEBUG`):
  `meson setup builddir` (quickjs-ng) and
  `meson setup builddir-quickjs -Djs_engine=quickjs`. ccache was
  disabled for the timed builds.
- **How pages were driven.** Every page ran in the real browser binary
  in headless mode (`--headless --dump=none`), served over a local HTTP
  server. Timings come from `performance.now()` inside the page and are
  reported through `console.log` (`--debug=js`). Peak memory is the
  process's `VmHWM` (peak RSS). For each test, the two engines ran in
  alternating order (A B, B A, A B …) so drift on the machine hits both
  equally.
- **Repetition.** Octane: 3 runs per benchmark per engine, median.
  Micro-benchmarks: 3 page loads × 5 repetitions per engine, median.
  Speedometer: 5 runs per suite per engine, median. Startup:
  `hyperfine`, 20 runs (10 for the cold-cache case).

## Results

### Build, binary and integration cost

| | quickjs-ng 0.17.0 | QuickJS 2026-06-04 |
|---|---|---|
| Clean build, wall clock (4 jobs) | 1 m 22 s | 1 m 07 s |
| Clean build, CPU time (user) | 268 s | 203 s |
| Ninja build steps | 398 | 390 |
| Engine objects, compile only (`-j1`) | 3.9 s | 3.8 s |
| Final LTO link of `northstar` | 40.3 s | 37.5 s |
| Warnings (project sources) | 0 | 0 |
| `northstar` binary (stripped) | 10,430,592 B | 10,115,456 B (−3.0 %) |
| `.text` size | 9,376,158 B | 9,065,170 B |
| Engine C sources compiled into the browser | 72,158 lines | 69,024 lines |
| Adapter code compiled in (`src/quickjs_compat.*`) | ~210 lines | ~560 lines (the same ~190 shared lines plus ~375 for the original only) |
| Local patches | 1 (Windows link) | 1 (sort comparator) + a meson build file |
| Built in CI | every workflow (four when measured, five since `linux-i386`) | none |
| Upstream release | v0.17.0, 2026-09-18 | 2026-06-04 |

The adapter line counts are those of `da7e522`. Withdrawing the 1.0.12
backport removed the shims it had added, and `src/quickjs_compat.c` and
`src/quickjs_compat.h` together went from 584 to 360 lines.

The quickjs-ng build is slower mainly because its subproject also builds
the `qjs`/`qjsc` command-line tools and `quickjs-libc`, which the browser
never links. Building only the library target would remove most of the
difference.

### Startup, memory and smoke tests

| | quickjs-ng | QuickJS |
|---|---|---|
| `--headless --dump=text about:start`, warm bytecode cache | 261.6 ± 13.7 ms | 258.9 ± 10.1 ms |
| Same, cold (bytecode cache deleted before each run) | 290.9 ± 8.4 ms | 293.5 ± 14.9 ms |
| `data/fixtures/js-dom.html` headless | 141.4 ± 4.1 ms | 138.3 ± 6.7 ms |
| Peak RSS, headless `about:start` | 98.5–99.9 MB | 97.9–98.1 MB |
| JS bytecode cache on disk after startup | 232 KB | 260 KB |
| GUI under Xvfb, total RSS after 8 s: `js-dom.html` (3 runs) | 189.7–190.5 MB | 189.7–190.2 MB |
| GUI under Xvfb, total RSS after 8 s: TodoMVC React-Complex (3 runs) | 310.5–311.5 MB | 310.4–311.9 MB |
| `scripts/dev.sh smoke` (9 fixtures) | 9/9 OK | 9/9 OK |
| `cute-tests/*.html` output | identical | identical |

Both GUI builds launch, render and stay up. Startup time is the same within
noise: most of it is GTK, fontconfig and the bytecode cache, not the engine.

### Octane 2.0 (pure JavaScript, in the page)

Score per benchmark (higher is better), median of 3, and peak RSS.

| Benchmark | quickjs-ng | QuickJS | QuickJS faster by | peak RSS ng / QuickJS (MB) |
|---|---:|---:|---:|---:|
| Richards | 648 | 768 | +18.5 % | 47 / 47 |
| DeltaBlue | 639 | 704 | +10.2 % | 47 / 47 |
| Crypto | 775 | 895 | +15.5 % | 46 / 46 |
| RayTrace | 1355 | 1340 | -1.1 % | 46 / 46 |
| EarleyBoyer | 1744 | 1898 | +8.8 % | 60 / 60 |
| RegExp | 360 | 383 | +6.4 % | 50 / 49 |
| Splay | 7669 | 7543 | -1.6 % | 195 / 186 |
| NavierStokes | 1430 | 1985 | +38.8 % | 48 / 48 |
| PdfJS | 3476 | 3722 | +7.1 % | 66 / 67 |
| Mandreel | 3988 | 5992 | +50.3 % | 124 / 118 |
| Gameboy | 5752 | 6464 | +12.4 % | 67 / 68 |
| CodeLoad | 12874 | 12878 | +0.0 % | 103 / 104 |
| Box2D | 3008 | 3034 | +0.9 % | 52 / 52 |
| zlib | did not finish | did not finish | — | 177 / 176 |
| Typescript | 9271 | 9427 | +1.7 % | 178 / 181 |
| **Geometric mean (14)** | **2251** | **2501** | **+11.1 %** | |

zlib never finishes on either engine. A single iteration of its
Emscripten-compiled inflate loop runs longer than the browser's 60-second
per-script budget (`js_eval_budget_ms`), so the watchdog interrupts it
(`InternalError: interrupted`). It is left out of the geometric mean.

QuickJS is more than 5 % ahead on 9 of the 14 benchmarks that finish.
The other five (RayTrace, Splay, CodeLoad, Box2D and Typescript) are
within ±2 %. Its biggest wins are on numeric and typed-array code:
Mandreel (+50 %), NavierStokes (+39 %) and Crypto (+15 %). Splay
allocates the most and is the noisiest; quickjs-ng peaks about 5 % higher
on it.

### Micro-benchmarks: DOM bindings and built-ins

Median ms (lower is better). The `dom.*` rows spend most of their time
crossing into Northstar Web Browser's C DOM. The `js.*` rows stay inside the engine.
"ng slower by" is the quickjs-ng time over the QuickJS time.

| Test | quickjs-ng | QuickJS | ng slower by |
|---|---:|---:|---:|
| `dom.createElement+append (20k)` | 76.6 | 69.7 | +10 % |
| `dom.setAttribute/getAttribute (100k)` | 104.4 | 97.2 | +7 % |
| `dom.innerHTML parse (2k rows x10)` | 78.3 | 77.3 | +1 % |
| `dom.querySelectorAll (5k nodes x200)` | 64.8 | 68.3 | -5 % |
| `dom.events dispatch (50k)` | 587.3 | 567.3 | +4 % |
| `dom.textContent/style write (50k)` | 560.2 | 558.0 | +0 % |
| `dom.getBoundingClientRect w/ relayout (2k)` | 58.5 | 57.0 | +3 % |
| `dom.tree walk childNodes (10k nodes x50)` | 148.8 | 146.1 | +2 % |
| `js.JSON parse+stringify (2MB x5)` | 564.0 | 444.2 | +27 % |
| `js.string concat/split/join (200k)` | 134.4 | 59.8 | +125 % |
| `js.regexp exec (email-ish, 100k)` | 149.8 | 134.6 | +11 % |
| `js.object property churn (1M)` | 273.8 | 246.0 | +11 % |
| `js.Map/Set ops (500k)` | 1103.5 | 92.5 | +1093 % |
| `js.Array sort numbers (300k)` | 212.7 | 202.1 | +5 % |
| `js.closures + higher-order (map/filter/reduce 1M)` | 210.7 | 230.1 | -8 % |
| `js.class/method dispatch (2M calls)` | 451.3 | 459.7 | -2 % |
| `js.typed array math (Float64Array 4M ops)` | 453.2 | 334.3 | +36 % |
| `js.BigInt factorial(2000) x5` | 5.8 | 2.9 | +100 % |
| `js.try/catch throw (100k)` | 91.3 | 90.0 | +1 % |
| `js.generators/iterators (1M yields)` | 54.3 | 59.5 | -9 % |
| `js.destructuring/spread (500k)` | 341.8 | 307.3 | +11 % |
| `js.eval/new Function compile (2k)` | 17.0 | 16.0 | +6 % |
| `js.Promise chain microtasks (200k)` | 239.5 | 256.7 | -7 % |
| `js.async/await loop (200k)` | 106.0 | 123.4 | -14 % |

Peak RSS for the whole page: 563 MB (quickjs-ng) vs 556 MB (QuickJS).

- **DOM-bound work is engine-neutral.** Both engines call the same C
  bindings, so the `dom.*` rows agree to within ±10 %.
- **QuickJS is much faster on strings, JSON, BigInt and typed arrays.**
  String concat/split/join is 2.2× faster, JSON 1.27×, BigInt 2× and
  `Float64Array` math 1.36×.
- **quickjs-ng is faster on async code.** `async`/`await` (14 %),
  promise chains (7 %), generators (9 %) and closures passed to
  `map`/`filter`/`reduce` (8 %) all run faster on quickjs-ng.
- **Integer-keyed `Map`/`Set` are pathologically slow on quickjs-ng.** See
  the next section.

### quickjs-ng: integer keys in `Map` and `Set`

| 500 k operations | quickjs-ng | QuickJS | ratio |
|---|---|---|---|
| `Map.set(i % 5000, …)`: repeated small-integer keys | 769 ms | 33 ms | 24× |
| `Set.add(i % 7000)` | 832 ms | 26 ms | 32× |
| `Set.add(i)`: 500 k distinct integers | 335 ms | 105 ms | 3.2× |
| `Map.set(i, i)`: 500 k distinct integers | 323 ms | 94 ms | 3.4× |
| `Map.set(keys[i % 5000], …)`: string keys | 40 ms | 45 ms | 0.9× |
| `Map.get` with string keys | 40 ms | 40 ms | 1.0× |

The standalone `qjs` built from the same quickjs-ng subproject shows the
same 776 ms, so this comes from the engine itself, not from Northstar Web Browser.

The cause is `map_hash_key()` in quickjs-ng's `quickjs.c`. It turns an
integer key into its float64 bit pattern, hashes it as
`(lo32 ^ hi32) * 3163`, and then keeps the **low** bits for the bucket.
A small integer stored as a double has an all-zero low word and many
trailing zero bits in the high word. Multiplying by an odd constant keeps
those zero bits, so most small-integer keys land in a few buckets, and
lookups walk long chains. Bellard's QuickJS mixes all 64 bits
(`map_hash64`) and takes the bucket from the **high** bits, so it does not
have this problem.

This is an upstream bug and should be reported to quickjs-ng. Real code
hits it often: ids, indices and counters are common `Map` keys, and
frameworks keep `Set`s of numeric ids.

### Speedometer 3.1 (TodoMVC workloads)

This uses the method of `scripts/speedometer-bench.sh`: each TodoMVC
suite loads as the top-level page and replays Speedometer's own
Adding100Items, CompletingAllItems and DeletingAllItems steps. Each suite
ran 5 times per engine, with the engines alternating. The table shows the
median total ms per suite (lower is better). The score is the geometric
mean of 1000/total (higher is better). `javascript-es6-webpack-complex`
does not load on either build.

| Suite | quickjs-ng (ms) | QuickJS (ms) | QuickJS faster by |
|---|---:|---:|---:|
| TodoMVC-Angular-Complex-DOM | 348.4 | 333.2 | +4.6 % |
| TodoMVC-Angular | 207.2 | 199.8 | +3.7 % |
| TodoMVC-Backbone-Complex-DOM | 1167.7 | 1180.3 | -1.1 % |
| TodoMVC-Backbone | 278.7 | 249.4 | +11.7 % |
| TodoMVC-jQuery-Complex-DOM | 2211.0 | 2190.2 | +0.9 % |
| TodoMVC-jQuery | 1402.2 | 1384.9 | +1.2 % |
| TodoMVC-Lit-Complex-DOM | 1794.6 | 1830.5 | -2.0 % |
| TodoMVC-Lit | 682.8 | 662.0 | +3.1 % |
| TodoMVC-Preact-Complex-DOM | 225.3 | 215.5 | +4.5 % |
| TodoMVC-Preact | 90.7 | 92.0 | -1.4 % |
| TodoMVC-React-Complex-DOM | 584.4 | 579.4 | +0.9 % |
| TodoMVC-React-Redux-Complex-DOM | 661.6 | 656.7 | +0.7 % |
| TodoMVC-React-Redux | 504.6 | 528.2 | -4.5 % |
| TodoMVC-React | 462.0 | 456.3 | +1.2 % |
| TodoMVC-Svelte-Complex-DOM | 198.7 | 195.7 | +1.5 % |
| TodoMVC-Svelte | 82.5 | 78.5 | +5.1 % |
| TodoMVC-Vue-Complex-DOM | 318.7 | 308.8 | +3.2 % |
| TodoMVC-Vue | 181.7 | 178.5 | +1.8 % |
| TodoMVC-JavaScript-ES5-Complex-DOM | 706.0 | 682.9 | +3.4 % |
| TodoMVC-JavaScript-ES5 | 510.2 | 510.8 | -0.1 % |
| TodoMVC-JavaScript-ES6-Webpack | 531.6 | 538.9 | -1.4 % |
| TodoMVC-WebComponents-Complex-DOM ¹ | 2388.9 | 2348.2 | +1.7 % |
| TodoMVC-WebComponents ¹ | 833.0 | 823.7 | +1.1 % |
| **Score (geomean of 1000/total, 23 suites)** | **2.061** | **2.097** | **+1.7 %** |

¹ On the original QuickJS these two suites crashed on every load after
the first, until the cached-module fix described under
[Issues found](#issues-found). Their rows come from 4 runs per engine
after the fix.

QuickJS is faster on 17 of the 23 suites, but only Backbone (+12 %) and
Svelte (+5 %) are more than 5 % apart. On these pages most of the time goes to the C DOM,
style and layout, which both builds share.

### Language features (ES2019–ES2026 plus extensions)

A page that probes 68 features, each with a small evaluated snippet, in
the browser (so Northstar Web Browser's own polyfills count).

| | quickjs-ng | QuickJS |
|---|---|---|
| All 68 probes | **63** | 57 |
| ES2019–ES2026 (46 probes) | **45** | 43 |
| `Array.fromAsync` (ES2026) | yes | no |
| `using` / `DisposableStack` (ES2026) | yes | no (`SyntaxError`) |
| `Iterator.zip` (stage 3) | yes | no |
| `Error.captureStackTrace`, `stackTraceLimit`, `prepareStackTrace` | yes | no |
| `Atomics.waitAsync` | no | no |
| Locale-aware `localeCompare` (`'é' < 'f'`) | no | no |
| Proper tail calls | no | no |
| `Error.stack` has `url:line:col` frames | yes | yes |
| Maximum recursion depth, `function f(){f()}` | 5,648 frames (`RangeError`) | 7,445 frames (`InternalError`) |
| Error stack format | `at f (url:line:col)` | same; columns can differ |

Both engines have Set methods, iterator helpers, `RegExp` `v` flag and
modifiers, duplicate named groups, `Float16Array`, `RegExp.escape`,
`Promise.try`, `Math.sumPrecise`, `Error.isError`, the `Uint8Array`
base64/hex methods, `Map.prototype.getOrInsert` and `JSON.rawJSON`.
`Temporal` and `Intl` exist on both, because Northstar Web Browser supplies them
(`src/js_date.c` and friends), not the engine. Neither engine runs
10,000-deep simple recursion: page JavaScript gets a 5 MB stack, which
quickjs-ng's larger frames fill sooner.

`Error.captureStackTrace` matters in practice: many npm libraries call it
whenever it exists. On the original engine they take their fallback path.

### web-platform-tests

The test files cover 19 areas of an upstream WPT checkout (`7108b8d`,
2026-10-02), served by WPT's own `./wpt serve`. They were run through
`--wpt` with a 15 s per-test timeout, 4 at a time. The legacy multi-byte
encoder tests (`encoding/legacy-mb-*`) were left out because they exercise
Northstar Web Browser's C decoders, not the engine. The 19 files whose results
differed were then re-run 3 more times per engine, one at a time with the
engines alternating. The table uses those re-run results for those
files.

| Area | files | quickjs-ng pass / subtests | QuickJS pass / subtests |
|---|---:|---:|---:|
| compression | 20 | 188 / 320 | 188 / 320 |
| console | 12 | 23 / 29 | 23 / 29 |
| dom/abort | 6 | 33 / 37 | 33 / 37 |
| dom/events | 205 | 784 / 877 | 784 / 877 |
| dom/nodes | 349 | 13030 / 13212 | 13030 / 13212 |
| dom/ranges | 68 | 44496 / 44697 | 44014 / 44697 |
| dom/traversal | 18 | 1602 / 1608 | 1602 / 1608 |
| domparsing | 73 | 444 / 1941 | 444 / 1941 |
| encoding | 48 | 8579 / 8684 | 8579 / 8684 |
| fetch/api | 84 | 501 / 1107 | 501 / 1107 |
| FileAPI | 69 | 488 / 723 | 488 / 723 |
| hr-time | 13 | 18 / 24 | 18 / 24 |
| html/webappapis | 23 | 167 / 190 | 167 / 190 |
| IndexedDB | 228 | 375 / 1370 | 375 / 1370 |
| js | 26 | 113 / 143 | 113 / 143 |
| streams | 85 | 197 / 1252 | 197 / 1252 |
| url | 35 | 7397 / 8631 | 7397 / 8631 |
| WebCryptoAPI | 131 | 19312 / 26428 | 19312 / 26428 |
| webidl | 45 | 199 / 348 | 130 / 348 |
| **Total** | **1538** | **97946 / 111621 (87.75 %)** | **97395 / 111621 (87.26 %)** |

The 11 IndexedDB files that differed in the parallel run gave identical
results on both engines in all 3 re-runs, so they were timing flakes
under load. The 8 genuine differences all involve `DOMException`:

- **QuickJS loses 552 subtests:** `Range-surroundContents.html` (482,
  which compares thrown codes with `DOMException.HIERARCHY_REQUEST_ERR`),
  `DOMException-constants` (50), `-custom-bindings` (8),
  `-constructor-behavior` (7), `-constructor-and-prototype` (2),
  `exceptions.html` (2) and `-stack-accessor` (1). On the original engine
  `DOMException` comes from Northstar Web Browser's fallback in `data/js/polyfills.js`,
  which is not shaped like the WebIDL interface.
- **quickjs-ng loses 1 subtest:** `DOMException-is-error`.
  quickjs-ng's native `DOMException` is not an `Error` to `Error.isError()`.

Everything else, including all 26,428 WebCryptoAPI, 44,697 Range and
8,631 URL subtests, is identical. The engines differ in pure language
features and speed. The DOM, networking and crypto code they call into is
the same.

## Issues found

1. **Fixed here: cached ES modules crashed the original-engine build.**
   `ns_js_compile_module_cached()` (`src/js.c`) read a module back from
   the bytecode cache with `JS_ReadObject()` and evaluated it without
   calling `JS_ResolveModule()`. quickjs-ng resolves imports during
   evaluation. Bellard's engine expects the caller to resolve them, as its
   own `qjs` does. It then dereferenced a NULL import in
   `js_create_module_function()`. Every site built from ES modules
   segfaulted on the second visit; the Speedometer WebComponents TodoMVC
   crashed on 12 of 12 warm-cache loads. The fix resolves the module after
   reading it, and has been verified on both engines.
2. **quickjs-ng: integer keys hash into a few buckets** (`map_hash_key()`,
   see above). `Map`/`Set` operations on small integers are 3–32× slower
   than on the original engine. This is worth an upstream report, or a
   small wrap patch that hashes the full 64 bits and takes the bucket from
   the high bits, as Bellard's `map_hash64` does.
3. **Northstar Web Browser: the `DOMException` fallback is not WebIDL-shaped.** Only
   the original-engine build uses it:
   - its constants are named `IndexSizeError_CODE` instead of
     `INDEX_SIZE_ERR`;
   - nothing is on the prototype;
   - it can be called without `new`;
   - its `prototype` is writable.

   Fixing it in `data/js/polyfills.js` would recover the 552 WPT subtests
   above.
4. **quickjs-ng: two small upstream bugs.** `Error.isError(new
   DOMException())` is `false`. And after a page sets
   `Error.prepareStackTrace` and then deletes it, every new error's
   `stack` still goes through the deleted function.
5. **Both engines: Octane's zlib cannot finish.** One iteration exceeds
   the 60 s script budget, which is working as designed.
6. **Build: the quickjs-ng subproject builds `qjs`, `qjsc` and
   `quickjs-libc`,** which the browser does not link.

## Reproducing

```sh
meson setup builddir && meson compile -C builddir
meson setup builddir-quickjs -Djs_engine=quickjs && meson compile -C builddir-quickjs

./scripts/dev.sh smoke
NS_BIN=$PWD/builddir-quickjs/src/gtk/northstar ./scripts/dev.sh smoke

NS_BIN=$PWD/builddir/src/gtk/northstar ./scripts/speedometer-bench.sh
NS_BIN=$PWD/builddir-quickjs/src/gtk/northstar ./scripts/speedometer-bench.sh

cd ~/wpt && ./wpt make-hosts-file | sudo tee -a /etc/hosts && ./wpt serve &
scripts/wpt-run.sh --wpt-root=$HOME/wpt --no-serve dom/nodes dom/ranges webidl
NS_BIN=$PWD/builddir-quickjs/src/gtk/northstar \
    scripts/wpt-run.sh --wpt-root=$HOME/wpt --no-serve dom/nodes dom/ranges webidl
```

The Octane suite came from <https://github.com/chromium/octane>. Each
benchmark ran as its own page: `base.js` plus the benchmark's files,
driven by `BenchmarkSuite.RunSuites` from the `load` event, with the
results logged to the console. The micro-benchmark and feature-probe pages
were written for this comparison. They are not in the tree; the
measurements above list what each one does.
