/* Northstar Web Browser — MP4 avcC config and length-prefixed H.264 samples to Annex B.
 * Shared by the OpenH264 and Media Foundation decoders.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "avc.h"

static const guint8 NS_AVC_START_CODE[4] = { 0, 0, 0, 1 };

gboolean
ns_avc_append_parameter_sets(GByteArray *out, GBytes *config, int *length_size)
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
            g_byte_array_append(out, NS_AVC_START_CODE, sizeof NS_AVC_START_CODE);
            g_byte_array_append(out, avcc + pos, (guint)size);
            pos += size;
        }
    }
    return TRUE;
}

gboolean
ns_avc_append_nal_units(GByteArray *out, const guint8 *data, gsize len,
                        int length_size)
{
    gsize pos = 0;
    while (pos < len) {
        if (len - pos < (gsize)length_size) return FALSE;
        gsize size = 0;
        for (int i = 0; i < length_size; i++) size = size << 8 | data[pos++];
        if (size > len - pos) return FALSE;
        if (size == 0) continue;
        g_byte_array_append(out, NS_AVC_START_CODE, sizeof NS_AVC_START_CODE);
        g_byte_array_append(out, data + pos, (guint)size);
        pos += size;
    }
    return TRUE;
}
