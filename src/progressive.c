/* Northstar Web Browser — progressive <video src> download of MP4/WebM via ranged fetches.
 * Feeds Media Source buffers: finds moov, sizes chunks by bitrate, evicts and refetches on seek.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "progressive.h"

#include <math.h>
#include <string.h>

#include "net.h"

#define NS_PROGRESSIVE_PROBE_BYTES (256 * 1024)
#define NS_PROGRESSIVE_MIN_CHUNK (256 * 1024)
#define NS_PROGRESSIVE_MAX_CHUNK (8 * 1024 * 1024)
#define NS_PROGRESSIVE_MAX_FETCH (64 * 1024 * 1024)
#define NS_PROGRESSIVE_TAIL_BYTES (16 * 1024 * 1024)
#define NS_PROGRESSIVE_MAX_RETRIES 3

static const double NS_PROGRESSIVE_CHUNK_S = 8.0;
static const double NS_PROGRESSIVE_AHEAD_PLAYING_S = 30.0;
static const double NS_PROGRESSIVE_AHEAD_PAUSED_S = 10.0;
static const double NS_PROGRESSIVE_KEEP_BEHIND_S = 30.0;
static const double NS_PROGRESSIVE_EVICT_BEHIND_S = 60.0;
static const double NS_PROGRESSIVE_KEEP_AHEAD_S = 120.0;
static const double NS_PROGRESSIVE_RUN_SLACK_S = 5.0;
static const double NS_PROGRESSIVE_GAP_S = 0.15;

typedef enum {
    REQUEST_PROBE,
    REQUEST_MEDIA,
    REQUEST_WHOLE,
} request_kind;

typedef struct {
    ns_progressive *file;
    request_kind    kind;
    guint64         start;
    GCancellable   *cancellable;
} progressive_request;

struct ns_progressive {
    char                *url;
    char                *top_url;
    ns_fetch_policy     *policy;
    ns_mse_source       *source;
    ns_mse_buffer       *buffer;
    ns_progressive_state state;
    gboolean             sequential;
    gboolean             sniffed;
    guint64              file_size;
    GBytes              *body;
    progressive_request *request;
    int                  failures;
    guint64              probe_offset;
    guint64              next_position;
    gsize                chunk;
    double               run_start;
    gboolean             evicted;
};

static void progressive_fetch(ns_progressive *file, request_kind kind,
                              guint64 start, guint64 length);

/* Cancels the in-flight range request and detaches it from the file. */
static void
progressive_cancel(ns_progressive *file)
{
    if (!file->request) return;
    file->request->file = NULL;
    g_cancellable_cancel(file->request->cancellable);
    file->request = NULL;
}

/* Cancels any request and marks the progressive download failed. */
static void
progressive_fail(ns_progressive *file)
{
    progressive_cancel(file);
    file->state = NS_PROGRESSIVE_FAILED;
}

/* Total file size from a Content-Range header in raw headers; 0 if absent or unknown. */
static guint64
content_range_total(const char *headers)
{
    for (const char *line = headers; line && *line;) {
        const char *end = strchr(line, '\n');
        if (g_ascii_strncasecmp(line, "content-range:", 14) == 0) {
            const char *slash = memchr(line, '/', end ? (gsize)(end - line)
                                                      : strlen(line));
            return slash && g_ascii_isdigit(slash[1])
                ? g_ascii_strtoull(slash + 1, NULL, 10) : 0;
        }
        line = end ? end + 1 : NULL;
    }
    return 0;
}

/* TRUE if the data starts with the EBML magic used by WebM. */
static gboolean
bytes_are_webm(const guint8 *data, gsize len)
{
    return len >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF &&
           data[3] == 0xA3;
}

/* TRUE if the first box type is a known top-level MP4 box. */
static gboolean
bytes_are_mp4(const guint8 *data, gsize len)
{
    static const char *const types[] = {
        "ftyp", "styp", "moov", "mdat", "free", "skip", "wide", "pnot",
    };
    if (len < 8) return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(types); i++)
        if (memcmp(data + 4, types[i], 4) == 0) return TRUE;
    return FALSE;
}

