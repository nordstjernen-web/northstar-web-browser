/* Northstar Web Browser — maps changed elements and images to the page rectangles they repaint.
 * Also scans for paint hazards (fixed, sticky, 3D) that force full repaint or limit scroll blits.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "damage.h"

#include <math.h>
#include <string.h>

#include "css.h"
#include "mat4.h"
#include "dom.h"
#include "image.h"

#define NS_DAMAGE_INFLATE 2.0
#define NS_DAMAGE_FIXED_INFLATE 16.0
#define NS_DAMAGE_EM_PX 64.0
#define NS_DAMAGE_PAINT_BOUND 1.0e7

typedef struct damage_walk {
    ns_js      *js;
    ns_anim    *anim;
    GHashTable *nodes;
    GHashTable *images;
    GHashTable *images_seen;
    GArray     *rects;
    gboolean    full;
} damage_walk;

static gboolean
style_keyword_is(const ns_style *s, ns_css_prop p, const char *kw)
{
    const char *v = ns_style_keyword(s, p);
    return v && strcmp(v, kw) == 0;
}

/* Returns TRUE if the box has an effective static or animated transform; flags 3D ones. */
static gboolean
box_has_transform(const ns_box *b, ns_anim *anim, gboolean *three_d)
{
    const ns_style *s = b->style;
    const ns_css_transform *anim_tf =
        anim && b->dom ? ns_anim_get_transform(anim, b->dom) : NULL;
    if (!anim_tf &&
        !(s && (s->values[NS_CSS_TRANSFORM] || s->values[NS_CSS_TRANSLATE] ||
                s->values[NS_CSS_ROTATE] || s->values[NS_CSS_SCALE])))
        return FALSE;
    ns_css_transform tf;
    tf.n_ops = 0;
    ns_css_style_effective_transform(s, anim_tf, &tf);
    if (tf.n_ops > 0 && three_d && ns_css_transform_is_3d(&tf))
        *three_d = TRUE;
    return tf.n_ops > 0;
}

static gboolean
box_has_perspective(const ns_box *b)
{
    const ns_css_value *v = b->style ? b->style->values[NS_CSS_PERSPECTIVE]
                                     : NULL;
    return v && v->kind == NS_CSS_V_LENGTH && v->u.length.v > 0;
}

/* Returns TRUE if paint may move the box from its layout position (sticky, 3D, transform). */
static gboolean
box_moves_at_paint(const ns_box *b, ns_anim *anim)
{
    if (b->fragment_context) return TRUE;
    if (!b->style) return FALSE;
    if (style_keyword_is(b->style, NS_CSS_POSITION, "sticky")) return TRUE;
    if (style_keyword_is(b->style, NS_CSS_TRANSFORM_STYLE, "preserve-3d"))
        return TRUE;
    if (box_has_perspective(b)) return TRUE;
    return box_has_transform(b, anim, NULL);
}

/* Returns TRUE if any background layer has background-attachment: fixed. */
static gboolean
box_has_fixed_background(const ns_box *b)
{
    const ns_css_value *v =
        b->style ? b->style->values[NS_CSS_BACKGROUND_ATTACHMENT] : NULL;
    int n = ns_css_value_layer_count(v);
    for (int i = 0; i < n; i++) {
        const ns_css_value *layer = ns_css_value_layer(v, i);
        if (layer && layer->kind == NS_CSS_V_KEYWORD && layer->u.keyword &&
            strcmp(layer->u.keyword, "fixed") == 0)
            return TRUE;
    }
    return FALSE;
}

/* Returns TRUE for the root, html or body box, whose background paints the canvas. */
static gboolean
box_is_canvas_root(const ns_box *b)
{
    return !b->parent || ns_node_is_element_named(b->dom, "html") ||
           ns_node_is_element_named(b->dom, "body");
}

/* Returns TRUE if img is in the changed set, recording that it was seen in layout. */
static gboolean
image_changed(damage_walk *w, gconstpointer img)
{
    if (!img || !g_hash_table_contains(w->images, img)) return FALSE;
    g_hash_table_add(w->images_seen, (gpointer)img);
    return TRUE;
}

