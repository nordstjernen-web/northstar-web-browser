/* Northstar Web Browser — Cairo paint API plus shared inline text and font helpers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_PAINT_H
#define NS_PAINT_H

#include <cairo.h>
#include <glib.h>
#include "ns_pango.h"

#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct ns_selection;
struct ns_anim;
void ns_paint(cairo_t *cr, const ns_box *root, const char *highlight_query);
void ns_paint_with_selection(cairo_t *cr, const ns_box *root,
                             const char *highlight_query,
                             const struct ns_selection *sel);
void ns_paint_set_js(ns_js *js);
void ns_paint_set_anim(struct ns_anim *anim);
void ns_paint_set_video_layers(GArray *out);
void ns_paint_set_caret_visible(gboolean visible);
void ns_paint_set_device_viewport(double x0, double y0, double x1, double y1);
void ns_paint_clear_device_viewport(void);

void ns_paint_3d_invalidate(void);
gboolean ns_paint_3d_registered(const ns_box *b);
const ns_box *ns_paint_3d_pick(const ns_box *root3d, double x, double y);

void ns_paint_set_search(gboolean case_sensitive, const ns_box *active);

gboolean ns_paint_inline_range_extents(const ns_box *b, gsize start, gsize len,
                                       double *out_x, double *out_y,
                                       double *out_w, double *out_h);
gboolean ns_paint_inline_xy_to_byte(const ns_box *b,
                                    double rel_x, double rel_y,
                                    gsize *out_byte);
gboolean ns_paint_inline_word_range(const ns_box *b, gsize byte,
                                    gsize *out_start, gsize *out_end);
gboolean ns_paint_inline_strut_line(const ns_box *b, const ns_style *s,
                                    NsPangoLayout *layout, double *out_offset,
                                    double *out_height, double *out_baseline);

void ns_paint_sync_inline_atomic_offsets(ns_box *root);

void ns_paint_register_font_oracle(void);
guint64 ns_paint_font_generation(void);

void ns_paint_apply_inline_font(NsPangoLayout *layout, const ns_style *style);
int ns_paint_pango_font_size(double size_px);
NsPangoWeight ns_paint_pango_weight(int weight);
NsPangoStretch ns_paint_pango_stretch(int rank);
void ns_paint_apply_text_align(NsPangoLayout *layout, const ns_style *style);
void ns_paint_start_align_overflowing_lines(NsPangoLayout *layout);

void ns_paint_apply_i18n(NsPangoLayout *layout, NsPangoAttrList *attrs,
                         const ns_box *box);
void ns_paint_apply_font_features(NsPangoAttrList *attrs, const ns_style *style,
                                  guint start, guint end);
NsPangoAttribute *ns_paint_font_features_attr_from_values(int kerning,
                                                        const char *ligatures,
                                                        const char *settings);
NsPangoAttribute *ns_paint_font_variations_attr_from_values(const char *settings);
NsPangoAttribute *ns_paint_inline_font_attr(const ns_inline_attr *r);

NsPangoWrapMode ns_paint_wrap_mode_for(const ns_style *style);

double ns_paint_css_line_height_px(const ns_style *style);
double ns_paint_normal_line_height_px(const ns_style *style);
void ns_paint_apply_css_line_spacing(NsPangoLayout *layout,
                                     const ns_style *style);

gboolean ns_paint_li_is_inside(const ns_style *li_style);
void     ns_paint_list_ordinals_begin(void);
void     ns_paint_list_ordinals_end(void);
gboolean ns_paint_li_marker_text(const ns_node *li, const ns_style *li_style,
                                 char *out, gsize out_sz);

G_END_DECLS

#endif
