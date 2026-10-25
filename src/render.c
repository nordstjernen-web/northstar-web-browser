/* Northstar Web Browser — shared style/layout pipeline used by GUI and headless.
 * Runs cascade and layout, repeating for container queries, and honors viewport meta.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "render.h"
#include "trace.h"

#include <math.h>
#include <string.h>

#include "font.h"
#include "net.h"
#include "paint.h"

static gboolean g_render_page_uses_hover = FALSE;
static gboolean g_render_page_uses_containers = FALSE;

gboolean
ns_render_page_uses_containers(void)
{
    return g_render_page_uses_containers;
}

gboolean
ns_render_page_uses_hover(void)
{
    return g_render_page_uses_hover;
}

static gboolean g_render_page_uses_active = FALSE;

gboolean
ns_render_page_uses_active(void)
{
    return g_render_page_uses_active;
}

static ns_css_page_rule g_render_page_rule;
static gboolean         g_render_page_rule_set = FALSE;

/* The last @page rule seen in the current sheets, or NULL. */
const ns_css_page_rule *
ns_render_page_rule(void)
{
    return g_render_page_rule_set ? &g_render_page_rule : NULL;
}

/* Loads keyframes from all sheets and starts observing animations on styles. */
static void
render_feed_animations(const ns_render_ctx *c, GHashTable *styles)
{
    if (!c->anim) return;
    for (guint i = 0; i < c->n_sheets; i++)
        if (c->sheets[i]) ns_anim_load_from_stylesheet(c->anim, c->sheets[i]);
    ns_anim_observe_all(c->anim, styles, g_get_monotonic_time());
}

/* Requests @font-face sources allowed by fetch policy from the font loader. */
static void
render_request_fonts(const ns_render_ctx *c)
{
    if (!ns_font_available()) return;
    ns_fetch_policy *policy = ns_js_fetch_policy(c->js, NULL);
    for (guint i = 0; i < c->n_sheets; i++) {
        const ns_css_stylesheet *sh = c->sheets[i];
        if (!sh || !sh->font_faces) continue;
        for (guint j = 0; j < sh->font_faces->len; j++) {
            const ns_css_font_face *ff =
                &g_array_index(sh->font_faces, ns_css_font_face, j);
            if (!ff->family || !ff->src_url) continue;
            char *abs = c->resolve_url
                ? c->resolve_url(ff->src_url, c->cb_ud)
                : ns_url_resolve(c->base_url, ff->src_url);
            if (!abs) continue;
            if (ns_fetch_verdict_blocks(ns_fetch_policy_check(
                    policy, NS_FETCH_DEST_FONT, c->base_url, abs, NULL))) {
                g_free(abs);
                continue;
            }
            ns_font_request(ff->family, abs, c->base_url, policy);
            g_free(abs);
        }
    }
}

/* Scales computed font sizes by the page zoom factor when not 1. */
static void
render_apply_zoom(const ns_render_ctx *c, GHashTable *styles)
{
    double zoom = c->zoom > 0 ? c->zoom : 1.0;
    if (fabs(zoom - 1.0) <= 0.001) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val))
        ns_css_style_scale_font_size(val, zoom);
}

/* Records size containers (container-type not normal) with their measured sizes. */
static void
render_collect_containers(const ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom && b->style) {
        const ns_css_value *ct = b->style->values[NS_CSS_CONTAINER_TYPE];
        if (ct && ct->kind == NS_CSS_V_KEYWORD && ct->u.keyword &&
            g_ascii_strcasecmp(ct->u.keyword, "normal") != 0) {
            const ns_css_value *nm = b->style->values[NS_CSS_CONTAINER_NAME];
            const char *names = (nm && nm->kind == NS_CSS_V_KEYWORD)
                ? nm->u.keyword : NULL;
            const ns_css_value *wm = b->style->values[NS_CSS_WRITING_MODE];
            gboolean vertical = wm && wm->kind == NS_CSS_V_KEYWORD &&
                wm->u.keyword && g_str_has_prefix(wm->u.keyword, "vertical");
            ns_css_container_map_add(map, b->dom, ct->u.keyword, names,
                                     b->content_width, b->content_height,
                                     vertical);
        }
    }
    for (const ns_box *ch = b->first_child; ch; ch = ch->next_sibling)
        render_collect_containers(ch, map);
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++)
        render_collect_containers(
            g_array_index(b->inline_atomics, ns_inline_atomic, i).box, map);
}