/* Sizes range fetches to about 8 s of media from bitrate, clamped to 256 KiB..8 MiB. */
static void
progressive_choose_chunk(ns_progressive *file)
{
    double duration = ns_mse_source_duration(file->source);
    gsize chunk = 1024 * 1024;
    if (file->file_size > 0 && duration > 0 && isfinite(duration))
        chunk = (gsize)((double)file->file_size / duration *
                        NS_PROGRESSIVE_CHUNK_S);
    file->chunk = CLAMP(chunk, NS_PROGRESSIVE_MIN_CHUNK, NS_PROGRESSIVE_MAX_CHUNK);
}

/* Sets the source duration from the container once known and resizes chunks. */
static void
progressive_update_duration(ns_progressive *file)
{
    double duration = ns_mse_buffer_file_duration(file->buffer);
    if (duration > 0 && isnan(ns_mse_source_duration(file->source))) {
        ns_mse_source_set_duration(file->source, duration);
        progressive_choose_chunk(file);
    }
}

/* Appends in-order bytes for linear playback; at EOF flushes, sets duration and ends. */
static void
progressive_append_sequential(ns_progressive *file, guint64 start,
                              const guint8 *data, gsize len)
{
    if (start != file->next_position) return;
    if (ns_mse_buffer_append(file->buffer, data, len, NULL, FALSE,
                             -INFINITY, INFINITY) == NS_MSE_APPEND_PARSE_ERROR) {
        progressive_fail(file);
        return;
    }
    file->next_position = start + len;
    progressive_update_duration(file);
    if (file->file_size > 0 && file->next_position >= file->file_size) {
        ns_mse_buffer_flush(file->buffer);
        file->next_position = NS_MP4_INDEX_END;
        if (isnan(ns_mse_source_duration(file->source))) {
            GArray *ranges = ns_mse_source_buffered(file->source);
            if (ranges->len)
                ns_mse_source_set_duration(
                    file->source, g_array_index(ranges, double, ranges->len - 1));
            g_array_free(ranges, TRUE);
        }
        ns_mse_source_set_ended(file->source, TRUE);
    }
}

/* Feeds a range response through the MP4 sample index from next_position onward. */
static void
progressive_append_indexed(ns_progressive *file, guint64 start,
                           const guint8 *data, gsize len)
{
    if (file->next_position == NS_MP4_INDEX_END ||
        file->next_position < start ||
        file->next_position - start >= len)
        return;
    gsize skip = (gsize)(file->next_position - start);
    guint64 next = NS_MP4_INDEX_END;
    ns_mse_append_result result =
        ns_mse_buffer_append_indexed(file->buffer, file->next_position,
                                     data + skip, len - skip, &next);
    if (result == NS_MSE_APPEND_PARSE_ERROR) {
        progressive_fail(file);
        return;
    }
    if (result == NS_MSE_APPEND_QUOTA_EXCEEDED) return;
    if (next == file->next_position)
        file->chunk = MIN(file->chunk * 2, (gsize)NS_PROGRESSIVE_MAX_FETCH);
    file->next_position = next;
    if (next == NS_MP4_INDEX_END) ns_mse_source_set_ended(file->source, TRUE);
}

static void
progressive_append_media(ns_progressive *file, guint64 start,
                         const guint8 *data, gsize len)
{
    if (file->sequential)
        progressive_append_sequential(file, start, data, len);
    else
        progressive_append_indexed(file, start, data, len);
}

/* Marks the file ready to play and sets the first fetch position. */
static void
progressive_become_ready(ns_progressive *file)
{
    file->state = NS_PROGRESSIVE_READY;
    file->run_start = 0.0;
    progressive_update_duration(file);
    if (isnan(ns_mse_source_duration(file->source))) progressive_choose_chunk(file);
    file->next_position = file->sequential
        ? 0 : ns_mse_buffer_file_seek(file->buffer, 0.0);
}

/* Switches to sequential (whole-stream) mode, e.g. for WebM or unindexed MP4. */
static void
progressive_start_sequential(ns_progressive *file, gboolean webm,
                             const guint8 *data, gsize len)
{
    if (!file->buffer)
        file->buffer = ns_mse_source_add_file_buffer(file->source, webm);
    else
        ns_mse_buffer_abort(file->buffer);
    file->sequential = TRUE;
    progressive_become_ready(file);
    if (data) progressive_append_sequential(file, 0, data, len);
}

