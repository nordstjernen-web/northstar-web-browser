/* Northstar Web Browser — incremental WebM (Matroska subset) demuxer for Media Source.
 * EBML parsing of tracks, clusters and laced blocks into MP4-style tracks and samples.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "webm.h"

#include <math.h>
#include <string.h>

enum {
    WEBM_ID_EBML = 0x1A45DFA3,
    WEBM_ID_EBML_READ_VERSION = 0x42F7,
    WEBM_ID_EBML_MAX_ID_LENGTH = 0x42F2,
    WEBM_ID_EBML_MAX_SIZE_LENGTH = 0x42F3,
    WEBM_ID_DOC_TYPE = 0x4282,
    WEBM_ID_SEGMENT = 0x18538067,
    WEBM_ID_SEEK_HEAD = 0x114D9B74,
    WEBM_ID_INFO = 0x1549A966,
    WEBM_ID_TIMECODE_SCALE = 0x2AD7B1,
    WEBM_ID_DURATION = 0x4489,
    WEBM_ID_TRACKS = 0x1654AE6B,
    WEBM_ID_TRACK_ENTRY = 0xAE,
    WEBM_ID_TRACK_NUMBER = 0xD7,
    WEBM_ID_TRACK_TYPE = 0x83,
    WEBM_ID_CODEC_ID = 0x86,
    WEBM_ID_CODEC_PRIVATE = 0x63A2,
    WEBM_ID_DEFAULT_DURATION = 0x23E383,
    WEBM_ID_VIDEO = 0xE0,
    WEBM_ID_PIXEL_WIDTH = 0xB0,
    WEBM_ID_PIXEL_HEIGHT = 0xBA,
    WEBM_ID_AUDIO = 0xE1,
    WEBM_ID_SAMPLING_FREQUENCY = 0xB5,
    WEBM_ID_CHANNELS = 0x9F,
    WEBM_ID_CLUSTER = 0x1F43B675,
    WEBM_ID_CLUSTER_TIMECODE = 0xE7,
    WEBM_ID_SIMPLE_BLOCK = 0xA3,
    WEBM_ID_BLOCK_GROUP = 0xA0,
    WEBM_ID_BLOCK = 0xA1,
    WEBM_ID_BLOCK_DURATION = 0x9B,
    WEBM_ID_REFERENCE_BLOCK = 0xFB,
    WEBM_ID_CUES = 0x1C53BB6B,
    WEBM_ID_ATTACHMENTS = 0x1941A469,
    WEBM_ID_CHAPTERS = 0x1043A770,
    WEBM_ID_TAGS = 0x1254C367,
};

enum {
    WEBM_MAX_TRACKS = 64,
    WEBM_MAX_CODEC_PRIVATE_BYTES = 1024 * 1024,
    WEBM_MAX_BLOCK_BYTES = 16 * 1024 * 1024,
    WEBM_MAX_BLOCK_GROUP_BYTES = WEBM_MAX_BLOCK_BYTES + 256 * 1024,
    WEBM_MAX_TRACKS_BYTES = 32 * 1024 * 1024,
    WEBM_MAX_INFO_BYTES = 1024 * 1024,
    WEBM_MAX_EBML_HEADER_BYTES = 64 * 1024,
    WEBM_MAX_LACED_FRAMES = 256,
    WEBM_MAX_APPEND_CHUNK_BYTES = 16 * 1024 * 1024,
    WEBM_TRACK_TYPE_VIDEO = 1,
    WEBM_TRACK_TYPE_AUDIO = 2,
};

static const guint64 WEBM_NANOSECONDS_PER_SECOND = 1000000000;
static const guint64 WEBM_DEFAULT_TIMECODE_SCALE = 1000000;
static const guint64 WEBM_MAX_TIMECODE_SCALE =
    G_GUINT64_CONSTANT(1000000000000);
static const gint64 WEBM_MAX_TIMESTAMP = G_GINT64_CONSTANT(1) << 62;
static const guint64 WEBM_MAX_OPUS_PACKET_NS = 120000000;

typedef enum {
    HEADER_NEED_MORE,
    HEADER_COMPLETE,
    HEADER_INVALID,
} header_result;

typedef enum {
    CONTEXT_TOP,
    CONTEXT_CLUSTER,
} parse_context;

typedef struct {
    guint32  id;
    guint64  size;
    gboolean unknown_size;
    gsize    header_length;
} element_header;

typedef struct {
    const guint8 *data;
    gsize         length;
    gsize         offset;
} element_iterator;

typedef struct {
    guint32       id;
    const guint8 *payload;
    gsize         length;
} child_element;

typedef struct {
    guint64 track_number;
    gint16  relative_timecode;
    guint8  flags;
    guint   frame_count;
    gsize   frame_offsets[WEBM_MAX_LACED_FRAMES];
    gsize   frame_sizes[WEBM_MAX_LACED_FRAMES];
} parsed_block;

typedef struct {
    ns_mp4_track public;
    guint64      default_duration_ns;
    gboolean     is_opus;
    gint64       last_frame_duration;
    gboolean     has_held_block;
    gint64       held_timestamp;
    gboolean     held_keyframe;
    GPtrArray   *held_frames;
} webm_track;

struct ns_webm_demuxer {
    GByteArray   *input;
    gsize         input_offset;
    guint64       position;
    gboolean      error;
    guint64       skip_remaining;
    parse_context context;
    gboolean      cluster_has_end;
    guint64       cluster_end;
    gboolean      cluster_has_timecode;
    guint64       cluster_timecode;
    guint64       timecode_scale;
    gboolean      has_segment_duration;
    double        segment_duration_seconds;
    GPtrArray    *tracks;
    gboolean      has_init;
    GQueue       *ready;
};

/* Length in bytes (1-8) of an EBML variable-size integer from its first byte; 0 if invalid. */
static gsize
vint_length(guint8 first_byte)
{
    for (gsize length = 1; length <= 8; length++) {
        if (first_byte & (0x80u >> (length - 1))) return length;
    }
    return 0;
}

