/* Northstar Web Browser — in-tree ICO/CUR decode (container parse, pixels via Wuffs).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bytes.h"
#include "image.h"

#include <string.h>

enum {
    NS_ICO_MAX_ENTRIES = 256,
    NS_ICO_MAX_DIM     = 1024,
    NS_ICO_MAX_BMP     = 64 * 1024 * 1024,
};

static gboolean
is_png(const guchar *d, gsize len)
{
    static const guchar sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return len >= 8 && memcmp(d, sig, 8) == 0;
}

/* Decodes a DIB icon entry by wrapping it as a BMP for Wuffs, then applies the AND mask. */
static ns_texture *
decode_dib_entry(const guchar *p, gsize len, int *out_w, int *out_h)
{
    if (len < 40) return NULL;
    guint32 hdr = ns_get_le32(p);
    if (hdr < 40 || hdr > len) return NULL;
    gint32 bw = (gint32)ns_get_le32(p + 4);
    gint32 bh = (gint32)ns_get_le32(p + 8);
    guint16 bitcount = ns_get_le16(p + 14);
    guint32 compression = ns_get_le32(p + 16);
    if (bw <= 0 || bh <= 0 || (bh & 1)) return NULL;
    if (bw > NS_ICO_MAX_DIM || bh > 2 * NS_ICO_MAX_DIM) return NULL;
    if (compression != 0) return NULL;
    if (bitcount != 1 && bitcount != 4 && bitcount != 8 &&
        bitcount != 24 && bitcount != 32) return NULL;

    guint32 w = (guint32)bw;
    guint32 real_h = (guint32)bh / 2;

    guint32 clr_used = ns_get_le32(p + 32);
    guint32 pal_count = bitcount <= 8
        ? (clr_used ? clr_used : (1u << bitcount)) : 0;
    if (pal_count > 256) return NULL;
    guint32 pal_bytes = pal_count * 4;

    guint64 xor_row  = (((guint64)w * bitcount + 31) / 32) * 4;
    guint64 xor_size = xor_row * real_h;
    guint64 pixel_off = (guint64)hdr + pal_bytes;
    if (pixel_off + xor_size > len) return NULL;

    guint64 data_off64 = (guint64)14 + hdr + pal_bytes;
    guint64 bmp_len = data_off64 + xor_size;
    if (data_off64 > G_MAXUINT32 || bmp_len > NS_ICO_MAX_BMP) return NULL;
    guint32 data_off = (guint32)data_off64;

    guint8 *bmp = g_try_malloc((gsize)bmp_len);
    if (!bmp) return NULL;
    bmp[0] = 'B'; bmp[1] = 'M';
    ns_put_le32(bmp + 2, (guint32)bmp_len);
    ns_put_le32(bmp + 6, 0);
    ns_put_le32(bmp + 10, data_off);
    memcpy(bmp + 14, p, (gsize)hdr + pal_bytes);
    ns_put_le32(bmp + 14 + 8, real_h);
    ns_put_le32(bmp + 14 + 20, 0);
    memcpy(bmp + data_off, p + pixel_off, (gsize)xor_size);

    int dw = 0, dh = 0;
    gsize stride = 0, buf_len = 0;
    guint8 *pix = ns_image_wuffs_decode_to_bgra(bmp, (gsize)bmp_len,
                                                &dw, &dh, &stride, &buf_len);
    g_free(bmp);
    if (!pix) return NULL;

    guint64 and_row  = (((guint64)w + 31) / 32) * 4;
    guint64 and_off  = pixel_off + xor_size;
    guint64 and_size = and_row * real_h;
    gboolean same_size = dw == (int)w && dh == (int)real_h;

    /* A 32-bpp entry carries per-pixel alpha in its fourth byte, which the
     * BMP decoder treats as unused padding.  When any alpha byte is set,
     * take the pixels straight from the (bottom-up) XOR bitmap with that
     * alpha, premultiplied, and ignore the AND mask, as Windows does; an
     * all-zero alpha channel means an old-style icon that relies on the
     * mask. */
    gboolean has_alpha = FALSE;
    if (bitcount == 32 && same_size) {
        const guchar *xor = p + pixel_off;
        for (guint64 i = 3; i < xor_size && !has_alpha; i += 4)
            has_alpha = xor[i] != 0;
    }
    if (has_alpha) {
        for (int y = 0; y < dh; y++) {
            const guchar *srow =
                p + pixel_off + (guint64)(real_h - 1 - (guint32)y) * xor_row;
            guint8 *prow = pix + (gsize)y * stride;
            for (int x = 0; x < dw; x++) {
                const guchar *s = srow + (gsize)x * 4;
                guint8 *px = prow + (gsize)x * 4;
                guint a = s[3];
                px[0] = (guint8)((s[0] * a + 127) / 255);
                px[1] = (guint8)((s[1] * a + 127) / 255);
                px[2] = (guint8)((s[2] * a + 127) / 255);
                px[3] = (guint8)a;
            }
        }
    } else if (same_size && and_off + and_size <= len) {
        for (int y = 0; y < dh; y++) {
            const guchar *mrow =
                p + and_off + (guint64)(real_h - 1 - (guint32)y) * and_row;
            guint8 *prow = pix + (gsize)y * stride;
            for (int x = 0; x < dw; x++) {
                if ((mrow[x >> 3] >> (7 - (x & 7))) & 1) {
                    guint8 *px = prow + (gsize)x * 4;
                    px[0] = px[1] = px[2] = px[3] = 0;
                }
            }
        }
    }

    GBytes *bytes = g_bytes_new_take(pix, buf_len);
    ns_texture *tex = ns_texture_new(
        dw, dh, NS_TEXTURE_BGRA_PREMULTIPLIED, bytes, stride);
    g_bytes_unref(bytes);
    if (tex) {
        if (out_w) *out_w = dw;
        if (out_h) *out_h = dh;
    }
    return tex;
}

/* Decodes an ICO/CUR file, picking the largest, deepest entry (PNG or DIB). */
ns_texture *
ns_image_decode_ico(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len < 6) return NULL;
    if (ns_get_le16(data) != 0) return NULL;
    guint16 type = ns_get_le16(data + 2);
    if (type != 1 && type != 2) return NULL;
    guint16 count = ns_get_le16(data + 4);
    if (count == 0 || count > NS_ICO_MAX_ENTRIES) return NULL;
    if (6 + (gsize)count * 16 > len) return NULL;

    int best = -1;
    guint64 best_px = 0;
    guint16 best_bits = 0;
    for (guint16 i = 0; i < count; i++) {
        const guchar *e = data + 6 + (gsize)i * 16;
        guint32 ew = e[0] ? e[0] : 256;
        guint32 eh = e[1] ? e[1] : 256;
        guint16 bits = ns_get_le16(e + 6);
        guint32 size = ns_get_le32(e + 8);
        guint32 off = ns_get_le32(e + 12);
        if (size == 0 || (guint64)off + size > len) continue;
        guint64 px = (guint64)ew * eh;
        if (best < 0 || px > best_px || (px == best_px && bits > best_bits)) {
            best = i;
            best_px = px;
            best_bits = bits;
        }
    }
    if (best < 0) return NULL;

    const guchar *e = data + 6 + (gsize)best * 16;
    guint32 size = ns_get_le32(e + 8);
    guint32 off = ns_get_le32(e + 12);
    const guchar *payload = data + off;

    if (is_png(payload, size))
        return ns_image_decode_wuffs(payload, size, out_w, out_h);
    return decode_dib_entry(payload, size, out_w, out_h);
}
