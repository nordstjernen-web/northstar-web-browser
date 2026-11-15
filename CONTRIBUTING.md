# Contributing to Nordstjernen

Thanks for your interest in Nordstjernen! This branch holds the source of
the project website, [nordstjernen.org](https://nordstjernen.org). The
browsers themselves live in their own repositories:

- **Nordstjernen** —
  [nordstjernen-web/nordstjernen-sol-browser](https://github.com/nordstjernen-web/nordstjernen-sol-browser)
- **Northstar** —
  [nordstjernen-web/northstar-web-browser](https://github.com/nordstjernen-web/northstar-web-browser)
- **Southstar** —
  [nordstjernen-web/southstar-browser](https://github.com/nordstjernen-web/southstar-browser)

## Where to file what

- **Bug in a browser** (crash, wrong rendering, broken site) → open an
  issue in that browser's repository:
  [Nordstjernen](https://github.com/nordstjernen-web/nordstjernen-sol-browser/issues),
  [Northstar](https://github.com/nordstjernen-web/northstar-web-browser/issues) or
  [Southstar](https://github.com/nordstjernen-web/southstar-browser/issues).
- **Question or idea** → start a
  [discussion](https://github.com/nordstjernen-web/nordstjernen-sol-browser/discussions).
- **Pull request against browser code** → open it in that browser's
  repository.
- **Fix to the website** → open a pull request against the `website`
  branch of
  [nordstjernen-web/northstar-web-browser](https://github.com/nordstjernen-web/northstar-web-browser).

## Filing a good bug report

Please include:

1. **Browser and version** — e.g. run `nordstjernen --version`.
2. **Operating system and version** — e.g. "Windows 11 24H2",
   "Ubuntu 26.04", "macOS 15.2".
3. **A reproducible test case** — ideally a URL that shows the
   problem, or a minimal HTML snippet.
4. **What you expected to happen** vs. **what actually happened**.
5. **Steps to reproduce**, if not obvious.
6. **Screenshots** for rendering bugs, if helpful.

For crashes, include the terminal output if you launched the browser
from a shell.

## Security issues

If you believe you've found a security vulnerability, please **do not**
open a public issue. Report it privately as described in the browser's
`SECURITY.md` — for example
[Nordstjernen's](https://github.com/nordstjernen-web/nordstjernen-sol-browser/blob/main/SECURITY.md#reporting) —
so the bug can be triaged before disclosure.

## Code of conduct

Be kind, be precise, and assume good faith. The project is small and
maintained by a single person — patience and clear bug reports go a
long way.
