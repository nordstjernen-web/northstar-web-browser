/* Northstar Web Browser — YUV picture to BGRA texture conversion for the video decoders.
 * BT.601/709 limited or full range, 8-16 bit samples, chroma subsampling and monochrome.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "yuv.h"

enum {
    NS_YUV_MAX_DIMENSION = 8192,
};

typedef struct {
    int y_scale;
    int y_offset;
    int r_v;
    int g_u;
    int g_v;
    int b_u;
} yuv_coefficients;

/* 16.16 fixed-point YUV to RGB coefficients for BT.601/709, limited or full range. */
static yuv_coefficients
coefficients_for(const ns_yuv_picture *picture)
{
    yuv_coefficients c;
    c.y_scale = picture->full_range ? 1 << 16 : 76309;
    c.y_offset = picture->full_range ? 0 : 16;
    double chroma = picture->full_range ? 1.0 : 255.0 / 224.0;
    double kr = picture->bt601 ? 0.299 : 0.2126;
    double kb = picture->bt601 ? 0.114 : 0.0722;
    double kg = 1.0 - kr - kb;
    c.r_v = (int)(65536.0 * chroma * 2.0 * (1.0 - kr) + 0.5);
    c.b_u = (int)(65536.0 * chroma * 2.0 * (1.0 - kb) + 0.5);
    c.g_u = (int)(65536.0 * chroma * 2.0 * (1.0 - kb) * kb / kg + 0.5);
    c.g_v = (int)(65536.0 * chroma * 2.0 * (1.0 - kr) * kr / kg + 0.5);
    return c;
}

/* Converts a 16.16 fixed-point value to a 0..255 channel. */
static inline guint8
clamp_channel(int value)
{
    value >>= 16;
    return (guint8)(value < 0 ? 0 : value > 255 ? 255 : value);
}

/* Reads a plane sample, downshifting high-bit-depth 16-bit samples to 8 bits. */
static inline int
sample_at(const void *plane, ptrdiff_t stride, int x, int y, int shift)
{
    if (shift < 0)
        return ((const guint8 *)plane)[y * stride + x];
    const guint16 *row =
        (const guint16 *)(const void *)((const guint8 *)plane + y * stride);
    return row[x] >> shift;
}

/* Converts a planar YUV picture (8-16 bit, subsampled or mono) to an opaque BGRA texture. */
ns_texture *
ns_yuv_to_texture(const ns_yuv_picture *picture)
{
    int w = picture->width;
    int h = picture->height;
    if (w <= 0 || h <= 0 || w > NS_YUV_MAX_DIMENSION || h > NS_YUV_MAX_DIMENSION ||
        picture->bits < 8 || picture->bits > 16 || !picture->planes[0])
        return NULL;
    /* A decoder that hands over no chroma planes is giving us a
     * monochrome picture whatever its flags say. */
    gboolean monochrome = picture->monochrome ||
                          !picture->planes[1] || !picture->planes[2];
    int shift = picture->bits > 8 ? picture->bits - 8 : -1;
    yuv_coefficients c = coefficients_for(picture);
    gsize stride = (gsize)w * 4;
    guint8 *pixels = g_try_malloc(stride * (gsize)h);
    if (!pixels) return NULL;
    for (int y = 0; y < h; y++) {
        guint8 *out = pixels + (gsize)y * stride;
        int cy = y >> picture->sub_y;
        for (int x = 0; x < w; x++) {
            int luma = (sample_at(picture->planes[0], picture->strides[0], x, y, shift) -
                        c.y_offset) * c.y_scale;
            int u = 0, v = 0;
            if (!monochrome) {
                int cx = x >> picture->sub_x;
                u = sample_at(picture->planes[1], picture->strides[1], cx, cy, shift) - 128;
                v = sample_at(picture->planes[2], picture->strides[2], cx, cy, shift) - 128;
            }
            out[0] = clamp_channel(luma + c.b_u * u);
            out[1] = clamp_channel(luma - c.g_u * u - c.g_v * v);
            out[2] = clamp_channel(luma + c.r_v * v);
            out[3] = 255;
            out += 4;
        }
    }
    GBytes *bytes = g_bytes_new_take(pixels, stride * (gsize)h);
    ns_texture *texture = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                         bytes, stride);
    g_bytes_unref(bytes);
    return texture;
}
