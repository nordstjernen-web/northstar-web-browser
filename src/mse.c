/* Northstar Web Browser — Media Source Extensions: source buffers holding demuxed coded frames.
 * MP4/WebM demux, codec support checks, frame eviction, buffered ranges; also file playback.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mse.h"

#include <math.h>
#include <string.h>

#include "audiodec.h"
#include "videodec.h"
#include "webm.h"

enum {
    NS_MSE_VIDEO_QUOTA = 300 * 1024 * 1024,
    NS_MSE_AUDIO_QUOTA = 48 * 1024 * 1024,
};

static const double NS_MSE_RANGE_GAP_S = 0.15;
static const double NS_MSE_REORDER_S = 1.0;

struct ns_mse_buffer {
    ns_mse_source    *source;
    ns_mp4_demuxer   *demuxer;
    ns_webm_demuxer  *webm;
    ns_mse_track_kind kind;
    guint32           track_id;
    GArray           *frames;
    gsize             bytes;
    double            group_end;
    double            last_dts;
    double            last_duration;
    ns_mse_buffer    *companion;
};

struct ns_mse_source {
    gint       refs;
    GPtrArray *buffers;
    double     duration;
    gboolean   ended;
    gboolean   has_live_seekable;
    double     live_seekable_start;
    double     live_seekable_end;
};

/* TRUE if a dotted codec string has the given 2nd (profile) and 4th (bit depth) fields. */
static gboolean
codec_parts_are(const char *codec, const char *profile, const char *depth)
{
    char **parts = g_strsplit(codec, ".", -1);
    gboolean ok = g_strv_length(parts) >= 4 && strcmp(parts[1], profile) == 0 &&
                  strcmp(parts[3], depth) == 0;
    g_strfreev(parts);
    return ok;
}

/* TRUE for avc1/avc3 codec strings with Baseline, Main or High profile. */
static gboolean
avc_profile_supported(const char *codec)
{
    if (!g_str_has_prefix(codec, "avc1.") && !g_str_has_prefix(codec, "avc3."))
        return FALSE;
    const char *digits = codec + 5;
    if (strlen(digits) != 6) return FALSE;
    for (int i = 0; i < 6; i++)
        if (!g_ascii_isxdigit(digits[i])) return FALSE;
    int profile = g_ascii_xdigit_value(digits[0]) << 4 | g_ascii_xdigit_value(digits[1]);
    return profile == 66 || profile == 77 || profile == 100;
}

/* Maps an RFC 6381 codec string to an internal decoder fourcc; sets *video. NULL if none. */
static const char *
decoder_for_codec(const char *codec, gboolean webm, gboolean *video)
{
    *video = FALSE;
    if (g_str_has_prefix(codec, "av01.") && codec_parts_are(codec, "0", "08")) {
        *video = TRUE;
        return "av01";
    }
    if (strcmp(codec, "vp9") == 0 ||
        (g_str_has_prefix(codec, "vp09.") && codec_parts_are(codec, "00", "08"))) {
        *video = TRUE;
        return "vp09";
    }
    if (!webm && avc_profile_supported(codec)) {
        *video = TRUE;
        return "avc1";
    }
    if (!webm && strcmp(codec, "mp4a.40.2") == 0) return "mp4a";
    if (strcmp(codec, "opus") == 0) return "Opus";
    return NULL;
}

/* MediaSource.isTypeSupported: MP4/WebM MIME with codecs all decodable here. */
gboolean
ns_mse_type_supported(const char *type)
{
    if (!type) return FALSE;
    char *lower = g_ascii_strdown(type, -1);
    char **params = g_strsplit(lower, ";", -1);
    g_free(lower);
    const char *mime = g_strstrip(params[0]);
    gboolean webm = g_str_has_suffix(mime, "/webm");
    gboolean video_mime = strcmp(mime, "video/mp4") == 0 || strcmp(mime, "video/webm") == 0;
    gboolean audio_mime = strcmp(mime, "audio/mp4") == 0 || strcmp(mime, "audio/webm") == 0;
    char *codecs = NULL;
    for (int i = 1; params[i]; i++) {
        char *eq = strchr(params[i], '=');
        if (!eq) continue;
        *eq = '\0';
        if (strcmp(g_strstrip(params[i]), "codecs") != 0) continue;
        codecs = g_strdup(g_strstrip(eq + 1));
        g_strdelimit(codecs, "\"'", ' ');
    }
    g_strfreev(params);
    gboolean ok = (video_mime || audio_mime) && codecs != NULL;
    if (ok) {
        char **list = g_strsplit(codecs, ",", -1);
        gboolean any = FALSE;
        for (int i = 0; list[i] && ok; i++) {
            const char *codec = g_strstrip(list[i]);
            if (!*codec) continue;
            any = TRUE;
            gboolean video = FALSE;
            const char *decoder = decoder_for_codec(codec, webm, &video);
            ok = decoder != NULL && (video ? video_mime && ns_video_decoder_supports(decoder)
                                           : ns_audio_decoder_supports(decoder));
        }
        ok = ok && any;
        g_strfreev(list);
    }
    g_free(codecs);
    return ok;
}

