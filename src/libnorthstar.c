/* Northstar Web Browser — internal page-engine host for the GTK shell and headless driver.
 * Restyle/relayout scheduling, image loading, input and focus, scrolling, rendering, debug dumps.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "libnorthstar.h"
#include "mainctx.h"

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "anim.h"
#include "bytecode_cache.h"
#include "cache.h"
#include "damage.h"
#include "debuglog.h"
#include "history.h"
#include "config.h"
#include "css.h"
#include "dom.h"
#include "engine.h"
#include "font.h"
#include "forms.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "spellcheck.h"
#include "paint.h"
#include "print.h"
#include "render.h"
#include "safebrowsing.h"
#include "security.h"
#include "selection.h"
#include "camera.h"
#include "trace.h"

static int ns_browser_animating(ns_browser *browser);

#define NS_IMAGE_RELAYOUT_BATCH 8

struct ns_browser {
    ns_node        *doc;
    ns_box         *layout;
    GHashTable     *styles;
    GHashTable     *layout_styles;
    char           *layout_base_href;
    const char     *relayout_why;
    guint           styles_serial;
    gboolean        styles_serial_valid;
    ns_js          *js;
    ns_anim        *anim;
    ns_image_cache *images;
    GHashTable     *css_cache;
    char           *base_url;
    gboolean        url_pushed;
    char           *doc_charset;
    char           *doc_language;
    int             vw;
    double          vh;
    gboolean        images_fetched;
    gboolean        has_deferred_lazy;
    gboolean        bfcache_ok;
    double          cur_scroll_x;
    double          cur_scroll_y;
    double          js_scroll_x;
    double          js_scroll_y;
    double          cur_viewport_h;
    int             pending_scroll_x;
    int             pending_scroll_y;
    gboolean        pending_scroll;
    const ns_node  *scroll_anchor;
    int             scroll_anchor_y;
    GPtrArray      *img_sessions;
    guint           image_arrivals_since_layout;
    guint           media_events_source;
    gboolean        images_arrived_since_layout;
    gint64          load_delay_deadline_us;
    GHashTable     *img_requested;
    gboolean        dirty;
    gboolean        restyle_pending;
    gboolean        cascade_dirty;
    gboolean        repaint_pending;
    gboolean        damage_full;
    GHashTable     *damage_nodes;
    GArray         *damage_rects;
    GHashTable     *damage_images;
    guint           layout_gen;
    guint           hazards_gen;
    ns_paint_hazards hazards;
    gint64          frame_time_us;
    gint64          last_frame_now_us;
    gboolean        relaying;
    char           *pending_nav;
    char           *pending_download;
    char           *pending_clipboard;
    char           *refresh_url;
    gint64          refresh_due_us;
    char           *pending_post_body;
    char           *pending_nav_initiator;
    gboolean        safebrowsing_interstitial;
    gsize           pending_post_len;
    char           *pending_post_ct;
    gsize           caret_byte;
    gsize           sel_anchor_byte;
    const ns_node  *caret_blink_node;
    gsize           caret_blink_byte;
    gsize           caret_blink_anchor;
    gint64          caret_blink_epoch_us;
    gboolean        caret_blink_active;
    gboolean        caret_paint_visible;
    ns_selection    selection;
    gboolean        selection_dragged;
    const ns_node  *hover_node;
    const ns_node  *open_select;
    gboolean        datalist_suppressed;
    const ns_node  *press_node;
    int             press_x;
    int             press_y;
    int             press_mods;
    gboolean        press_active;
    const ns_node  *media_seek_node;
    char           *search_query;
    gboolean        search_case;
    const ns_box   *search_active;
    GString        *console_buf;
    guint64         layout_sig[2];
    int             layout_osc;
    gint64          last_layout_us;
    gint64          damp_until_us;
    gboolean        damp_logged;
    gint64          hover_relayout_us;
    gint64          relayout_cost_us;
    gboolean        hover_restyle_pending;
    ns_box         *sb_box;
    const ns_node  *sb_node;
    double          sb_grab;
    gboolean        sb_dragging;
    int             security;
    char           *remote_ip;
};

#define NS_LAYOUT_OSC_THRESHOLD 12
#define NS_LAYOUT_RAPID_US (100 * 1000)
#define NS_LAYOUT_DAMP_US (400 * 1000)
#define NS_LAYOUT_EXPENSIVE_US (1000 * 1000)
#define NS_LAYOUT_DAMP_MAX_US (1 * G_USEC_PER_SEC)
#define NS_CARET_BLINK_US (530 * 1000)
#define NS_FRAME_TIME_MAX_LAG_US (100 * 1000)

/* TRUE if target is in the subtree forest starting at root (recursive search). */
static gboolean
browser_doc_has_node(const ns_node *root, const ns_node *target)
{
    for (const ns_node *n = root; n; n = n->next_sibling) {
        if (n == target) return TRUE;
        if (n->first_child && browser_doc_has_node(n->first_child, target))
            return TRUE;
    }
    return FALSE;
}

/* TRUE if a cached node pointer is still attached to the current document. */
static gboolean
browser_node_alive(const ns_browser *browser, const ns_node *node)
{
    if (!node || !browser || !browser->doc) return FALSE;
    return browser_doc_has_node(browser->doc, node);
}

/* Clears press, hover, open-select and media-seek pointers to detached nodes. */
static void
browser_prune_cached_nodes(ns_browser *browser)
{
    if (!browser || !browser->doc) return;
    if (browser->press_node && !browser_node_alive(browser, browser->press_node))
        browser->press_node = NULL;
    if (browser->hover_node && !browser_node_alive(browser, browser->hover_node))
        browser->hover_node = NULL;
    if (browser->open_select && !browser_node_alive(browser, browser->open_select))
        browser->open_select = NULL;
    if (browser->media_seek_node &&
        !browser_node_alive(browser, browser->media_seek_node))
        browser->media_seek_node = NULL;
}

/* Folds box kinds and quarter-pixel geometry of a box tree into an FNV-1a hash. */
static guint64
layout_signature_walk(const ns_box *b, guint64 h)
{
    if (!b) return h;
    gint32 q[4] = {
        (gint32)(b->x * 4), (gint32)(b->y * 4),
        (gint32)(b->content_width * 4), (gint32)(b->content_height * 4),
    };
    const guchar *bytes = (const guchar *)q;
    h ^= (guint64)b->kind;
    h *= 0x100000001b3ULL;
    for (gsize i = 0; i < sizeof q; i++) {
        h ^= bytes[i];
        h *= 0x100000001b3ULL;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        h = layout_signature_walk(c, h);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            h = layout_signature_walk(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, h);
    return h;
}

/* FNV-1a hash of the layout tree geometry, used to detect reflow oscillation. */
static guint64
layout_signature(const ns_box *root)
{
    return layout_signature_walk(root, 0xcbf29ce484222325ULL);
}

/* Resets the layout oscillation dampener state. */
static void
browser_damp_reset(ns_browser *b)
{
    b->layout_osc = 0;
    b->damp_until_us = 0;
    b->damp_logged = FALSE;
}

typedef struct { double x, y; } browser_scroll_pos;

/* Records non-zero scroll offsets of boxes by DOM node before relayout. */
static void
browser_collect_scroll(const ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom && (b->scroll_y != 0.0 || b->scroll_x != 0.0)) {
        browser_scroll_pos *p = g_new(browser_scroll_pos, 1);
        p->x = b->scroll_x;
        p->y = b->scroll_y;
        g_hash_table_insert(map, (gpointer)b->dom, p);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        browser_collect_scroll(c, map);
}

/* Restores saved scroll offsets onto the new box tree, clamped to the new extents. */
static void
browser_restore_scroll(ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom) {
        browser_scroll_pos *p = g_hash_table_lookup(map, b->dom);
        if (p) {
            double maxy = b->scroll_max_y > 0 ? b->scroll_max_y : 0;
            double maxx = b->scroll_max_x > 0 ? b->scroll_max_x : 0;
            double y = p->y < 0 ? 0 : (p->y > maxy ? maxy : p->y);
            double x = p->x < 0 ? 0 : (p->x > maxx ? maxx : p->x);
            b->scroll_y = y;
            b->scroll_x = x;
        }
    }
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        browser_restore_scroll(c, map);
}

static void browser_ensure_images(ns_browser *browser);
static void browser_schedule_media_events(ns_browser *b);

static void browser_relayout_impl(ns_browser *b);

/* Relayouts and records a trace event with the cause. */
static void
browser_relayout(ns_browser *b)
{
    gint64 start = ns_trace_now();
    const char *why = b->js ? ns_js_layout_mutated_by(b->js) : NULL;
    if (!why) why = b->relayout_why;
    b->relayout_why = NULL;
    browser_relayout_impl(b);
    ns_trace_complete("layout", "relayout", start, why);
}

/* Full relayout: rebuilds styles and boxes, keeps scroll, tracks reflow oscillation. */
static void
browser_relayout_impl(ns_browser *b)
{
    if (b->relaying) { b->dirty = TRUE; return; }
    gint64 relayout_t0 = g_get_monotonic_time();
    gboolean rapid = b->last_layout_us > 0 &&
                     relayout_t0 - b->last_layout_us < NS_LAYOUT_RAPID_US;
    b->relaying = TRUE;
    b->restyle_pending = FALSE;
    if (b->js) {
        (void)ns_js_consume_mutated(b->js);
        (void)ns_js_consume_layout_mutated(b->js);
        GPtrArray *targets = ns_js_take_layout_targets(b->js);
        if (targets) g_ptr_array_free(targets, TRUE);
    }
    g_free(b->layout_base_href);
    b->layout_base_href = g_strdup(ns_engine_document_base_href(b->doc));
    b->image_arrivals_since_layout = 0;
    GHashTable *scroll_save =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    browser_collect_scroll(b->layout, scroll_save);
    ns_css_set_viewport((double)b->vw, b->vh);
    ns_css_set_doc_language(b->doc_language);
    if (b->js) ns_js_sync_window_metrics(b->js);
    b->search_active = NULL;
    ns_selection_clear(&b->selection);
    if (b->js && b->layout) ns_js_set_layout_root(b->js, NULL);
    if (b->layout) {
        ns_paint_3d_invalidate();
        ns_box_free(b->layout);
        b->layout = NULL;
        b->sb_box = NULL;
        b->sb_node = NULL;
        b->sb_dragging = FALSE;
    }
    if (b->layout_styles) {
        g_hash_table_destroy(b->layout_styles);
        b->layout_styles = NULL;
    }
    if (b->js && b->styles) ns_js_set_style_table(b->js, NULL);
    if (b->styles) { g_hash_table_destroy(b->styles); b->styles = NULL; }
    browser_prune_cached_nodes(b);
    ns_layout_set_open_select(b->open_select);
    {
        const ns_node *fn = b->js ? ns_js_focused_node(b->js) : NULL;
        gboolean dl_open = !b->datalist_suppressed && fn &&
                           ns_node_is_element_named(fn, "input") &&
                           ns_element_get_attr(fn, "list") != NULL;
        ns_layout_set_datalist_open(dl_open);
    }
    b->styles = ns_engine_relayout(b->doc, b->base_url, b->vw, b->vh,
                                   b->images, b->anim, b->js,
                                   b->css_cache,
                                   b->js ? ns_js_focused_node(b->js) : NULL,
                                   b->hover_node,
                                   b->caret_byte, b->sel_anchor_byte,
                                   &b->layout);
    ns_render_offer_styles(NULL, 0, NULL, NULL);
    b->cascade_dirty = FALSE;
    b->relaying = FALSE;
    b->damage_full = TRUE;
    b->layout_gen++;
    if (g_hash_table_size(scroll_save) > 0)
        browser_restore_scroll(b->layout, scroll_save);
    g_hash_table_destroy(scroll_save);
    b->images_fetched = FALSE;
    b->has_deferred_lazy = FALSE;
    b->images_arrived_since_layout = FALSE;
    if (b->js) {
        ns_js_set_style_table(b->js, b->styles);
        ns_js_set_layout_root(b->js, b->layout);
        browser_ensure_images(b);
        browser_schedule_media_events(b);
    }

    gint64 now = g_get_monotonic_time();
    b->relayout_cost_us = now - relayout_t0;
    b->last_layout_us = now;
    guint64 sig = layout_signature(b->layout);
    gboolean alternating = sig != b->layout_sig[0] && sig == b->layout_sig[1];
    if (alternating) {
        if (rapid && b->relayout_cost_us >= NS_LAYOUT_EXPENSIVE_US)
            b->layout_osc = NS_LAYOUT_OSC_THRESHOLD;
        else if (rapid && b->layout_osc < G_MAXINT)
            b->layout_osc++;
    } else if (sig != b->layout_sig[0]) {
        browser_damp_reset(b);
    }
    b->layout_sig[1] = b->layout_sig[0];
    b->layout_sig[0] = sig;
    if (b->layout_osc >= NS_LAYOUT_OSC_THRESHOLD) {
        gint64 damp_us = b->relayout_cost_us * 2;
        if (damp_us < NS_LAYOUT_DAMP_US) damp_us = NS_LAYOUT_DAMP_US;
        if (damp_us > NS_LAYOUT_DAMP_MAX_US) damp_us = NS_LAYOUT_DAMP_MAX_US;
        b->damp_until_us = now + damp_us;
    }
}

/* Relayouts after script mutation unless the oscillation dampener defers it. */
static gboolean
browser_relayout_from_mutation(ns_browser *b)
{
    if (b->layout && b->layout_osc >= NS_LAYOUT_OSC_THRESHOLD) {
        gint64 now = g_get_monotonic_time();
        if (!b->damp_logged) {
            b->damp_logged = TRUE;
            g_message("northstar: layout dampener engaged "
                      "(script reflow loop with no user input)");
        }
        if (now < b->damp_until_us) {
            b->dirty = TRUE;
            return FALSE;
        }
    }
    browser_relayout(b);
    return TRUE;
}

/* TRUE for overflow hidden or clip. */
static gboolean
overflow_keyword_hidden(const char *kw)
{
    return kw && (g_ascii_strcasecmp(kw, "hidden") == 0 ||
                  g_ascii_strcasecmp(kw, "clip") == 0);
}

/* TRUE if overflow on the axis (falling back to overflow) is hidden or clip. */
static gboolean
box_axis_overflow_hidden(const ns_box *b, ns_css_prop axis)
{
    if (!b || !b->style) return FALSE;
    const char *kw = ns_style_keyword(b->style, axis);
    if (!kw) kw = ns_style_keyword(b->style, NS_CSS_OVERFLOW);
    return overflow_keyword_hidden(kw);
}

/* TRUE if an html or body box hides overflow on the given axis. */
static gboolean
root_axis_overflow_hidden(const ns_box *b, ns_css_prop axis)
{
    if (!b) return FALSE;
    if (b->dom && b->dom->name &&
        (strcmp(b->dom->name, "html") == 0 ||
         strcmp(b->dom->name, "body") == 0) &&
        box_axis_overflow_hidden(b, axis))
        return TRUE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (root_axis_overflow_hidden(c, axis)) return TRUE;
    return FALSE;
}

static int browser_images_outstanding(ns_browser *browser);

/* Idle callback that fires media element load events once after layout. */
static gboolean
browser_fire_media_events(gpointer user_data)
{
    ns_browser *b = user_data;
    b->media_events_source = 0;
    if (b->js && b->layout) ns_js_fire_media_load_events(b->js, b->layout);
    return G_SOURCE_REMOVE;
}

/* Schedules a one-shot dispatch of media load events if none is pending. */
static void
browser_schedule_media_events(ns_browser *b)
{
    if (!b->media_events_source)
        b->media_events_source =
            ns_engine_timeout_add(0, browser_fire_media_events, b);
}

/* TRUE while the load event should wait for images or media events (bounded). */
static gboolean
browser_load_waits_for_images(gpointer user_data)
{
    ns_browser *b = user_data;
    if (g_get_monotonic_time() >= b->load_delay_deadline_us) return FALSE;
    return b->media_events_source != 0 || b->images_arrived_since_layout ||
           browser_images_outstanding(b) > 0;
}

/* Image arrival callback: repaints, and relayouts every 8 arrivals or when all are done. */
static void
browser_image_arrived(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b) return;
    b->images_arrived_since_layout = TRUE;
    b->image_arrivals_since_layout++;
    b->repaint_pending = TRUE;
    b->damage_full = TRUE;
    if (b->image_arrivals_since_layout >= NS_IMAGE_RELAYOUT_BATCH ||
        browser_images_outstanding(b) == 0) {
        b->image_arrivals_since_layout = 0;
        b->dirty = TRUE;
        if (!b->relayout_why) b->relayout_why = "images arrived";
    }
}

/* Counts in-flight image fetches, closing finished image sessions. */
static int
browser_images_outstanding(ns_browser *browser)
{
    if (!browser->img_sessions) return 0;
    int total = 0;
    for (guint i = 0; i < browser->img_sessions->len; ) {
        ns_engine_img_session *s = g_ptr_array_index(browser->img_sessions, i);
        int o = ns_engine_img_session_outstanding(s);
        if (o == 0) {
            ns_engine_img_session_close(s);
            g_ptr_array_remove_index_fast(browser->img_sessions, i);
            continue;
        }
        total += o;
        i++;
    }
    return total;
}

/* Starts image fetches for the layout, deferring lazy images outside the viewport. */
static void
browser_ensure_images(ns_browser *browser)
{
    if (browser->images_fetched && !browser->has_deferred_lazy) return;
    if (!browser->img_requested)
        browser->img_requested = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    if (!browser->img_sessions)
        browser->img_sessions = g_ptr_array_new();
    double vp_h = browser->cur_viewport_h > 0.0 ? browser->cur_viewport_h
                                                : browser->vh;
    gboolean deferred = FALSE;
    ns_engine_img_session *s =
        ns_engine_fetch_images_start(browser->layout, browser->base_url,
                                     browser->images, browser->js,
                                     browser->img_requested,
                                     browser->cur_scroll_y, vp_h, &deferred,
                                     browser_image_arrived, browser);
    if (s) g_ptr_array_add(browser->img_sessions, s);
    browser->images_fetched = TRUE;
    browser->has_deferred_lazy = deferred;
}

