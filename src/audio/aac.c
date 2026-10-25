/* Northstar Web Browser — AAC-LC decoder for MP4 access units, written from ISO/IEC 14496-3.
 * Huffman spectra, dequantization, M/S, intensity, PNS, TNS and FFT-based IMDCT overlap-add.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aac.h"

#include <math.h>
#include <string.h>

enum {
    AAC_FRAME_LENGTH = 1024,
    AAC_SHORT_LENGTH = 128,
    AAC_MAX_CHANNELS = 2,
    AAC_MAX_WINDOWS = 8,
    AAC_MAX_BANDS = 64,
    AAC_MAX_TNS_FILTERS = 3,
    AAC_MAX_TNS_ORDER_LONG = 12,
    AAC_MAX_TNS_ORDER_SHORT = 7,
    AAC_MAX_HUFFMAN_LENGTH = 19,
    AAC_HUFFMAN_POOL_SIZE = 2800,
    AAC_MAX_PULSES = 4,
    AAC_POWER_TABLE_SIZE = 8192 + 16,
    AAC_SAMPLING_INDEX_COUNT = 13,
};

enum {
    AAC_ZERO_CODEBOOK = 0,
    AAC_ESCAPE_CODEBOOK = 11,
    AAC_RESERVED_CODEBOOK = 12,
    AAC_NOISE_CODEBOOK = 13,
    AAC_INTENSITY_OUT_OF_PHASE_CODEBOOK = 14,
    AAC_INTENSITY_IN_PHASE_CODEBOOK = 15,
};

enum {
    AAC_ONLY_LONG_SEQUENCE = 0,
    AAC_LONG_START_SEQUENCE = 1,
    AAC_EIGHT_SHORT_SEQUENCE = 2,
    AAC_LONG_STOP_SEQUENCE = 3,
};

enum {
    AAC_ELEMENT_SINGLE_CHANNEL = 0,
    AAC_ELEMENT_CHANNEL_PAIR = 1,
    AAC_ELEMENT_COUPLING_CHANNEL = 2,
    AAC_ELEMENT_LOW_FREQUENCY = 3,
    AAC_ELEMENT_DATA_STREAM = 4,
    AAC_ELEMENT_PROGRAM_CONFIG = 5,
    AAC_ELEMENT_FILL = 6,
    AAC_ELEMENT_END = 7,
};

enum {
    AAC_OBJECT_TYPE_LOW_COMPLEXITY = 2,
    AAC_SCALEFACTOR_OFFSET = 100,
    AAC_SCALEFACTOR_DELTA_OFFSET = 60,
    AAC_NOISE_ENERGY_OFFSET = 90,
    AAC_NOISE_PCM_OFFSET = 256,
};

typedef struct {
    float re;
    float im;
} aac_complex;

typedef struct {
    gint16 child[2];
} aac_huffman_node;

typedef struct {
    const guint32    *codes;
    const guint8     *lengths;
    int               symbol_count;
    int               dimension;
    int               modulus;
    gboolean          is_signed;
    aac_huffman_node *nodes;
} aac_codebook;

typedef struct {
    int                length;
    const aac_complex *pre_twiddle;
    const aac_complex *post_twiddle;
    const aac_complex *fft_twiddle;
    const guint16     *bit_reverse;
    float              scale;
} aac_imdct_plan;

typedef struct {
    const guint8 *data;
    gsize         size_bits;
    gsize         position;
    gboolean      overrun;
} aac_bit_reader;

typedef struct {
    int            window_sequence;
    int            window_shape;
    int            max_sfb;
    int            num_windows;
    int            num_window_groups;
    int            window_group_length[AAC_MAX_WINDOWS];
    int            window_length;
    int            num_swb;
    const guint16 *swb_offset;
    int            tns_max_bands;
    int            tns_max_order;
} aac_ics_info;

typedef struct {
    aac_ics_info info;
    guint8       band_type[AAC_MAX_WINDOWS][AAC_MAX_BANDS];
    int          band_value[AAC_MAX_WINDOWS][AAC_MAX_BANDS];
    gboolean     pulse_present;
    int          pulse_count;
    int          pulse_position[AAC_MAX_PULSES];
    int          pulse_amplitude[AAC_MAX_PULSES];
    gboolean     tns_present;
    int          tns_filter_count[AAC_MAX_WINDOWS];
    int          tns_length[AAC_MAX_WINDOWS][AAC_MAX_TNS_FILTERS];
    int          tns_order[AAC_MAX_WINDOWS][AAC_MAX_TNS_FILTERS];
    gboolean     tns_downward[AAC_MAX_WINDOWS][AAC_MAX_TNS_FILTERS];
    float        tns_lpc[AAC_MAX_WINDOWS][AAC_MAX_TNS_FILTERS]
                    [AAC_MAX_TNS_ORDER_LONG + 1];
    int          quantized[AAC_FRAME_LENGTH];
    float        spectrum[AAC_FRAME_LENGTH];
} aac_channel;

struct ns_aac_decoder {
    int            sample_rate;
    int            sampling_index;
    int            channels;
    const guint16 *swb_offset_long;
    int            num_swb_long;
    const guint16 *swb_offset_short;
    int            num_swb_short;
    guint32        noise_state;
    int            ms_mask_present;
    guint8         ms_used[AAC_MAX_WINDOWS][AAC_MAX_BANDS];
    aac_channel    channel[AAC_MAX_CHANNELS];
    int            previous_window_shape[AAC_MAX_CHANNELS];
    float          overlap[AAC_MAX_CHANNELS][AAC_FRAME_LENGTH];
    float          pcm[AAC_MAX_CHANNELS][AAC_FRAME_LENGTH];
    float          time_buffer[2 * AAC_FRAME_LENGTH];
    float          short_buffer[2 * AAC_SHORT_LENGTH];
    float          dct_buffer[AAC_FRAME_LENGTH];
    aac_complex    fft_buffer[AAC_FRAME_LENGTH / 2];
};

static const int aac_sample_rates[AAC_SAMPLING_INDEX_COUNT] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000,  7350,
};

static const guint8 tns_max_bands_long[AAC_SAMPLING_INDEX_COUNT] = {
    31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39,
};

static const guint8 tns_max_bands_short[AAC_SAMPLING_INDEX_COUNT] = {
    9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
};

static const guint16 swb_offset_long_96[] = {
    0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  48,  52,
    56,  64,  72,  80,  88,  96,  108, 120, 132, 144, 156, 172, 188, 212,
    240, 276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024,
};

static const guint16 swb_offset_long_64[] = {
    0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,
    48,  52,  56,  64,  72,  80,  88,  100, 112, 124, 140, 156,
    172, 192, 216, 240, 268, 304, 344, 384, 424, 464, 504, 544,
    584, 624, 664, 704, 744, 784, 824, 864, 904, 944, 984, 1024,
};

static const guint16 swb_offset_long_48[] = {
    0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  48,  56,
    64,  72,  80,  88,  96,  108, 120, 132, 144, 160, 176, 196, 216,
    240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608,
    640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024,
};

static const guint16 swb_offset_long_32[] = {
    0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  48,  56,
    64,  72,  80,  88,  96,  108, 120, 132, 144, 160, 176, 196, 216,
    240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608,
    640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024,
};

static const guint16 swb_offset_long_24[] = {
    0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,
    52,  60,  68,  76,  84,  92,  100, 108, 116, 124, 136, 148,
    160, 172, 188, 204, 220, 240, 260, 284, 308, 336, 364, 396,
    432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024,
};

static const guint16 swb_offset_long_16[] = {
    0,   8,   16,  24,  32,  40,  48,  56,  64,  72,  80,
    88,  100, 112, 124, 136, 148, 160, 172, 184, 196, 212,
    228, 244, 260, 280, 300, 320, 344, 368, 396, 424, 456,
    492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024,
};

static const guint16 swb_offset_long_8[] = {
    0,   12,  24,  36,  48,  60,  72,  84,  96,  108, 120, 132, 144, 156,
    172, 188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420,
    448, 476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024,
};

static const guint16 swb_offset_short_96[] = {
    0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128,
};

static const guint16 swb_offset_short_48[] = {
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128,
};

static const guint16 swb_offset_short_24[] = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128,
};

static const guint16 swb_offset_short_16[] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128,
};

static const guint16 swb_offset_short_8[] = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128,
};

static const guint32 scalefactor_codes[121] = {
    0x3ffe8, 0x3ffe6, 0x3ffe7, 0x3ffe5, 0x7fff5, 0x7fff1, 0x7ffed, 0x7fff6,
    0x7ffee, 0x7ffef, 0x7fff0, 0x7fffc, 0x7fffd, 0x7ffff, 0x7fffe, 0x7fff7,
    0x7fff8, 0x7fffb, 0x7fff9, 0x3ffe4, 0x7fffa, 0x3ffe3, 0x1ffef, 0x1fff0,
    0x0fff5, 0x1ffee, 0x0fff2, 0x0fff3, 0x0fff4, 0x0fff1, 0x07ff6, 0x07ff7,
    0x03ff9, 0x03ff5, 0x03ff7, 0x03ff3, 0x03ff6, 0x03ff2, 0x01ff7, 0x01ff5,
    0x00ff9, 0x00ff7, 0x00ff6, 0x007f9, 0x00ff4, 0x007f8, 0x003f9, 0x003f7,
    0x003f5, 0x001f8, 0x001f7, 0x000fa, 0x000f8, 0x000f6, 0x00079, 0x0003a,
    0x00038, 0x0001a, 0x0000b, 0x00004, 0x00000, 0x0000a, 0x0000c, 0x0001b,
    0x00039, 0x0003b, 0x00078, 0x0007a, 0x000f7, 0x000f9, 0x001f6, 0x001f9,
    0x003f4, 0x003f6, 0x003f8, 0x007f5, 0x007f4, 0x007f6, 0x007f7, 0x00ff5,
    0x00ff8, 0x01ff4, 0x01ff6, 0x01ff8, 0x03ff8, 0x03ff4, 0x0fff0, 0x07ff4,
    0x0fff6, 0x07ff5, 0x3ffe2, 0x7ffd9, 0x7ffda, 0x7ffdb, 0x7ffdc, 0x7ffdd,
    0x7ffde, 0x7ffd8, 0x7ffd2, 0x7ffd3, 0x7ffd4, 0x7ffd5, 0x7ffd6, 0x7fff2,
    0x7ffdf, 0x7ffe7, 0x7ffe8, 0x7ffe9, 0x7ffea, 0x7ffeb, 0x7ffe6, 0x7ffe0,
    0x7ffe1, 0x7ffe2, 0x7ffe3, 0x7ffe4, 0x7ffe5, 0x7ffd7, 0x7ffec, 0x7fff4,
    0x7fff3,
};

static const guint8 scalefactor_lengths[121] = {
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 18, 19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15,
    14, 14, 14, 14, 14, 14, 13, 13, 12, 12, 12, 11, 12, 11, 10, 10,
    10, 9,  9,  8,  8,  8,  7,  6,  6,  5,  4,  3,  1,  4,  4,  5,
    6,  6,  7,  7,  8,  8,  9,  9,  10, 10, 10, 11, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19,
};

static const guint32 spectrum1_codes[81] = {
    0x7f8, 0x1f1, 0x7fd, 0x3f5, 0x068, 0x3f0, 0x7f7, 0x1ec, 0x7f5,
    0x3f1, 0x072, 0x3f4, 0x074, 0x011, 0x076, 0x1eb, 0x06c, 0x3f6,
    0x7fc, 0x1e1, 0x7f1, 0x1f0, 0x061, 0x1f6, 0x7f2, 0x1ea, 0x7fb,
    0x1f2, 0x069, 0x1ed, 0x077, 0x017, 0x06f, 0x1e6, 0x064, 0x1e5,
    0x067, 0x015, 0x062, 0x012, 0x000, 0x014, 0x065, 0x016, 0x06d,
    0x1e9, 0x063, 0x1e4, 0x06b, 0x013, 0x071, 0x1e3, 0x070, 0x1f3,
    0x7fe, 0x1e7, 0x7f3, 0x1ef, 0x060, 0x1ee, 0x7f0, 0x1e2, 0x7fa,
    0x3f3, 0x06a, 0x1e8, 0x075, 0x010, 0x073, 0x1f4, 0x06e, 0x3f7,
    0x7f6, 0x1e0, 0x7f9, 0x3f2, 0x066, 0x1f5, 0x7ff, 0x1f7, 0x7f4,
};

static const guint8 spectrum1_lengths[81] = {
    11, 9,  11, 10, 7,  10, 11, 9,  11, 10, 7,  10, 7,  5,  7,  9,  7,
    10, 11, 9,  11, 9,  7,  9,  11, 9,  11, 9,  7,  9,  7,  5,  7,  9,
    7,  9,  7,  5,  7,  5,  1,  5,  7,  5,  7,  9,  7,  9,  7,  5,  7,
    9,  7,  9,  11, 9,  11, 9,  7,  9,  11, 9,  11, 10, 7,  9,  7,  5,
    7,  9,  7,  10, 11, 9,  11, 10, 7,  9,  11, 9,  11,
};

static const guint32 spectrum2_codes[81] = {
    0x1f3, 0x06f, 0x1fd, 0x0eb, 0x023, 0x0ea, 0x1f7, 0x0e8, 0x1fa,
    0x0f2, 0x02d, 0x070, 0x020, 0x006, 0x02b, 0x06e, 0x028, 0x0e9,
    0x1f9, 0x066, 0x0f8, 0x0e7, 0x01b, 0x0f1, 0x1f4, 0x06b, 0x1f5,
    0x0ec, 0x02a, 0x06c, 0x02c, 0x00a, 0x027, 0x067, 0x01a, 0x0f5,
    0x024, 0x008, 0x01f, 0x009, 0x000, 0x007, 0x01d, 0x00b, 0x030,
    0x0ef, 0x01c, 0x064, 0x01e, 0x00c, 0x029, 0x0f3, 0x02f, 0x0f0,
    0x1fc, 0x071, 0x1f2, 0x0f4, 0x021, 0x0e6, 0x0f7, 0x068, 0x1f8,
    0x0ee, 0x022, 0x065, 0x031, 0x002, 0x026, 0x0ed, 0x025, 0x06a,
    0x1fb, 0x072, 0x1fe, 0x069, 0x02e, 0x0f6, 0x1ff, 0x06d, 0x1f6,
};

static const guint8 spectrum2_lengths[81] = {
    9, 7, 9, 8, 6, 8, 9, 8, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 9, 7, 8,
    8, 6, 8, 9, 7, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 6, 5, 6, 5, 3, 5,
    6, 5, 6, 8, 6, 7, 6, 5, 6, 8, 6, 8, 9, 7, 9, 8, 6, 8, 8, 7, 9,
    8, 6, 7, 6, 4, 6, 8, 6, 7, 9, 7, 9, 7, 6, 8, 9, 7, 9,
};

static const guint32 spectrum3_codes[81] = {
    0x0000, 0x0009, 0x00ef, 0x000b, 0x0019, 0x00f0, 0x01eb, 0x01e6, 0x03f2,
    0x000a, 0x0035, 0x01ef, 0x0034, 0x0037, 0x01e9, 0x01ed, 0x01e7, 0x03f3,
    0x01ee, 0x03ed, 0x1ffa, 0x01ec, 0x01f2, 0x07f9, 0x07f8, 0x03f8, 0x0ff8,
    0x0008, 0x0038, 0x03f6, 0x0036, 0x0075, 0x03f1, 0x03eb, 0x03ec, 0x0ff4,
    0x0018, 0x0076, 0x07f4, 0x0039, 0x0074, 0x03ef, 0x01f3, 0x01f4, 0x07f6,
    0x01e8, 0x03ea, 0x1ffc, 0x00f2, 0x01f1, 0x0ffb, 0x03f5, 0x07f3, 0x0ffc,
    0x00ee, 0x03f7, 0x7ffe, 0x01f0, 0x07f5, 0x7ffd, 0x1ffb, 0x3ffa, 0xffff,
    0x00f1, 0x03f0, 0x3ffc, 0x01ea, 0x03ee, 0x3ffb, 0x0ff6, 0x0ffa, 0x7ffc,
    0x07f2, 0x0ff5, 0xfffe, 0x03f4, 0x07f7, 0x7ffb, 0x0ff7, 0x0ff9, 0x7ffa,
};

static const guint8 spectrum3_lengths[81] = {
    1,  4,  8,  4,  5,  8,  9,  9,  10, 4,  6,  9,  6,  6,  9,  9,  9,
    10, 9,  10, 13, 9,  9,  11, 11, 10, 12, 4,  6,  10, 6,  7,  10, 10,
    10, 12, 5,  7,  11, 6,  7,  10, 9,  9,  11, 9,  10, 13, 8,  9,  12,
    10, 11, 12, 8,  10, 15, 9,  11, 15, 13, 14, 16, 8,  10, 14, 9,  10,
    14, 12, 12, 15, 11, 12, 16, 10, 11, 15, 12, 12, 15,
};

static const guint32 spectrum4_codes[81] = {
    0x007, 0x016, 0x0f6, 0x018, 0x008, 0x0ef, 0x1ef, 0x0f3, 0x7f8,
    0x019, 0x017, 0x0ed, 0x015, 0x001, 0x0e2, 0x0f0, 0x070, 0x3f0,
    0x1ee, 0x0f1, 0x7fa, 0x0ee, 0x0e4, 0x3f2, 0x7f6, 0x3ef, 0x7fd,
    0x005, 0x014, 0x0f2, 0x009, 0x004, 0x0e5, 0x0f4, 0x0e8, 0x3f4,
    0x006, 0x002, 0x0e7, 0x003, 0x000, 0x06b, 0x0e3, 0x069, 0x1f3,
    0x0eb, 0x0e6, 0x3f6, 0x06e, 0x06a, 0x1f4, 0x3ec, 0x1f0, 0x3f9,
    0x0f5, 0x0ec, 0x7fb, 0x0ea, 0x06f, 0x3f7, 0x7f9, 0x3f3, 0xfff,
    0x0e9, 0x06d, 0x3f8, 0x06c, 0x068, 0x1f5, 0x3ee, 0x1f2, 0x7f4,
    0x7f7, 0x3f1, 0xffe, 0x3ed, 0x1f1, 0x7f5, 0x7fe, 0x3f5, 0x7fc,
};

static const guint8 spectrum4_lengths[81] = {
    4,  5,  8,  5,  4,  8,  9,  8,  11, 5,  5,  8,  5,  4,  8,  8,  7,
    10, 9,  8,  11, 8,  8,  10, 11, 10, 11, 4,  5,  8,  4,  4,  8,  8,
    8,  10, 4,  4,  8,  4,  4,  7,  8,  7,  9,  8,  8,  10, 7,  7,  9,
    10, 9,  10, 8,  8,  11, 8,  7,  10, 11, 10, 12, 8,  7,  10, 7,  7,
    9,  10, 9,  11, 11, 10, 12, 10, 9,  11, 11, 10, 11,
};

static const guint32 spectrum5_codes[81] = {
    0x1fff, 0x0ff7, 0x07f4, 0x07e8, 0x03f1, 0x07ee, 0x07f9, 0x0ff8, 0x1ffd,
    0x0ffd, 0x07f1, 0x03e8, 0x01e8, 0x00f0, 0x01ec, 0x03ee, 0x07f2, 0x0ffa,
    0x0ff4, 0x03ef, 0x01f2, 0x00e8, 0x0070, 0x00ec, 0x01f0, 0x03ea, 0x07f3,
    0x07eb, 0x01eb, 0x00ea, 0x001a, 0x0008, 0x0019, 0x00ee, 0x01ef, 0x07ed,
    0x03f0, 0x00f2, 0x0073, 0x000b, 0x0000, 0x000a, 0x0071, 0x00f3, 0x07e9,
    0x07ef, 0x01ee, 0x00ef, 0x0018, 0x0009, 0x001b, 0x00eb, 0x01e9, 0x07ec,
    0x07f6, 0x03eb, 0x01f3, 0x00ed, 0x0072, 0x00e9, 0x01f1, 0x03ed, 0x07f7,
    0x0ff6, 0x07f0, 0x03e9, 0x01ed, 0x00f1, 0x01ea, 0x03ec, 0x07f8, 0x0ff9,
    0x1ffc, 0x0ffc, 0x0ff5, 0x07ea, 0x03f3, 0x03f2, 0x07f5, 0x0ffb, 0x1ffe,
};

static const guint8 spectrum5_lengths[81] = {
    13, 12, 11, 11, 10, 11, 11, 12, 13, 12, 11, 10, 9,  8,  9,  10, 11,
    12, 12, 10, 9,  8,  7,  8,  9,  10, 11, 11, 9,  8,  5,  4,  5,  8,
    9,  11, 10, 8,  7,  4,  1,  4,  7,  8,  11, 11, 9,  8,  5,  4,  5,
    8,  9,  11, 11, 10, 9,  8,  7,  8,  9,  10, 11, 12, 11, 10, 9,  8,
    9,  10, 11, 12, 13, 12, 12, 11, 10, 10, 11, 12, 13,
};

static const guint32 spectrum6_codes[81] = {
    0x7fe, 0x3fd, 0x1f1, 0x1eb, 0x1f4, 0x1ea, 0x1f0, 0x3fc, 0x7fd,
    0x3f6, 0x1e5, 0x0ea, 0x06c, 0x071, 0x068, 0x0f0, 0x1e6, 0x3f7,
    0x1f3, 0x0ef, 0x032, 0x027, 0x028, 0x026, 0x031, 0x0eb, 0x1f7,
    0x1e8, 0x06f, 0x02e, 0x008, 0x004, 0x006, 0x029, 0x06b, 0x1ee,
    0x1ef, 0x072, 0x02d, 0x002, 0x000, 0x003, 0x02f, 0x073, 0x1fa,
    0x1e7, 0x06e, 0x02b, 0x007, 0x001, 0x005, 0x02c, 0x06d, 0x1ec,
    0x1f9, 0x0ee, 0x030, 0x024, 0x02a, 0x025, 0x033, 0x0ec, 0x1f2,
    0x3f8, 0x1e4, 0x0ed, 0x06a, 0x070, 0x069, 0x074, 0x0f1, 0x3fa,
    0x7ff, 0x3f9, 0x1f6, 0x1ed, 0x1f8, 0x1e9, 0x1f5, 0x3fb, 0x7fc,
};

static const guint8 spectrum6_lengths[81] = {
    11, 10, 9, 9, 9, 9, 9, 10, 11, 10, 9, 8, 7, 7, 7, 8, 9,
    10, 9,  8, 6, 6, 6, 6, 6,  8,  9,  9, 7, 6, 4, 4, 4, 6,
    7,  9,  9, 7, 6, 4, 4, 4,  6,  7,  9, 9, 7, 6, 4, 4, 4,
    6,  7,  9, 9, 8, 6, 6, 6,  6,  6,  8, 9, 10, 9, 8, 7, 7,
    7,  7,  8, 10, 11, 10, 9, 9, 9, 9, 9, 10, 11,
};

static const guint32 spectrum7_codes[64] = {
    0x000, 0x005, 0x037, 0x074, 0x0f2, 0x1eb, 0x3ed, 0x7f7,
    0x004, 0x00c, 0x035, 0x071, 0x0ec, 0x0ee, 0x1ee, 0x1f5,
    0x036, 0x034, 0x072, 0x0ea, 0x0f1, 0x1e9, 0x1f3, 0x3f5,
    0x073, 0x070, 0x0eb, 0x0f0, 0x1f1, 0x1f0, 0x3ec, 0x3fa,
    0x0f3, 0x0ed, 0x1e8, 0x1ef, 0x3ef, 0x3f1, 0x3f9, 0x7fb,
    0x1ed, 0x0ef, 0x1ea, 0x1f2, 0x3f3, 0x3f8, 0x7f9, 0x7fc,
    0x3ee, 0x1ec, 0x1f4, 0x3f4, 0x3f7, 0x7f8, 0xffd, 0xffe,
    0x7f6, 0x3f0, 0x3f2, 0x3f6, 0x7fa, 0x7fd, 0xffc, 0xfff,
};

static const guint8 spectrum7_lengths[64] = {
    1,  3,  6,  7,  8,  9,  10, 11, 3,  4,  6,  7,  8,  8,  9,  9,
    6,  6,  7,  8,  8,  9,  9,  10, 7,  7,  8,  8,  9,  9,  10, 10,
    8,  8,  9,  9,  10, 10, 10, 11, 9,  8,  9,  9,  10, 10, 11, 11,
    10, 9,  9,  10, 10, 11, 12, 12, 11, 10, 10, 10, 11, 11, 12, 12,
};

static const guint32 spectrum8_codes[64] = {
    0x00e, 0x005, 0x010, 0x030, 0x06f, 0x0f1, 0x1fa, 0x3fe,
    0x003, 0x000, 0x004, 0x012, 0x02c, 0x06a, 0x075, 0x0f8,
    0x00f, 0x002, 0x006, 0x014, 0x02e, 0x069, 0x072, 0x0f5,
    0x02f, 0x011, 0x013, 0x02a, 0x032, 0x06c, 0x0ec, 0x0fa,
    0x071, 0x02b, 0x02d, 0x031, 0x06d, 0x070, 0x0f2, 0x1f9,
    0x0ef, 0x068, 0x033, 0x06b, 0x06e, 0x0ee, 0x0f9, 0x3fc,
    0x1f8, 0x074, 0x073, 0x0ed, 0x0f0, 0x0f6, 0x1f6, 0x1fd,
    0x3fd, 0x0f3, 0x0f4, 0x0f7, 0x1f7, 0x1fb, 0x1fc, 0x3ff,
};

static const guint8 spectrum8_lengths[64] = {
    5,  4, 5, 6, 7, 8, 9, 10, 4, 3, 4, 5, 6, 7, 7, 8,
    5,  4, 4, 5, 6, 7, 7, 8,  6, 5, 5, 6, 6, 7, 8, 8,
    7,  6, 6, 6, 7, 7, 8, 9,  8, 7, 6, 7, 7, 8, 8, 10,
    9,  7, 7, 8, 8, 8, 9, 9,  10, 8, 8, 8, 9, 9, 9, 10,
};

static const guint32 spectrum9_codes[169] = {
    0x0000, 0x0005, 0x0037, 0x00e7, 0x01de, 0x03ce, 0x03d9, 0x07c8, 0x07cd,
    0x0fc8, 0x0fdd, 0x1fe4, 0x1fec, 0x0004, 0x000c, 0x0035, 0x0072, 0x00ea,
    0x00ed, 0x01e2, 0x03d1, 0x03d3, 0x03e0, 0x07d8, 0x0fcf, 0x0fd5, 0x0036,
    0x0034, 0x0071, 0x00e8, 0x00ec, 0x01e1, 0x03cf, 0x03dd, 0x03db, 0x07d0,
    0x0fc7, 0x0fd4, 0x0fe4, 0x00e6, 0x0070, 0x00e9, 0x01dd, 0x01e3, 0x03d2,
    0x03dc, 0x07cc, 0x07ca, 0x07de, 0x0fd8, 0x0fea, 0x1fdb, 0x01df, 0x00eb,
    0x01dc, 0x01e6, 0x03d5, 0x03de, 0x07cb, 0x07dd, 0x07dc, 0x0fcd, 0x0fe2,
    0x0fe7, 0x1fe1, 0x03d0, 0x01e0, 0x01e4, 0x03d6, 0x07c5, 0x07d1, 0x07db,
    0x0fd2, 0x07e0, 0x0fd9, 0x0feb, 0x1fe3, 0x1fe9, 0x07c4, 0x01e5, 0x03d7,
    0x07c6, 0x07cf, 0x07da, 0x0fcb, 0x0fda, 0x0fe3, 0x0fe9, 0x1fe6, 0x1ff3,
    0x1ff7, 0x07d3, 0x03d8, 0x03e1, 0x07d4, 0x07d9, 0x0fd3, 0x0fde, 0x1fdd,
    0x1fd9, 0x1fe2, 0x1fea, 0x1ff1, 0x1ff6, 0x07d2, 0x03d4, 0x03da, 0x07c7,
    0x07d7, 0x07e2, 0x0fce, 0x0fdb, 0x1fd8, 0x1fee, 0x3ff0, 0x1ff4, 0x3ff2,
    0x07e1, 0x03df, 0x07c9, 0x07d6, 0x0fca, 0x0fd0, 0x0fe5, 0x0fe6, 0x1feb,
    0x1fef, 0x3ff3, 0x3ff4, 0x3ff5, 0x0fe0, 0x07ce, 0x07d5, 0x0fc6, 0x0fd1,
    0x0fe1, 0x1fe0, 0x1fe8, 0x1ff0, 0x3ff1, 0x3ff8, 0x3ff6, 0x7ffc, 0x0fe8,
    0x07df, 0x0fc9, 0x0fd7, 0x0fdc, 0x1fdc, 0x1fdf, 0x1fed, 0x1ff5, 0x3ff9,
    0x3ffb, 0x7ffd, 0x7ffe, 0x1fe7, 0x0fcc, 0x0fd6, 0x0fdf, 0x1fde, 0x1fda,
    0x1fe5, 0x1ff2, 0x3ffa, 0x3ff7, 0x3ffc, 0x3ffd, 0x7fff,
};

static const guint8 spectrum9_lengths[169] = {
    1,  3,  6,  8,  9,  10, 10, 11, 11, 12, 12, 13, 13, 3,  4,  6,  7,
    8,  8,  9,  10, 10, 10, 11, 12, 12, 6,  6,  7,  8,  8,  9,  10, 10,
    10, 11, 12, 12, 12, 8,  7,  8,  9,  9,  10, 10, 11, 11, 11, 12, 12,
    13, 9,  8,  9,  9,  10, 10, 11, 11, 11, 12, 12, 12, 13, 10, 9,  9,
    10, 11, 11, 11, 12, 11, 12, 12, 13, 13, 11, 9,  10, 11, 11, 11, 12,
    12, 12, 12, 13, 13, 13, 11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13,
    13, 13, 11, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 13, 14, 11, 10,
    11, 11, 12, 12, 12, 12, 13, 13, 14, 14, 14, 12, 11, 11, 12, 12, 12,
    13, 13, 13, 14, 14, 14, 15, 12, 11, 12, 12, 12, 13, 13, 13, 13, 14,
    14, 15, 15, 13, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15,
};

static const guint32 spectrum10_codes[169] = {
    0x022, 0x008, 0x01d, 0x026, 0x05f, 0x0d3, 0x1cf, 0x3d0, 0x3d7, 0x3ed,
    0x7f0, 0x7f6, 0xffd, 0x007, 0x000, 0x001, 0x009, 0x020, 0x054, 0x060,
    0x0d5, 0x0dc, 0x1d4, 0x3cd, 0x3de, 0x7e7, 0x01c, 0x002, 0x006, 0x00c,
    0x01e, 0x028, 0x05b, 0x0cd, 0x0d9, 0x1ce, 0x1dc, 0x3d9, 0x3f1, 0x025,
    0x00b, 0x00a, 0x00d, 0x024, 0x057, 0x061, 0x0cc, 0x0dd, 0x1cc, 0x1de,
    0x3d3, 0x3e7, 0x05d, 0x021, 0x01f, 0x023, 0x027, 0x059, 0x064, 0x0d8,
    0x0df, 0x1d2, 0x1e2, 0x3dd, 0x3ee, 0x0d1, 0x055, 0x029, 0x056, 0x058,
    0x062, 0x0ce, 0x0e0, 0x0e2, 0x1da, 0x3d4, 0x3e3, 0x7eb, 0x1c9, 0x05e,
    0x05a, 0x05c, 0x063, 0x0ca, 0x0da, 0x1c7, 0x1ca, 0x1e0, 0x3db, 0x3e8,
    0x7ec, 0x1e3, 0x0d2, 0x0cb, 0x0d0, 0x0d7, 0x0db, 0x1c6, 0x1d5, 0x1d8,
    0x3ca, 0x3da, 0x7ea, 0x7f1, 0x1e1, 0x0d4, 0x0cf, 0x0d6, 0x0de, 0x0e1,
    0x1d0, 0x1d6, 0x3d1, 0x3d5, 0x3f2, 0x7ee, 0x7fb, 0x3e9, 0x1cd, 0x1c8,
    0x1cb, 0x1d1, 0x1d7, 0x1df, 0x3cf, 0x3e0, 0x3ef, 0x7e6, 0x7f8, 0xffa,
    0x3eb, 0x1dd, 0x1d3, 0x1d9, 0x1db, 0x3d2, 0x3cc, 0x3dc, 0x3ea, 0x7ed,
    0x7f3, 0x7f9, 0xff9, 0x7f2, 0x3ce, 0x1e4, 0x3cb, 0x3d8, 0x3d6, 0x3e2,
    0x3e5, 0x7e8, 0x7f4, 0x7f5, 0x7f7, 0xffb, 0x7fa, 0x3ec, 0x3df, 0x3e1,
    0x3e4, 0x3e6, 0x3f0, 0x7e9, 0x7ef, 0xff8, 0xffe, 0xffc, 0xfff,
};

static const guint8 spectrum10_lengths[169] = {
    6,  5,  6,  6,  7,  8,  9,  10, 10, 10, 11, 11, 12, 5,  4,  4,  5,
    6,  7,  7,  8,  8,  9,  10, 10, 11, 6,  4,  5,  5,  6,  6,  7,  8,
    8,  9,  9,  10, 10, 6,  5,  5,  5,  6,  7,  7,  8,  8,  9,  9,  10,
    10, 7,  6,  6,  6,  6,  7,  7,  8,  8,  9,  9,  10, 10, 8,  7,  6,
    7,  7,  7,  8,  8,  8,  9,  10, 10, 11, 9,  7,  7,  7,  7,  8,  8,
    9,  9,  9,  10, 10, 11, 9,  8,  8,  8,  8,  8,  9,  9,  9,  10, 10,
    11, 11, 9,  8,  8,  8,  8,  8,  9,  9,  10, 10, 10, 11, 11, 10, 9,
    9,  9,  9,  9,  9,  10, 10, 10, 11, 11, 12, 10, 9,  9,  9,  9,  10,
    10, 10, 10, 11, 11, 11, 12, 11, 10, 9,  10, 10, 10, 10, 10, 11, 11,
    11, 11, 12, 11, 10, 10, 10, 10, 10, 10, 11, 11, 12, 12, 12, 12,
};

static const guint32 spectrum11_codes[289] = {
    0x000, 0x006, 0x019, 0x03d, 0x09c, 0x0c6, 0x1a7, 0x390, 0x3c2, 0x3df,
    0x7e6, 0x7f3, 0xffb, 0x7ec, 0xffa, 0xffe, 0x38e, 0x005, 0x001, 0x008,
    0x014, 0x037, 0x042, 0x092, 0x0af, 0x191, 0x1a5, 0x1b5, 0x39e, 0x3c0,
    0x3a2, 0x3cd, 0x7d6, 0x0ae, 0x017, 0x007, 0x009, 0x018, 0x039, 0x040,
    0x08e, 0x0a3, 0x0b8, 0x199, 0x1ac, 0x1c1, 0x3b1, 0x396, 0x3be, 0x3ca,
    0x09d, 0x03c, 0x015, 0x016, 0x01a, 0x03b, 0x044, 0x091, 0x0a5, 0x0be,
    0x196, 0x1ae, 0x1b9, 0x3a1, 0x391, 0x3a5, 0x3d5, 0x094, 0x09a, 0x036,
    0x038, 0x03a, 0x041, 0x08c, 0x09b, 0x0b0, 0x0c3, 0x19e, 0x1ab, 0x1bc,
    0x39f, 0x38f, 0x3a9, 0x3cf, 0x093, 0x0bf, 0x03e, 0x03f, 0x043, 0x045,
    0x09e, 0x0a7, 0x0b9, 0x194, 0x1a2, 0x1ba, 0x1c3, 0x3a6, 0x3a7, 0x3bb,
    0x3d4, 0x09f, 0x1a0, 0x08f, 0x08d, 0x090, 0x098, 0x0a6, 0x0b6, 0x0c4,
    0x19f, 0x1af, 0x1bf, 0x399, 0x3bf, 0x3b4, 0x3c9, 0x3e7, 0x0a8, 0x1b6,
    0x0ab, 0x0a4, 0x0aa, 0x0b2, 0x0c2, 0x0c5, 0x198, 0x1a4, 0x1b8, 0x38c,
    0x3a4, 0x3c4, 0x3c6, 0x3dd, 0x3e8, 0x0ad, 0x3af, 0x192, 0x0bd, 0x0bc,
    0x18e, 0x197, 0x19a, 0x1a3, 0x1b1, 0x38d, 0x398, 0x3b7, 0x3d3, 0x3d1,
    0x3db, 0x7dd, 0x0b4, 0x3de, 0x1a9, 0x19b, 0x19c, 0x1a1, 0x1aa, 0x1ad,
    0x1b3, 0x38b, 0x3b2, 0x3b8, 0x3ce, 0x3e1, 0x3e0, 0x7d2, 0x7e5, 0x0b7,
    0x7e3, 0x1bb, 0x1a8, 0x1a6, 0x1b0, 0x1b2, 0x1b7, 0x39b, 0x39a, 0x3ba,
    0x3b5, 0x3d6, 0x7d7, 0x3e4, 0x7d8, 0x7ea, 0x0ba, 0x7e8, 0x3a0, 0x1bd,
    0x1b4, 0x38a, 0x1c4, 0x392, 0x3aa, 0x3b0, 0x3bc, 0x3d7, 0x7d4, 0x7dc,
    0x7db, 0x7d5, 0x7f0, 0x0c1, 0x7fb, 0x3c8, 0x3a3, 0x395, 0x39d, 0x3ac,
    0x3ae, 0x3c5, 0x3d8, 0x3e2, 0x3e6, 0x7e4, 0x7e7, 0x7e0, 0x7e9, 0x7f7,
    0x190, 0x7f2, 0x393, 0x1be, 0x1c0, 0x394, 0x397, 0x3ad, 0x3c3, 0x3c1,
    0x3d2, 0x7da, 0x7d9, 0x7df, 0x7eb, 0x7f4, 0x7fa, 0x195, 0x7f8, 0x3bd,
    0x39c, 0x3ab, 0x3a8, 0x3b3, 0x3b9, 0x3d0, 0x3e3, 0x3e5, 0x7e2, 0x7de,
    0x7ed, 0x7f1, 0x7f9, 0x7fc, 0x193, 0xffd, 0x3dc, 0x3b6, 0x3c7, 0x3cc,
    0x3cb, 0x3d9, 0x3da, 0x7d3, 0x7e1, 0x7ee, 0x7ef, 0x7f5, 0x7f6, 0xffc,
    0xfff, 0x19d, 0x1c2, 0x0b5, 0x0a1, 0x096, 0x097, 0x095, 0x099, 0x0a0,
    0x0a2, 0x0ac, 0x0a9, 0x0b1, 0x0b3, 0x0bb, 0x0c0, 0x18f, 0x004,
};

static const guint8 spectrum11_lengths[289] = {
    4,  5,  6,  7,  8,  8,  9,  10, 10, 10, 11, 11, 12, 11, 12, 12, 10,
    5,  4,  5,  6,  7,  7,  8,  8,  9,  9,  9,  10, 10, 10, 10, 11, 8,
    6,  5,  5,  6,  7,  7,  8,  8,  8,  9,  9,  9,  10, 10, 10, 10, 8,
    7,  6,  6,  6,  7,  7,  8,  8,  8,  9,  9,  9,  10, 10, 10, 10, 8,
    8,  7,  7,  7,  7,  8,  8,  8,  8,  9,  9,  9,  10, 10, 10, 10, 8,
    8,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 8,
    9,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  10, 10, 10, 10, 10, 8,
    9,  8,  8,  8,  8,  8,  8,  9,  9,  9,  10, 10, 10, 10, 10, 10, 8,
    10, 9,  8,  8,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 11, 8,
    10, 9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 11, 11, 8,
    11, 9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 11, 10, 11, 11, 8,
    11, 10, 9,  9,  10, 9,  10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8,
    11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 9,
    11, 10, 9,  9,  10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9,
    11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9,
    12, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 9,
    9,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  9,  5,
};

static gsize            aac_tables_initialized;
static gboolean         aac_tables_usable;
static aac_huffman_node huffman_pool[AAC_HUFFMAN_POOL_SIZE];
static aac_codebook     scalefactor_codebook;
static aac_codebook     spectral_codebooks[12];
static float            power_four_thirds[AAC_POWER_TABLE_SIZE];
static float            sine_window_long[AAC_FRAME_LENGTH];
static float            sine_window_short[AAC_SHORT_LENGTH];
static float            kbd_window_long[AAC_FRAME_LENGTH];
static float            kbd_window_short[AAC_SHORT_LENGTH];
static aac_complex      long_pre_twiddle[AAC_FRAME_LENGTH / 2];
static aac_complex      long_post_twiddle[AAC_FRAME_LENGTH / 2];
static aac_complex      long_fft_twiddle[AAC_FRAME_LENGTH / 4];
static guint16          long_bit_reverse[AAC_FRAME_LENGTH / 2];
static aac_complex      short_pre_twiddle[AAC_SHORT_LENGTH / 2];
static aac_complex      short_post_twiddle[AAC_SHORT_LENGTH / 2];
static aac_complex      short_fft_twiddle[AAC_SHORT_LENGTH / 4];
static guint16          short_bit_reverse[AAC_SHORT_LENGTH / 2];
static aac_imdct_plan   long_imdct_plan;
static aac_imdct_plan   short_imdct_plan;

/* Builds a binary decode tree from codes; leaves hold -(symbol+1). FALSE on conflict. */
static gboolean
build_huffman_tree(aac_codebook *book, aac_huffman_node *nodes, int capacity)
{
    memset(nodes, 0, sizeof(*nodes) * (gsize)capacity);
    int used = 1;
    for (int symbol = 0; symbol < book->symbol_count; symbol++) {
        int length = book->lengths[symbol];
        guint32 code = book->codes[symbol];
        if (length < 1 || length > AAC_MAX_HUFFMAN_LENGTH ||
            (code >> length) != 0)
            return FALSE;
        int node = 0;
        for (int bit_index = length - 1; bit_index >= 0; bit_index--) {
            int bit = (int)((code >> bit_index) & 1u);
            int child = nodes[node].child[bit];
            if (bit_index == 0) {
                if (child != 0) return FALSE;
                nodes[node].child[bit] = (gint16)(-(symbol + 1));
                break;
            }
            if (child < 0) return FALSE;
            if (child == 0) {
                if (used >= capacity) return FALSE;
                child = used++;
                nodes[node].child[bit] = (gint16)child;
            }
            node = child;
        }
    }
    book->nodes = nodes;
    return TRUE;
}