/* Returns TRUE if any image, background, marker or video the box paints has changed. */
static gboolean
box_uses_changed_image(damage_walk *w, const ns_box *b)
{
    gboolean used = FALSE;
    const ns_box_media *m = b->media;
    if (m) {
        used |= image_changed(w, m->image);
        used |= image_changed(w, m->bg_image);
        used |= image_changed(w, m->marker_image);
        used |= image_changed(w, m->border_image);
        used |= image_changed(w, m->video);
        if (m->bg_layer_images)
            for (guint i = 0; i < m->bg_layer_images->len; i++)
                used |= image_changed(w,
                                      g_ptr_array_index(m->bg_layer_images, i));
    }
    if (b->kind == NS_BOX_IMAGE && b->dom && w->js)
        used |= image_changed(w, ns_js_image_for_node(w->js, b->dom));
    if (b->attrs)
        for (guint i = 0; i < b->attrs->len; i++)
            used |= image_changed(
                w, g_array_index(b->attrs, ns_inline_attr, i).bg_image);
    return used;
}

/* Computes the box's border-box rectangle in layout coordinates. */
static void
box_border_rect(const ns_box *b, double *x, double *y, double *w, double *h)
{
    *x = b->x + b->margin.left;
    *y = b->y + b->margin.top;
    *w = b->border.left + b->padding.left + b->content_width +
         b->padding.right + b->border.right;
    *h = b->border.top + b->padding.top + b->content_height +
         b->padding.bottom + b->border.bottom;
}

/* Appends the box's inflated, offset border rect; non-finite geometry forces full repaint. */
static void
damage_add_box(damage_walk *w, const ns_box *b, double dx, double dy,
               gboolean fixed)
{
    ns_damage_rect r;
    box_border_rect(b, &r.x, &r.y, &r.w, &r.h);
    if (!isfinite(r.x) || !isfinite(r.y) || !isfinite(r.w) || !isfinite(r.h)) {
        w->full = TRUE;
        return;
    }
    r.x += dx - NS_DAMAGE_INFLATE;
    r.y += dy - NS_DAMAGE_INFLATE;
    r.w += 2 * NS_DAMAGE_INFLATE;
    r.h += 2 * NS_DAMAGE_INFLATE;
    r.fixed = fixed;
    g_array_append_val(w->rects, r);
}

