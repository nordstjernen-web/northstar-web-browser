/* Northstar Web Browser — video layer API: clock-driven frame presentation for compositing.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_VIDEOLAYER_H
#define NS_VIDEOLAYER_H

#include <glib.h>

#include "texture.h"
#include "videoworker.h"

G_BEGIN_DECLS

typedef struct ns_video_layer ns_video_layer;

typedef struct {
    gboolean paused;
    gboolean loop;
    gint64   start_us;
    int      paused_ms;
    int      total_ms;
    guint    seek_gen;
} ns_video_clock;

typedef struct {
    ns_video_layer *layer;
    double          x, y, w, h;
} ns_video_layer_rect;

void ns_video_layer_rect_clear(gpointer data);

ns_video_layer *ns_video_layer_new(void);
ns_video_layer *ns_video_layer_ref(ns_video_layer *layer);
void            ns_video_layer_unref(ns_video_layer *layer);

void     ns_video_layer_attach(ns_video_layer *layer, ns_video_worker *worker);
void     ns_video_layer_detach(ns_video_layer *layer);
gboolean ns_video_layer_detached(ns_video_layer *layer);

void     ns_video_layer_set_clock(ns_video_layer *layer,
                                  const ns_video_clock *clock);

gboolean     ns_video_layer_present(ns_video_layer *layer, double t);
gboolean     ns_video_layer_present_now(ns_video_layer *layer, gint64 now_us);
ns_texture  *ns_video_layer_texture(ns_video_layer *layer, double *shown_pts);
double       ns_video_layer_shown_pts(ns_video_layer *layer);

void     ns_video_layer_set_composited(ns_video_layer *layer,
                                       gboolean composited);
gboolean ns_video_layer_composited(ns_video_layer *layer, gint64 now_us);

G_END_DECLS

#endif
