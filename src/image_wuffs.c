/* Northstar Web Browser — memory-safe PNG/APNG/GIF/BMP/JPEG/WebP decode via Wuffs.
 * Sniffs by magic bytes, decodes stills and GIF/APNG frames, and applies WebP ALPH alpha itself.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "image.h"

#include <stdlib.h>
#include <string.h>


#include "wuffs-v0.4.c"

enum {
    NS_WUFFS_MAX_DIM    = 16384,
    NS_WUFFS_MAX_PIXELS = 64 * 1024 * 1024,
};

typedef enum {
    NS_WUFFS_NONE = 0,
    NS_WUFFS_PNG,
    NS_WUFFS_GIF,
    NS_WUFFS_BMP,
    NS_WUFFS_JPEG,
    NS_WUFFS_WEBP,
} ns_wuffs_format;

/* Sniffs PNG, GIF, BMP, JPEG or WebP from magic bytes. */
static ns_wuffs_format
ns_wuffs_detect(const guchar *data, gsize len)
{
    if (!data || len < 4) return NS_WUFFS_NONE;
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return NS_WUFFS_PNG;
    if (len >= 6 &&
        data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a')
        return NS_WUFFS_GIF;
    if (data[0] == 'B' && data[1] == 'M')
        return NS_WUFFS_BMP;
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return NS_WUFFS_JPEG;
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0)
        return NS_WUFFS_WEBP;
    return NS_WUFFS_NONE;
}

static guint32
ns_riff_u32le(const guchar *p)
{
    return (guint32)p[0] | ((guint32)p[1] << 8) |
           ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
}

typedef struct ns_webp_chunks {
    const guchar *alpha;
    gsize         alpha_len;
    const guchar *body;
    gsize         body_len;
    char          body_tag[4];
} ns_webp_chunks;

/* TRUE if the WebP uses the extended (VP8X) container. */
static gboolean
ns_webp_is_extended(const guchar *data, gsize len)
{
    return len >= 21 && memcmp(data + 12, "VP8X", 4) == 0;
}

/* Finds the ALPH and VP8/VP8L image chunks in an extended WebP, incl. the first ANMF frame. */
static gboolean
ns_webp_find_chunks(const guchar *data, gsize len, ns_webp_chunks *out)
{
    enum { NS_WEBP_ANMF_HEADER = 16 };
    if (len < 12 + 8) return FALSE;
    gsize riff_len = (gsize)ns_riff_u32le(data + 4) + 8;
    if (riff_len > len) riff_len = len;

    memset(out, 0, sizeof *out);

    gsize pos = 12;
    while (pos + 8 <= riff_len) {
        const guchar *tag = data + pos;
        gsize size = ns_riff_u32le(data + pos + 4);
        gsize payload = pos + 8;
        if (size > riff_len - payload) break;

        if (memcmp(tag, "ALPH", 4) == 0) {
            out->alpha = data + payload;
            out->alpha_len = size;
        } else if (memcmp(tag, "VP8 ", 4) == 0 ||
                   memcmp(tag, "VP8L", 4) == 0) {
            out->body = data + payload;
            out->body_len = size;
            memcpy(out->body_tag, tag, 4);
            return TRUE;
        } else if (memcmp(tag, "ANMF", 4) == 0 && size > NS_WEBP_ANMF_HEADER) {
            gsize sub = payload + NS_WEBP_ANMF_HEADER;
            gsize sub_end = payload + size;
            while (sub + 8 <= sub_end) {
                const guchar *stag = data + sub;
                gsize ssize = ns_riff_u32le(data + sub + 4);
                gsize spayload = sub + 8;
                if (ssize > sub_end - spayload) break;
                if (memcmp(stag, "ALPH", 4) == 0) {
                    out->alpha = data + spayload;
                    out->alpha_len = ssize;
                } else if (memcmp(stag, "VP8 ", 4) == 0 ||
                           memcmp(stag, "VP8L", 4) == 0) {
                    out->body = data + spayload;
                    out->body_len = ssize;
                    memcpy(out->body_tag, stag, 4);
                    return TRUE;
                }
                sub = spayload + ssize + (ssize & 1);
            }
            break;
        }
        pos = payload + size + (size & 1);
    }
    return FALSE;
}

