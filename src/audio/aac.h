/* Northstar Web Browser — AAC-LC decoder API: AudioSpecificConfig in, float PCM frames out.
 * Used by audiodec.c for "mp4a" Media Source Extensions audio tracks.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NS_AAC_H
#define NS_AAC_H

#include <glib.h>

typedef struct ns_aac_decoder ns_aac_decoder;

ns_aac_decoder *ns_aac_decoder_new(const guint8 *asc, gsize asc_len);
void            ns_aac_decoder_free(ns_aac_decoder *dec);
int             ns_aac_decoder_sample_rate(const ns_aac_decoder *dec);
int             ns_aac_decoder_channels(const ns_aac_decoder *dec);
int             ns_aac_decode_frame(ns_aac_decoder *dec, const guint8 *data,
                                    gsize len, float *out, int out_frames);
void            ns_aac_decoder_reset(ns_aac_decoder *dec);

#endif
