# npm package: northstar-browser

`npm/` is the [`northstar-browser`](https://www.npmjs.com/package/northstar-browser) package. It
carries no binaries: the `postinstall` script, or the first `northstar` launch
when install scripts are skipped, builds the release tag's source on the
user's machine and installs it with `meson install` into `vendor/<version>/`
inside the package.

## How it works

- `lib/product.js` is the only file specific to Northstar: the release
  repository, the executable, the meson options, the build tools and the
  required and optional libraries with their version floors and package names
  per package manager. Keep the floors in step with meson.build and src/web-text-lib/meson.build. The other files
  in `lib/` are the same in the Northstar and Southstar packages.
- `lib/dependencies.js` checks every tool and library before anything is
  downloaded and names the packages that provide what is missing, for apt and
  pacman from the tables in `product.js`, for dnf, zypper and apk as
  `pkgconfig(...)` / `pc:` provides, and for Homebrew and MSYS2 pacman.
  Libraries too old to build against are reported as a distribution that is too
  old; optional libraries as a warning naming the missing features.
- `lib/system.js` runs the build programs. On Windows it finds MSYS2
  (`MSYS2_ROOT`, `C:\msys64`), prefers the UCRT64 environment, falls back to
  MINGW64 when only that one has GTK 4, and runs that environment's programs
  with its `bin` first on `PATH`; the launcher puts the same directory on
  `PATH` when it starts the browser (`vendor/<version>/npm-runtime.json`).
  Progress goes to `/dev/tty` or `CONOUT$`, because npm hides install-script
  output.
- `lib/prepack.js` refuses a package version that has no release tag,
  records the SHA-256 of the tag's GitHub source tarball in
  `lib/source.json`, and copies LICENSE into the package. The installer
  refuses a tarball that does not match.

## Publishing a release

The npm version must name a published release tag, because the installer
builds that tag's source tarball.

1. Set `"version"` in `npm/package.json` to the release tag.
2. From `npm/`, check the contents with `npm pack --dry-run`.
3. Run `npm publish` (add `--otp=<code>` if two-factor authentication asks
   for it).

A published version cannot be republished. To fix a broken package, publish
the next version, or run `npm deprecate northstar-browser@<version> "<reason>"`.