/* Wraps a VP8/VP8L payload in a minimal simple-format RIFF WebP; caller frees. */
static guint8 *
ns_webp_bare_container(const char tag[4], const guchar *payload, gsize payload_len,
                       gsize *out_len)
{
    if (!payload || payload_len == 0 || payload_len > G_MAXUINT32 - 32)
        return NULL;
    gsize padded = payload_len + (payload_len & 1);
    gsize total = 12 + 8 + padded;
    guint8 *out = g_try_malloc0(total);
    if (!out) return NULL;
    memcpy(out, "RIFF", 4);
    guint32 riff_size = (guint32)(total - 8);
    out[4] = (guint8)(riff_size & 0xff);
    out[5] = (guint8)((riff_size >> 8) & 0xff);
    out[6] = (guint8)((riff_size >> 16) & 0xff);
    out[7] = (guint8)((riff_size >> 24) & 0xff);
    memcpy(out + 8, "WEBP", 4);
    memcpy(out + 12, tag, 4);
    out[16] = (guint8)(payload_len & 0xff);
    out[17] = (guint8)((payload_len >> 8) & 0xff);
    out[18] = (guint8)((payload_len >> 16) & 0xff);
    out[19] = (guint8)((payload_len >> 24) & 0xff);
    memcpy(out + 20, payload, payload_len);
    *out_len = total;
    return out;
}

/* TRUE if a PNG has an acTL chunk before its first IDAT (APNG). */
gboolean
ns_image_png_is_animated(const guchar *data, gsize len)
{
    static const guchar sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (!data || len < 8 + 8 || memcmp(data, sig, sizeof sig) != 0) return FALSE;

    gsize pos = 8;
    while (pos + 8 <= len) {
        guint32 size = ((guint32)data[pos] << 24) | ((guint32)data[pos + 1] << 16) |
                       ((guint32)data[pos + 2] << 8) | (guint32)data[pos + 3];
        const guchar *tag = data + pos + 4;
        if (memcmp(tag, "IDAT", 4) == 0) return FALSE;
        if (memcmp(tag, "acTL", 4) == 0) return TRUE;
        if (memcmp(tag, "IEND", 4) == 0) return FALSE;
        if (size > len - pos - 8) return FALSE;
        pos += 12 + (gsize)size;
    }
    return FALSE;
}

/* Allocates the Wuffs image decoder matching the sniffed format; caller frees. */
static wuffs_base__image_decoder *
ns_wuffs_pick_decoder(const guchar *data, gsize len)
{
    switch (ns_wuffs_detect(data, len)) {
    case NS_WUFFS_PNG:  return wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_GIF:  return wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_BMP:  return wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_JPEG: return wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_WEBP: return wuffs_webp__decoder__alloc_as__wuffs_base__image_decoder();
    default:            return NULL;
    }
}

gboolean
ns_image_wuffs_supports_bytes(const guchar *data, gsize len)
{
    return ns_wuffs_detect(data, len) != NS_WUFFS_NONE;
}

typedef struct ns_wuffs_canvas {
    wuffs_base__image_decoder *dec;
    wuffs_base__io_buffer      src;
    wuffs_base__pixel_buffer   pb;
    uint8_t                   *pix;
    uint64_t                   pix_len;
    uint8_t                   *workbuf;
    uint64_t                   workbuf_len;
    uint32_t                   w, h;
} ns_wuffs_canvas;

static void
ns_wuffs_canvas_close(ns_wuffs_canvas *c)
{
    g_free(c->workbuf);
    g_free(c->pix);
    free(c->dec);
    memset(c, 0, sizeof *c);
}

/* Sets up a decoder, size-checked pixel buffer and work buffer for an image. */
static gboolean
ns_wuffs_canvas_open(ns_wuffs_canvas *c, const guchar *data, gsize len,
                     uint32_t pixel_format)
{
    memset(c, 0, sizeof *c);
    c->dec = ns_wuffs_pick_decoder(data, len);
    if (!c->dec) return FALSE;

    c->src = wuffs_base__make_io_buffer(
        wuffs_base__make_slice_u8((uint8_t *)data, len),
        wuffs_base__make_io_buffer_meta(len, 0, 0, true));

    wuffs_base__image_config ic = {0};
    wuffs_base__status st =
        wuffs_base__image_decoder__decode_image_config(c->dec, &ic, &c->src);
    if (!wuffs_base__status__is_ok(&st) ||
        !wuffs_base__image_config__is_valid(&ic))
        goto fail;

    c->w = wuffs_base__pixel_config__width(&ic.pixcfg);
    c->h = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (c->w == 0 || c->h == 0 ||
        c->w > NS_WUFFS_MAX_DIM || c->h > NS_WUFFS_MAX_DIM ||
        (uint64_t)c->w * (uint64_t)c->h > (uint64_t)NS_WUFFS_MAX_PIXELS)
        goto fail;

    wuffs_base__pixel_config__set(&ic.pixcfg, pixel_format,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, c->w, c->h);

    c->pix_len = wuffs_base__pixel_config__pixbuf_len(&ic.pixcfg);
    if (c->pix_len == 0 || c->pix_len > (uint64_t)NS_WUFFS_MAX_PIXELS * 4u)
        goto fail;

    c->pix = g_try_malloc0((gsize)c->pix_len);
    if (!c->pix) goto fail;

    st = wuffs_base__pixel_buffer__set_from_slice(
        &c->pb, &ic.pixcfg,
        wuffs_base__make_slice_u8(c->pix, (size_t)c->pix_len));
    if (!wuffs_base__status__is_ok(&st)) goto fail;

    c->workbuf_len = wuffs_base__image_decoder__workbuf_len(c->dec).max_incl;
    if (c->workbuf_len) {
        if (c->workbuf_len > 64u * 1024u * 1024u) goto fail;
        c->workbuf = g_try_malloc((gsize)c->workbuf_len);
        if (!c->workbuf) goto fail;
    }
    return TRUE;

fail:
    ns_wuffs_canvas_close(c);
    return FALSE;
}

