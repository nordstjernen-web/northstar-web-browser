# HTML and CSS compliance

Where Northstar Web Browser's hand-written engine stands against the HTML and CSS
specifications, how the numbers here were produced, and which gaps are
known.

Northstar Web Browser carries no upstream browser engine. Every behaviour on this
page is implemented in `src/` — the parser glue in `src/html_lexbor.c`,
the cascade and value parsing in `src/css.c`, layout in `src/layout.c`,
paint in `src/paint.c`, and the DOM/CSSOM bindings in `src/js.c`. That
means compliance is not inherited from anywhere; it is measured.

## Reference specifications

- [HTML](https://html.spec.whatwg.org/) — parsing, the element
  definitions, scripting, forms.
- [DOM](https://dom.spec.whatwg.org/) — node trees, mutation, events.
- [CSSOM](https://drafts.csswg.org/cssom/) and
  [CSSOM View](https://drafts.csswg.org/cssom-view/) — `getComputedStyle`,
  `CSSStyleDeclaration`, stylesheet objects.
- [CSS Cascade](https://drafts.csswg.org/css-cascade/),
  [CSS Values](https://drafts.csswg.org/css-values/),
  [CSS Color](https://drafts.csswg.org/css-color/),
  [Selectors](https://drafts.csswg.org/selectors/).

## How the numbers are produced

The engine runs [web-platform-tests](https://github.com/web-platform-tests/wpt)
through headless mode. `--wpt` injects a completion hook
(`data/js/wpt-hook.js`) before any document script runs, waits for
`testharness.js` to finish, and reports each subtest on stdout.

```sh
git clone --depth 1 https://github.com/web-platform-tests/wpt.git ~/wpt
cd ~/wpt && ./wpt make-hosts-file | sudo tee -a /etc/hosts
cd ~/wpt && ./wpt serve &

cd nordstjernen-web-browser
scripts/dev.sh build
scripts/wpt-run.sh --wpt-root=$HOME/wpt --no-serve dom/nodes css/cssom
```

Three runners live in `scripts/`:

- **`wpt-run.sh`** — the stock-checkout runner above. It enumerates the
  tests under each path, serves them with WPT's own `./wpt serve` unless
  `--no-serve` says one is running at `--base`, runs each through the
  browser with a per-test `--timeout-ms` (15 s by default), and
  aggregates per-file and per-subtest results; `--results=FILE` appends
  one JSON line per test, and `--list` prints the enumerated URLs.
- **`wpt-local.sh`** — serves a checkout (`NS_WPT_ROOT`, default `~/wpt`)
  over a plain static HTTP server and prints one summary line per file.
  It needs no WPT server infrastructure, so multi-origin and server-side
  tests do not work under it. The sparse-checkout re-measurements below
  were served the same way.
- **`wpt-fast.sh`** — runs the
  [wpt-fast](https://github.com/nordstjernen-web/wpt-fast) checkout
  (`--fast-root`, default `~/wpt-fast`) in parallel (`--jobs`, up to 8),
  writes a `wptreport.json` and prints per-standard scores through the
  checkout's `./wpt-score`. The wpt-fast repository is no longer
  published, so this runner needs an existing checkout of it; use
  `wpt-run.sh` or `wpt-local.sh` otherwise.

Only `testharness.js` tests run. Reftests and wdspec tests need
screenshot comparison or WebDriver, which this edition does not have,
and the runners do not enumerate `*.worker.js` tests; `.any.js` tests run
in their window scope only.

Two caveats when reading any of this:

- **Subtest counts move.** Fixing an early failure often lets a file
  reach subtests it previously never got to, so the denominator grows
  along with the numerator. Compare passing counts, not just percentages.
- **Some files are timing-sensitive.** A test that loads iframes and
  calls `done()` from `onload` can time out under load and report zero
  subtests. Re-run a single file before treating it as a regression.

## Latest reading per area

The most recent measurement of each area, gathered from the dated runs
under *Measurement history*. They were taken against different
checkouts — a full WPT checkout for the rows measured at `7b38d66`, and
sparse checkouts served statically for the rest — so a denominator is
comparable only with rows from the same run. Nothing after release 1.0.8
has been re-measured: the work recorded in `Changelog.md` for 1.0.9
through 1.0.13 (among it the colour serialisation, writing-mode flexbox
and document-metadata fixes, and in 1.0.13 grid row spans beyond 24 rows,
grid rows sized from spanning items in order of increasing span, the
`Node` receiver checks and `postMessage()` delivered as a task) is not
reflected here, so treat each row as a floor. The standards-conformance
backport released in 1.0.12 was withdrawn in 1.0.13 and none of these
runs measured it.

| Area | Subtests | Pass rate | Measured at |
| --- | --- | --- | --- |
| `html/dom` | 60099 / 60879 | 98.7% | `7b38d66`, 2026-07-29 |
| `dom/nodes` | 11465 / 11680 | 98.2% | `7b38d66`, 2026-07-29 |
| `dom/events` | 738 / 781 | 94.5% | `7b38d66`, 2026-07-29 |
| `html/semantics/forms` | 3606 / 4302 | 83.8% | `51157ce`, 2026-09-12 |
| `html/semantics/text-level-semantics` | 27 / 38 | 71.1% | `7b38d66`, 2026-07-29 |
| `html/semantics/scripting-1` | 1304 / 1980 | 65.9% | `7b38d66`, 2026-07-29 |
| `html/semantics/document-metadata` | 68 / 171 | 39.8% | `7b38d66`, 2026-07-29 |
| `css/css-images` | 3107 / 3210 | 96.8% | `7afa634`, 2026-09-04 |
| `css/css-content` | 201 / 211 | 95.3% | `18df2bb`, 2026-09-04 |
| `css/css-backgrounds` | 1029 / 1093 | 94.1% | `51157ce`, 2026-09-12 |
| `css/css-lists` | 256 / 274 | 93.4% | `7afa634`, 2026-09-04 |
| `css/css-transitions` | 2282 / 2504 | 91.1% | `7afa634`, 2026-09-04 |
| `css/selectors` | 5245 / 5766 | 91.0% | `51157ce`, 2026-09-12 |
| `css/cssom` | 3466 / 3824 | 90.6% | `51157ce`, 2026-09-12 |
| `css/css-display` | 326 / 376 | 86.7% | `43a5d0a`, 2026-09-04 |
| `css/css-cascade` | 806 / 978 | 82.4% | `51157ce`, 2026-09-12 |
| `css/css-animations` | 803 / 976 | 82.3% | `7afa634`, 2026-09-04 |
| `css/css-conditional` | 2187 / 2718 | 80.5% | `7afa634`, 2026-09-04 |
| `css/css-logical` | 873 / 1170 | 74.6% | `7afa634`, 2026-09-04 |
| `css/css-fonts` | 3608 / 4980 | 72.4% | `7afa634`, 2026-09-04 |
| `css/css-nesting` | 84 / 117 | 71.8% | `7afa634`, 2026-09-04 |
| `css/css-align` | 3247 / 4534 | 71.6% | `7afa634`, 2026-09-04 |
| `css/css-box` | 273 / 407 | 67.1% | `43a5d0a`, 2026-09-04 |
| `css/css-variables` | 366 / 550 | 66.5% | `7afa634`, 2026-09-04 |
| `css/css-transforms` | 464 / 705 | 65.8% | `51157ce`, 2026-09-12 |
| `css/css-syntax` | 277 / 429 | 64.6% | `7afa634`, 2026-09-04 |
| `css/css-position` | 297 / 474 | 62.7% | `7afa634`, 2026-09-04 |
| `css/css-values` | 3743 / 6058 | 61.8% | `51157ce`, 2026-09-12 |
| `css/css-ui` | 525 / 898 | 58.5% | `51157ce`, 2026-09-12 |
| `css/css-grid` | 6265 / 11001 | 56.9% | `7afa634`, 2026-09-04 |
| `css/css-properties-values-api` | 590 / 1039 | 56.8% | `41c3030`, 2026-08-09 |
| `css/css-counter-styles` | 67 / 118 | 56.8% | `41c3030`, 2026-08-09 |
| `css/css-flexbox` | 2220 / 3917 | 56.7% | `7afa634`, 2026-09-04 |
| `css/css-text` | 1649 / 3027 | 54.5% | `51157ce`, 2026-09-12 |
| `css/css-sizing` | 1320 / 2444 | 54.0% | `7afa634`, 2026-09-04 |
| `css/css-easing` | 84 / 156 | 53.8% | `7afa634`, 2026-09-04 |
| `css/css-color` | 5219 / 11006 | 47.4% | `7b38d66`, 2026-07-29 |
| `css/cssom-view` | 895 / 2116 | 42.3% | `7afa634`, 2026-09-04 |
| `css/css-tables` | 331 / 787 | 42.1% | `7afa634`, 2026-09-04 |
| `css/css-multicol` | 143 / 344 | 41.6% | `7afa634`, 2026-09-04 |
| `css/css-overflow` | 365 / 972 | 37.6% | `7afa634`, 2026-09-04 |
| `css/css-pseudo` | 250 / 717 | 34.9% | `7afa634`, 2026-09-04 |
| `css/css-contain` | 107 / 352 | 30.4% | `7afa634`, 2026-09-04 |

`css/css-color` was last measured in full before the colour-space work in
1.0.10, when almost every serialisation subtest failed; expect it to have
moved the most. Read `html/semantics/scripting-1` with its denominator
in view: roughly a quarter of its 474 files load modules or iframes over
the network and time out under load, so the total swings by tens of
subtests between runs of the same binary (1944, 1962 and 1980 across
three runs). A change smaller than about thirty subtests there is noise.

## Measurement history

### Baseline, 2026-07-29

Measured at `7b38d66` against a WPT checkout of 2026-07-29, 8 s per-test
timeout — except `css/css-flexbox`, re-measured against a checkout of
2026-08-08 with a 6 s timeout after the CSSOM and flex work in 1.0.7.

These rows were all taken while `offsetLeft` and `offsetTop` returned
document coordinates instead of offsetParent-relative ones, which
`checkLayout` — the harness most of WPT's layout tests are written
against — compares directly, so every area with layout assertions scored
lower than the engine deserved. The `css/css-flexbox` row was re-measured
after that fix: 1437 of 3535 subtests, up from 653 on the same checkout.

| Area | Subtests | Pass rate |
| --- | --- | --- |
| `html/dom` | 60099 / 60879 | 98.7% |
| `dom/nodes` | 11465 / 11680 | 98.2% |
| `dom/events` | 738 / 781 | 94.5% |
| `css/cssom` | 3230 / 3508 | 92.1% |
| `css/css-cascade` | 951 / 1111 | 85.6% |
| `css/selectors` | 4775 / 5749 | 83.1% |
| `html/semantics/text-level-semantics` | 27 / 38 | 71.1% |
| `html/semantics/forms` | 3270 / 4857 | 67.3% |
| `css/css-values` | 3468 / 5879 | 59.0% |
| `css/css-color` | 5219 / 11006 | 47.4% |
| `css/css-backgrounds` | 474 / 1055 | 44.9% |
| `css/css-transforms` | 310 / 705 | 44.0% |
| `html/semantics/document-metadata` | 68 / 171 | 39.8% |
| `html/semantics/scripting-1` | 1304 / 1980 | 65.9% |
| `css/css-flexbox` | 1437 / 3535 | 40.7% |

### Re-measured for the registered-custom-property work

Measured at `41c3030` on 2026-08-09, 6 s per-test timeout, against a
**sparse** WPT checkout of the CSS areas served over a plain static HTTP
server. Its file set is smaller than the one the table above used, so
these denominators are not comparable with those rows — only with each
other, and with the same run against `5916e0b`, the commit before this
work, given in the *before* column.

| Area | Before | After | Pass rate |
| --- | --- | --- | --- |
| `css/cssom` | 3245 / 3484 | 3245 / 3484 | 93.1% |
| `css/selectors` | — | 4078 / 4413 | 92.4% |
| `css/css-cascade` | 744 / 911 | 744 / 911 | 81.7% |
| `css/css-properties-values-api` | 64 / 809 | 590 / 1039 | 56.8% |
| `css/css-counter-styles` | 50 / 118 | 67 / 118 | 56.8% |
| `css/css-values` | 683 / 1947 | 689 / 1960 | 35.2% |
| `css/css-color` | 22 / 108 | 22 / 108 | 20.4% |

`css/css-properties-values-api` is where nearly all of the movement is:
`@property` honours its `syntax` descriptor, `CSS.registerProperty`
exists, and a registered property computes its value. Its denominator
grew because files that used to stop at the first missing API now reach
subtests they never got to. The two areas that read as unchanged were
checked file by file rather than in aggregate — no file in either moved.

At that point `css/css-flexbox` was the weakest layout area, and almost
all of `css/css-color`'s failures were one structural gap — computed
colours that kept no colour space — which 1.0.10 closed.

### Re-measured for the flex, grid and alignment work in 1.0.8

Measured at `43a5d0a` on 2026-09-04, 6 s per-test timeout, against a
sparse upstream WPT checkout of the same day served over a plain static
HTTP server with `fonts/` (Ahem) and `css/support/` present. The
*before* column is the same checkout run at `ddd5e96`, the commit before
this work, but without the font and support directories, so part of the
movement in every row is tests that finally load Ahem or their support
stylesheet; `css/css-flexbox` was re-run at `ddd5e96` with those
directories in place and moved from 1465 to 1997 on the layout changes
alone.

| Area | Before | After | Pass rate |
| --- | --- | --- | --- |
| `css/css-display` | 33 / 45 | 326 / 376 | 86.7% |
| `css/css-box` | 34 / 128 | 273 / 407 | 67.1% |
| `css/css-align` | 1598 / 3881 | 3026 / 4534 | 66.7% |
| `css/css-position` | 97 / 274 | 267 / 474 | 56.3% |
| `css/css-flexbox` | 1465 / 3670 | 2197 / 3917 | 56.1% |
| `css/css-grid` | 852 / 9371 | 5162 / 11001 | 46.9% |
| `css/css-overflow` | 138 / 614 | 299 / 972 | 30.8% |
| `css/css-sizing` | 474 / 2091 | 665 / 2444 | 27.2% |

`css/css-grid` is where most of the movement is: absolutely positioned
boxes placed by grid lines, `auto-fit` track collapsing, rtl columns
and grid-area percentage heights, plus `document.fonts.ready` now
waiting for Ahem so `checkLayout` measures the intended font.
`css/css-sizing` and `css/css-overflow` stayed low because most of
their remaining files were vertical writing modes or the `stretch`
sizing keyword; `stretch` landed in the third pass below.

### Re-measured for the second pass of 1.0.8

Measured at `18df2bb` on 2026-09-04, 6 s per-test timeout, the same
sparse checkout and static server as the table above, over every
`css/` area the engine is measured on. The *before* column is the run
at `41fd6c1` (the merge of the first pass) for the layout areas and at
the start of this pass for the rest; every row below moved, and the
areas not listed (`css/selectors`, `css/css-transitions`, `css/css-transforms`, `css/css-tables`, `css/css-position`, `css/css-multicol`, `css/css-logical`, `css/css-inline`, `css/css-easing`, `css/css-display`, `css/css-counter-styles`, `css/css-contain`, `css/css-break`, `css/css-box`, `css/css-animations`, `css/css-align`, `css/css-cascade`, `css/cssom`) did not.
Across all 5096 files the total went from 35890 to 42482 of 68610
subtests.

| Area | Before | After | Pass rate |
| --- | --- | --- | --- |
| `css/css-images` | 784 / 3210 | 3106 / 3210 | 96.8% |
| `css/css-fonts` | 2188 / 4978 | 3596 / 4978 | 72.2% |
| `css/css-conditional` | 1031 / 2718 | 2179 / 2718 | 80.2% |
| `css/css-grid` | 5162 / 11001 | 6246 / 11001 | 56.8% |
| `css/cssom-view` | 646 / 2116 | 896 / 2116 | 42.3% |
| `css/css-content` | 96 / 211 | 201 / 211 | 95.3% |
| `css/css-syntax` | 172 / 419 | 272 / 429 | 63.4% |
| `css/css-sizing` | 665 / 2444 | 745 / 2444 | 30.5% |
| `css/css-backgrounds` | 496 / 1055 | 556 / 1055 | 52.7% |
| `css/css-values` | 3571 / 6037 | 3599 / 6037 | 59.6% |
| `css/css-ui` | 503 / 898 | 517 / 898 | 57.6% |
| `css/css-flexbox` | 2197 / 3917 | 2206 / 3917 | 56.3% |
| `css/css-nesting` | 14 / 74 | 20 / 117 | 17.1% |
| `css/css-pseudo` | 235 / 717 | 239 / 717 | 33.3% |
| `css/css-variables` | 346 / 520 | 348 / 520 | 66.9% |
| `css/css-text` | 1703 / 3010 | 1705 / 3010 | 56.6% |
| `css/css-lists` | 134 / 274 | 136 / 274 | 49.6% |
| `css/css-overflow` | 299 / 972 | 300 / 972 | 30.9% |

`css/css-images` is the gradient and image-set work, `css/css-fonts`
the font shorthand and font-family canonicalization, and
`css/css-conditional` the container query evaluator. `css/css-grid`
picked up the resolved track sizes, em and calc() tracks, fr rows and
self-alignment; `css/cssom-view` the scrollable-overflow rules. The
`css/css-transitions` and `css/css-animations` rows stayed where they
were: at `18df2bb` getComputedStyle did not reflect running transitions
and `document.getAnimations()` returned nothing, which is the gap most of
their subtests test for. The third pass closed it.

### Re-measured for the third pass of 1.0.8

Measured at `7afa634` (with the {}-block declaration fix that followed
it) on 2026-09-04, 6 s per-test timeout, the same sparse checkout and
static server as the table above, over the same 37 `css/` areas. The
*before* column is the run at `18df2bb` from the second pass. Three
timing-sensitive files that reported no subtests under load
(`css/css-animations/parsing/animation-computed.html`,
`css/css-transitions/parsing/transition-computed.html` and
`css/cssom-view/scroll-behavior-smooth-positions.html`) were re-run
alone and folded in. Every area moved except `css/selectors`,
`css/css-text`, `css/css-inline`, `css/css-break`, `css/css-display`,
`css/css-counter-styles`, `css/css-content`, `css/css-scoping` and
`css/css-box`. Across all 5097 files the total went from 42482 to 46653
of 68725 subtests.

| Area | Before | After | Pass rate |
| --- | --- | --- | --- |
| `css/css-transitions` | 149 / 2491 | 2282 / 2504 | 91.1% |
| `css/css-animations` | 191 / 955 | 803 / 976 | 82.3% |
| `css/css-sizing` | 745 / 2444 | 1320 / 2444 | 54.0% |
| `css/css-align` | 3025 / 4534 | 3247 / 4534 | 71.6% |
| `css/css-values` | 3599 / 6037 | 3739 / 6061 | 61.7% |
| `css/css-lists` | 136 / 274 | 256 / 274 | 93.4% |
| `css/css-overflow` | 300 / 972 | 365 / 972 | 37.6% |
| `css/css-nesting` | 20 / 117 | 84 / 117 | 71.8% |
| `css/css-easing` | 32 / 156 | 84 / 156 | 53.8% |
| `css/css-position` | 267 / 474 | 297 / 474 | 62.7% |
| `css/css-cascade` | 759 / 934 | 789 / 956 | 82.5% |
| `css/cssom` | 3243 / 3541 | 3264 / 3541 | 92.2% |
| `css/css-grid` | 6246 / 11001 | 6265 / 11001 | 56.9% |
| `css/css-variables` | 348 / 520 | 366 / 550 | 66.5% |
| `css/css-flexbox` | 2206 / 3917 | 2220 / 3917 | 56.7% |
| `css/css-fonts` | 3596 / 4978 | 3608 / 4980 | 72.4% |
| `css/css-pseudo` | 239 / 717 | 250 / 717 | 34.9% |
| `css/css-conditional` | 2179 / 2718 | 2187 / 2718 | 80.5% |
| `css/css-logical` | 867 / 1170 | 873 / 1170 | 74.6% |
| `css/css-ui` | 517 / 898 | 522 / 898 | 58.1% |
| `css/css-syntax` | 272 / 429 | 277 / 429 | 64.6% |
| `css/css-tables` | 328 / 784 | 331 / 787 | 42.1% |
| `css/css-multicol` | 140 / 344 | 143 / 344 | 41.6% |
| `css/css-transforms` | 307 / 705 | 308 / 705 | 43.7% |
| `css/css-images` | 3106 / 3210 | 3107 / 3210 | 96.8% |
| `css/css-contain` | 106 / 352 | 107 / 352 | 30.4% |
| `css/css-backgrounds` | 556 / 1055 | 557 / 1055 | 52.8% |
| `css/cssom-view` | 896 / 2116 | 895 / 2116 | 42.3% |

`css/css-transitions` and `css/css-animations` are the rewritten
animation engine: getComputedStyle now reflects running transitions and
animations, `getAnimations()` returns CSSTransition, CSSAnimation and
script Animation objects, and the phase-based events fire with spec
elapsed times. `css/css-sizing`, `css/css-align` and `css/css-position`
are the `stretch` sizing keyword, the aspect-ratio numerator/denominator
representation and the inset-abspos self-alignment; `css/css-values` is
attr() substitution; `css/css-lists` the counter and list-style
serialisation; `css/css-nesting` the nested-rule CSSOM; and
`css/css-easing` the per-keyframe timing functions. What remains in
`css/css-animations` is mostly scroll-driven timelines and
`animation-composition: add/accumulate`, which parse but do not run.

### Re-measured for the fourth pass of 1.0.8

Measured at `51157ce` on 2026-09-12, 6 s per-test timeout, against a
sparse upstream WPT checkout of the same day (`f7887b5`) served over a
plain static HTTP server with `fonts/` and `css/support/` present. The
*before* column is the same checkout run at `24d9668`, the commit before
this pass. This pass is the background, border, border-image, shadow and
transform grammar work: the `background` shorthand parsed per layer
with all eight longhands, `border-image` implemented, `box-shadow`
canonicalised, `transform` validated function by function, and the
inline-style CSSOM rebuilding `border`, `background`, `border-radius`
and `border-image` from their longhands. The `css/css-cascade` before
column comes from an earlier run of the same checkout; its two
`@font-face` layer files vary between runs of the same binary and the
row should be read as unchanged.

| Area | Before | After | Pass rate |
| --- | --- | --- | --- |
| `css/css-backgrounds` | 574 / 1093 | 1029 / 1093 | 94.1% |
| `css/css-transforms` | 308 / 705 | 464 / 705 | 65.8% |
| `css/cssom` | 3440 / 3824 | 3466 / 3824 | 90.6% |
| `css/css-values` | 3742 / 6058 | 3743 / 6058 | 61.8% |
| `css/css-ui` | 522 / 898 | 525 / 898 | 58.5% |
| `css/css-cascade` | 792 / 964 | 806 / 978 | 82.4% |
| `css/selectors` | 5245 / 5766 | 5245 / 5766 | 91.0% |
| `css/css-text` | 1649 / 3027 | 1649 / 3027 | 54.5% |
| `html/semantics/forms` | 3605 / 4302 | 3606 / 4302 | 83.8% |

What remains in `css/css-backgrounds` is `background-clip: text` and
`border-area` painting, `calc()` with `em` inside `border-image-width`
and the corner radii, and the `background: none` colour serialisation
that Chrome and the specification disagree on. `css/css-transforms` is
now mostly the rendering and interpolation files
(`animation/transform-interpolation-*`, the 3D point-mapping tests),
not parsing.

## Known gaps

### Vertical writing modes

Vertical writing modes are only partly laid out. Flex containers map
`row` and `column` through `writing-mode` and give vertical items a
central baseline, tables transpose their cells, logical properties map
through the writing mode, container queries size vertical containers,
and a single run of vertical text is measured and painted rotated or
upright. Block flow, grid, and line breaking along a vertical inline
axis are still horizontal: a vertical paragraph is one unbroken column,
and a block container stacks its children top to bottom whatever its
writing mode. This remains the largest source of failures in the layout
areas — the `*-vertWM-*`, `*-wmvert-*` and `orthogonal-*` files in
flexbox and grid, and half of `css/css-sizing/stretch` — and closing it
means threading a logical-to-physical mapping through block and inline
layout, not patching any one algorithm.

### Flex layout

`css/css-flexbox` stood at 2220 of 3917 subtests (56.7%) after the third
pass of 1.0.8, up from 653 on the checkout first measured. The main-size
step is the spec's resolve-flexible-lengths loop shared by row,
wrapping-row and column containers; column containers wrap and honour
`align-content`; negative free space overflows in the right direction;
the automatic minimum size is `min(content, specified)`; an absolutely
positioned child takes its static position from `justify-content` and
its own `align-self`; and since 1.0.10 the flex axes follow
`writing-mode` and replaced items stretch across a column container
keeping their aspect ratio. What remains, in the order it costs
subtests:

- **Vertical writing modes** beyond the flex axes, as above.
- **Baseline alignment across nested containers.** An item's baseline is
  its first in-flow descendant's, in document order, rather than one
  synthesized by the flex, grid and multicol rules, and in-flow items do
  not align on `last baseline`; the `alignment/flex-align-baseline-*`
  files fail as a group.
- **Intrinsic sizes.** A flex container's contribution to its parent is
  an estimate — the sum of its items' max-content widths plus gaps, or
  its widest item for min-content — not the result of running the flex
  algorithm (`intrinsic-size/*`, `flex-container-min-content-*`).
- **Replaced items in the main axis.** The min and max main-size clamp
  reads only the width properties, so an image's `height`, `min-height`
  or `max-height` does not transfer through its aspect ratio
  (`image-as-flexitem-size-*`).

### Colour interpolation

Colours keep the space they are authored in through the cascade and
serialise in it — `lab(20 0 10 / 0.5)` reads back as itself — and a
`none` component or `currentcolor` inside `color-mix()` and relative
colours is resolved per element. Transitions and animations still
interpolate colours as 8-bit sRGB rather than in Oklab, so an animated
colour takes a different path between its endpoints than the
specification describes.

### Typed arithmetic in `calc()`

`calc()` carries a value kind, and `asin()`, `acos()`, `atan()` and
`atan2()` produce angles, so `rotate(atan(1))` and
`calc(atan(1) + 10deg)` work. Division by a dimension does not: the
divisor must be a plain number, so `calc(atan(1) / 45deg)` and
`calc(100vw / 1px)` are rejected.

### Experimental CSS Values features

`if()`, `random()` and `calc-mix()` are unimplemented. These are
`.tentative.html` tests for features still being specified and are
deliberately not a priority. Typed `attr()` — `type()`, `raw-string` and
unit types — is implemented.

### Animation composition and scroll-driven animations

`animation-composition: add` and `accumulate` are parsed and reported on
keyframes but not composited, and scroll-driven timelines
(`animation-timeline`, `ScrollTimeline`, `ViewTimeline`) exist only as
property names. Most of what remains failing in `css/css-animations` is
these two features.

### `html/semantics/document-metadata`

Last measured at 39.8% on 2026-07-29, when the score was dominated by
files that timed out rather than failed assertions — `<link>` loading,
`rel=preload`, and `referrerpolicy` behaviours that need resource-timing
signals the headless driver does not settle on. 1.0.9 and 1.0.10 changed
`relList.supports`, `<style type>`, `<link disabled>`,
`document.title` creation, `<base href>` resolution and a `load` event
that waits for images, none of which has been re-measured.

## Recent fixes

Engine changes are recorded in [`../Changelog.md`](../Changelog.md), which
is the single list. This page tracks where the engine stands and what is
structurally missing; it is not a second changelog.

## Keeping this current

Re-run the areas an engine change moves, record the run as a dated
section under *Measurement history*, and update the matching rows of the
latest-reading table in the same commit. When a fix lands, name the tests it was
verified against in the commit message — the numbers above are only
useful if they can be reproduced from the commit that claims them.