/* Post-cascade style work: animations, web font requests and zoom. */
static void
render_style_pass(const ns_render_ctx *c, GHashTable *styles)
{
    render_feed_animations(c, styles);
    render_request_fonts(c);
    render_apply_zoom(c, styles);
}

/* Deep equality of computed CSS values, including layered values. */
static gboolean
render_value_equal(const ns_css_value *a, const ns_css_value *b)
{
    if (a == b) return TRUE;
    if (!a || !b || a->kind != b->kind) return FALSE;
    gboolean equal = FALSE;
    switch (a->kind) {
    case NS_CSS_V_KEYWORD:
        equal = g_strcmp0(a->u.keyword, b->u.keyword) == 0;
        break;
    case NS_CSS_V_LENGTH:
        equal = a->u.length.v == b->u.length.v &&
                a->u.length.unit == b->u.length.unit;
        break;
    case NS_CSS_V_SIZE:
        equal = a->u.size.w == b->u.size.w &&
                a->u.size.h == b->u.size.h &&
                a->u.size.w_unit == b->u.size.w_unit &&
                a->u.size.h_unit == b->u.size.h_unit &&
                a->u.size.w_auto == b->u.size.w_auto &&
                a->u.size.h_auto == b->u.size.h_auto;
        break;
    case NS_CSS_V_COLOR:
        equal = a->u.color.r == b->u.color.r &&
                a->u.color.g == b->u.color.g &&
                a->u.color.b == b->u.color.b &&
                a->u.color.a == b->u.color.a;
        break;
    case NS_CSS_V_CALC:
        equal = a->u.calc.pct == b->u.calc.pct &&
                a->u.calc.px == b->u.calc.px &&
                a->u.calc.em == b->u.calc.em &&
                a->u.calc.rem == b->u.calc.rem &&
                a->u.calc.lh == b->u.calc.lh &&
                a->u.calc.rlh == b->u.calc.rlh;
        break;
    case NS_CSS_V_URL:
        equal = g_strcmp0(a->u.url, b->u.url) == 0;
        break;
    case NS_CSS_V_RECT:
        equal = TRUE;
        for (int i = 0; i < 4 && equal; i++)
            equal = a->u.rect.v[i] == b->u.rect.v[i] &&
                    a->u.rect.unit[i] == b->u.rect.unit[i] &&
                    a->u.rect.is_auto[i] == b->u.rect.is_auto[i];
        break;
    default:
        return FALSE;
    }
    return equal && render_value_equal(a->next_layer, b->next_layer);
}

/* True if two custom property maps have the same names and values. */
static gboolean
render_vars_equal(const struct ns_var_map *a, const struct ns_var_map *b)
{
    if (a == b) return TRUE;
    GPtrArray *an = ns_var_map_names(a);
    GPtrArray *bn = ns_var_map_names(b);
    gboolean equal = an->len == bn->len;
    for (guint i = 0; equal && i < an->len; i++) {
        const char *name_a = g_ptr_array_index(an, i);
        const char *name_b = g_ptr_array_index(bn, i);
        equal = strcmp(name_a, name_b) == 0 &&
                g_strcmp0(ns_var_map_lookup(a, name_a),
                          ns_var_map_lookup(b, name_b)) == 0;
    }
    g_ptr_array_unref(an);
    g_ptr_array_unref(bn);
    return equal;
}

