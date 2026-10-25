/* Northstar Web Browser — headless engine driver: load a URL, run actions/eval, dump the result.
 * Text/DOM/layout/PNG/PDF dumps, box inspection, simulated input and WPT result collection.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "headless.h"
#include <cairo.h>

#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "anim.h"
#include "cache.h"
#include "css.h"
#include "debuglog.h"
#include "dom.h"
#include "engine.h"
#include "print.h"
#include "render.h"
#include "forms.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "paint.h"
#include "page_session.h"
#include "wpt_hook.h"

static char *g_headless_doc_charset;

static gboolean
settle_quit_cb(gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

/* Parses a comma list of debug-log level names (or "all") into a level bitmask. */
unsigned
ns_headless_debug_mask(const char *spec)
{
    if (!spec || !*spec) return 0;
    if (g_ascii_strcasecmp(spec, "all") == 0)
        return 0xFFFFFFFFu;
    unsigned mask = 0;
    char **toks = g_strsplit(spec, ",", -1);
    for (int i = 0; toks[i]; i++) {
        char *t = g_strstrip(toks[i]);
        for (int lvl = NS_DLOG_INFO; lvl <= NS_DLOG_JS; lvl++)
            if (g_ascii_strcasecmp(t, ns_dlog_level_name(lvl)) == 0)
                mask |= (1u << lvl);
    }
    g_strfreev(toks);
    return mask;
}

/* Prints debug-log entries whose level is in the subscribed mask to stderr. */
static void
headless_dlog_listener(const ns_dlog_entry *e, gpointer user_data)
{
    unsigned mask = GPOINTER_TO_UINT(user_data);
    if (!e || !(mask & (1u << e->level))) return;
    fprintf(stderr, "[%s %s] %s\n", ns_dlog_level_name(e->level),
            e->category ? e->category : "", e->message ? e->message : "");
}

static ns_print_setup g_headless_print_setup;

/* Writes the layout as paged print PDF, plain PDF or PNG depending on the dump kind. */
static int
write_capture(const ns_box *root, const char *path, ns_headless_dump kind)
{
    if (kind == NS_DUMP_PRINT)
        return ns_engine_write_pdf_paged(root, path, &g_headless_print_setup);
    if (kind == NS_DUMP_PDF) return ns_engine_write_pdf(root, path);
    return ns_engine_write_png(root, path);
}

static void
headless_js_log(const char *line, gpointer user_data)
{
    (void)user_data;
    fprintf(stderr, "[js] %s\n", line);
    fflush(stderr);
}

static gboolean g_headless_layout_dirty;
static gboolean g_headless_styles_stale;

static void
headless_js_mutated(gpointer user_data) { (void)user_data; g_headless_layout_dirty = TRUE; }

typedef struct headless_nav_capture {
    ns_js *js;
    char *pending_url;
    char *pending_post_body;
    gsize pending_post_len;
    char *pending_post_ct;
    gboolean user_activated;
} headless_nav_capture;

static void
headless_nav_capture_clear_post(headless_nav_capture *cap)
{
    g_free(cap->pending_post_body);
    cap->pending_post_body = NULL;
    cap->pending_post_len = 0;
    g_free(cap->pending_post_ct);
    cap->pending_post_ct = NULL;
}

/* JS navigation hook: records the requested URL for the headless driver to follow. */
static void
headless_js_navigate(const char *url, gboolean reload, gpointer user_data)
{
    (void)reload;
    headless_nav_capture *cap = user_data;
    if (cap && url && *url) {
        g_free(cap->pending_url);
        cap->pending_url = g_strdup(url);
    }
}

typedef enum headless_reveal_kind {
    HEADLESS_REVEAL_HIDDEN,
    HEADLESS_REVEAL_DETAILS,
} headless_reveal_kind;

typedef struct headless_reveal_item {
    ns_node *node;
    headless_reveal_kind kind;
} headless_reveal_item;

static void
headless_reveal_add(GArray *items, ns_node *node, headless_reveal_kind kind)
{
    headless_reveal_item item = { node, kind };
    g_array_append_val(items, item);
}

/* Reveals a fragment target by clearing hidden=until-found and opening enclosing details. */
static void
headless_reveal_fragment(ns_node *doc, const char *frag)
{
    ns_node *target = ns_node_find_fragment_target(doc, frag);
    if (!target) return;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(headless_reveal_item));
    for (ns_node *cur = target; cur; cur = cur->parent) {
        if (ns_element_hidden_until_found(cur))
            headless_reveal_add(items, cur, HEADLESS_REVEAL_HIDDEN);
        if (cur->parent && ns_details_fragment_needs_open(cur->parent, cur))
            headless_reveal_add(items, cur->parent, HEADLESS_REVEAL_DETAILS);
        if (cur == doc) break;
    }
    for (guint i = 0; i < items->len; i++) {
        headless_reveal_item item =
            g_array_index(items, headless_reveal_item, i);
        ns_node *el = item.node;
        if (ns_node_root(el) != doc) break;
        if (item.kind == HEADLESS_REVEAL_HIDDEN) {
            if (ns_element_hidden_until_found(el))
                ns_element_remove_attr(el, "hidden");
        } else if (!ns_element_get_attr(el, "open")) {
            ns_element_set_attr(el, "open", "");
        }
    }
    g_array_free(items, TRUE);
}

/* Serializes a form submission into a pending GET URL or POST body on the nav capture. */
static void
headless_js_form_submit(const ns_node *form, const ns_node *submitter,
                        gpointer user_data)
{
    headless_nav_capture *cap = user_data;
    if (!cap || !form) return;
    const char *method = ns_element_get_attr(form, "method");
    gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;
    const char *action = ns_element_get_attr(form, "action");
    if (!action) action = "";
    GString *q = g_string_new(NULL);
    gboolean first = TRUE;
    const ns_node *doc = ns_node_root(form);
    const ns_node *root = doc ? doc : form;
    const char *accept_charset = ns_element_get_attr(form, "accept-charset");
    ns_form_set_submission_charset(
        (accept_charset && *accept_charset) ? accept_charset
                                            : g_headless_doc_charset);
    const ns_node *trigger = submitter != form ? submitter : NULL;
    if (!cap->js || !ns_js_form_entry_list(cap->js, form, trigger, q, &first))
        ns_form_collect_inputs(form, root, root, q, &first, trigger);
    ns_form_set_submission_charset(NULL);
    headless_nav_capture_clear_post(cap);
    g_free(cap->pending_url);
    if (is_post) {
        cap->pending_url = g_strdup(action);
        cap->pending_post_len = q->len;
        cap->pending_post_body = g_string_free(q, FALSE);
        cap->pending_post_ct = g_strdup("application/x-www-form-urlencoded");
        return;
    }
    char *url;
    if (q->len > 0) {
        const char *sep = strchr(action, '?') ? "&" : "?";
        url = g_strdup_printf("%s%s%s", action, sep, q->str);
    } else {
        url = g_strdup(action);
    }
    g_string_free(q, TRUE);
    cap->pending_url = url;
}

static int ns_headless_run_one(const ns_headless_opts *opts,
                               const char *fetch_url, int hop,
                               const char *post_body, gsize post_len,
                               const char *post_ct, const char *top_url,
                               gboolean user_activated);

typedef struct {
    const char *name;
    const char *jskey;
    int         code;
} rdrv_keymap;

static const rdrv_keymap rdrv_keys[] = {
    {"Enter", "Enter", 13}, {"Return", "Enter", 13},
    {"Backspace", "Backspace", 8}, {"Delete", "Delete", 46},
    {"Tab", "Tab", 9}, {"Escape", "Escape", 27},
    {"Left", "ArrowLeft", 37}, {"Right", "ArrowRight", 39},
    {"Up", "ArrowUp", 38}, {"Down", "ArrowDown", 40},
    {"Home", "Home", 36}, {"End", "End", 35},
};

/* Copies the page session's buffered console output to stderr. */
static void
rdrv_drain_console(ns_page_session *s)
{
    char *log = ns_page_session_console(s);
    if (log && *log) {
        fputs(log, stderr);
        if (log[strlen(log) - 1] != '\n') fputc('\n', stderr);
    }
    free(log);
}

/* Follows up to six chained session navigations; frees href and always returns NULL. */
static char *
rdrv_follow_nav(ns_page_session *s, char *href, int vw, int vh, int settle_ms,
                gboolean user_activated)
{
    int hops = 0;
    while (href && *href && hops < 6) {
        ns_page_info pg;
        if (ns_page_session_open(s, href, vw, vh, settle_ms, 0,
                                  user_activated, &pg) != 0) {
            ns_page_info_clear(&pg);
            break;
        }
        fprintf(stderr, "[headless] open -> %s\n", href);
        g_free(href);
        href = pg.nav ? g_strdup(pg.nav) : NULL;
        ns_page_info_clear(&pg);
        user_activated = FALSE;
        hops++;
    }
    g_free(href);
    return NULL;
}