/* Decodes the first frame to BGRA (premultiplied or not); caller frees the pixels. */
static guint8 *
ns_wuffs_decode_still_to_bgra(const guchar *data, gsize len, gboolean premultiply,
                              int *out_w, int *out_h,
                              gsize *out_stride, gsize *out_buf_len)
{
    ns_wuffs_format kind = ns_wuffs_detect(data, len);
    uint32_t format = premultiply ? WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL
                                  : WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL;
    ns_wuffs_canvas c;
    if (!ns_wuffs_canvas_open(&c, data, len, format)) return NULL;

    wuffs_base__status st = wuffs_base__image_decoder__decode_frame(
        c.dec, &c.pb, &c.src, WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8(c.workbuf, (size_t)c.workbuf_len),
        NULL);

    gboolean frame_ok = wuffs_base__status__is_ok(&st) ||
                        st.repr == wuffs_base__error__too_much_data;
    wuffs_base__table_u8 tab = wuffs_base__pixel_buffer__plane(&c.pb, 0);
    if ((!frame_ok && kind != NS_WUFFS_GIF) ||
        tab.ptr == NULL || tab.stride == 0 ||
        (uint64_t)tab.stride < (uint64_t)c.w * 4 ||
        (uint64_t)tab.stride * (uint64_t)c.h > c.pix_len) {
        ns_wuffs_canvas_close(&c);
        return NULL;
    }

    if (out_w) *out_w = (int)c.w;
    if (out_h) *out_h = (int)c.h;
    if (out_stride) *out_stride = (gsize)tab.stride;
    if (out_buf_len) *out_buf_len = (gsize)c.pix_len;
    guint8 *pix = g_steal_pointer(&c.pix);
    ns_wuffs_canvas_close(&c);
    return pix;
}

/* Reverses the WebP ALPH horizontal, vertical or gradient prediction filter in place. */
static void
ns_webp_alpha_unfilter(guint8 *plane, int w, int h, int method)
{
    if (method == 0) return;
    for (int y = 0; y < h; y++) {
        guint8 *row = plane + (gsize)y * (gsize)w;
        const guint8 *prev = y ? row - w : NULL;
        if (!prev || method == 1) {
            int pred = prev ? prev[0] : 0;
            for (int x = 0; x < w; x++) {
                row[x] = (guint8)(row[x] + pred);
                pred = row[x];
            }
        } else if (method == 2) {
            for (int x = 0; x < w; x++) row[x] = (guint8)(row[x] + prev[x]);
        } else {
            int left = prev[0], top_left = prev[0];
            for (int x = 0; x < w; x++) {
                int top = prev[x];
                int g = left + top - top_left;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                left = (guint8)(row[x] + g);
                top_left = top;
                row[x] = (guint8)left;
            }
        }
    }
}

