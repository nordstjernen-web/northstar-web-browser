# Third-party software notices

Northstar Web Browser links to (and in some cases statically includes) the
following open-source libraries. Their copyright notices and license
texts are reproduced below. For libraries shipped dynamically in the
release bundles, you are entitled by the LGPL terms to replace them
with modified versions; the binary will continue to function with any
ABI-compatible replacement.

The Northstar Web Browser source code itself is licensed under the GNU General
Public License, version 3 or later; see `LICENSE`.

---

## Statically linked

### lexbor — Apache License 2.0

> HTML / CSS / WHATWG URL parser.
> <https://github.com/lexbor/lexbor>
>
> Copyright (c) 2018-2025 Alexander Borisov

Licensed under the Apache License, Version 2.0 (the "License"); you may
not use this software except in compliance with the License. You may
obtain a copy of the License at:

  <http://www.apache.org/licenses/LICENSE-2.0>

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied. See the License for the specific language governing permissions
and limitations under the License.

### web-text-lib — GNU LGPL 2.1 or later

> Text itemization, shaping and line breaking. Nordstjernen's fork of
> Pango, carrying a process-wide glyph-string cache and a per-context
> metrics cache, trimmed to what the browser calls, with every symbol
> renamed to `ns_pango_*` / `NsPango*` so it can coexist with the system
> Pango that GTK loads into the same process. Vendored in
> `src/web-text-lib/` (shared with Nordstjernen Sol) and linked
> statically.
> <https://github.com/nordstjernen-web/nordstjernen-sol-browser/tree/main/src/web-text-lib>,
> <https://gitlab.gnome.org/GNOME/pango>
>
> Copyright the GNU Project, the Pango contributors, and the
> Nordstjernen contributors. See `src/web-text-lib/COPYING`.

Licensed under the GNU Lesser General Public License version 2.1, or (at
your option) any later version. The full license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Because web-text-lib is linked statically rather than dynamically, LGPL
section 6 is satisfied through source availability: the library's
complete source ships in `src/web-text-lib/` alongside Nordstjernen Web
Browser's own GPL-3.0-or-later source, so a recipient can modify the
library and rebuild the binary against it.

### Wuffs — Apache License 2.0

> Memory-safe PNG / APNG / GIF / BMP / JPEG / WebP decoders, transpiled
> from the Wuffs language to C. Vendored as the single-file release in
> `subprojects/wuffs/wuffs-v0.4.c`.
> <https://github.com/google/wuffs-mirror-release-c>
>
> Copyright (c) 2017 The Wuffs Authors.

Licensed under the Apache License, Version 2.0. See the lexbor section
above for the license text (same license).

### quickjs-ng — MIT License

> JavaScript engine.
> <https://github.com/quickjs-ng/quickjs>
>
> Copyright (c) 2017-2026 Fabrice Bellard
> Copyright (c) 2017-2026 Charlie Gordon
> Copyright (c) 2023-2026 the quickjs-ng contributors

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

### QuickJS — MIT License (optional, `-Djs_engine=quickjs`)

> The original JavaScript engine, linked instead of quickjs-ng when the
> build selects it.
> <https://github.com/bellard/quickjs>
>
> Copyright (c) 2017-2021 Fabrice Bellard
> Copyright (c) 2017-2021 Charlie Gordon

Licensed under the MIT License. See the quickjs-ng section above for the
license text (same license).

### WebAssembly Micro Runtime (WAMR) — Apache License 2.0 with LLVM exceptions

> WebAssembly runtime. Vendored in `src/wamr/`.
> <https://github.com/bytecodealliance/wasm-micro-runtime>
>
> Copyright (c) The WebAssembly Micro Runtime contributors.

Licensed under the Apache License, Version 2.0, with LLVM exceptions.
See the lexbor section above for the base Apache 2.0 text; the full
license including the LLVM exceptions is reproduced in
`src/wamr/LICENSE`.

### minimp3 — CC0 1.0 (public domain)

> MP3 decoder for in-process audio playback. Vendored as the
> single header `src/audio/minimp3.h`.
> <https://github.com/lieff/minimp3>
>
> Copyright the minimp3 authors.

To the extent possible under law, the author(s) have dedicated all
copyright and related and neighboring rights to this software to the
public domain worldwide. See:

  <https://creativecommons.org/publicdomain/zero/1.0/>