/* Reads an EBML vint, optionally keeping the length marker; flags the all-ones value. */
static header_result
read_vint(const guint8 *data, gsize available, gsize max_length,
          gboolean keep_marker, guint64 *value, gsize *length,
          gboolean *all_ones)
{
    if (available < 1) return HEADER_NEED_MORE;
    gsize n = vint_length(data[0]);
    if (n == 0 || n > max_length) return HEADER_INVALID;
    if (available < n) return HEADER_NEED_MORE;
    guint64 raw = data[0];
    guint64 bits = data[0] & (0xFFu >> n);
    for (gsize i = 1; i < n; i++) {
        raw = (raw << 8) | data[i];
        bits = (bits << 8) | data[i];
    }
    *all_ones = bits == (G_GUINT64_CONSTANT(1) << (7 * n)) - 1;
    *value = keep_marker ? raw : bits;
    *length = n;
    return HEADER_COMPLETE;
}

/* Reads an EBML element ID and size; all-ones size means unknown size. */
static header_result
read_element_header(const guint8 *data, gsize available, element_header *out)
{
    guint64 id = 0;
    gsize id_length = 0;
    gboolean id_all_ones = FALSE;
    header_result result = read_vint(data, available, 4, TRUE, &id,
                                     &id_length, &id_all_ones);
    if (result != HEADER_COMPLETE) return result;
    guint64 id_bits = id & ((G_GUINT64_CONSTANT(1) << (7 * id_length)) - 1);
    if (id_all_ones || id_bits == 0) return HEADER_INVALID;
    guint64 size = 0;
    gsize size_length = 0;
    gboolean size_all_ones = FALSE;
    result = read_vint(data + id_length, available - id_length, 8, FALSE,
                       &size, &size_length, &size_all_ones);
    if (result != HEADER_COMPLETE) return result;
    out->id = (guint32)id;
    out->size = size_all_ones ? 0 : size;
    out->unknown_size = size_all_ones;
    out->header_length = id_length + size_length;
    return HEADER_COMPLETE;
}

static void
iterator_init(element_iterator *it, const guint8 *data, gsize length)
{
    it->data = data;
    it->length = data ? length : 0;
    it->offset = 0;
}

/* Yields the next sized child element; 0 at end, -1 if malformed or unknown-size. */
static int
iterator_next(element_iterator *it, child_element *out)
{
    if (it->offset >= it->length) return 0;
    element_header header;
    if (read_element_header(it->data + it->offset, it->length - it->offset,
                            &header) != HEADER_COMPLETE)
        return -1;
    if (header.unknown_size) return -1;
    gsize remaining = it->length - it->offset - header.header_length;
    if (header.size > remaining) return -1;
    out->id = header.id;
    out->payload = it->data + it->offset + header.header_length;
    out->length = (gsize)header.size;
    it->offset += header.header_length + (gsize)header.size;
    return 1;
}

/* Reads an EBML unsigned integer element of up to 8 bytes. */
static gboolean
read_unsigned(const child_element *element, guint64 *out)
{
    if (element->length > 8) return FALSE;
    guint64 value = 0;
    for (gsize i = 0; i < element->length; i++)
        value = (value << 8) | element->payload[i];
    *out = value;
    return TRUE;
}

/* Reads a sign-extended EBML signed integer element of up to 8 bytes. */
static gboolean
read_signed(const child_element *element, gint64 *out)
{
    if (element->length > 8) return FALSE;
    if (element->length == 0) {
        *out = 0;
        return TRUE;
    }
    guint64 value = (element->payload[0] & 0x80) ? G_MAXUINT64 : 0;
    for (gsize i = 0; i < element->length; i++)
        value = (value << 8) | element->payload[i];
    *out = (gint64)value;
    return TRUE;
}

/* Reads an EBML float element (0, 4 or 8 bytes). */
static gboolean
read_float(const child_element *element, double *out)
{
    guint64 bits = 0;
    if (!read_unsigned(element, &bits)) return FALSE;
    if (element->length == 0) {
        *out = 0.0;
    } else if (element->length == 4) {
        guint32 narrow_bits = (guint32)bits;
        float narrow = 0.0f;
        memcpy(&narrow, &narrow_bits, sizeof narrow);
        *out = narrow;
    } else if (element->length == 8) {
        memcpy(out, &bits, sizeof *out);
    } else {
        return FALSE;
    }
    return TRUE;
}

/* Copies an EBML string element into out, stopping at NUL and truncating. */
static void
read_string(const child_element *element, char *out, gsize out_size)
{
    gsize length = 0;
    while (length < element->length && element->payload[length] != 0)
        length++;
    length = MIN(length, out_size - 1);
    memcpy(out, element->payload, length);
    out[length] = '\0';
}

/* Drops a track's held block awaiting a duration. */
static void
release_held_block(webm_track *track)
{
    if (track->held_frames) g_ptr_array_unref(track->held_frames);
    track->held_frames = NULL;
    track->has_held_block = FALSE;
}

static void
webm_track_free(gpointer data)
{
    webm_track *track = data;
    if (!track) return;
    release_held_block(track);
    if (track->public.codec_config) g_bytes_unref(track->public.codec_config);
    if (track->public.config_obus) g_bytes_unref(track->public.config_obus);
    g_free(track);
}

/* Finds a WebM track by track number. */
static webm_track *
find_track(GPtrArray *tracks, guint64 number)
{
    for (guint i = 0; i < tracks->len; i++) {
        webm_track *track = g_ptr_array_index(tracks, i);
        if (track->public.id == number) return track;
    }
    return NULL;
}

/* True if the timecode scale divides one second evenly in nanoseconds. */
static gboolean
timecode_scale_is_exact(guint64 timecode_scale)
{
    return WEBM_NANOSECONDS_PER_SECOND % timecode_scale == 0;
}