/* Blocks (up to 15s) running the engine loop until images load, then relayouts. */
static void
browser_wait_images(ns_browser *browser)
{
    browser_ensure_images(browser);
    gint64 deadline = g_get_monotonic_time() + (gint64)15 * G_USEC_PER_SEC;
    while (browser_images_outstanding(browser) > 0 &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(ns_engine_context(), TRUE);
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
}

enum { BROWSER_JS_NONE, BROWSER_JS_RESTYLE, BROWSER_JS_RELAYOUT };

/* Consumes script mutation flags and classifies the needed work as restyle or relayout. */
static int
browser_absorb_js_mutation(ns_browser *b)
{
    if (!b->js || !ns_js_consume_mutated(b->js)) return BROWSER_JS_NONE;
    ns_render_offer_styles(NULL, 0, NULL, NULL);
    const char *why = ns_js_layout_mutated_by(b->js);
    if (ns_js_consume_layout_mutated(b->js) || !b->layout) {
        if (why && !b->relayout_why) b->relayout_why = why;
        b->dirty = TRUE;
        return BROWSER_JS_RELAYOUT;
    }
    b->restyle_pending = TRUE;
    return BROWSER_JS_RESTYLE;
}

/* TRUE if the node has an <svg> ancestor. */
static gboolean
node_in_svg(const ns_node *n)
{
    for (const ns_node *p = n ? n->parent : NULL; p; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT && p->name &&
            g_ascii_strcasecmp(p->name, "svg") == 0)
            return TRUE;
    return FALSE;
}

/* Records an old-to-new style mapping for a pseudo-element style when it changed. */
static void
restyle_map_pseudo(GHashTable *map, const ns_style *o, const ns_style *n)
{
    if (o && n && o != n) g_hash_table_insert(map, (gpointer)o, (gpointer)n);
}

/* Debug (NS_LAYOUT_VERIFY): compares incremental layout to a fresh one and logs diffs. */
static void
browser_verify_layout(ns_browser *b, const char *what)
{
    static int verify = -1;
    static guint checked, failed;
    if (verify < 0) verify = g_getenv("NS_LAYOUT_VERIFY") != NULL;
    if (!verify || !b->layout) return;
    ns_css_set_viewport((double)b->vw, b->vh);
    ns_css_set_doc_language(b->doc_language);
    ns_layout_set_open_select(b->open_select);
    ns_box *ref = NULL;
    gboolean in_sync = b->styles_serial_valid &&
                       b->styles_serial == ns_css_incremental_serial();
    GHashTable *styles = ns_engine_relayout(
        b->doc, b->base_url, b->vw, b->vh, b->images, b->anim, b->js,
        b->css_cache, b->js ? ns_js_focused_node(b->js) : NULL, b->hover_node,
        b->caret_byte, b->sel_anchor_byte, &ref);
    char *diff = ns_layout_diff(b->layout, ref);
    checked++;
    if (diff) {
        failed++;
        g_printerr("[layout-verify] %s: %s\n", what, diff);
    }
    if (checked % 50 == 0 || diff)
        g_printerr("[layout-verify] %u checked, %u differ\n", checked, failed);
    g_free(diff);
    if (b->js) {
        ns_js_set_layout_root(b->js, b->layout);
        ns_js_set_style_table(b->js, b->styles);
    }
    ns_box_free(ref);
    if (styles) g_hash_table_destroy(styles);
    if (in_sync) b->styles_serial = ns_css_incremental_serial();
}

static gboolean browser_restyle_impl(ns_browser *b, GPtrArray *targets);
static gboolean target_children_unrendered(GHashTable *styles,
                                           const ns_node *doc,
                                           const ns_node *t);
static void browser_damage_node(ns_browser *browser, const ns_node *node);

/* Tries a restyle without relayout for script changes; TRUE if the layout was kept. */
static gboolean
browser_restyle(ns_browser *b)
{
    GPtrArray *targets = b->js ? ns_js_take_layout_targets(b->js) : NULL;
    gint64 start = ns_trace_now();
    gboolean hopeless = FALSE;
    for (guint i = 0; targets && !hopeless && i < targets->len; i++)
        hopeless = !b->styles ||
                   !target_children_unrendered(b->styles, b->doc,
                                               g_ptr_array_index(targets, i));
    gboolean kept = !hopeless && browser_restyle_impl(b, targets);
    if (hopeless) b->dirty = TRUE;
    if (!kept && !b->relayout_why)
        b->relayout_why = targets ? "nodes added or removed"
                                  : "style change moved boxes";
    if (targets) g_ptr_array_free(targets, TRUE);
    ns_trace_complete("style", "restyle", start,
                      kept ? "layout kept"
                      : targets ? "needs relayout: nodes added or removed"
                      : "needs relayout");
    return kept;
}

/* TRUE if the node is in a display:none subtree or inside SVG (outside foreignObject). */
static gboolean
node_unrendered(GHashTable *styles, const ns_node *n)
{
    for (const ns_node *p = n; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT || !p->name) continue;
        if (p != n && g_ascii_strcasecmp(p->name, "svg") == 0) return TRUE;
        if (g_ascii_strcasecmp(p->name, "foreignObject") == 0) return FALSE;
        const ns_style *st = g_hash_table_lookup(styles, p);
        if (st && ns_display_is_none(ns_css_display_of(st))) return TRUE;
    }
    return FALSE;
}

/* TRUE if changes under a mutation target cannot affect layout (detached, hidden, SVG). */
static gboolean
target_children_unrendered(GHashTable *styles, const ns_node *doc,
                           const ns_node *t)
{
    const ns_node *root = t;
    while (root->parent) root = root->parent;
    if (root != doc) return TRUE;
    static const char *const read_hidden[] = {
        "select", "datalist", "option", "optgroup", "textarea", "picture",
        "video", "audio", "object", "embed", "iframe", "frame", "canvas",
        "map", "details", "noscript", "template",
    };
    for (const ns_node *p = t; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT || !p->name) continue;
        for (guint i = 0; i < G_N_ELEMENTS(read_hidden); i++)
            if (g_ascii_strcasecmp(p->name, read_hidden[i]) == 0) return FALSE;
    }
    if (t->kind == NS_NODE_ELEMENT && t->name &&
        g_ascii_strcasecmp(t->name, "svg") == 0)
        return TRUE;
    return node_unrendered(styles, t);
}

/* Returns the nearest <svg> element containing or equal to the node. */
static const ns_node *
node_svg_painter(const ns_node *n)
{
    for (const ns_node *p = n; p; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT && p->name &&
            g_ascii_strcasecmp(p->name, "svg") == 0)
            return p;
    return NULL;
}

/* Recomputes the cascade and patches box styles in place if no layout change; damages rects. */
static gboolean
browser_restyle_impl(ns_browser *b, GPtrArray *targets)
{
    b->restyle_pending = FALSE;
    gint64 t0 = g_get_monotonic_time();
    GHashTable *containers = ns_render_container_map(b->doc);
    if (!b->layout || b->relaying || !b->styles ||
        (!containers &&
         (ns_render_page_uses_containers() || ns_css_container_units_seen()))) {
        b->dirty = TRUE;
        return FALSE;
    }
    GHashTable *box_styles = b->layout_styles ? b->layout_styles : b->styles;
    double cascade_vw = ns_render_viewport_width(b->doc, (double)b->vw);
    ns_css_set_viewport(cascade_vw, b->vh > 0 ? b->vh : cascade_vw * 0.75);
    ns_css_set_doc_language(b->doc_language);
    const ns_node *prev_focus =
        ns_css_set_focus_node(b->js ? ns_js_focused_node(b->js) : NULL);
    const ns_node *prev_hover = ns_css_set_hover_node(b->hover_node);
    ns_css_set_container_map(containers);
    GPtrArray *delta = NULL;
    if (!b->layout_styles && b->styles_serial_valid &&
        b->styles_serial == ns_css_incremental_serial())
        delta = g_ptr_array_new_with_free_func(ns_css_style_change_free);
    GHashTable *styles = delta
        ? ns_engine_compute_cascade_delta(b->doc, b->base_url, b->css_cache,
                                          b->anim, delta)
        : ns_engine_compute_cascade(b->doc, b->base_url, b->css_cache,
                                    b->anim);
    if (styles && delta) g_clear_pointer(&delta, g_ptr_array_unref);
    b->styles_serial_valid = FALSE;
    ns_css_set_container_map(NULL);
    ns_css_set_hover_node(prev_hover);
    ns_css_set_focus_node(prev_focus);
    gint64 t1 = g_get_monotonic_time();
    GHashTable *old_to_new = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *moved = g_hash_table_new(g_direct_hash, g_direct_equal);
    GPtrArray *changed_nodes = g_ptr_array_new();
    gboolean structural = targets && targets->len > 0 &&
        ns_css_last_cascade_continued() &&
        g_strcmp0(ns_engine_document_base_href(b->doc),
                  b->layout_base_href) == 0;
    for (guint i = 0; structural && styles && i < targets->len; i++)
        structural = target_children_unrendered(styles, b->doc,
                                                g_ptr_array_index(targets, i));
    gboolean ok = (targets && targets->len > 0) ? structural
        : delta || g_hash_table_size(styles) == g_hash_table_size(box_styles);
    GHashTableIter it;
    if (styles) g_hash_table_iter_init(&it, styles);
    for (guint di = 0; ok; di++) {
        gpointer key, val;
        if (delta) {
            if (di >= delta->len) break;
            ns_css_style_change *c = g_ptr_array_index(delta, di);
            key = (gpointer)c->node;
            val = c->style;
        } else if (!g_hash_table_iter_next(&it, &key, &val)) {
            break;
        }
        const ns_style *ns = val;
        const ns_style *os = g_hash_table_lookup(box_styles, key);
        if (os == ns) continue;
        if (!os && structural && node_unrendered(styles, key)) continue;
        if (os && node_in_svg(key)) {
            g_ptr_array_add(changed_nodes, key);
            continue;
        }
        int change = os ? ns_css_style_layout_change(os, ns)
                        : NS_CSS_LAYOUT_CHANGED;
        if (change == NS_CSS_LAYOUT_CHANGED) {
            ok = FALSE;
            break;
        }
        g_hash_table_insert(old_to_new, (gpointer)os, (gpointer)ns);
        g_ptr_array_add(changed_nodes, key);
        if (change == NS_CSS_LAYOUT_MOVED) g_hash_table_add(moved, (gpointer)os);
        restyle_map_pseudo(old_to_new, os->before, ns->before);
        restyle_map_pseudo(old_to_new, os->after, ns->after);
        restyle_map_pseudo(old_to_new, os->first_letter, ns->first_letter);
        restyle_map_pseudo(old_to_new, os->first_line, ns->first_line);
        restyle_map_pseudo(old_to_new, os->placeholder, ns->placeholder);
        restyle_map_pseudo(old_to_new, os->selection, ns->selection);
        restyle_map_pseudo(old_to_new, os->marker, ns->marker);
        restyle_map_pseudo(old_to_new, os->backdrop, ns->backdrop);
        restyle_map_pseudo(old_to_new, os->file_selector_button,
                           ns->file_selector_button);
    }
    guint changed = g_hash_table_size(old_to_new);
    ns_layout_restyle_plan *plan = ok
        ? ns_layout_restyle_plan_new(b->layout, styles ? styles : b->styles,
                                     old_to_new, moved)
        : NULL;
    ok = plan != NULL;
    const GPtrArray *boxes = ns_layout_restyle_plan_changed(plan);
    GArray *rects = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
    gboolean rects_ok = ok;
    for (int pass = 0; pass < 2 && ok; pass++) {
        if (pass == 1) ns_layout_restyle_apply(plan);
        for (guint i = 0; rects_ok && i < boxes->len; i++) {
            ns_damage_rect r;
            rects_ok = ns_damage_box_visual_rect(g_ptr_array_index(boxes, i),
                                                 b->anim, &r);
            if (rects_ok && r.w > 0 && r.h > 0) g_array_append_val(rects, r);
        }
    }
    ns_layout_restyle_plan_free(plan);
    g_hash_table_destroy(old_to_new);
    g_hash_table_destroy(moved);
    if (!ok) {
        if (g_getenv("NS_PROFILE"))
            g_printerr("[profile] restyle needs relayout styles=%u %.2fms "
                       "cascade=%.2f\n", changed,
                       (g_get_monotonic_time() - t0) / 1000.0,
                       (t1 - t0) / 1000.0);
        g_array_free(rects, TRUE);
        g_ptr_array_free(changed_nodes, TRUE);
        if (styles)
            ns_render_offer_styles(b->doc, cascade_vw, containers, styles);
        if (delta) g_ptr_array_unref(delta);
        b->dirty = TRUE;
        return FALSE;
    }
    if (b->js) ns_js_set_style_table(b->js, NULL);
    if (b->layout_styles) {
        g_hash_table_destroy(b->layout_styles);
        b->layout_styles = NULL;
    }
    gboolean was_delta = delta != NULL;
    if (delta) {
        for (guint i = 0; i < delta->len; i++) {
            ns_css_style_change *c = g_ptr_array_index(delta, i);
            c->style->ref++;
            g_hash_table_replace(b->styles, (gpointer)c->node, c->style);
        }
        g_ptr_array_unref(delta);
    } else {
        g_hash_table_destroy(b->styles);
        b->styles = styles;
    }
    b->styles_serial = ns_css_incremental_serial();
    b->styles_serial_valid = TRUE;
    if (b->anim)
        ns_anim_observe_nodes(b->anim, b->styles, changed_nodes,
                              g_get_monotonic_time());
    g_ptr_array_free(changed_nodes, TRUE);
    if (b->js) ns_js_set_style_table(b->js, b->styles);
    b->cascade_dirty = FALSE;
    if (!rects_ok) {
        b->damage_full = TRUE;
    } else if (!b->damage_full) {
        if (!b->damage_rects)
            b->damage_rects = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
        g_array_append_vals(b->damage_rects, rects->data, rects->len);
        b->repaint_pending = TRUE;
    }
    g_array_free(rects, TRUE);
    if (g_getenv("NS_PROFILE"))
        g_printerr("[profile] restyle kept layout styles=%u %.2fms "
                   "cascade=%.2f%s\n", changed,
                   (g_get_monotonic_time() - t0) / 1000.0, (t1 - t0) / 1000.0,
                   was_delta ? " (changes only)" : "");
    for (guint i = 0; structural && i < targets->len; i++) {
        const ns_node *svg = node_svg_painter(g_ptr_array_index(targets, i));
        if (svg) browser_damage_node(b, svg);
    }
    browser_verify_layout(b, "restyle");
    return TRUE;
}

/* Applies pending script mutations by restyling or relayouting as needed. */
static void
browser_flush(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !b->js) return;
    browser_absorb_js_mutation(b);
    if (b->layout && !b->dirty && b->restyle_pending && browser_restyle(b))
        return;
    if (!b->layout || b->dirty || b->restyle_pending) {
        browser_relayout(b);
        b->dirty = FALSE;
    }
}

/* Brings computed styles up to date for script queries without forcing relayout. */
static void
browser_flush_style(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !b->js) return;
    if (browser_absorb_js_mutation(b) == BROWSER_JS_RELAYOUT)
        b->cascade_dirty = TRUE;
    if (b->restyle_pending) {
        if (b->layout && !b->dirty && !b->relaying && browser_restyle(b))
            return;
        b->restyle_pending = FALSE;
        b->dirty = TRUE;
        b->cascade_dirty = TRUE;
    }
    if (b->styles && !b->cascade_dirty) return;
    if (!b->layout || b->relaying) {
        browser_flush(user_data);
        return;
    }
    if (b->styles) {
        ns_js_set_style_table(b->js, NULL);
        if (!b->layout_styles)
            b->layout_styles = b->styles;
        else
            g_hash_table_destroy(b->styles);
        b->styles = NULL;
    }
    ns_css_set_viewport((double)b->vw, b->vh);
    ns_css_set_doc_language(b->doc_language);
    b->styles = ns_engine_compute_cascade(b->doc, b->base_url, b->css_cache, b->anim);
    b->damage_full = TRUE;
    if (b->anim)
        ns_engine_anim_observe(b->anim, b->styles, g_get_monotonic_time());
    ns_js_set_style_table(b->js, b->styles);
    b->cascade_dirty = FALSE;
}

/* Returns the percent-decoded fragment of a URL and whether one was present. */
static char *
browser_url_fragment(const char *url, gboolean *has_fragment)
{
    const char *hash = url ? strchr(url, '#') : NULL;
    if (has_fragment) *has_fragment = hash != NULL;
    if (!hash) return NULL;
    char *decoded = g_uri_unescape_string(hash + 1, NULL);
    return decoded ? decoded : g_strdup(hash + 1);
}

/* Opens hidden=until-found ancestors and <details> to reveal a fragment target. */
static gboolean
browser_reveal_fragment_target(ns_browser *browser, ns_node *target)
{
    gboolean changed = FALSE;
    for (ns_node *cur = target; cur; cur = cur->parent) {
        if (ns_element_hidden_until_found(cur)) {
            ns_element_remove_attr(cur, "hidden");
            changed = TRUE;
        }
        if (cur->parent && ns_details_fragment_needs_open(cur->parent, cur) &&
            !ns_element_get_attr(cur->parent, "open")) {
            ns_element_set_attr(cur->parent, "open", "");
            changed = TRUE;
        }
        if (cur == browser->doc) break;
    }
    return changed;
}

/* Finds the box generated by a node, including inline atomic boxes. */
static const ns_box *
browser_box_for_node(const ns_box *box, const ns_node *target)
{
    if (!box || !target) return NULL;
    if (box->dom == target) return box;
    for (const ns_box *child = box->first_child; child;
         child = child->next_sibling) {
        const ns_box *found = browser_box_for_node(child, target);
        if (found) return found;
    }
    if (box->inline_atomics)
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic = &g_array_index(
                box->inline_atomics, ns_inline_atomic, i);
            const ns_box *found = browser_box_for_node(atomic->box, target);
            if (found) return found;
        }
    return NULL;
}

/* Document y offset of a node's box or inline rect; FALSE if not laid out. */
static gboolean
browser_target_scroll_y(ns_browser *browser, const ns_node *target, int *out_y)
{
    if (!browser || !browser->layout || !target) return FALSE;
    const ns_box *box = browser_box_for_node(browser->layout, target);
    double y = 0;
    if (box) {
        y = box->y + box->margin.top;
    } else {
        double x = 0, w = 0, h = 0;
        if (!ns_box_inline_rect_for_dom(browser->layout, target,
                                        &x, &y, &w, &h))
            return FALSE;
    }
    *out_y = (int)floor(MAX(0.0, y));
    return TRUE;
}

/* Queues a scroll to a node, optionally revealing it and anchoring to it. */
static void
browser_queue_scroll_to(ns_browser *browser, const ns_node *target,
                        gboolean reveal)
{
    if (!browser || !target) return;
    if (reveal && browser_reveal_fragment_target(browser, (ns_node *)target))
        browser->dirty = TRUE;
    browser_flush(browser);
    int y = 0;
    if (!browser_target_scroll_y(browser, target, &y)) return;
    browser->pending_scroll_y = y;
    browser->pending_scroll = TRUE;
    if (reveal) {
        browser->scroll_anchor = target;
        browser->scroll_anchor_y = y;
    }
}

/* Keeps the scroll pinned to a fragment target while layout shifts it. */
static void
browser_follow_scroll_anchor(ns_browser *browser)
{
    if (!browser || !browser->scroll_anchor) return;
    if (fabs(browser->cur_scroll_y - (double)browser->scroll_anchor_y) > 1.0 &&
        !browser->pending_scroll) {
        browser->scroll_anchor = NULL;
        return;
    }
    int y = 0;
    if (browser_target_scroll_y(browser, browser->scroll_anchor, &y) &&
        y != browser->scroll_anchor_y) {
        browser->scroll_anchor_y = y;
        browser->pending_scroll_y = y;
        browser->pending_scroll = TRUE;
    }
    if (!ns_browser_animating(browser) && !browser->pending_scroll)
        browser->scroll_anchor = NULL;
}

/* Script scrollIntoView hook: queues a scroll to the node without anchoring. */
static void
browser_js_scroll_to(const ns_node *target, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (browser) browser->scroll_anchor = NULL;
    browser_queue_scroll_to(browser, target, FALSE);
}

