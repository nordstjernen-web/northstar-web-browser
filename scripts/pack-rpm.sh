#!/usr/bin/env bash
# Build a portable Northstar Web Browser RPM by repackaging the bundle that
# pack-linux.sh produces. The binary is already statically linked
# against the in-tree engine (lexbor, quickjs, wuffs); the RPM only
# needs to declare the dynamic GTK / curl system deps, and rpm's
# auto-Requires picks those up from the binary's SONAMEs.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")}
ARCH=$(uname -m)
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
RPMVERSION=${VERSION//[!A-Za-z0-9.]/.}
SLUG="northstar-${FSVERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild not found. Install it first:" >&2
    echo "    sudo zypper install rpm-build       # openSUSE" >&2
    echo "    sudo dnf install rpm-build          # Fedora/RHEL" >&2
    echo "    sudo apt install rpm                # Debian/Ubuntu (provides rpmbuild)" >&2
    exit 1
fi

if [ ! -x "$STAGE/northstar" ]; then
    echo "Bundle not staged yet — running pack-linux.sh first."
    "$ROOT/scripts/pack-linux.sh"
fi

RPMTOP="$ROOT/dist/rpmbuild"
rm -rf "$RPMTOP"
mkdir -p "$RPMTOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

cp "$STAGE/northstar" "$RPMTOP/SOURCES/"
# All app + toolbar icons the UI and about: pages look up by name
# (northstar-back, northstar-reload, …) — not just the app icon.
( cd "$ROOT/data/icons/hicolor/scalable/apps" && \
  tar -czf "$RPMTOP/SOURCES/northstar-icons.tar.gz" \
      northstar*.svg northstar.gif )
cp "$ROOT/data/northstar.desktop" "$RPMTOP/SOURCES/"
cp "$ROOT/README.md" "$RPMTOP/SOURCES/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$RPMTOP/SOURCES/"
cp "$ROOT/LICENSE" "$RPMTOP/SOURCES/"

SPEC="$RPMTOP/SPECS/northstar.spec"
cat > "$SPEC" <<SPEC_EOF
Name:           northstar
Version:        ${RPMVERSION}
Release:        1%{?dist}
Summary:        Northstar Web Browser — minimalist GTK 4 browser, GPL edition

License:        GPL-3.0-or-later
URL:            https://nordstjernen.org
BuildArch:      ${ARCH}

Source0:        northstar
Source1:        northstar-icons.tar.gz
Source2:        northstar.desktop
Source3:        README.md
Source4:        THIRD-PARTY-LICENSES.md
Source5:        LICENSE

AutoReqProv:    yes

%define debug_package %{nil}
%global __strip /bin/true
%global __os_install_post %{nil}

%description
Northstar Web Browser is a small free-software web browser written from scratch in
C with GTK 4 and libcurl. The HTML parser, CSS engine, layout, paint
and JavaScript glue contain no third-party browser engine. SVG images
are rendered in-engine. Licensed GPL-3.0-or-later.

%prep
# nothing to prep; binary is prebuilt

%build
# nothing to build; binary is prebuilt

%install
rm -rf %{buildroot}
install -dm755 %{buildroot}%{_bindir}
install -dm755 %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
install -dm755 %{buildroot}%{_datadir}/applications
install -dm755 %{buildroot}%{_docdir}/northstar

install -m755 %{SOURCE0} %{buildroot}%{_bindir}/northstar
tar -xzf %{SOURCE1} -C %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
chmod 644 %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/northstar*
# Install under the app-id name so Wayland compositors can match the
# window (StartupWMClass=org.northstar.WebBrowser) to its desktop file.
install -m644 %{SOURCE2} %{buildroot}%{_datadir}/applications/org.northstar.WebBrowser.desktop
install -m644 %{SOURCE3} %{buildroot}%{_docdir}/northstar/
install -m644 %{SOURCE4} %{buildroot}%{_docdir}/northstar/
install -m644 %{SOURCE5} %{buildroot}%{_docdir}/northstar/

%files
%{_bindir}/northstar
%{_datadir}/icons/hicolor/scalable/apps/northstar*
%{_datadir}/applications/org.northstar.WebBrowser.desktop
%{_docdir}/northstar/

%post
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t %{_datadir}/icons/hicolor || :
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q %{_datadir}/applications || :
fi
if command -v update-alternatives >/dev/null 2>&1; then
    update-alternatives --install %{_bindir}/open-internet-browser open-internet-browser \\
        %{_bindir}/northstar 50 || :
fi

%postun
if [ "\$1" = 0 ] && command -v update-alternatives >/dev/null 2>&1; then
    update-alternatives --remove open-internet-browser %{_bindir}/northstar || :
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t %{_datadir}/icons/hicolor || :
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q %{_datadir}/applications || :
fi

%changelog
* $(date "+%a %b %d %Y") Andreas Røsdal <andreas.rosdal@gmail.com> - ${RPMVERSION}-1
- Prebuilt binary repackaged as an RPM.
SPEC_EOF

rpmbuild --define "_topdir $RPMTOP" -bb "$SPEC"

RPMFILE=$(find "$RPMTOP/RPMS" -name "northstar-${RPMVERSION}-*.rpm" -print -quit)
if [ -z "$RPMFILE" ]; then
    echo "rpmbuild produced no .rpm — see $RPMTOP/BUILD for details." >&2
    exit 1
fi

cp "$RPMFILE" "$ROOT/dist/"
FINAL="$ROOT/dist/$(basename "$RPMFILE")"

echo
echo "Built: $FINAL ($(du -h "$FINAL" | cut -f1))"
echo
echo "Inspect: rpm -qpi $FINAL"
echo "Deps:    rpm -qpR $FINAL"
echo "Install: sudo rpm -i $FINAL    (or: sudo dnf install $FINAL)"