### pl_mpeg — MIT License

> MPEG-1 Part 2 video decoder for `<video>` and MPEG-1 Audio Layer II
> (MP2) decoder for in-process audio playback.
> Vendored as the `plmpeg` meson subproject
> (`subprojects/plmpeg/pl_mpeg.h`), with a local change.
> <https://github.com/phoboslab/pl_mpeg>
>
> Copyright (c) 2019 Dominic Szablewski

Licensed under the MIT License. See the quickjs-ng section above for the
full text (same license).

### libcurl — curl license (MIT-like)

> HTTP/TLS client.
> <https://curl.se>
>
> Copyright (c) 1996-2026 Daniel Stenberg, and many contributors.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE
FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

### OpenSSL (libcrypto) — Apache License 2.0

> Web Cryptography (`crypto.subtle`) primitives and the TLS backend used
> transitively by libcurl.
> <https://www.openssl.org>
>
> Copyright (c) 1998-2026 The OpenSSL Project Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0. See the lexbor section
above for the license text (same license).

### libpsl — MIT License

> Public Suffix List handling for cookie / origin policy.
> <https://github.com/rockdaboot/libpsl>
>
> Copyright (c) 2014-2024 Tim Rühsen

Licensed under the MIT License. See the quickjs-ng section above for the
full text (same license).

### SQLite — public domain

> Embedded SQL database used for history, bookmarks, and caches.
> <https://www.sqlite.org>

SQLite is in the public domain. The authors disclaim copyright to the
source code; it may be used for any purpose, commercial or
non-commercial, without restriction.

### zlib — zlib License

> DEFLATE compression behind `CompressionStream` and `DecompressionStream`,
> and a dependency of libcurl, Cairo, FreeType and GLib.
> <https://zlib.net>
>
> Copyright (c) 1995-2024 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software. Permission is granted to anyone to
use this software for any purpose, including commercial applications, and
to alter it and redistribute it freely, subject to the restrictions in
the zlib license: the origin of this software must not be misrepresented;
altered source versions must be plainly marked as such; and this notice
may not be removed from any source distribution.

### libseccomp — GNU LGPL 2.1 (Linux only)

> Syscall-filter sandbox for the browser and headless/tooling processes.
> Linked only on Linux.
> <https://github.com/seccomp/libseccomp>
>
> Copyright (c) Paul Moore and the libseccomp contributors.

Licensed under the GNU Lesser General Public License version 2.1. See the
LGPL section below for terms and obligations.

### libuchardet — MPL-1.1 / LGPL-2.1+ / GPL-2.0+ (tri-license)

> Charset detection.
> <https://www.freedesktop.org/wiki/Software/uchardet/>
>
> Based on Mozilla's universalchardet, originally Copyright (c)
> 1998-2006 Netscape Communications Corporation and others.

Distributed under the terms of the Mozilla Public License 1.1, the
GNU Lesser General Public License 2.1, or the GNU General Public
License 2.0, at your option. The full license texts are available at:

- <https://www.mozilla.org/MPL/1.1/>
- <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

### GTK 4, GLib, Pango, gdk-pixbuf — GNU LGPL 2.1 or later

> UI toolkit and core utilities.
> <https://www.gtk.org>, <https://gitlab.gnome.org/GNOME/glib>,
> <https://gitlab.gnome.org/GNOME/pango>,
> <https://gitlab.gnome.org/GNOME/gdk-pixbuf>
>
> Copyright the GNU Project and contributors.

These libraries are licensed under the GNU Lesser General Public
License version 2.1, or (at your option) any later version. The full
license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Per LGPL section 6, since Northstar Web Browser links to these libraries
dynamically, you are entitled to modify them and re-link Northstar Web Browser
against the modified copies. On the Windows bundle the libraries are
shipped alongside the executable as ordinary DLLs that you can replace;
on Linux distributions they are loaded from the system package manager.

### SDL2 — zlib License

> Cross-platform audio output (WASAPI / CoreAudio / ALSA) for in-process
> audio playback.
> <https://www.libsdl.org>
>
> Copyright (c) 1997-2024 Sam Lantinga and the SDL contributors.

Licensed under the zlib License. See the zlib section above for the
full text (same license). Linked into the browser when SDL2 is present
at build time (meson `audio` feature).

### Cairo — LGPL-2.1 or MPL-1.1

