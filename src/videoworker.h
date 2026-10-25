/* Northstar Web Browser — threaded video decode worker API.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_VIDEOWORKER_H
#define NS_VIDEOWORKER_H

#include <glib.h>

#include "texture.h"
#include "videodec.h"

G_BEGIN_DECLS

typedef struct ns_video_worker ns_video_worker;

ns_video_worker *ns_video_worker_new(ns_video_decoder *decoder);
void             ns_video_worker_free(ns_video_worker *worker);
void             ns_video_worker_restart(ns_video_worker *worker);
void             ns_video_worker_push(ns_video_worker *worker, GBytes *sample,
                                      GBytes *config, double pts,
                                      gboolean want_texture);
void             ns_video_worker_set_max_pictures(ns_video_worker *worker,
                                                  guint max_pictures);
gboolean         ns_video_worker_expects(ns_video_worker *worker, double pts);
void             ns_video_worker_counts(ns_video_worker *worker,
                                        guint *presented, guint *dropped);
double           ns_video_decode_pixel_rate(void);
guint            ns_video_worker_ready(ns_video_worker *worker,
                                       double *newest_pts);
ns_texture      *ns_video_worker_take(ns_video_worker *worker, double pts,
                                      double *taken_pts);

G_END_DECLS

#endif
