/* Northstar Web Browser — H.264 video decoding over OpenH264, to BGRA textures.
 * Converts MP4 avcC config and length-prefixed samples to Annex B before decoding.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "h264.h"

#ifdef NS_HAVE_H264

#include <string.h>

#include <wels/codec_api.h>

#include "yuv.h"

static const guint8 NS_H264_START_CODE[4] = { 0, 0, 0, 1 };

struct ns_h264_decoder {
    ISVCDecoder *codec;
    GByteArray  *access_unit;
    GBytes      *config;
    int          length_size;
};

/* Creates a quiet, single-threaded OpenH264 AVC decoder; NULL on failure. */
static ISVCDecoder *
open_codec(void)
{
    ISVCDecoder *codec = NULL;
    if (WelsCreateDecoder(&codec) != 0 || !codec) return NULL;
    int log_level = WELS_LOG_QUIET;
    (*codec)->SetOption(codec, DECODER_OPTION_TRACE_LEVEL, &log_level);
    int threads = 1;
    (*codec)->SetOption(codec, DECODER_OPTION_NUM_OF_THREADS, &threads);
    SDecodingParam param;
    memset(&param, 0, sizeof param);
    param.sVideoProperty.size = sizeof param.sVideoProperty;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    if ((*codec)->Initialize(codec, &param) != 0) {
        WelsDestroyDecoder(codec);
        return NULL;
    }
    return codec;
}

/* Uninitializes and destroys an OpenH264 decoder. */
static void
close_codec(ISVCDecoder *codec)
{
    if (!codec) return;
    (*codec)->Uninitialize(codec);
    WelsDestroyDecoder(codec);
}

/* Appends avcC SPS/PPS as Annex B NAL units and reads the NAL length size. */
static gboolean
append_parameter_sets(GByteArray *out, GBytes *config, int *length_size)
{
    gsize len = 0;
    const guint8 *avcc = g_bytes_get_data(config, &len);
    if (len < 7 || avcc[0] != 1) return FALSE;
    *length_size = (avcc[4] & 3) + 1;
    gsize pos = 5;
    for (int group = 0; group < 2; group++) {
        if (pos >= len) return FALSE;
        guint count = group == 0 ? avcc[pos] & 0x1F : avcc[pos];
        pos++;
        for (guint i = 0; i < count; i++) {
            if (len - pos < 2) return FALSE;
            gsize size = (gsize)avcc[pos] << 8 | avcc[pos + 1];
            pos += 2;
            if (size > len - pos) return FALSE;
            g_byte_array_append(out, NS_H264_START_CODE, sizeof NS_H264_START_CODE);
            g_byte_array_append(out, avcc + pos, (guint)size);
            pos += size;
        }
    }
    return TRUE;
}

/* Converts a length-prefixed MP4 sample into Annex B start-code NAL units. */
static gboolean
append_nal_units(GByteArray *out, GBytes *sample, int length_size)
{
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(sample, &len);
    gsize pos = 0;
    while (pos < len) {
        if (len - pos < (gsize)length_size) return FALSE;
        gsize size = 0;
        for (int i = 0; i < length_size; i++) size = size << 8 | data[pos++];
        if (size > len - pos) return FALSE;
        if (size == 0) continue;
        g_byte_array_append(out, NS_H264_START_CODE, sizeof NS_H264_START_CODE);
        g_byte_array_append(out, data + pos, (guint)size);
        pos += size;
    }
    return TRUE;
}

/* Converts a decoded 4:2:0 picture to a BGRA texture (BT.601 below 720p). */
static ns_texture *
picture_to_texture(const SBufferInfo *info, unsigned char *const planes[3])
{
    const SSysMEMBuffer *picture = &info->UsrData.sSystemBuffer;
    ns_yuv_picture yuv = {
        .planes = { planes[0], planes[1], planes[2] },
        .strides = { picture->iStride[0], picture->iStride[1], picture->iStride[1] },
        .width = picture->iWidth,
        .height = picture->iHeight,
        .bits = 8,
        .sub_x = 1,
        .sub_y = 1,
        .monochrome = FALSE,
        .bt601 = picture->iHeight < 720,
        .full_range = FALSE,
    };
    return ns_yuv_to_texture(&yuv);
}

