/* Northstar Web Browser — video decoding through GStreamer decoder plugins (the Linux default).
 * Each decoder is a small pipeline (pad → [parser] → decoder → videoconvert → appsink) fed
 * synchronously from the video worker thread: a buffer pushed into it has been decoded,
 * and its picture handed to the appsink, by the time the push returns.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gstvideo.h"

#ifdef NS_HAVE_GSTREAMER

#include <string.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "yuv.h"

/* GstClockTime is unsigned; MSE timestamps may be negative after a
 * timestampOffset, so every buffer is shifted by this much. */
static const gint64 NS_GST_PTS_BIAS_US = G_GINT64_CONSTANT(36000) * G_USEC_PER_SEC;

enum { NS_GST_MAX_LATE = 16 };

typedef struct {
    const char *codec;
    const char *caps;
    const char *parser;
    gboolean    codec_data;
} ns_gst_codec;

static const ns_gst_codec g_codecs[] = {
    { "av01", "video/x-av1, stream-format=(string)obu-stream, alignment=(string)tu",
      "av1parse", FALSE },
    { "vp09", "video/x-vp9", "vp9parse", FALSE },
    { "avc1", "video/x-h264, stream-format=(string)avc, alignment=(string)au",
      "h264parse", TRUE },
    { "avc3", "video/x-h264, stream-format=(string)avc3, alignment=(string)au",
      "h264parse", TRUE },
};

/* The decoder (and, when it cannot take the container's bitstream as is,
 * the parser in front of it) picked for each codec before the sandbox. */
typedef struct {
    GstElementFactory *decoder;
    GstElementFactory *parser;
} ns_gst_choice;

static ns_gst_choice g_choices[G_N_ELEMENTS(g_codecs)];
static gboolean      g_preinit_done;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define NS_GST_PLANAR_FORMATS \
    "{ I420, YV12, Y42B, Y444, I420_10LE, I422_10LE, Y444_10LE, " \
    "I420_12LE, I422_12LE, Y444_12LE, GRAY8, GRAY16_LE }"
#else
#define NS_GST_PLANAR_FORMATS \
    "{ I420, YV12, Y42B, Y444, I420_10BE, I422_10BE, Y444_10BE, " \
    "I420_12BE, I422_12BE, Y444_12BE, GRAY8, GRAY16_BE }"
#endif

typedef struct {
    gint64      timestamp;
    ns_texture *texture;
} ns_gst_late;

struct ns_gst_video_decoder {
    const ns_gst_codec *codec;
    GstElement         *pipeline;
    GstElement         *decoder;
    GstAppSink         *sink;
    GstPad             *src;
    GBytes             *config;
    gboolean            caps_sent;
    gboolean            broken;
    GHashTable         *wanted;
    GQueue              late;
};

static const ns_gst_codec *
find_codec(const char *codec, gsize *index)
{
    if (!codec) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(g_codecs); i++)
        if (strcmp(g_codecs[i].codec, codec) == 0) {
            if (index) *index = i;
            return &g_codecs[i];
        }
    return NULL;
}

/* Hardware decoders need GPU device access the sandbox does not grant
 * every mode, and load their drivers late; they are opt-in. */
static gboolean
factory_allowed(GstElementFactory *factory)
{
    const char *klass =
        gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    if (klass && strstr(klass, "Hardware") && !g_getenv("NS_GST_HARDWARE"))
        return FALSE;
    /* vp9alphadecodebin and friends want a separate alpha stream. */
    return strstr(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)),
                  "alpha") == NULL;
}

/* Returns the highest-ranked loaded factory of type whose sink takes caps (ref'd). */
static GstElementFactory *
best_factory(GstElementFactoryListType type, GstCaps *caps)
{
    GList *all = gst_element_factory_list_get_elements(type, GST_RANK_MARGINAL);
    GList *fit = gst_element_factory_list_filter(all, caps, GST_PAD_SINK, FALSE);
    fit = g_list_sort(fit, gst_plugin_feature_rank_compare_func);
    GstElementFactory *chosen = NULL;
    for (GList *l = fit; l && !chosen; l = l->next) {
        GstElementFactory *factory = l->data;
        if (!factory_allowed(factory)) continue;
        GstPluginFeature *loaded = gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory));
        if (loaded) chosen = GST_ELEMENT_FACTORY(loaded);
    }
    gst_plugin_feature_list_free(fit);
    gst_plugin_feature_list_free(all);
    return chosen;
}

