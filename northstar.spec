#
# spec file for package northstar
#
# Copyright 2026 Andreas Røsdal
#
# Northstar Web Browser is free software under the GNU General Public License,
# version 3 or later (GPL-3.0-or-later).
#


Name:           northstar
Version:        1.0.17
Release:        0
Summary:        Small GTK web browser written from scratch in C
License:        GPL-3.0-or-later
Group:          Productivity/Networking/Web/Browsers
URL:            https://nordstjernen.org

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  update-desktop-files
BuildRequires:  pkgconfig(cairo) >= 1.18
BuildRequires:  pkgconfig(enchant-2)
BuildRequires:  pkgconfig(fontconfig) >= 2.15
BuildRequires:  pkgconfig(fribidi) >= 1.0.6
BuildRequires:  pkgconfig(glib-2.0) >= 2.80
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(harfbuzz) >= 8.3
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(libcurl) >= 8.5
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(libavif)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(uchardet)
Requires:       hicolor-icon-theme
Requires(post): update-alternatives
Requires(postun): update-alternatives
Recommends:     myspell-en_US

%description
Northstar Web Browser is a clean-room web browser written from scratch in C, with a
GTK 4 user interface and a libcurl network stack. It is built to be small,
secure, and readable by a single person end to end.

  * A from-scratch HTML5, CSS, and JavaScript engine — no forked browser engine.
  * One window, one page, one process: the page engine runs in the shell
    process behind a seccomp + Landlock syscall sandbox on Linux.
  * No JIT, which keeps the JavaScript attack surface small.
  * No telemetry: it does not phone home and does not track the user.

%prep
%setup -q -c -T
top=$(find "%{_sourcedir}" -name meson.build 2>/dev/null \
      | awk '{ print length, $0 }' | sort -n | head -1 | cut -d' ' -f2-)
echo "DIAG: top meson.build = ${top:-<none>}"
echo "DIAG: %{_sourcedir} ="; ls -la "%{_sourcedir}"
if [ -z "$top" ]; then
    echo "DIAG: no meson.build under SOURCES; listing %{_builddir}:"
    ls -laR "%{_builddir}" | head -80
    exit 1
fi
cp -a "$(dirname "$top")"/. .
test -f meson.build

%build
%ifarch i386 i486 i586 i686
%global extra_meson -Dwasm=disabled
%endif
%meson \
    %{?extra_meson}
%meson_build

%install
%meson_install
mkdir -p %{buildroot}%{_sysconfdir}/alternatives
ln -sf %{_sysconfdir}/alternatives/open-internet-browser %{buildroot}%{_bindir}/open-internet-browser
touch %{buildroot}%{_sysconfdir}/alternatives/open-internet-browser

%suse_update_desktop_file org.northstar.WebBrowser

%post
%{_sbindir}/update-alternatives --install %{_bindir}/open-internet-browser \
    open-internet-browser %{_bindir}/northstar 50

%postun
if [ ! -f %{_bindir}/northstar ]; then
    %{_sbindir}/update-alternatives --remove open-internet-browser %{_bindir}/northstar
fi

%files
%doc README.md
%license %{_datadir}/northstar/LICENSE
%{_bindir}/northstar
%{_bindir}/open-internet-browser
%ghost %{_sysconfdir}/alternatives/open-internet-browser
%{_datadir}/applications/org.northstar.WebBrowser.desktop
%{_datadir}/metainfo/org.northstar.WebBrowser.metainfo.xml
%{_datadir}/northstar/
%{_datadir}/icons/hicolor/scalable/apps/northstar.gif
%{_datadir}/icons/hicolor/scalable/apps/northstar*.svg

%changelog
