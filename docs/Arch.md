# Northstar on Arch Linux — packaging and the AUR

The Arch package recipe is `packaging/arch/PKGBUILD`, with its generated
`packaging/arch/.SRCINFO`. The package is named `northstar-browser`; the
command it installs is `northstar`. It builds a tagged release tarball with
`arch-meson`, the same way the Debian and openSUSE packages build it. For
plain build-from-source instructions see [building.md](building.md).

## Build from source

```sh
sudo pacman -S --needed base-devel git meson cmake python gtk4 curl uchardet \
    libpsl sqlite libseccomp libavif dav1d libvpx sdl2 enchant opusfile libvorbis
git clone https://github.com/nordstjernen-web/northstar-web-browser.git
cd northstar-web-browser
meson setup builddir && meson compile -C builddir
./builddir/src/gtk/northstar
```

## Build the package

```sh
cd packaging/arch
makepkg -si                 # build and install
extra-x86_64-build          # or: clean-chroot build (devtools), as Arch builds it
namcap PKGBUILD *.pkg.tar.zst
```

The package:

- lists the lexbor and quickjs-ng checkouts the wraps pin as `git+` sources.
  `arch-meson` passes `--wrap-mode=nodownload`, so `prepare()` copies each
  checkout into `subprojects/` and applies the wrap's `diff_files`, as meson
  would when it downloads them. It fails if a wrap's `revision` no longer
  matches the commit in the PKGBUILD.
- requires AV1, VP9, AVIF and the audio output explicitly, so a missing
  dependency fails the build instead of dropping a feature. H.264 stays off:
  only Cisco's prebuilt OpenH264 binaries carry its patent licence.
- installs with `--skip-subprojects`, so the statically linked lexbor and
  quickjs-ng headers and archives are not shipped.
- drops the `open-internet-browser` link, an alternatives name that the
  Debian and openSUSE packages register and that other browsers may claim.
- links `LICENSE` into `/usr/share/licenses/northstar-browser/`.

`namcap` reports nothing for the package; for `northstar-browser-debug` it
flags the build-id symlinks into `/usr/bin`, which is expected for every
debug package.

## Submit it to the AUR

To publish it, use an AUR account with an SSH key:

```sh
git clone ssh://aur@aur.archlinux.org/northstar-browser.git aur-northstar-browser
cp packaging/arch/PKGBUILD packaging/arch/.SRCINFO aur-northstar-browser/
cd aur-northstar-browser
git add PKGBUILD .SRCINFO
git commit -m "Initial upload: northstar-browser 1.0.16"
git push
```

For each release, set `pkgver`, reset `pkgrel=1`, copy the lexbor and
quickjs-ng `revision`s from the wraps into `_lexbor_commit` and
`_quickjs_ng_commit`, run `updpkgsums`, then
`makepkg --printsrcinfo > .SRCINFO`, and commit both files here and in the
AUR repository.

## Tested on

Release 1.0.16 was built with `makepkg -s` in a fresh `archlinux:base-devel`
podman container (GCC 16, GTK 4.24) without compiler warnings.