/* Cairo PNG write callback appending bytes to a GByteArray. */
static cairo_status_t
rdrv_png_sink(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

/* Renders one session frame and returns any pending navigation URL; caller frees. */
static char *
rdrv_tick_take_nav(ns_page_session *s, int vw, int vh)
{
    ns_page_frame fr;
    if (ns_page_session_render(s, vw, vh, 0, 0, 1.0, 0, &fr) != 0) return NULL;
    char *nav = (fr.nav && *fr.nav) ? g_strdup(fr.nav) : NULL;
    ns_page_frame_clear(&fr);
    return nav;
}

/* Parses "X,Y" or "sel=CSS [dx=] [dy=]" into page coordinates via the element's center. */
static gboolean
rdrv_point(ns_page_session *s, const char *arg, double *x, double *y)
{
    if (sscanf(arg, "%lf , %lf", x, y) == 2) return TRUE;
    if (!g_str_has_prefix(arg, "sel=")) return FALSE;
    char *sel = g_strdup(arg + 4);
    double dx = 0, dy = 0;
    char *sp = strstr(sel, " dx=");
    char *sq = strstr(sel, " dy=");
    if (sp) dx = g_ascii_strtod(sp + 4, NULL);
    if (sq) dy = g_ascii_strtod(sq + 4, NULL);
    if (sp) *sp = 0;
    if (sq) *sq = 0;
    char *quoted = g_strescape(sel, NULL);
    char *src = g_strdup_printf(
        "(function(){var e=document.querySelector(\"%s\");if(!e)return '';"
        "var r=e.getBoundingClientRect();"
        "return (r.left+r.width/2)+','+(r.top+r.height/2);})()", quoted);
    char *res = ns_page_session_eval(s, src);
    gboolean ok = res && sscanf(res, "%lf , %lf", x, y) == 2;
    if (ok) { *x += dx; *y += dy; }
    else fprintf(stderr, "[headless] no element for %s\n", sel);
    g_free(res); g_free(src); g_free(quoted); g_free(sel);
    return ok;
}

/* Runs a ';'-separated action script (click, move, type, key, shot, wait...) on a session. */
static void
rdrv_run_actions(ns_page_session *s, const char *spec, int vw, int vh,
                 int settle_ms)
{
    char **acts = g_strsplit(spec, ";", -1);
    for (int i = 0; acts[i]; i++) {
        char *a = g_strstrip(acts[i]);
        if (!*a) continue;
        if (g_str_has_prefix(a, "click ")) {
            double x = 0, y = 0;
            if (!rdrv_point(s, a + 6, &x, &y)) continue;
            fprintf(stderr, "[headless] click %g,%g\n", x, y);
            char *h = ns_page_session_click(s, (int)x, (int)y, 0);
            g_free(h);
            int changed = 0;
            char *href = ns_page_session_release(s, &changed);
            if (href && *href)
                rdrv_follow_nav(s, href, vw, vh, settle_ms, TRUE);
            else
                g_free(href);
        } else if (g_str_has_prefix(a, "move ")) {
            double x = 0, y = 0;
            if (!rdrv_point(s, a + 5, &x, &y)) continue;
            fprintf(stderr, "[headless] move %g,%g\n", x, y);
            ns_page_session_hover(s, (int)x, (int)y, NULL, NULL);
        } else if (g_str_has_prefix(a, "down ")) {
            double x = 0, y = 0;
            if (!rdrv_point(s, a + 5, &x, &y)) continue;
            fprintf(stderr, "[headless] down %g,%g\n", x, y);
            g_free(ns_page_session_click(s, (int)x, (int)y, 0));
        } else if (g_strcmp0(a, "up") == 0) {
            fprintf(stderr, "[headless] up\n");
            int changed = 0;
            g_free(ns_page_session_release(s, &changed));
        } else if (g_str_has_prefix(a, "select ")) {
            int kind = 0;
            double x = 0, y = 0;
            if (sscanf(a + 7, "%d %lf , %lf", &kind, &x, &y) != 3) continue;
            char *text = ns_page_session_select(s, kind, (int)x, (int)y);
            if (kind == 4)
                fprintf(stdout, "act-select: %s\n", text ? text : "");
            g_free(text);
        } else if (g_str_has_prefix(a, "rightclick ")) {
            double x = 0, y = 0;
            if (!rdrv_point(s, a + 11, &x, &y)) continue;
            int prevented = 0;
            prevented = ns_page_session_contextmenu(s, (int)x, (int)y);
            fprintf(stderr, "[headless] rightclick %g,%g prevented=%d\n",
                    x, y, prevented);
        } else if (g_str_has_prefix(a, "type ")) {
            const char *text = a + 5;
            fprintf(stderr, "[headless] type \"%s\"\n", text);
            char *h = ns_page_session_key(s, 2, text, "", 0, 0, NULL);
            g_free(h);
        } else if (g_str_has_prefix(a, "key ")) {
            const char *name = g_strstrip(a + 4);
            fprintf(stderr, "[headless] key %s\n", name);
            const char *jskey = name;
            int code = 0;
            for (gsize k = 0; k < G_N_ELEMENTS(rdrv_keys); k++)
                if (g_ascii_strcasecmp(name, rdrv_keys[k].name) == 0) {
                    jskey = rdrv_keys[k].jskey;
                    code = rdrv_keys[k].code;
                    break;
                }
            char *h = ns_page_session_key(s, 0, jskey, jskey, code, 0, NULL);
            if (h && *h)
                rdrv_follow_nav(s, g_strdup(h), vw, vh, settle_ms, TRUE);
            g_free(h);
            char *hu = ns_page_session_key(s, 1, jskey, jskey, code, 0, NULL);
            g_free(hu);
        } else if (g_str_has_prefix(a, "eval ")) {
            char *res = ns_page_session_eval(s, a + 5);
            fprintf(stdout, "act-eval: %s\n", res ? res : "(null)");
            free(res);
            char *nav = rdrv_tick_take_nav(s, vw, vh);
            if (nav) rdrv_follow_nav(s, nav, vw, vh, settle_ms, FALSE);
        } else if (g_str_has_prefix(a, "evalfile ")) {
            char *src = NULL;
            if (g_file_get_contents(g_strstrip(a + 9), &src, NULL, NULL)) {
                char *res = ns_page_session_eval(s, src);
                fprintf(stdout, "act-eval: %s\n", res ? res : "(null)");
                free(res);
                g_free(src);
                char *nav = rdrv_tick_take_nav(s, vw, vh);
                if (nav) rdrv_follow_nav(s, nav, vw, vh, settle_ms, FALSE);
            } else {
                fprintf(stderr, "[headless] evalfile: cannot read %s\n", a + 9);
            }
        } else if (g_str_has_prefix(a, "shot ")) {
            const char *path = g_strstrip((char *)a + 5);
            ns_page_frame fr;
            char *inv = ns_page_session_eval(s, "0");
            free(inv);
            int shot_rc = *path ? ns_page_session_render(s, vw, vh, 0, 0, 1.0, 0, &fr)
                                : -2;
            if (shot_rc == 0) {
                if (fr.ok && fr.pixels && fr.width > 0 && fr.height > 0) {
                    cairo_surface_t *surf = cairo_image_surface_create_for_data(
                        (unsigned char *)fr.pixels, CAIRO_FORMAT_ARGB32,
                        fr.width, fr.height, fr.stride);
                    if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                        GByteArray *buf = g_byte_array_new();
                        cairo_surface_write_to_png_stream(surf,
                                                          rdrv_png_sink, buf);
                        char *b64 = g_base64_encode(buf->data, buf->len);
                        fprintf(stdout, "shot-b64:%s:%s\n", path, b64);
                        fflush(stdout);
                        g_free(b64);
                        g_byte_array_free(buf, TRUE);
                    }
                    cairo_surface_destroy(surf);
                    fprintf(stderr, "[headless] shot %dx%d emitted\n",
                            fr.width, fr.height);
                }
                ns_page_frame_clear(&fr);
            }
        } else if (g_str_has_prefix(a, "viewport ")) {
            int nw = 0, nh = 0;
            if (sscanf(a + 9, "%d %d", &nw, &nh) == 2 && nw > 0 && nh > 0) {
                fprintf(stderr, "[headless] viewport %dx%d\n", nw, nh);
                vw = nw;
                vh = nh;
                ns_page_session_set_viewport(s, vw, vh, NULL);
                ns_page_frame fr;
                if (ns_page_session_render(s, vw, vh, 0, 0, 1.0, 0, &fr) == 0) {
                    ns_page_frame_clear(&fr);
                    }
            }
        } else if (g_str_has_prefix(a, "wait ")) {
            gint64 ms = g_ascii_strtoll(a + 5, NULL, 10);
            fprintf(stderr, "[headless] wait %" G_GINT64_FORMAT "ms\n", ms);
            gint64 end = g_get_monotonic_time() + ms * 1000;
            while (g_get_monotonic_time() < end) {
                ns_page_frame fr;
                if (ns_page_session_render(s, vw, vh, 0, 0, 1.0, 0, &fr) == 0) {
                    char *nav = (fr.nav && *fr.nav) ? g_strdup(fr.nav) : NULL;
                    ns_page_frame_clear(&fr);
                    if (nav) {
                        rdrv_follow_nav(s, nav, vw, vh, settle_ms, FALSE);
                        continue;
                    }
                    }
                g_usleep(33000);
            }
        }
    }
    g_strfreev(acts);
}

/* Session-based headless run: open, follow navs, actions, eval, then dump. */
static int
ns_headless_run_session(const ns_headless_opts *o)
{
    int vw = o->viewport_width > 0 ? o->viewport_width : 1000;
    int vh = o->viewport_height > 0 ? o->viewport_height
                                    : (int)((double)vw * 0.75);
    ns_page_session *s = ns_page_session_new(vw, vh);
    if (!s)
        return 2;

    ns_page_info pg;
    if (ns_page_session_open(s, o->url, vw, vh, o->settle_ms, 0, 0,
                             &pg) != 0) {
        ns_page_info_clear(&pg);
        ns_page_session_free(s);
        return 2;
    }
    char *nav = pg.nav ? g_strdup(pg.nav) : NULL;
    ns_page_info_clear(&pg);
    if (nav)
        rdrv_follow_nav(s, nav, vw, vh, o->settle_ms, FALSE);
    rdrv_drain_console(s);

    if (o->actions && *o->actions) {
        rdrv_run_actions(s, o->actions, vw, vh, o->settle_ms);
        rdrv_drain_console(s);
    }

    if (o->eval && *o->eval) {
        char *res = ns_page_session_eval(s, o->eval);
        if (res) {
            fprintf(stdout, "eval: %s\n", res);
            g_free(res);
        }
        rdrv_drain_console(s);
    }

    const char *kind = NULL;
    if (o->dump == NS_DUMP_TEXT)        kind = "text";
    else if (o->dump == NS_DUMP_DOM)    kind = "dom";
    else if (o->dump == NS_DUMP_LAYOUT) kind = "layout";
    if (kind) {
        char *d = ns_page_session_dump(s, kind);
        if (d) {
            fwrite(d, 1, strlen(d), stdout);
            g_free(d);
        }
    }

    ns_page_session_free(s);
    return 0;
}

/* Returns TRUE when options can use the session path rather than the legacy in-process run. */
static gboolean
ns_headless_renderer_capable(const ns_headless_opts *opts)
{
    if (g_getenv("NS_HEADLESS_LEGACY")) return FALSE;
    if (opts->wpt) return FALSE;
    if (opts->inspect && *opts->inspect) return FALSE;
    if (opts->inspect_at && *opts->inspect_at) return FALSE;
    if (opts->dump == NS_DUMP_PNG || opts->dump == NS_DUMP_PDF ||
            opts->dump == NS_DUMP_PRINT) return FALSE;
    return TRUE;
}

/* Headless entry point: validates options, wires debug logging and picks the run path. */
int
ns_headless_run(const ns_headless_opts *opts)
{
    if (!opts || !opts->url || !*opts->url) {
        fprintf(stderr, "headless: --url is required\n");
        return 2;
    }
#ifdef G_OS_WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    guint dlog_sub = 0;
    if (opts->debug_levels)
        dlog_sub = ns_debug_log_subscribe(headless_dlog_listener,
                                          GUINT_TO_POINTER(opts->debug_levels));
    int rc = ns_headless_renderer_capable(opts)
             ? ns_headless_run_session(opts)
             : ns_headless_run_one(opts, opts->url, 0, NULL, 0, NULL,
                                   NULL, FALSE);
    if (dlog_sub) ns_debug_log_unsubscribe(dlog_sub);
    return rc;
}

typedef struct headless_flush_ctx {
    ns_node           *doc;
    ns_js             *js;
    const char        *base;
    int                vw;
    double             vh;
    ns_image_cache    *image_cache;
    ns_anim           *anim;
    GHashTable        *css_cache;
    GHashTable       **styles;
    ns_box           **layout;
    const ns_node     *focused;
    const ns_node     *hover;
    gsize              caret;
    gsize              anchor;
} headless_flush_ctx;

/* Returns the JS-focused node, falling back to the driver's own focus tracking. */
static const ns_node *
headless_focus(const headless_flush_ctx *c)
{
    const ns_node *focused = c->js ? ns_js_focused_node(c->js) : NULL;
    return focused ? focused : c->focused;
}