/* Decodes a WebP ALPH chunk (raw or VP8L-compressed) into a w*h alpha plane. */
static guint8 *
ns_webp_alpha_plane(const guchar *alph, gsize alph_len, int w, int h)
{
    if (!alph || alph_len < 2 || w <= 0 || h <= 0) return NULL;
    int method     = alph[0] & 0x03;
    int filtering  = (alph[0] >> 2) & 0x03;
    gsize pixels   = (gsize)w * (gsize)h;

    guint8 *plane = NULL;
    if (method == 0) {
        if (alph_len - 1 < pixels) return NULL;
        plane = g_try_malloc(pixels);
        if (!plane) return NULL;
        memcpy(plane, alph + 1, pixels);
    } else if (method == 1) {
        gsize vp8l_len = 5 + (alph_len - 1);
        guint8 *vp8l = g_try_malloc(vp8l_len);
        if (!vp8l) return NULL;
        guint32 dims = (guint32)(w - 1) | ((guint32)(h - 1) << 14);
        vp8l[0] = 0x2f;
        vp8l[1] = (guint8)(dims & 0xff);
        vp8l[2] = (guint8)((dims >> 8) & 0xff);
        vp8l[3] = (guint8)((dims >> 16) & 0xff);
        vp8l[4] = (guint8)((dims >> 24) & 0xff);
        memcpy(vp8l + 5, alph + 1, alph_len - 1);

        gsize bare_len = 0;
        guint8 *bare = ns_webp_bare_container("VP8L", vp8l, vp8l_len, &bare_len);
        g_free(vp8l);
        if (!bare) return NULL;

        int aw = 0, ah = 0;
        gsize astride = 0, abuf = 0;
        guint8 *apix = ns_wuffs_decode_still_to_bgra(bare, bare_len, FALSE,
                                                     &aw, &ah, &astride, &abuf);
        g_free(bare);
        if (!apix) return NULL;
        if (aw != w || ah != h) { g_free(apix); return NULL; }

        plane = g_try_malloc(pixels);
        if (!plane) { g_free(apix); return NULL; }
        for (int y = 0; y < h; y++) {
            const guint8 *src = apix + (gsize)y * astride;
            guint8 *dst = plane + (gsize)y * (gsize)w;
            for (int x = 0; x < w; x++) dst[x] = src[(gsize)x * 4 + 1];
        }
        g_free(apix);
    } else {
        return NULL;
    }

    ns_webp_alpha_unfilter(plane, w, h, filtering);
    return plane;
}

/* Applies an alpha plane to opaque BGRA pixels, premultiplying the color channels. */
static void
ns_webp_apply_alpha(guint8 *pix, gsize stride, int w, int h, const guint8 *plane)
{
    for (int y = 0; y < h; y++) {
        guint8 *row = pix + (gsize)y * stride;
        const guint8 *src = plane + (gsize)y * (gsize)w;
        for (int x = 0; x < w; x++) {
            guint8 *p = row + (gsize)x * 4;
            guint a = src[x];
            p[0] = (guint8)((p[0] * a + 127) / 255);
            p[1] = (guint8)((p[1] * a + 127) / 255);
            p[2] = (guint8)((p[2] * a + 127) / 255);
            p[3] = (guint8)a;
        }
    }
}

/* Decodes a still image to premultiplied BGRA, handling extended lossy WebP with alpha. */
guint8 *
ns_image_wuffs_decode_to_bgra(const guchar *data, gsize len,
                              int *out_w, int *out_h,
                              gsize *out_stride, gsize *out_buf_len)
{
    if (ns_wuffs_detect(data, len) != NS_WUFFS_WEBP ||
        !ns_webp_is_extended(data, len))
        return ns_wuffs_decode_still_to_bgra(data, len, TRUE, out_w, out_h,
                                             out_stride, out_buf_len);

    ns_webp_chunks parts;
    if (!ns_webp_find_chunks(data, len, &parts)) return NULL;

    gsize bare_len = 0;
    guint8 *bare = ns_webp_bare_container(parts.body_tag, parts.body,
                                          parts.body_len, &bare_len);
    if (!bare) return NULL;

    int w = 0, h = 0;
    gsize stride = 0, buf_len = 0;
    guint8 *pix = ns_wuffs_decode_still_to_bgra(bare, bare_len, TRUE,
                                                &w, &h, &stride, &buf_len);
    g_free(bare);
    if (!pix) return NULL;

    if (parts.alpha && memcmp(parts.body_tag, "VP8 ", 4) == 0) {
        guint8 *plane = ns_webp_alpha_plane(parts.alpha, parts.alpha_len, w, h);
        if (plane) {
            ns_webp_apply_alpha(pix, stride, w, h, plane);
            g_free(plane);
        }
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_stride) *out_stride = stride;
    if (out_buf_len) *out_buf_len = buf_len;
    return pix;
}

/* Decodes a still image into a premultiplied BGRA texture. */
ns_texture *
ns_image_decode_wuffs(const guchar *data, gsize len, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    gsize stride = 0, buf_len = 0;
    guint8 *pix = ns_image_wuffs_decode_to_bgra(data, len, &w, &h,
                                                &stride, &buf_len);
    if (!pix) return NULL;

    GBytes *bytes = g_bytes_new_take(pix, buf_len);
    ns_texture *tex = ns_texture_new(
        w, h, NS_TEXTURE_BGRA_PREMULTIPLIED, bytes, stride);
    g_bytes_unref(bytes);

    if (tex) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return tex;
}

