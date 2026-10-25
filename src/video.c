/* Northstar Web Browser — video streams for <video>: MPEG-1 via pl_mpeg, or MSE-fed decoding.
 * MSE streams feed coded frames to a decode worker and present through a video layer.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "video.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mse.h"
#include "videodec.h"
#include "videolayer.h"
#include "videoworker.h"
#include "trace.h"
#include "pl_mpeg.h"

enum {
    NS_VIDEO_MAX_DIMENSION = 4096,
    NS_VIDEO_DECODE_AHEAD = 4,
    NS_VIDEO_DECODE_AHEAD_MAX = 12,
};

static const gsize NS_VIDEO_DECODE_AHEAD_BYTES = 48u * 1024u * 1024u;
static const double NS_VIDEO_FEED_AHEAD_S = 3.0;

static const double NS_VIDEO_FORWARD_DECODE_S = 2.0;
static const double NS_VIDEO_LAYER_LEAD_S = 0.5;
static const int NS_VIDEO_OPEN_ENDED_MS = 24 * 3600 * 1000;

struct ns_video_stream {
    GBytes      *bytes;
    plm_t       *program;
    plm_video_t *elementary;
    int          width;
    int          height;
    double       frame_s;
    int          duration_ms;
    gboolean     has_audio;
    ns_texture  *texture;
    double       shown_time;
    ns_mse_source   *mse;
    ns_video_worker *worker;
    ns_video_layer  *layer;
    guint            seek_gen;
    guint            shown_seek_gen;
    double           fed_pts;
    gboolean         fed_valid;
    gboolean         waiting;
};

/* TRUE if data starts with an MPEG-1 program stream pack header (00 00 01 BA). */
static gboolean
bytes_are_program_stream(const guchar *data, gsize len)
{
    return data && len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
           data[2] == 0x01 && data[3] == 0xBA;
}

/* TRUE if data starts with an MPEG-1 video sequence header (00 00 01 B3). */
static gboolean
bytes_are_elementary_stream(const guchar *data, gsize len)
{
    return data && len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
           data[2] == 0x01 && data[3] == 0xB3;
}

gboolean
ns_video_bytes_are_mpeg1(const guchar *data, gsize len)
{
    return bytes_are_program_stream(data, len) ||
           bytes_are_elementary_stream(data, len);
}

/* Counts MPEG picture start codes, used to estimate elementary stream duration. */
static guint
count_pictures(const guchar *data, gsize len)
{
    guint pictures = 0;
    for (gsize i = 0; i + 3 < len; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01 &&
            data[i + 3] == 0x00)
            pictures++;
    }
    return pictures;
}

/* Opens the MPEG-1 data as a program or elementary stream; reports framerate and duration. */
static gboolean
stream_open(ns_video_stream *s, double *framerate, double *duration_s)
{
    gsize len = 0;
    const guchar *data = g_bytes_get_data(s->bytes, &len);
    if (bytes_are_program_stream(data, len)) {
        s->program = plm_create_with_memory((uint8_t *)data, len, 0);
        if (!s->program) return FALSE;
        plm_set_video_enabled(s->program, 1);
        plm_set_audio_enabled(s->program, 0);
        s->width = plm_get_width(s->program);
        s->height = plm_get_height(s->program);
        s->has_audio = plm_get_num_audio_streams(s->program) > 0;
        *framerate = plm_get_framerate(s->program);
        *duration_s = plm_get_duration(s->program);
        return TRUE;
    }
    plm_buffer_t *buffer =
        plm_buffer_create_with_memory((uint8_t *)data, len, 0);
    if (!buffer) return FALSE;
    s->elementary = plm_video_create_with_buffer(buffer, TRUE);
    if (!s->elementary) {
        plm_buffer_destroy(buffer);
        return FALSE;
    }
    s->width = plm_video_get_width(s->elementary);
    s->height = plm_video_get_height(s->elementary);
    *framerate = plm_video_get_framerate(s->elementary);
    *duration_s = *framerate > 0.0
        ? count_pictures(data, len) / *framerate : 0.0;
    return TRUE;
}

/* Decodes the next video frame from the program or elementary stream. */
static plm_frame_t *
stream_next(ns_video_stream *s)
{
    if (s->program) return plm_decode_video(s->program);
    return plm_video_decode(s->elementary);
}