static void
describe_codebook(aac_codebook *book, const guint32 *codes,
                  const guint8 *lengths, int symbol_count, int dimension,
                  int modulus, gboolean is_signed)
{
    book->codes = codes;
    book->lengths = lengths;
    book->symbol_count = symbol_count;
    book->dimension = dimension;
    book->modulus = modulus;
    book->is_signed = is_signed;
    book->nodes = NULL;
}

/* Describes the scalefactor and 11 spectral Huffman codebooks and builds their trees. */
static gboolean
build_codebooks(void)
{
    describe_codebook(&scalefactor_codebook, scalefactor_codes,
                      scalefactor_lengths, 121, 1, 121, FALSE);
    describe_codebook(&spectral_codebooks[1], spectrum1_codes,
                      spectrum1_lengths, 81, 4, 3, TRUE);
    describe_codebook(&spectral_codebooks[2], spectrum2_codes,
                      spectrum2_lengths, 81, 4, 3, TRUE);
    describe_codebook(&spectral_codebooks[3], spectrum3_codes,
                      spectrum3_lengths, 81, 4, 3, FALSE);
    describe_codebook(&spectral_codebooks[4], spectrum4_codes,
                      spectrum4_lengths, 81, 4, 3, FALSE);
    describe_codebook(&spectral_codebooks[5], spectrum5_codes,
                      spectrum5_lengths, 81, 2, 9, TRUE);
    describe_codebook(&spectral_codebooks[6], spectrum6_codes,
                      spectrum6_lengths, 81, 2, 9, TRUE);
    describe_codebook(&spectral_codebooks[7], spectrum7_codes,
                      spectrum7_lengths, 64, 2, 8, FALSE);
    describe_codebook(&spectral_codebooks[8], spectrum8_codes,
                      spectrum8_lengths, 64, 2, 8, FALSE);
    describe_codebook(&spectral_codebooks[9], spectrum9_codes,
                      spectrum9_lengths, 169, 2, 13, FALSE);
    describe_codebook(&spectral_codebooks[10], spectrum10_codes,
                      spectrum10_lengths, 169, 2, 13, FALSE);
    describe_codebook(&spectral_codebooks[11], spectrum11_codes,
                      spectrum11_lengths, 289, 2, 17, FALSE);

    int pool_used = 0;
    int capacity = 2 * scalefactor_codebook.symbol_count;
    if (!build_huffman_tree(&scalefactor_codebook, huffman_pool, capacity))
        return FALSE;
    pool_used += capacity;
    for (int index = 1; index <= AAC_ESCAPE_CODEBOOK; index++) {
        aac_codebook *book = &spectral_codebooks[index];
        capacity = 2 * book->symbol_count;
        if (pool_used + capacity > AAC_HUFFMAN_POOL_SIZE) return FALSE;
        if (!build_huffman_tree(book, huffman_pool + pool_used, capacity))
            return FALSE;
        pool_used += capacity;
    }
    return TRUE;
}