static gboolean
demux_append(ns_mse_buffer *buffer, const guint8 *data, gsize len)
{
    return buffer->webm ? ns_webm_demuxer_append(buffer->webm, data, len)
                        : ns_mp4_demuxer_append(buffer->demuxer, data, len);
}

static gboolean
demux_has_init(const ns_mse_buffer *buffer)
{
    return buffer->webm ? ns_webm_demuxer_has_init(buffer->webm)
                        : ns_mp4_demuxer_has_init(buffer->demuxer);
}

static guint
demux_n_tracks(const ns_mse_buffer *buffer)
{
    return buffer->webm ? ns_webm_demuxer_n_tracks(buffer->webm)
                        : ns_mp4_demuxer_n_tracks(buffer->demuxer);
}

static const ns_mp4_track *
demux_track(const ns_mse_buffer *buffer, guint index)
{
    return buffer->webm ? ns_webm_demuxer_track(buffer->webm, index)
                        : ns_mp4_demuxer_track(buffer->demuxer, index);
}

static gboolean
demux_pop_sample(ns_mse_buffer *buffer, ns_mp4_sample *sample)
{
    return buffer->webm ? ns_webm_demuxer_pop_sample(buffer->webm, sample)
                        : ns_mp4_demuxer_pop_sample(buffer->demuxer, sample);
}

static void
frame_clear(gpointer data)
{
    ns_mse_frame *frame = data;
    if (frame->data) g_bytes_unref(frame->data);
    if (frame->config) g_bytes_unref(frame->config);
}

/* Frees a source buffer and its companion; the companion shares and so skips the demuxer. */
static void
buffer_free(gpointer data)
{
    ns_mse_buffer *buffer = data;
    if (!buffer) return;
    if (buffer->companion) {
        buffer->companion->demuxer = NULL;
        buffer->companion->webm = NULL;
        buffer_free(buffer->companion);
    }
    ns_mp4_demuxer_free(buffer->demuxer);
    ns_webm_demuxer_free(buffer->webm);
    g_array_free(buffer->frames, TRUE);
    g_free(buffer);
}

ns_mse_source *
ns_mse_source_new(void)
{
    ns_mse_source *source = g_new0(ns_mse_source, 1);
    source->refs = 1;
    source->buffers = g_ptr_array_new_with_free_func(buffer_free);
    source->duration = NAN;
    return source;
}

ns_mse_source *
ns_mse_source_ref(ns_mse_source *source)
{
    if (source) g_atomic_int_inc(&source->refs);
    return source;
}

void
ns_mse_source_unref(ns_mse_source *source)
{
    if (!source || !g_atomic_int_dec_and_test(&source->refs)) return;
    g_ptr_array_free(source->buffers, TRUE);
    g_free(source);
}

double
ns_mse_source_duration(const ns_mse_source *source)
{
    return source ? source->duration : NAN;
}

void
ns_mse_source_set_duration(ns_mse_source *source, double duration)
{
    if (!source) return;
    source->duration = duration;
}

void
ns_mse_source_set_live_seekable_range(ns_mse_source *source, double start,
                                      double end)
{
    if (!source) return;
    source->has_live_seekable = TRUE;
    source->live_seekable_start = start;
    source->live_seekable_end = end;
}

void
ns_mse_source_clear_live_seekable_range(ns_mse_source *source)
{
    if (source) source->has_live_seekable = FALSE;
}

