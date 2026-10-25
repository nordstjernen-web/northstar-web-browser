/* Northstar Web Browser — AV1 video decoding over libdav1d, to BGRA textures.
 * Wraps demuxed samples for dav1d and converts pictures by layout, bit depth and color matrix.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "av1.h"

#ifdef NS_HAVE_AV1

#include <errno.h>
#include <string.h>

#include <dav1d/dav1d.h>

#include "yuv.h"

enum {
    NS_AV1_MAX_THREADS = 4,
};

struct ns_av1_decoder {
    Dav1dContext *context;
};

/* dav1d data free callback: drops the GBytes reference held for a wrapped sample. */
static void
release_sample(const uint8_t *data, void *cookie)
{
    (void)data;
    g_bytes_unref(cookie);
}

/* Converts a dav1d picture to a BGRA texture, mapping layout, depth and color matrix. */
static ns_texture *
picture_to_texture(const Dav1dPicture *picture)
{
    ns_yuv_picture yuv = {
        .planes = { picture->data[0], picture->data[1], picture->data[2] },
        .strides = { picture->stride[0], picture->stride[1], picture->stride[1] },
        .width = picture->p.w,
        .height = picture->p.h,
        .bits = picture->p.bpc,
        .sub_x = picture->p.layout == DAV1D_PIXEL_LAYOUT_I420 ||
                 picture->p.layout == DAV1D_PIXEL_LAYOUT_I422,
        .sub_y = picture->p.layout == DAV1D_PIXEL_LAYOUT_I420,
        .monochrome = picture->p.layout == DAV1D_PIXEL_LAYOUT_I400,
        .bt601 = picture->seq_hdr &&
                 (picture->seq_hdr->mtrx == DAV1D_MC_BT601 ||
                  picture->seq_hdr->mtrx == DAV1D_MC_BT470BG ||
                  picture->seq_hdr->mtrx == DAV1D_MC_FCC),
        .full_range = picture->seq_hdr && picture->seq_hdr->color_range,
    };
    return ns_yuv_to_texture(&yuv);
}

gboolean
ns_av1_available(void)
{
    return TRUE;
}

/* Creates a dav1d decoder (up to 4 threads, 1 frame delay); NULL without AV1 support. */
ns_av1_decoder *
ns_av1_decoder_new(void)
{
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = CLAMP((int)g_get_num_processors(), 1, NS_AV1_MAX_THREADS);
    settings.max_frame_delay = 1;
    ns_av1_decoder *decoder = g_new0(ns_av1_decoder, 1);
    if (dav1d_open(&decoder->context, &settings) < 0) {
        g_free(decoder);
        return NULL;
    }
    return decoder;
}

void
ns_av1_decoder_free(ns_av1_decoder *decoder)
{
    if (!decoder) return;
    dav1d_close(&decoder->context);
    g_free(decoder);
}

void
ns_av1_decoder_flush(ns_av1_decoder *decoder)
{
    if (decoder) dav1d_flush(decoder->context);
}

/* Decodes one AV1 sample; returns the latest picture as a texture if wanted, else NULL. */
ns_texture *
ns_av1_decoder_decode(ns_av1_decoder *decoder, GBytes *sample,
                      gint64 timestamp, gboolean want_texture)
{
    if (!decoder || !sample) return NULL;
    gsize len = 0;
    const guint8 *bytes = g_bytes_get_data(sample, &len);
    if (len == 0) return NULL;
    Dav1dData data;
    memset(&data, 0, sizeof data);
    if (dav1d_data_wrap(&data, bytes, len, release_sample,
                        g_bytes_ref(sample)) < 0) {
        g_bytes_unref(sample);
        return NULL;
    }
    data.m.timestamp = timestamp;
    Dav1dPicture picture;
    memset(&picture, 0, sizeof picture);
    gboolean have_picture = FALSE;
    while (data.sz > 0) {
        int status = dav1d_send_data(decoder->context, &data);
        if (status == 0) continue;
        if (status != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&data);
            if (have_picture) dav1d_picture_unref(&picture);
            return NULL;
        }
        if (have_picture) dav1d_picture_unref(&picture);
        have_picture = dav1d_get_picture(decoder->context, &picture) == 0;
        if (!have_picture) {
            dav1d_data_unref(&data);
            return NULL;
        }
    }
    Dav1dPicture next;
    memset(&next, 0, sizeof next);
    if (dav1d_get_picture(decoder->context, &next) == 0) {
        if (have_picture) dav1d_picture_unref(&picture);
        picture = next;
        have_picture = TRUE;
    }
    if (!have_picture) return NULL;
    ns_texture *texture = want_texture ? picture_to_texture(&picture) : NULL;
    dav1d_picture_unref(&picture);
    return texture;
}

#else

gboolean
ns_av1_available(void)
{
    return FALSE;
}

ns_av1_decoder *
ns_av1_decoder_new(void)
{
    return NULL;
}

void
ns_av1_decoder_free(ns_av1_decoder *decoder)
{
    (void)decoder;
}

void
ns_av1_decoder_flush(ns_av1_decoder *decoder)
{
    (void)decoder;
}

ns_texture *
ns_av1_decoder_decode(ns_av1_decoder *decoder, GBytes *sample,
                      gint64 timestamp, gboolean want_texture)
{
    (void)decoder;
    (void)sample;
    (void)timestamp;
    (void)want_texture;
    return NULL;
}

#endif