/* Script hook for pushState/replaceState: updates the URL if same-origin. */
static void
browser_js_soft_navigate(const char *url, gboolean replace, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (!browser || !url) return;
    /* history.pushState/replaceState and same-document navigations may
     * only change the URL within the current origin; base_url also feeds
     * the navigation privilege checks. */
    if (browser->base_url && g_strcmp0(browser->base_url, url) != 0 &&
        !ns_url_same_origin(browser->base_url, url) &&
        !(g_str_has_prefix(browser->base_url, "file:") &&
          g_str_has_prefix(url, "file:")))
        return;
    gboolean has_fragment = FALSE;
    g_autofree char *fragment = browser_url_fragment(url, &has_fragment);
    ns_css_set_target_fragment(has_fragment && fragment && *fragment
                                   ? fragment : NULL);
    if (!replace) browser->url_pushed = TRUE;
    g_free(browser->base_url);
    browser->base_url = g_strdup(url);
    browser->dirty = TRUE;
}

/* Script hook for fragment navigation: sets :target and scrolls to it. */
static void
browser_js_fragment_navigate(const char *url, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (!browser || !url) return;
    gboolean has_fragment = FALSE;
    g_autofree char *fragment = browser_url_fragment(url, &has_fragment);
    if (!has_fragment) return;
    ns_css_set_target_fragment(fragment && *fragment ? fragment : NULL);
    browser->dirty = TRUE;
    browser->scroll_anchor = NULL;
    if (!fragment || !*fragment) {
        browser->pending_scroll_y = 0;
        browser->pending_scroll = TRUE;
        return;
    }
    ns_node *target = ns_node_find_fragment_target(browser->doc, fragment);
    if (target) browser_queue_scroll_to(browser, target, TRUE);
}

static gboolean
settle_quit_cb(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_CONTINUE;
}

typedef struct settle_ctx {
    ns_browser *b;
    GMainLoop  *loop;
    int         quiet_ticks;
} settle_ctx;

#define NS_SETTLE_QUIET_TICKS 3

/* TRUE when no layout, script, animation-frame, image or loop work is pending. */
static gboolean
browser_settle_quiet(ns_browser *b)
{
    if (b->dirty || b->restyle_pending) return FALSE;
    if (b->js && ns_js_has_pending_work(b->js)) return FALSE;
    if (b->js && ns_js_has_pending_animation_frame(b->js)) return FALSE;
    if (b->images && ns_image_cache_has_pending(b->images)) return FALSE;
    if (g_main_context_pending(ns_engine_context())) return FALSE;
    return TRUE;
}

/* Settle loop tick: runs animations and frames, applies mutations, quits when quiet. */
static gboolean
settle_tick_cb(gpointer user_data)
{
    settle_ctx *ctx = user_data;
    ns_browser *b = ctx->b;
    gint64 now = g_get_monotonic_time();
    if (b->images) ns_image_cache_tick(b->images, now);
    if (b->anim && ns_anim_tick(b->anim, now)) {
        b->cascade_dirty = TRUE;
        if (ns_anim_needs_layout(b->anim)) b->dirty = TRUE;
    }
    if (b->anim && ns_anim_take_restyle(b->anim)) b->dirty = TRUE;
    if (b->anim && b->js) ns_js_dispatch_anim_events(b->js, b->anim);
    if (b->js) ns_js_run_animation_frame(b->js);
    if (!b->dirty && b->restyle_pending && b->layout && !browser_restyle(b))
        b->dirty = TRUE;
    if (b->dirty || (b->js && ns_js_consume_mutated(b->js))) {
        if (browser_relayout_from_mutation(b))
            b->dirty = FALSE;
    }
    if (browser_settle_quiet(b)) {
        if (++ctx->quiet_ticks >= NS_SETTLE_QUIET_TICKS) {
            g_main_loop_quit(ctx->loop);
            return G_SOURCE_CONTINUE;
        }
    } else {
        ctx->quiet_ticks = 0;
    }
    return G_SOURCE_CONTINUE;
}

/* Runs the engine loop up to settle_ms until the page goes quiet. */
static void
browser_settle(ns_browser *b, int settle_ms)
{
    if (settle_ms <= 0) return;
    if (browser_settle_quiet(b)) return;
    GMainLoop *loop = g_main_loop_new(ns_engine_context(), FALSE);
    settle_ctx ctx = { .b = b, .loop = loop };
    guint quit = ns_engine_timeout_add(settle_ms, settle_quit_cb, loop);
    guint tick = ns_engine_timeout_add(16, settle_tick_cb, &ctx);
    g_main_loop_run(loop);
    ns_engine_source_remove(tick);
    ns_engine_source_remove(quit);
    g_main_loop_unref(loop);
}

/* Console hook: logs a line and appends it to a capped console buffer. */
static void
browser_js_log(const char *line, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !line) return;
    ns_debug_log_emit(NS_DLOG_JS, "console", "%s", line);
    if (!b->console_buf) b->console_buf = g_string_new(NULL);
    if (b->console_buf->len > 256u * 1024u)
        g_string_erase(b->console_buf, 0,
                       (gssize)(b->console_buf->len - 192u * 1024u));
    g_string_append(b->console_buf, line);
    g_string_append_c(b->console_buf, '\n');
}
/* Script window.scrollTo hook: clamps to page extents and queues the scroll. */
static void
browser_js_viewport_scroll(double *x, double *y, gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b) return;
    browser_flush(b);
    int page_w = 0, page_h = 0;
    ns_browser_page_size(b, &page_w, &page_h);
    double view_h = b->cur_viewport_h > 0.0 ? b->cur_viewport_h : b->vh;
    double max_x = MAX((double)page_w - (double)b->vw, 0.0);
    double max_y = MAX((double)page_h - view_h, 0.0);
    *x = round(CLAMP(*x, 0.0, max_x));
    *y = round(CLAMP(*y, 0.0, max_y));
    b->pending_scroll_x = (int)*x;
    b->pending_scroll_y = (int)*y;
    b->pending_scroll = TRUE;
    b->js_scroll_x = *x;
    b->js_scroll_y = *y;
    b->scroll_anchor = NULL;
}

/* Script hook requesting a full repaint. */
static void
browser_js_repaint(gpointer user_data)
{
    ns_browser *browser = user_data;
    if (!browser) return;
    browser->repaint_pending = TRUE;
    browser->damage_full = TRUE;
}

/* TRUE if the node's root is the browser's document. */
static gboolean
browser_node_in_document(const ns_browser *browser, const ns_node *node)
{
    const ns_node *n = node;
    while (n->parent) n = n->parent;
    return n == browser->doc;
}

/* Marks a node for partial repaint, unless a full repaint is already pending. */
static void
browser_damage_node(ns_browser *browser, const ns_node *node)
{
    if (!node || !browser_node_in_document(browser, node)) return;
    browser->repaint_pending = TRUE;
    if (browser->damage_full) return;
    if (!browser->damage_nodes)
        browser->damage_nodes = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_add(browser->damage_nodes, (gpointer)node);
}

static void
browser_js_repaint_node(const ns_node *node, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (browser) browser_damage_node(browser, node);
}

/* Script DOM mutation hook: schedules relayout or restyle and cascade refresh. */
static void
browser_js_mutated(gpointer user_data)
{
    ns_browser *browser = user_data;
    if (!browser) return;
    ns_render_offer_styles(NULL, 0, NULL, NULL);
    const char *why = browser->js ? ns_js_layout_mutated_by(browser->js) : NULL;
    if (!browser->js || !browser->layout ||
        ns_js_consume_layout_mutated(browser->js)) {
        if (why && !browser->relayout_why) browser->relayout_why = why;
        browser->dirty = TRUE;
    } else {
        browser->restyle_pending = TRUE;
    }
    browser->cascade_dirty = TRUE;
}

/* Policy check for page-initiated navigation to drive paths, file:, about: and view-source:. */
static gboolean
browser_allows_navigation_url(ns_browser *b, const char *url)
{
    if (!url) return TRUE;
    /* A drive path ("c:/Users/...", or drive-relative "c:x/..."), which
     * browser_open_common would turn into a file: URL; there is no
     * one-letter URL scheme, and only the user may open local paths. */
    if (g_ascii_isalpha(url[0]) && url[1] == ':')
        return b && b->base_url && g_str_has_prefix(b->base_url, "file:");
    /* The "continue anyway" link is only meaningful on the Safe Browsing
     * warning page; from anywhere else it would be a way to navigate with
     * no initiator (and so with chrome privileges). */
    if (g_ascii_strncasecmp(url, NS_UNSAFE_CONTINUE_SCHEME,
                            strlen(NS_UNSAFE_CONTINUE_SCHEME)) == 0)
        return b && b->safebrowsing_interstitial;
    /* The start page links to about:history, as the browser pages do. */
    if (g_str_has_prefix(url, "about:"))
        return ns_about_url_is_public(url) ||
               (b && (ns_about_url_is_chrome(b->base_url) ||
                      ns_about_url_is_start(b->base_url)));
    /* view-source: can wrap file: and chrome about: URLs. */
    if (g_ascii_strncasecmp(url, "view-source:", 12) == 0)
        return b && (ns_about_url_is_chrome(b->base_url) ||
                     g_str_has_prefix(b->base_url, "view-source:"));
    if (!g_str_has_prefix(url, "file:")) return TRUE;
    return b && b->base_url && g_str_has_prefix(b->base_url, "file:");
}

/* Resolves an href against the page and applies navigation policy; NULL if refused. */
static char *
browser_resolve_navigation(ns_browser *b, const char *href)
{
    if (!b || !href) return NULL;
    char *abs = ns_url_resolve(b->base_url, href);
    if (!browser_allows_navigation_url(b, abs)) {
        g_free(abs);
        return NULL;
    }
    return abs;
}

/* Script navigation hook: records the resolved, policy-checked URL as pending. */
static void browser_js_navigate(const char *url, gboolean reload, gpointer ud)
{
    (void)reload;
    ns_browser *b = ud;
    if (!b || !url || !*url) return;
    g_free(b->pending_nav);
    b->pending_nav = browser_resolve_navigation(b, url);
    g_clear_pointer(&b->pending_nav_initiator, g_free);
}

/* Script download hook: queues "url\tfilename" for the shell if navigation is allowed. */
static void browser_js_download(const char *url, const char *filename, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !url || !*url) return;
    /* A URL the page may not navigate to (file:, chrome about: pages) must
     * not be fetched as a download either. */
    char *abs = browser_resolve_navigation(b, url);
    if (!abs) return;
    g_free(b->pending_download);
    b->pending_download = g_strdup_printf("%s\t%s", abs,
                                          filename ? filename : "");
    g_free(abs);
}

#define NS_PENDING_CLIPBOARD_MAX 1048576

/* Script clipboard hook: stages text (up to 1 MiB) for the shell to copy. */
static gboolean
browser_js_clipboard_write(const char *text, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !text) return FALSE;
    if (strlen(text) > NS_PENDING_CLIPBOARD_MAX) return FALSE;
    g_free(b->pending_clipboard);
    b->pending_clipboard = g_strdup(text);
    return TRUE;
}

static void browser_sync_js_selection(ns_browser *browser);

/* Script execCommand hook for selectAll and unselect. */
static gboolean
browser_js_selection_cmd(const char *command, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !command || !b->layout) return FALSE;
    gboolean ok = FALSE;
    if (strcmp(command, "selectAll") == 0)
        ok = ns_selection_select_all(&b->selection, b->layout);
    else if (strcmp(command, "unselect") == 0) {
        ns_selection_clear(&b->selection);
        ok = TRUE;
    }
    if (ok) {
        browser_sync_js_selection(b);
        b->dirty = TRUE;
    }
    return ok;
}

/* Initializes engine subsystems: config, networking, caches, history, fonts, spelling. */
int
ns_browser_init(void)
{
    ns_config_init();
    if (ns_config_get()->harden_allocator)
        ns_security_harden_allocator();
    ns_net_init();
    ns_net_set_allow_file_urls(TRUE);
    ns_cache_init();
    ns_bytecode_cache_init();
    ns_history_init();
    ns_font_init();
    ns_spell_init();
    return 0;
}

/* Converts an existing local path argument into a file:// URL; NULL otherwise. */
static char *
resolve_local_path(const char *url)
{
    if (!url || strstr(url, "://") ||
        g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:") ||
        !g_file_test(url, G_FILE_TEST_EXISTS))
        return NULL;
    char *abs = g_canonicalize_filename(url, NULL);
    char *file_url = g_filename_to_uri(abs, NULL, NULL);
    g_free(abs);
    return file_url;
}

