/* Northstar Web Browser — video decoding through Windows Media Foundation transforms (the Windows default).
 * Each decoder is one synchronous IMFTransform from the system's codecs (the H.264 decoder that
 * ships with Windows, and the AV1 and VP9 Video Extensions where installed), fed one coded frame
 * at a time from the video worker thread. mfplat.dll loads at runtime, so Windows N editions
 * without the Media Feature Pack still start and fall back to the bundled decoders.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfvideo.h"

#ifdef NS_HAVE_MEDIA_FOUNDATION

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <string.h>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>

#include "yuv.h"

enum { NS_MF_MAX_LATE = 16, NS_MF_MAX_OUTPUTS = 32 };

static const guint8 NS_MF_START_CODE[4] = { 0, 0, 0, 1 };

/* MF_LOW_LATENCY; mfapi.h declares it only for WINVER >= 0x0602. */
static const GUID NS_MF_LOW_LATENCY = {
    0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee }
};

typedef struct {
    const char *codec;
    const GUID *subtype;
    gboolean    avc;
} ns_mf_codec;

static const ns_mf_codec g_codecs[] = {
    { "av01", &MFVideoFormat_AV1, FALSE },
    { "vp09", &MFVideoFormat_VP90, FALSE },
    { "avc1", &MFVideoFormat_H264, TRUE },
    { "avc3", &MFVideoFormat_H264, TRUE },
};

typedef HRESULT (WINAPI *ns_mf_startup_fn)(ULONG, DWORD);
typedef HRESULT (WINAPI *ns_mf_enum_fn)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
                                        const MFT_REGISTER_TYPE_INFO *,
                                        IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *ns_mf_media_type_fn)(IMFMediaType **);
typedef HRESULT (WINAPI *ns_mf_sample_fn)(IMFSample **);
typedef HRESULT (WINAPI *ns_mf_buffer_fn)(DWORD, IMFMediaBuffer **);

static struct {
    ns_mf_startup_fn    startup;
    ns_mf_enum_fn       enum_ex;
    ns_mf_media_type_fn create_media_type;
    ns_mf_sample_fn     create_sample;
    ns_mf_buffer_fn     create_memory_buffer;
} g_mf;

static gboolean g_preinit_done;
static gboolean g_mf_ok;
static gboolean g_available[G_N_ELEMENTS(g_codecs)];

typedef struct {
    gint64      timestamp;
    ns_texture *texture;
} ns_mf_late;

struct ns_mf_video_decoder {
    const ns_mf_codec *codec;
    IMFTransform      *mft;
    GBytes            *config;
    GByteArray        *unit;
    int                length_size;
    gboolean           need_param_sets;
    gboolean           output_ready;
    GUID               out_subtype;
    int                frame_width;
    int                frame_height;
    int                width;
    int                height;
    gboolean           full_range;
    gboolean           bt601;
    DWORD              out_buf_size;
    gboolean           provides_samples;
    gboolean           broken;
    guint8            *chroma;
    gsize              chroma_size;
    GHashTable        *wanted;
    GQueue             late;
};

static gboolean
native_forced(void)
{
    const char *backend = g_getenv("NS_VIDEO_DECODER");
    return backend && g_ascii_strcasecmp(backend, "native") == 0;
}

/* Joins the calling thread to the multithreaded apartment once. */
static gboolean
com_ready(void)
{
    static GPrivate joined;
    if (g_private_get(&joined)) return TRUE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return FALSE;
    g_private_set(&joined, GINT_TO_POINTER(1));
    return TRUE;
}

static gboolean
mf_load(void)
{
    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
    if (!mfplat) return FALSE;
    g_mf.startup = (ns_mf_startup_fn)(void *)GetProcAddress(mfplat, "MFStartup");
    g_mf.enum_ex = (ns_mf_enum_fn)(void *)GetProcAddress(mfplat, "MFTEnumEx");
    g_mf.create_media_type =
        (ns_mf_media_type_fn)(void *)GetProcAddress(mfplat, "MFCreateMediaType");
    g_mf.create_sample = (ns_mf_sample_fn)(void *)GetProcAddress(mfplat, "MFCreateSample");
    g_mf.create_memory_buffer =
        (ns_mf_buffer_fn)(void *)GetProcAddress(mfplat, "MFCreateMemoryBuffer");
    return g_mf.startup && g_mf.enum_ex && g_mf.create_media_type &&
           g_mf.create_sample && g_mf.create_memory_buffer;
}