/* Walks boxes collecting damage for changed nodes/images; moving boxes force full repaint. */
static void
damage_walk_box(damage_walk *w, const ns_box *b, double dx, double dy,
                gboolean fixed, gboolean moved)
{
    if (!b || w->full) return;
    gboolean in_fixed = fixed || ns_box_is_fixed(b);
    gboolean in_moved = moved || box_moves_at_paint(b, w->anim);
    gboolean hit = w->nodes && b->dom && g_hash_table_contains(w->nodes, b->dom);
    if (w->images && box_uses_changed_image(w, b)) {
        if (box_is_canvas_root(b)) {
            w->full = TRUE;
            return;
        }
        hit = TRUE;
    }
    if (hit) {
        if (in_moved) {
            w->full = TRUE;
            return;
        }
        damage_add_box(w, b, dx, dy, in_fixed);
    }
    double cdx = dx, cdy = dy;
    if (isfinite(b->scroll_x) && isfinite(b->scroll_y)) {
        cdx -= b->scroll_x;
        cdy -= b->scroll_y;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        damage_walk_box(w, c, cdx, cdy, in_fixed, in_moved);
    if (!b->inline_atomics) return;
    for (guint i = 0; i < b->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(b->inline_atomics, ns_inline_atomic, i);
        if (!a->box) continue;
        double tx = b->x + a->owner_offset_x - a->box->x;
        double ty = b->y + a->owner_offset_y - a->box->y;
        if (!isfinite(tx) || !isfinite(ty)) {
            w->full = TRUE;
            return;
        }
        damage_walk_box(w, a->box, cdx + tx, cdy + ty, in_fixed, in_moved);
    }
}

/* Maps changed nodes and images to repaint rects; FALSE when a full repaint is needed. */
gboolean
ns_damage_resolve(const ns_box *root, ns_js *js, ns_anim *anim,
                  GHashTable *nodes, GHashTable *images, GArray *out_rects)
{
    if (!root) return FALSE;
    gboolean have_nodes = nodes && g_hash_table_size(nodes) > 0;
    gboolean have_images = images && g_hash_table_size(images) > 0;
    if (!have_nodes && !have_images) return TRUE;
    damage_walk w = {
        .js = js,
        .anim = anim,
        .nodes = have_nodes ? nodes : NULL,
        .images = have_images ? images : NULL,
        .images_seen = g_hash_table_new(g_direct_hash, g_direct_equal),
        .rects = out_rects,
    };
    damage_walk_box(&w, root, 0, 0, FALSE, FALSE);
    if (have_images &&
        g_hash_table_size(w.images_seen) < g_hash_table_size(images))
        w.full = TRUE;
    g_hash_table_destroy(w.images_seen);
    return !w.full;
}

typedef struct paint_extent {
    double x0, y0, x1, y1;
    double overflow;
} paint_extent;

/* Returns how far outer box-shadows can paint beyond the border box, padding em units. */
static double
style_shadow_overflow(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_BOX_SHADOW] : NULL;
    if (!v || v->kind != NS_CSS_V_SHADOW) return 0;
    double out = 0;
    const ns_css_shadow_list *sl = &v->u.shadow;
    for (int i = 0; i < sl->n; i++) {
        const ns_css_shadow *sh = &sl->s[i];
        if (sh->inset) continue;
        double rel = 0;
        for (int k = 0; k < 4; k++) rel += fabs(sh->em[k]) + fabs(sh->rem[k]);
        out = MAX(out, MAX(fabs(sh->x), fabs(sh->y)) + fabs(sh->spread) +
                       1.5 * fabs(sh->blur) + NS_DAMAGE_EM_PX * rel);
    }
    return out;
}

/* Accumulates the border-box and paint extent of a subtree, stopping at scrollers. */
static void
subtree_extent(const ns_box *b, double dx, double dy, paint_extent *e)
{
    double x, y, w, h;
    box_border_rect(b, &x, &y, &w, &h);
    if (isfinite(x) && isfinite(y) && isfinite(w) && isfinite(h)) {
        e->x0 = MIN(e->x0, x + dx);
        e->y0 = MIN(e->y0, y + dy);
        e->x1 = MAX(e->x1, x + dx + w);
        e->y1 = MAX(e->y1, y + dy + h);
    }
    if (b->paint_bottom > b->paint_top &&
        fabs(b->paint_top) < NS_DAMAGE_PAINT_BOUND &&
        fabs(b->paint_bottom) < NS_DAMAGE_PAINT_BOUND) {
        e->y0 = MIN(e->y0, b->paint_top + dy);
        e->y1 = MAX(e->y1, b->paint_bottom + dy);
    }
    e->overflow = MAX(e->overflow, style_shadow_overflow(b->style));
    if (b->scrolls) return;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        subtree_extent(c, dx, dy, e);
    if (!b->inline_atomics) return;
    for (guint i = 0; i < b->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(b->inline_atomics, ns_inline_atomic, i);
        if (!a->box) continue;
        double tx = b->x + a->owner_offset_x - a->box->x;
        double ty = b->y + a->owner_offset_y - a->box->y;
        if (isfinite(tx) && isfinite(ty))
            subtree_extent(a->box, dx + tx, dy + ty, e);
    }
}

