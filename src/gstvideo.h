/* Northstar Web Browser — video decoding through GStreamer decoder plugins (the Linux default).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_GSTVIDEO_H
#define NS_GSTVIDEO_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_gst_video_decoder ns_gst_video_decoder;

/* Loads GStreamer and its video decoder plugins. Must run before the
 * sandbox, which refuses the executable mappings dlopen needs. */
void                  ns_gst_video_preinit(void);
gboolean              ns_gst_video_available(const char *codec);
ns_gst_video_decoder *ns_gst_video_decoder_new(const char *codec);
void                  ns_gst_video_decoder_free(ns_gst_video_decoder *decoder);
void                  ns_gst_video_decoder_flush(ns_gst_video_decoder *decoder);
ns_texture           *ns_gst_video_decoder_decode(ns_gst_video_decoder *decoder,
                                                  GBytes *sample, GBytes *config,
                                                  gint64 timestamp,
                                                  gboolean want_texture);
ns_texture           *ns_gst_video_decoder_take_late(ns_gst_video_decoder *decoder,
                                                     gint64 *timestamp);

G_END_DECLS

#endif