/* Activates the best-ranked synchronous software decoder for subtype; NULL if none. */
static IMFTransform *
find_decoder(const GUID *subtype)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, *subtype };
    IMFActivate **acts = NULL;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                   MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(g_mf.enum_ex(MFT_CATEGORY_VIDEO_DECODER, flags, &in, NULL, &acts, &count)))
        return NULL;
    IMFTransform *mft = NULL;
    for (UINT32 i = 0; i < count; i++) {
        if (!mft && FAILED(IMFActivate_ActivateObject(acts[i], &IID_IMFTransform,
                                                      (void **)&mft)))
            mft = NULL;
        IMFActivate_Release(acts[i]);
    }
    CoTaskMemFree(acts);
    return mft;
}

static const ns_mf_codec *
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

static ns_mf_video_decoder *build(const ns_mf_codec *codec);

/* Runs on its own thread: the main thread must stay out of the
 * multithreaded apartment, since GTK initialises OLE there. */
static gpointer
preinit_thread(gpointer data)
{
    (void)data;
    if (!com_ready()) return NULL;
    if (FAILED(g_mf.startup(MF_VERSION, MFSTARTUP_LITE))) {
        g_info("mfvideo: Media Foundation unavailable");
        return NULL;
    }
    g_mf_ok = TRUE;
    /* Open each decoder once now, so the DLLs behind it are mapped
     * before the mitigation policies apply. */
    for (gsize i = 0; i < G_N_ELEMENTS(g_codecs); i++) {
        ns_mf_video_decoder *warm = build(&g_codecs[i]);
        g_available[i] = warm != NULL;
        ns_mf_video_decoder_free(warm);
        g_debug("mfvideo: %s %s", g_codecs[i].codec,
                g_available[i] ? "decodes through Media Foundation" : "has no decoder");
    }
    CoUninitialize();
    return NULL;
}

void
ns_mf_video_preinit(void)
{
    if (g_preinit_done) return;
    g_preinit_done = TRUE;
    if (native_forced() || !mf_load()) return;
    g_thread_join(g_thread_new("mf-preinit", preinit_thread, NULL));
}

gboolean
ns_mf_video_available(const char *codec)
{
    gsize i = 0;
    if (!g_mf_ok || !find_codec(codec, &i)) return FALSE;
    return g_available[i];
}

static void
read_size(IMFAttributes *attrs, REFGUID key, int *w, int *h)
{
    UINT64 packed = 0;
    if (SUCCEEDED(IMFAttributes_GetUINT64(attrs, key, &packed))) {
        *w = (int)(packed >> 32);
        *h = (int)(packed & 0xffffffffu);
    }
}

static gboolean
subtype_supported(const GUID *sub)
{
    return IsEqualGUID(sub, &MFVideoFormat_NV12) || IsEqualGUID(sub, &MFVideoFormat_P010) ||
           IsEqualGUID(sub, &MFVideoFormat_I420) || IsEqualGUID(sub, &MFVideoFormat_IYUV);
}