gboolean
ns_mse_source_ended(const ns_mse_source *source)
{
    return source && source->ended;
}

void
ns_mse_source_set_ended(ns_mse_source *source, gboolean ended)
{
    if (!source) return;
    source->ended = ended;
}

static ns_mse_buffer *
buffer_new(ns_mse_source *source)
{
    ns_mse_buffer *buffer = g_new0(ns_mse_buffer, 1);
    buffer->source = source;
    buffer->frames = g_array_new(FALSE, FALSE, sizeof(ns_mse_frame));
    g_array_set_clear_func(buffer->frames, frame_clear);
    buffer->group_end = -1.0;
    buffer->last_dts = NAN;
    return buffer;
}

/* addSourceBuffer: creates a buffer with an MP4 or WebM demuxer for a supported type. */
ns_mse_buffer *
ns_mse_source_add_buffer(ns_mse_source *source, const char *type)
{
    if (!source || !ns_mse_type_supported(type)) return NULL;
    ns_mse_buffer *buffer = buffer_new(source);
    char *lower = g_ascii_strdown(type, -1);
    if (strstr(lower, "/webm")) buffer->webm = ns_webm_demuxer_new();
    else buffer->demuxer = ns_mp4_demuxer_new();
    g_free(lower);
    g_ptr_array_add(source->buffers, buffer);
    return buffer;
}

/* Creates a buffer for whole-file (non-MSE) playback with the given demuxer kind. */
ns_mse_buffer *
ns_mse_source_add_file_buffer(ns_mse_source *source, gboolean webm)
{
    if (!source) return NULL;
    ns_mse_buffer *buffer = buffer_new(source);
    if (webm) buffer->webm = ns_webm_demuxer_new();
    else buffer->demuxer = ns_mp4_demuxer_new();
    g_ptr_array_add(source->buffers, buffer);
    return buffer;
}

void
ns_mse_source_remove_buffer(ns_mse_source *source, ns_mse_buffer *buffer)
{
    if (!source || !buffer) return;
    g_ptr_array_remove(source->buffers, buffer);
}

/* Finds the buffer, or companion, carrying the given track kind. */
ns_mse_buffer *
ns_mse_source_track_buffer(ns_mse_source *source, ns_mse_track_kind kind)
{
    if (!source) return NULL;
    for (guint i = 0; i < source->buffers->len; i++) {
        ns_mse_buffer *buffer = g_ptr_array_index(source->buffers, i);
        if (buffer->kind == kind) return buffer;
        if (buffer->companion && buffer->companion->kind == kind)
            return buffer->companion;
    }
    return NULL;
}

/* Orders coded frames by decode timestamp, then presentation timestamp. */
static int
frame_dts_cmp(gconstpointer a, gconstpointer b)
{
    const ns_mse_frame *fa = a, *fb = b;
    if (fa->dts != fb->dts) return fa->dts < fb->dts ? -1 : 1;
    if (fa->pts != fb->pts) return fa->pts < fb->pts ? -1 : 1;
    return 0;
}

/* Classifies a demuxed track as video/audio if its codec has a decoder here. */
static ns_mse_track_kind
track_kind(const ns_mp4_track *track)
{
    if (!track) return NS_MSE_TRACK_NONE;
    if (strcmp(track->handler, "vide") == 0 && ns_video_decoder_supports(track->codec))
        return NS_MSE_TRACK_VIDEO;
    if (strcmp(track->handler, "soun") == 0 && ns_audio_decoder_supports(track->codec))
        return NS_MSE_TRACK_AUDIO;
    return NS_MSE_TRACK_NONE;
}

/* After init, picks the first playable track; a track of the other kind gets a companion. */
static void
buffer_choose_track(ns_mse_buffer *buffer)
{
    if (buffer->kind != NS_MSE_TRACK_NONE || !demux_has_init(buffer))
        return;
    for (guint i = 0; i < demux_n_tracks(buffer); i++) {
        const ns_mp4_track *track = demux_track(buffer, i);
        ns_mse_track_kind kind = track_kind(track);
        if (kind == NS_MSE_TRACK_NONE) continue;
        if (buffer->kind == NS_MSE_TRACK_NONE) {
            buffer->kind = kind;
            buffer->track_id = track->id;
        } else if (kind != buffer->kind && !buffer->companion) {
            buffer->companion = buffer_new(buffer->source);
            buffer->companion->demuxer = buffer->demuxer;
            buffer->companion->webm = buffer->webm;
            buffer->companion->kind = kind;
            buffer->companion->track_id = track->id;
        }
    }
}