/* Sample timescale for a timecode scale: ticks per second, or ns when inexact. */
static guint32
timescale_for(guint64 timecode_scale)
{
    if (timecode_scale_is_exact(timecode_scale))
        return (guint32)(WEBM_NANOSECONDS_PER_SECOND / timecode_scale);
    return (guint32)WEBM_NANOSECONDS_PER_SECOND;
}

/* Factor converting block timecodes to timescale ticks (1 when scale is exact). */
static guint64
tick_multiplier(guint64 timecode_scale)
{
    return timecode_scale_is_exact(timecode_scale) ? 1 : timecode_scale;
}

/* Rounds a nanosecond duration to timescale ticks, capped to the max timestamp. */
static gint64
nanoseconds_to_ticks(guint64 nanoseconds, guint32 timescale)
{
    guint64 nanoseconds_per_tick = WEBM_NANOSECONDS_PER_SECOND / timescale;
    guint64 ticks = nanoseconds / nanoseconds_per_tick;
    if ((nanoseconds % nanoseconds_per_tick) * 2 >= nanoseconds_per_tick)
        ticks++;
    return (gint64)MIN(ticks, (guint64)WEBM_MAX_TIMESTAMP);
}

/* Splits a total duration across count frames, spreading the remainder. */
static void
fill_spread_durations(gint64 total, guint count, gint64 *durations)
{
    if (total < 0) total = 0;
    gint64 base = total / count;
    gint64 remainder = total % count;
    for (guint i = 0; i < count; i++)
        durations[i] = base + ((gint64)i < remainder ? 1 : 0);
}

/* Queues laced frames as consecutive samples sharing keyframe status. */
static void
emit_frame_group(ns_webm_demuxer *d, webm_track *track, gint64 timestamp,
                 gboolean keyframe, GPtrArray *frames, const gint64 *durations)
{
    for (guint i = 0; i < frames->len; i++) {
        gint64 duration = CLAMP(durations[i], 0, (gint64)G_MAXUINT32);
        ns_mp4_sample *sample = g_new0(ns_mp4_sample, 1);
        sample->track_id = track->public.id;
        sample->dts = timestamp;
        sample->pts = timestamp;
        sample->duration = (guint32)duration;
        sample->keyframe = keyframe;
        sample->data = g_bytes_ref(g_ptr_array_index(frames, i));
        g_queue_push_tail(d->ready, sample);
        timestamp += duration;
        if (duration > 0) track->last_frame_duration = duration;
    }
}

/* Emits a held block, deriving its duration from the next timestamp or last frame. */
static void
resolve_held_block(ns_webm_demuxer *d, webm_track *track,
                   gboolean has_next_timestamp, gint64 next_timestamp)
{
    if (!track->has_held_block) return;
    guint count = track->held_frames->len;
    gint64 total = track->last_frame_duration * (gint64)count;
    if (has_next_timestamp && next_timestamp > track->held_timestamp)
        total = next_timestamp - track->held_timestamp;
    gint64 durations[WEBM_MAX_LACED_FRAMES];
    fill_spread_durations(total, count, durations);
    emit_frame_group(d, track, track->held_timestamp, track->held_keyframe,
                     track->held_frames, durations);
    release_held_block(track);
}

static void
resolve_all_held_blocks(ns_webm_demuxer *d)
{
    for (guint i = 0; i < d->tracks->len; i++)
        resolve_held_block(d, g_ptr_array_index(d->tracks, i), FALSE, 0);
}

static void
drop_all_held_blocks(ns_webm_demuxer *d)
{
    for (guint i = 0; i < d->tracks->len; i++)
        release_held_block(g_ptr_array_index(d->tracks, i));
}

/* Sets the segment timecode scale, flushing held blocks if it changes. */
static void
apply_timecode_scale(ns_webm_demuxer *d, guint64 timecode_scale)
{
    if (timecode_scale != d->timecode_scale) resolve_all_held_blocks(d);
    d->timecode_scale = timecode_scale;
    for (guint i = 0; i < d->tracks->len; i++) {
        webm_track *track = g_ptr_array_index(d->tracks, i);
        track->public.timescale = timescale_for(timecode_scale);
    }
}

/* Opus packet duration from its TOC byte and frame count; 0 if invalid. */
static guint64
opus_packet_duration_ns(GBytes *frame)
{
    static const guint64 silk_frame_ns[4] = {
        10000000, 20000000, 40000000, 60000000,
    };
    static const guint64 celt_frame_ns[4] = {
        2500000, 5000000, 10000000, 20000000,
    };
    gsize length = 0;
    const guint8 *packet = g_bytes_get_data(frame, &length);
    if (!packet || length < 1) return 0;
    guint configuration = packet[0] >> 3;
    guint64 frame_ns;
    if (configuration < 12)
        frame_ns = silk_frame_ns[configuration & 3];
    else if (configuration < 16)
        frame_ns = (configuration & 1) ? 20000000 : 10000000;
    else
        frame_ns = celt_frame_ns[configuration & 3];
    guint frame_count;
    switch (packet[0] & 3) {
    case 0:
        frame_count = 1;
        break;
    case 1:
    case 2:
        frame_count = 2;
        break;
    default:
        if (length < 2) return 0;
        frame_count = packet[1] & 0x3F;
        break;
    }
    guint64 total = frame_ns * frame_count;
    if (total == 0 || total > WEBM_MAX_OPUS_PACKET_NS) return 0;
    return total;
}

/* Reads Xiph-style lace sizes (255-continued bytes) for all but the last frame. */
static gboolean
read_xiph_lace_sizes(const guint8 *data, gsize length, gsize *offset,
                     parsed_block *block, gsize *sum)
{
    for (guint i = 0; i + 1 < block->frame_count; i++) {
        gsize size = 0;
        guint8 byte;
        do {
            if (*offset >= length) return FALSE;
            byte = data[(*offset)++];
            size += byte;
            if (size > length) return FALSE;
        } while (byte == 255);
        block->frame_sizes[i] = size;
        *sum += size;
        if (*sum > length) return FALSE;
    }
    return TRUE;
}