/* Decodes forward until the frame covering time t; returns the last decoded frame. */
static plm_frame_t *
stream_decode_until(ns_video_stream *s, double t)
{
    plm_frame_t *last = NULL;
    plm_frame_t *frame;
    while ((frame = stream_next(s)) != NULL) {
        last = frame;
        if (frame->time + s->frame_s > t) break;
    }
    return last;
}

/* Seeks to time t, falling back to rewinding and decoding forward. */
static plm_frame_t *
stream_seek(ns_video_stream *s, double t)
{
    if (s->program) {
        plm_frame_t *frame = plm_seek_frame(s->program, t, TRUE);
        if (frame) return frame;
        plm_rewind(s->program);
        return stream_decode_until(s, t);
    }
    plm_video_rewind(s->elementary);
    return stream_decode_until(s, t);
}

/* Converts a decoded frame to a new BGRA texture and makes it the shown frame. */
static gboolean
stream_present(ns_video_stream *s, plm_frame_t *frame)
{
    gsize stride = (gsize)s->width * 4;
    gsize frame_bytes = stride * (gsize)s->height;
    guint8 *pixels = g_try_malloc(frame_bytes);
    if (!pixels) return FALSE;
    memset(pixels, 0xFF, frame_bytes);
    plm_frame_to_bgra(frame, pixels, (int)stride);
    GBytes *bytes = g_bytes_new_take(pixels, frame_bytes);
    ns_texture *texture = ns_texture_new(s->width, s->height,
                                         NS_TEXTURE_BGRA_PREMULTIPLIED,
                                         bytes, stride);
    g_bytes_unref(bytes);
    if (!texture) return FALSE;
    ns_texture_unref(s->texture);
    s->texture = texture;
    s->shown_time = frame->time;
    return TRUE;
}

/* Opens an in-memory MPEG-1 video (dimension-capped) and decodes its first frame. */
ns_video_stream *
ns_video_stream_new(const guchar *data, gsize len)
{
    if (!ns_video_bytes_are_mpeg1(data, len)) return NULL;
    ns_video_stream *s = g_new0(ns_video_stream, 1);
    s->bytes = g_bytes_new(data, len);
    double framerate = 0.0;
    double duration_s = 0.0;
    if (!stream_open(s, &framerate, &duration_s) ||
        s->width <= 0 || s->height <= 0 ||
        s->width > NS_VIDEO_MAX_DIMENSION ||
        s->height > NS_VIDEO_MAX_DIMENSION) {
        ns_video_stream_free(s);
        return NULL;
    }
    s->frame_s = framerate > 0.0 ? 1.0 / framerate : 0.04;
    plm_frame_t *first = stream_next(s);
    if (!first || !stream_present(s, first)) {
        ns_video_stream_free(s);
        return NULL;
    }
    if (duration_s < s->frame_s) duration_s = s->frame_s;
    s->duration_ms = (int)CLAMP(duration_s * 1000.0 + 0.5, 1, G_MAXINT / 2);
    return s;
}

/* Creates a video stream fed from MSE source buffers via a decode worker and layer. */
ns_video_stream *
ns_video_stream_new_mse(ns_mse_source *source)
{
    if (!source) return NULL;
    ns_video_stream *s = g_new0(ns_video_stream, 1);
    s->mse = ns_mse_source_ref(source);
    s->layer = ns_video_layer_new();
    s->shown_time = -1.0;
    return s;
}

/* Decoded-picture queue depth: about 48 MiB of frames, clamped to 4..12. */
static guint
mse_decode_ahead(const ns_video_stream *s)
{
    if (s->width <= 0 || s->height <= 0) return NS_VIDEO_DECODE_AHEAD;
    gsize frame_bytes = (gsize)s->width * (gsize)s->height * 4u;
    gsize n = NS_VIDEO_DECODE_AHEAD_BYTES / frame_bytes;
    return (guint)CLAMP(n, (gsize)NS_VIDEO_DECODE_AHEAD,
                        (gsize)NS_VIDEO_DECODE_AHEAD_MAX);
}

/* Adopts the layer's latest texture; TRUE if a repaint is needed (not composited/resized). */
static gboolean
mse_sync_texture(ns_video_stream *s, gboolean composited)
{
    double pts = -1.0;
    ns_texture *texture = ns_video_layer_texture(s->layer, &pts);
    if (!texture || texture == s->texture) {
        ns_texture_unref(texture);
        return FALSE;
    }
    int width = ns_texture_get_width(texture);
    int height = ns_texture_get_height(texture);
    gboolean resized = !s->texture || width != s->width || height != s->height;
    ns_texture_unref(s->texture);
    s->texture = texture;
    s->shown_time = pts;
    s->shown_seek_gen = s->seek_gen;
    s->width = width;
    s->height = height;
    return !composited || resized;
}