/* Demuxer track descriptor for the buffer's selected track id. */
const ns_mp4_track *
ns_mse_buffer_track(const ns_mse_buffer *buffer)
{
    if (!buffer || buffer->kind == NS_MSE_TRACK_NONE) return NULL;
    for (guint i = 0; i < demux_n_tracks(buffer); i++) {
        const ns_mp4_track *track = demux_track(buffer, i);
        if (track && track->id == buffer->track_id) return track;
    }
    return NULL;
}

/* Removes frames with PTS in [start, end); for video also drops dependents until a keyframe. */
static void
buffer_remove_range(ns_mse_buffer *buffer, double start, double end)
{
    gboolean removing_dependents = FALSE;
    for (guint i = 0; i < buffer->frames->len;) {
        ns_mse_frame *frame = &g_array_index(buffer->frames, ns_mse_frame, i);
        gboolean inside = frame->pts >= start && frame->pts < end;
        if (!inside && removing_dependents && frame->keyframe)
            removing_dependents = FALSE;
        if (inside || removing_dependents) {
            if (inside && buffer->kind == NS_MSE_TRACK_VIDEO)
                removing_dependents = TRUE;
            if (frame->data) buffer->bytes -= g_bytes_get_size(frame->data);
            g_array_remove_index(buffer->frames, i);
            continue;
        }
        i++;
    }
}

static gsize
batch_bytes(GArray *batch)
{
    gsize bytes = 0;
    for (guint i = 0; i < batch->len; i++) {
        const ns_mse_frame *frame = &g_array_index(batch, ns_mse_frame, i);
        if (frame->data) bytes += g_bytes_get_size(frame->data);
    }
    return bytes;
}

/* TRUE if adding the batch keeps the buffer under its audio/video byte quota. */
static gboolean
batch_fits(const ns_mse_buffer *buffer, GArray *batch)
{
    gsize quota = buffer->kind == NS_MSE_TRACK_AUDIO ? NS_MSE_AUDIO_QUOTA
                                                     : NS_MSE_VIDEO_QUOTA;
    return buffer->bytes + batch_bytes(batch) <= quota;
}

/* Coded frame processing: overlap removal against the coded frame group, merge, re-sort. */
static void
buffer_commit(ns_mse_buffer *buffer, GArray *batch)
{
    if (batch->len == 0) {
        g_array_free(batch, TRUE);
        return;
    }
    double batch_start = G_MAXDOUBLE, batch_end = -G_MAXDOUBLE;
    for (guint i = 0; i < batch->len; i++) {
        const ns_mse_frame *frame = &g_array_index(batch, ns_mse_frame, i);
        batch_start = MIN(batch_start, frame->pts);
        batch_end = MAX(batch_end, frame->pts + frame->duration);
    }
    const ns_mse_frame *first = &g_array_index(batch, ns_mse_frame, 0);
    const ns_mse_frame *last = &g_array_index(batch, ns_mse_frame, batch->len - 1);
    gboolean continues = !isnan(buffer->last_dts) && first->dts >= buffer->last_dts &&
                         first->dts - buffer->last_dts <= 2.0 * buffer->last_duration + 1e-6;
    double remove_from = continues ? MAX(batch_start, buffer->group_end) : batch_start;
    if (remove_from < batch_end) buffer_remove_range(buffer, remove_from, batch_end);
    if (continues) batch_end = MAX(batch_end, buffer->group_end);
    buffer->last_dts = last->dts;
    buffer->last_duration = last->duration;
    buffer->bytes += batch_bytes(batch);
    buffer->group_end = batch_end;
    g_array_set_clear_func(batch, NULL);
    g_array_append_vals(buffer->frames, batch->data, batch->len);
    g_array_free(batch, TRUE);
    g_array_sort(buffer->frames, frame_dts_cmp);
}

static GArray *
batch_new(void)
{
    GArray *batch = g_array_new(FALSE, FALSE, sizeof(ns_mse_frame));
    g_array_set_clear_func(batch, frame_clear);
    return batch;
}