/* Discards and rebuilds styles and layout for the document, updating the JS bindings. */
static void
headless_relayout(headless_flush_ctx *c)
{
    if (!c) return;
    if (g_getenv("NS_ANIM_DEBUG")) g_printerr("[anim] headless_relayout\n");
    if (c->js && *c->layout) ns_js_set_layout_root(c->js, NULL);
    if (*c->layout) { ns_paint_3d_invalidate(); ns_box_free(*c->layout); *c->layout = NULL; }
    if (c->js && *c->styles) ns_js_set_style_table(c->js, NULL);
    if (*c->styles) { g_hash_table_destroy(*c->styles); *c->styles = NULL; }

    *c->styles = ns_engine_relayout(c->doc, c->base, c->vw, c->vh,
                                    c->image_cache, c->anim, c->js,
                                    c->css_cache, headless_focus(c), c->hover,
                                    c->caret, c->anchor, c->layout);
    g_headless_styles_stale = FALSE;
}

/* JS layout-flush hook: relayouts only when DOM mutations or a dirty flag require it. */
static void
headless_flush_layout(gpointer ud)
{
    headless_flush_ctx *c = ud;
    if (!c || !c->js) return;
    gboolean mutated = ns_js_consume_mutated(c->js);
    gboolean dirty = !c->layout || !*c->layout || mutated || g_headless_layout_dirty;
    if (!dirty) return;
    g_headless_layout_dirty = FALSE;
    headless_relayout(c);
}

/* Returns TRUE if any computed style has a non-normal container-type. */
static gboolean
headless_styles_have_containers(GHashTable *styles)
{
    if (!styles) return FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const ns_style *st = val;
        const ns_css_value *ct = st ? st->values[NS_CSS_CONTAINER_TYPE] : NULL;
        if (ct && ct->kind == NS_CSS_V_KEYWORD && ct->u.keyword &&
            strcmp(ct->u.keyword, "normal") != 0)
            return TRUE;
    }
    return FALSE;
}

/* JS style-flush hook: recomputes the cascade, relayouting if container queries exist. */
static void
headless_flush_style(gpointer ud)
{
    headless_flush_ctx *c = ud;
    if (!c || !c->js) return;
    if (ns_js_consume_mutated(c->js)) {
        g_headless_layout_dirty = TRUE;
        g_headless_styles_stale = TRUE;
    }
    if (*c->styles && !g_headless_styles_stale) return;
    if (!*c->layout) {
        headless_flush_layout(ud);
        return;
    }
    if (*c->styles) {
        ns_js_set_style_table(c->js, NULL);
        g_hash_table_destroy(*c->styles);
        *c->styles = NULL;
    }
    const ns_node *prev_focus = ns_css_set_focus_node(headless_focus(c));
    *c->styles = ns_engine_compute_cascade(c->doc, c->base, c->css_cache, c->anim);
    ns_css_set_focus_node(prev_focus);
    if (c->anim)
        ns_engine_anim_observe(c->anim, *c->styles, g_get_monotonic_time());
    ns_js_set_style_table(c->js, *c->styles);
    g_headless_styles_stale = FALSE;
    if (headless_styles_have_containers(*c->styles)) {
        g_headless_layout_dirty = TRUE;
        headless_relayout(c);
    }
}

typedef struct {
    headless_flush_ctx *fc;
    gint64              last_flush_us;
    gboolean            pending_mutation;
} settle_state;

/* 16ms tick: advances images, animations and rAF; relayouts on mutation at most every 200ms. */
static gboolean
settle_raf_tick(gpointer user_data)
{
    settle_state *s = user_data;
    headless_flush_ctx *fc = s->fc;
    gint64 now = g_get_monotonic_time();
    if (fc->image_cache) ns_image_cache_tick(fc->image_cache, now);
    if (fc->anim && ns_anim_tick(fc->anim, now)) {
        g_headless_styles_stale = TRUE;
        if (ns_anim_needs_layout(fc->anim)) g_headless_layout_dirty = TRUE;
    }
    if (fc->anim && fc->js) ns_js_dispatch_anim_events(fc->js, fc->anim);
    if (fc->js) ns_js_run_animation_frame(fc->js);
    if (fc->js && ns_js_consume_mutated(fc->js)) {
        s->pending_mutation = TRUE;
        g_headless_styles_stale = TRUE;
    }
    if (g_headless_layout_dirty) s->pending_mutation = TRUE;
    if (s->pending_mutation && now - s->last_flush_us >= 200000) {
        g_headless_layout_dirty = FALSE;
        headless_relayout(fc);
        s->pending_mutation = FALSE;
        s->last_flush_us = g_get_monotonic_time();
    }
    return G_SOURCE_CONTINUE;
}

/* Runs the main loop for ms milliseconds with animation-frame ticks. */
static void
settle_main_loop(int ms, headless_flush_ctx *fc)
{
    if (ms <= 0 || !fc) return;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(ms, settle_quit_cb, loop);
    settle_state st = { .fc = fc, .last_flush_us = g_get_monotonic_time() };
    guint raf_id = g_timeout_add(16, settle_raf_tick, &st);
    g_main_loop_run(loop);
    g_source_remove(raf_id);
    g_main_loop_unref(loop);
}

static const char *const ns_wpt_poll_js =
    "(function () {"
    "    var g = globalThis;"
    "    if (g.__ns_wpt_done) return \"1\";"
    "    if (g.__ns_wpt_installed && !g.__ns_wpt_seen_harness &&"
    "        typeof g.add_completion_callback === \"function\") {"
    "        try {"
    "            g.add_completion_callback(g.__ns_wpt_oncomplete);"
    "            g.__ns_wpt_seen_harness = true;"
    "        } catch (e) {}"
    "    }"
    "    return \"0\";"
    "})()";

/* Returns TRUE once the injected WPT hook reports that testharness has finished. */
static gboolean
wpt_results_ready(ns_js *js)
{
    char *r = ns_js_eval_source(js, ns_wpt_poll_js, "wpt-poll");
    gboolean done = r && strcmp(r, "1") == 0;
    g_free(r);
    return done;
}

typedef struct wpt_wait_state {
    GMainLoop          *loop;
    headless_flush_ctx *fc;
    gboolean            done;
} wpt_wait_state;

static gboolean
wpt_poll_cb(gpointer user_data)
{
    wpt_wait_state *w = user_data;
    if (!wpt_results_ready(w->fc->js)) return G_SOURCE_CONTINUE;
    w->done = TRUE;
    g_main_loop_quit(w->loop);
    return G_SOURCE_REMOVE;
}

static char *
wpt_eval(ns_js *js, const char *src)
{
    char *r = ns_js_eval_source(js, src, "wpt-report");
    ns_js_consume_mutated(js);
    return r;
}

/* Waits for WPT results up to the timeout, prints report and JSON; returns exit code. */
static int
headless_wpt_finish(headless_flush_ctx *fc, const ns_headless_opts *opts)
{
    if (!fc->js) return 2;
    int timeout_ms = opts->wpt_timeout_ms > 0 ? opts->wpt_timeout_ms : 15000;
    gboolean done = wpt_results_ready(fc->js);
    if (!done) {
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        wpt_wait_state w = { .loop = loop, .fc = fc };
        settle_state st = { .fc = fc, .last_flush_us = g_get_monotonic_time() };
        guint raf_id = g_timeout_add(16, settle_raf_tick, &st);
        guint poll_id = g_timeout_add(50, wpt_poll_cb, &w);
        guint stop_id = g_timeout_add(timeout_ms, settle_quit_cb, loop);
        g_main_loop_run(loop);
        g_source_remove(raf_id);
        if (w.done) g_source_remove(stop_id);
        else        g_source_remove(poll_id);
        g_main_loop_unref(loop);
        done = w.done;
    }
    if (!done) {
        char *seen = wpt_eval(fc->js,
                              "globalThis.__ns_wpt_seen_harness ? \"1\" : \"0\"");
        const char *why = seen && strcmp(seen, "1") == 0
            ? "tests did not complete before the timeout"
            : "testharness.js never registered";
        g_free(seen);
        fprintf(stdout, "WPT HARNESS TIMEOUT | %s\n", why);
        fprintf(stdout, "WPT SUMMARY total=0 pass=0 fail=0 timeout=0 "
                        "notrun=0 precondition_failed=0\n");
        fprintf(stdout, "WPT JSON {\"harness\":\"TIMEOUT\",\"message\":\"%s\","
                        "\"subtests\":[]}\n", why);
        fflush(stdout);
        return 2;
    }
    char *report = wpt_eval(fc->js, "globalThis.__ns_wpt_report || \"\"");
    char *json   = wpt_eval(fc->js, "globalThis.__ns_wpt_json || \"{}\"");
    char *fails  = wpt_eval(fc->js, "String(globalThis.__ns_wpt_failures || 0)");
    if (report) fputs(report, stdout);
    fprintf(stdout, "WPT JSON %s\n", json && *json ? json : "{}");
    fflush(stdout);
    int rc = (!fails || atoi(fails) > 0) ? 1 : 0;
    g_free(report);
    g_free(json);
    g_free(fails);
    return rc;
}

/* Replaces [lo,hi) of the focused control's value honoring numeric filter and maxlength. */
static void
headless_edit_replace(headless_flush_ctx *fc, gsize lo, gsize hi, const char *ins)
{
    if (!fc->focused) return;
    ns_node *t = (ns_node *)fc->focused;
    const char *cur = ns_node_editable_value(t);
    gsize clen = strlen(cur);
    if (lo > clen) lo = clen;
    if (hi > clen) hi = clen;
    if (hi < lo) hi = lo;
    gsize ins_len = ins ? strlen(ins) : 0;
    char *numeric_filtered = NULL;
    if (ins_len && ns_node_is_numeric_input(t)) {
        gsize fl = 0;
        numeric_filtered = ns_numeric_filter_insert(ins, ins_len, &fl);
        ins = numeric_filtered;
        ins_len = fl;
        if (ins_len == 0) { g_free(numeric_filtered); return; }
    }
    if (ins_len && ns_form_control_length_limits_apply(t)) {
        const char *ml = ns_element_get_attr(t, "maxlength");
        if (ml && *ml) {
            long maxl = atol(ml);
            if (maxl >= 0) {
                glong kept = g_utf8_strlen(cur, (gssize)lo) +
                             g_utf8_strlen(cur + hi, (gssize)(clen - hi));
                glong room = maxl - kept;
                if (room < 0) room = 0;
                if (g_utf8_strlen(ins, (gssize)ins_len) > room) {
                    const char *p = ins;
                    for (glong i = 0; i < room; i++) p = g_utf8_next_char(p);
                    ins_len = (gsize)(p - ins);
                    if (ins_len == 0) { g_free(numeric_filtered); return; }
                }
            }
        }
    }
    GString *s = g_string_new(NULL);
    g_string_append_len(s, cur, (gssize)lo);
    if (ins_len) g_string_append_len(s, ins, (gssize)ins_len);
    g_string_append_len(s, cur + hi, (gssize)(clen - hi));
    g_free(numeric_filtered);
    if (fc->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_event(fc->js, t, "beforeinput", &prevented);
        if (prevented) { g_string_free(s, TRUE); return; }
    }
    ns_node_set_editable_value(t, s->str);
    fc->caret = lo + ins_len;
    fc->anchor = fc->caret;
    g_string_free(s, TRUE);
    if (fc->js) {
        ns_js_dispatch_event(fc->js, t, "input", NULL);
        ns_js_consume_mutated(fc->js);
    }
}

