/* Northstar Web Browser — video decoding through Windows Media Foundation transforms (the Windows default).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_MFVIDEO_H
#define NS_MFVIDEO_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_mf_video_decoder ns_mf_video_decoder;

/* Starts Media Foundation and loads its video decoders. Must run before the
 * process mitigation policies, which narrow what may be loaded later. */
void                 ns_mf_video_preinit(void);
gboolean             ns_mf_video_available(const char *codec);
ns_mf_video_decoder *ns_mf_video_decoder_new(const char *codec);
void                 ns_mf_video_decoder_free(ns_mf_video_decoder *decoder);
void                 ns_mf_video_decoder_flush(ns_mf_video_decoder *decoder);
ns_texture          *ns_mf_video_decoder_decode(ns_mf_video_decoder *decoder,
                                                GBytes *sample, GBytes *config,
                                                gint64 timestamp,
                                                gboolean want_texture);
ns_texture          *ns_mf_video_decoder_take_late(ns_mf_video_decoder *decoder,
                                                   gint64 *timestamp);

G_END_DECLS

#endif