/* Deep equality of computed styles, including pseudo-element styles. */
static gboolean
render_style_equal(const ns_style *a, const ns_style *b)
{
    if (a == b) return TRUE;
    if (!a || !b || a->display.box != b->display.box ||
        a->display.outer != b->display.outer ||
        a->display.inner != b->display.inner ||
        a->display.internal != b->display.internal ||
        a->display.list_item != b->display.list_item)
        return FALSE;
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++)
        if (!render_value_equal(a->values[i], b->values[i])) return FALSE;
    return render_vars_equal(a->vars, b->vars) &&
           render_style_equal(a->before, b->before) &&
           render_style_equal(a->after, b->after) &&
           render_style_equal(a->first_letter, b->first_letter) &&
           render_style_equal(a->first_line, b->first_line) &&
           render_style_equal(a->placeholder, b->placeholder) &&
           render_style_equal(a->selection, b->selection) &&
           render_style_equal(a->marker, b->marker) &&
           render_style_equal(a->backdrop, b->backdrop) &&
           render_style_equal(a->file_selector_button,
                              b->file_selector_button);
}

/* True if two node-to-style tables hold equal styles for the same nodes. */
static gboolean
render_style_tables_equal(GHashTable *a, GHashTable *b)
{
    if (g_hash_table_size(a) != g_hash_table_size(b)) return FALSE;
    GHashTableIter iter;
    gpointer node, style;
    g_hash_table_iter_init(&iter, a);
    while (g_hash_table_iter_next(&iter, &node, &style)) {
        ns_style *other = g_hash_table_lookup(b, node);
        if (!render_style_equal(style, other)) return FALSE;
    }
    return TRUE;
}

/* Finds the first meta name="viewport" element. */
static const ns_node *
render_find_viewport_meta(const ns_node *n, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (ns_node_is_element_named(n, "meta")) {
        const char *name = ns_element_get_attr(n, "name");
        if (name && g_ascii_strcasecmp(name, "viewport") == 0)
            return n;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *found = render_find_viewport_meta(c, depth + 1);
        if (found) return found;
    }
    return NULL;
}

/* Parses an explicit width (320-4096) from viewport meta content; 0 otherwise. */
static double
render_parse_viewport_width(const char *content)
{
    if (!content || !*content) return 0;
    double out = 0;
    char **parts = g_strsplit_set(content, ",;", -1);
    for (int i = 0; parts && parts[i]; i++) {
        char *part = g_strstrip(parts[i]);
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = g_strstrip(part);
        char *value = g_strstrip(eq + 1);
        if (g_ascii_strcasecmp(key, "width") != 0) continue;
        if (g_ascii_strcasecmp(value, "device-width") == 0) break;
        char *end = NULL;
        double n = g_ascii_strtod(value, &end);
        if (end != value && n >= 320 && n <= 4096) {
            out = n;
            break;
        }
    }
    g_strfreev(parts);
    return out;
}

/* Layout width: the real viewport, widened to a larger viewport meta width. */
static double
render_effective_viewport_width(const ns_render_ctx *c)
{
    double width = c->viewport_width;
    const ns_node *meta = render_find_viewport_meta(c->doc, 0);
    const char *content = meta ? ns_element_get_attr(meta, "content") : NULL;
    double hint = render_parse_viewport_width(content);
    if (hint > width) width = hint;
    return width;
}

#define RENDER_CONTAINER_PASSES_MAX 3

/* Builds the container size map from a layout when enabled (else empty). */
static GHashTable *
render_measure_containers(const ns_box *layout, gboolean enabled)
{
    GHashTable *containers = ns_css_container_map_new();
    if (enabled) render_collect_containers(layout, containers);
    return containers;
}

static const ns_node *g_cq_seed_doc;
static double         g_cq_seed_width;
static GHashTable    *g_cq_seed;
static gboolean       g_cq_seed_settled;

/* Container map kept from the last render of this document and width, or NULL. */
static GHashTable *
render_container_seed(const ns_node *doc, double viewport_width)
{
    if (!g_cq_seed || g_cq_seed_doc != doc ||
        g_cq_seed_width != viewport_width)
        return NULL;
    return g_cq_seed;
}

/* Stores the measured container map for reuse by the next render. */
static void
render_keep_container_seed(const ns_node *doc, double viewport_width,
                           GHashTable *containers, gboolean settled)
{
    if (g_cq_seed && g_cq_seed != containers)
        g_hash_table_destroy(g_cq_seed);
    g_cq_seed = containers;
    g_cq_seed_doc = doc;
    g_cq_seed_width = viewport_width;
    g_cq_seed_settled = settled;
}