/* Returns the src template caps of a factory, or NULL. */
static GstCaps *
factory_src_caps(GstElementFactory *factory)
{
    for (const GList *l = gst_element_factory_get_static_pad_templates(factory);
         l; l = l->next) {
        GstStaticPadTemplate *templ = l->data;
        if (templ->direction == GST_PAD_SRC)
            return gst_static_pad_template_get_caps(templ);
    }
    return NULL;
}

/* Picks a decoder for one codec, with its parser in front when needed. */
static void
choose(const ns_gst_codec *codec, ns_gst_choice *choice)
{
    GstCaps *caps = gst_caps_from_string(codec->caps);
    choice->decoder = best_factory(GST_ELEMENT_FACTORY_TYPE_DECODER |
                                   GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, caps);
    if (!choice->decoder && codec->parser) {
        GstElementFactory *parser = gst_element_factory_find(codec->parser);
        GstPluginFeature *loaded =
            parser ? gst_plugin_feature_load(GST_PLUGIN_FEATURE(parser)) : NULL;
        if (parser) gst_object_unref(parser);
        GstCaps *parsed = loaded ? factory_src_caps(GST_ELEMENT_FACTORY(loaded)) : NULL;
        if (parsed) {
            /* Parsers advertise every stream-format they convert to;
             * keep the first decoder that takes any of them. */
            for (guint i = 0; i < gst_caps_get_size(parsed) && !choice->decoder; i++) {
                GstStructure *s = gst_caps_get_structure(parsed, i);
                const GValue *formats = gst_structure_get_value(s, "stream-format");
                guint n = formats && GST_VALUE_HOLDS_LIST(formats)
                    ? gst_value_list_get_size(formats) : 1;
                for (guint f = 0; f < n && !choice->decoder; f++) {
                    GstCaps *one = gst_caps_new_empty_simple(gst_structure_get_name(s));
                    if (formats) {
                        const GValue *v = GST_VALUE_HOLDS_LIST(formats)
                            ? gst_value_list_get_value(formats, f) : formats;
                        gst_caps_set_value(one, "stream-format", v);
                    }
                    const GValue *align = gst_structure_get_value(s, "alignment");
                    if (align && G_VALUE_HOLDS_STRING(align))
                        gst_caps_set_value(one, "alignment", align);
                    else if (align && GST_VALUE_HOLDS_LIST(align))
                        gst_caps_set_simple(one, "alignment", G_TYPE_STRING,
                                            strcmp(codec->codec, "av01") == 0 ? "tu" : "au",
                                            NULL);
                    choice->decoder = best_factory(
                        GST_ELEMENT_FACTORY_TYPE_DECODER |
                        GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, one);
                    gst_caps_unref(one);
                }
            }
            gst_caps_unref(parsed);
        }
        if (choice->decoder) choice->parser = GST_ELEMENT_FACTORY(loaded);
        else if (loaded) gst_object_unref(loaded);
    }
    gst_caps_unref(caps);
    if (choice->decoder)
        g_debug("gstvideo: %s → %s%s%s", codec->codec,
                choice->parser ? GST_OBJECT_NAME(choice->parser) : "",
                choice->parser ? " ! " : "", GST_OBJECT_NAME(choice->decoder));
}

static gboolean
native_forced(void)
{
    const char *backend = g_getenv("NS_VIDEO_DECODER");
    return backend && g_ascii_strcasecmp(backend, "native") == 0;
}

static ns_gst_video_decoder *build(const ns_gst_codec *codec,
                                   const ns_gst_choice *choice);

void
ns_gst_video_preinit(void)
{
    if (g_preinit_done) return;
    g_preinit_done = TRUE;
    if (native_forced()) return;
    /* Orc compiles its kernels into executable memory, which the sandbox
     * refuses; its portable C fallbacks need nothing of the kind. */
    g_setenv("ORC_CODE", "backup", FALSE);
    GError *error = NULL;
    if (!gst_init_check(NULL, NULL, &error)) {
        g_info("gstvideo: GStreamer unavailable: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    static const char *const helpers[] = { "videoconvert", "appsink", NULL };
    for (gsize i = 0; helpers[i]; i++) {
        GstElementFactory *factory = gst_element_factory_find(helpers[i]);
        GstPluginFeature *loaded =
            factory ? gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory)) : NULL;
        if (factory) gst_object_unref(factory);
        if (!loaded) {
            g_info("gstvideo: missing the %s element; GStreamer video is off",
                   helpers[i]);
            return;
        }
        gst_object_unref(loaded);
    }
    for (gsize i = 0; i < G_N_ELEMENTS(g_codecs); i++) {
        choose(&g_codecs[i], &g_choices[i]);
        /* Instantiate each pipeline once now, so libraries an element
         * opens lazily are mapped while dlopen still works. */
        ns_gst_video_decoder *warm = build(&g_codecs[i], &g_choices[i]);
        if (!warm) {
            g_clear_object(&g_choices[i].decoder);
            g_clear_object(&g_choices[i].parser);
        }
        ns_gst_video_decoder_free(warm);
    }
}