/* Reads EBML lace sizes: first absolute, the rest as signed vint deltas. */
static gboolean
read_ebml_lace_sizes(const guint8 *data, gsize length, gsize *offset,
                     parsed_block *block, gsize *sum)
{
    gint64 previous = 0;
    for (guint i = 0; i + 1 < block->frame_count; i++) {
        guint64 raw = 0;
        gsize vint_size = 0;
        gboolean all_ones = FALSE;
        if (read_vint(data + *offset, length - *offset, 8, FALSE, &raw,
                      &vint_size, &all_ones) != HEADER_COMPLETE)
            return FALSE;
        *offset += vint_size;
        gint64 size;
        if (i == 0) {
            if (raw > length) return FALSE;
            size = (gint64)raw;
        } else {
            guint64 half_range = G_GUINT64_CONSTANT(1) << (7 * vint_size - 1);
            gint64 bias = (gint64)(half_range - 1);
            size = previous + ((gint64)raw - bias);
        }
        if (size < 0 || (guint64)size > length) return FALSE;
        block->frame_sizes[i] = (gsize)size;
        previous = size;
        *sum += (gsize)size;
        if (*sum > length) return FALSE;
    }
    return TRUE;
}

/* Parses a (Simple)Block header and lacing into per-frame offsets and sizes. */
static gboolean
parse_block_payload(const guint8 *data, gsize length, parsed_block *block)
{
    gsize offset = 0;
    gsize vint_size = 0;
    gboolean all_ones = FALSE;
    if (read_vint(data, length, 8, FALSE, &block->track_number, &vint_size,
                  &all_ones) != HEADER_COMPLETE)
        return FALSE;
    if (all_ones) return FALSE;
    offset = vint_size;
    if (length - offset < 3) return FALSE;
    block->relative_timecode =
        (gint16)(guint16)(((guint16)data[offset] << 8) | data[offset + 1]);
    block->flags = data[offset + 2];
    offset += 3;
    guint lacing = (block->flags >> 1) & 3;
    if (lacing == 0) {
        block->frame_count = 1;
        block->frame_offsets[0] = offset;
        block->frame_sizes[0] = length - offset;
        return TRUE;
    }
    if (offset >= length) return FALSE;
    block->frame_count = (guint)data[offset] + 1;
    offset++;
    gsize sum = 0;
    if (lacing == 1) {
        if (!read_xiph_lace_sizes(data, length, &offset, block, &sum))
            return FALSE;
    } else if (lacing == 3) {
        if (!read_ebml_lace_sizes(data, length, &offset, block, &sum))
            return FALSE;
    } else {
        gsize remaining = length - offset;
        if (remaining % block->frame_count != 0) return FALSE;
        for (guint i = 0; i + 1 < block->frame_count; i++)
            block->frame_sizes[i] = remaining / block->frame_count;
        sum = remaining - remaining / block->frame_count;
    }
    if (offset > length || sum > length - offset) return FALSE;
    block->frame_sizes[block->frame_count - 1] = length - offset - sum;
    for (guint i = 0; i < block->frame_count; i++) {
        block->frame_offsets[i] = offset;
        offset += block->frame_sizes[i];
    }
    return TRUE;
}

/* Absolute block timestamp in timescale ticks from cluster and relative timecodes. */
static gboolean
block_timestamp(const ns_webm_demuxer *d, gint16 relative_timecode,
                gint64 *out)
{
    gint64 ticks = (gint64)d->cluster_timecode + relative_timecode;
    guint64 multiplier = tick_multiplier(d->timecode_scale);
    gint64 limit = WEBM_MAX_TIMESTAMP / (gint64)multiplier;
    if (ticks > limit || ticks < -limit) return FALSE;
    *out = ticks * (gint64)multiplier;
    return TRUE;
}

/* Splits a block into samples, holding it when no duration source is known. */
static gboolean
handle_block(ns_webm_demuxer *d, GBytes *block_bytes, gboolean is_simple_block,
             gboolean has_block_duration, guint64 block_duration,
             gboolean has_reference)
{
    gsize length = 0;
    const guint8 *data = g_bytes_get_data(block_bytes, &length);
    parsed_block block;
    if (!data || !parse_block_payload(data, length, &block)) return FALSE;
    if (!d->cluster_has_timecode) return FALSE;
    gint64 timestamp = 0;
    if (!block_timestamp(d, block.relative_timecode, &timestamp)) return FALSE;
    webm_track *track = find_track(d->tracks, block.track_number);
    if (!track) return TRUE;
    gboolean keyframe = is_simple_block ? (block.flags & 0x80) != 0
                                        : !has_reference;
    resolve_held_block(d, track, TRUE, timestamp);
    GPtrArray *frames =
        g_ptr_array_new_full(block.frame_count, (GDestroyNotify)g_bytes_unref);
    for (guint i = 0; i < block.frame_count; i++)
        g_ptr_array_add(frames,
                        g_bytes_new_from_bytes(block_bytes,
                                               block.frame_offsets[i],
                                               block.frame_sizes[i]));
    gint64 durations[WEBM_MAX_LACED_FRAMES];
    gboolean known = TRUE;
    if (has_block_duration) {
        guint64 multiplier = tick_multiplier(d->timecode_scale);
        guint64 cap = (guint64)G_MAXUINT32 * WEBM_MAX_LACED_FRAMES;
        guint64 total = block_duration > cap / multiplier
            ? cap : block_duration * multiplier;
        fill_spread_durations((gint64)total, block.frame_count, durations);
    } else if (track->default_duration_ns > 0) {
        gint64 per_frame = nanoseconds_to_ticks(track->default_duration_ns,
                                                track->public.timescale);
        for (guint i = 0; i < block.frame_count; i++)
            durations[i] = per_frame;
    } else if (track->is_opus) {
        for (guint i = 0; i < block.frame_count && known; i++) {
            guint64 ns = opus_packet_duration_ns(g_ptr_array_index(frames, i));
            if (ns == 0) known = FALSE;
            durations[i] = nanoseconds_to_ticks(ns, track->public.timescale);
        }
    } else {
        known = FALSE;
    }
    if (known) {
        emit_frame_group(d, track, timestamp, keyframe, frames, durations);
        g_ptr_array_unref(frames);
        return TRUE;
    }
    track->has_held_block = TRUE;
    track->held_timestamp = timestamp;
    track->held_keyframe = keyframe;
    track->held_frames = frames;
    return TRUE;
}

