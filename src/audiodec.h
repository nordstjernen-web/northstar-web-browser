/* Northstar Web Browser — codec-agnostic audio decoder interface for Media Source Extensions.
 * Create from an RFC 6381 codec string plus init config, then decode samples to float PCM.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_AUDIODEC_H
#define NS_AUDIODEC_H

#include <glib.h>

G_BEGIN_DECLS

enum {
    NS_AUDIO_DECODER_MAX_FRAMES = 5760,
};

typedef struct ns_audio_decoder ns_audio_decoder;

gboolean          ns_audio_decoder_supports(const char *codec);
ns_audio_decoder *ns_audio_decoder_new(const char *codec, GBytes *config,
                                       int channels, int sample_rate);
void              ns_audio_decoder_free(ns_audio_decoder *decoder);
int               ns_audio_decoder_sample_rate(const ns_audio_decoder *decoder);
int               ns_audio_decoder_channels(const ns_audio_decoder *decoder);
void              ns_audio_decoder_reset(ns_audio_decoder *decoder,
                                         gboolean from_start);
int               ns_audio_decoder_decode(ns_audio_decoder *decoder,
                                          const guint8 *data, gsize len,
                                          float *out, int out_frames);

G_END_DECLS

#endif