gboolean
ns_gst_video_available(const char *codec)
{
    gsize i = 0;
    if (!find_codec(codec, &i)) return FALSE;
    return g_choices[i].decoder != NULL;
}

/* Sets a property only when the element has it, from its string form. */
static void
maybe_set(GstElement *element, const char *name, const char *value)
{
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), name))
        gst_util_set_object_arg(G_OBJECT(element), name, value);
}

/* Keeps a decoder synchronous: one coded frame in, its picture out before
 * the push returns, as the worker's pts-matching contract expects. */
static void
configure_decoder(GstElement *decoder)
{
    maybe_set(decoder, "max-errors", "-1");
    maybe_set(decoder, "thread-type", "slice");
    maybe_set(decoder, "max-frame-delay", "1");
    maybe_set(decoder, "output-corrupt", "false");
    maybe_set(decoder, "qos", "false");
}

static ns_gst_video_decoder *
build(const ns_gst_codec *codec, const ns_gst_choice *choice)
{
    if (!choice->decoder) return NULL;
    GstElement *pipeline = gst_pipeline_new(NULL);
    GstElement *parser = choice->parser ? gst_element_factory_create(choice->parser, NULL)
                                        : NULL;
    GstElement *decoder = gst_element_factory_create(choice->decoder, NULL);
    GstElement *convert = gst_element_factory_make("videoconvert", NULL);
    GstElement *sink = gst_element_factory_make("appsink", NULL);
    if (!pipeline || !decoder || !convert || !sink || (choice->parser && !parser)) {
        if (parser) gst_object_unref(parser);
        if (decoder) gst_object_unref(decoder);
        if (convert) gst_object_unref(convert);
        if (sink) gst_object_unref(sink);
        if (pipeline) gst_object_unref(pipeline);
        return NULL;
    }
    configure_decoder(decoder);
    GstCaps *raw = gst_caps_from_string("video/x-raw, format=(string)"
                                        NS_GST_PLANAR_FORMATS);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "qos", FALSE,
                 "enable-last-sample", FALSE, "max-buffers", 0u, "drop", FALSE,
                 "emit-signals", FALSE, "caps", raw, NULL);
    gst_caps_unref(raw);
    gst_bin_add_many(GST_BIN(pipeline), decoder, convert, sink, NULL);
    if (parser) gst_bin_add(GST_BIN(pipeline), parser);
    gboolean linked = gst_element_link_many(decoder, convert, sink, NULL) &&
                      (!parser || gst_element_link(parser, decoder));
    /* Nothing listens on the bus; flushing it keeps messages from piling up. */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_set_flushing(bus, TRUE);
    gst_object_unref(bus);

    ns_gst_video_decoder *self = g_new0(ns_gst_video_decoder, 1);
    self->codec = codec;
    self->pipeline = pipeline;
    self->decoder = decoder;
    self->sink = GST_APP_SINK(sink);
    self->wanted = g_hash_table_new(g_int64_hash, g_int64_equal);
    g_queue_init(&self->late);
    self->src = gst_pad_new("src", GST_PAD_SRC);
    GstPad *first = gst_element_get_static_pad(parser ? parser : decoder, "sink");
    linked = linked && first && gst_pad_link(self->src, first) == GST_PAD_LINK_OK;
    if (first) gst_object_unref(first);
    if (!linked || !gst_pad_set_active(self->src, TRUE) ||
        gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
        ns_gst_video_decoder_free(self);
        return NULL;
    }
    return self;
}

ns_gst_video_decoder *
ns_gst_video_decoder_new(const char *codec)
{
    gsize i = 0;
    const ns_gst_codec *row = find_codec(codec, &i);
    if (!row) return NULL;
    ns_gst_video_decoder *decoder = build(row, &g_choices[i]);
    if (decoder)
        g_debug("gstvideo: new %s decoder (%s)", row->codec,
                GST_OBJECT_NAME(g_choices[i].decoder));
    return decoder;
}

static void
late_clear(ns_gst_video_decoder *self)
{
    ns_gst_late *late;
    while ((late = g_queue_pop_head(&self->late)) != NULL) {
        ns_texture_unref(late->texture);
        g_free(late);
    }
}