/* Zeroth-order modified Bessel function of the first kind, by power series. */
static double
bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    double half = x / 2.0;
    for (int k = 1; k < 64; k++) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* Computes the rising half of a Kaiser-Bessel-derived window with the given alpha. */
static void
build_kbd_window(float *rising_half, int half_length, double alpha)
{
    int window_length = 2 * half_length;
    double quarter = window_length / 4.0;
    double kernel[AAC_FRAME_LENGTH + 1];
    double total = 0.0;
    for (int p = 0; p <= half_length; p++) {
        double ratio = (p - quarter) / quarter;
        double argument = 1.0 - ratio * ratio;
        if (argument < 0.0) argument = 0.0;
        kernel[p] = bessel_i0(G_PI * alpha * sqrt(argument));
        total += kernel[p];
    }
    double running = 0.0;
    for (int n = 0; n < half_length; n++) {
        running += kernel[n];
        rising_half[n] = (float)sqrt(running / total);
    }
}

/* Computes the rising half of the AAC sine window. */
static void
build_sine_window(float *rising_half, int half_length)
{
    int window_length = 2 * half_length;
    for (int n = 0; n < half_length; n++)
        rising_half[n] = (float)sin(G_PI / window_length * (n + 0.5));
}

/* Precomputes twiddles and bit-reversal for an FFT-based IMDCT of the given length. */
static void
build_imdct_plan(aac_imdct_plan *plan, int length, aac_complex *pre,
                 aac_complex *post, aac_complex *fft, guint16 *bit_reverse)
{
    int half = length / 2;
    int fft_size = half / 2;
    for (int n = 0; n < fft_size; n++) {
        double pre_angle = -G_PI * (n + 0.25) / half;
        pre[n].re = (float)cos(pre_angle);
        pre[n].im = (float)sin(pre_angle);
        double post_angle = -G_PI * n / half;
        post[n].re = (float)cos(post_angle);
        post[n].im = (float)sin(post_angle);
    }
    for (int n = 0; n < fft_size / 2; n++) {
        double angle = -2.0 * G_PI * n / fft_size;
        fft[n].re = (float)cos(angle);
        fft[n].im = (float)sin(angle);
    }
    int bits = 0;
    while ((1 << bits) < fft_size) bits++;
    for (int n = 0; n < fft_size; n++) {
        int reversed = 0;
        for (int b = 0; b < bits; b++)
            if (n & (1 << b)) reversed |= 1 << (bits - 1 - b);
        bit_reverse[n] = (guint16)reversed;
    }
    plan->length = length;
    plan->pre_twiddle = pre;
    plan->post_twiddle = post;
    plan->fft_twiddle = fft;
    plan->bit_reverse = bit_reverse;
    plan->scale = (float)(2.0 / length / 32768.0);
}