/* Parses the moov box; indexed mode if it has a sample table, else sequential. */
static gboolean
progressive_moov(ns_progressive *file, const guint8 *data, gsize len,
                 const guint8 *whole, gsize whole_len)
{
    if (ns_mse_buffer_append(file->buffer, data, len, NULL, FALSE, -INFINITY,
                             INFINITY) != NS_MSE_APPEND_OK ||
        !ns_mse_buffer_track(file->buffer))
        return FALSE;
    if (ns_mse_buffer_file_seek(file->buffer, 0.0) == NS_MP4_INDEX_END) {
        if (!whole && file->file_size == 0) return FALSE;
        progressive_start_sequential(file, FALSE, whole, whole_len);
        return TRUE;
    }
    progressive_become_ready(file);
    return TRUE;
}

/* Walks top-level MP4 boxes to find moov, fetching more ranges as needed. */
static void
progressive_walk_boxes(ns_progressive *file, guint64 start, const guint8 *data,
                       gsize len, gboolean whole)
{
    for (;;) {
        guint64 rel = file->probe_offset - start;
        gboolean beyond = file->file_size > 0 &&
                          file->probe_offset + 8 > file->file_size;
        gboolean fetchable = !whole && (file->file_size == 0 ||
                                        file->probe_offset + 16 <= file->file_size);
        if (file->probe_offset < start || beyond ||
            (rel + 8 > len && !fetchable)) {
            progressive_fail(file);
            return;
        }
        if (rel + 16 > len && fetchable) {
            guint64 rest = file->file_size - file->probe_offset;
            progressive_fetch(file, REQUEST_PROBE, file->probe_offset,
                              file->file_size > 0 && rest <= NS_PROGRESSIVE_TAIL_BYTES
                                  ? rest : NS_PROGRESSIVE_PROBE_BYTES);
            return;
        }
        const guint8 *box = data + rel;
        guint64 size = (guint64)box[0] << 24 | (guint64)box[1] << 16 |
                       (guint64)box[2] << 8 | box[3];
        gsize header = 8;
        if (size == 1 && rel + 16 <= len) {
            size = 0;
            for (int i = 8; i < 16; i++) size = size << 8 | box[i];
            header = 16;
        } else if (size == 0) {
            size = (file->file_size ? file->file_size : start + len) -
                   file->probe_offset;
        }
        if (size < header || size > G_MAXUINT64 - file->probe_offset) {
            progressive_fail(file);
            return;
        }
        if (memcmp(box + 4, "moov", 4) == 0) {
            if (size > NS_PROGRESSIVE_MAX_FETCH) {
                progressive_fail(file);
                return;
            }
            if (rel + size > len) {
                if (whole) progressive_fail(file);
                else progressive_fetch(file, REQUEST_PROBE, file->probe_offset, size);
                return;
            }
            const guint8 *all = whole ? data : NULL;
            if (!progressive_moov(file, box, (gsize)size, all, len)) {
                progressive_fail(file);
                return;
            }
            if (!file->sequential) progressive_append_media(file, start, data, len);
            return;
        }
        file->probe_offset += size;
    }
}

/* Sniffs the first bytes: WebM goes sequential, non-MP4 is fetched whole, MP4 is probed. */
static void
progressive_probe(ns_progressive *file, guint64 start, const guint8 *data,
                  gsize len, gboolean whole)
{
    if (!file->sniffed) {
        file->sniffed = TRUE;
        if (bytes_are_webm(data, len)) {
            progressive_start_sequential(file, TRUE, data, len);
            return;
        }
        if (!bytes_are_mp4(data, len)) {
            if (whole) {
                file->body = g_bytes_new(data, len);
                file->state = NS_PROGRESSIVE_OTHER_FORMAT;
            } else {
                progressive_fetch(file, REQUEST_WHOLE, 0, 0);
            }
            return;
        }
        file->buffer = ns_mse_source_add_file_buffer(file->source, FALSE);
    }
    progressive_walk_boxes(file, start, data, len, whole);
}