/* Parses a BlockGroup: Block, BlockDuration and ReferenceBlock (non-keyframe). */
static gboolean
parse_block_group(ns_webm_demuxer *d, const guint8 *payload, gsize length)
{
    element_iterator it;
    iterator_init(&it, payload, length);
    child_element child;
    const guint8 *block_payload = NULL;
    gsize block_length = 0;
    gboolean has_duration = FALSE;
    guint64 duration = 0;
    gboolean has_reference = FALSE;
    int step;
    while ((step = iterator_next(&it, &child)) > 0) {
        switch (child.id) {
        case WEBM_ID_BLOCK:
            if (block_payload) return FALSE;
            block_payload = child.payload;
            block_length = child.length;
            break;
        case WEBM_ID_BLOCK_DURATION:
            if (!read_unsigned(&child, &duration)) return FALSE;
            has_duration = TRUE;
            break;
        case WEBM_ID_REFERENCE_BLOCK: {
            gint64 reference = 0;
            if (!read_signed(&child, &reference)) return FALSE;
            has_reference = TRUE;
            break;
        }
        default:
            break;
        }
    }
    if (step < 0 || !block_payload || block_length > WEBM_MAX_BLOCK_BYTES)
        return FALSE;
    GBytes *block_bytes = g_bytes_new(block_payload, block_length);
    gboolean ok = handle_block(d, block_bytes, FALSE, has_duration, duration,
                               has_reference);
    g_bytes_unref(block_bytes);
    return ok;
}

/* Validates the EBML header limits and requires a webm or matroska DocType. */
static gboolean
parse_ebml_header(const guint8 *payload, gsize length)
{
    element_iterator it;
    iterator_init(&it, payload, length);
    child_element child;
    char doc_type[16] = "matroska";
    int step;
    while ((step = iterator_next(&it, &child)) > 0) {
        guint64 value = 0;
        switch (child.id) {
        case WEBM_ID_DOC_TYPE:
            read_string(&child, doc_type, sizeof doc_type);
            break;
        case WEBM_ID_EBML_READ_VERSION:
            if (!read_unsigned(&child, &value) || value > 1) return FALSE;
            break;
        case WEBM_ID_EBML_MAX_ID_LENGTH:
            if (!read_unsigned(&child, &value) || value > 4) return FALSE;
            break;
        case WEBM_ID_EBML_MAX_SIZE_LENGTH:
            if (!read_unsigned(&child, &value) || value > 8) return FALSE;
            break;
        default:
            break;
        }
    }
    if (step < 0) return FALSE;
    return strcmp(doc_type, "webm") == 0 || strcmp(doc_type, "matroska") == 0;
}

/* Parses segment Info: timecode scale and duration in seconds. */
static gboolean
parse_info(ns_webm_demuxer *d, const guint8 *payload, gsize length)
{
    element_iterator it;
    iterator_init(&it, payload, length);
    child_element child;
    guint64 timecode_scale = WEBM_DEFAULT_TIMECODE_SCALE;
    gboolean has_duration = FALSE;
    double duration = 0.0;
    int step;
    while ((step = iterator_next(&it, &child)) > 0) {
        if (child.id == WEBM_ID_TIMECODE_SCALE) {
            if (!read_unsigned(&child, &timecode_scale)) return FALSE;
            if (timecode_scale == 0 || timecode_scale > WEBM_MAX_TIMECODE_SCALE)
                return FALSE;
        } else if (child.id == WEBM_ID_DURATION) {
            if (!read_float(&child, &duration)) return FALSE;
            has_duration = isfinite(duration) && duration >= 0.0;
        }
    }
    if (step < 0) return FALSE;
    apply_timecode_scale(d, timecode_scale);
    d->has_segment_duration = has_duration;
    d->segment_duration_seconds = has_duration
        ? duration * (double)timecode_scale /
              (double)WEBM_NANOSECONDS_PER_SECOND
        : 0.0;
    return TRUE;
}

/* Maps a Matroska CodecID to an MP4-style codec fourcc name. */
static void
assign_codec_name(webm_track *track, const char *codec_id)
{
    static const struct {
        const char *codec_id;
        const char *name;
    } known_codecs[] = {
        { "V_VP9", "vp09" },
        { "V_VP8", "vp08" },
        { "V_AV1", "av01" },
        { "A_OPUS", "Opus" },
        { "A_VORBIS", "Vorb" },
        { "A_FLAC", "fLaC" },
        { "V_MPEG4/ISO/AVC", "avc1" },
        { "V_MPEGH/ISO/HEVC", "hvc1" },
        { "A_AAC", "mp4a" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known_codecs); i++) {
        if (strcmp(codec_id, known_codecs[i].codec_id) == 0 ||
            (g_str_has_prefix(codec_id, "A_AAC/") &&
             strcmp(known_codecs[i].codec_id, "A_AAC") == 0)) {
            g_strlcpy(track->public.codec, known_codecs[i].name,
                      sizeof track->public.codec);
            return;
        }
    }
    const char *underscore = strchr(codec_id, '_');
    const char *name = underscore ? underscore + 1 : codec_id;
    gsize i = 0;
    for (; i < 4 && name[i]; i++)
        track->public.codec[i] = g_ascii_isprint(name[i]) ? name[i] : '?';
    track->public.codec[i] = '\0';
}