/* Picks the first output type the decoder offers that we can convert. */
static gboolean
pick_output(ns_mf_video_decoder *self)
{
    for (DWORD i = 0;; i++) {
        IMFMediaType *type = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(self->mft, 0, i, &type)))
            return FALSE;
        GUID sub;
        gboolean ok = SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &sub)) &&
                      subtype_supported(&sub) &&
                      SUCCEEDED(IMFTransform_SetOutputType(self->mft, 0, type, 0));
        if (!ok) {
            IMFMediaType_Release(type);
            continue;
        }
        IMFAttributes *attrs = (IMFAttributes *)type;
        int w = 0, h = 0;
        read_size(attrs, &MF_MT_FRAME_SIZE, &w, &h);
        self->out_subtype = sub;
        self->frame_width = w;
        self->frame_height = h;
        self->width = w;
        self->height = h;
        MFVideoArea area;
        UINT32 size = 0;
        if (SUCCEEDED(IMFMediaType_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE,
                                           (UINT8 *)&area, sizeof area, &size)) &&
            size == sizeof area && area.Area.cx > 0 && area.Area.cy > 0 &&
            area.Area.cx <= w && area.Area.cy <= h) {
            self->width = (int)area.Area.cx;
            self->height = (int)area.Area.cy;
        }
        UINT32 range = 0, matrix = 0;
        self->full_range =
            SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_VIDEO_NOMINAL_RANGE, &range)) &&
            range == MFNominalRange_0_255;
        self->bt601 =
            SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_YUV_MATRIX, &matrix))
                ? matrix == MFVideoTransferMatrix_BT601
                : h < 720;
        IMFMediaType_Release(type);
        MFT_OUTPUT_STREAM_INFO info;
        memset(&info, 0, sizeof info);
        if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(self->mft, 0, &info))) {
            self->out_buf_size = info.cbSize;
            self->provides_samples =
                (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                 MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        }
        self->output_ready = w > 0 && h > 0;
        g_debug("mfvideo: %s output %.4s %dx%d, shown %dx%d", self->codec->codec,
                (const char *)&sub.Data1, w, h, self->width, self->height);
        return self->output_ready;
    }
}

static ns_mf_video_decoder *
build(const ns_mf_codec *codec)
{
    if (!g_mf_ok || !com_ready()) return NULL;
    IMFTransform *mft = find_decoder(codec->subtype);
    if (!mft) return NULL;
    ns_mf_video_decoder *self = g_new0(ns_mf_video_decoder, 1);
    self->codec = codec;
    self->mft = mft;
    self->unit = g_byte_array_new();
    self->length_size = 4;
    self->need_param_sets = TRUE;
    self->wanted = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    g_queue_init(&self->late);
    IMFAttributes *attrs = NULL;
    if (SUCCEEDED(IMFTransform_GetAttributes(mft, &attrs)) && attrs) {
        IMFAttributes_SetUINT32(attrs, &NS_MF_LOW_LATENCY, TRUE);
        IMFAttributes_Release(attrs);
    }
    IMFMediaType *in = NULL;
    if (FAILED(g_mf.create_media_type(&in))) goto fail;
    IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, codec->subtype);
    IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    /* Decoders want a size up front; the stream's own replaces it. */
    IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, ((UINT64)1280 << 32) | 720);
    HRESULT hr = IMFTransform_SetInputType(mft, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr)) goto fail;
    pick_output(self);
    IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return self;
fail:
    ns_mf_video_decoder_free(self);
    return NULL;
}

ns_mf_video_decoder *
ns_mf_video_decoder_new(const char *codec)
{
    gsize i = 0;
    const ns_mf_codec *row = find_codec(codec, &i);
    if (!row || !ns_mf_video_available(codec)) return NULL;
    ns_mf_video_decoder *decoder = build(row);
    if (decoder) g_debug("mfvideo: new %s decoder", row->codec);
    return decoder;
}

static void
late_clear(ns_mf_video_decoder *self)
{
    ns_mf_late *late;
    while ((late = g_queue_pop_head(&self->late)) != NULL) {
        ns_texture_unref(late->texture);
        g_free(late);
    }
}