/* Dispatches a range/whole response by request kind; retries media fetches up to 3 times. */
static void
progressive_handle(ns_progressive *file, progressive_request *request,
                   ns_response *resp)
{
    gboolean ok = resp && !resp->error && resp->body &&
                  (resp->status == 200 || resp->status == 206);
    if (!ok) {
        if (request->kind == REQUEST_MEDIA &&
            ++file->failures < NS_PROGRESSIVE_MAX_RETRIES)
            return;
        progressive_fail(file);
        return;
    }
    file->failures = 0;
    const guint8 *data = resp->body->data;
    gsize len = resp->body->len;
    guint64 start = request->start;
    gboolean whole = resp->status == 200;
    if (whole) {
        start = 0;
        file->file_size = len;
    } else if (file->file_size == 0) {
        file->file_size = content_range_total(resp->raw_headers);
    }
    if (whole && file->state == NS_PROGRESSIVE_READY && file->sequential &&
        file->next_position != 0) {
        ns_mse_buffer_abort(file->buffer);
        file->next_position = 0;
    }
    switch (request->kind) {
    case REQUEST_PROBE:
        progressive_probe(file, start, data, len, whole);
        break;
    case REQUEST_MEDIA:
        progressive_append_media(file, start, data, len);
        break;
    case REQUEST_WHOLE:
        file->body = g_bytes_new(data, len);
        file->state = NS_PROGRESSIVE_OTHER_FORMAT;
        break;
    }
}

/* Async fetch completion: hands the response to the file unless it was cancelled. */
static void
progressive_fetched(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    progressive_request *request = user_data;
    GError *error = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &error);
    ns_progressive *file = request->file;
    if (file) {
        file->request = NULL;
        progressive_handle(file, request, resp);
    }
    g_clear_error(&error);
    if (resp) ns_response_free(resp);
    g_object_unref(request->cancellable);
    g_free(request);
}

/* Issues a GET, ranged when length > 0, clamped to the known file size. */
static void
progressive_fetch(ns_progressive *file, request_kind kind, guint64 start,
                  guint64 length)
{
    progressive_request *request = g_new0(progressive_request, 1);
    request->file = file;
    request->kind = kind;
    request->start = start;
    request->cancellable = g_cancellable_new();
    file->request = request;
    char *range = NULL;
    if (length > 0) {
        guint64 last = start + length - 1;
        if (file->file_size > 0) last = MIN(last, file->file_size - 1);
        range = g_strdup_printf("Range: bytes=%" G_GUINT64_FORMAT "-%"
                                G_GUINT64_FORMAT, start, last);
    }
    const char *const headers[] = { range, NULL };
    ns_net_request_async(file->url, file->top_url, "GET", NULL, 0, NULL,
                         range ? headers : NULL, NS_FETCH_DEST_MEDIA,
                         file->policy, request->cancellable,
                         progressive_fetched, request);
    g_free(range);
}

/* Starts progressive download of a media URL with an initial 256 KiB probe. */
ns_progressive *
ns_progressive_new(const char *url, const char *top_url, ns_fetch_policy *policy)
{
    ns_progressive *file = g_new0(ns_progressive, 1);
    file->url = g_strdup(url);
    file->top_url = g_strdup(top_url);
    file->policy = policy ? ns_fetch_policy_ref(policy) : NULL;
    file->source = ns_mse_source_new();
    file->state = NS_PROGRESSIVE_PROBING;
    file->chunk = NS_PROGRESSIVE_MIN_CHUNK;
    progressive_fetch(file, REQUEST_PROBE, 0, NS_PROGRESSIVE_PROBE_BYTES);
    return file;
}

void
ns_progressive_free(ns_progressive *file)
{
    if (!file) return;
    progressive_cancel(file);
    ns_mse_source_unref(file->source);
    if (file->policy) ns_fetch_policy_unref(file->policy);
    if (file->body) g_bytes_unref(file->body);
    g_free(file->url);
    g_free(file->top_url);
    g_free(file);
}

ns_mse_source *
ns_progressive_source(const ns_progressive *file)
{
    return file ? file->source : NULL;
}

ns_progressive_state
ns_progressive_get_state(const ns_progressive *file)
{
    return file ? file->state : NS_PROGRESSIVE_FAILED;
}