/* Drains demuxed samples into timestamped frame batches, applying offset and append window. */
static ns_mse_append_result
buffer_take_samples(ns_mse_buffer *buffer, double *timestamp_offset,
                    gboolean sequence_mode, double window_start,
                    double window_end)
{
    buffer_choose_track(buffer);
    ns_mse_buffer *targets[2] = { buffer, buffer->companion };
    GArray *batches[2] = { batch_new(), batch_new() };
    double offset = timestamp_offset ? *timestamp_offset : 0.0;
    gboolean offset_fixed = FALSE;
    ns_mp4_sample sample;
    while (demux_pop_sample(buffer, &sample)) {
        int which = targets[1] && sample.track_id == targets[1]->track_id ? 1 : 0;
        ns_mse_buffer *target = targets[which];
        const ns_mp4_track *track = ns_mse_buffer_track(target);
        if (!track || sample.track_id != target->track_id || track->timescale == 0) {
            if (sample.data) g_bytes_unref(sample.data);
            continue;
        }
        double scale = (double)track->timescale;
        double edit = track->edit_media_time > 0
            ? (double)track->edit_media_time / scale : 0.0;
        double pts = (double)sample.pts / scale - edit;
        double dts = (double)sample.dts / scale - edit;
        double duration = (double)sample.duration / scale;
        if (sequence_mode && !offset_fixed) {
            double group_start = target->group_end >= 0 ? target->group_end : 0.0;
            offset = group_start - pts;
            offset_fixed = TRUE;
        }
        pts += offset;
        dts += offset;
        if (pts < window_start - 1e-6 || pts + duration > window_end + 1e-6) {
            if (sample.data) g_bytes_unref(sample.data);
            continue;
        }
        ns_mse_frame frame = {
            .pts = pts, .dts = dts, .duration = duration,
            .keyframe = sample.keyframe, .data = sample.data,
            .config = track->codec_config ? g_bytes_ref(track->codec_config) : NULL,
        };
        g_array_append_val(batches[which], frame);
    }
    if (timestamp_offset) *timestamp_offset = offset;
    if (!batch_fits(buffer, batches[0]) ||
        (targets[1] && !batch_fits(targets[1], batches[1]))) {
        g_array_free(batches[0], TRUE);
        g_array_free(batches[1], TRUE);
        return NS_MSE_APPEND_QUOTA_EXCEEDED;
    }
    buffer_commit(buffer, batches[0]);
    if (targets[1]) buffer_commit(targets[1], batches[1]);
    else g_array_free(batches[1], TRUE);
    return NS_MSE_APPEND_OK;
}

/* appendBuffer: demuxes the bytes and commits frames; parse error or quota results. */
ns_mse_append_result
ns_mse_buffer_append(ns_mse_buffer *buffer, const guint8 *data, gsize len,
                     double *timestamp_offset, gboolean sequence_mode,
                     double window_start, double window_end)
{
    if (!buffer) return NS_MSE_APPEND_PARSE_ERROR;
    if (!demux_append(buffer, data, len))
        return NS_MSE_APPEND_PARSE_ERROR;
    return buffer_take_samples(buffer, timestamp_offset, sequence_mode,
                               window_start, window_end);
}

/* Feeds bytes at a file position through the MP4 sample index; sets the next position. */
ns_mse_append_result
ns_mse_buffer_append_indexed(ns_mse_buffer *buffer, guint64 position,
                             const guint8 *data, gsize len,
                             guint64 *next_position)
{
    *next_position = NS_MP4_INDEX_END;
    if (!buffer || !ns_mp4_demuxer_has_sample_index(buffer->demuxer))
        return NS_MSE_APPEND_PARSE_ERROR;
    *next_position = ns_mp4_demuxer_index_extract(buffer->demuxer, position,
                                                  data, len);
    return buffer_take_samples(buffer, NULL, FALSE, -INFINITY, INFINITY);
}

/* Flushes buffered WebM demuxer data into frames at end of stream. */
void
ns_mse_buffer_flush(ns_mse_buffer *buffer)
{
    if (!buffer || !buffer->webm) return;
    ns_webm_demuxer_flush(buffer->webm);
    buffer_take_samples(buffer, NULL, FALSE, -INFINITY, INFINITY);
}