/* Finds the content of the first <meta http-equiv=refresh> in the document. */
static const char *
browser_find_meta_refresh(const ns_node *n, int depth)
{
    if (!n || depth > 1024) return NULL;
    if (ns_node_is_element_named(n, "meta")) {
        const char *equiv = ns_element_get_attr(n, "http-equiv");
        if (equiv && g_ascii_strcasecmp(equiv, "refresh") == 0) {
            const char *content = ns_element_get_attr(n, "content");
            if (content && *content) return content;
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const char *found = browser_find_meta_refresh(c, depth + 1);
        if (found) return found;
    }
    return NULL;
}

/* Schedules a Refresh header or meta refresh navigation, policy-checked. */
static void
browser_arm_declarative_refresh(ns_browser *b, const char *header_value)
{
    double seconds = 0.0;
    char *target = NULL;
    gboolean armed = header_value &&
        ns_net_parse_refresh(header_value, &seconds, &target);
    if (!armed) {
        const char *meta = browser_find_meta_refresh(b->doc, 0);
        armed = meta && ns_net_parse_refresh(meta, &seconds, &target);
    }
    if (!armed) return;
    if (target) {
        b->refresh_url = browser_resolve_navigation(b, target);
        g_free(target);
        if (!b->refresh_url) return;
    } else {
        b->refresh_url = g_strdup(b->base_url);
    }
    b->refresh_due_us = g_get_monotonic_time() + (gint64)(seconds * 1e6);
}

static gboolean
browser_content_type_starts(const char *content_type, const char *prefix)
{
    return content_type && prefix &&
        g_ascii_strncasecmp(content_type, prefix, strlen(prefix)) == 0;
}

static gboolean
browser_content_type_is_html(const char *content_type)
{
    return browser_content_type_starts(content_type, "text/html");
}

/* TRUE for JSON media types, including +json suffixes. */
static gboolean
browser_content_type_is_json(const char *content_type)
{
    return browser_content_type_starts(content_type, "application/json") ||
           browser_content_type_starts(content_type, "text/json") ||
           (content_type && strstr(content_type, "+json") != NULL);
}

/* Wraps plain text in an HTML <pre> document titled with the URL; caller frees. */
static char *
browser_text_document(const char *url, const char *text)
{
    char *esc_url = ns_html_escape_text(url && *url ? url : "text file");
    char *esc_text = ns_html_escape_text(text ? text : "");
    char *html = g_strconcat(
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>",
        esc_url,
        "</title><style>"
        "body{margin:0;background:#fff;color:#111}"
        "pre{margin:0;padding:12px;font:13px/1.45 ui-monospace,"
        "\"SF Mono\",Menlo,Consolas,monospace;white-space:pre-wrap;"
        "overflow-wrap:anywhere}"
        "</style></head><body><pre>",
        esc_text,
        "</pre></body></html>",
        NULL);
    g_free(esc_url);
    g_free(esc_text);
    return html;
}

/* Rewrites image, JSON and non-HTML text responses into HTML viewer documents. */
static void
browser_prepare_document_response(ns_response *resp)
{
    if (!resp || !resp->body || !resp->content_type)
        return;
    const char *final_url = resp->final_url ? resp->final_url : "";
    char *html = NULL;
    if (browser_content_type_starts(resp->content_type, "image/") &&
        !ns_html_mime_is_xml(resp->content_type)) {
        html = ns_html_image_document(final_url);
    } else if (browser_content_type_is_json(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = ns_html_json_document(final_url, decoded,
                                     decoded ? strlen(decoded) : 0);
        if (!html) html = browser_text_document(final_url, decoded);
        g_free(decoded);
    } else if (browser_content_type_starts(resp->content_type, "text/") &&
               !browser_content_type_is_html(resp->content_type) &&
               !ns_html_mime_is_xml(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = browser_text_document(final_url, decoded);
        g_free(decoded);
    }
    if (!html)
        return;
    g_byte_array_set_size(resp->body, 0);
    g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
    g_free(html);
    g_free(resp->content_type);
    resp->content_type = g_strdup("text/html; charset=utf-8");
}

/* TRUE if raw headers mention no-store (case-insensitive). */
static gboolean
headers_have_no_store(const char *raw)
{
    if (!raw) return FALSE;
    char *low = g_ascii_strdown(raw, -1);
    gboolean found = strstr(low, "no-store") != NULL;
    g_free(low);
    return found;
}

static void browser_js_form_submit(const ns_node *form, const ns_node *submitter,
                                   gpointer user_data);

/* Creates a browser for a parsed document: cascade, JS hooks, scripts, layout, settle. */
static ns_browser *
browser_build_from_doc(ns_node *doc, char *base, int viewport_width,
                       double viewport_height, int settle_ms,
                       gboolean bfcache_ok, char *refresh_hdr,
                       char *doc_language, char *csp_header, char *doc_charset,
                       const char *url, const char *content_type,
                       const ns_js_navigation_timing *navigation_timing)
{
    int vw = viewport_width > 0 ? viewport_width : 1000;
    double vh = viewport_height > 0.0
        ? viewport_height
        : (double)vw * 0.75;
    ns_css_set_viewport((double)vw, vh);
    gboolean has_fragment = FALSE;
    g_autofree char *fragment = browser_url_fragment(url, &has_fragment);
    ns_css_set_target_fragment(has_fragment && fragment && *fragment
                                   ? fragment : NULL);

    if (ns_config_get()->speculative_preload)
        ns_engine_speculative_preload(doc, base, csp_header, FALSE);

    ns_browser *b = g_new0(ns_browser, 1);
    b->pending_scroll_x = -1;
    b->doc = doc;
    b->doc_charset = doc_charset;
    b->doc_language = doc_language;
    b->base_url = base;
    ns_css_set_doc_language(b->doc_language);
    b->vw = vw;
    b->vh = vh;
    b->bfcache_ok = bfcache_ok;
    b->css_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)g_bytes_unref);
    b->images = ns_image_cache_new();
    b->styles = ns_engine_compute_cascade(doc, base, b->css_cache, NULL);

    b->anim = ns_anim_new();
    ns_engine_load_keyframes(b->anim, doc, base, b->css_cache);
    ns_engine_anim_observe(b->anim, b->styles, g_get_monotonic_time());

    b->js = ns_js_new(browser_js_log, b,
                      browser_js_mutated, b,
                      browser_js_navigate, b,
                      navigation_timing);
    if (b->js) {
        b->load_delay_deadline_us =
            g_get_monotonic_time() + (gint64)10 * G_USEC_PER_SEC;
        ns_js_set_load_delay_cb(b->js, browser_load_waits_for_images, b);
        ns_js_set_style_table(b->js, b->styles);
        ns_js_set_image_cache(b->js, b->images);
        ns_js_set_anim(b->js, b->anim);
        ns_js_set_form_submit_cb(b->js, browser_js_form_submit, b);
        ns_js_set_layout_flush_cb(b->js, browser_flush, b);
        ns_js_set_repaint_cb(b->js, browser_js_repaint, b);
        ns_js_set_repaint_node_cb(b->js, browser_js_repaint_node);
        ns_js_set_viewport_scroll_cb(b->js, browser_js_viewport_scroll, b);
        ns_js_set_style_flush_cb(b->js, browser_flush_style, b);
        ns_js_set_scroll_to_cb(b->js, browser_js_scroll_to, b);
        ns_js_set_fragment_nav_cb(b->js, browser_js_fragment_navigate, b);
        ns_js_set_soft_nav_cb(b->js, browser_js_soft_navigate, b);
        ns_js_set_download_cb(b->js, browser_js_download, b);
        ns_js_set_clipboard_write_cb(b->js, browser_js_clipboard_write, b);
        ns_js_set_selection_cmd_cb(b->js, browser_js_selection_cmd, b);
        ns_js_add_csp_header(b->js, csp_header);
        ns_js_apply_meta_csp(b->js, doc);
        const ns_config *run_cfg = ns_config_get();
        if (!run_cfg || run_cfg->javascript_enabled)
            ns_js_run_scripts_in_doc(b->js, doc, base, b->doc_charset,
                                     content_type);
        ns_media_scan(b->js, doc, base);
    }
    g_free(csp_header);

    browser_arm_declarative_refresh(b, refresh_hdr);
    g_free(refresh_hdr);

    if (!b->layout || b->dirty)
        browser_relayout(b);
    browser_settle(b, settle_ms);
    if (!b->layout || b->dirty)
        browser_relayout(b);
    ns_node *fragment_target = fragment && *fragment
        ? ns_node_find_fragment_target(b->doc, fragment) : NULL;
    if (fragment_target) browser_queue_scroll_to(b, fragment_target, TRUE);
    else if (has_fragment && (!fragment || !*fragment)) {
        b->pending_scroll_y = 0;
        b->pending_scroll = TRUE;
    }
    return b;
}

static char *g_pending_referrer;
static gboolean g_pending_user_activated;

/* Sets the referrer and user-activation flag consumed by the next open. */
void
ns_browser_set_next_navigation(const char *referrer, int user_activated)
{
    g_free(g_pending_referrer);
    g_pending_referrer = (referrer && *referrer) ? g_strdup(referrer) : NULL;
    g_pending_user_activated = user_activated != 0;
}

/* Builds the Safe Browsing warning page shown in place of a blocked URL. */
static ns_browser *
browser_open_blocked(const char *url, const char *host, int viewport_width,
                     double viewport_height, int settle_ms)
{
    char *html = ns_safebrowsing_interstitial(url, host);
    ns_node *doc = ns_html_parse(html, html ? (gssize)strlen(html) : 0);
    g_free(html);
    ns_browser *warning = browser_build_from_doc(doc, g_strdup(url),
        viewport_width, viewport_height, settle_ms, FALSE,
        NULL, NULL, NULL, g_strdup("UTF-8"), url, "text/html", NULL);
    if (warning) warning->safebrowsing_interstitial = TRUE;
    return warning;
}

/* Loads a URL (Safe Browsing, HTTPS-First, error pages) and builds the page browser. */
static ns_browser *
browser_open_common(const char *url, int viewport_width, double viewport_height,
                    int settle_ms,
                    const void *body, size_t body_len, const char *content_type)
{
    if (!url || !*url) return NULL;

    g_autofree char *referrer = g_pending_referrer;
    g_pending_referrer = NULL;
    gboolean user_activated = g_pending_user_activated;
    g_pending_user_activated = FALSE;

    if (g_str_has_prefix(url, NS_UNSAFE_CONTINUE_SCHEME)) {
        const char *rest = url + strlen(NS_UNSAFE_CONTINUE_SCHEME);
        /* Only ever continue to the web page that was blocked. */
        if (body || !ns_url_is_http_or_https(rest)) return NULL;
        char *real = g_strdup(rest);
        char *host = ns_url_host_from(real);
        if (host) {
            ns_safebrowsing_allow_host(host);
            g_free(host);
        }
        ns_browser *b = browser_open_common(real, viewport_width,
                                            viewport_height, settle_ms,
                                            body, body_len, content_type);
        g_free(real);
        return b;
    }

    /* A form POST is checked too: it would send its data to the host. */
    {
        char *host = ns_url_host_from(url);
        if (host && ns_safebrowsing_blocked(host)) {
            ns_browser *warning = browser_open_blocked(url, host,
                viewport_width, viewport_height, settle_ms);
            g_free(host);
            return warning;
        }
        g_free(host);
    }

    char *file_url = resolve_local_path(url);
    const char *fetch_url = file_url ? file_url : url;

    char *stripped_url = body ? NULL : ns_url_strip_tracking_params(fetch_url);
    if (stripped_url)
        fetch_url = stripped_url;

    char *https_url = body ? NULL : ns_net_https_first_upgrade(fetch_url);

    GError *err = NULL;
    ns_response *resp = NULL;
    ns_net_set_navigation_fetch(TRUE, user_activated);
    if (https_url) {
        resp = ns_engine_fetch_blocking(https_url, referrer, &err);
        if (resp && !resp->error && resp->body) {
            fetch_url = https_url;
        } else {
            if (resp) ns_response_free(resp);
            resp = NULL;
            g_clear_error(&err);
        }
    }
    if (!resp) {
        ns_net_set_navigation_fetch(TRUE, user_activated);
        resp = body
            ? ns_engine_post_blocking(fetch_url, referrer, body, body_len,
                                      content_type, &err)
            : ns_engine_fetch_blocking(fetch_url, referrer, &err);
    }
    ns_net_set_navigation_fetch(FALSE, FALSE);
    /* ns_fetch_sync stops at a redirect hop to a blocked host and reports
     * that hop as the final URL, so a clean link cannot launder one. */
    char *landed_host = resp && resp->final_url
        ? ns_url_host_from(resp->final_url) : NULL;
    if (landed_host && ns_safebrowsing_blocked(landed_host)) {
        char *landed = g_strdup(resp->final_url);
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(file_url);
        g_free(stripped_url);
        g_free(https_url);
        ns_browser *warning = browser_open_blocked(landed, landed_host,
            viewport_width, viewport_height, settle_ms);
        g_free(landed);
        g_free(landed_host);
        return warning;
    }
    g_free(landed_host);
    if (resp && !body && (resp->error || resp->status >= 400) &&
        (!resp->body || resp->body->len == 0)) {
        gboolean tls_failure = resp->error &&
            g_str_has_prefix(fetch_url, "https://");
        char *html = ns_build_error_page(fetch_url,
                                         resp->error ? 0 : resp->status,
                                         resp->error);
        if (html) {
            if (!resp->body)
                resp->body = g_byte_array_new();
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->error);
            resp->error = NULL;
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
            g_free(resp->final_url);
            /* A page that was refused a local or browser URL must not get
             * an error document at that address: its retry link, a
             * reload or history would then open it with no initiator. */
            gboolean privileged = g_ascii_strncasecmp(fetch_url, "file:", 5) == 0 ||
                g_ascii_strncasecmp(fetch_url, "view-source:", 12) == 0 ||
                (g_ascii_strncasecmp(fetch_url, "about:", 6) == 0 &&
                 !ns_about_url_is_public(fetch_url));
            resp->final_url = g_strdup(referrer && *referrer && privileged
                                       ? "about:blank" : fetch_url);
            if (tls_failure)
                resp->security = NS_SEC_INVALID;
        }
    }
    if (!resp || resp->error || !resp->body) {
        if (resp) ns_response_free(resp);
        g_clear_error(&err);
        g_free(file_url);
        g_free(stripped_url);
        g_free(https_url);
        return NULL;
    }
    g_clear_error(&err);

    char *base = g_strdup(resp->final_url ? resp->final_url : fetch_url);
    if (!body) {
        char *base_stripped = ns_url_strip_tracking_params(base);
        if (base_stripped) {
            g_free(base);
            base = base_stripped;
        }
    }
    gboolean bfcache_ok = !body &&
        (g_str_has_prefix(base, "http://") ||
         g_str_has_prefix(base, "https://")) &&
        resp->status >= 200 && resp->status < 400 &&
        !headers_have_no_store(resp->raw_headers);
    char *refresh_hdr = g_strdup(resp->refresh);
    char *doc_language = g_strdup(resp->content_language);
    char *csp_header = g_strdup(resp->csp_header);
    g_free(file_url);
    g_free(stripped_url);
    g_free(https_url);
    browser_prepare_document_response(resp);

    char *doc_charset = NULL;
    char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                             resp->body->len,
                                             resp->content_type,
                                             &doc_charset);
    char *doc_content_type = ns_html_mime_essence(resp->content_type);
    const ns_config *parse_cfg = ns_config_get();
    gboolean scripting_on = !parse_cfg || parse_cfg->javascript_enabled;
    ns_node *doc = !scripting_on && !ns_html_mime_is_xml(doc_content_type)
        ? ns_html_parse_with_scripting(
              decoded ? decoded : "",
              decoded ? (gssize)strlen(decoded) : 0, FALSE)
        : ns_html_parse_document(
              decoded ? decoded : "",
              decoded ? (gssize)strlen(decoded) : 0, doc_content_type);
    g_free(decoded);
    int sec = resp->security;
    if (sec == NS_SEC_NONE) {
        const char *u = resp->final_url ? resp->final_url : fetch_url;
        if (g_str_has_prefix(u, "https://"))
            sec = NS_SEC_SECURE;
        else if (g_str_has_prefix(u, "http://"))
            sec = NS_SEC_PLAIN;
    }
    char *ip = g_strdup(resp->remote_ip);
    ns_js_navigation_timing navigation_timing =
        ns_js_navigation_timing_from_response(resp);
    ns_response_free(resp);

    ns_browser *b = browser_build_from_doc(doc, base, viewport_width,
                                           viewport_height, settle_ms,
                                           bfcache_ok, refresh_hdr, doc_language,
                                           csp_header, doc_charset, url,
                                           doc_content_type,
                                           &navigation_timing);
    g_free(doc_content_type);
    if (b) {
        b->security = sec;
        b->remote_ip = ip;
    } else {
        g_free(ip);
    }
    return b;
}

ns_browser *
ns_browser_open_viewport(const char *url, int viewport_width,
                         double viewport_height, int settle_ms)
{
    return browser_open_common(url, viewport_width, viewport_height, settle_ms,
                               NULL, 0, NULL);
}

ns_browser *
ns_browser_open_post_viewport(const char *url, int viewport_width,
                              double viewport_height, int settle_ms,
                              const void *body, size_t body_len,
                              const char *content_type)
{
    return browser_open_common(url, viewport_width, viewport_height, settle_ms,
                               body, body_len, content_type);
}

/* Returns the page's rendered text; malloc'd, caller frees with free(). */
char *
ns_browser_render_text(ns_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    ns_engine_dump_text(browser->layout, out);
    char *text = malloc(out->len + 1);
    if (text) {
        memcpy(text, out->str, out->len);
        text[out->len] = '\0';
    }
    g_string_free(out, TRUE);
    return text;
}

/* Serializes the DOM tree for debugging; caller frees. */
char *
ns_browser_dump_dom(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    GString *out = ns_node_dump(browser->doc);
    return out ? g_string_free(out, FALSE) : NULL;
}

/* Serializes the box tree for debugging; caller frees. */
char *
ns_browser_dump_layout(ns_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    ns_engine_dump_layout(browser->layout, 0, out);
    return g_string_free(out, FALSE);
}

/* Counts DOM nodes in a sibling forest, depth-limited to 1024. */
static int
node_count_walk(const ns_node *n, int depth)
{
    if (depth > 1024) return 0;
    int c = 0;
    for (; n; n = n->next_sibling)
        c += 1 + node_count_walk(n->first_child, depth + 1);
    return c;
}

/* Counts boxes in a sibling forest. */
static int
box_count_walk(const ns_box *b)
{
    int c = 0;
    for (; b; b = b->next_sibling)
        c += 1 + box_count_walk(b->first_child);
    return c;
}

/* Appends process thread names (Linux /proc only) to a performance report. */
static void
perf_dump_threads(GString *out)
{
    g_string_append(out, "Threads\n");
#if defined(__linux__)
    GError *err = NULL;
    GDir *dir = g_dir_open("/proc/self/task", 0, &err);
    if (!dir) {
        g_string_append_printf(out, "  (unavailable: %s)\n",
                               err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    const char *tid;
    int n = 0;
    while ((tid = g_dir_read_name(dir))) {
        char *path = g_strdup_printf("/proc/self/task/%s/comm", tid);
        char *comm = NULL;
        if (g_file_get_contents(path, &comm, NULL, NULL) && comm) {
            g_string_append_printf(out, "  [%s] %s", tid, g_strchomp(comm));
            g_string_append_c(out, '\n');
            n++;
        }
        g_free(comm);
        g_free(path);
    }
    g_dir_close(dir);
    g_string_append_printf(out, "  %d thread%s total\n", n, n == 1 ? "" : "s");
#else
    g_string_append(out, "  (thread enumeration is Linux-only)\n");
#endif
}

/* Builds a text performance report: document, layout, JS stats and threads. */
char *
ns_browser_dump_performance(ns_browser *browser)
{
    if (!browser) return NULL;
    GString *out = g_string_new(NULL);

    int pw = 0, ph = 0;
    ns_browser_page_size(browser, &pw, &ph);

    g_string_append(out, "Document\n");
    g_string_append_printf(out, "  url         %s\n",
                           browser->base_url ? browser->base_url : "");
    g_string_append_printf(out, "  charset     %s\n",
                           browser->doc_charset ? browser->doc_charset : "");
    g_string_append_printf(out, "  DOM nodes   %d\n",
                           node_count_walk(browser->doc, 0));
    g_string_append_printf(out, "  layout boxes %d\n",
                           box_count_walk(browser->layout));
    g_string_append_printf(out, "  page size   %d x %d px\n", pw, ph);

    g_string_append(out, "\nLayout\n");
    g_string_append_printf(out, "  viewport    %d x %.0f px\n",
                           browser->vw, browser->vh);
    g_string_append_printf(out, "  last reflow %.2f ms\n",
                           browser->relayout_cost_us / 1000.0);
    g_string_append_printf(out, "  oscillation %d\n", browser->layout_osc);

    g_string_append_c(out, '\n');
    if (browser->js)
        ns_js_dump_stats(browser->js, out);

    g_string_append_c(out, '\n');
    perf_dump_threads(out);

    return g_string_free(out, FALSE);
}

/* Waits for images, then renders the page to a PNG or (by .pdf extension) PDF file. */
int
ns_browser_render_image(ns_browser *browser, const char *path)
{
    if (!browser || !browser->layout || !path) return -1;

    browser_wait_images(browser);

    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);

    int rc;
    gsize len = strlen(path);
    if (len >= 4 && g_ascii_strcasecmp(path + len - 4, ".pdf") == 0)
        rc = ns_engine_write_pdf(browser->layout, path);
    else
        rc = ns_engine_write_png(browser->layout, path);

    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);
    return rc;
}

/* Relayouts for print media and @page size, records pages, then restores the screen layout. */
GPtrArray *
ns_browser_print_pages(ns_browser *browser, ns_print_setup *out_setup)
{
    if (!browser || !browser->doc || !out_setup) return NULL;

    browser_wait_images(browser);

    ns_print_setup setup;
    ns_print_setup_default(&setup);

    int saved_vw = browser->vw;
    double saved_vh = browser->vh;

    ns_css_set_print_media(TRUE);
    browser->cascade_dirty = TRUE;
    browser->vw = (int)(setup.width - setup.margin_left - setup.margin_right);
    browser->vh = setup.height - setup.margin_top - setup.margin_bottom;
    browser_relayout(browser);

    const ns_css_page_rule *rule = ns_render_page_rule();
    if (rule) {
        ns_print_setup_apply_page_rule(&setup, rule);
        int w = (int)(setup.width - setup.margin_left - setup.margin_right);
        if (w > 0 && w != browser->vw) {
            browser->vw = w;
            browser->vh = setup.height - setup.margin_top - setup.margin_bottom;
            browser_relayout(browser);
        }
    }

    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    GPtrArray *pages = ns_engine_print_recordings(browser->layout, &setup);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);

    ns_css_set_print_media(FALSE);
    browser->cascade_dirty = TRUE;
    browser->vw = saved_vw;
    browser->vh = saved_vh;
    browser_relayout(browser);

    *out_setup = setup;
    return pages;
}

/* Monotonic frame timestamp from the shell's frame clock, bounded in lag and non-decreasing. */
static gint64
browser_frame_now(ns_browser *browser)
{
    gint64 now = g_get_monotonic_time();
    gint64 t = browser->frame_time_us;
    if (t <= 0 || t > now || now - t > NS_FRAME_TIME_MAX_LAG_US) t = now;
    if (t < browser->last_frame_now_us) t = browser->last_frame_now_us;
    browser->last_frame_now_us = t;
    return t;
}

/* Collects damage rects of animated nodes; FALSE if any rendered node lacks a box. */
static gboolean
browser_anim_rects(ns_browser *b, GHashTable *nodes, GArray *out)
{
    GHashTable *found = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean ok = ns_damage_nodes_visual_rects(b->layout, b->anim, nodes,
                                               found, out);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, nodes);
    while (ok && g_hash_table_iter_next(&it, &k, NULL)) {
        const ns_node *n = k;
        if (g_hash_table_contains(found, n)) continue;
        const ns_node *root = n;
        while (root->parent) root = root->parent;
        ok = root != b->doc || (b->styles && node_unrendered(b->styles, n));
    }
    g_hash_table_destroy(found);
    return ok;
}

/* Ticks animations, damaging only the animated nodes' rects when that suffices. */
static gboolean
browser_anim_tick(ns_browser *browser, gint64 now)
{
    GPtrArray *nodes = g_ptr_array_new();
    GHashTable *set = NULL;
    GArray *rects = NULL;
    if (browser->layout && !browser->damage_full && !browser->dirty &&
        ns_anim_repaint_nodes(browser->anim, nodes) && nodes->len > 0) {
        set = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (guint i = 0; i < nodes->len; i++)
            g_hash_table_add(set, g_ptr_array_index(nodes, i));
        rects = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
        if (!browser_anim_rects(browser, set, rects))
            g_clear_pointer(&rects, g_array_unref);
    }
    gboolean ticked = ns_anim_tick(browser->anim, now);
    if (ticked) {
        g_ptr_array_set_size(nodes, 0);
        gboolean same = rects && !ns_anim_needs_layout(browser->anim) &&
                        ns_anim_repaint_nodes(browser->anim, nodes);
        for (guint i = 0; same && i < nodes->len; i++)
            same = g_hash_table_contains(set, g_ptr_array_index(nodes, i));
        if (same && browser_anim_rects(browser, set, rects)) {
            if (!browser->damage_rects)
                browser->damage_rects =
                    g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
            g_array_append_vals(browser->damage_rects, rects->data,
                                rects->len);
            browser->repaint_pending = TRUE;
        } else {
            browser->damage_full = TRUE;
        }
    }
    if (rects) g_array_unref(rects);
    if (set) g_hash_table_destroy(set);
    g_ptr_array_free(nodes, TRUE);
    return ticked;
}