/* Decodes GIF/APNG frames with disposal handling into full-canvas BGRA frames (capped). */
GArray *
ns_image_decode_wuffs_anim_to_pixels(const guchar *data, gsize len,
                                     int *out_w, int *out_h)
{
    if (!data || len < 6) return NULL;
    ns_wuffs_format kind = ns_wuffs_detect(data, len);
    if (kind != NS_WUFFS_GIF &&
        !(kind == NS_WUFFS_PNG && ns_image_png_is_animated(data, len)))
        return NULL;

    ns_wuffs_canvas c;
    if (!ns_wuffs_canvas_open(&c, data, len,
                              WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL))
        return NULL;
    uint32_t w = c.w, h = c.h;
    uint8_t *pix = c.pix;

    GArray *frames = g_array_new(FALSE, FALSE, sizeof(ns_image_pixel_frame));
    g_array_set_clear_func(frames, ns_image_pixel_frame_clear);
    enum { NS_ANIM_MAX_FRAMES = 1024 };
    const gsize NS_ANIM_MAX_TOTAL_BYTES = (gsize)512 * 1024 * 1024;
    gsize anim_bytes_total = 0;

    uint8_t prev_disposal = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;
    wuffs_base__rect_ie_u32 prev_dirty = {0, 0, 0, 0};
    uint8_t *backup = NULL;
    gsize    pix_bytes_total = (gsize)c.pix_len;

    while (frames->len < NS_ANIM_MAX_FRAMES) {
        wuffs_base__frame_config fc = {0};
        wuffs_base__status st =
            wuffs_base__image_decoder__decode_frame_config(c.dec, &fc, &c.src);
        if (!wuffs_base__status__is_ok(&st)) break;

        wuffs_base__table_u8 tab = wuffs_base__pixel_buffer__plane(&c.pb, 0);
        if (!tab.ptr || tab.stride == 0) break;

        if (prev_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) {
            for (uint32_t y = prev_dirty.min_incl_y; y < prev_dirty.max_excl_y; y++) {
                if ((gsize)y * (gsize)tab.stride >= (gsize)tab.stride * (gsize)h)
                    break;
                uint8_t *row = pix + (gsize)y * (gsize)tab.stride;
                gsize x0 = (gsize)prev_dirty.min_incl_x * 4;
                gsize x1 = (gsize)prev_dirty.max_excl_x * 4;
                if (x1 > tab.stride) x1 = tab.stride;
                if (x0 < x1) memset(row + x0, 0, x1 - x0);
            }
        } else if (prev_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS
                   && backup) {
            memcpy(pix, backup, pix_bytes_total);
        }

        uint8_t cur_disposal = wuffs_base__frame_config__disposal(&fc);
        if (cur_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
            if (!backup) backup = g_try_malloc(pix_bytes_total);
            if (backup) memcpy(backup, pix, pix_bytes_total);
        }

        st = wuffs_base__image_decoder__decode_frame(
            c.dec, &c.pb, &c.src, WUFFS_BASE__PIXEL_BLEND__SRC_OVER,
            wuffs_base__make_slice_u8(c.workbuf, (size_t)c.workbuf_len),
            NULL);
        if (!wuffs_base__status__is_ok(&st)) break;

        prev_disposal = cur_disposal;
        prev_dirty    = wuffs_base__frame_config__bounds(&fc);

        gsize frame_bytes = (gsize)tab.stride * (gsize)h;
        if (frame_bytes > pix_bytes_total) break;
        anim_bytes_total += frame_bytes;
        if (anim_bytes_total > NS_ANIM_MAX_TOTAL_BYTES) break;
        uint8_t *copy = g_try_malloc(frame_bytes);
        if (!copy) break;
        memcpy(copy, pix, frame_bytes);

        uint64_t flicks = wuffs_base__frame_config__duration(&fc);
        int delay_ms = (int)MIN(flicks / 705600, (uint64_t)600000);
        if (delay_ms <= 0) delay_ms = 100;

        ns_image_pixel_frame f = {
            .pixels = copy,
            .pixels_len = frame_bytes,
            .stride = (gsize)tab.stride,
            .format = NS_TEXTURE_BGRA_PREMULTIPLIED,
            .width = (int)w,
            .height = (int)h,
            .delay_ms = delay_ms,
        };
        g_array_append_val(frames, f);
    }

    g_free(backup);
    ns_wuffs_canvas_close(&c);

    if (frames->len == 0) { g_array_free(frames, TRUE); return NULL; }
    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    return frames;
}