/* Records sticky, 3D, fixed-background use and the painted band of each fixed subtree. */
static void
hazards_walk(const ns_box *b, ns_anim *anim, ns_paint_hazards *h,
             gboolean in_fixed)
{
    if (!b) return;
    if (b->style) {
        if (style_keyword_is(b->style, NS_CSS_POSITION, "sticky"))
            h->sticky = TRUE;
        if (style_keyword_is(b->style, NS_CSS_TRANSFORM_STYLE, "preserve-3d") ||
            box_has_perspective(b))
            h->three_d = TRUE;
        box_has_transform(b, anim, &h->three_d);
        if (box_has_fixed_background(b)) h->fixed_background = TRUE;
    }
    if (!in_fixed && ns_box_is_fixed(b)) {
        paint_extent e = { G_MAXDOUBLE, G_MAXDOUBLE, -G_MAXDOUBLE,
                           -G_MAXDOUBLE, 0 };
        subtree_extent(b, 0, 0, &e);
        if (e.x1 > e.x0 && e.y1 > e.y0) {
            double pad = NS_DAMAGE_FIXED_INFLATE + e.overflow;
            ns_damage_rect band = {
                .x = e.x0 - pad,
                .y = e.y0 - pad,
                .w = e.x1 - e.x0 + 2 * pad,
                .h = e.y1 - e.y0 + 2 * pad,
                .fixed = TRUE,
            };
            g_array_append_val(h->fixed_bands, band);
        }
        in_fixed = TRUE;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        hazards_walk(c, anim, h, in_fixed);
    if (!b->inline_atomics) return;
    for (guint i = 0; i < b->inline_atomics->len; i++)
        hazards_walk(g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                     anim, h, in_fixed);
}

/* Scans the box tree for paint hazards that limit partial repaint and scroll blits. */
void
ns_paint_hazards_scan(const ns_box *root, ns_anim *anim, ns_paint_hazards *out)
{
    ns_paint_hazards_clear(out);
    out->fixed_bands = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
    hazards_walk(root, anim, out, FALSE);
}

void
ns_paint_hazards_clear(ns_paint_hazards *hazards)
{
    if (!hazards) return;
    if (hazards->fixed_bands) g_array_free(hazards->fixed_bands, TRUE);
    memset(hazards, 0, sizeof *hazards);
}

/* Builds the box's transform matrix about its transform-origin; FALSE if none. */
static gboolean
box_local_transform(const ns_box *b, ns_anim *anim, ns_mat4 *m)
{
    ns_mat4_identity(m);
    const ns_style *s = b->style;
    const ns_css_transform *anim_tf =
        anim && b->dom ? ns_anim_get_transform(anim, b->dom) : NULL;
    if (!s || (!anim_tf &&
               !(s->values[NS_CSS_TRANSFORM] || s->values[NS_CSS_TRANSLATE] ||
                 s->values[NS_CSS_ROTATE] || s->values[NS_CSS_SCALE])))
        return FALSE;
    ns_css_transform eff;
    eff.n_ops = 0;
    ns_css_style_effective_transform(s, anim_tf, &eff);
    if (eff.n_ops == 0) return FALSE;
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    double ox = bx + bw / 2.0, oy = by + bh / 2.0, oz = 0;
    const ns_css_value *origin = s->values[NS_CSS_TRANSFORM_ORIGIN];
    if (origin && origin->kind == NS_CSS_V_TRANSFORM &&
        origin->u.transform.n_ops > 0) {
        const ns_css_transform_op *o = &origin->u.transform.ops[0];
        ox = bx + (o->a_is_percent ? o->a / 100.0 * bw : o->a);
        oy = by + (o->b_is_percent ? o->b / 100.0 * bh : o->b);
        oz = o->c;
    }
    ns_mat4 tm;
    ns_css_transform_to_mat4(&eff, bw, bh, &tm);
    ns_mat4_translate(m, ox, oy, oz);
    ns_mat4_multiply(m, &tm, m);
    ns_mat4_translate(m, -ox, -oy, -oz);
    return TRUE;
}

/* Extends an extent by a rectangle's four corners mapped through m. */
static void
extent_add_corners(paint_extent *e, const ns_mat4 *m, double x0, double y0,
                   double x1, double y1)
{
    double xs[4] = { x0, x1, x0, x1 };
    double ys[4] = { y0, y0, y1, y1 };
    for (int i = 0; i < 4; i++) {
        double px, py, pz, pw;
        ns_mat4_apply(m, xs[i], ys[i], 0, &px, &py, &pz, &pw);
        if (fabs(pw) > 1e-9) { px /= pw; py /= pw; }
        e->x0 = MIN(e->x0, px);
        e->y0 = MIN(e->y0, py);
        e->x1 = MAX(e->x1, px);
        e->y1 = MAX(e->y1, py);
    }
}

/* Accumulates a subtree's transformed paint extent; FALSE on fixed, sticky or 3D parts. */
static gboolean
subtree_visual_extent(const ns_box *b, ns_anim *anim, const ns_mat4 *m,
                      paint_extent *e, gboolean root)
{
    if (b->fragment_context) return FALSE;
    ns_mat4 local = *m;
    if (!root && b->style) {
        if (ns_box_is_fixed(b) ||
            style_keyword_is(b->style, NS_CSS_POSITION, "sticky") ||
            style_keyword_is(b->style, NS_CSS_TRANSFORM_STYLE, "preserve-3d") ||
            box_has_perspective(b))
            return FALSE;
        gboolean three_d = FALSE;
        if (box_has_transform(b, anim, &three_d)) {
            if (three_d) return FALSE;
            ns_mat4 t;
            box_local_transform(b, anim, &t);
            ns_mat4_multiply(m, &t, &local);
        }
    }
    double x, y, w, h;
    box_border_rect(b, &x, &y, &w, &h);
    double pad = style_shadow_overflow(b->style);
    if (isfinite(x) && isfinite(y) && isfinite(w) && isfinite(h))
        extent_add_corners(e, &local, x - pad, y - pad, x + w + pad,
                           y + h + pad);
    if (b->paint_bottom > b->paint_top &&
        fabs(b->paint_top) < NS_DAMAGE_PAINT_BOUND &&
        fabs(b->paint_bottom) < NS_DAMAGE_PAINT_BOUND &&
        isfinite(x) && isfinite(w))
        extent_add_corners(e, &local, x - pad, b->paint_top - pad,
                           x + w + pad, b->paint_bottom + pad);
    if (b->scrolls) return TRUE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (!subtree_visual_extent(c, anim, &local, e, FALSE)) return FALSE;
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++) {
        const ns_inline_atomic *a =
            &g_array_index(b->inline_atomics, ns_inline_atomic, i);
        if (!a->box) continue;
        double tx = b->x + a->owner_offset_x - a->box->x;
        double ty = b->y + a->owner_offset_y - a->box->y;
        if (!isfinite(tx) || !isfinite(ty)) return FALSE;
        ns_mat4 shifted = local;
        ns_mat4_translate(&shifted, tx, ty, 0);
        if (!subtree_visual_extent(a->box, anim, &shifted, e, FALSE))
            return FALSE;
    }
    return TRUE;
}