/* Runs one frame: refresh, animations, rAF, event loop within budget, restyle/relayout. */
int
ns_browser_tick(ns_browser *browser, int budget_ms)
{
    if (!browser) return -1;
    if (budget_ms < 0) budget_ms = 0;

    if (browser->refresh_due_us && !browser->pending_nav &&
        g_get_monotonic_time() >= browser->refresh_due_us) {
        browser->refresh_due_us = 0;
        browser->pending_nav = browser->refresh_url;
        browser->refresh_url = NULL;
    }

    gint64 deadline = g_get_monotonic_time() + (gint64)budget_ms * 1000;
    gboolean changed = browser->repaint_pending;
    browser->repaint_pending = FALSE;
    if (browser->js)
        ns_js_set_frame_time(browser->js, browser_frame_now(browser));
    gboolean video_changed = FALSE;
    gboolean other_changed = FALSE;
    int guard = 0;
    if (browser->js && !browser->pending_scroll &&
        (fabs(browser->cur_scroll_x - browser->js_scroll_x) > 0.5 ||
         fabs(browser->cur_scroll_y - browser->js_scroll_y) > 0.5)) {
        browser->js_scroll_x = browser->cur_scroll_x;
        browser->js_scroll_y = browser->cur_scroll_y;
        ns_js_note_viewport_scroll(browser->js, browser->cur_scroll_x,
                                   browser->cur_scroll_y);
    }
    if (browser->hover_restyle_pending) {
        gint64 now = g_get_monotonic_time();
        gint64 min_gap = browser->relayout_cost_us * 2;
        if (min_gap < 60000) min_gap = 60000;
        if (now - browser->hover_relayout_us >= min_gap) {
            browser->hover_restyle_pending = FALSE;
            browser->hover_relayout_us = now;
            browser_relayout(browser);
            browser->dirty = FALSE;
            changed = TRUE;
        }
    }
    GPtrArray *frames_changed = g_ptr_array_new();
    for (;;) {
        gint64 now = browser_frame_now(browser);
        if (browser->images &&
            ns_image_cache_tick_collect(browser->images, now, frames_changed)) {
            changed = TRUE;
            other_changed = TRUE;
        }
        if (browser->anim && browser_anim_tick(browser, now)) {
            changed = TRUE;
            other_changed = TRUE;
            browser->cascade_dirty = TRUE;
            if (ns_anim_needs_layout(browser->anim)) {
                browser->dirty = TRUE;
                if (!browser->relayout_why)
                    browser->relayout_why = "animation moves boxes";
            }
        }
        if (browser->anim && ns_anim_take_restyle(browser->anim)) {
            browser->restyle_pending = TRUE;
            changed = TRUE;
        }
        if (browser->anim && browser->js)
            ns_js_dispatch_anim_events(browser->js, browser->anim);
        if (browser->js && ns_js_run_animation_frame(browser->js)) {
            changed = TRUE;
            other_changed = TRUE;
        }

        gboolean did_iter = FALSE;
        int it = 0;
        while (g_main_context_pending(ns_engine_context()) && it++ < 64) {
            g_main_context_iteration(ns_engine_context(), FALSE);
            did_iter = TRUE;
            changed = TRUE;
            if (g_get_monotonic_time() >= deadline) break;
        }

        if (!did_iter) break;
        if (++guard >= 4096) break;
        if (g_get_monotonic_time() >= deadline) break;
    }
    browser_absorb_js_mutation(browser);
    if (!browser->dirty && browser->restyle_pending) {
        if (browser_restyle(browser)) {
            changed = TRUE;
            other_changed = TRUE;
        } else {
            browser->dirty = TRUE;
        }
    }
    if (browser->dirty) {
        if (browser_relayout_from_mutation(browser)) {
            changed = TRUE;
            other_changed = TRUE;
            browser->dirty = FALSE;
        }
    }
    if (frames_changed->len > 0 && !browser->damage_full) {
        if (!browser->damage_images)
            browser->damage_images =
                g_hash_table_new(g_direct_hash, g_direct_equal);
        for (guint i = 0; i < frames_changed->len; i++)
            g_hash_table_add(browser->damage_images,
                             g_ptr_array_index(frames_changed, i));
    }
    g_ptr_array_free(frames_changed, TRUE);
    browser_follow_scroll_anchor(browser);
    if (browser->pending_scroll) changed = TRUE;
    if (browser->repaint_pending) {
        browser->repaint_pending = FALSE;
        changed = TRUE;
    }
    browser->frame_time_us = 0;
    if (browser->js) ns_js_set_frame_time(browser->js, 0);
    (void)video_changed;
    (void)other_changed;
    return changed ? 1 : 0;
}

void
ns_browser_set_frame_time(ns_browser *browser, gint64 frame_time_us)
{
    if (browser) browser->frame_time_us = frame_time_us;
}

/* TRUE while the page needs continuous frames (animations, rAF, animated images). */
int
ns_browser_continuous(ns_browser *browser)
{
    if (!browser) return 0;
    if (browser->dirty || browser->hover_restyle_pending) return 1;
    if (browser->js && ns_js_has_pending_animation_frame(browser->js))
        return 1;
    if (browser->anim && ns_anim_has_active(browser->anim)) return 1;
    if (browser->images && ns_image_cache_animating(browser->images))
        return 1;
    return 0;
}

/* TRUE if any pending layout, paint, navigation, script or animation work needs a frame. */
int
ns_browser_needs_frame(ns_browser *browser)
{
    if (!browser) return 0;
    if (browser->dirty || browser->restyle_pending ||
        browser->repaint_pending || browser->hover_restyle_pending ||
        browser->pending_scroll)
        return 1;
    if (browser->pending_nav || browser->pending_download ||
        browser->pending_clipboard)
        return 1;
    if (ns_camera_has_pending_origin()) return 1;
    if (browser->refresh_url && browser->refresh_due_us &&
        g_get_monotonic_time() >= browser->refresh_due_us)
        return 1;
    if (browser->js && ns_js_wants_frame(browser->js)) return 1;
    if (browser->anim && ns_anim_has_active(browser->anim)) return 1;
    return 0;
}

/* Monotonic time of the next scheduled refresh navigation, or 0. */
gint64
ns_browser_next_wake_us(ns_browser *browser)
{
    if (!browser || !browser->refresh_url) return 0;
    return browser->refresh_due_us;
}

/* TRUE while the page is still changing (layout, images, script, animations, refresh). */
static int
ns_browser_animating(ns_browser *browser)
{
    if (!browser) return 0;
    if (browser->dirty) return 1;
    if (browser->hover_restyle_pending) return 1;
    if (browser->refresh_due_us || browser->refresh_url) return 1;
    if (browser_images_outstanding(browser) > 0) return 1;
    if (browser->js && ns_js_has_pending_animation_frame(browser->js))
        return 1;
    if (browser->js && ns_js_has_pending_work(browser->js))
        return 1;
    if (browser->anim && ns_anim_has_active(browser->anim))
        return 1;
    if (browser->images && ns_image_cache_animating(browser->images))
        return 1;
    return 0;
}

/* Resizes the CSS viewport, dispatching resize to script and relayouting. */
int
ns_browser_set_viewport(ns_browser *browser, int css_width, double css_height)
{
    if (!browser || !browser->doc || css_width <= 0) return -1;
    if (css_height <= 0.0) css_height = (double)css_width * 0.75;
    if (css_width == browser->vw && css_height == browser->vh) return 0;
    browser->vw = css_width;
    browser->vh = css_height;
    ns_css_set_viewport((double)browser->vw, browser->vh);
    if (browser->js) {
        ns_js_sync_window_metrics(browser->js);
        ns_js_dispatch_resize(browser->js);
    }
    browser_damp_reset(browser);
    browser_relayout(browser);
    return 0;
}

/* Scrollable page extent, honoring overflow hidden on html/body. */
int
ns_browser_page_size(ns_browser *browser, int *out_width, int *out_height)
{
    if (!browser || !browser->layout) return -1;
    gboolean hide_x = root_axis_overflow_hidden(browser->layout,
                                                NS_CSS_OVERFLOW_X);
    gboolean hide_y = root_axis_overflow_hidden(browser->layout,
                                                NS_CSS_OVERFLOW_Y);
    double w = hide_x ? browser->vw : browser->layout->content_width;
    if (!(w > 0)) w = browser->vw;
    double bottom = hide_y ? browser->vh : browser->layout->content_height;
    if (!hide_y)
        bottom = ns_box_max_bottom(browser->layout, bottom);
    if (!(bottom > 0)) bottom = 0;
    if (out_width)  *out_width  = (int)w;
    int ypad = (!hide_y && bottom > browser->vh + 0.5) ? 32 : 0;
    if (out_height) *out_height = (int)bottom + ypad;
    return 0;
}

/* Returns the html or body style that declares scroll-snap-type, if any. */
static const ns_style *
viewport_snap_style(ns_browser *browser)
{
    static const char *const sources[] = { "html", "body" };
    if (!browser->doc || !browser->styles) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(sources); i++) {
        const ns_node *n = ns_node_find_first_element(browser->doc, sources[i]);
        const ns_style *s = n ? g_hash_table_lookup(browser->styles, n) : NULL;
        const char *type = s ? ns_style_keyword(s, NS_CSS_SCROLL_SNAP_TYPE)
                             : NULL;
        if (type && strcmp(type, "none") != 0) return s;
    }
    return NULL;
}

/* Applies viewport scroll snapping to a proposed scroll position; 1 if it moved. */
int
ns_browser_snap_document(ns_browser *browser, double viewport_w,
                         double viewport_h, int prev_x, int prev_y,
                         int *scroll_x, int *scroll_y)
{
    if (!browser || !browser->layout || !scroll_x || !scroll_y) return 0;
    const ns_style *s = viewport_snap_style(browser);
    if (!s) return 0;

    int page_w = 0, page_h = 0;
    if (ns_browser_page_size(browser, &page_w, &page_h) != 0) return 0;
    double max_x = page_w - viewport_w;
    double max_y = page_h - viewport_h;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    double x = *scroll_x, y = *scroll_y;
    if (!ns_box_scroll_snap_viewport(browser->layout, s, viewport_w, viewport_h,
                                     max_x, max_y, prev_x, prev_y, &x, &y))
        return 0;

    int nx = (int)(x + 0.5), ny = (int)(y + 0.5);
    if (nx == *scroll_x && ny == *scroll_y) return 0;
    *scroll_x = nx;
    *scroll_y = ny;
    return 1;
}

/* Rescans paint hazards (fixed bands, sticky, 3D) once per layout generation. */
static void
browser_scan_hazards(ns_browser *browser)
{
    if (browser->hazards.fixed_bands &&
        browser->hazards_gen == browser->layout_gen)
        return;
    ns_paint_hazards_scan(browser->layout, browser->anim, &browser->hazards);
    browser->hazards_gen = browser->layout_gen;
}

/* Takes accumulated damage as viewport rects; returns 1 if a full repaint is needed. */
int
ns_browser_take_damage(ns_browser *browser, int scroll_x, int scroll_y,
                       GArray *out_rects)
{
    if (!browser) return 1;
    gboolean full = browser->damage_full || !browser->layout;
    GHashTable *nodes = browser->damage_nodes;
    GHashTable *images = browser->damage_images;
    GArray *extra = browser->damage_rects;
    browser->damage_nodes = NULL;
    browser->damage_images = NULL;
    browser->damage_rects = NULL;
    browser->damage_full = FALSE;
    if (full) {
        browser->layout_gen++;
    } else {
        browser_scan_hazards(browser);
        full = browser->hazards.three_d;
    }
    if (!full) {
        GArray *rects = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
        full = !ns_damage_resolve(browser->layout, browser->js, browser->anim,
                                  nodes, images, rects);
        if (extra) g_array_append_vals(rects, extra->data, extra->len);
        for (guint i = 0; !full && i < rects->len; i++) {
            ns_damage_rect r = g_array_index(rects, ns_damage_rect, i);
            if (!r.fixed) {
                r.x -= scroll_x;
                r.y -= scroll_y;
            }
            g_array_append_val(out_rects, r);
        }
        g_array_free(rects, TRUE);
    }
    if (nodes) g_hash_table_destroy(nodes);
    if (images) g_hash_table_destroy(images);
    if (extra) g_array_free(extra, TRUE);
    return full ? 1 : 0;
}

/* Reports fixed bands for scroll blitting; 0 if sticky, 3D or fixed backgrounds forbid it. */
int
ns_browser_scroll_blit_bands(ns_browser *browser, GArray *out_bands)
{
    if (!browser || !browser->layout) return 0;
    browser_scan_hazards(browser);
    const ns_paint_hazards *h = &browser->hazards;
    if (h->three_d || h->sticky || h->fixed_background) return 0;
    g_array_append_vals(out_bands, h->fixed_bands->data, h->fixed_bands->len);
    return 1;
}

/* Paints one clipped device rect of the page at the scroll offset and scale. */
static void
browser_paint_rect(ns_browser *browser, cairo_surface_t *surf, int scroll_x,
                   int scroll_y, double scale, int x, int y, int w, int h)
{
    cairo_t *cr = cairo_create(surf);
    cairo_set_tolerance(cr, scale > 0 ? 0.5 / scale : 0.5);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);
    if (ns_selection_has_range(&browser->selection))
        ns_paint_with_selection(cr, browser->layout, browser->search_query,
                                &browser->selection);
    else
        ns_paint(cr, browser->layout, browser->search_query);
    cairo_destroy(cr);
}

/* Renders the viewport (or given rects) into a caller ARGB32 buffer with padding. */
int
ns_browser_render_argb32_rects(ns_browser *browser, int scroll_x, int scroll_y,
                               int width, int height, int pad, double scale,
                               unsigned char *out, int stride,
                               const int *rects, int n_rects)
{
    if (!browser || !browser->layout || !out) return -1;
    if (width <= 0 || height <= 0 || stride < width * 4) return -1;
    if (!(scale > 0)) scale = 1.0;

    browser->cur_scroll_x = (double)scroll_x;
    browser->cur_scroll_y = (double)scroll_y;
    browser->cur_viewport_h = (double)height / scale;
    ns_box_set_hit_viewport(browser->cur_scroll_x, browser->cur_scroll_y);
    browser_ensure_images(browser);

    cairo_surface_t *surf =
        cairo_image_surface_create_for_data(out, CAIRO_FORMAT_ARGB32,
                                            width, height + 2 * pad, stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return -1;
    }
    cairo_surface_set_device_offset(surf, 0, pad);
    ns_paint_set_device_viewport(0, 0, width, height);
    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    ns_paint_set_search(browser->search_case, browser->search_active);
    ns_paint_set_caret_visible(browser->caret_paint_visible);
    gint64 paint_t0 = g_get_monotonic_time();
    if (rects && n_rects > 0) {
        for (int i = 0; i < n_rects; i++)
            browser_paint_rect(browser, surf, scroll_x, scroll_y, scale,
                               rects[4 * i], rects[4 * i + 1],
                               rects[4 * i + 2], rects[4 * i + 3]);
    } else {
        browser_paint_rect(browser, surf, scroll_x, scroll_y, scale,
                           0, 0, width, height);
    }
    if (g_getenv("NS_PROFILE"))
        g_printerr("[profile] paint %6.1fms %dx%d\n",
                   (double)(g_get_monotonic_time() - paint_t0) / 1000.0,
                   width, height);
    ns_paint_set_search(FALSE, NULL);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);
    ns_paint_clear_device_viewport();

    cairo_surface_flush(surf);
    /* Frame dumps are a tooling aid: never in the interactive browser,
     * where any process that can set the environment could make it write
     * every page it shows to disk. */
    const char *dump_dir = ns_security_tool_mode()
        ? g_getenv("NS_FRAME_DUMP") : NULL;
    if (dump_dir && *dump_dir) {
        static int frame_no;
        if (frame_no % 30 == 0) {
            char *path = g_strdup_printf("%s/frame-%05d.png",
                                         dump_dir, frame_no);
            cairo_surface_write_to_png(surf, path);
            g_free(path);
        }
        frame_no++;
    }
    cairo_surface_destroy(surf);
    return 0;
}

static const ns_node *browser_hit_node(ns_browser *browser, int x, int y);

static gboolean
browser_node_is_hyperlink(const ns_node *n)
{
    return ns_node_is_element_named(n, "a") ||
           ns_node_is_element_named(n, "area");
}

/* Resolved href of a link at or near a point (6px probe ring); caller frees. */
char *
ns_browser_link_at(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return NULL;

    static const int kR = 6;
    static const int probe[][2] = {
        { 0, 0 },
        { 0, -kR }, { 0, kR }, { -kR, 0 }, { kR, 0 },
        { -kR, -kR }, { kR, -kR }, { -kR, kR }, { kR, kR },
    };
    for (int i = 0; i < (int)(sizeof probe / sizeof probe[0]); i++) {
        int px = x + probe[i][0], py = y + probe[i][1];
        const char *href = NULL;
        const ns_node *node = browser_hit_node(browser, px, py);
        for (const ns_node *a = node; a && (!href || !*href); a = a->parent)
            if (browser_node_is_hyperlink(a))
                href = ns_element_get_attr(a, "href");
        if (href && *href) return browser_resolve_navigation(browser, href);
    }
    return NULL;
}

/* CSS cursor keyword at a point, or "text" over editable/selectable text; caller frees. */
char *
ns_browser_cursor_at(ns_browser *browser, int x, int y)
{
    static const char *const known[] = {
        "default", "none", "context-menu", "help", "pointer", "progress",
        "wait", "cell", "crosshair", "text", "vertical-text", "alias",
        "copy", "move", "no-drop", "not-allowed", "grab", "grabbing",
        "all-scroll", "col-resize", "row-resize", "n-resize", "e-resize",
        "s-resize", "w-resize", "ne-resize", "nw-resize", "se-resize",
        "sw-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize",
        "zoom-in", "zoom-out",
    };
    if (!browser || !browser->layout) return NULL;
    if (browser->js && !browser->relaying)
        browser_flush_style(browser);
    if (!browser->layout || !browser->styles) return NULL;

    const ns_node *node =
        ns_box_hit_dom(browser->layout, (double)x, (double)y);

    const ns_style *style = NULL;
    for (const ns_node *n = node; n && !style; n = n->parent)
        style = g_hash_table_lookup(browser->styles, n);
    if (!style) return NULL;

    const ns_css_value *v = style->values[NS_CSS_CURSOR];
    char *match = NULL;
    if (v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        char **tokens = g_strsplit_set(v->u.keyword, ", \t", -1);
        for (int i = 0; tokens && tokens[i]; i++) {
            if (!tokens[i][0]) continue;
            for (gsize k = 0; k < G_N_ELEMENTS(known); k++)
                if (g_ascii_strcasecmp(tokens[i], known[k]) == 0) {
                    g_free(match);
                    match = g_strdup(known[k]);
                }
        }
        g_strfreev(tokens);
    }
    if (match) return match;

    for (const ns_node *n = node; n; n = n->parent)
        if (browser_node_is_hyperlink(n) &&
            ns_element_get_attr(n, "href")) return NULL;
    for (const ns_node *n = node; n; n = n->parent)
        if (ns_node_is_text_input(n)) return g_strdup("text");
    for (const ns_node *n = node; n; n = n->parent)
        if (ns_node_is_contenteditable_host(n)) return g_strdup("text");
    if (ns_selection_text_at(browser->layout, (double)x, (double)y))
        return g_strdup("text");
    return NULL;
}