void
ns_mf_video_decoder_free(ns_mf_video_decoder *decoder)
{
    if (!decoder) return;
    if (decoder->mft) {
        IMFTransform_ProcessMessage(decoder->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(decoder->mft);
    }
    if (decoder->config) g_bytes_unref(decoder->config);
    if (decoder->unit) g_byte_array_free(decoder->unit, TRUE);
    late_clear(decoder);
    if (decoder->wanted) g_hash_table_unref(decoder->wanted);
    g_free(decoder->chroma);
    g_free(decoder);
}

/* Drops decoder state after a seek; the next sample starts at a keyframe. */
void
ns_mf_video_decoder_flush(ns_mf_video_decoder *decoder)
{
    if (!decoder || !decoder->mft) return;
    IMFTransform_ProcessMessage(decoder->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(decoder->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    late_clear(decoder);
    g_hash_table_remove_all(decoder->wanted);
    decoder->need_param_sets = TRUE;
    decoder->broken = FALSE;
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
            g_byte_array_append(out, NS_MF_START_CODE, sizeof NS_MF_START_CODE);
            g_byte_array_append(out, avcc + pos, (guint)size);
            pos += size;
        }
    }
    return TRUE;
}

/* Converts a length-prefixed MP4 sample into Annex B start-code NAL units. */
static gboolean
append_nal_units(GByteArray *out, const guint8 *data, gsize len, int length_size)
{
    gsize pos = 0;
    while (pos < len) {
        if (len - pos < (gsize)length_size) return FALSE;
        gsize size = 0;
        for (int i = 0; i < length_size; i++) size = size << 8 | data[pos++];
        if (size > len - pos) return FALSE;
        if (size == 0) continue;
        g_byte_array_append(out, NS_MF_START_CODE, sizeof NS_MF_START_CODE);
        g_byte_array_append(out, data + pos, (guint)size);
        pos += size;
    }
    return TRUE;
}

static IMFSample *
make_sample(const guint8 *data, gsize len, gint64 timestamp)
{
    if (len == 0 || len > G_MAXINT32) return NULL;
    IMFMediaBuffer *buf = NULL;
    if (FAILED(g_mf.create_memory_buffer((DWORD)len, &buf))) return NULL;
    BYTE *p = NULL;
    if (FAILED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
        IMFMediaBuffer_Release(buf);
        return NULL;
    }
    memcpy(p, data, len);
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_SetCurrentLength(buf, (DWORD)len);
    IMFSample *sample = NULL;
    if (FAILED(g_mf.create_sample(&sample))) {
        IMFMediaBuffer_Release(buf);
        return NULL;
    }
    IMFSample_AddBuffer(sample, buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_SetSampleTime(sample, (LONGLONG)timestamp * 10);
    return sample;
}

static IMFSample *
make_output_sample(DWORD size)
{
    IMFSample *sample = NULL;
    IMFMediaBuffer *buf = NULL;
    if (FAILED(g_mf.create_sample(&sample))) return NULL;
    if (FAILED(g_mf.create_memory_buffer(size ? size : 4096, &buf))) {
        IMFSample_Release(sample);
        return NULL;
    }
    IMFSample_AddBuffer(sample, buf);
    IMFMediaBuffer_Release(buf);
    return sample;
}

/* Splits an interleaved UV plane into the decoder's scratch U and V planes. */
static gboolean
split_chroma(ns_mf_video_decoder *self, const guint8 *uv, ptrdiff_t pitch, int bytes,
             int cw, int ch, const void **u, const void **v, ptrdiff_t *stride)
{
    gsize row = (gsize)cw * (gsize)bytes;
    gsize need = row * (gsize)ch * 2;
    if (need > self->chroma_size) {
        g_free(self->chroma);
        self->chroma = g_try_malloc(need);
        self->chroma_size = self->chroma ? need : 0;
        if (!self->chroma) return FALSE;
    }
    guint8 *uo = self->chroma;
    guint8 *vo = self->chroma + row * (gsize)ch;
    for (int y = 0; y < ch; y++) {
        const guint8 *src = uv + (ptrdiff_t)y * pitch;
        guint8 *ud = uo + (gsize)y * row;
        guint8 *vd = vo + (gsize)y * row;
        if (bytes == 1) {
            for (int x = 0; x < cw; x++) {
                ud[x] = src[2 * x];
                vd[x] = src[2 * x + 1];
            }
        } else {
            for (int x = 0; x < cw; x++) {
                memcpy(ud + 2 * x, src + 4 * x, 2);
                memcpy(vd + 2 * x, src + 4 * x + 2, 2);
            }
        }
    }
    *u = uo;
    *v = vo;
    *stride = (ptrdiff_t)row;
    return TRUE;
}

/* Converts a decoded NV12, P010 or I420 sample to a BGRA texture. */
static ns_texture *
sample_to_texture(ns_mf_video_decoder *self, IMFSample *sample)
{
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) return NULL;
    IMF2DBuffer *b2 = NULL;
    BYTE *base = NULL;
    LONG pitch = 0;
    DWORD cur = 0;
    gboolean locked2d = FALSE, locked = FALSE;
    gboolean p010 = IsEqualGUID(&self->out_subtype, &MFVideoFormat_P010);
    gboolean nv12 = p010 || IsEqualGUID(&self->out_subtype, &MFVideoFormat_NV12);
    int bytes = p010 ? 2 : 1;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
        SUCCEEDED(IMF2DBuffer_Lock2D(b2, &base, &pitch)) && pitch > 0) {
        locked2d = TRUE;
        IMFMediaBuffer_GetCurrentLength(buf, &cur);
    } else if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &base, NULL, &cur))) {
        locked = TRUE;
        pitch = self->frame_width * bytes;
    }
    ns_texture *texture = NULL;
    gsize luma = (gsize)pitch * (gsize)self->frame_height;
    int cw = (self->width + 1) / 2, ch = (self->height + 1) / 2;
    gsize chroma_rows = (gsize)(self->frame_height + 1) / 2;
    gsize need = nv12 ? luma + (gsize)pitch * chroma_rows
                      : luma + 2 * ((gsize)pitch / 2) * chroma_rows;
    if (base && (locked2d || locked) && (!locked || cur >= need)) {
        ns_yuv_picture yuv = {
            .planes = { base, NULL, NULL },
            .strides = { pitch, 0, 0 },
            .width = self->width,
            .height = self->height,
            .bits = p010 ? 16 : 8,
            .sub_x = 1,
            .sub_y = 1,
            .bt601 = self->bt601,
            .full_range = self->full_range,
        };
        gboolean ok = TRUE;
        if (nv12) {
            ok = split_chroma(self, base + luma, pitch, bytes, cw, ch, &yuv.planes[1],
                              &yuv.planes[2], &yuv.strides[1]);
            yuv.strides[2] = yuv.strides[1];
        } else {
            yuv.planes[1] = base + luma;
            yuv.planes[2] = base + luma + (gsize)(pitch / 2) * chroma_rows;
            yuv.strides[1] = yuv.strides[2] = pitch / 2;
        }
        if (ok) texture = ns_yuv_to_texture(&yuv);
    }
    if (locked2d) IMF2DBuffer_Unlock2D(b2);
    if (locked) IMFMediaBuffer_Unlock(buf);
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(buf);
    return texture;
}