/* One-time init of codebooks, x^(4/3) table, windows and IMDCT plans (thread-safe). */
static void
initialize_tables(void)
{
    if (!g_once_init_enter(&aac_tables_initialized)) return;
    aac_tables_usable = build_codebooks();
    for (int i = 0; i < AAC_POWER_TABLE_SIZE; i++)
        power_four_thirds[i] = (float)pow((double)i, 4.0 / 3.0);
    build_sine_window(sine_window_long, AAC_FRAME_LENGTH);
    build_sine_window(sine_window_short, AAC_SHORT_LENGTH);
    build_kbd_window(kbd_window_long, AAC_FRAME_LENGTH, 4.0);
    build_kbd_window(kbd_window_short, AAC_SHORT_LENGTH, 6.0);
    build_imdct_plan(&long_imdct_plan, 2 * AAC_FRAME_LENGTH,
                     long_pre_twiddle, long_post_twiddle, long_fft_twiddle,
                     long_bit_reverse);
    build_imdct_plan(&short_imdct_plan, 2 * AAC_SHORT_LENGTH,
                     short_pre_twiddle, short_post_twiddle,
                     short_fft_twiddle, short_bit_reverse);
    g_once_init_leave(&aac_tables_initialized, 1);
}

static void
bit_reader_init(aac_bit_reader *reader, const guint8 *data, gsize len)
{
    reader->data = data;
    reader->size_bits = len * 8;
    reader->position = 0;
    reader->overrun = FALSE;
}