/* Reads pixel width and height from a Video track element. */
static gboolean
parse_track_video(webm_track *track, const child_element *video)
{
    element_iterator it;
    iterator_init(&it, video->payload, video->length);
    child_element child;
    int step;
    while ((step = iterator_next(&it, &child)) > 0) {
        guint64 value = 0;
        if (child.id != WEBM_ID_PIXEL_WIDTH && child.id != WEBM_ID_PIXEL_HEIGHT)
            continue;
        if (!read_unsigned(&child, &value)) return FALSE;
        int clamped = (int)MIN(value, (guint64)G_MAXINT);
        if (child.id == WEBM_ID_PIXEL_WIDTH)
            track->public.width = clamped;
        else
            track->public.height = clamped;
    }
    return step == 0;
}

/* Reads sampling frequency and channel count from an Audio track element. */
static gboolean
parse_track_audio(webm_track *track, const child_element *audio)
{
    element_iterator it;
    iterator_init(&it, audio->payload, audio->length);
    child_element child;
    int step;
    while ((step = iterator_next(&it, &child)) > 0) {
        if (child.id == WEBM_ID_SAMPLING_FREQUENCY) {
            double rate = 0.0;
            if (!read_float(&child, &rate)) return FALSE;
            gboolean plausible = isfinite(rate) && rate > 0.0 && rate < 1e7;
            track->public.sample_rate = plausible ? (int)(rate + 0.5) : 0;
        } else if (child.id == WEBM_ID_CHANNELS) {
            guint64 channels = 0;
            if (!read_unsigned(&child, &channels)) return FALSE;
            track->public.channels = (int)MIN(channels, (guint64)1024);
        }
    }
    return step == 0;
}

/* Parses a TrackEntry; *out is NULL for non audio/video tracks. */
static gboolean
parse_track_entry(ns_webm_demuxer *d, const child_element *entry,
                  webm_track **out)
{
    webm_track *track = g_new0(webm_track, 1);
    track->public.edit_media_time = -1;
    track->public.channels = 1;
    track->public.sample_rate = 8000;
    track->public.timescale = timescale_for(d->timecode_scale);
    element_iterator it;
    iterator_init(&it, entry->payload, entry->length);
    child_element child;
    guint64 number = 0;
    guint64 type = 0;
    char codec_id[64] = "";
    gboolean has_codec_id = FALSE;
    gboolean ok = TRUE;
    int step;
    while (ok && (step = iterator_next(&it, &child)) > 0) {
        switch (child.id) {
        case WEBM_ID_TRACK_NUMBER:
            ok = read_unsigned(&child, &number);
            break;
        case WEBM_ID_TRACK_TYPE:
            ok = read_unsigned(&child, &type);
            break;
        case WEBM_ID_CODEC_ID:
            read_string(&child, codec_id, sizeof codec_id);
            has_codec_id = TRUE;
            break;
        case WEBM_ID_CODEC_PRIVATE:
            if (child.length > WEBM_MAX_CODEC_PRIVATE_BYTES) {
                ok = FALSE;
                break;
            }
            if (track->public.codec_config)
                g_bytes_unref(track->public.codec_config);
            track->public.codec_config =
                g_bytes_new(child.payload, child.length);
            break;
        case WEBM_ID_DEFAULT_DURATION:
            ok = read_unsigned(&child, &track->default_duration_ns);
            break;
        case WEBM_ID_VIDEO:
            ok = parse_track_video(track, &child);
            break;
        case WEBM_ID_AUDIO:
            ok = parse_track_audio(track, &child);
            break;
        default:
            break;
        }
    }
    if (!ok || step < 0 || number == 0 || number > G_MAXUINT32 ||
        !has_codec_id) {
        webm_track_free(track);
        return FALSE;
    }
    *out = NULL;
    if (type != WEBM_TRACK_TYPE_VIDEO && type != WEBM_TRACK_TYPE_AUDIO) {
        webm_track_free(track);
        return TRUE;
    }
    track->public.id = (guint32)number;
    g_strlcpy(track->public.handler,
              type == WEBM_TRACK_TYPE_VIDEO ? "vide" : "soun",
              sizeof track->public.handler);
    assign_codec_name(track, codec_id);
    track->is_opus = strcmp(codec_id, "A_OPUS") == 0;
    if (type == WEBM_TRACK_TYPE_VIDEO) {
        track->public.channels = 0;
        track->public.sample_rate = 0;
    }
    if (strcmp(codec_id, "V_AV1") == 0 && track->public.codec_config &&
        g_bytes_get_size(track->public.codec_config) >= 4) {
        gsize size = g_bytes_get_size(track->public.codec_config);
        track->public.config_obus =
            g_bytes_new_from_bytes(track->public.codec_config, 4, size - 4);
    }
    *out = track;
    return TRUE;
}

/* Parses Tracks into a new track set, replacing the old one (init segment). */
static gboolean
parse_tracks(ns_webm_demuxer *d, const guint8 *payload, gsize length)
{
    GPtrArray *tracks = g_ptr_array_new_with_free_func(webm_track_free);
    element_iterator it;
    iterator_init(&it, payload, length);
    child_element child;
    int step;
    while ((step = iterator_next(&it, &child)) > 0) {
        if (child.id != WEBM_ID_TRACK_ENTRY) continue;
        webm_track *track = NULL;
        if (!parse_track_entry(d, &child, &track)) break;
        if (!track) continue;
        if (tracks->len >= WEBM_MAX_TRACKS ||
            find_track(tracks, track->public.id)) {
            webm_track_free(track);
            step = -1;
            break;
        }
        g_ptr_array_add(tracks, track);
    }
    if (step != 0) {
        g_ptr_array_unref(tracks);
        return FALSE;
    }
    resolve_all_held_blocks(d);
    g_ptr_array_unref(d->tracks);
    d->tracks = tracks;
    d->has_init = TRUE;
    return TRUE;
}

