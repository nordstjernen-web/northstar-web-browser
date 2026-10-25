/* Northstar Web Browser — MSE video decoder dispatch over the AV1, H.264 and VP9 backends.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "videodec.h"

#include <string.h>

#include "av1.h"
#include "h264.h"
#include "vp9.h"

struct ns_video_decoder {
    ns_av1_decoder  *av1;
    ns_vp9_decoder  *vp9;
    ns_h264_decoder *h264;
};

static gboolean
codec_is_h264(const char *codec)
{
    return strcmp(codec, "avc1") == 0 || strcmp(codec, "avc3") == 0;
}

/* Returns TRUE if a decoder is available for av01, vp09 or avc1/avc3. */
gboolean
ns_video_decoder_supports(const char *codec)
{
    if (!codec) return FALSE;
    if (strcmp(codec, "av01") == 0) return ns_av1_available();
    if (strcmp(codec, "vp09") == 0) return ns_vp9_available();
    if (codec_is_h264(codec)) return ns_h264_available();
    return FALSE;
}

/* Creates an AV1, H.264 or VP9 decoder for the codec, or NULL if unsupported. */
ns_video_decoder *
ns_video_decoder_new(const char *codec)
{
    if (!ns_video_decoder_supports(codec)) return NULL;
    ns_video_decoder *decoder = g_new0(ns_video_decoder, 1);
    if (strcmp(codec, "av01") == 0) decoder->av1 = ns_av1_decoder_new();
    else if (codec_is_h264(codec)) decoder->h264 = ns_h264_decoder_new();
    else decoder->vp9 = ns_vp9_decoder_new();
    if (!decoder->av1 && !decoder->vp9 && !decoder->h264) {
        g_free(decoder);
        return NULL;
    }
    return decoder;
}

void
ns_video_decoder_free(ns_video_decoder *decoder)
{
    if (!decoder) return;
    ns_av1_decoder_free(decoder->av1);
    ns_vp9_decoder_free(decoder->vp9);
    ns_h264_decoder_free(decoder->h264);
    g_free(decoder);
}

void
ns_video_decoder_flush(ns_video_decoder *decoder)
{
    if (!decoder) return;
    ns_av1_decoder_flush(decoder->av1);
    ns_vp9_decoder_flush(decoder->vp9);
    ns_h264_decoder_flush(decoder->h264);
}

/* Decodes one sample with the active backend; returns a texture if requested. */
ns_texture *
ns_video_decoder_decode(ns_video_decoder *decoder, GBytes *sample,
                        GBytes *config, gint64 timestamp, gboolean want_texture)
{
    if (!decoder) return NULL;
    if (decoder->av1)
        return ns_av1_decoder_decode(decoder->av1, sample, timestamp, want_texture);
    if (decoder->h264)
        return ns_h264_decoder_decode(decoder->h264, sample, config, timestamp,
                                      want_texture);
    return ns_vp9_decoder_decode(decoder->vp9, sample, want_texture);
}