/* Reads up to 32 bits MSB-first; on overrun sets the flag and returns 0. */
static guint32
read_bits(aac_bit_reader *reader, int count)
{
    if (count <= 0) return 0;
    if (reader->overrun || count > 32 ||
        reader->size_bits - reader->position < (gsize)count) {
        reader->overrun = TRUE;
        reader->position = reader->size_bits;
        return 0;
    }
    guint32 value = 0;
    while (count > 0) {
        gsize byte_index = reader->position >> 3;
        int available = 8 - (int)(reader->position & 7);
        int take = count < available ? count : available;
        guint32 byte = reader->data[byte_index];
        guint32 bits = (byte >> (available - take)) & ((1u << take) - 1u);
        value = (value << take) | bits;
        reader->position += (gsize)take;
        count -= take;
    }
    return value;
}

static int
read_bit(aac_bit_reader *reader)
{
    return (int)read_bits(reader, 1);
}

/* Advances the reader by count bits; FALSE and overrun flag if past the end. */
static gboolean
skip_bits(aac_bit_reader *reader, gsize count)
{
    if (reader->overrun || reader->size_bits - reader->position < count) {
        reader->overrun = TRUE;
        reader->position = reader->size_bits;
        return FALSE;
    }
    reader->position += count;
    return TRUE;
}

/* Advances the reader to the next byte boundary. */
static void
byte_align(aac_bit_reader *reader)
{
    gsize misalignment = reader->position & 7;
    if (misalignment) skip_bits(reader, 8 - misalignment);
}

/* Decodes one symbol by walking the codebook tree; -1 on invalid code or overrun. */
static int
read_huffman_symbol(aac_bit_reader *reader, const aac_codebook *book)
{
    int node = 0;
    for (int depth = 0; depth < AAC_MAX_HUFFMAN_LENGTH; depth++) {
        int bit = read_bit(reader);
        if (reader->overrun) return -1;
        int child = book->nodes[node].child[bit];
        if (child == 0) return -1;
        if (child < 0) return -child - 1;
        node = child;
    }
    return -1;
}

/* Maps an explicit sample rate to the nearest sampling-frequency index (14496-3 table). */
static int
sampling_index_for_rate(guint32 rate)
{
    static const guint32 lower_bounds[AAC_SAMPLING_INDEX_COUNT - 1] = {
        92017, 75132, 55426, 46009, 37566, 27713,
        23004, 18783, 13856, 11502, 9391,  0,
    };
    for (int index = 0; index < AAC_SAMPLING_INDEX_COUNT - 1; index++)
        if (rate >= lower_bounds[index]) return index;
    return AAC_SAMPLING_INDEX_COUNT - 2;
}

/* Selects long/short scalefactor-band offset tables for the sampling index. */
static void
select_band_tables(ns_aac_decoder *dec)
{
    switch (dec->sampling_index) {
    case 0:
    case 1:
        dec->swb_offset_long = swb_offset_long_96;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_96) - 1;
        dec->swb_offset_short = swb_offset_short_96;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_96) - 1;
        break;
    case 2:
        dec->swb_offset_long = swb_offset_long_64;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_64) - 1;
        dec->swb_offset_short = swb_offset_short_96;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_96) - 1;
        break;
    case 3:
    case 4:
        dec->swb_offset_long = swb_offset_long_48;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_48) - 1;
        dec->swb_offset_short = swb_offset_short_48;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_48) - 1;
        break;
    case 5:
        dec->swb_offset_long = swb_offset_long_32;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_32) - 1;
        dec->swb_offset_short = swb_offset_short_48;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_48) - 1;
        break;
    case 6:
    case 7:
        dec->swb_offset_long = swb_offset_long_24;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_24) - 1;
        dec->swb_offset_short = swb_offset_short_24;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_24) - 1;
        break;
    case 8:
    case 9:
    case 10:
        dec->swb_offset_long = swb_offset_long_16;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_16) - 1;
        dec->swb_offset_short = swb_offset_short_16;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_16) - 1;
        break;
    default:
        dec->swb_offset_long = swb_offset_long_8;
        dec->num_swb_long = G_N_ELEMENTS(swb_offset_long_8) - 1;
        dec->swb_offset_short = swb_offset_short_8;
        dec->num_swb_short = G_N_ELEMENTS(swb_offset_short_8) - 1;
        break;
    }
}

/* Parses an AudioSpecificConfig; returns a decoder for mono/stereo AAC-LC or NULL. */
ns_aac_decoder *
ns_aac_decoder_new(const guint8 *asc, gsize asc_len)
{
    if (!asc || asc_len < 2 || asc_len > 4096) return NULL;
    initialize_tables();
    if (!aac_tables_usable) return NULL;

    aac_bit_reader reader;
    bit_reader_init(&reader, asc, asc_len);
    int object_type = (int)read_bits(&reader, 5);
    if (object_type == 31) object_type = 32 + (int)read_bits(&reader, 6);
    int sampling_index = (int)read_bits(&reader, 4);
    int sample_rate;
    if (sampling_index == 15) {
        guint32 explicit_rate = read_bits(&reader, 24);
        if (explicit_rate < 7000 || explicit_rate > 96000) return NULL;
        sample_rate = (int)explicit_rate;
        sampling_index = sampling_index_for_rate(explicit_rate);
    } else if (sampling_index < AAC_SAMPLING_INDEX_COUNT) {
        sample_rate = aac_sample_rates[sampling_index];
    } else {
        return NULL;
    }
    int channel_configuration = (int)read_bits(&reader, 4);
    if (object_type != AAC_OBJECT_TYPE_LOW_COMPLEXITY) return NULL;
    int frame_length_flag = read_bit(&reader);
    int depends_on_core_coder = read_bit(&reader);
    if (depends_on_core_coder) read_bits(&reader, 14);
    read_bit(&reader);
    if (reader.overrun || frame_length_flag) return NULL;
    if (channel_configuration < 1 || channel_configuration > 2) return NULL;

    ns_aac_decoder *dec = g_new0(ns_aac_decoder, 1);
    dec->sample_rate = sample_rate;
    dec->sampling_index = sampling_index;
    dec->channels = channel_configuration;
    select_band_tables(dec);
    ns_aac_decoder_reset(dec);
    return dec;
}

