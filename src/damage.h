/* Northstar Web Browser — damage rectangles and paint-hazard scanning for partial repaint.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_DAMAGE_H
#define NS_DAMAGE_H

#include <glib.h>

#include "anim.h"
#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

typedef struct ns_damage_rect {
    double x, y, w, h;
    gboolean fixed;
} ns_damage_rect;

typedef struct ns_paint_hazards {
    gboolean three_d;
    gboolean sticky;
    gboolean fixed_background;
    GArray  *fixed_bands;
} ns_paint_hazards;

gboolean ns_damage_resolve(const ns_box *root, ns_js *js, ns_anim *anim,
                           GHashTable *nodes, GHashTable *images,
                           GArray *out_rects);

void ns_paint_hazards_scan(const ns_box *root, ns_anim *anim,
                           ns_paint_hazards *out);

gboolean ns_damage_box_visual_rect(const ns_box *b, ns_anim *anim,
                                   ns_damage_rect *out);
gboolean ns_damage_nodes_visual_rects(const ns_box *root, ns_anim *anim,
                                      GHashTable *nodes, GHashTable *found,
                                      GArray *out);
void ns_paint_hazards_clear(ns_paint_hazards *hazards);

G_END_DECLS

#endif