/* Validates and dispatches submit for the trigger's form, then captures the navigation. */
static void
headless_submit_form_from(headless_flush_ctx *fc, headless_nav_capture *nav,
                          const ns_node *trigger)
{
    if (!nav || !trigger) return;
    if (ns_element_effectively_disabled(trigger)) return;
    const ns_node *doc = fc->doc ? fc->doc : ns_node_root(trigger);
    const ns_node *form = ns_form_owner(trigger, doc);
    if (!form) return;
    const ns_node *root = doc ? doc : form;
    if (fc->js) {
        if (!ns_js_form_submission_allowed(fc->js, form, trigger)) {
            fprintf(stderr, "[headless] form blocked by an invalid field\n");
            return;
        }
    } else if (!ns_element_get_attr(form, "novalidate") &&
               !ns_element_get_attr(trigger, "formnovalidate") &&
               ns_form_first_invalid(form, root, root)) {
        fprintf(stderr, "[headless] form blocked by an invalid field\n");
        return;
    }
    if (fc->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_submit_event(fc->js, form, trigger, &prevented);
        ns_js_consume_mutated(fc->js);
        if (prevented) return;
    }
    headless_js_form_submit(form, trigger, nav);
}

static gboolean
headless_emit_pointer_and_mouse(headless_flush_ctx *fc, const ns_node *target,
                                const char *ptr_type, const char *mouse_type,
                                double x, double y, int button, int buttons);

/* Simulates a click: label retargeting, pointer/mouse events, focus, activation and links. */
static void
headless_click(headless_flush_ctx *fc, headless_nav_capture *nav,
               double x, double y)
{
    ns_box *layout = *fc->layout;
    if (!layout) return;
    ns_js_note_pointer_input(fc->js, TRUE);
    const ns_box *hit = ns_box_hit_test(layout, x, y);
    const ns_node *dom = ns_box_hit_dom(layout, x, y);
    if (!dom) { fc->focused = NULL; return; }
    gboolean form_target = ns_node_is_element_named(dom, "button") ||
                           ns_node_is_element_named(dom, "input") ||
                           ns_node_is_element_named(dom, "select") ||
                           ns_node_is_element_named(dom, "textarea");
    if (!form_target) {
        for (const ns_node *lc = dom; lc; lc = lc->parent) {
            if (!ns_node_is_element_named(lc, "label")) continue;
            const ns_node *tgt = NULL;
            const char *for_id = ns_element_get_attr(lc, "for");
            if (for_id && *for_id && fc->doc)
                tgt = ns_node_find_by_id(fc->doc, for_id);
            if (!tgt) {
                GQueue q = G_QUEUE_INIT;
                for (const ns_node *d = lc->first_child; d; d = d->next_sibling)
                    g_queue_push_tail(&q, (gpointer)d);
                while (!g_queue_is_empty(&q) && !tgt) {
                    const ns_node *d = g_queue_pop_head(&q);
                    if (d->kind == NS_NODE_ELEMENT && d->name &&
                        (strcmp(d->name, "input") == 0 ||
                         strcmp(d->name, "select") == 0 ||
                         strcmp(d->name, "textarea") == 0 ||
                         strcmp(d->name, "button") == 0))
                        tgt = d;
                    else
                        for (const ns_node *e = d->first_child; e; e = e->next_sibling)
                            g_queue_push_tail(&q, (gpointer)e);
                }
                g_queue_clear(&q);
            }
            if (tgt && tgt != dom) dom = tgt;
            break;
        }
    }
    fprintf(stderr, "[headless] click hit <%s>\n",
            dom->name ? dom->name : "(text)");
    const ns_node *editable = NULL;
    for (const ns_node *cur = dom; cur; cur = cur->parent)
        if (ns_node_is_editable(cur)) { editable = cur; break; }
    gboolean prevented = FALSE;
    gboolean activated = FALSE;
    if (fc->js) {
        headless_emit_pointer_and_mouse(fc, dom, "pointerdown", "mousedown",
                                        x, y, 0, 1);
        headless_emit_pointer_and_mouse(fc, dom, "pointerup", "mouseup",
                                        x, y, 0, 0);
        ns_js_click_state click_state = {0};
        ns_js_click_begin(fc->js, dom, &click_state);
        ns_js_dispatch_mouse_event(fc->js, dom, "click", x, y, x, y, 0, 0,
                                   FALSE, FALSE, FALSE, FALSE, NULL,
                                   &prevented);
        activated = ns_js_click_end(fc->js, dom, &click_state, prevented);
        ns_js_consume_mutated(fc->js);
    }
    if (editable) {
        ns_node_flatten_editable((ns_node *)editable);
        if (fc->js && fc->focused && fc->focused != editable)
            ns_js_dispatch_event(fc->js, fc->focused, "blur", NULL);
        fc->focused = editable;
        const char *v = ns_node_editable_value(editable);
        fc->caret = v ? strlen(v) : 0;
        fc->anchor = fc->caret;
        if (fc->js) {
            ns_js_set_focused_node(fc->js, editable);
            ns_js_dispatch_event(fc->js, editable, "focus",   NULL);
            ns_js_dispatch_event(fc->js, editable, "focusin", NULL);
        }
        return;
    }
    if (prevented || activated) return;
    for (const ns_node *cur = dom; cur; cur = cur->parent) {
        if (!ns_form_is_submit_trigger(cur)) continue;
        headless_submit_form_from(fc, nav, cur);
        return;
    }
    if (hit && hit->dom && ns_node_is_element_named(hit->dom, "img") && nav) {
        const char *usemap = ns_element_get_attr(hit->dom, "usemap");
        if (usemap && *usemap && fc->doc) {
            double cx0 = hit->x + hit->margin.left +
                         hit->border.left + hit->padding.left;
            double cy0 = hit->y + hit->margin.top +
                         hit->border.top + hit->padding.top;
            char *ahref = ns_image_map_resolve(fc->doc, usemap, x - cx0, y - cy0,
                                               hit->content_width,
                                               hit->content_height, NULL);
            if (ahref) {
                g_free(nav->pending_url);
                nav->pending_url = ahref;
                return;
            }
        }
    }
    if (nav) {
        for (const ns_node *cur = dom; cur; cur = cur->parent) {
            if (!ns_node_is_element_named(cur, "a")) continue;
            const char *href = ns_element_get_attr(cur, "href");
            if (!href || !*href) break;
            char *url;
            if (hit && hit->dom && ns_node_is_element_named(hit->dom, "img") &&
                ns_element_get_attr(hit->dom, "ismap")) {
                double cx0 = hit->x + hit->margin.left +
                             hit->border.left + hit->padding.left;
                double cy0 = hit->y + hit->margin.top +
                             hit->border.top + hit->padding.top;
                int ix = (int)(x - cx0); if (ix < 0) ix = 0;
                int iy = (int)(y - cy0); if (iy < 0) iy = 0;
                url = g_strdup_printf("%s?%d,%d", href, ix, iy);
            } else {
                url = g_strdup(href);
            }
            g_free(nav->pending_url);
            nav->pending_url = url;
            return;
        }
    }
    for (const ns_node *cur = dom; cur; cur = cur->parent) {
        if (cur->kind == NS_NODE_ELEMENT && cur->name &&
            strcmp(cur->name, "summary") == 0 && cur->parent &&
            ns_node_is_element_named(cur->parent, "details")) {
            ns_node *details = (ns_node *)cur->parent;
            gboolean now_open;
            if (ns_element_get_attr(details, "open")) {
                ns_element_remove_attr(details, "open"); now_open = FALSE;
            } else {
                ns_element_set_attr(details, "open", ""); now_open = TRUE;
            }
            if (fc->js) {
                ns_js_details_toggle_open(fc->js, details, now_open);
                ns_js_consume_mutated(fc->js);
            }
            return;
        }
        if (!fc->js && cur->kind == NS_NODE_ELEMENT && cur->name &&
            strcmp(cur->name, "input") == 0) {
            const char *type = ns_element_get_attr(cur, "type");
            if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
                ns_element_set_attr((ns_node *)cur, "data-nd-checked",
                                    ns_input_is_checked(cur) ? "0" : "1");
            } else if (type && g_ascii_strcasecmp(type, "radio") == 0) {
                ns_element_set_attr((ns_node *)cur, "data-nd-checked", "1");
            } else {
                continue;
            }
            if (fc->js) {
                ns_js_dispatch_event(fc->js, cur, "input",  NULL);
                ns_js_dispatch_event(fc->js, cur, "change", NULL);
                ns_js_consume_mutated(fc->js);
            }
            return;
        }
    }
    fc->focused = NULL;
}

static gboolean
headless_attr_true(const ns_node *n, const char *name)
{
    const char *v = ns_element_get_attr(n, name);
    return v && g_ascii_strcasecmp(v, "true") == 0;
}

static gboolean
headless_attr_false(const ns_node *n, const char *name)
{
    const char *v = ns_element_get_attr(n, name);
    return v && g_ascii_strcasecmp(v, "false") == 0;
}

/* Finds the draggable element at a point: draggable=true, or a link/image by default. */
static const ns_node *
headless_drag_source_at(ns_box *layout, double x, double y)
{
    const ns_box *hit = layout ? ns_box_hit_test(layout, x, y) : NULL;
    for (const ns_node *p = hit ? hit->dom : NULL; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT || !p->name) continue;
        if (headless_attr_true(p, "draggable")) return p;
        if (headless_attr_false(p, "draggable")) continue;
        if (strcmp(p->name, "a") == 0) {
            const char *href = ns_element_get_attr(p, "href");
            if (href && *href) return p;
        }
        if (strcmp(p->name, "img") == 0) {
            const char *src = ns_element_get_attr(p, "src");
            if (src && *src) return p;
        }
    }
    return NULL;
}

/* Returns the element at a point for drop events, falling back to body or the document. */
static const ns_node *
headless_drag_target_at(headless_flush_ctx *fc, double x, double y)
{
    ns_box *layout = fc && fc->layout ? *fc->layout : NULL;
    const ns_box *hit = layout ? ns_box_hit_test(layout, x, y) : NULL;
    if (hit && hit->dom) return hit->dom;
    if (!fc || !fc->doc) return NULL;
    ns_node *body = ns_node_find_first_element(fc->doc, "body");
    return body ? body : fc->doc;
}

/* Puts the absolute href/src of a link or image source into the drag data as text and URI. */
static void
headless_seed_drag_data(headless_flush_ctx *fc, ns_js_drag_session *session,
                        const ns_node *source)
{
    if (!fc || !session || !source) return;
    const char *raw = NULL;
    if (ns_node_is_element_named(source, "a"))
        raw = ns_element_get_attr(source, "href");
    else if (ns_node_is_element_named(source, "img"))
        raw = ns_element_get_attr(source, "src");
    if (!raw || !*raw) return;
    char *abs = fc->base ? ns_url_resolve(fc->base, raw) : g_strdup(raw);
    if (!abs) return;
    ns_js_drag_session_set_data(session, "text/plain", abs);
    ns_js_drag_session_set_data(session, "text/uri-list", abs);
    g_free(abs);
}

