/* Northstar Web Browser — VP9 video decoding over libvpx, to BGRA textures.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vp9.h"

#ifdef NS_HAVE_VP9

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "yuv.h"

enum {
    NS_VP9_MAX_THREADS = 4,
};

struct ns_vp9_decoder {
    vpx_codec_ctx_t codec;
};

/* Converts a libvpx image (8-bit or high bit depth YUV) to a BGRA texture. */
static ns_texture *
image_to_texture(const vpx_image_t *image)
{
    gboolean high = (image->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
    ns_yuv_picture yuv = {
        .planes = { image->planes[0], image->planes[1], image->planes[2] },
        .strides = { image->stride[0], image->stride[1], image->stride[2] },
        .width = (int)image->d_w,
        .height = (int)image->d_h,
        .bits = high ? (int)image->bit_depth : 8,
        .sub_x = (int)image->x_chroma_shift,
        .sub_y = (int)image->y_chroma_shift,
        .monochrome = !image->planes[1] || !image->planes[2],
        .bt601 = image->cs == VPX_CS_BT_601 || image->cs == VPX_CS_SMPTE_170,
        .full_range = image->range == VPX_CR_FULL_RANGE,
    };
    if (high && yuv.bits <= 8) yuv.bits = 16;
    return ns_yuv_to_texture(&yuv);
}

gboolean
ns_vp9_available(void)
{
    return TRUE;
}

ns_vp9_decoder *
ns_vp9_decoder_new(void)
{
    ns_vp9_decoder *decoder = g_new0(ns_vp9_decoder, 1);
    vpx_codec_dec_cfg_t config = {
        .threads = (unsigned int)CLAMP((int)g_get_num_processors(), 1, NS_VP9_MAX_THREADS),
        .w = 0,
        .h = 0,
    };
    if (vpx_codec_dec_init(&decoder->codec, vpx_codec_vp9_dx(), &config, 0) !=
        VPX_CODEC_OK) {
        g_free(decoder);
        return NULL;
    }
    return decoder;
}

void
ns_vp9_decoder_free(ns_vp9_decoder *decoder)
{
    if (!decoder) return;
    vpx_codec_destroy(&decoder->codec);
    g_free(decoder);
}

/* Drains pending frames from the decoder after a seek or stream end. */
void
ns_vp9_decoder_flush(ns_vp9_decoder *decoder)
{
    if (!decoder) return;
    vpx_codec_decode(&decoder->codec, NULL, 0, NULL, 0);
    vpx_codec_iter_t iter = NULL;
    while (vpx_codec_get_frame(&decoder->codec, &iter) != NULL) {}
}

/* Decodes one VP9 sample; returns the last output frame as a texture when wanted. */
ns_texture *
ns_vp9_decoder_decode(ns_vp9_decoder *decoder, GBytes *sample,
                      gboolean want_texture)
{
    if (!decoder || !sample) return NULL;
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(sample, &len);
    if (len == 0 || len > G_MAXUINT32) return NULL;
    if (vpx_codec_decode(&decoder->codec, data, (unsigned int)len, NULL, 0) !=
        VPX_CODEC_OK)
        return NULL;
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *image = NULL;
    vpx_image_t *next;
    while ((next = vpx_codec_get_frame(&decoder->codec, &iter)) != NULL)
        image = next;
    if (!image || !want_texture) return NULL;
    return image_to_texture(image);
}

#else

gboolean
ns_vp9_available(void)
{
    return FALSE;
}

ns_vp9_decoder *
ns_vp9_decoder_new(void)
{
    return NULL;
}

void
ns_vp9_decoder_free(ns_vp9_decoder *decoder)
{
    (void)decoder;
}

void
ns_vp9_decoder_flush(ns_vp9_decoder *decoder)
{
    (void)decoder;
}

ns_texture *
ns_vp9_decoder_decode(ns_vp9_decoder *decoder, GBytes *sample,
                      gboolean want_texture)
{
    (void)decoder;
    (void)sample;
    (void)want_texture;
    return NULL;
}

#endif