static void
wanted_clear(ns_gst_video_decoder *self)
{
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, self->wanted);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        g_hash_table_iter_remove(&iter);
        g_free(key);
    }
}

void
ns_gst_video_decoder_free(ns_gst_video_decoder *decoder)
{
    if (!decoder) return;
    if (decoder->src) {
        gst_pad_set_active(decoder->src, FALSE);
        gst_object_unref(decoder->src);
    }
    if (decoder->pipeline) {
        gst_element_set_state(decoder->pipeline, GST_STATE_NULL);
        gst_object_unref(decoder->pipeline);
    }
    if (decoder->config) g_bytes_unref(decoder->config);
    late_clear(decoder);
    if (decoder->wanted) {
        wanted_clear(decoder);
        g_hash_table_unref(decoder->wanted);
    }
    g_free(decoder);
}

static void
push_segment(ns_gst_video_decoder *self)
{
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    gst_pad_push_event(self->src, gst_event_new_segment(&segment));
}

/* Sends stream-start, caps (with the avcC as codec_data) and a segment. */
static gboolean
send_caps(ns_gst_video_decoder *self, GBytes *config)
{
    if (!self->caps_sent) {
        gchar *id = g_strdup_printf("ns-video-%p", (void *)self);
        gst_pad_push_event(self->src, gst_event_new_stream_start(id));
        g_free(id);
    }
    GstCaps *caps = gst_caps_from_string(self->codec->caps);
    if (config && self->codec->codec_data) {
        GstBuffer *data = gst_buffer_new_wrapped_bytes(config);
        gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, data, NULL);
        gst_buffer_unref(data);
    }
    gboolean ok = gst_pad_push_event(self->src, gst_event_new_caps(caps));
    gst_caps_unref(caps);
    if (!self->caps_sent) push_segment(self);
    self->caps_sent = TRUE;
    if (self->config) g_bytes_unref(self->config);
    self->config = config ? g_bytes_ref(config) : NULL;
    return ok;
}

/* Converts a decoded planar YUV sample to a BGRA texture. */
static ns_texture *
sample_to_texture(GstSample *sample)
{
    GstVideoInfo info;
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) return NULL;
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return NULL;
    const GstVideoFormatInfo *finfo = info.finfo;
    gboolean gray = GST_VIDEO_FORMAT_INFO_N_COMPONENTS(finfo) < 3;
    ns_yuv_picture yuv = {
        .planes = {
            GST_VIDEO_FRAME_COMP_DATA(&frame, 0),
            gray ? NULL : GST_VIDEO_FRAME_COMP_DATA(&frame, 1),
            gray ? NULL : GST_VIDEO_FRAME_COMP_DATA(&frame, 2),
        },
        .strides = {
            GST_VIDEO_FRAME_COMP_STRIDE(&frame, 0),
            gray ? 0 : GST_VIDEO_FRAME_COMP_STRIDE(&frame, 1),
            gray ? 0 : GST_VIDEO_FRAME_COMP_STRIDE(&frame, 2),
        },
        .width = GST_VIDEO_INFO_WIDTH(&info),
        .height = GST_VIDEO_INFO_HEIGHT(&info),
        .bits = GST_VIDEO_FORMAT_INFO_DEPTH(finfo, 0),
        .sub_x = gray ? 0 : GST_VIDEO_FORMAT_INFO_W_SUB(finfo, 1),
        .sub_y = gray ? 0 : GST_VIDEO_FORMAT_INFO_H_SUB(finfo, 1),
        .monochrome = gray,
        .bt601 = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601,
        .full_range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255,
    };
    if (yuv.bits > 8) yuv.bits = MAX(yuv.bits, 9);
    ns_texture *texture = ns_yuv_to_texture(&yuv);
    gst_video_frame_unmap(&frame);
    return texture;
}

/* Collects pictures the decoder has handed the sink; returns the one for timestamp. */
static ns_texture *
collect(ns_gst_video_decoder *self, gint64 timestamp)
{
    ns_texture *result = NULL;
    GstSample *sample;
    while ((sample = gst_app_sink_try_pull_sample(self->sink, 0)) != NULL) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstClockTime pts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
        if (!GST_CLOCK_TIME_IS_VALID(pts)) {
            gst_sample_unref(sample);
            continue;
        }
        gint64 stamp = (gint64)(pts / GST_USECOND) - NS_GST_PTS_BIAS_US;
        gpointer key = NULL;
        if (g_hash_table_lookup_extended(self->wanted, &stamp, &key, NULL)) {
            g_hash_table_remove(self->wanted, &stamp);
            g_free(key);
            ns_texture *texture = sample_to_texture(sample);
            if (stamp == timestamp && !result) {
                result = texture;
            } else if (texture) {
                ns_gst_late *late = g_new(ns_gst_late, 1);
                late->timestamp = stamp;
                late->texture = texture;
                g_queue_push_tail(&self->late, late);
                while (g_queue_get_length(&self->late) > NS_GST_MAX_LATE) {
                    ns_gst_late *old = g_queue_pop_head(&self->late);
                    ns_texture_unref(old->texture);
                    g_free(old);
                }
            }
        }
        gst_sample_unref(sample);
    }
    return result;
}