/* Index of the last frame fed to the decoder, or -1 if unknown or since evicted. */
static gssize
mse_fed_index(ns_video_stream *s, ns_mse_buffer *buffer)
{
    if (!s->fed_valid) return -1;
    gssize last = ns_mse_buffer_frame_at(buffer, s->fed_pts);
    const ns_mse_frame *at = last >= 0 ? ns_mse_buffer_frame(buffer, (guint)last) : NULL;
    return at && fabs(at->pts - s->fed_pts) < 1e-6 ? last : -1;
}

/* Decides if decoding must restart from a keyframe (seek back, far jump, lost position). */
static gboolean
mse_needs_restart(ns_video_stream *s, ns_mse_buffer *buffer, gssize last,
                  gssize target, gboolean shown)
{
    const ns_mse_frame *goal = ns_mse_buffer_frame(buffer, (guint)target);
    if (last < 0) return TRUE;
    if (shown) return FALSE;
    if (target <= last)
        return !ns_video_worker_expects(s->worker, goal->pts) &&
               goal->pts > ns_video_layer_shown_pts(s->layer) + 1e-6;
    if (goal->pts < s->shown_time) return TRUE;
    return goal->pts - s->shown_time > NS_VIDEO_FORWARD_DECODE_S &&
           ns_mse_buffer_keyframe_at_or_before(buffer, target) > last;
}

/* Pushes coded frames to the worker from the last fed (or a keyframe) to 3 s past target. */
static void
mse_feed(ns_video_stream *s, ns_mse_buffer *buffer, gssize target, gboolean shown)
{
    const ns_mse_frame *goal = ns_mse_buffer_frame(buffer, (guint)target);
    double goal_pts = goal->pts;
    gssize last = mse_fed_index(s, buffer);
    if (mse_needs_restart(s, buffer, last, target, shown)) {
        gssize keyframe = ns_mse_buffer_keyframe_at_or_before(buffer, target);
        if (keyframe < 0) return;
        ns_video_worker_restart(s->worker);
        last = keyframe - 1;
    }
    guint count = ns_mse_buffer_n_frames(buffer);
    ns_video_worker_set_max_pictures(s->worker, mse_decode_ahead(s));
    gint64 trace_start = ns_trace_now();
    int pushed = 0;
    for (gssize i = last + 1; i < (gssize)count; i++) {
        const ns_mse_frame *frame = ns_mse_buffer_frame(buffer, (guint)i);
        if (i > target && frame->pts > goal_pts + NS_VIDEO_FEED_AHEAD_S) break;
        gboolean want = frame->pts > goal_pts - 1e-6 &&
                        !(shown && fabs(frame->pts - goal_pts) < 1e-6);
        ns_video_worker_push(s->worker, frame->data, frame->config, frame->pts, want);
        s->fed_pts = frame->pts;
        s->fed_valid = TRUE;
        pushed++;
    }
    if (pushed)
        ns_trace_completef("media", "video feed", trace_start,
                           "%d coded frames up to %.3f for %.3f", pushed,
                           s->fed_pts, goal_pts);
}

/* MSE playback step: syncs the layer texture, presents the target frame, feeds the decoder. */
static gboolean
mse_show(ns_video_stream *s, double t)
{
    s->waiting = FALSE;
    ns_mse_buffer *buffer = ns_mse_source_track_buffer(s->mse, NS_MSE_TRACK_VIDEO);
    const ns_mp4_track *track = ns_mse_buffer_track(buffer);
    if (!track) return FALSE;
    if (!s->worker) {
        s->worker = ns_video_worker_new(ns_video_decoder_new(track->codec));
        s->fed_valid = FALSE;
        if (!s->worker) return FALSE;
        ns_video_layer_attach(s->layer, s->worker);
    }
    gboolean composited =
        ns_video_layer_composited(s->layer, g_get_monotonic_time());
    gboolean changed = mse_sync_texture(s, composited);
    if (s->texture && s->shown_seek_gen == s->seek_gen &&
        s->shown_time > t && s->shown_time - t < NS_VIDEO_LAYER_LEAD_S)
        t = s->shown_time;
    gssize target = ns_mse_buffer_frame_at(buffer, t);
    if (target < 0) return FALSE;
    double goal_pts = ns_mse_buffer_frame(buffer, (guint)target)->pts;
    if (!composited && !(s->texture && fabs(goal_pts - s->shown_time) < 1e-6))
        ns_video_layer_present(s->layer, goal_pts);
    if (mse_sync_texture(s, composited)) changed = TRUE;
    gboolean shown = s->texture && fabs(goal_pts - s->shown_time) < 1e-6;
    mse_feed(s, buffer, target, shown);
    s->waiting = !shown && ns_video_worker_expects(s->worker, goal_pts);
    return changed;
}