/* Keeps a picture that came out for an earlier timestamp until the worker takes it. */
static void
push_late(ns_mf_video_decoder *self, gint64 timestamp, ns_texture *texture)
{
    ns_mf_late *late = g_new(ns_mf_late, 1);
    late->timestamp = timestamp;
    late->texture = texture;
    g_queue_push_tail(&self->late, late);
    while (g_queue_get_length(&self->late) > NS_MF_MAX_LATE) {
        ns_mf_late *old = g_queue_pop_head(&self->late);
        ns_texture_unref(old->texture);
        g_free(old);
    }
}

/* Pulls every picture the decoder has ready; returns the one for timestamp. */
static ns_texture *
collect(ns_mf_video_decoder *self, gint64 timestamp)
{
    ns_texture *result = NULL;
    for (int i = 0; i < NS_MF_MAX_OUTPUTS; i++) {
        if (!self->output_ready && !pick_output(self)) break;
        MFT_OUTPUT_DATA_BUFFER out;
        memset(&out, 0, sizeof out);
        if (!self->provides_samples) {
            out.pSample = make_output_sample(self->out_buf_size);
            if (!out.pSample) break;
        }
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(self->mft, 0, 1, &out, &status);
        if (out.pEvents) IMFCollection_Release(out.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (out.pSample) IMFSample_Release(out.pSample);
            self->output_ready = FALSE;
            continue;
        }
        if (FAILED(hr)) {
            if (out.pSample) IMFSample_Release(out.pSample);
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT)
                g_info("mfvideo: %s decoder output failed: 0x%08lx",
                       self->codec->codec, (unsigned long)hr);
            break;
        }
        if (!out.pSample) continue;
        LONGLONG time = 0;
        if (SUCCEEDED(IMFSample_GetSampleTime(out.pSample, &time))) {
            gint64 stamp = (gint64)(time / 10);
            if (g_hash_table_remove(self->wanted, &stamp)) {
                ns_texture *texture = sample_to_texture(self, out.pSample);
                if (stamp == timestamp && !result) result = texture;
                else if (texture) push_late(self, stamp, texture);
            }
        }
        IMFSample_Release(out.pSample);
    }
    return result;
}

