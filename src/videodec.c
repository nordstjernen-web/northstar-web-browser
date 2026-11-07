/* Northstar Web Browser — MSE video decoder dispatch over the GStreamer, Media Foundation, AV1, H.264 and VP9 backends.
 * The system's decoders are preferred where they are built: GStreamer on Linux, Media Foundation
 * on Windows. NS_VIDEO_DECODER=native skips them.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "videodec.h"

#include <string.h>

#include "av1.h"
#include "gstvideo.h"
#include "h264.h"
#include "mfvideo.h"
#include "vp9.h"

struct ns_video_decoder {
    ns_gst_video_decoder *gst;
    ns_mf_video_decoder *mf;
    ns_av1_decoder  *av1;
    ns_vp9_decoder  *vp9;
    ns_h264_decoder *h264;
};

static gboolean
codec_is_h264(const char *codec)
{
    return strcmp(codec, "avc1") == 0 || strcmp(codec, "avc3") == 0;
}

/* Returns TRUE if GStreamer, Media Foundation or a native decoder is available for av01, vp09 or avc1/avc3. */
gboolean
ns_video_decoder_supports(const char *codec)
{
    if (!codec) return FALSE;
    if (ns_gst_video_available(codec) || ns_mf_video_available(codec)) return TRUE;
    if (strcmp(codec, "av01") == 0) return ns_av1_available();
    if (strcmp(codec, "vp09") == 0) return ns_vp9_available();
    if (codec_is_h264(codec)) return ns_h264_available();
    return FALSE;
}

/* Creates a GStreamer or Media Foundation decoder for the codec, else a native AV1, H.264 or VP9 one; NULL if unsupported. */
ns_video_decoder *
ns_video_decoder_new(const char *codec)
{
    if (!ns_video_decoder_supports(codec)) return NULL;
    ns_video_decoder *decoder = g_new0(ns_video_decoder, 1);
    decoder->gst = ns_gst_video_decoder_new(codec);
    if (decoder->gst) return decoder;
    decoder->mf = ns_mf_video_decoder_new(codec);
    if (decoder->mf) return decoder;
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
    ns_gst_video_decoder_free(decoder->gst);
    ns_mf_video_decoder_free(decoder->mf);
    ns_av1_decoder_free(decoder->av1);
    ns_vp9_decoder_free(decoder->vp9);
    ns_h264_decoder_free(decoder->h264);
    g_free(decoder);
}

void
ns_video_decoder_flush(ns_video_decoder *decoder)
{
    if (!decoder) return;
    ns_gst_video_decoder_flush(decoder->gst);
    ns_mf_video_decoder_flush(decoder->mf);
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
    if (decoder->gst)
        return ns_gst_video_decoder_decode(decoder->gst, sample, config, timestamp,
                                           want_texture);
    if (decoder->mf)
        return ns_mf_video_decoder_decode(decoder->mf, sample, config, timestamp,
                                          want_texture);
    if (decoder->av1)
        return ns_av1_decoder_decode(decoder->av1, sample, timestamp, want_texture);
    if (decoder->h264)
        return ns_h264_decoder_decode(decoder->h264, sample, config, timestamp,
                                      want_texture);
    return ns_vp9_decoder_decode(decoder->vp9, sample, want_texture);
}

/* Returns a picture decoded for an earlier timestamp (reordered output), or NULL. */
ns_texture *
ns_video_decoder_take_late(ns_video_decoder *decoder, gint64 *timestamp)
{
    if (!decoder) return NULL;
    if (decoder->mf) return ns_mf_video_decoder_take_late(decoder->mf, timestamp);
    if (!decoder->gst) return NULL;
    return ns_gst_video_decoder_take_late(decoder->gst, timestamp);
}

gboolean
ns_video_decoder_native_forced(void)
{
    const char *backend = g_getenv("NS_VIDEO_DECODER");
    return backend && g_ascii_strcasecmp(backend, "native") == 0;
}

enum { NS_VIDEO_MAX_LATE = 16 };

typedef struct {
    gint64      timestamp;
    ns_texture *texture;
} ns_video_late;

void
ns_video_late_push(GQueue *late, gint64 timestamp, ns_texture *texture)
{
    ns_video_late *entry = g_new(ns_video_late, 1);
    entry->timestamp = timestamp;
    entry->texture = texture;
    g_queue_push_tail(late, entry);
    while (g_queue_get_length(late) > NS_VIDEO_MAX_LATE) {
        ns_video_late *old = g_queue_pop_head(late);
        ns_texture_unref(old->texture);
        g_free(old);
    }
}

ns_texture *
ns_video_late_take(GQueue *late, gint64 *timestamp)
{
    ns_video_late *entry = g_queue_pop_head(late);
    if (!entry) return NULL;
    ns_texture *texture = entry->texture;
    if (timestamp) *timestamp = entry->timestamp;
    g_free(entry);
    return texture;
}

void
ns_video_late_clear(GQueue *late)
{
    ns_video_late *entry;
    while ((entry = g_queue_pop_head(late)) != NULL) {
        ns_texture_unref(entry->texture);
        g_free(entry);
    }
}
