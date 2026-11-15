#!/usr/bin/env python3
# gen-pdf-fixture.py: write data/fixtures/compressed-text.pdf, the PDF viewer smoke-test document.
"""Generate the PDF viewer's smoke-test document.

Usage: gen-pdf-fixture.py OUT.pdf

The page content is FlateDecode-compressed, as in nearly every real PDF,
so text only shows when PDF.js can inflate it in its worker. It holds
over 300 separately drawn words, which PDF.js streams to the page in
several chunks, so the last marker only shows when the stream delivers
every chunk. The smoke test looks for NSPDF-START and NSPDF-END.
"""
import sys
import zlib


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: gen-pdf-fixture.py OUT.pdf\n")
        return 2
    ops = ["BT /F1 14 Tf 40 800 Td (NSPDF-START) Tj ET"]
    y = 770
    for row in range(30):
        x = 40
        for col in range(11):
            ops.append("BT /F1 9 Tf %d %d Td (w%d.%d) Tj ET" % (x, y, row, col))
            x += 48
        y -= 22
    ops.append("BT /F1 14 Tf 40 60 Td (NSPDF-END) Tj ET")
    content = zlib.compress("\n".join(ops).encode("ascii"), 9)

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(content)
        + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    pdf = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(pdf))
        pdf += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(pdf)
    pdf += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        pdf += b"%010d 00000 n \n" % off
    pdf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objs) + 1, xref)
    with open(argv[1], "wb") as f:
        f.write(pdf)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