/* Publishes the current selection text and viewport bounds to script. */
static void
browser_sync_js_selection(ns_browser *browser)
{
    if (!browser || !browser->js) return;
    char *text = ns_selection_collect_text(browser->layout,
                                           &browser->selection);
    double x = 0, y = 0, w = 0, h = 0;
    if (ns_selection_bounds(browser->layout, &browser->selection,
                            &x, &y, &w, &h)) {
        x -= browser->cur_scroll_x;
        y -= browser->cur_scroll_y;
    }
    ns_js_set_selection(browser->js, text,
                        ns_selection_has_range(&browser->selection),
                        x, y, w, h);
    g_free(text);
}

/* Finds the media element whose native control is under a point and the seek fraction. */
static const ns_node *
browser_media_control_at(ns_browser *browser, const ns_node *node,
                         int x, int y, ns_media_control *control,
                         double *fraction)
{
    for (const ns_node *el = node; el; el = el->parent) {
        if (!ns_node_is_element_named(el, "video") &&
            !ns_node_is_element_named(el, "audio"))
            continue;
        const ns_box *b = browser_box_for_node(browser->layout, el);
        if (!b) return NULL;
        ns_media_controls_rect r;
        if (!ns_media_controls_layout(el,
                b->x + b->margin.left + b->border.left + b->padding.left,
                b->y + b->margin.top + b->border.top + b->padding.top,
                b->content_width, b->content_height, &r))
            return NULL;
        *control = ns_media_controls_hit(&r, x, y);
        if (*control == NS_MEDIA_CONTROL_NONE) return NULL;
        double span = r.seek_x1 - r.seek_x0;
        *fraction = span > 0.0 ? (x - r.seek_x0) / span : 0.0;
        return el;
    }
    return NULL;
}

/* Continues a seek-bar drag on the active media element; FALSE if none. */
static gboolean
browser_media_seek_drag(ns_browser *browser, int x)
{
    const ns_node *el = browser->media_seek_node;
    if (!el) return FALSE;
    const ns_box *b = browser_box_for_node(browser->layout, el);
    ns_media_controls_rect r;
    if (b && ns_media_controls_layout(el,
            b->x + b->margin.left + b->border.left + b->padding.left,
            b->y + b->margin.top + b->border.top + b->padding.top,
            b->content_width, b->content_height, &r)) {
        double span = r.seek_x1 - r.seek_x0;
        double fraction = span > 0.0 ? (x - r.seek_x0) / span : 0.0;
        ns_media_controls_activate(browser->js, el, NS_MEDIA_CONTROL_SEEK,
                                   fraction);
    }
    return TRUE;
}

/* Performs a selection action by kind (anchor, extend, clear, all, copy, word, block). */
char *
ns_browser_select(ns_browser *browser, int kind, int x, int y)
{
    if (!browser || !browser->layout) return NULL;
    switch (kind) {
    case 0: ns_selection_anchor_at(&browser->selection, browser->layout,
                                   (double)x, (double)y); break;
    case 1: if (browser_media_seek_drag(browser, x)) return NULL;
            ns_selection_extend_to(&browser->selection, browser->layout,
                                   (double)x, (double)y);
            browser->selection_dragged = TRUE; break;
    case 2: ns_selection_clear(&browser->selection); break;
    case 3: ns_selection_select_all(&browser->selection, browser->layout);
            break;
    case 4: return ns_selection_collect_text(browser->layout,
                                             &browser->selection);
    case 5: ns_selection_select_word_at(&browser->selection, browser->layout,
                                        (double)x, (double)y);
            browser->selection_dragged = TRUE; break;
    case 6: ns_selection_select_block_at(&browser->selection, browser->layout,
                                         (double)x, (double)y);
            browser->selection_dragged = TRUE; break;
    default: break;
    }
    browser_sync_js_selection(browser);
    return NULL;
}

/* Dispatches a pointer event and its mouse counterpart to a target. */
static void
browser_hover_dispatch(ns_browser *b, const ns_node *target, int x, int y,
                       const char *ptr_type, const char *mouse_type,
                       const ns_node *related)
{
    if (!b->js || !target) return;
    ns_js_dispatch_mouse_event(b->js, target, ptr_type,
                               (double)x - b->cur_scroll_x,
                               (double)y - b->cur_scroll_y,
                               (double)x, (double)y, 0, 0,
                               FALSE, FALSE, FALSE, FALSE, related, NULL);
    ns_js_dispatch_mouse_event(b->js, target, mouse_type,
                               (double)x - b->cur_scroll_x,
                               (double)y - b->cur_scroll_y,
                               (double)x, (double)y, 0, 0,
                               FALSE, FALSE, FALSE, FALSE, related, NULL);
}

/* Updates hover: dispatches over/out/enter/leave/move and restyles for :hover (throttled). */
int
ns_browser_hover(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return -1;

    const ns_node *node =
        ns_box_hit_dom(browser->layout, (double)x, (double)y);

    browser_prune_cached_nodes(browser);
    const ns_node *prev = browser->hover_node;
    gboolean changed = node != prev;
    browser->hover_node = node;

    gboolean dirty = FALSE;
    if (browser->js) {
        if (changed) {
            browser_hover_dispatch(browser, prev, x, y, "pointerout",
                                   "mouseout", node);
            browser_hover_dispatch(browser, prev, x, y, "pointerleave",
                                   "mouseleave", node);
            browser_hover_dispatch(browser, node, x, y, "pointerover",
                                   "mouseover", prev);
            browser_hover_dispatch(browser, node, x, y, "pointerenter",
                                   "mouseenter", prev);
        }
        browser_hover_dispatch(browser, node, x, y, "pointermove",
                               "mousemove", NULL);
        if (browser_absorb_js_mutation(browser) == BROWSER_JS_RELAYOUT)
            dirty = TRUE;
    }

    gboolean hover_restyle = changed && ns_render_page_uses_hover() &&
                             !ns_selection_has_range(&browser->selection);
    if (!dirty && (hover_restyle || browser->restyle_pending) &&
        !browser->dirty) {
        gboolean js_restyle = browser->restyle_pending;
        if (browser_restyle(browser)) {
            browser->hover_restyle_pending = FALSE;
            return 1;
        }
        if (js_restyle) dirty = TRUE;
        else browser->dirty = FALSE;
    }
    if (dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        return 1;
    }
    if (hover_restyle) {
        gint64 now = g_get_monotonic_time();
        gint64 min_gap = browser->relayout_cost_us * 2;
        if (min_gap < 60000) min_gap = 60000;
        if (now - browser->hover_relayout_us >= min_gap) {
            browser->hover_relayout_us = now;
            browser->hover_restyle_pending = FALSE;
            browser_relayout(browser);
            browser->dirty = FALSE;
            return 1;
        }
        browser->hover_restyle_pending = TRUE;
    }
    return 0;
}

/* Scrolls the scrollable box under a point, applying snap and firing scroll; 1 if consumed. */
int
ns_browser_scroll_at(ns_browser *browser, int x, int y, int dx, int dy)
{
    if (!browser || !browser->layout) return 0;

    ns_box *box = ns_box_hit_scrollable(browser->layout, (double)x, (double)y);
    if (!box) return 0;

    double prev_x = box->scroll_x, prev_y = box->scroll_y;
    int consumed = 0;
    if (dy != 0 && box->scroll_max_y > 0) {
        double ny = box->scroll_y + dy;
        if (ny < 0) ny = 0;
        if (ny > box->scroll_max_y) ny = box->scroll_max_y;
        if (ny != box->scroll_y) { box->scroll_y = ny; consumed = 1; }
    }
    if (dx != 0 && box->scroll_max_x > 0) {
        double nx = box->scroll_x + dx;
        if (nx < 0) nx = 0;
        if (nx > box->scroll_max_x) nx = box->scroll_max_x;
        if (nx != box->scroll_x) { box->scroll_x = nx; consumed = 1; }
    }

    if (consumed) {
        ns_box_scroll_snap_from(box, prev_x, prev_y);
        if (browser->js && box->dom)
            ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    }
    return consumed;
}

/* Computes vertical scrollbar track and thumb geometry for a scrolling box. */
static gboolean
sb_vgeom(const ns_box *b, double *track_x, double *track_w,
         double *track_y, double *track_h, double *thumb_y, double *thumb_h)
{
    if (!b || b->scroll_max_y <= 0) return FALSE;
    double py = b->y + b->margin.top + b->border.top;
    double ph = b->content_height + b->padding.top + b->padding.bottom;
    if (ph <= 16.0) return FALSE;
    double px = b->x + b->margin.left + b->border.left;
    double pw = b->content_width + b->padding.left + b->padding.right;
    double tw = 8.0;
    double th = ph - 2.0;
    double total = ph + b->scroll_max_y;
    double thh = th * (ph / total);
    if (thh < 16.0) thh = 16.0;
    if (thh > th) thh = th;
    *track_x = px + pw - tw - 1.0;
    *track_w = tw;
    *track_y = py + 1.0;
    *track_h = th;
    *thumb_h = thh;
    *thumb_y = *track_y + (th - thh) * (b->scroll_y / b->scroll_max_y);
    return TRUE;
}

/* Finds the scrolling box generated by a node, for re-finding it after relayout. */
static ns_box *
box_find_scrollable_by_dom(ns_box *root, const ns_node *node)
{
    if (!root || !node) return NULL;
    if (root->dom == node && root->scrolls) return root;
    for (ns_box *c = root->first_child; c; c = c->next_sibling) {
        ns_box *m = box_find_scrollable_by_dom(c, node);
        if (m) return m;
    }
    return NULL;
}

/* Starts a vertical scrollbar drag at a point; clicks on the track jump the thumb there. */
int
ns_browser_scrollbar_press(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return 0;
    double lx = 0, ly = 0;
    ns_box *box = ns_box_hit_scrollbar(browser->layout, (double)x, (double)y,
                                       &lx, &ly);
    if (!box) return 0;

    double tx, tw, ty, th, thy, thh;
    if (!sb_vgeom(box, &tx, &tw, &ty, &th, &thy, &thh)) return 0;
    if (lx < tx - 3.0 || lx > tx + tw + 3.0 || ly < ty || ly > ty + th)
        return 0;

    double grab;
    if (ly >= thy && ly <= thy + thh) {
        grab = ly - thy;
    } else {
        grab = thh / 2.0;
        double ns = (th > thh)
            ? (ly - ty - grab) / (th - thh) * box->scroll_max_y : 0.0;
        if (ns < 0) ns = 0;
        if (ns > box->scroll_max_y) ns = box->scroll_max_y;
        box->scroll_y = ns;
        if (box->dom && browser->js)
            ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    }

    browser->sb_dragging = TRUE;
    browser->sb_box = box;
    browser->sb_node = box->dom;
    browser->sb_grab = grab;
    return 1;
}

/* Moves the dragged scrollbar thumb to y and fires scroll; 1 if the offset changed. */
int
ns_browser_scrollbar_drag(ns_browser *browser, int x, int y)
{
    (void)x;
    if (!browser || !browser->sb_dragging) return 0;
    ns_box *box = browser->sb_box;
    if (browser->sb_node)
        box = box_find_scrollable_by_dom(browser->layout, browser->sb_node);
    if (!box) {
        browser->sb_dragging = FALSE;
        browser->sb_box = NULL;
        return 0;
    }
    browser->sb_box = box;

    double tx, tw, ty, th, thy, thh;
    if (!sb_vgeom(box, &tx, &tw, &ty, &th, &thy, &thh) || th <= thh)
        return 0;
    double ns = ((double)y - ty - browser->sb_grab) / (th - thh)
              * box->scroll_max_y;
    if (ns < 0) ns = 0;
    if (ns > box->scroll_max_y) ns = box->scroll_max_y;
    if (ns == box->scroll_y) return 0;
    box->scroll_y = ns;
    if (box->dom && browser->js)
        ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    return 1;
}

/* Ends any scrollbar drag. */
void
ns_browser_scrollbar_release(ns_browser *browser)
{
    if (!browser) return;
    browser->sb_dragging = FALSE;
    browser->sb_box = NULL;
    browser->sb_node = NULL;
}

/* Dispatches dragenter/dragover then drop (if accepted) or dragleave for dropped files. */
int
ns_browser_drop_files(ns_browser *browser, int x, int y,
                      const char *const *paths, int n_paths)
{
    if (!browser || !browser->js || !browser->layout || !paths || n_paths <= 0)
        return 0;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_node *target = hit ? hit->dom : NULL;
    if (!target && browser->doc)
        target = ns_node_find_first_element(browser->doc, "body");
    if (!target) target = browser->doc;
    if (!target) return 0;

    ns_js_drag_session *session = ns_js_drag_session_new(browser->js);
    if (!session) return 0;
    for (int i = 0; i < n_paths; i++)
        if (paths[i]) ns_js_drag_session_add_file(session, paths[i]);

    gboolean accept = FALSE;
    gboolean prevented = FALSE;
    ns_js_dispatch_drag_event(browser->js, session, target, "dragenter",
                              x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                              NULL, &prevented);
    if (prevented) accept = TRUE;
    prevented = FALSE;
    ns_js_dispatch_drag_event(browser->js, session, target, "dragover",
                              x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                              NULL, &prevented);
    if (prevented) accept = TRUE;
    if (accept)
        ns_js_dispatch_drag_event(browser->js, session, target, "drop",
                                  x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                                  NULL, &prevented);
    else
        ns_js_dispatch_drag_event(browser->js, session, target, "dragleave",
                                  x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                                  NULL, &prevented);
    ns_js_drag_session_free(session);

    if (ns_js_consume_mutated(browser->js)) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        return 1;
    }
    return 0;
}

/* Returns and clears buffered console output; caller frees. */
char *
ns_browser_console_drain(ns_browser *browser)
{
    if (!browser || !browser->console_buf || browser->console_buf->len == 0)
        return NULL;
    char *out = g_strdup(browser->console_buf->str);
    g_string_truncate(browser->console_buf, 0);
    return out;
}

/* Evaluates devtools console source in the page and applies resulting layout changes. */
char *
ns_browser_eval(ns_browser *browser, const char *src)
{
    if (!browser || !browser->js || !src) return NULL;
    browser_damp_reset(browser);
    char *res = ns_js_eval_source(browser->js, src, "devtools-console");
    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    browser_absorb_js_mutation(browser);
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    return res;
}

/* Dispatches contextmenu at a point; 1 if the page prevented the default menu. */
int
ns_browser_contextmenu(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout || !browser->js) return 0;
    const ns_node *node = browser_hit_node(browser, x, y);
    if (!node) return 0;
    gboolean prevented = FALSE;
    ns_js_dispatch_mouse_event(browser->js, node, "contextmenu",
                               (double)x - browser->cur_scroll_x,
                               (double)y - browser->cur_scroll_y,
                               (double)x, (double)y,
                               2, 0, FALSE, FALSE, FALSE, FALSE, NULL,
                               &prevented);
    browser_absorb_js_mutation(browser);
    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    browser_absorb_js_mutation(browser);
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    return prevented ? 1 : 0;
}

/* Media URL under a point for opening externally (page URL for streams); caller frees. */
char *
ns_browser_media_at(ns_browser *browser, int x, int y, int *out_is_video,
                    int *out_stream)
{
    if (out_is_video) *out_is_video = 0;
    if (out_stream) *out_stream = 0;
    if (!browser || !browser->layout) return NULL;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_box *media = NULL;
    for (const ns_box *b = hit; b; b = b->parent) {
        gboolean has_media_url =
            b->media && (b->media->video_src || b->media->video_audio_src);
        gboolean has_internal_video =
            b->dom && b->dom->kind == NS_NODE_ELEMENT &&
            (ns_element_get_attr(b->dom, NS_MEDIA_SRC_ATTR) != NULL ||
             ns_element_get_attr(b->dom, NS_MEDIA_STREAM_ATTR) != NULL);
        if (b->dom && (ns_node_is_element_named(b->dom, "video") ||
                       ns_node_is_element_named(b->dom, "audio") ||
                       has_media_url || has_internal_video)) {
            media = b;
            break;
        }
    }
    if (!media || !media->dom) return NULL;

    if (media->dom->kind == NS_NODE_ELEMENT) {
        const char *stream_kind = ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR);
        if (stream_kind && g_strcmp0(stream_kind, "camera") == 0)
            return NULL;
    }

    gboolean is_video =
        ns_node_is_element_named(media->dom, "video") ||
        (media->media && media->media->video_src) ||
        (media->dom->kind == NS_NODE_ELEMENT &&
         (ns_element_get_attr(media->dom, NS_MEDIA_SRC_ATTR) != NULL ||
          ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR) != NULL));
    gboolean force_stream =
        media->dom->kind == NS_NODE_ELEMENT &&
        ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR) != NULL;
    const char *msrc = NULL;
    if (media->media) {
        if (is_video) {
            msrc = media->media->video_src;
            if (!msrc) msrc = media->media->video_audio_src;
        } else {
            msrc = media->media->video_audio_src;
        }
    }
    if ((!msrc || !*msrc) && media->dom->kind == NS_NODE_ELEMENT)
        msrc = ns_element_get_attr(media->dom, NS_MEDIA_SRC_ATTR);
    char *abs = (!force_stream && msrc) ? ns_url_resolve(browser->base_url, msrc)
                                        : NULL;
    gboolean stream = force_stream || !abs || g_str_has_prefix(abs, "blob:") ||
                      g_str_has_prefix(abs, "data:");
    if (stream) {
        g_free(abs);
        abs = browser->base_url ? g_strdup(browser->base_url) : NULL;
    }
    if (!abs) return NULL;
    if (g_str_has_prefix(abs, "file://") &&
        (!browser->base_url || !g_str_has_prefix(browser->base_url, "file://"))) {
        g_free(abs);
        return NULL;
    }
    if (out_is_video) *out_is_video = is_video ? 1 : 0;
    if (out_stream) *out_stream = stream ? 1 : 0;
    return abs;
}

/* Find-in-page: counts matches and selects the next/previous match by direction. */
int
ns_browser_find(ns_browser *browser, const char *query, int case_sensitive,
                int direction, int from_y, int *out_total, int *out_current,
                int *out_y)
{
    if (out_total) *out_total = 0;
    if (out_current) *out_current = 0;
    if (out_y) *out_y = 0;
    if (!browser || !browser->layout) return -1;

    gboolean cs = case_sensitive != 0;
    if (!query || !*query) {
        g_clear_pointer(&browser->search_query, g_free);
        browser->search_active = NULL;
        browser->search_case = cs;
        return 0;
    }

    if (!browser->search_query || strcmp(browser->search_query, query) != 0 ||
        browser->search_case != cs) {
        g_free(browser->search_query);
        browser->search_query = g_strdup(query);
        browser->search_active = NULL;
    }
    browser->search_case = cs;

    guint total = ns_box_count_matches(browser->layout, query, cs);
    if (out_total) *out_total = (int)total;
    if (total == 0) {
        browser->search_active = NULL;
        return 0;
    }

    double cur_y = browser->search_active ? browser->search_active->y
                                          : (double)from_y;
    const ns_box *target = NULL;
    if (direction == 2) {
        target = ns_box_first_match_above(browser->layout, query, cur_y, cs);
        if (!target)
            target = ns_box_first_match_above(browser->layout, query,
                                              G_MAXDOUBLE, cs);
    } else if (direction == 1) {
        target = ns_box_first_match_below(browser->layout, query, cur_y + 2,
                                          cs);
        if (!target)
            target = ns_box_first_match_below(browser->layout, query, -1, cs);
    } else {
        target = ns_box_first_match_below(browser->layout, query,
                                          (double)from_y - 1, cs);
        if (!target)
            target = ns_box_first_match_below(browser->layout, query, -1, cs);
    }

    browser->search_active = target;
    if (target) {
        if (out_y) *out_y = (int)target->y;
        if (out_current)
            *out_current = (int)ns_box_match_ordinal(browser->layout, query,
                                                     target, cs);
    }
    return 0;
}