/* Dispatches one drag event of type at a point; returns TRUE if it was canceled. */
static gboolean
headless_dispatch_drag(headless_flush_ctx *fc, ns_js_drag_session *session,
                       const ns_node *target, const char *type,
                       double x, double y, int buttons,
                       const ns_node *related)
{
    if (!fc || !fc->js || !session || !target) return FALSE;
    gboolean prevented = FALSE;
    ns_js_dispatch_drag_event(fc->js, session, target, type,
                              x, y, x, y, 0, buttons,
                              FALSE, FALSE, FALSE, FALSE,
                              related, &prevented);
    ns_js_consume_mutated(fc->js);
    return prevented;
}

/* Simulates HTML drag and drop: dragstart, dragenter/over, drop or dragleave, dragend. */
static void
headless_drag(headless_flush_ctx *fc,
              double x0, double y0, double x1, double y1)
{
    ns_box *layout = fc && fc->layout ? *fc->layout : NULL;
    if (!fc || !fc->js || !layout) return;
    const ns_node *source = headless_drag_source_at(layout, x0, y0);
    if (!source) return;
    ns_js_drag_session *session = ns_js_drag_session_new(fc->js);
    if (!session) return;
    headless_seed_drag_data(fc, session, source);
    fprintf(stderr, "[headless] drag hit <%s>\n",
            source->name ? source->name : "(text)");
    gboolean start_prevented =
        headless_dispatch_drag(fc, session, source, "dragstart",
                               x0, y0, 1, NULL);
    if (!start_prevented) {
        const ns_node *target = headless_drag_target_at(fc, x1, y1);
        gboolean can_drop = FALSE;
        if (target) {
            if (headless_dispatch_drag(fc, session, target, "dragenter",
                                       x1, y1, 1, source))
                can_drop = TRUE;
            if (headless_dispatch_drag(fc, session, target, "dragover",
                                       x1, y1, 1, NULL))
                can_drop = TRUE;
            if (can_drop)
                headless_dispatch_drag(fc, session, target, "drop",
                                       x1, y1, 0, NULL);
            else
                headless_dispatch_drag(fc, session, target, "dragleave",
                                       x1, y1, 1, NULL);
        }
        headless_dispatch_drag(fc, session, source, "dragend",
                               x1, y1, 0, target);
    }
    ns_js_drag_session_free(session);
}

static const ns_node *
headless_mouse_target_at(headless_flush_ctx *fc, double x, double y)
{
    ns_box *layout = fc && fc->layout ? *fc->layout : NULL;
    const ns_box *hit = layout ? ns_box_hit_test(layout, x, y) : NULL;
    if (hit && hit->dom) return hit->dom;
    return fc && fc->doc ? ns_node_find_first_element(fc->doc, "body") : NULL;
}

/* Dispatches a pointer event then its mouse twin; returns TRUE if either was canceled. */
static gboolean
headless_emit_pointer_and_mouse(headless_flush_ctx *fc, const ns_node *target,
                                const char *ptr_type, const char *mouse_type,
                                double x, double y, int button, int buttons)
{
    if (!fc || !fc->js || !target) return FALSE;
    gboolean prevented = FALSE;
    ns_js_dispatch_mouse_event(fc->js, target, ptr_type, x, y, x, y,
                               button, buttons, FALSE, FALSE, FALSE, FALSE,
                               NULL, &prevented);
    if (fc->js)
        ns_js_dispatch_mouse_event(fc->js, target, mouse_type, x, y, x, y,
                                   button, buttons, FALSE, FALSE, FALSE, FALSE,
                                   NULL, &prevented);
    if (fc->js) ns_js_consume_mutated(fc->js);
    return prevented;
}

/* Moves the virtual pointer, firing out/over transitions and move events, then relayouts. */
static void
headless_mouse_move(headless_flush_ctx *fc, double x, double y)
{
    if (!fc || !fc->js) return;
    const ns_node *node = headless_mouse_target_at(fc, x, y);
    const ns_node *prev = fc->hover;
    if (node != prev) {
        if (prev) {
            ns_js_dispatch_mouse_event(fc->js, prev, "pointerout", x, y, x, y,
                                       0, 0, FALSE, FALSE, FALSE, FALSE,
                                       node, NULL);
            ns_js_dispatch_mouse_event(fc->js, prev, "mouseout", x, y, x, y,
                                       0, 0, FALSE, FALSE, FALSE, FALSE,
                                       node, NULL);
        }
        if (node) {
            ns_js_dispatch_mouse_event(fc->js, node, "pointerover", x, y, x, y,
                                       0, 0, FALSE, FALSE, FALSE, FALSE,
                                       prev, NULL);
            ns_js_dispatch_mouse_event(fc->js, node, "mouseover", x, y, x, y,
                                       0, 0, FALSE, FALSE, FALSE, FALSE,
                                       prev, NULL);
        }
        fc->hover = node;
    }
    if (node)
        headless_emit_pointer_and_mouse(fc, node, "pointermove", "mousemove",
                                        x, y, 0, 0);
    headless_relayout(fc);
}

/* Simulates a mouse drag as down, eight interpolated moves with settling, and up. */
static void
headless_mouse_drag(headless_flush_ctx *fc,
                    double x0, double y0, double x1, double y1)
{
    if (!fc || !fc->js) return;
    const ns_node *down = headless_mouse_target_at(fc, x0, y0);
    if (!down) return;
    headless_emit_pointer_and_mouse(fc, down, "pointerdown", "mousedown",
                                    x0, y0, 0, 1);
    const int steps = 8;
    for (int i = 1; i <= steps; i++) {
        double x = x0 + (x1 - x0) * i / steps;
        double y = y0 + (y1 - y0) * i / steps;
        const ns_node *over = headless_mouse_target_at(fc, x, y);
        if (over)
            headless_emit_pointer_and_mouse(fc, over, "pointermove",
                                            "mousemove", x, y, 0, 1);
        settle_main_loop(30, fc);
    }
    const ns_node *up = headless_mouse_target_at(fc, x1, y1);
    if (up)
        headless_emit_pointer_and_mouse(fc, up, "pointerup", "mouseup",
                                        x1, y1, 0, 0);
}

/* Simulates a named key on the focused control: key events, Tab, editing, Enter submit. */
static void
headless_key(headless_flush_ctx *fc, headless_nav_capture *nav,
             const char *name)
{
    ns_js_note_pointer_input(fc->js, FALSE);
    if (!fc->focused || !name || !*name) return;
    ns_node *t = (ns_node *)fc->focused;
    const char *cur = ns_node_editable_value(t);
    gsize clen = strlen(cur);
    if (fc->caret > clen) fc->caret = clen;
    if (fc->anchor > clen) fc->anchor = clen;
    gsize lo = MIN(fc->caret, fc->anchor);
    gsize hi = MAX(fc->caret, fc->anchor);
    gboolean has_sel = lo != hi;
    gboolean multiline = (t->name && strcmp(t->name, "textarea") == 0) ||
                         ns_node_is_contenteditable_host(t);
    gboolean key_prevented = FALSE;
    if (fc->js) {
        int key_code = 0;
        const char *jskey = name;
        struct { const char *n; int c; const char *k; } map[] = {
            {"Enter",13,"Enter"}, {"Return",13,"Enter"},
            {"Backspace",8,"Backspace"}, {"Delete",46,"Delete"},
            {"Tab",9,"Tab"}, {"Escape",27,"Escape"},
            {"Left",37,"ArrowLeft"}, {"Right",39,"ArrowRight"},
            {"Up",38,"ArrowUp"}, {"Down",40,"ArrowDown"},
            {"Home",36,"Home"}, {"End",35,"End"},
        };
        for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
            if (g_ascii_strcasecmp(name, map[i].n) == 0) {
                key_code = map[i].c; jskey = map[i].k; break;
            }
        if (key_code) {
            ns_js_dispatch_key_event(fc->js, t, "keydown", jskey, jskey,
                                     key_code, FALSE, FALSE, FALSE, FALSE,
                                     &key_prevented);
            ns_js_dispatch_key_event(fc->js, t, "keyup", jskey, jskey,
                                     key_code, FALSE, FALSE, FALSE, FALSE, NULL);
            ns_js_consume_mutated(fc->js);
        }
    }
    if (g_ascii_strcasecmp(name, "Tab") == 0) {
        if (!key_prevented && fc->js) {
            const ns_node *next =
                ns_js_sequential_focus_target(fc->js, FALSE);
            if (next) {
                if (fc->focused && fc->focused != next)
                    ns_js_dispatch_event(fc->js, fc->focused, "blur", NULL);
                fc->focused = next;
                ns_js_set_focused_node(fc->js, next);
                const char *nv = ns_node_editable_value(next);
                fc->caret = nv ? strlen(nv) : 0;
                fc->anchor = fc->caret;
                ns_js_consume_mutated(fc->js);
            }
        }
        return;
    }
    if (g_ascii_strcasecmp(name, "Enter") == 0 ||
        g_ascii_strcasecmp(name, "Return") == 0) {
        if (multiline)
            headless_edit_replace(fc, lo, hi, "\n");
        else if (!key_prevented && t->name && strcmp(t->name, "input") == 0)
            headless_submit_form_from(fc, nav, t);
        return;
    }
    if (g_ascii_strcasecmp(name, "Backspace") == 0) {
        if (has_sel) headless_edit_replace(fc, lo, hi, NULL);
        else if (fc->caret > 0) {
            const char *prev = g_utf8_prev_char(cur + fc->caret);
            headless_edit_replace(fc, (gsize)(prev - cur), fc->caret, NULL);
        }
        return;
    }
    if (g_ascii_strcasecmp(name, "Delete") == 0) {
        if (has_sel) headless_edit_replace(fc, lo, hi, NULL);
        else if (fc->caret < clen) {
            const char *nxt = g_utf8_next_char(cur + fc->caret);
            headless_edit_replace(fc, fc->caret, (gsize)(nxt - cur), NULL);
        }
        return;
    }
    if (g_ascii_strcasecmp(name, "Left") == 0) {
        if (fc->caret > 0) {
            const char *p = g_utf8_prev_char(cur + fc->caret);
            fc->caret = (gsize)(p - cur);
        }
        fc->anchor = fc->caret;
        return;
    }
    if (g_ascii_strcasecmp(name, "Right") == 0) {
        if (fc->caret < clen) {
            const char *p = g_utf8_next_char(cur + fc->caret);
            fc->caret = (gsize)(p - cur);
        }
        fc->anchor = fc->caret;
        return;
    }
    if (g_ascii_strcasecmp(name, "Home") == 0) { fc->caret = 0; fc->anchor = 0; return; }
    if (g_ascii_strcasecmp(name, "End") == 0)  { fc->caret = clen; fc->anchor = clen; return; }
    if (g_ascii_strcasecmp(name, "Up") == 0 || g_ascii_strcasecmp(name, "Down") == 0) {
        const char *itype = t->name && strcmp(t->name, "input") == 0
            ? ns_element_get_attr(t, "type") : NULL;
        if (itype && g_ascii_strcasecmp(itype, "number") == 0) {
            const char *sv = ns_element_get_attr(t, "step");
            double step = sv && *sv ? g_ascii_strtod(sv, NULL) : 1.0;
            if (!(step > 0)) step = 1.0;
            double val = *cur ? g_ascii_strtod(cur, NULL) : 0.0;
            val += (g_ascii_strcasecmp(name, "Up") == 0) ? step : -step;
            const char *mn = ns_element_get_attr(t, "min");
            const char *mx = ns_element_get_attr(t, "max");
            if (mn && *mn) { double m = g_ascii_strtod(mn, NULL); if (val < m) val = m; }
            if (mx && *mx) { double m = g_ascii_strtod(mx, NULL); if (val > m) val = m; }
            char buf[32];
            g_snprintf(buf, sizeof buf, "%g", val);
            ns_node_set_editable_value(t, buf);
            fc->caret = strlen(buf); fc->anchor = fc->caret;
            if (fc->js) {
                ns_js_dispatch_event(fc->js, t, "input",  NULL);
                ns_js_dispatch_event(fc->js, t, "change", NULL);
                ns_js_consume_mutated(fc->js);
            }
        }
        return;
    }
}