> 2D drawing.
> <https://www.cairographics.org>
>
> Copyright Carl Worth, Behdad Esfahbod, and the Cairo contributors.

Dual-licensed under the GNU Lesser General Public License 2.1 or the
Mozilla Public License 1.1. See:

- <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- <https://www.mozilla.org/MPL/1.1/>

### libavif — BSD 2-Clause

> AVIF image decoding (AV1 still images). Optional and auto-detected
> (meson `avif` feature); `-Davif=disabled` drops it.
> <https://github.com/AOMediaCodec/libavif>
>
> Copyright the AOMedia / libavif authors.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the BSD 2-Clause conditions
are met. See:

  <https://github.com/AOMediaCodec/libavif/blob/main/LICENSE>

### Optional dynamic dependencies

These are linked only when present on the build host, or reached
transitively through a library Northstar Web Browser does link. When a build bundles
them, their notices apply:

- **libepoxy** — MIT License, © 2013-2014 Intel Corporation. OpenGL
  function-pointer management, pulled in transitively by GTK's GL
  renderer; Northstar Web Browser links nothing against it directly.
- **Fontconfig** — MIT-style license, © Keith Packard and contributors.
- **FreeType** — FreeType License (BSD-style with credit clause) or GNU
  GPL 2.0, at your option, © The FreeType Project.
- **HarfBuzz** — MIT ("Old MIT") License, © the HarfBuzz contributors.
  Text shaping, called by web-text-lib.
- **GNU FriBidi** — GNU LGPL 2.1 or later, © the FriBidi contributors.
  Bidirectional text, called by web-text-lib.
- **libthai** — GNU LGPL 2.1 or later, © the libthai contributors.
  Optional Thai word breaking in web-text-lib, linked when present.
- **Enchant** — GNU LGPL 2.1 or later, © the AbiSource / Enchant
  contributors. Optional on-screen spell-checking.
- **opusfile / libopus / vorbisfile / libvorbis / libogg** — BSD 3-Clause,
  © the Xiph.Org Foundation. Optional Ogg Opus / Vorbis decode in the
  in-process audio mixer.
- **OpenH264** — BSD 2-Clause, © Cisco Systems and the OpenH264
  contributors. Optional H.264 video decode (meson `h264` feature). The
  H.264 patent license Cisco pays for covers only the binaries Cisco
  builds and distributes, not builds from source.

---

## NOTICE files

Apache 2.0 section 4(d) requires propagating any `NOTICE` files
shipped with the upstream sources. As of this release:

- lexbor ships no `NOTICE` file.
- Wuffs ships no `NOTICE` file.
- WAMR ships no `NOTICE` file separate from its `LICENSE`, which is
  reproduced at `src/wamr/LICENSE`.

If a future upstream release adds one, it will be included verbatim
in this section.

## Modifications to upstream sources

Where Northstar Web Browser changes a third-party source, the change is carried as a
patch rather than as an edited copy, so what was modified stays legible
and separable:

- `subprojects/packagefiles/lexbor-named-charref-bound.patch` — grows
  the HTML tokenizer's temporary buffer before a named character
  reference is written into it (`html/tokenizer/state.c`).
- `subprojects/packagefiles/quickjs-ng-windows-link.patch` — build-only:
  on Windows, drops the threads dependency and builds the `qjs` and
  `qjsc` tools without PIE.
- `subprojects/packagefiles/quickjs-ng-module-resolve-failed.patch` —
  keeps a module whose dependencies failed to resolve, and rejects every
  later import of it, instead of freeing it while an import cycle still
  points at it.

- `subprojects/packagefiles/quickjs-sort-calls-comparator.patch` — for the
  optional original QuickJS: `Array.prototype.sort` calls the comparator
  for identical values too, as other engines do.

Meson applies them to the fetched sources at `meson setup`; the wrap
files name them under `diff_files`. The original QuickJS ships no meson
build, so `subprojects/packagefiles/quickjs/meson.build` is also laid over
its checkout (`patch_directory` in `subprojects/quickjs.wrap`). A system lexbor or quickjs-ng, which
the build prefers when it finds one, does not carry them. pl_mpeg carries its local change in
the vendored copy directly, as does web-text-lib, whose changes from
upstream Pango are described in `src/web-text-lib/README.md`. Northstar Web Browser's own source is
GPL-3.0-or-later.
