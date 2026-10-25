/* Northstar Web Browser — print pagination API: page setup, page offsets and per-page drawing.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_PRINT_H
#define NS_PRINT_H

#include <cairo.h>
#include <glib.h>

#include "css.h"
#include "layout.h"

G_BEGIN_DECLS

typedef struct ns_print_setup {
    double width;
    double height;
    double margin_top;
    double margin_right;
    double margin_bottom;
    double margin_left;
} ns_print_setup;

void ns_print_setup_default(ns_print_setup *setup);
void ns_print_setup_apply_page_rule(ns_print_setup *setup,
                                    const ns_css_page_rule *rule);

GArray *ns_print_page_offsets(const ns_box *root, double page_content_height);

double ns_print_page_bottom(const GArray *offsets, guint i,
                            double page_content_height);

void ns_print_draw_page(cairo_t *cr, const ns_box *root,
                        const ns_print_setup *setup, double scale,
                        double page_top, double page_bottom);

G_END_DECLS

#endif