/* Takes the pending form POST body and content type; caller frees both. */
char *
ns_browser_take_post(ns_browser *browser, size_t *out_len, char **out_ct)
{
    if (out_len) *out_len = 0;
    if (out_ct) *out_ct = NULL;
    if (!browser || !browser->pending_post_body) return NULL;
    char *body = browser->pending_post_body;
    browser->pending_post_body = NULL;
    if (out_len) *out_len = browser->pending_post_len;
    browser->pending_post_len = 0;
    if (out_ct) *out_ct = browser->pending_post_ct;
    else        g_free(browser->pending_post_ct);
    browser->pending_post_ct = NULL;
    return body;
}

/* Builds the form data set via script's entry list or the static collector. */
static void
browser_collect_form_entries(ns_browser *b, const ns_node *form,
                             const ns_node *clicked, GString *out,
                             gboolean *first)
{
    const ns_node *submitter =
        clicked && clicked != form && !ns_node_is_text_input(clicked)
            ? clicked : NULL;
    if (b->js && ns_js_form_entry_list(b->js, form, submitter, out, first))
        return;
    ns_form_collect_inputs(form, b->doc, b->doc, out, first, clicked);
}

/* Builds a GET/POST form submission per method/action, charset, CSP and frame identity. */
static void
browser_perform_form_navigation(ns_browser *b, const ns_node *form,
                                const ns_node *clicked)
{
    if (!b || !form || !b->doc) return;
    /* Forms inside a frame are navigated by the top-level browser.  The
     * request must still carry the frame's identity -- its URL as the
     * base for the action and as the initiator (Origin, Sec-Fetch-Site,
     * Lax cookies) -- not the embedder's, or a cross-origin frame could
     * post to its embedder as if the embedder had. */
    const char *form_base = b->base_url;
    g_clear_pointer(&b->pending_nav_initiator, g_free);
    for (const ns_node *p = form->parent; p; p = p->parent) {
        if (!ns_node_is_element_named(p, "iframe") &&
            !ns_node_is_element_named(p, "frame") &&
            !ns_node_is_element_named(p, "object"))
            continue;
        const char *fu = ns_element_get_attr(p, "data-nd-frame-url");
        gboolean opaque = ns_element_get_attr(p, "data-nd-frame-opaque") != NULL;
        if (fu && *fu) form_base = fu;
        if (opaque || !fu || !ns_url_same_origin(fu, b->base_url))
            b->pending_nav_initiator =
                g_strdup(opaque || !fu ? "about:blank" : fu);
        break;
    }
    gboolean from_text = clicked && ns_node_is_text_input(clicked);

    const char *method = ns_element_get_attr(form, "method");
    const char *formmethod = (clicked && !from_text)
                             ? ns_element_get_attr(clicked, "formmethod") : NULL;
    if (formmethod && *formmethod) method = formmethod;
    gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;

    const char *action = ns_element_get_attr(form, "action");
    const char *formaction = (clicked && !from_text)
                             ? ns_element_get_attr(clicked, "formaction") : NULL;
    if (formaction && *formaction) action = formaction;
    char *abs_action = (action && *action) ? ns_url_resolve(form_base, action)
                                           : g_strdup(form_base);
    if (!abs_action) return;
    if (!browser_allows_navigation_url(b, abs_action)) {
        g_free(abs_action);
        return;
    }
    if (b->js && !ns_js_csp_form_action_allowed(b->js, abs_action)) {
        g_free(abs_action);
        return;
    }

    const char *accept_charset = ns_element_get_attr(form, "accept-charset");
    ns_form_set_submission_charset(
        (accept_charset && *accept_charset) ? accept_charset
                                            : b->doc_charset);

    if (is_post) {
        GString *body = g_string_new(NULL);
        gboolean first = TRUE;
        browser_collect_form_entries(b, form, clicked, body, &first);
        ns_form_set_submission_charset(NULL);
        g_free(b->pending_post_body);
        g_free(b->pending_post_ct);
        b->pending_post_len = body->len;
        b->pending_post_body = g_string_free(body, FALSE);
        b->pending_post_ct = g_strdup("application/x-www-form-urlencoded");
        g_free(b->pending_nav);
        b->pending_nav = abs_action;
        return;
    }

    GString *query = g_string_new(NULL);
    gboolean first = TRUE;
    browser_collect_form_entries(b, form, clicked, query, &first);
    ns_form_set_submission_charset(NULL);

    char *frag = strchr(abs_action, '#');
    if (frag) *frag = '\0';
    char *full;
    if (query->len == 0) {
        full = g_strdup(abs_action);
    } else {
        full = strchr(abs_action, '?')
            ? g_strdup_printf("%s&%s", abs_action, query->str)
            : g_strdup_printf("%s?%s", abs_action, query->str);
    }
    g_string_free(query, TRUE);
    g_free(abs_action);
    g_free(b->pending_nav);
    b->pending_nav = full;
}

/* Script form.submit()/requestSubmit hook: performs the form navigation. */
static void
browser_js_form_submit(const ns_node *form, const ns_node *submitter,
                       gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !form) return;
    browser_perform_form_navigation(b, form, submitter ? submitter : form);
}

/* Implicit/explicit submission: validates, fires submit, then navigates unless cancelled. */
static void
browser_submit_form(ns_browser *b, const ns_node *clicked)
{
    if (!clicked || !b->doc) return;
    gboolean from_text = ns_node_is_text_input(clicked);
    gboolean from_form = ns_node_is_element_named(clicked, "form");
    if (!from_text && !from_form && !ns_form_is_submit_trigger(clicked))
        return;
    const ns_node *form = from_form ? clicked : ns_form_owner(clicked, b->doc);
    if (!form) return;

    if (b->js) {
        if (!ns_js_form_submission_allowed(b->js, form, clicked)) return;
    } else if (!ns_element_get_attr(form, "novalidate") &&
               !ns_element_get_attr(clicked, "formnovalidate") &&
               ns_form_first_invalid(form, b->doc, b->doc)) {
        return;
    }

    if (b->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_submit_event(b->js, form, clicked, &prevented);
        browser_absorb_js_mutation(b);
        if (prevented) return;
    }

    browser_perform_form_navigation(b, form, clicked);
}

static const ns_node *
browser_hit_node(ns_browser *browser, int x, int y)
{
    return ns_box_hit_dom(browser->layout, (double)x, (double)y);
}

/* Pointer down: clears selection, fires pointerdown/mousedown, media controls, focus. */
char *
ns_browser_press(ns_browser *browser, int x, int y, int mods)
{
    if (!browser || !browser->layout) return NULL;
    ns_js_note_pointer_input(browser->js, TRUE);
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);
    gboolean extending = (mods & 1) != 0 &&
                         ns_selection_has_range(&browser->selection);
    if (!extending) {
        ns_selection_clear(&browser->selection);
        browser_sync_js_selection(browser);
    }
    browser->selection_dragged = FALSE;

    const ns_node *node = browser_hit_node(browser, x, y);
    browser->media_seek_node = NULL;
    browser->press_node = node;
    browser->press_x = x;
    browser->press_y = y;
    browser->press_mods = mods;
    browser->press_active = node != NULL;

    ns_css_set_active_node(node);
    if (node && ns_render_page_uses_active()) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    if (browser->js && node) {
        gboolean sh = (mods & 1) != 0, ct = (mods & 2) != 0;
        gboolean al = (mods & 4) != 0, me = (mods & 8) != 0;
        ns_js_dispatch_mouse_event(browser->js, node, "pointerdown",
                                   (double)x - browser->cur_scroll_x,
                                   (double)y - browser->cur_scroll_y,
                                   (double)x, (double)y,
                                   0, 1, sh, ct, al, me, NULL, NULL);
        ns_js_dispatch_mouse_event(browser->js, node, "mousedown",
                                   (double)x - browser->cur_scroll_x,
                                   (double)y - browser->cur_scroll_y,
                                   (double)x, (double)y,
                                   0, 1, sh, ct, al, me, NULL, NULL);
        browser_absorb_js_mutation(browser);
    }

    ns_media_control control = NS_MEDIA_CONTROL_NONE;
    double fraction = 0.0;
    const ns_node *media = browser->js
        ? browser_media_control_at(browser, node, x, y, &control, &fraction)
        : NULL;
    if (media) {
        browser->press_active = FALSE;
        browser->press_node = NULL;
        if (control == NS_MEDIA_CONTROL_SEEK) browser->media_seek_node = media;
        ns_media_controls_activate(browser->js, media, control, fraction);
        browser_absorb_js_mutation(browser);
        if (browser->dirty) {
            browser_relayout(browser);
            browser->dirty = FALSE;
        }
        return NULL;
    }

    gboolean in_datalist = FALSE;
    for (const ns_node *a = node; a; a = a->parent)
        if (ns_node_is_element_named(a, "datalist")) { in_datalist = TRUE; break; }
    if (browser->js && !in_datalist) {
        const ns_node *focus = NULL;
        for (const ns_node *a = node; a; a = a->parent)
            if (ns_node_is_focusable(a)) { focus = a; break; }
        ns_js_focus_from_pointer(browser->js, node);
        const char *val = focus ? ns_node_editable_value(focus) : NULL;
        browser->caret_byte = val ? strlen(val) : 0;
        browser->sel_anchor_byte = browser->caret_byte;
        browser->datalist_suppressed =
            !(focus && ns_node_is_element_named(focus, "input") &&
              ns_element_get_attr(focus, "list") != NULL);
        browser_absorb_js_mutation(browser);
    }

    if (browser->js) {
        if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
        browser_absorb_js_mutation(browser);
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

/* TRUE for a single-choice <select> shown as a dropdown (no multiple, size <= 1). */
static gboolean
ns_is_dropdown_select(const ns_node *n)
{
    if (!ns_node_is_element_named(n, "select")) return FALSE;
    if (ns_element_get_attr(n, "multiple")) return FALSE;
    const char *sz = ns_element_get_attr(n, "size");
    if (sz && atoi(sz) > 1) return FALSE;
    return TRUE;
}

static void browser_input_replace(ns_browser *b, ns_node *node, gsize del_start,
                                  gsize del_end, const char *insert);

/* Applies a clicked <datalist> option to the focused input and fires change. */
static gboolean
browser_datalist_click(ns_browser *browser, const ns_node *node)
{
    const ns_node *option = NULL, *dl = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!option && ns_node_is_element_named(a, "option")) option = a;
        if (ns_node_is_element_named(a, "datalist")) { dl = a; break; }
        if (ns_node_is_element_named(a, "select")) return FALSE;
    }
    if (!option || !dl || !browser->js) return FALSE;
    ns_node *inp = (ns_node *)ns_js_focused_node(browser->js);
    if (!inp || !ns_node_is_element_named(inp, "input")) return FALSE;
    const char *ov = ns_element_get_attr(option, "value");
    char *val = (ov && *ov) ? g_strdup(ov) : ns_option_label_dup(option);
    const char *cur = ns_node_editable_value(inp);
    browser_input_replace(browser, inp, 0, cur ? strlen(cur) : 0,
                          val ? val : "");
    gboolean p = FALSE;
    ns_js_dispatch_event(browser->js, inp, "change", &p);
    ns_js_consume_mutated(browser->js);
    g_free(val);
    browser->datalist_suppressed = TRUE;
    browser->dirty = TRUE;
    return TRUE;
}

/* Handles clicks on list-box options, open dropdown options and dropdown toggles. */
static gboolean
browser_dropdown_click(ns_browser *browser, const ns_node *node)
{
    if (browser_datalist_click(browser, node)) return TRUE;
    const ns_node *option = NULL, *select = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!option && ns_node_is_element_named(a, "option")) option = a;
        if (ns_node_is_element_named(a, "select")) { select = a; break; }
    }
    if (select && option && !ns_is_dropdown_select(select) &&
        !ns_element_get_attr(select, "disabled")) {
        gboolean multiple = ns_element_get_attr(select, "multiple") != NULL;
        gboolean toggle = multiple && (browser->press_mods & 2) != 0;
        if (browser->js) {
            if (toggle)
                ns_js_select_toggle_option(browser->js, (ns_node *)option);
            else
                ns_js_select_choose_option(browser->js, (ns_node *)option);
            ns_js_consume_mutated(browser->js);
        }
        browser->dirty = TRUE;
        return TRUE;
    }
    if (browser->open_select && option && select == browser->open_select) {
        if (browser->js &&
            ns_js_select_choose_option(browser->js, (ns_node *)option)) {
            ns_js_consume_mutated(browser->js);
            browser->open_select = NULL;
        }
        browser->dirty = TRUE;
        return TRUE;
    }
    if (select && ns_is_dropdown_select(select) &&
        !ns_element_get_attr(select, "disabled")) {
        browser->open_select =
            (browser->open_select == select) ? NULL : select;
        browser->dirty = TRUE;
        return TRUE;
    }
    if (browser->open_select) {
        browser->open_select = NULL;
        browser->dirty = TRUE;
    }
    return FALSE;
}