/* Drops decoder state after a seek; the next sample starts at a keyframe. */
void
ns_gst_video_decoder_flush(ns_gst_video_decoder *decoder)
{
    if (!decoder || !decoder->caps_sent) return;
    gst_pad_push_event(decoder->src, gst_event_new_flush_start());
    gst_pad_push_event(decoder->src, gst_event_new_flush_stop(TRUE));
    push_segment(decoder);
    GstSample *sample;
    while ((sample = gst_app_sink_try_pull_sample(decoder->sink, 0)) != NULL)
        gst_sample_unref(sample);
    late_clear(decoder);
    wanted_clear(decoder);
    decoder->broken = FALSE;
}

/* Decodes one sample; returns the picture for timestamp when wanted and ready. */
ns_texture *
ns_gst_video_decoder_decode(ns_gst_video_decoder *decoder, GBytes *sample,
                            GBytes *config, gint64 timestamp,
                            gboolean want_texture)
{
    if (!decoder || !sample || decoder->broken) return NULL;
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(sample, &len);
    if (len == 0 || timestamp < -NS_GST_PTS_BIAS_US) return NULL;
    gboolean new_config = !decoder->caps_sent ||
        (decoder->codec->codec_data && config &&
         !(decoder->config && g_bytes_equal(config, decoder->config)));
    if (new_config && !send_caps(decoder, config)) {
        decoder->broken = TRUE;
        return NULL;
    }
    GstBuffer *buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY, (gpointer)data, len, 0, len,
        g_bytes_ref(sample), (GDestroyNotify)g_bytes_unref);
    GST_BUFFER_PTS(buffer) = (GstClockTime)(timestamp + NS_GST_PTS_BIAS_US) * GST_USECOND;
    if (want_texture) {
        gint64 *key = g_new(gint64, 1);
        *key = timestamp;
        if (!g_hash_table_add(decoder->wanted, key)) g_free(key);
    }
    GstFlowReturn flow = gst_pad_push(decoder->src, buffer);
    if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING) {
        g_info("gstvideo: %s decoder stopped: %s", decoder->codec->codec,
               gst_flow_get_name(flow));
        if (flow <= GST_FLOW_NOT_NEGOTIATED) decoder->broken = TRUE;
    }
    ns_texture *texture = collect(decoder, timestamp);
    if (!want_texture && texture) {
        ns_texture_unref(texture);
        texture = NULL;
    }
    return texture;
}

/* Hands back one picture that came out for an earlier timestamp (decoder reordering). */
ns_texture *
ns_gst_video_decoder_take_late(ns_gst_video_decoder *decoder, gint64 *timestamp)
{
    if (!decoder) return NULL;
    ns_gst_late *late = g_queue_pop_head(&decoder->late);
    if (!late) return NULL;
    ns_texture *texture = late->texture;
    if (timestamp) *timestamp = late->timestamp;
    g_free(late);
    return texture;
}

#else

void
ns_gst_video_preinit(void)
{
}

gboolean
ns_gst_video_available(const char *codec)
{
    (void)codec;
    return FALSE;
}

ns_gst_video_decoder *
ns_gst_video_decoder_new(const char *codec)
{
    (void)codec;
    return NULL;
}

void
ns_gst_video_decoder_free(ns_gst_video_decoder *decoder)
{
    (void)decoder;
}

void
ns_gst_video_decoder_flush(ns_gst_video_decoder *decoder)
{
    (void)decoder;
}

ns_texture *
ns_gst_video_decoder_decode(ns_gst_video_decoder *decoder, GBytes *sample,
                            GBytes *config, gint64 timestamp,
                            gboolean want_texture)
{
    (void)decoder;
    (void)sample;
    (void)config;
    (void)timestamp;
    (void)want_texture;
    return NULL;
}

ns_texture *
ns_gst_video_decoder_take_late(ns_gst_video_decoder *decoder, gint64 *timestamp)
{
    (void)decoder;
    (void)timestamp;
    return NULL;
}

#endif