/* Runs a ';'-separated action script in the legacy driver until a navigation is pending. */
static void
headless_run_actions(headless_flush_ctx *fc, headless_nav_capture *nav,
                     const char *spec)
{
    if (!fc || !spec || !*spec) return;
    headless_relayout(fc);
    char **acts = g_strsplit(spec, ";", -1);
    for (int i = 0; acts[i]; i++) {
        char *a = g_strstrip(acts[i]);
        if (!*a) continue;
        gboolean activation_action = g_str_has_prefix(a, "click ") ||
                                     g_str_has_prefix(a, "key ");
        if (g_str_has_prefix(a, "click ")) {
            double x = 0, y = 0;
            if (sscanf(a + 6, "%lf , %lf", &x, &y) == 2) {
                fprintf(stderr, "[headless] click %g,%g\n", x, y);
                headless_click(fc, nav, x, y);
            }
        } else if (g_str_has_prefix(a, "rightclick ")) {
            double x = 0, y = 0;
            if (sscanf(a + 11, "%lf , %lf", &x, &y) == 2) {
                ns_box *layout = *fc->layout;
                const ns_node *dom = layout
                    ? ns_box_hit_dom(layout, x, y) : NULL;
                if (dom && fc->js) {
                    gboolean prevented = FALSE;
                    ns_js_dispatch_mouse_event(fc->js, dom, "contextmenu",
                                               x, y, x, y, 2, 0,
                                               FALSE, FALSE, FALSE, FALSE,
                                               NULL, &prevented);
                    fprintf(stderr, "[headless] rightclick %g,%g prevented=%d\n",
                            x, y, prevented);
                    headless_relayout(fc);
                }
            }
        } else if (g_str_has_prefix(a, "hold ")) {
            double x = 0, y = 0;
            long ms = 0;
            if (sscanf(a + 5, "%lf , %lf %ld", &x, &y, &ms) == 3) {
                fprintf(stderr, "[headless] hold %g,%g %ldms\n", x, y, ms);
                ns_box *layout = *fc->layout;
                const ns_box *hit =
                    layout ? ns_box_hit_test(layout, x, y) : NULL;
                const ns_node *dom = hit ? hit->dom : NULL;
                fprintf(stderr, "[headless] hold hit <%s>\n",
                        dom && dom->name ? dom->name : "(none)");
                if (dom) {
                    ns_css_set_active_node(dom);
                    headless_relayout(fc);
                    if (ms > 0) settle_main_loop((int)ms, fc);
                    ns_css_set_active_node(NULL);
                    headless_relayout(fc);
                }
            }
        } else if (g_str_has_prefix(a, "move ")) {
            double x = 0, y = 0;
            if (sscanf(a + 5, "%lf , %lf", &x, &y) == 2) {
                fprintf(stderr, "[headless] move %g,%g\n", x, y);
                headless_mouse_move(fc, x, y);
            }
        } else if (g_str_has_prefix(a, "mousedrag ")) {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (sscanf(a + 10, "%lf , %lf %lf , %lf",
                       &x0, &y0, &x1, &y1) == 4) {
                fprintf(stderr, "[headless] mousedrag %g,%g -> %g,%g\n",
                        x0, y0, x1, y1);
                headless_mouse_drag(fc, x0, y0, x1, y1);
            }
        } else if (g_str_has_prefix(a, "drag ")) {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (sscanf(a + 5, "%lf , %lf %lf , %lf",
                       &x0, &y0, &x1, &y1) == 4) {
                fprintf(stderr, "[headless] drag %g,%g -> %g,%g\n",
                        x0, y0, x1, y1);
                headless_drag(fc, x0, y0, x1, y1);
            }
        } else if (g_str_has_prefix(a, "type ")) {
            fprintf(stderr, "[headless] type \"%s\"\n", a + 5);
            headless_edit_replace(fc, MIN(fc->caret, fc->anchor),
                                  MAX(fc->caret, fc->anchor), a + 5);
        } else if (g_str_has_prefix(a, "key ")) {
            fprintf(stderr, "[headless] key %s\n", a + 4);
            headless_key(fc, nav, g_strstrip(a + 4));
        } else if (g_str_has_prefix(a, "eval ")) {
            char *result = ns_js_eval_source(fc->js, a + 5, "headless-act-eval");
            if (result) {
                fprintf(stdout, "act-eval: %s\n", result);
                g_free(result);
            }
            ns_js_consume_mutated(fc->js);
        } else if (g_str_has_prefix(a, "evalfile ")) {
            char *src = NULL;
            if (g_file_get_contents(g_strstrip(a + 9), &src, NULL, NULL)) {
                char *result = ns_js_eval_source(fc->js, src,
                                                 "headless-act-evalfile");
                if (result) {
                    fprintf(stdout, "act-eval: %s\n", result);
                    g_free(result);
                }
                ns_js_consume_mutated(fc->js);
                g_free(src);
            } else {
                fprintf(stderr, "[headless] evalfile: cannot read %s\n", a + 9);
            }
        } else if (g_str_has_prefix(a, "scroll ")) {
            double x = 0, y = 0;
            if (sscanf(a + 7, "%lf , %lf", &x, &y) == 2 ||
                sscanf(a + 7, "%lf %lf", &x, &y) == 2) {
                fprintf(stderr, "[headless] scroll %g,%g\n", x, y);
                ns_js_note_viewport_scroll(fc->js, x, y);
                ns_js_consume_mutated(fc->js);
            }
        } else if (g_str_has_prefix(a, "wait ")) {
            gint64 ms = g_ascii_strtoll(a + 5, NULL, 10);
            if (ms < 0) ms = 0;
            if (ms > 600000) ms = 600000;
            fprintf(stderr, "[headless] wait %" G_GINT64_FORMAT "ms\n", ms);
            settle_main_loop((int)ms, fc);
        } else {
            fprintf(stderr, "[headless] unknown action: %s\n", a);
        }
        headless_relayout(fc);
        if (activation_action && nav && nav->pending_url)
            nav->user_activated = TRUE;
        if (nav && nav->pending_url) break;
    }
    g_strfreev(acts);
}

/* Formats a computed CSS value into buf for inspect output. */
static void
inspect_value_str(const ns_css_value *v, char *buf, size_t cap)
{
    if (!v) { g_strlcpy(buf, "(unset)", cap); return; }
    switch (v->kind) {
    case NS_CSS_V_KEYWORD:
        g_snprintf(buf, cap, "%s", v->u.keyword ? v->u.keyword : "?");
        break;
    case NS_CSS_V_LENGTH:
        g_snprintf(buf, cap, "%g%s", v->u.length.v,
                   ns_css_unit_suffix(v->u.length.unit));
        break;
    case NS_CSS_V_CALC:
        g_snprintf(buf, cap, "calc(%g%% + %gpx)", v->u.calc.pct, v->u.calc.px);
        break;
    case NS_CSS_V_COLOR:
        g_snprintf(buf, cap, "rgba(%u, %u, %u, %u)", v->u.color.r,
                   v->u.color.g, v->u.color.b, v->u.color.a);
        break;
    case NS_CSS_V_URL:
        g_snprintf(buf, cap, "url(%s)", v->u.url ? v->u.url : "");
        break;
    default:
        g_strlcpy(buf, "(set)", cap);
        break;
    }
}

/* Formats a node as tag#id.class for inspect output. */
static void
inspect_node_label(const ns_node *n, char *buf, size_t cap)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) {
        g_strlcpy(buf, n && n->kind == NS_NODE_TEXT ? "#text" : "(anonymous)",
                  cap);
        return;
    }
    GString *s = g_string_new(n->name);
    const char *id = ns_element_get_attr(n, "id");
    if (id && *id) g_string_append_printf(s, "#%s", id);
    const char *cls = ns_element_get_attr(n, "class");
    if (cls && *cls) {
        char **parts = g_strsplit_set(cls, " \t\r\n", -1);
        for (int i = 0; parts[i]; i++)
            if (*parts[i]) g_string_append_printf(s, ".%s", parts[i]);
        g_strfreev(parts);
    }
    g_strlcpy(buf, s->str, cap);
    g_string_free(s, TRUE);
}

/* Depth-first search for the first box generated by a DOM node. */
static const ns_box *
inspect_find_box(const ns_box *root, const ns_node *dom)
{
    if (!root) return NULL;
    if (root->dom == dom) return root;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
        const ns_box *m = inspect_find_box(c, dom);
        if (m) return m;
    }
    return NULL;
}

static void
inspect_prop_line(GString *out, const ns_style *s, ns_css_prop p,
                  const char *label)
{
    if (!s || !s->values[p]) return;
    char b[160];
    inspect_value_str(s->values[p], b, sizeof b);
    g_string_append_printf(out, "    %-15s %s\n", label, b);
}

