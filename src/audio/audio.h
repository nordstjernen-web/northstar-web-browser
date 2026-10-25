/* Northstar Web Browser — per-document audio context API for media element playback.
 * Token-addressed players fed by URL, bytes or pushed PCM streams, with play/seek/volume/status.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NS_AUDIO_H
#define NS_AUDIO_H

#include <glib.h>

#include "fetch_policy.h"

typedef struct NsAudioContext NsAudioContext;

typedef enum {
    NS_AUDIO_PLAYER_ABSENT,
    NS_AUDIO_PLAYER_LOADING,
    NS_AUDIO_PLAYER_READY,
    NS_AUDIO_PLAYER_FAILED,
} NsAudioPlayerState;

typedef enum {
    NS_AUDIO_ERROR_NONE,
    NS_AUDIO_ERROR_FETCH,
    NS_AUDIO_ERROR_DECODE,
    NS_AUDIO_ERROR_NO_DEVICE,
    NS_AUDIO_ERROR_TOO_MANY,
} NsAudioError;

typedef struct {
    NsAudioPlayerState state;
    NsAudioError       error;
    double             duration;
    double             position;
    gboolean           playing;
    gboolean           ended;
} NsAudioStatus;

void ns_audio_set_silent(gboolean silent);

/* policy is the document's fetch policy (CSP media-src, mixed content);
 * the context keeps a reference. */
NsAudioContext *ns_audio_context_new(const char *document_url,
                                     ns_fetch_policy *policy);
void ns_audio_context_open(NsAudioContext *context, const char *token,
                           const char *url);
void ns_audio_context_open_bytes(NsAudioContext *context, const char *token,
                                 GBytes *bytes, gboolean reload);
void ns_audio_context_play(NsAudioContext *context, const char *token);
void ns_audio_context_pause(NsAudioContext *context, const char *token);
void ns_audio_context_seek(NsAudioContext *context, const char *token,
                           double seconds);
void ns_audio_context_set_volume(NsAudioContext *context, const char *token,
                                 double volume);
void ns_audio_context_set_loop(NsAudioContext *context, const char *token,
                               gboolean loop);
void ns_audio_context_close(NsAudioContext *context, const char *token);
gboolean ns_audio_context_open_stream(NsAudioContext *context,
                                      const char *token, int rate,
                                      int channels);
gboolean ns_audio_context_stream_push(NsAudioContext *context,
                                      const char *token, const float *samples,
                                      int frames, int channels);
void ns_audio_context_stream_restart(NsAudioContext *context,
                                     const char *token, double seconds);
void ns_audio_context_stream_end(NsAudioContext *context, const char *token,
                                 gboolean ended);
gboolean ns_audio_context_status(NsAudioContext *context, const char *token,
                                 NsAudioStatus *out);
void ns_audio_context_destroy(NsAudioContext *context);
void ns_audio_shutdown(void);

#endif