/* Flushes the next frame still buffered in the decoder; FALSE when none remain. */
static gboolean
next_buffered_picture(ISVCDecoder *codec, SBufferInfo *info,
                      unsigned char *planes[3])
{
    int remaining = 0;
    (*codec)->GetOption(codec, DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER,
                        &remaining);
    if (remaining <= 0) return FALSE;
    int end_of_stream = 1;
    (*codec)->SetOption(codec, DECODER_OPTION_END_OF_STREAM, &end_of_stream);
    memset(info, 0, sizeof *info);
    planes[0] = planes[1] = planes[2] = NULL;
    (*codec)->FlushFrame(codec, planes, info);
    return info->iBufferStatus == 1;
}

gboolean
ns_h264_available(void)
{
    return TRUE;
}

/* Creates an H.264 decoder wrapper; NULL when OpenH264 is unavailable. */
ns_h264_decoder *
ns_h264_decoder_new(void)
{
    ISVCDecoder *codec = open_codec();
    if (!codec) return NULL;
    ns_h264_decoder *decoder = g_new0(ns_h264_decoder, 1);
    decoder->codec = codec;
    decoder->access_unit = g_byte_array_new();
    decoder->length_size = 4;
    return decoder;
}

void
ns_h264_decoder_free(ns_h264_decoder *decoder)
{
    if (!decoder) return;
    close_codec(decoder->codec);
    g_byte_array_free(decoder->access_unit, TRUE);
    if (decoder->config) g_bytes_unref(decoder->config);
    g_free(decoder);
}

/* Recreates the codec and forgets the cached avcC config. */
void
ns_h264_decoder_flush(ns_h264_decoder *decoder)
{
    if (!decoder) return;
    close_codec(decoder->codec);
    decoder->codec = open_codec();
    g_clear_pointer(&decoder->config, g_bytes_unref);
}

/* Decodes one sample; returns a texture only for the frame matching timestamp. */
ns_texture *
ns_h264_decoder_decode(ns_h264_decoder *decoder, GBytes *sample, GBytes *config,
                       gint64 timestamp, gboolean want_texture)
{
    if (!decoder || !decoder->codec || !sample) return NULL;
    GByteArray *unit = decoder->access_unit;
    g_byte_array_set_size(unit, 0);
    gboolean new_config = config && !(decoder->config &&
                                      g_bytes_equal(config, decoder->config));
    if (new_config &&
        !append_parameter_sets(unit, config, &decoder->length_size))
        return NULL;
    if (!append_nal_units(unit, sample, decoder->length_size) || unit->len == 0 ||
        unit->len > G_MAXINT)
        return NULL;
    if (new_config) {
        if (decoder->config) g_bytes_unref(decoder->config);
        decoder->config = g_bytes_ref(config);
    }
    SBufferInfo info;
    memset(&info, 0, sizeof info);
    info.uiInBsTimeStamp = (unsigned long long)timestamp;
    unsigned char *planes[3] = { NULL, NULL, NULL };
    (*decoder->codec)->DecodeFrameNoDelay(decoder->codec, unit->data,
                                          (int)unit->len, planes, &info);
    ns_texture *texture = NULL;
    do {
        if (info.iBufferStatus == 1 && want_texture && !texture &&
            (gint64)info.uiOutYuvTimeStamp == timestamp)
            texture = picture_to_texture(&info, planes);
    } while (next_buffered_picture(decoder->codec, &info, planes));
    return texture;
}

#else

gboolean
ns_h264_available(void)
{
    return FALSE;
}

ns_h264_decoder *
ns_h264_decoder_new(void)
{
    return NULL;
}

void
ns_h264_decoder_free(ns_h264_decoder *decoder)
{
    (void)decoder;
}

void
ns_h264_decoder_flush(ns_h264_decoder *decoder)
{
    (void)decoder;
}

ns_texture *
ns_h264_decoder_decode(ns_h264_decoder *decoder, GBytes *sample, GBytes *config,
                       gint64 timestamp, gboolean want_texture)
{
    (void)decoder;
    (void)sample;
    (void)config;
    (void)timestamp;
    (void)want_texture;
    return NULL;
}

#endif
