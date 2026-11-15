#!/usr/bin/env python3
# check-pdfjs.py: find and vet an installed PDF.js for -Dpdfjs_dir; prints "name<TAB>path" lines and the version for meson.
"""Validate a PDF.js installation for embedding in the PDF viewer.

Usage: check-pdfjs.py DIR

Accepts the pdfjs-dist npm layout (build/pdf.min.mjs, wasm/,
standard_fonts/) and the generic viewer layout distributions install
(build/pdf.mjs, web/wasm/, web/standard_fonts/), minified or not. On
success prints one "name<TAB>path" line per script and decoder, one
"font<TAB>path" line per standard font, and "version<TAB>X.Y.Z", and
exits 0; otherwise prints the reason to stderr and exits 1.
"""
import os
import re
import sys

# The viewer (data/js/pdf-viewer.js) is written against this API range.
MIN_VERSION = (6, 5, 0)
MAX_MAJOR = 6

CANDIDATES = {
    "pdfjs_lib": ["build/pdf.min.mjs", "build/pdf.mjs"],
    "pdfjs_worker": ["build/pdf.worker.min.mjs", "build/pdf.worker.mjs"],
    "pdfjs_openjpeg": ["wasm/openjpeg.wasm", "web/wasm/openjpeg.wasm"],
    "pdfjs_jbig2": ["wasm/jbig2.wasm", "web/wasm/jbig2.wasm"],
    "pdfjs_qcms": ["wasm/qcms_bg.wasm", "web/wasm/qcms_bg.wasm"],
}

# Glyph data for PDF's standard fonts when a document does not embed them.
# Only the BSD-licensed Foxit fonts (PDFium) are embedded; PDF.js's
# Liberation Sans, under GPL-2.0 with a font exception, is left out, and
# Helvetica/Arial then draw with a system sans-serif font.
STANDARD_FONT_DIRS = ["standard_fonts", "web/standard_fonts"]
STANDARD_FONTS = [
    "FoxitDingbats.pfb", "FoxitFixed.pfb", "FoxitFixedBold.pfb",
    "FoxitFixedBoldItalic.pfb", "FoxitFixedItalic.pfb", "FoxitSerif.pfb",
    "FoxitSerifBold.pfb", "FoxitSerifBoldItalic.pfb", "FoxitSerifItalic.pfb",
    "FoxitSymbol.pfb",
]

VERSION_RE = re.compile(r"pdfjsVersion\s*=\s*\"?(\d+)\.(\d+)\.(\d+)")


def fail(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(1)


def script_version(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "</script" in text.lower():
        fail("%s contains \"</script\" and cannot be inlined in a script element"
             % path)
    m = VERSION_RE.search(text)
    if not m:
        fail("%s does not state a pdfjsVersion" % path)
    return tuple(int(g) for g in m.groups())


def main(argv):
    if len(argv) != 2:
        fail("usage: check-pdfjs.py DIR")
    root = argv[1]
    if not os.path.isdir(root):
        fail("%s is not a directory" % root)
    found = {}
    for name, rels in CANDIDATES.items():
        for rel in rels:
            path = os.path.join(root, *rel.split("/"))
            if os.path.isfile(path):
                found[name] = os.path.abspath(path)
                break
        else:
            fail("%s has none of %s" % (root, ", ".join(rels)))
    font_dir = next((os.path.join(root, *d.split("/"))
                     for d in STANDARD_FONT_DIRS
                     if os.path.isdir(os.path.join(root, *d.split("/")))), None)
    if not font_dir:
        fail("%s has none of %s" % (root, ", ".join(STANDARD_FONT_DIRS)))
    fonts = []
    for name in STANDARD_FONTS:
        path = os.path.join(font_dir, name)
        if not os.path.isfile(path):
            fail("%s lacks the standard font %s" % (font_dir, name))
        fonts.append(os.path.abspath(path))
    lib = script_version(found["pdfjs_lib"])
    worker = script_version(found["pdfjs_worker"])
    if lib != worker:
        fail("PDF.js library %s and worker %s differ in version"
             % (".".join(map(str, lib)), ".".join(map(str, worker))))
    if lib < MIN_VERSION or lib[0] > MAX_MAJOR:
        fail("PDF.js %s in %s is outside the supported range %s to %d.x"
             % (".".join(map(str, lib)), root,
                ".".join(map(str, MIN_VERSION)), MAX_MAJOR))
    for name, path in found.items():
        print("%s\t%s" % (name, path))
    for path in fonts:
        print("font\t%s" % path)
    print("version\t%s" % ".".join(map(str, lib)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