/* Decodes one sample; returns the picture for timestamp when wanted and ready. */
ns_texture *
ns_mf_video_decoder_decode(ns_mf_video_decoder *decoder, GBytes *sample,
                           GBytes *config, gint64 timestamp, gboolean want_texture)
{
    if (!decoder || !decoder->mft || !sample || decoder->broken || !com_ready())
        return NULL;
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(sample, &len);
    if (len == 0) return NULL;
    IMFSample *input;
    gboolean discontinuity = decoder->need_param_sets;
    if (decoder->codec->avc) {
        GByteArray *unit = decoder->unit;
        g_byte_array_set_size(unit, 0);
        gboolean new_config = config && !(decoder->config &&
                                          g_bytes_equal(config, decoder->config));
        if ((new_config || decoder->need_param_sets) && config &&
            !append_parameter_sets(unit, config, &decoder->length_size))
            return NULL;
        if (!append_nal_units(unit, data, len, decoder->length_size) || unit->len == 0)
            return NULL;
        if (new_config) {
            if (decoder->config) g_bytes_unref(decoder->config);
            decoder->config = g_bytes_ref(config);
        }
        input = make_sample(unit->data, unit->len, timestamp);
    } else {
        input = make_sample(data, len, timestamp);
    }
    if (!input) return NULL;
    if (discontinuity) IMFSample_SetUINT32(input, &MFSampleExtension_Discontinuity, TRUE);
    if (want_texture) {
        gint64 *key = g_new(gint64, 1);
        *key = timestamp;
        g_hash_table_add(decoder->wanted, key);
    }
    ns_texture *texture = NULL;
    HRESULT hr = IMFTransform_ProcessInput(decoder->mft, 0, input, 0);
    if (hr == MF_E_NOTACCEPTING) {
        texture = collect(decoder, timestamp);
        hr = IMFTransform_ProcessInput(decoder->mft, 0, input, 0);
    }
    IMFSample_Release(input);
    if (FAILED(hr)) {
        g_info("mfvideo: %s decoder refused input: 0x%08lx", decoder->codec->codec,
               (unsigned long)hr);
        g_hash_table_remove(decoder->wanted, &timestamp);
        if (hr != MF_E_NOTACCEPTING) decoder->broken = TRUE;
        return texture;
    }
    decoder->need_param_sets = FALSE;
    if (!texture) texture = collect(decoder, timestamp);
    if (!want_texture && texture) {
        ns_texture_unref(texture);
        texture = NULL;
    }
    return texture;
}

/* Hands back one picture that came out for an earlier timestamp (decoder reordering). */
ns_texture *
ns_mf_video_decoder_take_late(ns_mf_video_decoder *decoder, gint64 *timestamp)
{
    if (!decoder) return NULL;
    ns_mf_late *late = g_queue_pop_head(&decoder->late);
    if (!late) return NULL;
    ns_texture *texture = late->texture;
    if (timestamp) *timestamp = late->timestamp;
    g_free(late);
    return texture;
}

#else

void
ns_mf_video_preinit(void)
{
}

gboolean
ns_mf_video_available(const char *codec)
{
    (void)codec;
    return FALSE;
}

ns_mf_video_decoder *
ns_mf_video_decoder_new(const char *codec)
{
    (void)codec;
    return NULL;
}

void
ns_mf_video_decoder_free(ns_mf_video_decoder *decoder)
{
    (void)decoder;
}

void
ns_mf_video_decoder_flush(ns_mf_video_decoder *decoder)
{
    (void)decoder;
}

ns_texture *
ns_mf_video_decoder_decode(ns_mf_video_decoder *decoder, GBytes *sample,
                           GBytes *config, gint64 timestamp, gboolean want_texture)
{
    (void)decoder;
    (void)sample;
    (void)config;
    (void)timestamp;
    (void)want_texture;
    return NULL;
}

ns_texture *
ns_mf_video_decoder_take_late(ns_mf_video_decoder *decoder, gint64 *timestamp)
{
    (void)decoder;
    (void)timestamp;
    return NULL;
}

#endif