static gsize
input_available(const ns_webm_demuxer *d)
{
    return d->input->len - d->input_offset;
}

static const guint8 *
input_cursor(const ns_webm_demuxer *d)
{
    return d->input->data + d->input_offset;
}

/* Advances past count input bytes, updating the stream position. */
static void
consume_input(ns_webm_demuxer *d, gsize count)
{
    d->input_offset += count;
    d->position += count;
}

/* Discards consumed bytes from the front of the input buffer. */
static void
compact_input(ns_webm_demuxer *d)
{
    if (d->input_offset == 0) return;
    g_byte_array_remove_range(d->input, 0, (guint)d->input_offset);
    d->input_offset = 0;
}

/* 1 if the element is fully buffered, 0 if more is needed, -1 if too big/unknown. */
static int
element_ready(const ns_webm_demuxer *d, const element_header *header,
              guint64 limit)
{
    if (header->unknown_size || header->size > limit) return -1;
    if (input_available(d) - header->header_length < header->size) return 0;
    return 1;
}

/* Consumes an element header and skips its payload. */
static void
enter_skip(ns_webm_demuxer *d, const element_header *header)
{
    consume_input(d, header->header_length);
    d->skip_remaining = header->size;
}

/* True for segment-level element IDs, which end an unknown-size cluster. */
static gboolean
id_is_top_level(guint32 id)
{
    switch (id) {
    case WEBM_ID_EBML:
    case WEBM_ID_SEGMENT:
    case WEBM_ID_SEEK_HEAD:
    case WEBM_ID_INFO:
    case WEBM_ID_TRACKS:
    case WEBM_ID_CLUSTER:
    case WEBM_ID_CUES:
    case WEBM_ID_ATTACHMENTS:
    case WEBM_ID_CHAPTERS:
    case WEBM_ID_TAGS:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Leaves cluster context, flushing held blocks. */
static void
end_cluster(ns_webm_demuxer *d)
{
    resolve_all_held_blocks(d);
    d->context = CONTEXT_TOP;
    d->cluster_has_end = FALSE;
    d->cluster_end = 0;
    d->cluster_has_timecode = FALSE;
    d->cluster_timecode = 0;
}

/* Parses a fully buffered element (EBML, Info, Tracks, timecode, blocks). */
static gboolean
parse_buffered_element(ns_webm_demuxer *d, const element_header *header,
                       guint64 limit, gboolean *need_more)
{
    int ready = element_ready(d, header, limit);
    if (ready < 0) return FALSE;
    if (ready == 0) {
        *need_more = TRUE;
        return TRUE;
    }
    const guint8 *payload = input_cursor(d) + header->header_length;
    gsize length = (gsize)header->size;
    gboolean ok = FALSE;
    switch (header->id) {
    case WEBM_ID_EBML:
        ok = parse_ebml_header(payload, length);
        break;
    case WEBM_ID_INFO:
        ok = parse_info(d, payload, length);
        break;
    case WEBM_ID_TRACKS:
        ok = parse_tracks(d, payload, length);
        break;
    case WEBM_ID_CLUSTER_TIMECODE: {
        child_element element = { header->id, payload, length };
        ok = read_unsigned(&element, &d->cluster_timecode) &&
             d->cluster_timecode <= (guint64)WEBM_MAX_TIMESTAMP;
        d->cluster_has_timecode = ok;
        break;
    }
    case WEBM_ID_SIMPLE_BLOCK: {
        GBytes *block_bytes = g_bytes_new(payload, length);
        ok = handle_block(d, block_bytes, TRUE, FALSE, 0, FALSE);
        g_bytes_unref(block_bytes);
        break;
    }
    case WEBM_ID_BLOCK_GROUP:
        ok = parse_block_group(d, payload, length);
        break;
    default:
        break;
    }
    consume_input(d, header->header_length + length);
    return ok;
}

/* Handles a segment-level element: enters Segment/Cluster, parses headers, skips rest. */
static gboolean
process_top_level_element(ns_webm_demuxer *d, const element_header *header,
                          gboolean *need_more)
{
    switch (header->id) {
    case WEBM_ID_EBML:
        return parse_buffered_element(d, header, WEBM_MAX_EBML_HEADER_BYTES,
                                      need_more);
    case WEBM_ID_SEGMENT:
        consume_input(d, header->header_length);
        apply_timecode_scale(d, WEBM_DEFAULT_TIMECODE_SCALE);
        d->has_segment_duration = FALSE;
        d->segment_duration_seconds = 0.0;
        return TRUE;
    case WEBM_ID_INFO:
        return parse_buffered_element(d, header, WEBM_MAX_INFO_BYTES,
                                      need_more);
    case WEBM_ID_TRACKS:
        return parse_buffered_element(d, header, WEBM_MAX_TRACKS_BYTES,
                                      need_more);
    case WEBM_ID_CLUSTER:
        consume_input(d, header->header_length);
        d->context = CONTEXT_CLUSTER;
        d->cluster_has_end = !header->unknown_size;
        d->cluster_end = d->position + header->size;
        d->cluster_has_timecode = FALSE;
        d->cluster_timecode = 0;
        return TRUE;
    default:
        if (header->unknown_size) return FALSE;
        enter_skip(d, header);
        return TRUE;
    }
}

/* Handles an element inside a cluster, bounded by the cluster end when known. */
static gboolean
process_cluster_element(ns_webm_demuxer *d, const element_header *header,
                        gboolean *need_more)
{
    if (id_is_top_level(header->id)) {
        end_cluster(d);
        return TRUE;
    }
    if (header->unknown_size) return FALSE;
    if (d->cluster_has_end) {
        guint64 room = d->cluster_end - d->position;
        if (header->header_length > room ||
            header->size > room - header->header_length)
            return FALSE;
    }
    switch (header->id) {
    case WEBM_ID_CLUSTER_TIMECODE:
        return parse_buffered_element(d, header, 8, need_more);
    case WEBM_ID_SIMPLE_BLOCK:
        return parse_buffered_element(d, header, WEBM_MAX_BLOCK_BYTES,
                                      need_more);
    case WEBM_ID_BLOCK_GROUP:
        return parse_buffered_element(d, header, WEBM_MAX_BLOCK_GROUP_BYTES,
                                      need_more);
    default:
        enter_skip(d, header);
        return TRUE;
    }
}

/* Runs the incremental parser over buffered input until it needs more data. */
static gboolean
process_input(ns_webm_demuxer *d)
{
    for (;;) {
        if (d->skip_remaining > 0) {
            gsize count = (gsize)MIN(d->skip_remaining,
                                     (guint64)input_available(d));
            consume_input(d, count);
            d->skip_remaining -= count;
            if (d->skip_remaining > 0) return TRUE;
        }
        if (d->context == CONTEXT_CLUSTER && d->cluster_has_end &&
            d->position >= d->cluster_end) {
            end_cluster(d);
            continue;
        }
        element_header header;
        header_result result =
            read_element_header(input_cursor(d), input_available(d), &header);
        if (result == HEADER_NEED_MORE) return TRUE;
        if (result == HEADER_INVALID) return FALSE;
        gboolean need_more = FALSE;
        gboolean ok = d->context == CONTEXT_CLUSTER
            ? process_cluster_element(d, &header, &need_more)
            : process_top_level_element(d, &header, &need_more);
        if (!ok) return FALSE;
        if (need_more) return TRUE;
    }
}

static void
clear_parser_state(ns_webm_demuxer *d)
{
    g_byte_array_set_size(d->input, 0);
    d->input_offset = 0;
    d->position = 0;
    d->error = FALSE;
    d->skip_remaining = 0;
    d->context = CONTEXT_TOP;
    d->cluster_has_end = FALSE;
    d->cluster_end = 0;
    d->cluster_has_timecode = FALSE;
    d->cluster_timecode = 0;
}

/* Puts the demuxer in a permanent error state, dropping buffered input. */
static void
enter_error_state(ns_webm_demuxer *d)
{
    d->error = TRUE;
    g_byte_array_set_size(d->input, 0);
    d->input_offset = 0;
    d->skip_remaining = 0;
    drop_all_held_blocks(d);
}

ns_webm_demuxer *
ns_webm_demuxer_new(void)
{
    ns_webm_demuxer *d = g_new0(ns_webm_demuxer, 1);
    d->input = g_byte_array_new();
    d->tracks = g_ptr_array_new_with_free_func(webm_track_free);
    d->ready = g_queue_new();
    d->timecode_scale = WEBM_DEFAULT_TIMECODE_SCALE;
    clear_parser_state(d);
    return d;
}

void
ns_webm_demuxer_free(ns_webm_demuxer *d)
{
    if (!d) return;
    g_byte_array_unref(d->input);
    g_ptr_array_unref(d->tracks);
    g_queue_free_full(d->ready, ns_mp4_sample_free);
    g_free(d);
}

/* Feeds bytes to the demuxer, fast-skipping ignored payloads; FALSE on error. */
gboolean
ns_webm_demuxer_append(ns_webm_demuxer *d, const guint8 *data, gsize len)
{
    if (!d || d->error) return FALSE;
    if (len > 0 && !data) return FALSE;
    while (len > 0) {
        if (d->skip_remaining > 0 && input_available(d) == 0) {
            gsize skipped = (gsize)MIN(d->skip_remaining, (guint64)len);
            d->skip_remaining -= skipped;
            d->position += skipped;
            data += skipped;
            len -= skipped;
            continue;
        }
        guint chunk = (guint)MIN(len, (gsize)WEBM_MAX_APPEND_CHUNK_BYTES);
        g_byte_array_append(d->input, data, chunk);
        data += chunk;
        len -= chunk;
        gboolean ok = process_input(d);
        compact_input(d);
        if (!ok) {
            enter_error_state(d);
            return FALSE;
        }
    }
    return TRUE;
}

gboolean
ns_webm_demuxer_has_init(const ns_webm_demuxer *d)
{
    return d && d->has_init;
}

guint
ns_webm_demuxer_n_tracks(const ns_webm_demuxer *d)
{
    return d && d->has_init ? d->tracks->len : 0;
}

const ns_mp4_track *
ns_webm_demuxer_track(const ns_webm_demuxer *d, guint index)
{
    if (!d || !d->has_init || index >= d->tracks->len) return NULL;
    webm_track *track = g_ptr_array_index(d->tracks, index);
    return &track->public;
}

/* Pops the next demuxed sample; caller owns out->data. */
gboolean
ns_webm_demuxer_pop_sample(ns_webm_demuxer *d, ns_mp4_sample *out)
{
    if (!d || !out) return FALSE;
    ns_mp4_sample *sample = g_queue_pop_head(d->ready);
    if (!sample) return FALSE;
    *out = *sample;
    g_free(sample);
    return TRUE;
}

/* Emits any blocks held for duration resolution (end of stream). */
void
ns_webm_demuxer_flush(ns_webm_demuxer *d)
{
    if (!d || d->error) return;
    resolve_all_held_blocks(d);
}

/* Resets parsing at a discontinuity, flushing held blocks first. */
void
ns_webm_demuxer_reset_parser(ns_webm_demuxer *d)
{
    if (!d) return;
    if (!d->error) resolve_all_held_blocks(d);
    clear_parser_state(d);
}

/* Segment duration in seconds, or -1 when not declared. */
double
ns_webm_demuxer_duration_seconds(const ns_webm_demuxer *d)
{
    if (!d || !d->has_segment_duration) return -1.0;
    return d->segment_duration_seconds;
}