/* Whole-file duration in seconds from the WebM header or MP4 sample index. */
double
ns_mse_buffer_file_duration(const ns_mse_buffer *buffer)
{
    if (!buffer) return 0.0;
    if (buffer->webm) return ns_webm_demuxer_duration_seconds(buffer->webm);
    return ns_mp4_demuxer_index_duration(buffer->demuxer);
}

/* File byte position to fetch from to seek to seconds via the MP4 index. */
guint64
ns_mse_buffer_file_seek(const ns_mse_buffer *buffer, double seconds)
{
    return buffer ? ns_mp4_demuxer_index_seek(buffer->demuxer, seconds)
                  : NS_MP4_INDEX_END;
}

/* SourceBuffer.remove(start, end) on the buffer and its companion; resets DTS continuity. */
void
ns_mse_buffer_remove(ns_mse_buffer *buffer, double start, double end)
{
    if (!buffer) return;
    buffer_remove_range(buffer, start, end);
    buffer->last_dts = NAN;
    if (buffer->companion) ns_mse_buffer_remove(buffer->companion, start, end);
}

/* SourceBuffer.abort(): resets the demuxer parser and coded frame group state. */
void
ns_mse_buffer_abort(ns_mse_buffer *buffer)
{
    if (!buffer) return;
    if (buffer->webm) ns_webm_demuxer_reset_parser(buffer->webm);
    else ns_mp4_demuxer_reset_parser(buffer->demuxer);
    buffer->group_end = -1.0;
    buffer->last_dts = NAN;
    if (buffer->companion) {
        buffer->companion->group_end = -1.0;
        buffer->companion->last_dts = NAN;
    }
}

static int
range_start_cmp(gconstpointer a, gconstpointer b)
{
    const double *ra = a, *rb = b;
    return ra[0] < rb[0] ? -1 : ra[0] > rb[0] ? 1 : 0;
}

/* Buffered ranges of one track as flat [start, end] pairs, merging gaps up to 0.15 s. */
static GArray *
track_ranges(const ns_mse_buffer *buffer)
{
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(double));
    if (!buffer || buffer->frames->len == 0) return ranges;
    GArray *spans = g_array_sized_new(FALSE, FALSE, 2 * sizeof(double),
                                      buffer->frames->len);
    for (guint i = 0; i < buffer->frames->len; i++) {
        const ns_mse_frame *frame = &g_array_index(buffer->frames, ns_mse_frame, i);
        double span[2] = { frame->pts, frame->pts + frame->duration };
        g_array_append_vals(spans, span, 1);
    }
    g_array_sort(spans, range_start_cmp);
    double start = ((double *)(void *)spans->data)[0];
    double end = ((double *)(void *)spans->data)[1];
    for (guint i = 1; i < spans->len; i++) {
        double *span = (double *)(void *)spans->data + 2 * i;
        if (span[0] <= end + NS_MSE_RANGE_GAP_S) {
            end = MAX(end, span[1]);
            continue;
        }
        g_array_append_val(ranges, start);
        g_array_append_val(ranges, end);
        start = span[0];
        end = span[1];
    }
    g_array_append_val(ranges, start);
    g_array_append_val(ranges, end);
    g_array_free(spans, TRUE);
    return ranges;
}

/* Intersects two sorted flat range arrays; consumes both inputs. */
static GArray *
ranges_intersect(GArray *first, GArray *second)
{
    GArray *both = g_array_new(FALSE, FALSE, sizeof(double));
    guint a = 0, b = 0;
    while (a < first->len && b < second->len) {
        double *ra = (double *)(void *)first->data + a;
        double *rb = (double *)(void *)second->data + b;
        double start = MAX(ra[0], rb[0]);
        double end = MIN(ra[1], rb[1]);
        if (end > start) {
            g_array_append_val(both, start);
            g_array_append_val(both, end);
        }
        if (ra[1] < rb[1]) a += 2;
        else b += 2;
    }
    g_array_free(first, TRUE);
    g_array_free(second, TRUE);
    return both;
}

/* SourceBuffer.buffered: intersection of the buffer's and companion's track ranges. */
GArray *
ns_mse_buffer_ranges(const ns_mse_buffer *buffer)
{
    GArray *ranges = track_ranges(buffer);
    if (buffer && buffer->companion)
        ranges = ranges_intersect(ranges, track_ranges(buffer->companion));
    return ranges;
}