/* Prints a box's geometry, key computed properties, children and ancestor path. */
static void
inspect_print_box(const ns_box *box, GString *out)
{
    char label[320];
    inspect_node_label(box->dom, label, sizeof label);
    g_string_append_printf(out, "  <%s>\n", label);

    double bx = box->x + box->margin.left;
    double by = box->y + box->margin.top;
    double bw = box->content_width + box->padding.left + box->padding.right +
                box->border.left + box->border.right;
    double bh = box->content_height + box->padding.top + box->padding.bottom +
                box->border.top + box->border.bottom;
    g_string_append_printf(out, "    content         %g x %g  (at %g,%g)\n",
                           box->content_width, box->content_height,
                           box->x, box->y);
    g_string_append_printf(out, "    border-box      %g x %g  (at %g,%g)\n",
                           bw, bh, bx, by);
    g_string_append_printf(out, "    margin          T%g R%g B%g L%g\n",
                           box->margin.top, box->margin.right,
                           box->margin.bottom, box->margin.left);
    g_string_append_printf(out, "    border          T%g R%g B%g L%g\n",
                           box->border.top, box->border.right,
                           box->border.bottom, box->border.left);
    g_string_append_printf(out, "    padding         T%g R%g B%g L%g\n",
                           box->padding.top, box->padding.right,
                           box->padding.bottom, box->padding.left);

    const ns_style *s = box->style;
    inspect_prop_line(out, s, NS_CSS_DISPLAY,    "display");
    inspect_prop_line(out, s, NS_CSS_POSITION,   "position");
    inspect_prop_line(out, s, NS_CSS_BOX_SIZING, "box-sizing");
    inspect_prop_line(out, s, NS_CSS_WIDTH,      "width");
    inspect_prop_line(out, s, NS_CSS_HEIGHT,     "height");
    inspect_prop_line(out, s, NS_CSS_MIN_WIDTH,  "min-width");
    inspect_prop_line(out, s, NS_CSS_MAX_WIDTH,  "max-width");
    inspect_prop_line(out, s, NS_CSS_MIN_HEIGHT, "min-height");
    inspect_prop_line(out, s, NS_CSS_MAX_HEIGHT, "max-height");
    inspect_prop_line(out, s, NS_CSS_FLEX_DIRECTION,  "flex-direction");
    inspect_prop_line(out, s, NS_CSS_JUSTIFY_CONTENT, "justify-content");
    inspect_prop_line(out, s, NS_CSS_ALIGN_ITEMS,     "align-items");
    inspect_prop_line(out, s, NS_CSS_ALIGN_SELF,      "align-self");
    inspect_prop_line(out, s, NS_CSS_FLEX_GROW,       "flex-grow");
    inspect_prop_line(out, s, NS_CSS_FLEX_SHRINK,     "flex-shrink");
    inspect_prop_line(out, s, NS_CSS_FLEX_BASIS,      "flex-basis");
    inspect_prop_line(out, s, NS_CSS_GAP,             "gap");
    inspect_prop_line(out, s, NS_CSS_FONT_SIZE,       "font-size");
    inspect_prop_line(out, s, NS_CSS_COLOR,           "color");
    inspect_prop_line(out, s, NS_CSS_BACKGROUND_COLOR, "background-color");

    gboolean has_children = box->first_child != NULL;
    if (has_children) {
        g_string_append(out, "    children:\n");
        int idx = 0;
        for (const ns_box *c = box->first_child; c; c = c->next_sibling, idx++) {
            char cl[320];
            inspect_node_label(c->dom, cl, sizeof cl);
            char grow[32] = "";
            if (c->style && c->style->values[NS_CSS_FLEX_GROW]) {
                char gb[32];
                inspect_value_str(c->style->values[NS_CSS_FLEX_GROW], gb, sizeof gb);
                g_snprintf(grow, sizeof grow, "  grow=%s", gb);
            }
            g_string_append_printf(out,
                "      [%d] at %g,%g  %g x %g  <%s>%s\n",
                idx, c->x, c->y, c->content_width, c->content_height, cl, grow);
        }
    }

    GPtrArray *chain = g_ptr_array_new();
    for (const ns_box *p = box; p; p = p->parent)
        g_ptr_array_add(chain, (gpointer)p);
    g_string_append(out, "    path            ");
    for (int i = (int)chain->len - 1; i >= 0; i--) {
        const ns_box *p = g_ptr_array_index(chain, i);
        char pl[320];
        inspect_node_label(p->dom, pl, sizeof pl);
        g_string_append(out, pl);
        if (i > 0) g_string_append(out, " > ");
    }
    g_string_append_c(out, '\n');
    g_ptr_array_free(chain, TRUE);
}