void
ns_aac_decoder_free(ns_aac_decoder *dec)
{
    g_free(dec);
}

int
ns_aac_decoder_sample_rate(const ns_aac_decoder *dec)
{
    return dec ? dec->sample_rate : 0;
}

int
ns_aac_decoder_channels(const ns_aac_decoder *dec)
{
    return dec ? dec->channels : 0;
}

/* Clears overlap buffers and window history and reseeds the noise generator. */
void
ns_aac_decoder_reset(ns_aac_decoder *dec)
{
    if (!dec) return;
    memset(dec->overlap, 0, sizeof(dec->overlap));
    for (int ch = 0; ch < AAC_MAX_CHANNELS; ch++)
        dec->previous_window_shape[ch] = 0;
    dec->noise_state = 0x1f2e3d4cu;
}

/* Parses ics_info: window sequence/shape, max_sfb and short-window grouping. */
static gboolean
read_ics_info(ns_aac_decoder *dec, aac_bit_reader *reader, aac_ics_info *info)
{
    if (read_bit(reader)) return FALSE;
    info->window_sequence = (int)read_bits(reader, 2);
    info->window_shape = read_bit(reader);
    if (info->window_sequence == AAC_EIGHT_SHORT_SEQUENCE) {
        info->max_sfb = (int)read_bits(reader, 4);
        int grouping = (int)read_bits(reader, 7);
        info->num_windows = AAC_MAX_WINDOWS;
        info->num_window_groups = 1;
        info->window_group_length[0] = 1;
        for (int window = 1; window < AAC_MAX_WINDOWS; window++) {
            if (grouping & (1 << (7 - window))) {
                info->window_group_length[info->num_window_groups - 1]++;
            } else {
                info->window_group_length[info->num_window_groups] = 1;
                info->num_window_groups++;
            }
        }
        info->window_length = AAC_SHORT_LENGTH;
        info->num_swb = dec->num_swb_short;
        info->swb_offset = dec->swb_offset_short;
        info->tns_max_bands = tns_max_bands_short[dec->sampling_index];
        info->tns_max_order = AAC_MAX_TNS_ORDER_SHORT;
    } else {
        info->max_sfb = (int)read_bits(reader, 6);
        if (read_bit(reader)) return FALSE;
        info->num_windows = 1;
        info->num_window_groups = 1;
        info->window_group_length[0] = 1;
        info->window_length = AAC_FRAME_LENGTH;
        info->num_swb = dec->num_swb_long;
        info->swb_offset = dec->swb_offset_long;
        info->tns_max_bands = tns_max_bands_long[dec->sampling_index];
        info->tns_max_order = AAC_MAX_TNS_ORDER_LONG;
    }
    if (info->max_sfb > info->num_swb) return FALSE;
    return !reader->overrun;
}

/* Parses section_data, assigning a codebook to each scalefactor band per group. */
static gboolean
read_section_data(aac_bit_reader *reader, aac_channel *channel)
{
    const aac_ics_info *info = &channel->info;
    int length_bits = info->window_sequence == AAC_EIGHT_SHORT_SEQUENCE ? 3 : 5;
    int escape_value = (1 << length_bits) - 1;
    memset(channel->band_type, 0, sizeof(channel->band_type));
    for (int group = 0; group < info->num_window_groups; group++) {
        int band = 0;
        while (band < info->max_sfb) {
            int codebook = (int)read_bits(reader, 4);
            if (codebook == AAC_RESERVED_CODEBOOK) return FALSE;
            int section_length = 0;
            int increment;
            do {
                increment = (int)read_bits(reader, length_bits);
                if (reader->overrun) return FALSE;
                section_length += increment;
                if (band + section_length > info->max_sfb) return FALSE;
            } while (increment == escape_value);
            for (int k = 0; k < section_length; k++)
                channel->band_type[group][band + k] = (guint8)codebook;
            band += section_length;
        }
    }
    return !reader->overrun;
}

/* Decodes differential scalefactors, intensity positions and noise energies per band. */
static gboolean
read_scalefactor_data(aac_bit_reader *reader, aac_channel *channel,
                      int global_gain, gboolean allow_intensity)
{
    const aac_ics_info *info = &channel->info;
    int scalefactor = global_gain;
    int intensity_position = 0;
    int noise_energy = global_gain - AAC_NOISE_ENERGY_OFFSET;
    gboolean noise_first = TRUE;
    memset(channel->band_value, 0, sizeof(channel->band_value));
    for (int group = 0; group < info->num_window_groups; group++) {
        for (int band = 0; band < info->max_sfb; band++) {
            int codebook = channel->band_type[group][band];
            if (codebook == AAC_ZERO_CODEBOOK) continue;
            if (codebook == AAC_INTENSITY_IN_PHASE_CODEBOOK ||
                codebook == AAC_INTENSITY_OUT_OF_PHASE_CODEBOOK) {
                if (!allow_intensity) return FALSE;
                int delta = read_huffman_symbol(reader, &scalefactor_codebook);
                if (delta < 0) return FALSE;
                intensity_position += delta - AAC_SCALEFACTOR_DELTA_OFFSET;
                if (intensity_position < -255 || intensity_position > 255)
                    return FALSE;
                channel->band_value[group][band] = intensity_position;
            } else if (codebook == AAC_NOISE_CODEBOOK) {
                if (noise_first) {
                    noise_first = FALSE;
                    noise_energy += (int)read_bits(reader, 9) -
                                    AAC_NOISE_PCM_OFFSET;
                } else {
                    int delta =
                        read_huffman_symbol(reader, &scalefactor_codebook);
                    if (delta < 0) return FALSE;
                    noise_energy += delta - AAC_SCALEFACTOR_DELTA_OFFSET;
                }
                if (noise_energy < -256 || noise_energy > 255) return FALSE;
                channel->band_value[group][band] = noise_energy;
            } else {
                int delta = read_huffman_symbol(reader, &scalefactor_codebook);
                if (delta < 0) return FALSE;
                scalefactor += delta - AAC_SCALEFACTOR_DELTA_OFFSET;
                if (scalefactor < 0 || scalefactor > 255) return FALSE;
                channel->band_value[group][band] = scalefactor;
            }
        }
    }
    return !reader->overrun;
}

/* Parses pulse_data positions and amplitudes for long windows. */
static gboolean
read_pulse_data(aac_bit_reader *reader, aac_channel *channel)
{
    const aac_ics_info *info = &channel->info;
    channel->pulse_count = (int)read_bits(reader, 2) + 1;
    int start_band = (int)read_bits(reader, 6);
    if (start_band >= info->num_swb) return FALSE;
    int position = info->swb_offset[start_band];
    for (int i = 0; i < channel->pulse_count; i++) {
        position += (int)read_bits(reader, 5);
        if (position >= AAC_FRAME_LENGTH) return FALSE;
        channel->pulse_position[i] = position;
        channel->pulse_amplitude[i] = (int)read_bits(reader, 4);
    }
    return !reader->overrun;
}

/* Converts TNS reflection coefficients to direct-form LPC by Levinson recursion. */
static void
reflection_to_lpc(const float *reflection, int order, float *lpc)
{
    float previous[AAC_MAX_TNS_ORDER_LONG + 1];
    lpc[0] = 1.0f;
    for (int m = 1; m <= order; m++) {
        for (int i = 0; i < m; i++) previous[i] = lpc[i];
        for (int i = 1; i < m; i++)
            lpc[i] = previous[i] + reflection[m - 1] * previous[m - i];
        lpc[m] = reflection[m - 1];
    }
}

/* Parses tns_data per window and converts quantized coefficients to LPC filters. */
static gboolean
read_tns_data(aac_bit_reader *reader, aac_channel *channel)
{
    const aac_ics_info *info = &channel->info;
    gboolean is_short = info->window_sequence == AAC_EIGHT_SHORT_SEQUENCE;
    int count_bits = is_short ? 1 : 2;
    int length_bits = is_short ? 4 : 6;
    int order_bits = is_short ? 3 : 5;
    for (int window = 0; window < info->num_windows; window++) {
        int filter_count = (int)read_bits(reader, count_bits);
        channel->tns_filter_count[window] = filter_count;
        if (filter_count == 0) continue;
        int resolution_bits = 3 + read_bit(reader);
        for (int filter = 0; filter < filter_count; filter++) {
            channel->tns_length[window][filter] =
                (int)read_bits(reader, length_bits);
            int order = (int)read_bits(reader, order_bits);
            if (order > info->tns_max_order) return FALSE;
            channel->tns_order[window][filter] = order;
            if (order == 0) continue;
            channel->tns_downward[window][filter] = read_bit(reader);
            int compress = read_bit(reader);
            int coefficient_bits = resolution_bits - compress;
            double positive_scale =
                ((1 << (resolution_bits - 1)) - 0.5) / (G_PI / 2.0);
            double negative_scale =
                ((1 << (resolution_bits - 1)) + 0.5) / (G_PI / 2.0);
            float reflection[AAC_MAX_TNS_ORDER_LONG];
            for (int i = 0; i < order; i++) {
                int raw = (int)read_bits(reader, coefficient_bits);
                if (raw & (1 << (coefficient_bits - 1)))
                    raw -= 1 << coefficient_bits;
                reflection[i] = (float)sin(
                    raw / (raw >= 0 ? positive_scale : negative_scale));
            }
            reflection_to_lpc(reflection, order,
                              channel->tns_lpc[window][filter]);
        }
    }
    return !reader->overrun;
}

/* Decodes an escape-codebook magnitude (prefix length, then N+4 bits); -1 on error. */
static int
read_escape_magnitude(aac_bit_reader *reader)
{
    int prefix = 0;
    while (read_bit(reader)) {
        if (++prefix > 8) return -1;
    }
    if (reader->overrun) return -1;
    return (1 << (prefix + 4)) + (int)read_bits(reader, prefix + 4);
}

/* Decodes one 2- or 4-value spectral tuple, applying signs and escape magnitudes. */
static gboolean
read_spectral_tuple(aac_bit_reader *reader, int codebook_index, int *values)
{
    const aac_codebook *book = &spectral_codebooks[codebook_index];
    int symbol = read_huffman_symbol(reader, book);
    if (symbol < 0) return FALSE;
    int modulus = book->modulus;
    if (book->dimension == 4) {
        values[0] = symbol / (modulus * modulus * modulus);
        values[1] = (symbol / (modulus * modulus)) % modulus;
        values[2] = (symbol / modulus) % modulus;
        values[3] = symbol % modulus;
    } else {
        values[0] = symbol / modulus;
        values[1] = symbol % modulus;
    }
    if (book->is_signed) {
        int offset = modulus / 2;
        for (int i = 0; i < book->dimension; i++) values[i] -= offset;
        return TRUE;
    }
    for (int i = 0; i < book->dimension; i++)
        if (values[i] != 0 && read_bit(reader)) values[i] = -values[i];
    if (codebook_index == AAC_ESCAPE_CODEBOOK) {
        for (int i = 0; i < book->dimension; i++) {
            if (values[i] != 16 && values[i] != -16) continue;
            int magnitude = read_escape_magnitude(reader);
            if (magnitude < 0) return FALSE;
            values[i] = values[i] < 0 ? -magnitude : magnitude;
        }
    }
    return !reader->overrun;
}