/* Container size map from the last render of doc, or NULL. */
GHashTable *
ns_render_container_map(const ns_node *doc)
{
    return g_cq_seed && g_cq_seed_doc == doc ? g_cq_seed : NULL;
}

static GHashTable    *g_offered_styles;
static const ns_node *g_offered_doc;
static double         g_offered_width;
static GHashTable    *g_offered_containers;

/* Offers a precomputed style table for the next relayout; takes ownership. */
void
ns_render_offer_styles(const ns_node *doc, double viewport_width,
                       GHashTable *containers, GHashTable *styles)
{
    if (g_offered_styles) g_hash_table_destroy(g_offered_styles);
    g_offered_styles = styles;
    g_offered_doc = doc;
    g_offered_width = viewport_width;
    g_offered_containers = containers;
}

/* Effective layout viewport width for doc, honoring viewport meta. */
double
ns_render_viewport_width(ns_node *doc, double viewport_width)
{
    ns_render_ctx c = { .doc = doc, .viewport_width = viewport_width };
    return render_effective_viewport_width(&c);
}

/* Takes offered styles if they match doc, width and container map; else frees them. */
static GHashTable *
render_take_offered_styles(const ns_node *doc, double viewport_width,
                           GHashTable *containers)
{
    GHashTable *styles = g_offered_styles;
    gboolean usable = styles && g_offered_doc == doc &&
                      g_offered_width == viewport_width &&
                      g_offered_containers == containers;
    g_offered_styles = NULL;
    if (styles && !usable) {
        g_hash_table_destroy(styles);
        styles = NULL;
    }
    return styles;
}