/* Pointer up: fires pointerup/mouseup/click, then select, summary, form or link actions. */
char *
ns_browser_release_click(ns_browser *browser, int *out_changed)
{
    if (out_changed) *out_changed = 0;
    if (!browser) {
        if (out_changed) *out_changed = -1;
        return NULL;
    }
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);

    browser_prune_cached_nodes(browser);
    const ns_node *node = browser->press_active ? browser->press_node : NULL;
    int x = browser->press_x;
    int y = browser->press_y;
    int mods = browser->press_mods;
    browser->press_node = NULL;
    browser->press_active = FALSE;
    browser->media_seek_node = NULL;

    gboolean drag_selected = browser->selection_dragged &&
                             ns_selection_has_range(&browser->selection);
    browser->selection_dragged = FALSE;

    gboolean prevented = FALSE;
    ns_js_click_state click_state = {0};
    if (browser->js && node) {
        gboolean sh = (mods & 1) != 0, ct = (mods & 2) != 0;
        gboolean al = (mods & 4) != 0, me = (mods & 8) != 0;
        ns_js_dispatch_mouse_event(browser->js, node, "pointerup",
                                   (double)x - browser->cur_scroll_x,
                                   (double)y - browser->cur_scroll_y,
                                   (double)x, (double)y,
                                   0, 0, sh, ct, al, me, NULL, NULL);
        ns_js_dispatch_mouse_event(browser->js, node, "mouseup",
                                   (double)x - browser->cur_scroll_x,
                                   (double)y - browser->cur_scroll_y,
                                   (double)x, (double)y,
                                   0, 0, sh, ct, al, me, NULL, NULL);
        if (!drag_selected) {
            ns_js_click_begin(browser->js, node, &click_state);
            ns_js_dispatch_mouse_event(browser->js, node, "click",
                                       (double)x - browser->cur_scroll_x,
                                       (double)y - browser->cur_scroll_y,
                                       (double)x, (double)y,
                                       0, 0, sh, ct, al, me, NULL, &prevented);
            if (ns_js_click_end(browser->js, node, &click_state, prevented))
                browser->dirty = TRUE;
        }
        browser_absorb_js_mutation(browser);
    }
    if (drag_selected) prevented = TRUE;

    gboolean select_consumed =
        !prevented && node && browser_dropdown_click(browser, node);

    if (!select_consumed) {
    if (!prevented && node && browser->js &&
        ns_js_activate_summary(browser->js, node)) {
        browser_absorb_js_mutation(browser);
    } else if (!prevented && !browser->pending_nav && node &&
        ns_form_is_submit_trigger(node)) {
        browser_submit_form(browser, node);
    } else if (!prevented && node && browser->js && browser->doc &&
               ns_form_is_reset_trigger(node)) {
        ns_node *form = (ns_node *)ns_form_owner(node, browser->doc);
        if (form) {
            ns_js_form_reset(browser->js, form);
            browser_absorb_js_mutation(browser);
        }
    } else if (!prevented && !browser->pending_nav) {
        const char *href = NULL;
        for (const ns_node *a = node; a && !href; a = a->parent) {
            if (browser_node_is_hyperlink(a)) {
                const char *h = ns_element_get_attr(a, "href");
                if (h && *h) href = h;
            }
        }
        if (!href)
            href = ns_box_hit_link(browser->layout, (double)x, (double)y);
        if (href && *href)
            browser->pending_nav = browser_resolve_navigation(browser, href);
    }
    }

    const ns_node *prev = ns_css_set_active_node(NULL);
    if (prev && ns_render_page_uses_active())
        browser->dirty = TRUE;

    if (browser->js) {
        if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
        browser_absorb_js_mutation(browser);
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        if (out_changed) *out_changed = 1;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

/* Replaces a byte range of a control's value, firing beforeinput (cancellable) and input. */
static void
browser_input_replace(ns_browser *b, ns_node *node, gsize del_start,
                      gsize del_end, const char *insert)
{
    const char *cur = ns_node_editable_value(node);
    if (!cur) return;
    gsize cur_len = strlen(cur);
    if (del_start > cur_len) del_start = cur_len;
    if (del_end > cur_len) del_end = cur_len;
    if (del_end < del_start) del_end = del_start;
    gsize ins_len = insert ? strlen(insert) : 0;

    GString *s = g_string_sized_new(cur_len - (del_end - del_start) + ins_len);
    g_string_append_len(s, cur, (gssize)del_start);
    if (ins_len) g_string_append_len(s, insert, (gssize)ins_len);
    g_string_append_len(s, cur + del_end, (gssize)(cur_len - del_end));

    if (b->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_event(b->js, node, "beforeinput", &prevented);
        if (prevented || ns_js_focused_node(b->js) != node) {
            g_string_free(s, TRUE);
            return;
        }
    }
    ns_node_set_editable_value(node, s->str);
    b->caret_byte = del_start + ins_len;
    b->sel_anchor_byte = b->caret_byte;
    g_string_free(s, TRUE);
    if (b->js) {
        ns_js_dispatch_event(b->js, node, "input", NULL);
        (void)ns_js_consume_mutated(b->js);
    }
}

/* Applies an editing key (Backspace, arrows, Enter, text) to a focused control's value. */
static gboolean
browser_edit_key(ns_browser *b, ns_node *node, const char *key, int mods)
{
    gboolean shift = (mods & 1) != 0, ctrl = (mods & 2) != 0;
    if (mods & (4 | 8)) return FALSE;
    const char *cur = ns_node_editable_value(node);
    if (!cur || !key || !*key) return FALSE;
    gsize cur_len = strlen(cur);
    if (b->caret_byte > cur_len) b->caret_byte = cur_len;
    if (b->sel_anchor_byte > cur_len) b->sel_anchor_byte = cur_len;
    gsize sel_lo = b->sel_anchor_byte < b->caret_byte ? b->sel_anchor_byte
                                                      : b->caret_byte;
    gsize sel_hi = b->sel_anchor_byte < b->caret_byte ? b->caret_byte
                                                      : b->sel_anchor_byte;
    gboolean has_sel = sel_lo != sel_hi;
    gboolean multiline = (node->name && strcmp(node->name, "textarea") == 0) ||
                         ns_node_is_contenteditable_host(node);

    if (ctrl) {
        if ((key[0] == 'a' || key[0] == 'A') && !key[1]) {
            b->sel_anchor_byte = 0;
            b->caret_byte = cur_len;
            return TRUE;
        }
        return FALSE;
    }
    if (strcmp(key, "Backspace") == 0) {
        if (has_sel) browser_input_replace(b, node, sel_lo, sel_hi, NULL);
        else if (b->caret_byte > 0) {
            const char *prev = g_utf8_prev_char(cur + b->caret_byte);
            browser_input_replace(b, node, (gsize)(prev - cur), b->caret_byte,
                                  NULL);
        }
        return TRUE;
    }
    if (strcmp(key, "Delete") == 0) {
        if (has_sel) browser_input_replace(b, node, sel_lo, sel_hi, NULL);
        else if (b->caret_byte < cur_len) {
            const char *nxt = g_utf8_next_char(cur + b->caret_byte);
            browser_input_replace(b, node, b->caret_byte, (gsize)(nxt - cur),
                                  NULL);
        }
        return TRUE;
    }
    if (strcmp(key, "ArrowLeft") == 0) {
        if (has_sel && !shift) b->caret_byte = sel_lo;
        else if (b->caret_byte > 0)
            b->caret_byte = (gsize)(g_utf8_prev_char(cur + b->caret_byte) - cur);
        if (!shift) b->sel_anchor_byte = b->caret_byte;
        return TRUE;
    }
    if (strcmp(key, "ArrowRight") == 0) {
        if (has_sel && !shift) b->caret_byte = sel_hi;
        else if (b->caret_byte < cur_len)
            b->caret_byte = (gsize)(g_utf8_next_char(cur + b->caret_byte) - cur);
        if (!shift) b->sel_anchor_byte = b->caret_byte;
        return TRUE;
    }
    if (strcmp(key, "Home") == 0) {
        b->caret_byte = 0;
        if (!shift) b->sel_anchor_byte = 0;
        return TRUE;
    }
    if (strcmp(key, "End") == 0) {
        b->caret_byte = cur_len;
        if (!shift) b->sel_anchor_byte = cur_len;
        return TRUE;
    }
    if (strcmp(key, "Enter") == 0) {
        if (multiline) {
            browser_input_replace(b, node, sel_lo, sel_hi, "\n");
            return TRUE;
        }
        browser_submit_form(b, node);
        return TRUE;
    }
    if (g_utf8_strlen(key, -1) == 1 &&
        !g_unichar_iscntrl(g_utf8_get_char(key))) {
        browser_input_replace(b, node, sel_lo, sel_hi, key);
        return TRUE;
    }
    return FALSE;
}

/* Code point of a single printable character key, else 0 (for keypress). */
static int
browser_key_char_code(const char *key)
{
    if (!key || !*key || !g_utf8_validate(key, -1, NULL))
        return 0;
    const char *next = g_utf8_next_char(key);
    if (!next || *next)
        return 0;
    gunichar ch = g_utf8_get_char(key);
    return g_unichar_iscntrl(ch) ? 0 : (int)ch;
}

/* TRUE if the element's accesskey token list contains key (case-insensitive). */
static gboolean
browser_accesskey_matches(const ns_node *el, const char *key)
{
    const char *ak = ns_element_get_attr(el, "accesskey");
    if (!ak || !*ak || !key || !*key) return FALSE;
    gsize klen = strlen(key);
    for (const char *p = ak; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        gsize len = (gsize)(p - s);
        if (len == klen && g_ascii_strncasecmp(s, key, len) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Finds the first non-inert element with a matching accesskey outside templates. */
static const ns_node *
browser_find_accesskey(const ns_node *n, const char *key, int depth)
{
    if (!n || depth > 1024) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        g_ascii_strcasecmp(n->name, "template") == 0)
        return NULL;
    if (n->kind == NS_NODE_ELEMENT && browser_accesskey_matches(n, key) &&
        !ns_element_effectively_inert(n))
        return n;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *m = browser_find_accesskey(c, key, depth + 1);
        if (m) return m;
    }
    return NULL;
}

/* Keyboard handling for a focused <select>: arrows, Home/End, open/close, typeahead. */
static gboolean
browser_select_key(ns_browser *browser, ns_node *select, const char *key,
                   int mods)
{
    if (!key || !*key || !browser->js) return FALSE;
    gboolean dropdown = ns_is_dropdown_select(select);
    if (strcmp(key, "ArrowDown") == 0)
        return ns_js_select_step(browser->js, select, +1);
    if (strcmp(key, "ArrowUp") == 0)
        return ns_js_select_step(browser->js, select, -1);
    if (strcmp(key, "Home") == 0)
        return ns_js_select_edge(browser->js, select, FALSE);
    if (strcmp(key, "End") == 0)
        return ns_js_select_edge(browser->js, select, TRUE);
    if (dropdown && (strcmp(key, "Enter") == 0 || strcmp(key, " ") == 0)) {
        browser->open_select = (browser->open_select == select) ? NULL : select;
        return TRUE;
    }
    if (dropdown && strcmp(key, "Escape") == 0 &&
        browser->open_select == select) {
        browser->open_select = NULL;
        return TRUE;
    }
    if ((mods & (2 | 4 | 8)) == 0 && g_utf8_validate(key, -1, NULL) &&
        g_utf8_strlen(key, -1) == 1 && key[0] != ' ' &&
        g_unichar_isprint(g_utf8_get_char(key)))
        return ns_js_select_typeahead(browser->js, select, key);
    return FALSE;
}

/* Dispatches key events and default actions: text input, accesskey, Tab, select, edit. */
char *
ns_browser_key_full(ns_browser *browser, int kind, const char *key,
                    const char *code, int keycode, int mods,
                    int *out_prevented)
{
    if (out_prevented) *out_prevented = 0;
    if (!browser || !browser->js) return NULL;
    ns_js_note_pointer_input(browser->js, FALSE);
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);

    const ns_node *target = ns_js_focused_node(browser->js);
    if (!target && browser->doc)
        target = ns_node_find_first_element(browser->doc, "body");
    if (!target) return NULL;

    if (kind == 2) {
        const ns_node *f = ns_js_focused_node(browser->js);
        if (f && ns_node_editable_value(f) && key && *key &&
            g_utf8_validate(key, -1, NULL)) {
            const char *cur = ns_node_editable_value(f);
            gsize cur_len = cur ? strlen(cur) : 0;
            if (browser->caret_byte > cur_len)
                browser->caret_byte = cur_len;
            if (browser->sel_anchor_byte > cur_len)
                browser->sel_anchor_byte = cur_len;
            gsize lo = browser->sel_anchor_byte < browser->caret_byte
                       ? browser->sel_anchor_byte : browser->caret_byte;
            gsize hi = browser->sel_anchor_byte < browser->caret_byte
                       ? browser->caret_byte : browser->sel_anchor_byte;
            browser_input_replace(browser, (ns_node *)f, lo, hi, key);
            browser->datalist_suppressed = FALSE;
            browser->dirty = TRUE;
        }
    } else {
        gboolean prevented = FALSE;
        ns_js_dispatch_key_event_full(browser->js, target,
                                      kind == 1 ? "keyup" : "keydown",
                                      key ? key : "", code ? code : "",
                                      keycode, 0,
                                      (mods & 1) != 0, (mods & 2) != 0,
                                      (mods & 4) != 0, (mods & 8) != 0,
                                      &prevented);
        if (out_prevented && prevented)
            *out_prevented = 1;
        browser_absorb_js_mutation(browser);

        if (kind == 0 && !prevented && (mods & 4) && !(mods & (2 | 8)) &&
            key && g_utf8_validate(key, -1, NULL) &&
            g_utf8_strlen(key, -1) == 1 && browser->doc) {
            const ns_node *ak = browser_find_accesskey(browser->doc, key, 0);
            if (ak) {
                ns_js_set_focus(browser->js, ak);
                ns_js_activate_element(browser->js, ak);
                browser_absorb_js_mutation(browser);
                if (out_prevented) *out_prevented = 1;
            }
        }

        int char_code = browser_key_char_code(key);
        if (kind == 3 && !prevented && char_code > 0 &&
            (mods & (2 | 4 | 8)) == 0) {
            gboolean press_prevented = FALSE;
            ns_js_dispatch_key_event_full(browser->js, target, "keypress",
                                          key ? key : "", code ? code : "",
                                          keycode, char_code,
                                          (mods & 1) != 0, FALSE, FALSE, FALSE,
                                          &press_prevented);
            if (out_prevented && press_prevented)
                *out_prevented = 1;
            browser_absorb_js_mutation(browser);
        }

        if (!prevented && kind == 0 && key && strcmp(key, "Tab") == 0) {
            gboolean backward = (mods & 1) != 0;
            const ns_node *next =
                ns_js_sequential_focus_target(browser->js, backward);
            if (next) {
                ns_js_set_focus(browser->js, (ns_node *)next);
                const char *val = ns_node_editable_value(next);
                browser->caret_byte = val ? strlen(val) : 0;
                browser->sel_anchor_byte = browser->caret_byte;
                browser->dirty = TRUE;
            }
        } else if (!prevented && kind == 0) {
            const ns_node *f = ns_js_focused_node(browser->js);
            if (f && ns_node_is_element_named(f, "input") &&
                !browser->datalist_suppressed && key &&
                strcmp(key, "Escape") == 0 &&
                ns_element_get_attr(f, "list") != NULL) {
                browser->datalist_suppressed = TRUE;
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_is_element_named(f, "select") &&
                !ns_element_get_attr(f, "disabled") &&
                browser_select_key(browser, (ns_node *)f, key, mods)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && !(mods & (2 | 4 | 8)) &&
                       ns_js_keyboard_activates(f, key)) {
                if (out_prevented) *out_prevented = 1;
            } else if (f && !(mods & (2 | 4 | 8)) &&
                       ns_js_keyboard_activate(browser->js, f, key, FALSE)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_editable_value(f) &&
                browser_edit_key(browser, (ns_node *)f, key, mods)) {
                browser->dirty = TRUE;
            } else if (key && strcmp(key, "Escape") == 0 &&
                       ns_js_process_close_request(browser->js)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            }
        } else if (!prevented && kind == 1) {
            const ns_node *f = ns_js_focused_node(browser->js);
            if (f && !(mods & (2 | 4 | 8)) &&
                ns_js_keyboard_activate(browser->js, f, key, TRUE))
                browser->dirty = TRUE;
        }
    }

    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    browser_absorb_js_mutation(browser);
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

/* 1 if the focused node has an editable value. */
static int
ns_browser_focused_editable(ns_browser *browser)
{
    if (!browser || !browser->js)
        return 0;
    const ns_node *f = ns_js_focused_node(browser->js);
    return f && ns_node_editable_value(f) ? 1 : 0;
}

/* Updates caret blink phase (530ms) for the focused field; 1 if repaint is needed. */
int
ns_browser_set_caret_blink_active(ns_browser *browser, int active)
{
    if (!browser) return 0;
    const ns_node *focused = active && ns_browser_focused_editable(browser)
                           ? ns_js_focused_node(browser->js) : NULL;
    gsize caret = focused ? browser->caret_byte : 0;
    gsize anchor = focused ? browser->sel_anchor_byte : 0;
    gint64 now = g_get_monotonic_time();
    if (focused != browser->caret_blink_node)
        browser->damage_full = TRUE;
    if (focused != browser->caret_blink_node ||
        caret != browser->caret_blink_byte ||
        anchor != browser->caret_blink_anchor) {
        browser->caret_blink_epoch_us = now;
        browser->caret_blink_node = focused;
        browser->caret_blink_byte = caret;
        browser->caret_blink_anchor = anchor;
    }
    gboolean visible = focused &&
        ((now - browser->caret_blink_epoch_us) / NS_CARET_BLINK_US) % 2 == 0;
    gboolean changed = browser->caret_blink_active != (focused != NULL) ||
                       browser->caret_paint_visible != visible;
    browser->caret_blink_active = focused != NULL;
    browser->caret_paint_visible = visible;
    if (!focused) browser->caret_blink_epoch_us = 0;
    if (changed && focused) browser_damage_node(browser, focused);
    return changed ? 1 : 0;
}

int
ns_browser_caret_blinking(ns_browser *browser)
{
    return browser && browser->caret_blink_active ? 1 : 0;
}

/* Document title with whitespace collapsed; malloc'd, NULL if empty. */
char *
ns_browser_title(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    ns_node *title = ns_node_find_first_element(browser->doc, "title");
    if (!title) return NULL;
    char *raw = ns_node_collect_text(title);
    if (!raw) return NULL;

    GString *out = g_string_new(NULL);
    gboolean prev_ws = TRUE;
    for (const char *p = raw; *p; p++) {
        gboolean ws = (*p == ' ' || *p == '\t' || *p == '\n' ||
                       *p == '\r' || *p == '\f');
        if (ws) {
            if (!prev_ws) g_string_append_c(out, ' ');
            prev_ws = TRUE;
        } else {
            g_string_append_c(out, *p);
            prev_ws = FALSE;
        }
    }
    if (out->len > 0 && out->str[out->len - 1] == ' ')
        g_string_set_size(out, out->len - 1);
    g_free(raw);

    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    char *result = strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

char *
ns_browser_url(ns_browser *browser)
{
    if (!browser || !browser->base_url) return NULL;
    return strdup(browser->base_url);
}

/* Returns 1 once after script pushed a new history URL. */
int
ns_browser_take_url_pushed(ns_browser *browser)
{
    if (!browser || !browser->url_pushed) return 0;
    browser->url_pushed = FALSE;
    return 1;
}

int
ns_browser_security(ns_browser *browser, const char **out_ip)
{
    if (out_ip)
        *out_ip = browser ? browser->remote_ip : NULL;
    return browser ? browser->security : NS_SEC_NONE;
}

/* Takes the pending navigation's initiator URL; malloc'd, caller frees. */
char *
ns_browser_take_pending_initiator(ns_browser *browser)
{
    if (!browser || !browser->pending_nav_initiator) return NULL;
    char *out = strdup(browser->pending_nav_initiator);
    g_clear_pointer(&browser->pending_nav_initiator, g_free);
    return out;
}

/* Takes the pending navigation URL; malloc'd, caller frees. */
char *
ns_browser_take_pending_nav(ns_browser *browser)
{
    if (!browser || !browser->pending_nav) return NULL;
    char *out = strdup(browser->pending_nav);
    g_clear_pointer(&browser->pending_nav, g_free);
    return out;
}

/* Takes a pending scroll request; returns 1 if there was one. */
int
ns_browser_take_pending_scroll(ns_browser *browser, int *out_scroll_x,
                               int *out_scroll_y)
{
    if (!browser || !browser->pending_scroll) return 0;
    if (out_scroll_x) *out_scroll_x = browser->pending_scroll_x;
    if (out_scroll_y) *out_scroll_y = browser->pending_scroll_y;
    browser->pending_scroll_x = -1;
    browser->pending_scroll = FALSE;
    return 1;
}

char *
ns_browser_take_pending_download(ns_browser *browser)
{
    if (!browser || !browser->pending_download) return NULL;
    char *out = browser->pending_download;
    browser->pending_download = NULL;
    return out;
}

int
ns_browser_has_pending_clipboard(ns_browser *browser)
{
    return browser && browser->pending_clipboard ? 1 : 0;
}

char *
ns_browser_take_pending_clipboard(ns_browser *browser)
{
    if (!browser || !browser->pending_clipboard) return NULL;
    char *out = browser->pending_clipboard;
    browser->pending_clipboard = NULL;
    return out;
}

char *
ns_browser_take_pending_camera(ns_browser *browser)
{
    (void)browser;
    return ns_camera_take_pending_origin();
}

/* Records the user's camera decision for an origin and resolves pending page requests. */
void
ns_browser_resolve_camera(ns_browser *browser, const char *origin, int allow)
{
    ns_camera_set_decision(origin, allow);
    if (browser && browser->js) {
        char *r = ns_js_eval_source(browser->js,
            allow ? "__nd_camera_resolve_pending(true)"
                  : "__nd_camera_resolve_pending(false)",
            "camera-resolve");
        free(r);
    }
}

int
ns_browser_bfcache_eligible(ns_browser *browser)
{
    return browser && browser->bfcache_ok;
}

/* Fires pagehide (persisted) and suspends media before caching the page. */
void
ns_browser_bfcache_park(ns_browser *browser)
{
    if (!browser) return;
    if (browser->js) {
        ns_js_fire_page_transition(browser->js, "pagehide", TRUE);
        ns_js_suspend_media(browser->js);
    }
}

/* Restores a cached page: applies the viewport and fires pageshow (persisted). */
void
ns_browser_bfcache_restore(ns_browser *browser, int viewport_width,
                           double viewport_height)
{
    if (!browser) return;
    if (viewport_width > 0 && viewport_height > 0.0)
        ns_browser_set_viewport(browser, viewport_width, viewport_height);
    if (browser->js)
        ns_js_fire_page_transition(browser->js, "pageshow", TRUE);
}

void
ns_browser_close(ns_browser *browser)
{
    if (!browser) return;
    if (browser->media_events_source)
        ns_engine_source_remove(browser->media_events_source);
    if (browser->img_sessions) {
        for (guint i = 0; i < browser->img_sessions->len; i++)
            ns_engine_img_session_close(
                g_ptr_array_index(browser->img_sessions, i));
        g_ptr_array_free(browser->img_sessions, TRUE);
    }
    if (browser->img_requested) g_hash_table_destroy(browser->img_requested);
    if (browser->damage_nodes) g_hash_table_destroy(browser->damage_nodes);
    if (browser->damage_rects) g_array_free(browser->damage_rects, TRUE);
    if (browser->damage_images) g_hash_table_destroy(browser->damage_images);
    ns_paint_hazards_clear(&browser->hazards);
    ns_css_set_active_node(NULL);
    ns_paint_set_anim(NULL);
    if (browser->js) {
        ns_js_set_layout_root(browser->js, NULL);
        ns_js_set_style_table(browser->js, NULL);
    }
    if (browser->anim) ns_anim_free(browser->anim);
    if (browser->layout) { ns_paint_3d_invalidate(); ns_box_free(browser->layout); }
    if (browser->layout_styles) g_hash_table_destroy(browser->layout_styles);
    if (browser->styles) g_hash_table_destroy(browser->styles);
    if (browser->css_cache) g_hash_table_destroy(browser->css_cache);
    if (browser->js) ns_js_free(browser->js);
    if (browser->doc) ns_node_free(browser->doc);
    if (browser->images) ns_image_cache_free(browser->images);
    g_free(browser->base_url);
    g_free(browser->layout_base_href);
    g_free(browser->doc_charset);
    g_free(browser->doc_language);
    g_free(browser->pending_nav);
    g_free(browser->pending_nav_initiator);
    g_free(browser->pending_download);
    g_free(browser->pending_clipboard);
    g_free(browser->refresh_url);
    g_free(browser->pending_post_body);
    g_free(browser->pending_post_ct);
    g_free(browser->search_query);
    g_free(browser->remote_ip);
    if (browser->console_buf) g_string_free(browser->console_buf, TRUE);
    g_free(browser);
}
