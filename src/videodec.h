/* Northstar Web Browser — codec-agnostic video decoder interface for Media Source Extensions.
 * Decodes one sample at a time, optionally producing a texture for the video layer.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_VIDEODEC_H
#define NS_VIDEODEC_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_video_decoder ns_video_decoder;

gboolean          ns_video_decoder_supports(const char *codec);
ns_video_decoder *ns_video_decoder_new(const char *codec);
void              ns_video_decoder_free(ns_video_decoder *decoder);
void              ns_video_decoder_flush(ns_video_decoder *decoder);
ns_texture       *ns_video_decoder_decode(ns_video_decoder *decoder,
                                          GBytes *sample, GBytes *config,
                                          gint64 timestamp,
                                          gboolean want_texture);
ns_texture       *ns_video_decoder_take_late(ns_video_decoder *decoder,
                                             gint64 *timestamp);

/* TRUE when NS_VIDEO_DECODER=native asks to skip the system decoders. */
gboolean          ns_video_decoder_native_forced(void);

/* A queue of pictures a system decoder produced for earlier timestamps
 * (reordering), kept until the worker takes them; holds at most 16. */
void              ns_video_late_push(GQueue *late, gint64 timestamp,
                                     ns_texture *texture);
ns_texture       *ns_video_late_take(GQueue *late, gint64 *timestamp);
void              ns_video_late_clear(GQueue *late);

G_END_DECLS

#endif