/* Decodes quantized spectral coefficients for every coded band and window. */
static gboolean
read_spectral_data(aac_bit_reader *reader, aac_channel *channel)
{
    const aac_ics_info *info = &channel->info;
    memset(channel->quantized, 0, sizeof(channel->quantized));
    int window_start = 0;
    for (int group = 0; group < info->num_window_groups; group++) {
        int group_length = info->window_group_length[group];
        for (int band = 0; band < info->max_sfb; band++) {
            int codebook = channel->band_type[group][band];
            if (codebook == AAC_ZERO_CODEBOOK ||
                codebook >= AAC_NOISE_CODEBOOK)
                continue;
            int dimension = spectral_codebooks[codebook].dimension;
            int band_start = info->swb_offset[band];
            int band_end = info->swb_offset[band + 1];
            for (int window = 0; window < group_length; window++) {
                int *base = channel->quantized +
                            (window_start + window) * info->window_length;
                for (int k = band_start; k < band_end; k += dimension) {
                    if (!read_spectral_tuple(reader, codebook, base + k))
                        return FALSE;
                }
            }
        }
        window_start += group_length;
    }
    return !reader->overrun;
}

/* Parses one individual_channel_stream: gain, sections, scalefactors, pulse, TNS, spectrum. */
static gboolean
read_individual_channel_stream(ns_aac_decoder *dec, aac_bit_reader *reader,
                               aac_channel *channel, gboolean common_window,
                               gboolean allow_intensity)
{
    int global_gain = (int)read_bits(reader, 8);
    if (!common_window && !read_ics_info(dec, reader, &channel->info))
        return FALSE;
    if (!read_section_data(reader, channel)) return FALSE;
    if (!read_scalefactor_data(reader, channel, global_gain, allow_intensity))
        return FALSE;
    channel->pulse_present = read_bit(reader);
    if (channel->pulse_present) {
        if (channel->info.window_sequence == AAC_EIGHT_SHORT_SEQUENCE)
            return FALSE;
        if (!read_pulse_data(reader, channel)) return FALSE;
    }
    channel->tns_present = read_bit(reader);
    if (channel->tns_present && !read_tns_data(reader, channel)) return FALSE;
    if (read_bit(reader)) return FALSE;
    if (!read_spectral_data(reader, channel)) return FALSE;
    return !reader->overrun;
}

/* Applies pulses, then inverse-quantizes as sign*|q|^(4/3)*2^((sf-100)/4). */
static void
dequantize_channel(aac_channel *channel)
{
    const aac_ics_info *info = &channel->info;
    memset(channel->spectrum, 0, sizeof(channel->spectrum));
    if (channel->pulse_present) {
        for (int i = 0; i < channel->pulse_count; i++) {
            int *value = &channel->quantized[channel->pulse_position[i]];
            if (*value > 0)
                *value += channel->pulse_amplitude[i];
            else
                *value -= channel->pulse_amplitude[i];
        }
    }
    int window_start = 0;
    for (int group = 0; group < info->num_window_groups; group++) {
        int group_length = info->window_group_length[group];
        for (int band = 0; band < info->max_sfb; band++) {
            int codebook = channel->band_type[group][band];
            if (codebook == AAC_ZERO_CODEBOOK ||
                codebook >= AAC_NOISE_CODEBOOK)
                continue;
            float gain = (float)exp2(
                0.25 * (channel->band_value[group][band] -
                        AAC_SCALEFACTOR_OFFSET));
            int band_start = info->swb_offset[band];
            int band_end = info->swb_offset[band + 1];
            for (int window = 0; window < group_length; window++) {
                int offset = (window_start + window) * info->window_length;
                for (int k = band_start; k < band_end; k++) {
                    int value = channel->quantized[offset + k];
                    int magnitude = value < 0 ? -value : value;
                    if (magnitude >= AAC_POWER_TABLE_SIZE)
                        magnitude = AAC_POWER_TABLE_SIZE - 1;
                    float scaled = power_four_thirds[magnitude] * gain;
                    channel->spectrum[offset + k] = value < 0 ? -scaled : scaled;
                }
            }
        }
        window_start += group_length;
    }
}

/* Advances the LCG noise state and returns it as a signed sample. */
static float
next_noise_sample(ns_aac_decoder *dec)
{
    dec->noise_state = dec->noise_state * 1664525u + 1013904223u;
    return (float)(gint32)dec->noise_state;
}

/* Fills a band with random noise scaled to energy 2^(noise_energy/4) (PNS). */
static void
fill_noise_band(ns_aac_decoder *dec, float *coefficients, int width,
                int noise_energy)
{
    double energy = 0.0;
    for (int k = 0; k < width; k++) {
        float sample = next_noise_sample(dec);
        coefficients[k] = sample;
        energy += (double)sample * sample;
    }
    if (energy <= 0.0) {
        memset(coefficients, 0, sizeof(*coefficients) * (gsize)width);
        return;
    }
    float scale = (float)(exp2(0.25 * noise_energy) / sqrt(energy));
    for (int k = 0; k < width; k++) coefficients[k] *= scale;
}

/* Applies perceptual noise substitution, sharing partner noise for correlated M/S bands. */
static void
apply_noise_substitution(ns_aac_decoder *dec, aac_channel *channel,
                         const aac_channel *correlated_partner)
{
    const aac_ics_info *info = &channel->info;
    int window_start = 0;
    for (int group = 0; group < info->num_window_groups; group++) {
        int group_length = info->window_group_length[group];
        for (int band = 0; band < info->max_sfb; band++) {
            if (channel->band_type[group][band] != AAC_NOISE_CODEBOOK)
                continue;
            int band_start = info->swb_offset[band];
            int width = info->swb_offset[band + 1] - band_start;
            gboolean shared_noise =
                correlated_partner && dec->ms_used[group][band] &&
                correlated_partner->band_type[group][band] ==
                    AAC_NOISE_CODEBOOK;
            float ratio = 0.0f;
            if (shared_noise)
                ratio = (float)exp2(
                    0.25 * (channel->band_value[group][band] -
                            correlated_partner->band_value[group][band]));
            for (int window = 0; window < group_length; window++) {
                int offset = (window_start + window) * info->window_length +
                             band_start;
                if (shared_noise) {
                    for (int k = 0; k < width; k++)
                        channel->spectrum[offset + k] =
                            correlated_partner->spectrum[offset + k] * ratio;
                } else {
                    fill_noise_band(dec, channel->spectrum + offset, width,
                                    channel->band_value[group][band]);
                }
            }
        }
        window_start += group_length;
    }
}

static gboolean
band_is_coded_spectrum(int codebook)
{
    return codebook < AAC_NOISE_CODEBOOK;
}

/* Converts M/S-coded bands of a channel pair back to left/right. */
static void
apply_mid_side(ns_aac_decoder *dec, aac_channel *left, aac_channel *right)
{
    const aac_ics_info *info = &left->info;
    int window_start = 0;
    for (int group = 0; group < info->num_window_groups; group++) {
        int group_length = info->window_group_length[group];
        for (int band = 0; band < info->max_sfb; band++) {
            if (!dec->ms_used[group][band] ||
                !band_is_coded_spectrum(left->band_type[group][band]) ||
                !band_is_coded_spectrum(right->band_type[group][band]))
                continue;
            int band_start = info->swb_offset[band];
            int band_end = info->swb_offset[band + 1];
            for (int window = 0; window < group_length; window++) {
                int offset = (window_start + window) * info->window_length;
                for (int k = band_start; k < band_end; k++) {
                    float mid = left->spectrum[offset + k];
                    float side = right->spectrum[offset + k];
                    left->spectrum[offset + k] = mid + side;
                    right->spectrum[offset + k] = mid - side;
                }
            }
        }
        window_start += group_length;
    }
}

/* Reconstructs intensity-coded right-channel bands from the scaled left spectrum. */
static void
apply_intensity_stereo(ns_aac_decoder *dec, const aac_channel *left,
                       aac_channel *right)
{
    const aac_ics_info *info = &right->info;
    int window_start = 0;
    for (int group = 0; group < info->num_window_groups; group++) {
        int group_length = info->window_group_length[group];
        for (int band = 0; band < info->max_sfb; band++) {
            int codebook = right->band_type[group][band];
            if (codebook != AAC_INTENSITY_IN_PHASE_CODEBOOK &&
                codebook != AAC_INTENSITY_OUT_OF_PHASE_CODEBOOK)
                continue;
            double direction =
                codebook == AAC_INTENSITY_IN_PHASE_CODEBOOK ? 1.0 : -1.0;
            if (dec->ms_mask_present == 1 && dec->ms_used[group][band])
                direction = -direction;
            float scale = (float)(direction *
                                  exp2(-0.25 * right->band_value[group][band]));
            int band_start = info->swb_offset[band];
            int band_end = info->swb_offset[band + 1];
            for (int window = 0; window < group_length; window++) {
                int offset = (window_start + window) * info->window_length;
                for (int k = band_start; k < band_end; k++)
                    right->spectrum[offset + k] =
                        left->spectrum[offset + k] * scale;
            }
        }
        window_start += group_length;
    }
}

/* Runs the temporal noise shaping all-pole filters over the spectrum per window. */
static void
apply_tns(aac_channel *channel)
{
    const aac_ics_info *info = &channel->info;
    if (!channel->tns_present) return;
    int band_limit = MIN(info->tns_max_bands, info->max_sfb);
    for (int window = 0; window < info->num_windows; window++) {
        float *spectrum = channel->spectrum + window * info->window_length;
        int bottom = info->num_swb;
        for (int filter = 0; filter < channel->tns_filter_count[window];
             filter++) {
            int top = bottom;
            bottom = MAX(top - channel->tns_length[window][filter], 0);
            int order = channel->tns_order[window][filter];
            if (order == 0) continue;
            int start = info->swb_offset[MIN(bottom, band_limit)];
            int end = info->swb_offset[MIN(top, band_limit)];
            int size = end - start;
            if (size <= 0) continue;
            int step = 1;
            int position = start;
            if (channel->tns_downward[window][filter]) {
                step = -1;
                position = end - 1;
            }
            const float *lpc = channel->tns_lpc[window][filter];
            for (int n = 0; n < size; n++, position += step) {
                float value = spectrum[position];
                int history = MIN(n, order);
                for (int i = 1; i <= history; i++)
                    value -= lpc[i] * spectrum[position - i * step];
                spectrum[position] = value;
            }
        }
    }
}

/* Iterative radix-2 FFT on bit-reversed input using precomputed twiddles. */
static void
fft_in_place(aac_complex *buffer, int size, const aac_complex *twiddle)
{
    for (int span = 2; span <= size; span <<= 1) {
        int half = span / 2;
        int stride = size / span;
        for (int start = 0; start < size; start += span) {
            for (int j = 0; j < half; j++) {
                aac_complex w = twiddle[j * stride];
                aac_complex a = buffer[start + j];
                aac_complex b = buffer[start + j + half];
                aac_complex product = {
                    b.re * w.re - b.im * w.im,
                    b.re * w.im + b.im * w.re,
                };
                buffer[start + j].re = a.re + product.re;
                buffer[start + j].im = a.im + product.im;
                buffer[start + j + half].re = a.re - product.re;
                buffer[start + j + half].im = a.im - product.im;
            }
        }
    }
}