/* Returns the parent's inline-atomic entry that places this box, or NULL. */
static const ns_inline_atomic *
box_atomic_slot(const ns_box *b)
{
    const ns_box *o = b->parent;
    for (guint i = 0; o && o->inline_atomics && i < o->inline_atomics->len;
         i++) {
        const ns_inline_atomic *a =
            &g_array_index(o->inline_atomics, ns_inline_atomic, i);
        if (a->box == b) return a;
    }
    return NULL;
}

/* Computes a box's on-screen paint rect through ancestor transforms and scrolls. */
gboolean
ns_damage_box_visual_rect(const ns_box *b, ns_anim *anim, ns_damage_rect *out)
{
    if (!b || !out) return FALSE;
    ns_mat4 id;
    ns_mat4_identity(&id);
    paint_extent e = { G_MAXDOUBLE, G_MAXDOUBLE, -G_MAXDOUBLE, -G_MAXDOUBLE, 0 };
    if (!subtree_visual_extent(b, anim, &id, &e, TRUE)) return FALSE;
    if (!(e.x1 > e.x0 && e.y1 > e.y0)) {
        memset(out, 0, sizeof *out);
        return TRUE;
    }
    ns_mat4 total;
    ns_mat4_identity(&total);
    gboolean fixed = FALSE;
    for (const ns_box *p = b; p; p = p->parent) {
        if (p->fragment_context) return FALSE;
        if (p->style) {
            if (style_keyword_is(p->style, NS_CSS_POSITION, "sticky") ||
                style_keyword_is(p->style, NS_CSS_TRANSFORM_STYLE, "preserve-3d") ||
                box_has_perspective(p))
                return FALSE;
        }
        if (p != b && (p->scroll_x != 0 || p->scroll_y != 0)) {
            if (!isfinite(p->scroll_x) || !isfinite(p->scroll_y)) return FALSE;
            ns_mat4 m;
            ns_mat4_identity(&m);
            ns_mat4_translate(&m, -p->scroll_x, -p->scroll_y, 0);
            ns_mat4_multiply(&m, &total, &total);
        }
        gboolean three_d = FALSE;
        if (p->style && box_has_transform(p, anim, &three_d)) {
            if (three_d) return FALSE;
            ns_mat4 m;
            box_local_transform(p, anim, &m);
            ns_mat4_multiply(&m, &total, &total);
        }
        const ns_inline_atomic *at = box_atomic_slot(p);
        if (at) {
            double tx = p->parent->x + at->owner_offset_x - p->x;
            double ty = p->parent->y + at->owner_offset_y - p->y;
            if (!isfinite(tx) || !isfinite(ty)) return FALSE;
            ns_mat4 m;
            ns_mat4_identity(&m);
            ns_mat4_translate(&m, tx, ty, 0);
            ns_mat4_multiply(&m, &total, &total);
        }
        if (ns_box_is_fixed(p)) fixed = TRUE;
    }
    double pad = NS_DAMAGE_INFLATE;
    paint_extent r = { G_MAXDOUBLE, G_MAXDOUBLE, -G_MAXDOUBLE, -G_MAXDOUBLE, 0 };
    extent_add_corners(&r, &total, e.x0 - pad, e.y0 - pad, e.x1 + pad,
                       e.y1 + pad);
    if (!isfinite(r.x0) || !isfinite(r.y0) || !isfinite(r.x1) ||
        !isfinite(r.y1))
        return FALSE;
    out->x = floor(r.x0);
    out->y = floor(r.y0);
    out->w = ceil(r.x1) - out->x;
    out->h = ceil(r.y1) - out->y;
    out->fixed = fixed;
    return TRUE;
}

/* Appends visual rects for boxes of the given nodes; FALSE if any rect is unresolvable. */
static gboolean
nodes_visual_rects_walk(const ns_box *b, ns_anim *anim, GHashTable *nodes,
                        GHashTable *found, GArray *out)
{
    if (b->dom && g_hash_table_contains(nodes, b->dom)) {
        ns_damage_rect r;
        if (!ns_damage_box_visual_rect(b, anim, &r)) return FALSE;
        if (r.w > 0 && r.h > 0) g_array_append_val(out, r);
        g_hash_table_add(found, (gpointer)b->dom);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (!nodes_visual_rects_walk(c, anim, nodes, found, out))
            return FALSE;
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++) {
        const ns_box *a =
            g_array_index(b->inline_atomics, ns_inline_atomic, i).box;
        if (a && !nodes_visual_rects_walk(a, anim, nodes, found, out))
            return FALSE;
    }
    return TRUE;
}

/* Collects visual rects of all boxes for the given nodes; FALSE means repaint fully. */
gboolean
ns_damage_nodes_visual_rects(const ns_box *root, ns_anim *anim,
                             GHashTable *nodes, GHashTable *found,
                             GArray *out)
{
    return !root || nodes_visual_rects_walk(root, anim, nodes, found, out);
}