ns_video_layer *
ns_video_stream_layer(const ns_video_stream *s)
{
    return s ? s->layer : NULL;
}

/* Passes the media clock (and seek generation) to the video layer. */
void
ns_video_stream_set_clock(ns_video_stream *s, const ns_video_clock *clock)
{
    if (!s || !clock) return;
    s->seek_gen = clock->seek_gen;
    ns_video_layer_set_clock(s->layer, clock);
}

/* Reports presented and dropped frame counts from the decode worker. */
gboolean
ns_video_stream_frame_counts(const ns_video_stream *s, guint *presented,
                             guint *dropped)
{
    *presented = *dropped = 0;
    if (!s || !s->worker) return FALSE;
    ns_video_worker_counts(s->worker, presented, dropped);
    return TRUE;
}

gboolean
ns_video_stream_waiting(const ns_video_stream *s)
{
    return s && s->waiting;
}

void
ns_video_stream_free(ns_video_stream *s)
{
    if (!s) return;
    ns_video_layer_detach(s->layer);
    ns_video_layer_unref(s->layer);
    ns_video_worker_free(s->worker);
    ns_mse_source_unref(s->mse);
    if (s->program) plm_destroy(s->program);
    if (s->elementary) plm_video_destroy(s->elementary);
    ns_texture_unref(s->texture);
    if (s->bytes) g_bytes_unref(s->bytes);
    g_free(s);
}

int
ns_video_stream_width(const ns_video_stream *s)
{
    return s ? s->width : 0;
}

int
ns_video_stream_height(const ns_video_stream *s)
{
    return s ? s->height : 0;
}

/* Duration in ms; MSE streams with unknown or infinite duration report 24 h. */
int
ns_video_stream_duration_ms(const ns_video_stream *s)
{
    if (s && s->mse) {
        double d = ns_mse_source_duration(s->mse);
        if (!(d > 0) || isinf(d) || d * 1000.0 > NS_VIDEO_OPEN_ENDED_MS)
            return NS_VIDEO_OPEN_ENDED_MS;
        return (int)(d * 1000.0 + 0.5);
    }
    return s ? s->duration_ms : 0;
}

gboolean
ns_video_stream_has_audio(const ns_video_stream *s)
{
    return s && s->has_audio;
}

GBytes *
ns_video_stream_bytes(const ns_video_stream *s)
{
    return s && s->bytes ? g_bytes_ref(s->bytes) : NULL;
}

/* Approximate memory use: encoded bytes plus one BGRA frame. */
gsize
ns_video_stream_memory(const ns_video_stream *s)
{
    if (!s) return 0;
    gsize encoded = s->bytes ? g_bytes_get_size(s->bytes) : 0;
    return encoded + (gsize)s->width * (gsize)s->height * 4;
}

gboolean
ns_video_stream_is_mse(const ns_video_stream *s)
{
    return s && s->mse;
}

ns_texture *
ns_video_stream_texture(const ns_video_stream *s)
{
    return s ? s->texture : NULL;
}

/* Shows the frame for phase_ms, decoding forward up to 2 s or seeking; TRUE if changed. */
gboolean
ns_video_stream_show(ns_video_stream *s, int phase_ms)
{
    if (!s) return FALSE;
    double t = phase_ms / 1000.0;
    if (s->mse) return mse_show(s, t);
    if (t >= s->shown_time && t < s->shown_time + s->frame_s) return FALSE;
    gboolean forward = t > s->shown_time &&
                       t - s->shown_time < NS_VIDEO_FORWARD_DECODE_S;
    plm_frame_t *frame = forward ? stream_decode_until(s, t)
                                 : stream_seek(s, t);
    return frame && stream_present(s, frame);
}