/* Transfers ownership of the whole-file body fetched for non-MP4/WebM formats. */
GBytes *
ns_progressive_take_body(ns_progressive *file)
{
    GBytes *body = file ? file->body : NULL;
    if (file) file->body = NULL;
    return body;
}

/* End of the buffered range containing t (0.15 s tolerance); -1 if none. */
static double
buffered_end_at(ns_mse_source *source, double t)
{
    GArray *ranges = ns_mse_source_buffered(source);
    double end = -1.0;
    for (guint i = 0; i + 1 < ranges->len; i += 2) {
        double start = g_array_index(ranges, double, i);
        double stop = g_array_index(ranges, double, i + 1);
        if (t >= start - NS_PROGRESSIVE_GAP_S && t < stop) {
            end = stop;
            break;
        }
    }
    g_array_free(ranges, TRUE);
    return end;
}

/* Evicts media far behind (from a keyframe) and far ahead of the playhead. */
static void
progressive_evict(ns_progressive *file, double t)
{
    GArray *ranges = ns_mse_source_buffered(file->source);
    double first = ranges->len ? g_array_index(ranges, double, 0) : 0.0;
    double last = ranges->len ? g_array_index(ranges, double, ranges->len - 1) : 0.0;
    gboolean any = ranges->len > 0;
    g_array_free(ranges, TRUE);
    if (!any) return;
    if (first < t - NS_PROGRESSIVE_EVICT_BEHIND_S) {
        double cut = t - NS_PROGRESSIVE_KEEP_BEHIND_S;
        ns_mse_buffer *video = ns_mse_source_track_buffer(file->source,
                                                          NS_MSE_TRACK_VIDEO);
        gssize at = ns_mse_buffer_frame_at(video, cut);
        gssize key = at >= 0 ? ns_mse_buffer_keyframe_at_or_before(video, at) : -1;
        if (key >= 0) cut = ns_mse_buffer_frame(video, (guint)key)->pts;
        if (cut > first) {
            ns_mse_buffer_remove(file->buffer, 0.0, cut);
            file->evicted = TRUE;
            file->run_start = MAX(file->run_start, cut);
        }
    }
    if (last > t + NS_PROGRESSIVE_KEEP_AHEAD_S + NS_PROGRESSIVE_RUN_SLACK_S)
        ns_mse_buffer_remove(file->buffer, t + NS_PROGRESSIVE_KEEP_AHEAD_S,
                             INFINITY);
}

/* Restarts fetching from time t, cancelling the current request (seek). */
static void
progressive_restart(ns_progressive *file, double t)
{
    progressive_cancel(file);
    file->run_start = t;
    file->evicted = FALSE;
    if (file->sequential) {
        ns_mse_buffer_abort(file->buffer);
        file->next_position = 0;
    } else {
        file->next_position = ns_mse_buffer_file_seek(file->buffer, t);
    }
    ns_mse_source_set_ended(file->source, file->next_position == NS_MP4_INDEX_END);
}

/* Playback tick: evicts, restarts on seek, and fetches the next chunk when ahead is short. */
void
ns_progressive_update(ns_progressive *file, double t, gboolean playing)
{
    if (!file || file->state != NS_PROGRESSIVE_READY) return;
    progressive_evict(file, t);
    double reach;
    if (file->sequential) {
        double end = buffered_end_at(file->source, t);
        if (end < 0 && file->evicted && t < file->run_start)
            progressive_restart(file, 0.0);
        reach = end >= 0 ? end : t - 1.0;
    } else {
        double run_end = buffered_end_at(file->source, file->run_start);
        reach = MAX(run_end, file->run_start);
        if (t < file->run_start - 0.5 || t > reach + NS_PROGRESSIVE_RUN_SLACK_S) {
            progressive_restart(file, t);
            reach = t;
        }
    }
    if (file->request || file->next_position == NS_MP4_INDEX_END) return;
    double target = playing ? NS_PROGRESSIVE_AHEAD_PLAYING_S
                            : NS_PROGRESSIVE_AHEAD_PAUSED_S;
    if (reach - t >= target) return;
    progressive_fetch(file, REQUEST_MEDIA, file->next_position, file->chunk);
}
