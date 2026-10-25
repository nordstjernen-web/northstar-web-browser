/* Northstar Web Browser — a playing video's picture, presented apart from page painting.
 * Picks the decoded frame for the media clock so the shell can composite it without repaint.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "videolayer.h"
#include "trace.h"

static const gint64 NS_VIDEO_LAYER_STALE_US = 250 * 1000;

struct ns_video_layer {
    gint             ref_count;
    GMutex           lock;
    ns_video_worker *worker;
    gboolean         detached;
    ns_video_clock   clock;
    ns_texture      *texture;
    double           shown_pts;
    guint            shown_seek_gen;
    gboolean         composited;
    gint64           presented_us;
};

/* GArray clear func that drops the rect's layer reference. */
void
ns_video_layer_rect_clear(gpointer data)
{
    ns_video_layer_rect *rect = data;
    ns_video_layer_unref(rect->layer);
    rect->layer = NULL;
}

/* Creates a paused video layer with refcount 1 and no frame shown. */
ns_video_layer *
ns_video_layer_new(void)
{
    ns_video_layer *layer = g_new0(ns_video_layer, 1);
    layer->ref_count = 1;
    layer->shown_pts = -1.0;
    layer->clock.paused = TRUE;
    g_mutex_init(&layer->lock);
    return layer;
}

ns_video_layer *
ns_video_layer_ref(ns_video_layer *layer)
{
    if (layer) g_atomic_int_inc(&layer->ref_count);
    return layer;
}

/* Drops a reference; frees the layer and its current texture at zero. */
void
ns_video_layer_unref(ns_video_layer *layer)
{
    if (!layer || !g_atomic_int_dec_and_test(&layer->ref_count)) return;
    ns_texture_unref(layer->texture);
    g_mutex_clear(&layer->lock);
    g_free(layer);
}

void
ns_video_layer_attach(ns_video_layer *layer, ns_video_worker *worker)
{
    if (!layer) return;
    g_mutex_lock(&layer->lock);
    layer->worker = worker;
    g_mutex_unlock(&layer->lock);
}

/* Disconnects the decode worker and marks the layer detached and not composited. */
void
ns_video_layer_detach(ns_video_layer *layer)
{
    if (!layer) return;
    g_mutex_lock(&layer->lock);
    layer->worker = NULL;
    layer->detached = TRUE;
    layer->composited = FALSE;
    g_mutex_unlock(&layer->lock);
}

gboolean
ns_video_layer_detached(ns_video_layer *layer)
{
    if (!layer) return TRUE;
    g_mutex_lock(&layer->lock);
    gboolean detached = layer->detached;
    g_mutex_unlock(&layer->lock);
    return detached;
}

void
ns_video_layer_set_clock(ns_video_layer *layer, const ns_video_clock *clock)
{
    if (!layer || !clock) return;
    g_mutex_lock(&layer->lock);
    layer->clock = *clock;
    g_mutex_unlock(&layer->lock);
}

/* Playback position in seconds from the clock: paused, clamped or looped. */
static double
clock_position(const ns_video_clock *clock, gint64 now_us)
{
    if (clock->paused) return clock->paused_ms / 1000.0;
    gint64 elapsed_ms = (now_us - clock->start_us) / 1000;
    if (elapsed_ms < 0) elapsed_ms = 0;
    if (clock->total_ms <= 0) return elapsed_ms / 1000.0;
    if (!clock->loop) return MIN(elapsed_ms, (gint64)clock->total_ms) / 1000.0;
    return (elapsed_ms % clock->total_ms) / 1000.0;
}

/* Takes the worker's frame for time t, ignoring older frames unless a seek happened. */
static gboolean
layer_present_locked(ns_video_layer *layer, double t)
{
    if (!layer->worker) return FALSE;
    double pts = 0.0;
    ns_texture *texture = ns_video_worker_take(layer->worker, t, &pts);
    if (!texture) return FALSE;
    if (layer->texture && pts < layer->shown_pts &&
        layer->clock.seek_gen == layer->shown_seek_gen) {
        ns_texture_unref(texture);
        return FALSE;
    }
    layer->shown_seek_gen = layer->clock.seek_gen;
    ns_texture_unref(layer->texture);
    layer->texture = texture;
    layer->shown_pts = pts;
    return TRUE;
}

/* Locks and presents the decoded frame for time t; TRUE if the picture changed. */
gboolean
ns_video_layer_present(ns_video_layer *layer, double t)
{
    if (!layer) return FALSE;
    g_mutex_lock(&layer->lock);
    gboolean changed = layer_present_locked(layer, t);
    g_mutex_unlock(&layer->lock);
    return changed;
}

/* Presents the frame for the clock position at now_us when composited; traces the pick. */
gboolean
ns_video_layer_present_now(ns_video_layer *layer, gint64 now_us)
{
    if (!layer) return FALSE;
    gint64 trace_start = ns_trace_now();
    g_mutex_lock(&layer->lock);
    layer->presented_us = now_us;
    double t = clock_position(&layer->clock, now_us);
    gboolean changed = layer->composited && layer_present_locked(layer, t);
    if (trace_start && layer->composited && !layer->clock.paused) {
        double newest = -1.0;
        guint ready = ns_video_worker_ready(layer->worker, &newest);
        ns_trace_completef("media", "video pick", trace_start,
                           "clock %.3f shown %.3f %s, %u decoded up to %.3f",
                           t, layer->shown_pts, changed ? "new" : "kept",
                           ready, newest);
    }
    g_mutex_unlock(&layer->lock);
    return changed;
}

/* PTS of the shown frame for the current seek generation, or -1. */
double
ns_video_layer_shown_pts(ns_video_layer *layer)
{
    if (!layer) return -1.0;
    g_mutex_lock(&layer->lock);
    double pts = layer->texture &&
                 layer->shown_seek_gen == layer->clock.seek_gen
        ? layer->shown_pts : -1.0;
    g_mutex_unlock(&layer->lock);
    return pts;
}

/* Returns a new reference to the current frame texture and its PTS. */
ns_texture *
ns_video_layer_texture(ns_video_layer *layer, double *shown_pts)
{
    if (!layer) return NULL;
    g_mutex_lock(&layer->lock);
    ns_texture *texture = ns_texture_ref(layer->texture);
    if (shown_pts) *shown_pts = layer->shown_pts;
    g_mutex_unlock(&layer->lock);
    return texture;
}

/* Marks whether the compositor presents this layer (never when detached). */
void
ns_video_layer_set_composited(ns_video_layer *layer, gboolean composited)
{
    if (!layer) return;
    g_mutex_lock(&layer->lock);
    if (composited && !layer->composited)
        layer->presented_us = g_get_monotonic_time();
    layer->composited = composited && !layer->detached;
    g_mutex_unlock(&layer->lock);
}

/* True if composited and presented within the last 250 ms. */
gboolean
ns_video_layer_composited(ns_video_layer *layer, gint64 now_us)
{
    if (!layer) return FALSE;
    g_mutex_lock(&layer->lock);
    gboolean composited = layer->composited &&
                          now_us - layer->presented_us < NS_VIDEO_LAYER_STALE_US;
    g_mutex_unlock(&layer->lock);
    return composited;
}
