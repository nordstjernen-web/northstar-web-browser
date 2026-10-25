/* Northstar Web Browser — H.264 video decoder API over OpenH264.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_H264_H
#define NS_H264_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_h264_decoder ns_h264_decoder;

gboolean         ns_h264_available(void);
ns_h264_decoder *ns_h264_decoder_new(void);
void             ns_h264_decoder_free(ns_h264_decoder *decoder);
void             ns_h264_decoder_flush(ns_h264_decoder *decoder);
ns_texture      *ns_h264_decoder_decode(ns_h264_decoder *decoder, GBytes *sample,
                                        GBytes *config, gint64 timestamp,
                                        gboolean want_texture);

G_END_DECLS

#endif