/* Collects elements matching any selector in document order, depth-limited to 512. */
static void
headless_collect_matches(const ns_node *n, GPtrArray *sels, GPtrArray *out,
                         int depth)
{
    if (!n || depth >= 512) return;
    if (n->kind == NS_NODE_ELEMENT && n->name) {
        for (guint i = 0; i < sels->len; i++) {
            if (ns_css_selector_matches(g_ptr_array_index(sels, i), n)) {
                g_ptr_array_add(out, (gpointer)n);
                break;
            }
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        headless_collect_matches(c, sels, out, depth + 1);
}

static GHashTable *g_inspect_styles;

/* Writes an inspect report for every element matching selector, rendered or not. */
static void
headless_inspect(const ns_box *layout, const ns_node *doc, const char *selector,
                 GString *out)
{
    GPtrArray *sels = ns_css_parse_selector_list(selector);
    if (!sels || sels->len == 0) {
        g_string_append_printf(out, "inspect: could not parse selector '%s'\n",
                               selector);
        if (sels) g_ptr_array_free(sels, TRUE);
        return;
    }
    GPtrArray *matches = g_ptr_array_new();
    headless_collect_matches(doc, sels, matches, 0);
    g_string_append_printf(out, "inspect: '%s' matched %u element(s)\n",
                           selector, matches->len);
    for (guint i = 0; i < matches->len; i++) {
        const ns_node *el = g_ptr_array_index(matches, i);
        const ns_box *box = inspect_find_box(layout, el);
        g_string_append_printf(out, "\n--- match %u ---\n", i + 1);
        if (box) {
            inspect_print_box(box, out);
        } else {
            char label[320];
            inspect_node_label(el, label, sizeof label);
            g_string_append_printf(out,
                "  <%s>\n    (not in layout: display:none or non-rendered)\n",
                label);
            const ns_style *s = g_inspect_styles
                ? g_hash_table_lookup(g_inspect_styles, el) : NULL;
            if (s) {
                inspect_prop_line(out, s, NS_CSS_DISPLAY,    "display");
                inspect_prop_line(out, s, NS_CSS_VISIBILITY, "visibility");
                inspect_prop_line(out, s, NS_CSS_POSITION,   "position");
                inspect_prop_line(out, s, NS_CSS_CONTENT,    "content");
            } else {
                g_string_append(out, "    (no style-table entry)\n");
            }
        }
    }
    g_ptr_array_free(matches, TRUE);
    g_ptr_array_free(sels, TRUE);
}

/* Writes the box stack at a point, innermost first, then details of the hit box. */
static void
headless_inspect_at(const ns_box *layout, double x, double y, GString *out)
{
    const ns_box *hit = ns_box_hit_test(layout, x, y);
    g_string_append_printf(out, "inspect-at: %g,%g\n", x, y);
    if (!hit || !hit->dom) {
        g_string_append(out, "  (no element at point)\n");
        return;
    }
    GPtrArray *chain = g_ptr_array_new();
    for (const ns_box *p = hit; p; p = p->parent)
        g_ptr_array_add(chain, (gpointer)p);
    g_string_append(out, "  stack (innermost first):\n");
    for (guint i = 0; i < chain->len; i++) {
        const ns_box *p = g_ptr_array_index(chain, i);
        char label[320];
        inspect_node_label(p->dom, label, sizeof label);
        g_string_append_printf(out, "    %*s<%s>  at %g,%g  %g x %g\n",
                               (int)(i * 2), "", label, p->x, p->y,
                               p->content_width, p->content_height);
    }
    g_ptr_array_free(chain, TRUE);
    g_string_append(out, "\n");
    inspect_print_box(hit, out);
}

/* Legacy in-process run: fetch, parse, script, settle, follow navs, then dump or capture. */
static int
ns_headless_run_one(const ns_headless_opts *opts, const char *fetch_url, int hop,
                    const char *post_body, gsize post_len, const char *post_ct,
                    const char *top_url, gboolean user_activated)
{
    GError *err = NULL;
    ns_net_set_navigation_fetch(TRUE, user_activated);
    ns_response *resp = post_body
        ? ns_engine_post_blocking(fetch_url, top_url, post_body, post_len,
                                  post_ct, &err)
        : ns_engine_fetch_blocking(fetch_url, top_url, &err);
    ns_net_set_navigation_fetch(FALSE, FALSE);
    if (!resp) {
        const char *emsg = err ? err->message : "unknown error";
        fprintf(stderr, "headless: fetch failed: %s\n", emsg);
        if (opts->dump == NS_DUMP_PNG || opts->dump == NS_DUMP_PDF ||
            opts->dump == NS_DUMP_PRINT) {
            resp = g_new0(ns_response, 1);
            resp->body = g_byte_array_new();
            resp->final_url = g_strdup(opts->url ? opts->url : "");
            resp->content_type = g_strdup("text/html; charset=utf-8");
            char *html = ns_build_error_page(opts->url, 0, emsg);
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_clear_error(&err);
        } else {
            g_clear_error(&err);
            return 1;
        }
    } else if (resp->error) {
        fprintf(stderr, "headless: fetch error: %s\n", resp->error);
        if (opts->dump == NS_DUMP_PNG || opts->dump == NS_DUMP_PDF ||
            opts->dump == NS_DUMP_PRINT) {
            char *html = ns_build_error_page(
                resp->final_url ? resp->final_url : opts->url,
                resp->status, resp->error);
            if (resp->body) g_byte_array_set_size(resp->body, 0);
            else            resp->body = g_byte_array_new();
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
        } else {
            ns_response_free(resp);
            return 1;
        }
    } else if (resp->status >= 400) {
        gboolean body_is_html =
            resp->content_type &&
            (g_ascii_strncasecmp(resp->content_type, "text/html", 9) == 0 ||
             g_ascii_strncasecmp(resp->content_type, "application/xhtml", 17) == 0);
        gboolean body_useful = resp->body && resp->body->len > 64 && body_is_html;
        if (!body_useful &&
            (opts->dump == NS_DUMP_PNG || opts->dump == NS_DUMP_PDF ||
             opts->dump == NS_DUMP_PRINT)) {
            char *html = ns_build_error_page(
                resp->final_url ? resp->final_url : opts->url,
                resp->status, NULL);
            if (resp->body) g_byte_array_set_size(resp->body, 0);
            else            resp->body = g_byte_array_new();
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
        }
    }

    if (resp->content_type &&
        g_ascii_strncasecmp(resp->content_type, "image/", 6) == 0 &&
        !ns_html_mime_is_xml(resp->content_type) &&
        resp->body && resp->body->len > 0) {
        char *html = ns_html_image_document(
            resp->final_url ? resp->final_url : fetch_url);
        g_byte_array_set_size(resp->body, 0);
        g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
        g_free(html);
        g_free(resp->content_type);
        resp->content_type = g_strdup("text/html; charset=utf-8");
    }

    if (resp->content_type && resp->body && resp->body->len > 0) {
        const char *ct = resp->content_type;
        gboolean is_json = strstr(ct, "json") != NULL;
        if (is_json) {
            char *decoded = ns_html_decode_body_full(
                (const char *)resp->body->data, resp->body->len, ct, NULL);
            char *html = ns_html_json_document(
                resp->final_url ? resp->final_url : fetch_url, decoded,
                decoded ? strlen(decoded) : 0);
            g_free(decoded);
            if (html) {
                g_byte_array_set_size(resp->body, 0);
                g_byte_array_append(resp->body, (const guint8 *)html,
                                    strlen(html));
                g_free(html);
                g_free(resp->content_type);
                resp->content_type = g_strdup("text/html; charset=utf-8");
            }
        }
    }

    const char *raw = resp->body ? (const char *)resp->body->data : "";
    gsize raw_len = resp->body ? resp->body->len : 0;
    g_free(g_headless_doc_charset);
    g_headless_doc_charset = NULL;
    char *decoded = ns_html_decode_body_full(raw, raw_len,
                                             resp->content_type,
                                             &g_headless_doc_charset);
    ns_node *doc = ns_html_parse_document(
        decoded ? decoded : "", decoded ? (gssize)strlen(decoded) : 0,
        resp->content_type);
    const char *page_url = resp->final_url ? resp->final_url : opts->url;

    ns_print_setup_default(&g_headless_print_setup);
    ns_css_set_print_media(opts->dump == NS_DUMP_PRINT);

    int vw = opts->viewport_width > 0 ? opts->viewport_width : 1000;
    double vh = opts->viewport_height > 0 ? (double)opts->viewport_height
                                          : (double)vw * 0.75;
    if (opts->dump == NS_DUMP_PRINT && opts->viewport_width <= 0) {
        const ns_print_setup *ps = &g_headless_print_setup;
        vw = (int)(ps->width - ps->margin_left - ps->margin_right);
        vh = ps->height - ps->margin_top - ps->margin_bottom;
    }
    ns_css_set_viewport((double)vw, vh);
    ns_css_media_device media_device = {
        .width = vw,
        .height = vh,
        .resolution_dppx = 1,
        .color_bits = 8,
        .pointer = NS_CSS_MEDIA_POINTER_FINE,
        .any_pointer = NS_CSS_MEDIA_POINTER_FINE,
        .update = NS_CSS_MEDIA_UPDATE_FAST,
        .hover = TRUE,
        .any_hover = TRUE,
    };
    ns_css_set_media_device(&media_device);
    const char *frag = opts->url ? strchr(opts->url, '#') : NULL;
    const char *target_frag = frag && *(frag + 1) ? frag + 1 : NULL;
    ns_css_set_target_fragment(target_frag);
    ns_css_set_doc_language(resp->content_language);
    if (target_frag) headless_reveal_fragment(doc, target_frag);
    GHashTable *css_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                              (GDestroyNotify)g_bytes_unref);
    GHashTable *styles = ns_engine_compute_cascade(doc, page_url, css_cache, NULL);

    ns_anim *anim = ns_anim_new();
    ns_engine_load_keyframes(anim, doc, page_url, css_cache);
    ns_engine_anim_observe(anim, styles, g_get_monotonic_time());

    headless_nav_capture nav_cap = {0};
    ns_js_navigation_timing navigation_timing =
        ns_js_navigation_timing_from_response(resp);
    ns_js *js = ns_js_new(headless_js_log, NULL,
                          headless_js_mutated, NULL,
                          headless_js_navigate, &nav_cap,
                          &navigation_timing);
    nav_cap.js = js;
    if (js) ns_js_set_form_submit_cb(js, headless_js_form_submit, &nav_cap);
    ns_image_cache *image_cache = ns_image_cache_new();
    ns_box *layout = NULL;
    const char *flush_base = resp->final_url ? resp->final_url : opts->url;
    headless_flush_ctx flush_ctx = {
        .doc = doc, .js = js, .base = flush_base, .vw = vw, .vh = vh,
        .image_cache = image_cache, .anim = anim,
        .css_cache = css_cache, .styles = &styles, .layout = &layout,
    };
    if (js) {
        ns_js_set_style_table(js, styles);
        ns_js_set_image_cache(js, image_cache);
        ns_js_set_anim(js, anim);
        ns_js_set_layout_flush_cb(js, headless_flush_layout, &flush_ctx);
        ns_js_set_style_flush_cb(js, headless_flush_style, &flush_ctx);
        if (opts->wpt) ns_js_set_early_inject_src(js, ns_wpt_hook_src);
        headless_relayout(&flush_ctx);
        ns_engine_fetch_images(layout, flush_base, image_cache, js);
        ns_js_run_scripts_in_doc(js, doc, resp->final_url,
                                 g_headless_doc_charset, resp->content_type);
        ns_media_scan(js, doc, resp->final_url);
        headless_relayout(&flush_ctx);
        ns_js_fire_media_load_events(js, layout);
    }

    if (opts->settle_ms > 0) settle_main_loop(opts->settle_ms, &flush_ctx);

    if (opts->actions && *opts->actions)
        headless_run_actions(&flush_ctx, &nav_cap, opts->actions);

    if (nav_cap.pending_url && hop < 4 && !opts->wpt) {
        char *next = NULL;
        if (strstr(nav_cap.pending_url, "://")) {
            next = g_strdup(nav_cap.pending_url);
        } else {
            const char *base = resp->final_url ? resp->final_url : fetch_url;
            next = ns_url_resolve(base, nav_cap.pending_url);
            if (!next) next = g_strdup(nav_cap.pending_url);
        }
        char *next_post_body = nav_cap.pending_post_body;
        gsize next_post_len = nav_cap.pending_post_len;
        char *next_post_ct = nav_cap.pending_post_ct;
        char *next_top_url = g_strdup(resp->final_url
            ? resp->final_url : fetch_url);
        nav_cap.pending_post_body = NULL;
        nav_cap.pending_post_ct = NULL;
        nav_cap.pending_post_len = 0;
        fprintf(stderr, "[headless follow%s %s]\n",
                next_post_body ? " POST" : "", next);
        g_free(nav_cap.pending_url);
        nav_cap.pending_url = NULL;
        if (js) {
            ns_js_set_layout_flush_cb(js, NULL, NULL);
            ns_js_set_style_flush_cb(js, NULL, NULL);
        }
        if (js)            ns_js_set_layout_root(js, NULL);
        if (js)            ns_js_set_style_table(js, NULL);
        if (anim)          ns_anim_free(anim);
        if (layout)        { ns_paint_3d_invalidate(); ns_box_free(layout); }
        if (styles)        g_hash_table_destroy(styles);
        if (css_cache)     g_hash_table_destroy(css_cache);
        if (js)            ns_js_free(js);
        if (doc)           ns_node_free(doc);
        if (image_cache)   ns_image_cache_free(image_cache);
        g_free(decoded);
        ns_response_free(resp);
        int rc2 = ns_headless_run_one(opts, next, hop + 1,
                                      next_post_body, next_post_len,
                                      next_post_ct, next_top_url,
                                      nav_cap.user_activated);
        g_free(next);
        g_free(next_top_url);
        g_free(next_post_body);
        g_free(next_post_ct);
        return rc2;
    }

    headless_relayout(&flush_ctx);
    if (js && opts->settle_ms > 0) {
        settle_main_loop(opts->settle_ms, &flush_ctx);
        headless_relayout(&flush_ctx);
    }

    int wpt_rc = 0;
    if (js && opts->wpt)
        wpt_rc = headless_wpt_finish(&flush_ctx, opts);

    if (js && opts->eval && *opts->eval) {
        char *result = ns_js_eval_source(js, opts->eval, "headless-eval");
        if (result) {
            fprintf(stdout, "eval: %s\n", result);
            g_free(result);
        }
        if (ns_js_consume_mutated(js))
            headless_relayout(&flush_ctx);
    }

    int rc = wpt_rc;
    GString *out = g_string_new(NULL);

    switch (opts->dump) {
    case NS_DUMP_NONE:
        break;
    case NS_DUMP_TEXT:
        ns_engine_dump_text(layout, out);
        fwrite(out->str, 1, out->len, stdout);
        break;
    case NS_DUMP_DOM: {
        GString *dom = ns_node_dump(doc);
        fwrite(dom->str, 1, dom->len, stdout);
        g_string_free(dom, TRUE);
        break;
    }
    case NS_DUMP_LAYOUT:
        ns_engine_dump_layout(layout, 0, out);
        fwrite(out->str, 1, out->len, stdout);
        break;
    case NS_DUMP_PNG:
    case NS_DUMP_PDF:
    case NS_DUMP_PRINT: {
        const char *base = resp->final_url ? resp->final_url : opts->url;
        if (!image_cache) image_cache = ns_image_cache_new();
        ns_engine_fetch_images(layout, base, image_cache, js);
        headless_relayout(&flush_ctx);
        if (opts->dump == NS_DUMP_PRINT) {
            ns_print_setup_apply_page_rule(&g_headless_print_setup,
                                           ns_render_page_rule());
            const ns_print_setup *ps = &g_headless_print_setup;
            double w = ps->width - ps->margin_left - ps->margin_right;
            if (w > 0 && opts->viewport_width <= 0 && (int)w != vw) {
                vw = (int)w;
                vh = ps->height - ps->margin_top - ps->margin_bottom;
                ns_css_set_viewport((double)vw, vh);
                headless_relayout(&flush_ctx);
            }
        }
        ns_paint_set_js(js);

        int time_ms = opts->time_ms >= 0 ? opts->time_ms : 1000;

        ns_anim_rebase(anim, 0);
        ns_anim_tick(anim, 0);
        ns_paint_set_anim(anim);
        char *initial_path = ns_engine_suffix_before_ext(opts->out_path, "-initial");
        rc = write_capture(layout, initial_path, opts->dump);
        fprintf(stderr, "[headless] initial render -> %s\n", initial_path);
        g_free(initial_path);

        if (js) ns_js_fire_media_load_events(js, layout);
        settle_main_loop(time_ms, &flush_ctx);
        headless_relayout(&flush_ctx);
        ns_anim_rebase(anim, 0);
        for (gint64 t = 0; t <= (gint64)time_ms * 1000; t += 16000)
            ns_anim_tick(anim, t);
        ns_anim_tick(anim, (gint64)time_ms * 1000);
        ns_paint_set_anim(anim);
        int rc2 = write_capture(layout, opts->out_path, opts->dump);
        fprintf(stderr, "[headless] after %dms -> %s\n", time_ms, opts->out_path);
        if (rc == 0) rc = rc2;
        break;
    }
    }
    g_string_free(out, TRUE);

    if ((opts->inspect && *opts->inspect) ||
        (opts->inspect_at && *opts->inspect_at)) {
        GString *report = g_string_new(NULL);
        g_inspect_styles = styles;
        if (opts->inspect && *opts->inspect)
            headless_inspect(layout, doc, opts->inspect, report);
        g_inspect_styles = NULL;
        if (opts->inspect_at && *opts->inspect_at) {
            double ix = 0, iy = 0;
            if (sscanf(opts->inspect_at, "%lf , %lf", &ix, &iy) == 2)
                headless_inspect_at(layout, ix, iy, report);
            else
                g_string_append_printf(report,
                    "inspect-at: bad coordinate '%s' (expected X,Y)\n",
                    opts->inspect_at);
        }
        fwrite(report->str, 1, report->len, stdout);
        g_string_free(report, TRUE);
    }

    g_free(decoded);
    g_free(nav_cap.pending_url);
    headless_nav_capture_clear_post(&nav_cap);
    ns_paint_set_anim(NULL);
    if (anim)          ns_anim_free(anim);
    if (js)            ns_js_set_layout_root(js, NULL);
    if (js)            ns_js_set_style_table(js, NULL);
    if (layout)        { ns_paint_3d_invalidate(); ns_box_free(layout); }
    if (styles)        g_hash_table_destroy(styles);
    if (css_cache)     g_hash_table_destroy(css_cache);
    if (js)            ns_js_free(js);
    if (doc)           ns_node_free(doc);
    if (image_cache)   ns_image_cache_free(image_cache);
    ns_response_free(resp);
    return rc;
}