/* Cascade plus layout, re-running for container queries until sizes settle. */
GHashTable *
ns_render_relayout_profile(const ns_render_ctx *c, ns_box **out_layout,
                           ns_render_profile *profile)
{
    if (out_layout) *out_layout = NULL;
    if (!c || !out_layout) return NULL;
    if (profile) memset(profile, 0, sizeof *profile);

    double viewport_width = render_effective_viewport_width(c);
    ns_css_set_viewport(viewport_width, c->viewport_height);
    ns_css_set_focus_node(c->focused_input);
    ns_css_set_hover_node(c->hover_node);

    g_render_page_rule_set = FALSE;
    for (guint i = 0; i < c->n_sheets; i++)
        if (c->sheets[i] && c->sheets[i]->page_rule) {
            g_render_page_rule = *c->sheets[i]->page_rule;
            g_render_page_rule_set = TRUE;
        }

    gboolean uses_hover = FALSE;
    for (guint i = 0; i < c->n_sheets && !uses_hover; i++)
        uses_hover = ns_css_stylesheet_has_hover_rules(c->sheets[i]);
    g_render_page_uses_hover = uses_hover;

    gboolean uses_active = FALSE;
    for (guint i = 0; i < c->n_sheets && !uses_active; i++)
        uses_active = ns_css_stylesheet_has_active_rules(c->sheets[i]);
    g_render_page_uses_active = uses_active;

    gint64 t0 = profile ? g_get_monotonic_time() : 0;
    ns_css_set_render_zoom(c->zoom > 0 ? c->zoom : 1.0);
    gboolean container_rules = FALSE;
    for (guint i = 0; i < c->n_sheets && !container_rules; i++)
        container_rules = ns_css_stylesheet_has_container_rules(c->sheets[i]);
    GHashTable *seed = render_container_seed(c->doc, viewport_width);
    g_render_page_uses_containers = container_rules || seed != NULL;
    gboolean cache_selectors = container_rules &&
                               !(seed && g_cq_seed_settled);
    if (cache_selectors) ns_css_selector_cache_begin();
    gint64 trace_start = ns_trace_now();
    GHashTable *styles = render_take_offered_styles(c->doc, viewport_width,
                                                    seed);
    if (!styles) {
        ns_css_set_container_map(seed);
        styles = ns_css_compute_scoped(c->doc, c->sheets, c->sheet_docs,
                                       c->n_sheets);
        ns_css_set_container_map(NULL);
    }
    ns_trace_complete("style", "cascade", trace_start, NULL);
    gint64 t1 = profile ? g_get_monotonic_time() : 0;

    trace_start = ns_trace_now();
    render_style_pass(c, styles);
    ns_trace_complete("style", "style pass", trace_start, NULL);
    gint64 t2 = profile ? g_get_monotonic_time() : 0;

    trace_start = ns_trace_now();
    ns_paint_list_ordinals_begin();
    ns_box *layout = ns_layout_build(c->doc, styles, viewport_width,
                                     c->focused_input, c->caret_byte,
                                     c->sel_anchor_byte,
                                     c->images, c->base_url);
    ns_paint_list_ordinals_end();
    ns_trace_complete("layout", "layout", trace_start, NULL);
    gint64 t3 = profile ? g_get_monotonic_time() : 0;
    if (profile) {
        profile->css1_us = t1 - t0;
        profile->style1_us = t2 - t1;
        profile->layout1_us = t3 - t2;
    }

    gint64 tc0 = profile ? g_get_monotonic_time() : 0;
    GHashTable *measured = render_measure_containers(
        layout, container_rules || seed || ns_css_container_units_seen());
    gint64 tc1 = profile ? g_get_monotonic_time() : 0;
    if (profile) {
        profile->container_us = tc1 - tc0;
        profile->containers = g_hash_table_size(measured);
    }
    GHashTable *styled_with = seed;
    gboolean settled = seed ? ns_css_container_maps_equal(seed, measured)
                            : g_hash_table_size(measured) == 0;
    gboolean first_pass_settled = settled;
    for (int pass = 0; !settled && pass < RENDER_CONTAINER_PASSES_MAX; pass++) {
        if (profile) profile->container_pass = TRUE;
        ns_css_set_container_map(measured);
        ns_css_container_features_begin();
        gint64 t4 = profile ? g_get_monotonic_time() : 0;
        trace_start = ns_trace_now();
        GHashTable *styles2 = ns_css_compute_scoped(c->doc, c->sheets,
                                                    c->sheet_docs, c->n_sheets);
        ns_trace_complete("style", "cascade for container queries",
                          trace_start, NULL);
        gint64 t5 = profile ? g_get_monotonic_time() : 0;
        gboolean container_features_used = ns_css_container_features_used();
        ns_css_set_container_map(NULL);
        if (styled_with != seed) g_hash_table_destroy(styled_with);
        styled_with = measured;
        if (profile) profile->css2_us += t5 - t4;
        if ((!container_features_used && !seed && pass == 0) ||
            render_style_tables_equal(styles, styles2)) {
            g_hash_table_destroy(styles2);
            settled = TRUE;
            break;
        }
        render_style_pass(c, styles2);
        gint64 t6 = profile ? g_get_monotonic_time() : 0;
        trace_start = ns_trace_now();
        ns_paint_list_ordinals_begin();
        ns_box *layout2 = ns_layout_build(c->doc, styles2, viewport_width,
                                          c->focused_input, c->caret_byte,
                                          c->sel_anchor_byte,
                                          c->images, c->base_url);
        ns_paint_list_ordinals_end();
        ns_trace_complete("layout", "layout for container queries",
                          trace_start, NULL);
        gint64 t7 = profile ? g_get_monotonic_time() : 0;
        if (profile) {
            profile->style2_us += t6 - t5;
            profile->layout2_us += t7 - t6;
        }
        ns_box_free(layout);
        g_hash_table_destroy(styles);
        layout = layout2;
        styles = styles2;
        measured = render_measure_containers(layout, TRUE);
        settled = ns_css_container_maps_equal(styled_with, measured);
    }
    if (styled_with != seed && styled_with != measured)
        g_hash_table_destroy(styled_with);
    render_keep_container_seed(c->doc, viewport_width, measured,
                               first_pass_settled);
    if (cache_selectors) ns_css_selector_cache_end();
    ns_css_set_focus_node(NULL);
    ns_css_set_hover_node(NULL);

    if (c->js) {
        ns_js_set_style_table(c->js, styles);
        ns_js_set_layout_root(c->js, layout);
    }
    *out_layout = layout;
    return styles;
}

GHashTable *
ns_render_relayout(const ns_render_ctx *c, ns_box **out_layout)
{
    return ns_render_relayout_profile(c, out_layout, NULL);
}