/* IMDCT via pre-twiddle, N/4-point complex FFT and post-twiddle; output unwindowed. */
static void
inverse_mdct(ns_aac_decoder *dec, const aac_imdct_plan *plan,
             const float *coefficients, float *output)
{
    int length = plan->length;
    int half = length / 2;
    int quarter = length / 4;
    int fft_size = half / 2;
    aac_complex *buffer = dec->fft_buffer;
    float *dct = dec->dct_buffer;
    for (int n = 0; n < fft_size; n++) {
        float even = coefficients[2 * n];
        float odd = coefficients[half - 1 - 2 * n];
        aac_complex t = plan->pre_twiddle[n];
        aac_complex *slot = &buffer[plan->bit_reverse[n]];
        slot->re = even * t.re - odd * t.im;
        slot->im = even * t.im + odd * t.re;
    }
    fft_in_place(buffer, fft_size, plan->fft_twiddle);
    for (int k = 0; k < fft_size; k++) {
        aac_complex t = plan->post_twiddle[k];
        float re = buffer[k].re * t.re - buffer[k].im * t.im;
        float im = buffer[k].re * t.im + buffer[k].im * t.re;
        dct[2 * k] = re * plan->scale;
        dct[half - 1 - 2 * k] = -im * plan->scale;
    }
    for (int n = 0; n < quarter; n++) output[n] = dct[n + quarter];
    for (int n = quarter; n < 3 * quarter; n++)
        output[n] = -dct[3 * quarter - 1 - n];
    for (int n = 3 * quarter; n < length; n++)
        output[n] = -dct[n - 3 * quarter];
}

static const float *
long_window_for_shape(int shape)
{
    return shape ? kbd_window_long : sine_window_long;
}

static const float *
short_window_for_shape(int shape)
{
    return shape ? kbd_window_short : sine_window_short;
}

/* IMDCT, windowing for the window sequence and overlap-add producing 1024 PCM samples. */
static void
synthesize_channel(ns_aac_decoder *dec, int channel_index)
{
    aac_channel *channel = &dec->channel[channel_index];
    const aac_ics_info *info = &channel->info;
    float *time = dec->time_buffer;
    float *overlap = dec->overlap[channel_index];
    float *pcm = dec->pcm[channel_index];
    int previous_shape = dec->previous_window_shape[channel_index];
    int current_shape = info->window_shape;
    const float *previous_long = long_window_for_shape(previous_shape);
    const float *current_long = long_window_for_shape(current_shape);
    const float *previous_short = short_window_for_shape(previous_shape);
    const float *current_short = short_window_for_shape(current_shape);
    const int flat_length = (AAC_FRAME_LENGTH - AAC_SHORT_LENGTH) / 2;

    if (info->window_sequence == AAC_EIGHT_SHORT_SEQUENCE) {
        memset(time, 0, sizeof(dec->time_buffer));
        float *block = dec->short_buffer;
        for (int window = 0; window < AAC_MAX_WINDOWS; window++) {
            inverse_mdct(dec, &short_imdct_plan,
                         channel->spectrum + window * AAC_SHORT_LENGTH,
                         block);
            const float *rising = window == 0 ? previous_short : current_short;
            for (int n = 0; n < AAC_SHORT_LENGTH; n++) {
                block[n] *= rising[n];
                block[AAC_SHORT_LENGTH + n] *=
                    current_short[AAC_SHORT_LENGTH - 1 - n];
            }
            float *target = time + flat_length + window * AAC_SHORT_LENGTH;
            for (int n = 0; n < 2 * AAC_SHORT_LENGTH; n++)
                target[n] += block[n];
        }
    } else {
        inverse_mdct(dec, &long_imdct_plan, channel->spectrum, time);
        if (info->window_sequence == AAC_LONG_STOP_SEQUENCE) {
            for (int n = 0; n < flat_length; n++) time[n] = 0.0f;
            for (int n = 0; n < AAC_SHORT_LENGTH; n++)
                time[flat_length + n] *= previous_short[n];
        } else {
            for (int n = 0; n < AAC_FRAME_LENGTH; n++)
                time[n] *= previous_long[n];
        }
        float *right = time + AAC_FRAME_LENGTH;
        if (info->window_sequence == AAC_LONG_START_SEQUENCE) {
            for (int n = 0; n < AAC_SHORT_LENGTH; n++)
                right[flat_length + n] *=
                    current_short[AAC_SHORT_LENGTH - 1 - n];
            for (int n = flat_length + AAC_SHORT_LENGTH; n < AAC_FRAME_LENGTH;
                 n++)
                right[n] = 0.0f;
        } else {
            for (int n = 0; n < AAC_FRAME_LENGTH; n++)
                right[n] *= current_long[AAC_FRAME_LENGTH - 1 - n];
        }
    }
    for (int n = 0; n < AAC_FRAME_LENGTH; n++) {
        pcm[n] = time[n] + overlap[n];
        overlap[n] = time[AAC_FRAME_LENGTH + n];
    }
    dec->previous_window_shape[channel_index] = current_shape;
}

/* Skips a data_stream_element including its optional byte alignment. */
static gboolean
skip_data_stream_element(aac_bit_reader *reader)
{
    read_bits(reader, 4);
    int align = read_bit(reader);
    gsize count = read_bits(reader, 8);
    if (count == 255) count += read_bits(reader, 8);
    if (align) byte_align(reader);
    return skip_bits(reader, count * 8) && !reader->overrun;
}

/* Skips a fill_element using its escaped byte count. */
static gboolean
skip_fill_element(aac_bit_reader *reader)
{
    gsize count = read_bits(reader, 4);
    if (count == 15) count = 14 + (gsize)read_bits(reader, 8);
    return skip_bits(reader, count * 8) && !reader->overrun;
}

/* Skips a program_config_element by computing its element lists and comment size. */
static gboolean
skip_program_config_element(aac_bit_reader *reader)
{
    read_bits(reader, 4 + 2 + 4);
    int front = (int)read_bits(reader, 4);
    int side = (int)read_bits(reader, 4);
    int back = (int)read_bits(reader, 4);
    int low_frequency = (int)read_bits(reader, 2);
    int associated_data = (int)read_bits(reader, 3);
    int coupling = (int)read_bits(reader, 4);
    if (read_bit(reader)) read_bits(reader, 4);
    if (read_bit(reader)) read_bits(reader, 4);
    if (read_bit(reader)) read_bits(reader, 3);
    gsize element_bits = (gsize)(front + side + back) * 5 +
                         (gsize)(low_frequency + associated_data) * 4 +
                         (gsize)coupling * 5;
    if (!skip_bits(reader, element_bits)) return FALSE;
    byte_align(reader);
    gsize comment_bytes = read_bits(reader, 8);
    return skip_bits(reader, comment_bytes * 8) && !reader->overrun;
}

/* Decodes an SCE: parse, dequantize, noise substitution and TNS. */
static gboolean
decode_single_channel_element(ns_aac_decoder *dec, aac_bit_reader *reader)
{
    read_bits(reader, 4);
    aac_channel *channel = &dec->channel[0];
    dec->ms_mask_present = 0;
    memset(dec->ms_used, 0, sizeof(dec->ms_used));
    if (!read_individual_channel_stream(dec, reader, channel, FALSE, FALSE))
        return FALSE;
    dequantize_channel(channel);
    apply_noise_substitution(dec, channel, NULL);
    apply_tns(channel);
    return TRUE;
}

/* Decodes a CPE with optional common window, M/S, intensity stereo and TNS. */
static gboolean
decode_channel_pair_element(ns_aac_decoder *dec, aac_bit_reader *reader)
{
    read_bits(reader, 4);
    aac_channel *left = &dec->channel[0];
    aac_channel *right = &dec->channel[1];
    gboolean common_window = read_bit(reader);
    dec->ms_mask_present = 0;
    memset(dec->ms_used, 0, sizeof(dec->ms_used));
    if (common_window) {
        if (!read_ics_info(dec, reader, &left->info)) return FALSE;
        right->info = left->info;
        dec->ms_mask_present = (int)read_bits(reader, 2);
        if (dec->ms_mask_present == 3) return FALSE;
        if (dec->ms_mask_present == 1) {
            for (int group = 0; group < left->info.num_window_groups; group++)
                for (int band = 0; band < left->info.max_sfb; band++)
                    dec->ms_used[group][band] = (guint8)read_bit(reader);
        } else if (dec->ms_mask_present == 2) {
            memset(dec->ms_used, 1, sizeof(dec->ms_used));
        }
    }
    if (!read_individual_channel_stream(dec, reader, left, common_window,
                                        FALSE))
        return FALSE;
    if (!read_individual_channel_stream(dec, reader, right, common_window,
                                        TRUE))
        return FALSE;
    dequantize_channel(left);
    dequantize_channel(right);
    apply_noise_substitution(dec, left, NULL);
    apply_noise_substitution(dec, right, left);
    apply_mid_side(dec, left, right);
    apply_intensity_stereo(dec, left, right);
    apply_tns(left);
    apply_tns(right);
    return TRUE;
}

/* Clamps a sample to [-1, 1], mapping NaN to 0. */
static float
clamp_sample(float value)
{
    if (value > 1.0f) return 1.0f;
    if (value < -1.0f) return -1.0f;
    if (value != value) return 0.0f;
    return value;
}

/* Decodes one raw AAC access unit into 1024 interleaved float frames; -1 on error. */
int
ns_aac_decode_frame(ns_aac_decoder *dec, const guint8 *data, gsize len,
                    float *out, int out_frames)
{
    if (!dec || !data || !out || len == 0 || len > G_MAXSIZE / 16 ||
        out_frames < AAC_FRAME_LENGTH)
        return -1;
    aac_bit_reader reader;
    bit_reader_init(&reader, data, len);
    gboolean decoded_audio = FALSE;
    gboolean finished = FALSE;
    while (!finished) {
        int element = (int)read_bits(&reader, 3);
        if (reader.overrun) return -1;
        switch (element) {
        case AAC_ELEMENT_SINGLE_CHANNEL:
            if (dec->channels != 1 || decoded_audio) return -1;
            if (!decode_single_channel_element(dec, &reader)) return -1;
            decoded_audio = TRUE;
            break;
        case AAC_ELEMENT_CHANNEL_PAIR:
            if (dec->channels != 2 || decoded_audio) return -1;
            if (!decode_channel_pair_element(dec, &reader)) return -1;
            decoded_audio = TRUE;
            break;
        case AAC_ELEMENT_DATA_STREAM:
            if (!skip_data_stream_element(&reader)) return -1;
            break;
        case AAC_ELEMENT_PROGRAM_CONFIG:
            if (!skip_program_config_element(&reader)) return -1;
            break;
        case AAC_ELEMENT_FILL:
            if (!skip_fill_element(&reader)) return -1;
            break;
        case AAC_ELEMENT_END:
            finished = TRUE;
            break;
        default:
            return -1;
        }
    }
    if (!decoded_audio) return -1;
    for (int ch = 0; ch < dec->channels; ch++) synthesize_channel(dec, ch);
    for (int n = 0; n < AAC_FRAME_LENGTH; n++)
        for (int ch = 0; ch < dec->channels; ch++)
            out[n * dec->channels + ch] = clamp_sample(dec->pcm[ch][n]);
    return AAC_FRAME_LENGTH;
}