/* MediaSource buffered: intersection of all active buffers' ranges; caller frees. */
GArray *
ns_mse_source_buffered(ns_mse_source *source)
{
    GArray *result = NULL;
    if (source) {
        for (guint i = 0; i < source->buffers->len; i++) {
            ns_mse_buffer *buffer = g_ptr_array_index(source->buffers, i);
            if (buffer->kind == NS_MSE_TRACK_NONE) continue;
            GArray *ranges = ns_mse_buffer_ranges(buffer);
            result = result ? ranges_intersect(result, ranges) : ranges;
        }
    }
    return result ? result : g_array_new(FALSE, FALSE, sizeof(double));
}

/* HTMLMediaElement seekable for MSE: [0, duration], or live/buffered range for infinite. */
GArray *
ns_mse_source_seekable(ns_mse_source *source)
{
    GArray *result = g_array_new(FALSE, FALSE, sizeof(double));
    double duration = ns_mse_source_duration(source);
    if (isnan(duration)) return result;
    double start = 0.0, end = duration;
    if (isinf(duration) && duration > 0) {
        GArray *buffered = ns_mse_source_buffered(source);
        gboolean live = source->has_live_seekable &&
            source->live_seekable_start < source->live_seekable_end;
        if (!live && buffered->len == 0) {
            g_array_free(buffered, TRUE);
            return result;
        }
        start = live ? source->live_seekable_start : 0.0;
        end = live ? source->live_seekable_end : 0.0;
        if (buffered->len > 0) {
            double first = g_array_index(buffered, double, 0);
            double last = g_array_index(buffered, double, buffered->len - 1);
            if (live && first < start) start = first;
            if (last > end) end = last;
        }
        g_array_free(buffered, TRUE);
    }
    g_array_append_val(result, start);
    g_array_append_val(result, end);
    return result;
}

guint
ns_mse_buffer_n_frames(const ns_mse_buffer *buffer)
{
    return buffer ? buffer->frames->len : 0;
}

const ns_mse_frame *
ns_mse_buffer_frame(const ns_mse_buffer *buffer, guint index)
{
    if (!buffer || index >= buffer->frames->len) return NULL;
    return &g_array_index(buffer->frames, ns_mse_frame, index);
}

/* Index of the frame presented at time t (latest PTS <= t back to a keyframe); -1 if gap. */
gssize
ns_mse_buffer_frame_at(const ns_mse_buffer *buffer, double t)
{
    if (!buffer || buffer->frames->len == 0) return -1;
    guint lo = 0, hi = buffer->frames->len;
    while (lo < hi) {
        guint mid = lo + (hi - lo) / 2;
        if (g_array_index(buffer->frames, ns_mse_frame, mid).dts <= t + NS_MSE_REORDER_S)
            lo = mid + 1;
        else
            hi = mid;
    }
    const ns_mse_frame *best = NULL;
    gssize best_index = -1;
    for (gssize i = (gssize)lo - 1; i >= 0; i--) {
        const ns_mse_frame *frame = &g_array_index(buffer->frames, ns_mse_frame, i);
        if (frame->pts > t + 1e-6) continue;
        if (!best || frame->pts > best->pts) {
            best = frame;
            best_index = i;
        }
        if (frame->keyframe) break;
    }
    if (!best || t > best->pts + best->duration + NS_MSE_RANGE_GAP_S) return -1;
    return best_index;
}

/* Index of the first frame whose presentation ends after t; -1 if none. */
gssize
ns_mse_buffer_frame_from(const ns_mse_buffer *buffer, double t)
{
    if (!buffer) return -1;
    for (guint i = 0; i < buffer->frames->len; i++) {
        const ns_mse_frame *frame = &g_array_index(buffer->frames, ns_mse_frame, i);
        if (frame->pts + frame->duration > t + 1e-6) return i;
    }
    return -1;
}

/* Index of the nearest keyframe at or before index; -1 if none. */
gssize
ns_mse_buffer_keyframe_at_or_before(const ns_mse_buffer *buffer, gssize index)
{
    if (!buffer) return -1;
    for (gssize i = index; i >= 0; i--)
        if (g_array_index(buffer->frames, ns_mse_frame, i).keyframe) return i;
    return -1;
}
