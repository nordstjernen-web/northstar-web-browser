/* Northstar Web Browser — incremental WebM demuxer API for Media Source Extensions.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_WEBM_H
#define NS_WEBM_H

#include <glib.h>

#include "mp4.h"

G_BEGIN_DECLS

typedef struct ns_webm_demuxer ns_webm_demuxer;

ns_webm_demuxer *ns_webm_demuxer_new(void);
void             ns_webm_demuxer_free(ns_webm_demuxer *d);
gboolean         ns_webm_demuxer_append(ns_webm_demuxer *d,
                                        const guint8 *data, gsize len);
gboolean         ns_webm_demuxer_has_init(const ns_webm_demuxer *d);
guint            ns_webm_demuxer_n_tracks(const ns_webm_demuxer *d);
const ns_mp4_track *ns_webm_demuxer_track(const ns_webm_demuxer *d,
                                          guint index);
gboolean         ns_webm_demuxer_pop_sample(ns_webm_demuxer *d,
                                            ns_mp4_sample *out);
void             ns_webm_demuxer_flush(ns_webm_demuxer *d);
void             ns_webm_demuxer_reset_parser(ns_webm_demuxer *d);
double           ns_webm_demuxer_duration_seconds(const ns_webm_demuxer *d);

G_END_DECLS

#endif
