/* Northstar Web Browser — GTK view over the page engine, driven on the engine thread.
 * Posts load, render, input and find requests; blits frames and damage; print, devtools.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "procview.h"
#include "i18n.h"

#include "proc_limits.h"
#include "trace.h"
#include "enginethread.h"
#include "page_session.h"
#include "videolayer.h"
#include "net.h"

#include <cairo.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

#define NS_PROC_CARET_BLINK_US (530 * 1000)
/* Page animations run at most at 60 fps. A 60 Hz display's frame times
 * jitter around 1/60 s, so the cap allows a quarter frame of slack: a tick
 * that comes a few microseconds early must not be dropped. */
#define NS_PROC_ANIM_FRAME_US  (G_USEC_PER_SEC / 60)
#define NS_PROC_ANIM_FRAME_SLACK_US (NS_PROC_ANIM_FRAME_US / 4)

#ifndef G_OS_WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Page settle time in ms from the env override (0..10000), else NS_PROC_SETTLE_MS. */
static int
pv_settle_ms(void)
{
    const char *e = g_getenv(NS_PROC_SETTLE_ENV);
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v <= 10000)
            return v;
    }
    return NS_PROC_SETTLE_MS;
}

typedef enum {
    REQ_LOAD, REQ_RENDER, REQ_LINK, REQ_CLICK, REQ_VIEWPORT, REQ_KEY,
    REQ_SELECT, REQ_HOVER, REQ_RELEASE, REQ_FIND, REQ_EXPORT, REQ_CONSOLE,
    REQ_EVAL, REQ_DUMP, REQ_DROPFILES, REQ_SCROLL, REQ_SCROLLBAR,
    REQ_CAMERA, REQ_PRINT, REQ_CLOSE
} ReqType;
typedef enum { ACT_HOVER, ACT_NAVIGATE, ACT_CONTEXT } LinkAct;

enum {
    DEV_TAB_CONSOLE = 0,
    DEV_TAB_NETWORK,
    DEV_TAB_PERFORMANCE,
    DEV_TAB_LAYOUT,
    DEV_TAB_ELEMENTS
};

typedef struct {
    NsProcView *view;
    ReqType type;
    int     seq;
    char   *url;
    int     vw;
    int     vh;
    int     w, h, sx, sy;
    double  scale;
    int     x, y;
    int     dx, dy;
    int     mods;
    int     kind;
    int     keycode;
    int     fallback_scroll;
    double  fallback_x;
    double  fallback_y;
    char   *key;
    char   *code;
    LinkAct action;
    char   *query;
    int     find_dir;
    int     find_from_y;
    int     find_case;
    char   *export_dest;
    char   *paths;
    int     dump_tab;
    gboolean inspect;
    gboolean history;
    gboolean user_activated;
    gboolean caret_active;
    gboolean force_full;
    gint64  frame_time_us;
} Req;

typedef enum {
    RES_PAGE, RES_FRAME, RES_LINK, RES_CLICK, RES_VIEWPORT, RES_KEY,
    RES_SELECT, RES_COPY, RES_HOVER, RES_RELEASE, RES_FIND, RES_EXPORT,
    RES_CONSOLE, RES_EVAL, RES_DUMP, RES_DROPFILES, RES_SCROLL,
    RES_SCROLLBAR, RES_PRINT
} ResType;

typedef struct {
    NsProcView      *view;
    ResType          type;
    int              seq;
    gboolean         ok;
    int              pw, ph;
    char            *title;
    char            *url;
    gboolean         url_pushed;
    char            *nav;
    int              security;
    char            *remote_ip;
    char            *camera;
    char            *download;
    char            *clipboard;
    cairo_surface_t *surface;
    gboolean         surface_borrowed;
    cairo_surface_t *patch;
    int              patch_x, patch_y;
    int             *damage;
    int              n_damage;
    guint64          gen, base_gen;
    char            *href;
    char            *cursor;
    LinkAct          action;
    int              kind;
    int              prevented;
    int              fallback_scroll;
    double           fallback_x;
    double           fallback_y;
    gboolean         animating;
    gboolean         caret_blinking;
    int              requested_scroll_y, requested_scroll_x;
    int              find_total, find_current, find_scroll_y;
    char            *media_url;
    int              media_is_video, media_stream;
    int              dump_tab;
    gboolean         inspect;
    GPtrArray       *print_pages;
    ns_print_setup   print_setup;
    GArray          *video_layers;
} Res;

struct NsProcView {
    gatomicrefcount rc;

    GtkWidget     *root;
    GtkWidget     *area;
    GtkWidget     *hscroll;
    GtkWidget     *vscroll;
    GtkIMContext  *im;
    GtkAdjustment *hadj;
    GtkAdjustment *vadj;
    gboolean    closed;

    ns_page_session *session;
    gboolean    private_mode;

    NsProcNotify notify;
    gpointer     notify_ud;

    char       *current_url;
    char       *current_title;
    int         security;
    char       *remote_ip;
    int         page_w, page_h;
    int         scroll_x, scroll_y;
    gboolean    opened;

    cairo_surface_t *frame;
    GArray          *video_layers;
    GHashTable      *video_surfaces;
    guint64          frame_gen;
    gboolean         frame_force_full;

    gboolean    render_inflight;
    gboolean    render_pending;

    gboolean    viewport_inflight;
    gboolean    viewport_pending;

    gboolean    scroll_inflight;
    gboolean    scroll_pending;
    int         scroll_seq;
    double      scroll_pending_dx, scroll_pending_dy;

    gboolean    link_inflight;
    gboolean    link_pending;
    int         link_pending_x, link_pending_y;
    LinkAct     link_pending_action;

    gboolean    hover_inflight;
    gboolean    hover_pending;
    int         hover_pending_x, hover_pending_y;

    gboolean    has_selection;
    gboolean    multi_click;
    double      ctx_x, ctx_y;
    char       *ctx_link;
    GtkWidget  *ctx_popover;
    GSimpleActionGroup *ctx_actions;

    GtkWidget  *search_revealer;
    GtkWidget  *search_entry;
    GtkWidget  *search_label;
    int         find_seq;
    gboolean    find_case;

    GtkWidget  *overlay;
    GtkWidget  *perm_revealer;
    GtkWidget  *perm_label;
    ReqType     perm_kind;
    gboolean    perm_pending;
    char       *perm_origin;

    GtkWidget    *console_window;
    GtkWidget    *console_notebook;
    GtkWidget    *console_entry;
    GtkWidget    *console_view;
    GtkTextBuffer *console_buffer;
    GtkWidget    *net_view;
    GtkTextBuffer *net_buffer;
    GtkWidget    *perf_view;
    GtkTextBuffer *perf_buffer;
    GtkWidget    *layout_view;
    GtkTextBuffer *layout_buffer;
    GtkWidget    *elements_view;
    GtkTextBuffer *elements_buffer;
    GtkWidget    *inspect_entry;
    gboolean      console_open;
    guint         console_poll_id;

    GPtrArray  *history;
    int         hist_index;
    gboolean    pending_record;

    char       *deferred_url;
    gboolean    deferred_record;
    gboolean    deferred_history;
    gboolean    deferred_user_activated;

    int         js_redirects;

    double      scale;
    gboolean    loading;
    gboolean    busy_cursor;
    GdkCursor  *hourglass_cursor;

    int         load_seq, render_seq, link_seq, click_seq, viewport_seq;
    int         key_seq, select_seq, hover_seq;
    int         last_vp_w, last_vp_h;
    double      drag_start_x, drag_start_y;
    double      pointer_x, pointer_y;
    gboolean    drag_anchored;
    gboolean    sb_probe, sb_dragging, sb_have_last;
    double      sb_last_x, sb_last_y;

    guint       anim_tick_id;
    gint64      last_anim_frame_us;
    gboolean    page_animating;
    gboolean    caret_blinking;
};

enum {
    NS_PV_ZOOM_MIN_PERMILLE = (int)(NS_PROC_ZOOM_MIN * 1000.0 + 0.5),
    NS_PV_ZOOM_MAX_PERMILLE = (int)(NS_PROC_ZOOM_MAX * 1000.0 + 0.5)
};

static NsProcView *pv_ref(NsProcView *v) { g_atomic_ref_count_inc(&v->rc); return v; }

static void
set_accessible_label(GtkWidget *w, const char *label)
{
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

/* Adds the two hourglass bulb outlines to the path, offset by (ox, oy) on a 32px grid. */
static void
hourglass_bulbs_path(cairo_t *cr, double ox, double oy)
{
    cairo_move_to(cr, 14.6 + ox, 16.0 + oy);
    cairo_curve_to(cr, 12.0 + ox, 13.2 + oy, 9.9 + ox, 10.6 + oy, 10.1 + ox, 8.0 + oy);
    cairo_curve_to(cr, 10.0 + ox, 6.8 + oy, 10.8 + ox, 6.1 + oy, 12.2 + ox, 6.1 + oy);
    cairo_curve_to(cr, 14.2 + ox, 5.6 + oy, 17.8 + ox, 5.6 + oy, 19.8 + ox, 6.1 + oy);
    cairo_curve_to(cr, 21.2 + ox, 6.1 + oy, 22.0 + ox, 6.8 + oy, 21.9 + ox, 8.0 + oy);
    cairo_curve_to(cr, 22.1 + ox, 10.6 + oy, 20.0 + ox, 13.2 + oy, 17.4 + ox, 16.0 + oy);
    cairo_close_path(cr);
    cairo_move_to(cr, 14.6 + ox, 16.0 + oy);
    cairo_curve_to(cr, 12.0 + ox, 18.8 + oy, 9.9 + ox, 21.4 + oy, 10.1 + ox, 24.0 + oy);
    cairo_curve_to(cr, 10.0 + ox, 25.2 + oy, 10.8 + ox, 25.9 + oy, 12.2 + ox, 25.9 + oy);
    cairo_curve_to(cr, 14.2 + ox, 26.4 + oy, 17.8 + ox, 26.4 + oy, 19.8 + ox, 25.9 + oy);
    cairo_curve_to(cr, 21.2 + ox, 25.9 + oy, 22.0 + ox, 25.2 + oy, 21.9 + ox, 24.0 + oy);
    cairo_curve_to(cr, 22.1 + ox, 21.4 + oy, 20.0 + ox, 18.8 + oy, 17.4 + ox, 16.0 + oy);
    cairo_close_path(cr);
}

static void
hourglass_caps_path(cairo_t *cr, double ox, double oy)
{
    cairo_move_to(cr, 9.7 + ox, 6.2 + oy);
    cairo_line_to(cr, 22.3 + ox, 6.2 + oy);
    cairo_move_to(cr, 9.7 + ox, 25.8 + oy);
    cairo_line_to(cr, 22.3 + ox, 25.8 + oy);
}

/* Renders the 32x32 hourglass cursor image into a new GdkTexture; NULL on failure. */
static GdkTexture *
hourglass_texture(void)
{
    const int size = 32;
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.22);
    cairo_set_line_width(cr, 3.0);
    hourglass_caps_path(cr, 0.7, 0.9);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 2.6);
    hourglass_bulbs_path(cr, 0.7, 0.9);
    cairo_stroke(cr);

    cairo_pattern_t *glass = cairo_pattern_create_linear(16.0, 7.0, 16.0, 26.0);
    cairo_pattern_add_color_stop_rgba(glass, 0.0, 0.97, 1.0, 1.0, 0.55);
    cairo_pattern_add_color_stop_rgba(glass, 0.5, 0.62, 0.80, 0.95, 0.30);
    cairo_pattern_add_color_stop_rgba(glass, 1.0, 0.93, 0.98, 1.0, 0.52);
    cairo_set_source(cr, glass);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_fill(cr);
    cairo_pattern_destroy(glass);

    cairo_pattern_t *sand = cairo_pattern_create_linear(16.0, 7.0, 16.0, 25.0);
    cairo_pattern_add_color_stop_rgba(sand, 0.0, 1.0, 0.85, 0.40, 0.98);
    cairo_pattern_add_color_stop_rgba(sand, 1.0, 0.90, 0.52, 0.14, 0.98);
    cairo_set_source(cr, sand);
    cairo_move_to(cr, 11.7, 8.0);
    cairo_curve_to(cr, 14.0, 7.6, 18.0, 7.6, 20.3, 8.0);
    cairo_curve_to(cr, 18.9, 10.9, 17.4, 13.3, 16.0, 15.3);
    cairo_curve_to(cr, 14.6, 13.3, 13.1, 10.9, 11.7, 8.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_move_to(cr, 11.7, 24.0);
    cairo_curve_to(cr, 13.5, 21.6, 14.8, 20.9, 16.0, 20.9);
    cairo_curve_to(cr, 17.2, 20.9, 18.5, 21.6, 20.3, 24.0);
    cairo_curve_to(cr, 17.6, 24.7, 14.4, 24.7, 11.7, 24.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.1);
    cairo_move_to(cr, 16.0, 15.3);
    cairo_line_to(cr, 16.0, 20.9);
    cairo_stroke(cr);
    cairo_pattern_destroy(sand);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.80);
    cairo_set_line_width(cr, 0.9);
    cairo_move_to(cr, 12.6, 8.2);
    cairo_curve_to(cr, 13.7, 10.4, 14.6, 12.2, 15.3, 13.8);
    cairo_stroke(cr);
    cairo_move_to(cr, 16.7, 18.4);
    cairo_curve_to(cr, 17.6, 20.2, 18.7, 22.2, 19.6, 24.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.10, 0.12, 0.14, 0.98);
    cairo_set_line_width(cr, 2.0);
    hourglass_caps_path(cr, 0.0, 0.0);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.5);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.74, 0.82, 0.90, 0.85);
    cairo_set_line_width(cr, 0.6);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_flush(surface);

    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    GBytes *bytes = g_bytes_new(data, (gsize)stride * (gsize)size);
    GdkTexture *texture =
        gdk_memory_texture_new(size, size, GDK_MEMORY_DEFAULT, bytes,
                               (gsize)stride);
    g_bytes_unref(bytes);
    cairo_surface_destroy(surface);
    return texture;
}

/* Lazily builds and caches the hourglass cursor, falling back to the themed "wait" cursor. */
static GdkCursor *
busy_hourglass_cursor(NsProcView *v)
{
    if (v->hourglass_cursor)
        return v->hourglass_cursor;

    GdkTexture *texture = hourglass_texture();
    if (texture) {
        GdkCursor *fallback = gdk_cursor_new_from_name("wait", NULL);
        v->hourglass_cursor =
            gdk_cursor_new_from_texture(texture, 16, 16, fallback);
        if (fallback)
            g_object_unref(fallback);
        g_object_unref(texture);
    }
    if (!v->hourglass_cursor)
        v->hourglass_cursor = gdk_cursor_new_from_name("wait", NULL);
    return v->hourglass_cursor;
}

/* Sets a CSS cursor by name with a fallback table for names GDK themes often lack. */
static void
pv_set_named_cursor(GtkWidget *w, const char *name)
{
    if (!name || !*name) {
        gtk_widget_set_cursor(w, NULL);
        return;
    }
    static const struct { const char *name; const char *fallback; } fb[] = {
        { "context-menu",  "default" },
        { "help",          "default" },
        { "progress",      "wait" },
        { "cell",          "crosshair" },
        { "vertical-text", "text" },
        { "alias",         "copy" },
        { "copy",          "default" },
        { "move",          "all-scroll" },
        { "no-drop",       "not-allowed" },
        { "not-allowed",   "default" },
        { "grab",          "all-scroll" },
        { "grabbing",      "all-scroll" },
        { "all-scroll",    "move" },
        { "col-resize",    "ew-resize" },
        { "row-resize",    "ns-resize" },
        { "ne-resize",     "nesw-resize" },
        { "sw-resize",     "nesw-resize" },
        { "nw-resize",     "nwse-resize" },
        { "se-resize",     "nwse-resize" },
        { "nesw-resize",   "crosshair" },
        { "nwse-resize",   "crosshair" },
        { "zoom-in",       "crosshair" },
        { "zoom-out",      "crosshair" },
    };
    const char *fallback = NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(fb); i++)
        if (strcmp(name, fb[i].name) == 0) { fallback = fb[i].fallback; break; }
    GdkCursor *fb_cur = fallback ? gdk_cursor_new_from_name(fallback, NULL) : NULL;
    GdkCursor *cur = gdk_cursor_new_from_name(name, fb_cur);
    gtk_widget_set_cursor(w, cur ? cur : fb_cur);
    if (cur)
        g_object_unref(cur);
    if (fb_cur)
        g_object_unref(fb_cur);
}

/* Marks the view busy and shows the hourglass (or "wait") cursor on the drawing area. */
static void
set_busy_cursor(NsProcView *v)
{
    v->busy_cursor = TRUE;
    if (!v->area)
        return;

    GdkCursor *cursor = busy_hourglass_cursor(v);
    if (cursor)
        gtk_widget_set_cursor(v->area, cursor);
    else
        gtk_widget_set_cursor_from_name(v->area, "wait");
}

static void request_render(NsProcView *v);
static void print_run(NsProcView *v, GPtrArray *pages,
                      const ns_print_setup *setup);

static void
pv_set_video_layers(NsProcView *v, GArray *layers)
{
    if (v->video_layers)
        g_array_free(v->video_layers, TRUE);
    v->video_layers = layers;
}

/* Final teardown of the view once its refcount drops to zero; frees surfaces and state. */
static void
pv_free(NsProcView *v)
{
    if (v->frame)
        cairo_surface_destroy(v->frame);
    v->frame = NULL;
    pv_set_video_layers(v, NULL);
    if (v->video_surfaces)
        g_hash_table_destroy(v->video_surfaces);
    if (v->ctx_popover)
        gtk_widget_unparent(v->ctx_popover);
    if (v->ctx_actions)
        g_object_unref(v->ctx_actions);
    g_free(v->ctx_link);
    if (v->history)
        g_ptr_array_unref(v->history);
    g_free(v->current_url);
    g_free(v->current_title);
    g_free(v->remote_ip);
    g_free(v->deferred_url);
    g_free(v->perm_origin);
    if (v->hourglass_cursor)
        g_object_unref(v->hourglass_cursor);
    g_free(v);
}

static void pv_unref(NsProcView *v) { if (g_atomic_ref_count_dec(&v->rc)) pv_free(v); }

/* Copies a full engine frame into a new ARGB32 cairo surface; NULL on allocation failure. */
static cairo_surface_t *
stage_fill(const unsigned char *px, int w, int h, int stride)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int dstride = cairo_image_surface_get_stride(s);
    size_t row = (size_t)w * 4u;
    if (dstride == stride && (size_t)stride == row) {
        memcpy(dst, px, row * (size_t)h);
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * dstride, px + (size_t)y * stride, row);
    }
    cairo_surface_mark_dirty(s);
    return s;
}

/* Copies damage rects into one surface covering their bounding box; returns its origin. */
static cairo_surface_t *
stage_patch(const unsigned char *px, int stride, const int *rects, int n,
            int *out_x, int *out_y)
{
    int x0 = G_MAXINT, y0 = G_MAXINT, x1 = G_MININT, y1 = G_MININT;
    for (int i = 0; i < n; i++) {
        x0 = MIN(x0, rects[4 * i]);
        y0 = MIN(y0, rects[4 * i + 1]);
        x1 = MAX(x1, rects[4 * i] + rects[4 * i + 2]);
        y1 = MAX(y1, rects[4 * i + 1] + rects[4 * i + 3]);
    }
    if (x1 <= x0 || y1 <= y0)
        return NULL;
    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, x1 - x0, y1 - y0);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int dstride = cairo_image_surface_get_stride(s);
    for (int i = 0; i < n; i++) {
        int rx = rects[4 * i], ry = rects[4 * i + 1];
        size_t row = (size_t)rects[4 * i + 2] * 4u;
        for (int y = ry; y < ry + rects[4 * i + 3]; y++)
            memcpy(dst + (size_t)(y - y0) * dstride + (size_t)(rx - x0) * 4u,
                   px + (size_t)y * stride + (size_t)rx * 4u, row);
    }
    cairo_surface_mark_dirty(s);
    *out_x = x0;
    *out_y = y0;
    return s;
}

static void
post_emit(NsProcView *v, NsProcEvent evt, const char *text)
{
    if (v->notify)
        v->notify(v, evt, text, v->notify_ud);
}

static void
clear_busy_cursor(NsProcView *v)
{
    if (!v->busy_cursor)
        return;
    v->busy_cursor = FALSE;
    if (v->area)
        gtk_widget_set_cursor_from_name(v->area, NULL);
}

/* Clears the loading flag and emits LOADING "0" if a load was in progress. */
static void
finish_loading(NsProcView *v)
{
    if (v->loading) {
        v->loading = FALSE;
        post_emit(v, NS_PROC_EVT_LOADING, "0");
    }
}

/* Drops in-flight and pending renders by bumping the sequence and ends the loading state. */
void
ns_proc_view_stop(NsProcView *v)
{
    if (!v)
        return;
    v->render_seq++;
    v->render_inflight = FALSE;
    v->render_pending = FALSE;
    finish_loading(v);
    clear_busy_cursor(v);
}

static gboolean on_result(gpointer data);

/* Blits a damage patch onto the cached frame if generations match; FALSE to refetch. */
static gboolean
apply_patch(NsProcView *v, const Res *res)
{
    if (!v->frame || v->frame_gen != res->base_gen)
        return FALSE;
    int fw = cairo_image_surface_get_width(v->frame);
    int fh = cairo_image_surface_get_height(v->frame);
    cairo_t *cr = cairo_create(v->frame);
    for (int i = 0; i < res->n_damage; i++) {
        const int *r = res->damage + 4 * i;
        if (r[0] < 0 || r[1] < 0 || r[0] + r[2] > fw || r[1] + r[3] > fh) {
            cairo_destroy(cr);
            return FALSE;
        }
        cairo_rectangle(cr, r[0], r[1], r[2], r[3]);
    }
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, res->patch, res->patch_x, res->patch_y);
    cairo_paint(cr);
    cairo_destroy(cr);
    v->frame_gen = res->gen;
    return TRUE;
}

static void
post(Res *res)
{
    g_idle_add(on_result, res);
}

static gboolean
pv_unref_idle(gpointer data)
{
    pv_unref(data);
    return G_SOURCE_REMOVE;
}

static Res *
res_new(NsProcView *v, ResType type, int seq)
{
    Res *res = g_new0(Res, 1);
    res->view = pv_ref(v);
    res->type = type;
    res->seq = seq;
    return res;
}

static void
req_free(Req *req)
{
    g_free(req->url);
    g_free(req->key);
    g_free(req->code);
    g_free(req->query);
    g_free(req->export_dest);
    g_free(req->paths);
    g_free(req);
}

/* Engine thread: opens the URL in the page session and posts a RES_PAGE result. */
static void
run_load(NsProcView *v, ns_page_session *s, Req *req)
{
    Res *res = res_new(v, RES_PAGE, req->seq);
    ns_page_info pg;
    if (ns_page_session_open(s, req->url, req->vw, req->vh, pv_settle_ms(),
                             req->history, req->user_activated, &pg) == 0 &&
        pg.ok) {
        res->ok = TRUE;
        res->pw = pg.page_width;
        res->ph = pg.page_height;
        res->title = g_strdup(pg.title ? pg.title : "");
        res->url = g_strdup(pg.url ? pg.url : req->url);
        res->nav = pg.nav ? g_strdup(pg.nav) : NULL;
        res->security = pg.security;
        res->remote_ip = pg.remote_ip ? g_strdup(pg.remote_ip) : NULL;
    }
    ns_page_info_clear(&pg);
    post(res);
}

/* Engine thread: renders a frame, staging a damage patch or full copy, plus side data. */
static void
run_render(NsProcView *v, ns_page_session *s, Req *req)
{
    Res *res = res_new(v, RES_FRAME, req->seq);
    ns_page_frame fr;
    ns_page_session_set_frame_time(s, req->frame_time_us);
    if (req->force_full)
        ns_page_session_invalidate_frame(s);
    if (ns_page_session_render(s, req->w, req->h, req->sx, req->sy,
                               req->scale, req->caret_active, &fr) == 0) {
        res->ok = TRUE;
        res->animating = fr.animating ? TRUE : FALSE;
        res->caret_blinking = fr.caret_blinking ? TRUE : FALSE;
        res->pw = fr.page_w;
        res->ph = fr.page_h;
        res->requested_scroll_y = fr.scroll_y;
        res->requested_scroll_x = fr.scroll_x;
        if (!fr.unchanged) {
            gint64 trace_start = ns_trace_now();
            if (fr.n_damage > 0) {
                res->patch = stage_patch(fr.pixels, fr.stride, fr.damage,
                                         fr.n_damage, &res->patch_x,
                                         &res->patch_y);
                res->damage = fr.damage;
                res->n_damage = fr.n_damage;
                fr.damage = NULL;
                fr.n_damage = 0;
            }
            if (!res->patch)
                res->surface = stage_fill(fr.pixels, fr.width, fr.height,
                                          fr.stride);
            res->gen = fr.gen;
            res->base_gen = fr.base_gen;
            ns_trace_complete("frame", res->patch ? "copy damage" : "copy frame",
                              trace_start, NULL);
        }
        res->nav = fr.nav;
        res->url = fr.url;
        res->url_pushed = fr.url_pushed ? TRUE : FALSE;
        res->title = fr.title;
        res->camera = fr.camera;
        res->download = fr.download;
        res->video_layers = fr.video_layers;
        fr.video_layers = NULL;
        fr.nav = fr.url = fr.title = fr.camera = fr.download = NULL;
        if (fr.clipboard)
            res->clipboard = ns_page_session_clipboard(s);
    }
    ns_page_frame_clear(&fr);
    post(res);
}

static void arm_anim(NsProcView *v);

/* Main-loop idle that rearms the animation tick after the engine requests a wake. */
static gboolean
pv_wake_idle(gpointer data)
{
    NsProcView *v = data;
    if (!v->closed && v->opened) {
        v->last_anim_frame_us = 0;
        arm_anim(v);
    }
    pv_unref(v);
    return G_SOURCE_REMOVE;
}

/* Engine-thread wake callback; bounces to the main loop holding a view reference. */
static void
pv_session_wake(gpointer data)
{
    g_idle_add(pv_wake_idle, pv_ref(data));
}

/* Engine-thread dispatcher: executes one Req against the page session and posts its Res. */
static void
run_req(gpointer data)
{
    Req *req = data;
    NsProcView *v = req->view;
    if (req->type == REQ_CLOSE) {
        ns_page_session_free(v->session);
        v->session = NULL;
        req_free(req);
        g_idle_add(pv_unref_idle, v);
        return;
    }
    if (!v->session) {
        v->session = ns_page_session_new(NS_PROC_MAX_WIDTH,
                                         NS_PROC_MAX_HEIGHT);
        ns_page_session_set_wake(v->session, pv_session_wake, v);
        ns_page_session_set_video_layers(v->session, TRUE);
    }
    ns_page_session *s = v->session;
    Res *res;
    switch (req->type) {
    case REQ_LOAD:
        run_load(v, s, req);
        break;
    case REQ_RENDER:
        run_render(v, s, req);
        break;
    case REQ_LINK:
        res = res_new(v, RES_LINK, req->seq);
        res->action = req->action;
        if (req->action == ACT_HOVER) {
            res->href = ns_page_session_link_at(s, req->x, req->y,
                                                &res->cursor);
        } else if (req->action == ACT_CONTEXT) {
            res->prevented = ns_page_session_contextmenu(s, req->x, req->y);
            if (!res->prevented)
                res->href = ns_page_session_link_at(s, req->x, req->y, NULL);
        } else {
            res->href = ns_page_session_link_at(s, req->x, req->y, NULL);
        }
        post(res);
        break;
    case REQ_CLICK:
        res = res_new(v, RES_CLICK, req->seq);
        res->href = ns_page_session_click(s, req->x, req->y, req->mods);
        post(res);
        break;
    case REQ_VIEWPORT: {
        res = res_new(v, RES_VIEWPORT, req->seq);
        ns_page_info pg;
        if (ns_page_session_set_viewport(s, req->vw, req->vh, &pg) == 0) {
            res->ok = pg.ok;
            res->pw = pg.page_width;
            res->ph = pg.page_height;
        }
        ns_page_info_clear(&pg);
        post(res);
        break;
    }
    case REQ_KEY:
        res = res_new(v, RES_KEY, req->seq);
        res->kind = req->kind;
        res->fallback_scroll = req->fallback_scroll;
        res->fallback_x = req->fallback_x;
        res->fallback_y = req->fallback_y;
        res->href = ns_page_session_key(s, req->kind, req->key, req->code,
                                        req->keycode, req->mods,
                                        &res->prevented);
        post(res);
        break;
    case REQ_SELECT:
        res = res_new(v, req->kind == 4 ? RES_COPY : RES_SELECT, req->seq);
        res->href = ns_page_session_select(s, req->kind, req->x, req->y);
        post(res);
        break;
    case REQ_HOVER:
        res = res_new(v, RES_HOVER, req->seq);
        res->ok = ns_page_session_hover(s, req->x, req->y, &res->href,
                                        &res->cursor) == 1;
        post(res);
        break;
    case REQ_SCROLL:
        res = res_new(v, RES_SCROLL, req->seq);
        res->ok = ns_page_session_scroll(s, req->x, req->y, req->dx,
                                         req->dy);
        res->fallback_x = req->fallback_x;
        res->fallback_y = req->fallback_y;
        post(res);
        break;
    case REQ_SCROLLBAR:
        res = res_new(v, RES_SCROLLBAR, req->seq);
        res->kind = req->kind;
        res->ok = ns_page_session_scrollbar(s, req->kind, req->x, req->y);
        post(res);
        break;
    case REQ_DROPFILES:
        res = res_new(v, RES_DROPFILES, req->seq);
        if (req->paths && *req->paths) {
            char **list = g_strsplit(req->paths, "\n", -1);
            guint count = list ? g_strv_length(list) : 0;
            if (count > 0)
                res->ok = ns_page_session_drop_files(
                    s, req->x, req->y, (const char *const *)list,
                    (int)count) == 1;
            g_strfreev(list);
        }
        post(res);
        break;
    case REQ_RELEASE:
        res = res_new(v, RES_RELEASE, req->seq);
        res->href = ns_page_session_release(s, &res->ok);
        if (!res->href || !*res->href)
            res->media_url = ns_page_session_media_at(s, req->x, req->y,
                                                      &res->media_is_video,
                                                      &res->media_stream);
        post(res);
        break;
    case REQ_FIND:
        res = res_new(v, RES_FIND, req->seq);
        ns_page_session_find(s, req->query, req->find_case, req->find_dir,
                             req->find_from_y, &res->find_total,
                             &res->find_current, &res->find_scroll_y);
        post(res);
        break;
    case REQ_EXPORT: {
        res = res_new(v, RES_EXPORT, req->seq);
        gboolean ok = FALSE;
        if (req->url && req->export_dest &&
            ns_page_session_export(s, req->url) == 0) {
            GFile *src = g_file_new_for_path(req->url);
            GFile *dst = g_file_new_for_path(req->export_dest);
            ok = g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                             NULL, NULL);
            g_object_unref(src);
            g_object_unref(dst);
        }
        if (req->url)
            g_unlink(req->url);
        res->ok = ok;
        res->url = g_strdup(req->export_dest ? req->export_dest : "");
        post(res);
        break;
    }
    case REQ_CONSOLE:
        res = res_new(v, RES_CONSOLE, req->seq);
        res->href = ns_page_session_console(s);
        post(res);
        break;
    case REQ_EVAL:
        res = res_new(v, RES_EVAL, req->seq);
        res->dump_tab = req->dump_tab;
        res->inspect = req->inspect;
        res->href = ns_page_session_eval(s, req->query);
        post(res);
        break;
    case REQ_DUMP:
        res = res_new(v, RES_DUMP, req->seq);
        res->dump_tab = req->dump_tab;
        res->href = ns_page_session_dump(s, req->query);
        post(res);
        break;
    case REQ_CAMERA:
        ns_page_session_resolve_camera(s, req->url, req->mods);
        break;
    case REQ_PRINT:
        res = res_new(v, RES_PRINT, req->seq);
        res->print_pages = ns_page_session_print(s, &res->print_setup);
        res->ok = res->print_pages != NULL;
        post(res);
        break;
    case REQ_CLOSE:
        break;
    }
    req_free(req);
    g_idle_add(pv_unref_idle, v);
}

/* Queues a request to the engine thread with a view ref; drops it once the view is closed. */
static void
push_req(NsProcView *v, Req *req)
{
    if (v->closed && req->type != REQ_CLOSE) {
        req_free(req);
        return;
    }
    req->view = pv_ref(v);
    ns_engine_thread_post(run_req, req);
}

static int
viewport_w(NsProcView *v)
{
    int w = v->area ? gtk_widget_get_width(v->area) : 0;
    return w > 0 ? w : 1;
}

static int
viewport_h(NsProcView *v)
{
    int h = v->area ? gtk_widget_get_height(v->area) : 0;
    return h > 0 ? h : 1;
}

static double
cur_scale(NsProcView *v)
{
    return v->scale > 0.0 ? v->scale : 1.0;
}

/* Syncs scrollbar adjustments and visibility with page size and zoomed viewport size. */
static void
configure_adjustments(NsProcView *v)
{
    double s = cur_scale(v);
    double cw = viewport_w(v) / s;
    double ch = viewport_h(v) / s;
    gboolean can_scroll_x = v->page_w > cw + 0.5;
    gboolean can_scroll_y = v->page_h > ch + 0.5;
    if (!can_scroll_x) v->scroll_x = 0;
    if (!can_scroll_y) v->scroll_y = 0;
    if (v->hscroll) gtk_widget_set_visible(v->hscroll, can_scroll_x);
    if (v->vscroll) gtk_widget_set_visible(v->vscroll, can_scroll_y);
    double upper_w = v->page_w > cw ? v->page_w : cw;
    double upper_h = v->page_h > ch ? v->page_h : ch;
    gtk_adjustment_configure(v->hadj, v->scroll_x, 0, upper_w, 60, cw, cw);
    gtk_adjustment_configure(v->vadj, v->scroll_y, 0, upper_h, 60, ch, ch);
    v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
    v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
}

/* Picks up the scrollbar position and requests a re-render. */
static void
on_adj_changed(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    NsProcView *v = data;
    if (v->closed)
        return;
    v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
    v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
    if (v->opened)
        request_render(v);
}

static void start_render(NsProcView *v);

/* Starts a render, or marks one pending if a render is already in flight. */
static void
request_render(NsProcView *v)
{
    if (!v->opened)
        return;
    if (v->render_inflight) {
        v->render_pending = TRUE;
        return;
    }
    start_render(v);
}

/* Posts a REQ_RENDER with current viewport, scroll, zoom, focus and frame-clock time. */
static void
start_render(NsProcView *v)
{
    if (!v->opened)
        return;
    v->render_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_RENDER;
    req->seq = ++v->render_seq;
    req->w = viewport_w(v);
    req->h = viewport_h(v);
    req->sx = v->scroll_x;
    req->sy = v->scroll_y;
    req->scale = cur_scale(v);
    req->caret_active = gtk_widget_has_focus(v->area);
    req->force_full = v->frame_force_full;
    v->frame_force_full = FALSE;
    GdkFrameClock *clock = gtk_widget_get_frame_clock(v->area);
    req->frame_time_us = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    push_req(v, req);
}

static void
on_area_focus_notify(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object;
    (void)pspec;
    NsProcView *v = data;
    v->last_anim_frame_us = 0;
    request_render(v);
}

/* Advances each video layer to the frame due at now; queues a redraw if any changed. */
static void
present_video_layers(NsProcView *v, gint64 now)
{
    gboolean changed = FALSE;
    for (guint i = 0; v->video_layers && i < v->video_layers->len; i++) {
        ns_video_layer_rect *r =
            &g_array_index(v->video_layers, ns_video_layer_rect, i);
        if (ns_video_layer_present_now(r->layer, now))
            changed = TRUE;
    }
    if (changed)
        gtk_widget_queue_draw(v->area);
}

static const cairo_user_data_key_t video_texture_key;

static void
video_texture_release(void *data)
{
    ns_texture_unref(data);
}

/* Wraps a video texture in a cached cairo surface holding a texture ref; marks it used. */
static cairo_surface_t *
video_surface_for(NsProcView *v, ns_texture *tex, GHashTable *used)
{
    int tw = ns_texture_get_width(tex);
    int th = ns_texture_get_height(tex);
    gsize stride = 0;
    const guchar *px = ns_texture_peek(tex, &stride);
    if (!px || tw <= 0 || th <= 0 ||
        stride != (gsize)cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, tw))
        return NULL;
    if (!v->video_surfaces)
        v->video_surfaces = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL,
            (GDestroyNotify)cairo_surface_destroy);
    cairo_surface_t *s = g_hash_table_lookup(v->video_surfaces, tex);
    if (!s) {
        s = cairo_image_surface_create_for_data((unsigned char *)px,
                                                CAIRO_FORMAT_ARGB32, tw, th,
                                                (int)stride);
        cairo_surface_set_user_data(s, &video_texture_key,
                                    ns_texture_ref(tex), video_texture_release);
        g_hash_table_insert(v->video_surfaces, tex, s);
    }
    g_hash_table_add(used, tex);
    return s;
}

static gboolean
video_surface_unused(gpointer key, gpointer value, gpointer used)
{
    (void)value;
    return !g_hash_table_contains(used, key);
}

/* Paints video layers over the frame, scaled into their rects; evicts unused surface cache. */
static void
draw_video_layers(NsProcView *v, cairo_t *cr)
{
    GHashTable *used = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; v->video_layers && i < v->video_layers->len; i++) {
        const ns_video_layer_rect *r =
            &g_array_index(v->video_layers, ns_video_layer_rect, i);
        if (r->w <= 0 || r->h <= 0)
            continue;
        cairo_save(cr);
        cairo_rectangle(cr, r->x, r->y, r->w, r->h);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        ns_texture *tex = ns_video_layer_texture(r->layer, NULL);
        cairo_surface_t *s = tex ? video_surface_for(v, tex, used) : NULL;
        if (s) {
            cairo_translate(cr, r->x, r->y);
            cairo_scale(cr, r->w / ns_texture_get_width(tex),
                        r->h / ns_texture_get_height(tex));
            cairo_set_source_surface(cr, s, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
        }
        ns_texture_unref(tex);
        cairo_restore(cr);
    }
    if (v->video_surfaces)
        g_hash_table_foreach_remove(v->video_surfaces, video_surface_unused,
                                    used);
    g_hash_table_destroy(used);
}

/* Frame-clock tick: presents video, then re-renders at 60Hz if animating else at caret rate. */
static gboolean
anim_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    (void)clock;
    NsProcView *v = data;
    if (v->closed || !v->opened) {
        v->anim_tick_id = 0;
        v->last_anim_frame_us = 0;
        return G_SOURCE_REMOVE;
    }
    gint64 now = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    if (now <= 0) now = g_get_monotonic_time();
    present_video_layers(v, now);
    gint64 interval = v->page_animating
                          ? NS_PROC_ANIM_FRAME_US - NS_PROC_ANIM_FRAME_SLACK_US
                          : NS_PROC_CARET_BLINK_US;
    if (v->last_anim_frame_us > 0 && now - v->last_anim_frame_us < interval)
        return G_SOURCE_CONTINUE;
    v->last_anim_frame_us = now;
    request_render(v);
    return G_SOURCE_CONTINUE;
}

/* Installs the animation tick callback on the drawing area if not already armed. */
static void
arm_anim(NsProcView *v)
{
    if (v->anim_tick_id || v->closed || !v->area)
        return;
    v->last_anim_frame_us = 0;
    v->anim_tick_id = gtk_widget_add_tick_callback(v->area, anim_tick, v, NULL);
}

static void
disarm_anim(NsProcView *v)
{
    if (v->anim_tick_id && v->area)
        gtk_widget_remove_tick_callback(v->area, v->anim_tick_id);
    v->anim_tick_id = 0;
    v->last_anim_frame_us = 0;
}

static void start_link(NsProcView *v, int x, int y, LinkAct action);
static void start_scroll(NsProcView *v, double dx, double dy);
static void show_context_menu(NsProcView *v, const char *href);
static void build_search_bar(NsProcView *v);
static void console_append(NsProcView *v, const char *text);
static void console_set_open(NsProcView *v, gboolean open);

/* Starts a link query or coalesces it as pending; a hover never overwrites a pending click. */
static void
request_link(NsProcView *v, int x, int y, LinkAct action)
{
    if (!v->opened)
        return;
    if (v->link_inflight) {
        if (action != ACT_HOVER || v->link_pending_action == ACT_HOVER) {
            v->link_pending_x = x;
            v->link_pending_y = y;
            v->link_pending_action = action;
        }
        v->link_pending = TRUE;
        return;
    }
    start_link(v, x, y, action);
}

static void
start_link(NsProcView *v, int x, int y, LinkAct action)
{
    if (!v->opened)
        return;
    v->link_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_LINK;
    req->seq = ++v->link_seq;
    req->x = x;
    req->y = y;
    req->action = action;
    push_req(v, req);
}

static void start_hover(NsProcView *v, int x, int y);

/* Starts a hover query, or records the latest coordinates as pending if one is in flight. */
static void
request_hover(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    if (v->hover_inflight) {
        v->hover_pending_x = x;
        v->hover_pending_y = y;
        v->hover_pending = TRUE;
        return;
    }
    start_hover(v, x, y);
}

static void
start_hover(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    v->hover_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_HOVER;
    req->seq = ++v->hover_seq;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

/* Maps a GDK keyval to a DOM KeyboardEvent.key string (printable char or named key). */
static const char *
keyval_js_key(guint keyval, char *buf, size_t bufsz)
{
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc >= 0x20 && uc != 0x7f) {
        int len = g_unichar_to_utf8(uc, buf);
        if (len >= (int)bufsz) len = (int)bufsz - 1;
        buf[len] = '\0';
        return buf;
    }
    switch (keyval) {
    case GDK_KEY_Up:         return "ArrowUp";
    case GDK_KEY_Down:       return "ArrowDown";
    case GDK_KEY_Left:       return "ArrowLeft";
    case GDK_KEY_Right:      return "ArrowRight";
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:   return "Enter";
    case GDK_KEY_Escape:     return "Escape";
    case GDK_KEY_BackSpace:  return "Backspace";
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return "Tab";
    case GDK_KEY_Delete:     return "Delete";
    case GDK_KEY_Insert:     return "Insert";
    case GDK_KEY_Home:       return "Home";
    case GDK_KEY_End:        return "End";
    case GDK_KEY_Page_Up:    return "PageUp";
    case GDK_KEY_Page_Down:  return "PageDown";
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:    return "Shift";
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:  return "Control";
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:      return "Alt";
    default: { const char *n = gdk_keyval_name(keyval); return n ? n : ""; }
    }
}

/* Maps a GDK keyval to a DOM KeyboardEvent.code (KeyA, Digit0, ArrowUp...); "" if none. */
static const char *
keyval_js_code(guint keyval, char *buf, size_t bufsz)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
        g_snprintf(buf, bufsz, "Key%c", 'A' + (int)(keyval - GDK_KEY_a));
        return buf;
    }
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
        g_snprintf(buf, bufsz, "Key%c", 'A' + (int)(keyval - GDK_KEY_A));
        return buf;
    }
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
        g_snprintf(buf, bufsz, "Digit%c", '0' + (int)(keyval - GDK_KEY_0));
        return buf;
    }
    switch (keyval) {
    case GDK_KEY_Up:         return "ArrowUp";
    case GDK_KEY_Down:       return "ArrowDown";
    case GDK_KEY_Left:       return "ArrowLeft";
    case GDK_KEY_Right:      return "ArrowRight";
    case GDK_KEY_Return:     return "Enter";
    case GDK_KEY_KP_Enter:   return "NumpadEnter";
    case GDK_KEY_Escape:     return "Escape";
    case GDK_KEY_BackSpace:  return "Backspace";
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return "Tab";
    case GDK_KEY_Delete:     return "Delete";
    case GDK_KEY_Home:       return "Home";
    case GDK_KEY_End:        return "End";
    case GDK_KEY_Page_Up:    return "PageUp";
    case GDK_KEY_Page_Down:  return "PageDown";
    case GDK_KEY_space:      return "Space";
    default:                 return "";
    }
}

/* Maps a GDK keyval to the legacy DOM keyCode; 0 when unmapped. */
static int
keyval_js_keycode(guint keyval)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z)
        return 65 + (int)(keyval - GDK_KEY_a);
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z)
        return 65 + (int)(keyval - GDK_KEY_A);
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
        return 48 + (int)(keyval - GDK_KEY_0);
    switch (keyval) {
    case GDK_KEY_BackSpace:  return 8;
    case GDK_KEY_Tab:        return 9;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:   return 13;
    case GDK_KEY_Escape:     return 27;
    case GDK_KEY_space:      return 32;
    case GDK_KEY_Page_Up:    return 33;
    case GDK_KEY_Page_Down:  return 34;
    case GDK_KEY_End:        return 35;
    case GDK_KEY_Home:       return 36;
    case GDK_KEY_Left:       return 37;
    case GDK_KEY_Up:         return 38;
    case GDK_KEY_Right:      return 39;
    case GDK_KEY_Down:       return 40;
    case GDK_KEY_Delete:     return 46;
    default:                 return 0;
    }
}

/* Posts a REQ_KEY with DOM key/code/keyCode and modifiers, plus optional fallback scroll. */
static void
start_key_full(NsProcView *v, int kind, guint keyval, GdkModifierType state,
               int fallback_scroll, double fallback_x, double fallback_y)
{
    if (!v->opened)
        return;
    char keybuf[8] = {0}, codebuf[16] = {0};
    Req *req = g_new0(Req, 1);
    req->type = REQ_KEY;
    req->seq = kind == 0 ? ++v->key_seq : v->key_seq;
    req->kind = kind;
    req->keycode = keyval_js_keycode(keyval);
    req->fallback_scroll = fallback_scroll;
    req->fallback_x = fallback_x;
    req->fallback_y = fallback_y;
    req->mods = ((state & GDK_SHIFT_MASK)   ? 1 : 0) |
                ((state & GDK_CONTROL_MASK) ? 2 : 0) |
                ((state & GDK_ALT_MASK)     ? 4 : 0) |
                ((state & GDK_META_MASK)    ? 8 : 0);
    req->key = g_strdup(keyval_js_key(keyval, keybuf, sizeof keybuf));
    req->code = g_strdup(keyval_js_code(keyval, codebuf, sizeof codebuf));
    push_req(v, req);
}

static void
start_key(NsProcView *v, int kind, guint keyval, GdkModifierType state)
{
    start_key_full(v, kind, keyval, state, 0, 0, 0);
}

/* Posts committed IME/text input as a REQ_KEY carrying the text in key. */
static void
start_key_text(NsProcView *v, int kind, const char *text)
{
    if (!v->opened || !text || !*text)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_KEY;
    req->seq = ++v->key_seq;
    req->kind = kind;
    req->keycode = 0;
    req->mods = 0;
    req->key = g_strdup(text);
    req->code = g_strdup("");
    push_req(v, req);
}

static void
on_im_commit(GtkIMContext *im, const char *text, gpointer data)
{
    (void)im;
    NsProcView *v = data;
    if (!v || !v->opened || !text || !*text) return;
    start_key_text(v, 2, text);
}

static void
start_release(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_RELEASE;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

static void
start_select(NsProcView *v, int kind, int x, int y)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_SELECT;
    req->seq = ++v->select_seq;
    req->kind = kind;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

static void
start_click(NsProcView *v, int x, int y, int mods)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CLICK;
    req->seq = ++v->click_seq;
    req->x = x;
    req->y = y;
    req->mods = mods;
    push_req(v, req);
}

/* Posts dropped file paths (newline-joined, ownership taken) to the engine at (x, y). */
static void
start_dropfiles(NsProcView *v, int x, int y, char *paths)
{
    if (!v->opened) {
        g_free(paths);
        return;
    }
    Req *req = g_new0(Req, 1);
    req->type = REQ_DROPFILES;
    req->x = x;
    req->y = y;
    req->paths = paths;
    push_req(v, req);
}

/* Sends a viewport resize to the engine, coalescing while one is in flight. */
static void
start_viewport(NsProcView *v, int width, int height)
{
    if (!v->opened)
        return;
    if (v->viewport_inflight) {
        v->viewport_pending = TRUE;
        return;
    }
    v->viewport_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_VIEWPORT;
    req->seq = ++v->viewport_seq;
    req->vw = width;
    req->vh = height;
    push_req(v, req);
}

/* Issues a viewport update only when the drawing area size changed; TRUE if sent. */
static gboolean
maybe_update_viewport(NsProcView *v)
{
    if (!v->opened)
        return FALSE;
    int w = viewport_w(v);
    int h = viewport_h(v);
    if (w <= 1 || h <= 1)
        return FALSE;
    if (w == v->last_vp_w && h == v->last_vp_h)
        return FALSE;
    v->last_vp_w = w;
    v->last_vp_h = h;
    start_viewport(v, w, h);
    return TRUE;
}

/* Appends a URL to back/forward history, truncating forward entries; skips duplicates. */
static void
push_history(NsProcView *v, const char *url)
{
    if (!url || !*url)
        return;
    if (v->hist_index >= 0 &&
        g_strcmp0(g_ptr_array_index(v->history, v->hist_index), url) == 0)
        return;
    while ((int)v->history->len > v->hist_index + 1)
        g_ptr_array_remove_index(v->history, v->history->len - 1);
    g_ptr_array_add(v->history, g_strdup(url));
    v->hist_index = (int)v->history->len - 1;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
}

/* Replaces the current history entry's URL in place (replaceState-style). */
static void
replace_history(NsProcView *v, const char *url)
{
    if (!url || !*url || v->hist_index < 0 ||
        v->hist_index >= (int)v->history->len)
        return;
    g_free(g_ptr_array_index(v->history, v->hist_index));
    g_ptr_array_index(v->history, v->hist_index) = g_strdup(url);
}

static void pv_perm_resolve(NsProcView *v, gboolean allow);

/* Starts a navigation: invalidates all in-flight requests, resets state, posts REQ_LOAD. */
static void
do_load(NsProcView *v, const char *url, gboolean record, gboolean history,
        gboolean user_activated)
{
    if (!url || !*url)
        return;
    pv_perm_resolve(v, FALSE);
    v->pending_record = record;
    int seq = ++v->load_seq;
    ++v->render_seq;
    ++v->link_seq;
    ++v->click_seq;
    ++v->viewport_seq;
    ++v->key_seq;
    ++v->select_seq;
    ++v->hover_seq;
    v->render_pending = FALSE;
    v->render_inflight = FALSE;
    v->viewport_inflight = FALSE;
    v->viewport_pending = FALSE;
    ++v->scroll_seq;
    v->scroll_inflight = FALSE;
    v->scroll_pending = FALSE;
    v->scroll_pending_dx = v->scroll_pending_dy = 0.0;
    v->link_inflight = FALSE;
    v->link_pending = FALSE;
    v->link_pending_action = ACT_HOVER;
    v->hover_inflight = FALSE;
    v->hover_pending = FALSE;
    v->has_selection = FALSE;
    ++v->find_seq;
    if (v->search_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), FALSE);
    if (v->search_label)
        gtk_label_set_text(GTK_LABEL(v->search_label), "");
    v->opened = FALSE;
    v->page_animating = FALSE;
    v->caret_blinking = FALSE;
    disarm_anim(v);
    if (v->frame)
        cairo_surface_destroy(v->frame);
    v->frame = NULL;
    pv_set_video_layers(v, NULL);
    gtk_widget_queue_draw(v->area);
    if (!v->loading) {
        v->loading = TRUE;
        post_emit(v, NS_PROC_EVT_LOADING, "1");
    }
    set_busy_cursor(v);
    post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Loading…"));

    int vw = gtk_widget_get_width(v->area);
    int vh = gtk_widget_get_height(v->area);
    if (vw <= 1 || vh <= 1) {
        g_free(v->deferred_url);
        v->deferred_url = g_strdup(url);
        v->deferred_record = record;
        v->deferred_history = history;
        v->deferred_user_activated = user_activated;
        return;
    }
    v->last_vp_w = vw;
    v->last_vp_h = vh;

    Req *req = g_new0(Req, 1);
    req->type = REQ_LOAD;
    req->seq = seq;
    req->url = g_strdup(url);
    req->vw = vw;
    req->vh = vh;
    req->history = history;
    req->user_activated = user_activated;
    push_req(v, req);
}

void
ns_proc_view_load(NsProcView *v, const char *url)
{
    do_load(v, url, TRUE, FALSE, TRUE);
}

gboolean ns_proc_view_can_back(NsProcView *v) { return v->hist_index > 0; }

gboolean
ns_proc_view_can_forward(NsProcView *v)
{
    return v->hist_index >= 0 && v->hist_index < (int)v->history->len - 1;
}

void
ns_proc_view_back(NsProcView *v)
{
    if (!ns_proc_view_can_back(v))
        return;
    v->hist_index--;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
    do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, TRUE, TRUE);
}

void
ns_proc_view_forward(NsProcView *v)
{
    if (!ns_proc_view_can_forward(v))
        return;
    v->hist_index++;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
    do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, TRUE, TRUE);
}

/* Reloads the current history entry, or the current URL when history is empty. */
void
ns_proc_view_reload(NsProcView *v)
{
    if (v->hist_index >= 0 && v->hist_index < (int)v->history->len)
        do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, FALSE,
                TRUE);
    else if (v->current_url)
        do_load(v, v->current_url, FALSE, FALSE, TRUE);
}

void
ns_proc_view_toggle_console(NsProcView *v)
{
    if (v->opened)
        console_set_open(v, !v->console_open);
}

const char *ns_proc_view_url(NsProcView *v) { return v->current_url; }
const char *ns_proc_view_title(NsProcView *v) { return v->current_title; }
int ns_proc_view_security(NsProcView *v) { return v ? v->security : 0; }
const char *ns_proc_view_remote_ip(NsProcView *v) { return v ? v->remote_ip : NULL; }
gboolean ns_proc_view_is_loading(NsProcView *v) { return v->loading; }
void ns_proc_view_focus(NsProcView *v)
{
    if (v->area)
        gtk_widget_grab_focus(v->area);
}

/* Clamps zoom to the permille range, emits the percent and re-renders if changed. */
static void
set_zoom(NsProcView *v, double scale)
{
    int permille = (int)(scale * 1000.0 + 0.5);
    if (permille < NS_PV_ZOOM_MIN_PERMILLE)
        permille = NS_PV_ZOOM_MIN_PERMILLE;
    if (permille > NS_PV_ZOOM_MAX_PERMILLE)
        permille = NS_PV_ZOOM_MAX_PERMILLE;
    double clamped = permille / 1000.0;
    if (clamped == cur_scale(v))
        return;
    v->scale = clamped;
    char percent[16];
    g_snprintf(percent, sizeof percent, "%d", permille / 10);
    post_emit(v, NS_PROC_EVT_ZOOM, percent);
    if (v->opened) {
        configure_adjustments(v);
        request_render(v);
    }
}

static const int k_zoom_ladder_percent[] = {
    25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300,
    400, 500
};

/* Moves zoom to the next step up or down the fixed browser zoom ladder. */
static void
zoom_step(NsProcView *v, int direction)
{
    int now = (int)(cur_scale(v) * 100.0 + 0.5);
    gsize n = G_N_ELEMENTS(k_zoom_ladder_percent);
    if (direction > 0) {
        for (gsize i = 0; i < n; i++)
            if (k_zoom_ladder_percent[i] > now) {
                set_zoom(v, k_zoom_ladder_percent[i] / 100.0);
                return;
            }
        set_zoom(v, k_zoom_ladder_percent[n - 1] / 100.0);
        return;
    }
    for (gsize i = n; i-- > 0; )
        if (k_zoom_ladder_percent[i] < now) {
            set_zoom(v, k_zoom_ladder_percent[i] / 100.0);
            return;
        }
    set_zoom(v, k_zoom_ladder_percent[0] / 100.0);
}

void ns_proc_view_zoom_in(NsProcView *v)  { zoom_step(v, 1); }
void ns_proc_view_zoom_out(NsProcView *v) { zoom_step(v, -1); }
void ns_proc_view_zoom_reset(NsProcView *v) { set_zoom(v, 1.0); }

int
ns_proc_view_zoom_percent(NsProcView *v)
{
    return v ? (int)(cur_scale(v) * 100.0 + 0.5) : 100;
}

/* Answers the pending permission prompt to the engine and hides the permission bar. */
static void
pv_perm_resolve(NsProcView *v, gboolean allow)
{
    if (!v->perm_pending)
        return;
    v->perm_pending = FALSE;
    ReqType kind = v->perm_kind;
    char *origin = v->perm_origin;
    v->perm_origin = NULL;
    if (v->perm_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(v->perm_revealer), FALSE);
    if (!v->closed) {
        Req *req = g_new0(Req, 1);
        req->type = kind;
        req->url = g_strdup(origin);
        req->mods = allow ? 1 : 0;
        push_req(v, req);
    }
    g_free(origin);
}

static void
on_perm_allow(GtkButton *btn, gpointer data)
{
    (void)btn;
    pv_perm_resolve(data, TRUE);
}

static void
on_perm_deny(GtkButton *btn, gpointer data)
{
    (void)btn;
    pv_perm_resolve(data, FALSE);
}

/* Shows the camera/microphone permission bar for an origin, denying any other pending one. */
static void
pv_perm_bar_show(NsProcView *v, ReqType kind, const char *origin)
{
    if (v->perm_pending && v->perm_kind == kind &&
        g_strcmp0(v->perm_origin, origin) == 0)
        return;
    if (v->perm_pending)
        pv_perm_resolve(v, FALSE);
    v->perm_pending = TRUE;
    v->perm_kind = kind;
    v->perm_origin = g_strdup(origin);
    const char *what =
        ns_i18n("This site wants to use your camera and microphone");
    char *text = g_strdup_printf("%s — %s", what, origin);
    gtk_label_set_text(GTK_LABEL(v->perm_label), text);
    g_free(text);
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->perm_revealer), TRUE);
}

/* Main-loop handler for engine results; drops stale seqs, updates UI, frees the Res. */
static gboolean
on_result(gpointer data)
{
    Res *res = data;
    NsProcView *v = res->view;

    if (v->closed)
        goto done;

    if (res->type == RES_PAGE) {
        if (res->seq != v->load_seq)
            goto done;
        if (!res->ok) {
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Failed to load page"));
            finish_loading(v);
            clear_busy_cursor(v);
            goto done;
        }
        if (res->nav && *res->nav) {
            if (v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
                v->js_redirects++;
                do_load(v, res->nav, v->pending_record, FALSE, FALSE);
                goto done;
            }
            post_emit(v, NS_PROC_EVT_STATUS,
                      ns_i18n("Stopped after too many redirects"));
        }
        v->js_redirects = 0;
        g_free(v->current_url);
        v->current_url = g_strdup(res->url);
        g_free(v->current_title);
        v->current_title = g_strdup(res->title);
        v->security = res->security;
        g_free(v->remote_ip);
        v->remote_ip = res->remote_ip ? g_strdup(res->remote_ip) : NULL;
        v->page_w = res->pw;
        v->page_h = res->ph;
        v->scroll_x = 0;
        v->scroll_y = 0;
        v->opened = TRUE;
        configure_adjustments(v);
        if (v->pending_record)
            push_history(v, v->current_url);
        post_emit(v, NS_PROC_EVT_URL, v->current_url);
        post_emit(v, NS_PROC_EVT_TITLE, v->current_title);
        post_emit(v, NS_PROC_EVT_STATUS, "");
        finish_loading(v);
        maybe_update_viewport(v);
        request_render(v);
    } else if (res->type == RES_FRAME) {
        gboolean current = res->seq == v->render_seq;
        if (current && res->ok && v->opened && !v->loading) {
            if (res->url && *res->url &&
                g_strcmp0(res->url, v->current_url) != 0) {
                g_free(v->current_url);
                v->current_url = g_strdup(res->url);
                if (res->url_pushed)
                    push_history(v, v->current_url);
                else
                    replace_history(v, v->current_url);
                post_emit(v, NS_PROC_EVT_URL, v->current_url);
            }
            if (res->title && g_strcmp0(res->title, v->current_title) != 0) {
                g_free(v->current_title);
                v->current_title = g_strdup(res->title);
                post_emit(v, NS_PROC_EVT_TITLE, v->current_title);
            }
        }
        if (current && res->ok) {
            pv_set_video_layers(v, res->video_layers);
            res->video_layers = NULL;
            v->page_animating = res->animating;
            v->caret_blinking = res->caret_blinking;
            if (v->page_animating || v->caret_blinking)
                arm_anim(v);
            else
                disarm_anim(v);
            if ((res->ph > 0 && res->ph != v->page_h) ||
                (res->pw > 0 && res->pw != v->page_w)) {
                if (res->ph > 0) v->page_h = res->ph;
                if (res->pw > 0) v->page_w = res->pw;
                configure_adjustments(v);
                gtk_widget_queue_draw(v->area);
            }
            if (res->requested_scroll_y >= 0 || res->requested_scroll_x >= 0) {
                configure_adjustments(v);
                if (res->requested_scroll_y >= 0)
                    gtk_adjustment_set_value(v->vadj,
                                             res->requested_scroll_y);
                if (res->requested_scroll_x >= 0)
                    gtk_adjustment_set_value(v->hadj,
                                             res->requested_scroll_x);
            }
        }
        if (current && res->ok && res->surface) {
            if (v->frame)
                cairo_surface_destroy(v->frame);
            v->frame = res->surface;
            v->frame_gen = res->gen;
            res->surface = NULL;
            gtk_widget_queue_draw(v->area);
            clear_busy_cursor(v);
        } else if (res->ok && res->patch) {
            if (apply_patch(v, res)) {
                gtk_widget_queue_draw(v->area);
            } else {
                v->frame_force_full = TRUE;
                request_render(v);
            }
        }
        if (current && res->ok && res->nav && *res->nav &&
            v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
            v->js_redirects++;
            do_load(v, res->nav, FALSE, FALSE, FALSE);
        }
        if (current && res->ok && res->camera && *res->camera)
            pv_perm_bar_show(v, REQ_CAMERA, res->camera);
        if (current && res->ok && res->download && *res->download)
            post_emit(v, NS_PROC_EVT_DOWNLOAD, res->download);
        if (current && res->ok && res->clipboard && v->area) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area),
                                   res->clipboard);
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Copied to clipboard"));
        }
        v->render_inflight = FALSE;
        if (v->render_pending) {
            v->render_pending = FALSE;
            start_render(v);
        } else if (current && !res->ok) {
            finish_loading(v);
            clear_busy_cursor(v);
        }
    } else if (res->type == RES_VIEWPORT) {
        if (res->seq != v->viewport_seq)
            goto done;
        v->viewport_inflight = FALSE;
        if (res->ok) {
            v->page_w = res->pw;
            v->page_h = res->ph;
            configure_adjustments(v);
            request_render(v);
        }
        if (v->viewport_pending) {
            v->viewport_pending = FALSE;
            start_viewport(v, v->last_vp_w, v->last_vp_h);
        }
    } else if (res->type == RES_SELECT) {
        if (res->seq == v->select_seq)
            request_render(v);
    } else if (res->type == RES_COPY) {
        if (res->href && *res->href && v->area) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area),
                                   res->href);
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Copied selection"));
        }
    } else if (res->type == RES_KEY) {
        if (res->seq != v->key_seq)
            goto done;
        if (res->kind == 0 && res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else {
            if (res->fallback_scroll && !res->prevented) {
                gtk_adjustment_set_value(v->hadj, res->fallback_x);
                gtk_adjustment_set_value(v->vadj, res->fallback_y);
            }
            request_render(v);
        }
    } else if (res->type == RES_CLICK) {
        if (res->seq != v->click_seq)
            goto done;
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else {
            request_render(v);
        }
    } else if (res->type == RES_LINK) {
        if (res->seq != v->link_seq)
            goto done;
        v->link_inflight = FALSE;
        gboolean navigated = FALSE;
        GtkWidget *area = v->area;
        if (res->action == ACT_CONTEXT) {
            if (res->prevented)
                request_render(v);
            else
                show_context_menu(v, res->href);
            if (v->link_pending) {
                v->link_pending = FALSE;
                LinkAct a = v->link_pending_action;
                v->link_pending_action = ACT_HOVER;
                start_link(v, v->link_pending_x, v->link_pending_y, a);
            }
            goto done;
        }
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            if (!v->busy_cursor)
                pv_set_named_cursor(area, res->cursor ? res->cursor : "pointer");
            if (res->action == ACT_NAVIGATE) {
                navigated = TRUE;
                ns_proc_view_load(v, res->href);
            }
        } else {
            post_emit(v, NS_PROC_EVT_STATUS, "");
            if (!v->busy_cursor)
                pv_set_named_cursor(area, res->cursor);
        }
        if (!navigated && v->link_pending) {
            v->link_pending = FALSE;
            LinkAct a = v->link_pending_action;
            v->link_pending_action = ACT_HOVER;
            start_link(v, v->link_pending_x, v->link_pending_y, a);
        }
    } else if (res->type == RES_HOVER) {
        if (res->seq != v->hover_seq)
            goto done;
        v->hover_inflight = FALSE;
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            if (!v->busy_cursor)
                pv_set_named_cursor(v->area, res->cursor ? res->cursor : "pointer");
        } else {
            post_emit(v, NS_PROC_EVT_STATUS, "");
            if (!v->busy_cursor)
                pv_set_named_cursor(v->area, res->cursor);
        }
        if (res->ok)
            request_render(v);
        if (v->hover_pending) {
            v->hover_pending = FALSE;
            start_hover(v, v->hover_pending_x, v->hover_pending_y);
        }
    } else if (res->type == RES_DROPFILES) {
        if (res->ok)
            request_render(v);
    } else if (res->type == RES_SCROLL) {
        if (res->seq != v->scroll_seq)
            goto done;
        v->scroll_inflight = FALSE;
        if (res->ok)
            request_render(v);
        else {
            gtk_adjustment_set_value(
                v->hadj,
                gtk_adjustment_get_value(v->hadj) + res->fallback_x * 60.0);
            gtk_adjustment_set_value(
                v->vadj,
                gtk_adjustment_get_value(v->vadj) + res->fallback_y * 60.0);
        }
        if (v->scroll_pending && v->opened) {
            double dx = v->scroll_pending_dx, dy = v->scroll_pending_dy;
            v->scroll_pending = FALSE;
            v->scroll_pending_dx = v->scroll_pending_dy = 0.0;
            start_scroll(v, dx, dy);
        }
    } else if (res->type == RES_SCROLLBAR) {
        if (res->kind == 0) {
            gboolean still = v->sb_probe;
            v->sb_probe = FALSE;
            if (res->ok) {
                request_render(v);
                if (still) {
                    v->sb_dragging = TRUE;
                    if (v->sb_have_last) {
                        double s = cur_scale(v);
                        Req *req = g_new0(Req, 1);
                        req->type = REQ_SCROLLBAR;
                        req->kind = 1;
                        req->x = v->scroll_x + (int)(v->sb_last_x / s);
                        req->y = v->scroll_y + (int)(v->sb_last_y / s);
                        push_req(v, req);
                    }
                }
            } else if (still && v->sb_have_last) {
                double s = cur_scale(v);
                if (!v->multi_click) {
                    start_select(v, 0,
                                 v->scroll_x + (int)(v->drag_start_x / s),
                                 v->scroll_y + (int)(v->drag_start_y / s));
                    v->drag_anchored = TRUE;
                }
                start_select(v, 1, v->scroll_x + (int)(v->sb_last_x / s),
                             v->scroll_y + (int)(v->sb_last_y / s));
                v->has_selection = TRUE;
            }
        } else if (res->kind == 1) {
            if (res->ok)
                request_render(v);
        }
    } else if (res->type == RES_RELEASE) {
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else if (res->ok) {
            request_render(v);
        }
    } else if (res->type == RES_FIND) {
        if (res->seq != v->find_seq)
            goto done;
        if (v->search_label) {
            const char *q = v->search_entry
                ? gtk_editable_get_text(GTK_EDITABLE(v->search_entry)) : NULL;
            if (res->find_total > 0) {
                char buf[64];
                g_snprintf(buf, sizeof buf, "%d/%d", res->find_current,
                           res->find_total);
                gtk_label_set_text(GTK_LABEL(v->search_label), buf);
            } else {
                gtk_label_set_text(GTK_LABEL(v->search_label),
                                   (q && *q) ? ns_i18n("No results") : "");
            }
        }
        if (res->find_total > 0) {
            double target = res->find_scroll_y > 40 ? res->find_scroll_y - 40
                                                    : 0;
            gtk_adjustment_set_value(v->vadj, target);
        }
        request_render(v);
    } else if (res->type == RES_EXPORT) {
        if (res->ok && res->url)
            post_emit(v, NS_PROC_EVT_STATUS, res->url);
        else
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Could not save page"));
    } else if (res->type == RES_PRINT) {
        if (res->print_pages && res->print_pages->len > 0) {
            print_run(v, res->print_pages, &res->print_setup);
            res->print_pages = NULL;
        } else {
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Nothing to print"));
        }
        request_render(v);
    } else if (res->type == RES_CONSOLE) {
        if (res->href && *res->href)
            console_append(v, res->href);
    } else if (res->type == RES_EVAL) {
        if (res->inspect) {
            if (v->elements_buffer)
                gtk_text_buffer_set_text(
                    v->elements_buffer,
                    (res->href && *res->href) ? res->href
                                              : ns_i18n("No matching element"),
                    -1);
        } else if (res->href && *res->href) {
            console_append(v, res->href);
            console_append(v, "\n");
        } else {
            console_append(v, "undefined\n");
        }
        request_render(v);
    } else if (res->type == RES_DUMP) {
        GtkTextBuffer *buf = NULL;
        switch (res->dump_tab) {
        case DEV_TAB_NETWORK:     buf = v->net_buffer;      break;
        case DEV_TAB_PERFORMANCE: buf = v->perf_buffer;     break;
        case DEV_TAB_LAYOUT:      buf = v->layout_buffer;   break;
        case DEV_TAB_ELEMENTS:    buf = v->elements_buffer; break;
        default: break;
        }
        if (buf) {
            gtk_text_buffer_set_text(
                buf, (res->href && *res->href) ? res->href : ns_i18n("(empty)"),
                -1);
        }
    }

done:
    if (res->surface && !res->surface_borrowed)
        cairo_surface_destroy(res->surface);
    if (res->patch)
        cairo_surface_destroy(res->patch);
    if (res->video_layers)
        g_array_free(res->video_layers, TRUE);
    free(res->damage);
    g_free(res->title);
    g_free(res->url);
    g_free(res->nav);
    g_free(res->remote_ip);
    g_free(res->camera);
    g_free(res->download);
    free(res->clipboard);
    free(res->href);
    free(res->cursor);
    free(res->media_url);
    pv_unref(res->view);
    g_free(res);
    return G_SOURCE_REMOVE;
}

/* Presents the cached frame with video layers beneath; white fill when it doesn't cover. */
static void
on_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
        gpointer data)
{
    (void)area;
    NsProcView *v = data;
    gint64 trace_start = ns_trace_now();
    gboolean covers = v->frame &&
        cairo_image_surface_get_width(v->frame) >= width &&
        cairo_image_surface_get_height(v->frame) >= height;
    if (!covers) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_rectangle(cr, 0, 0, width, height);
        cairo_fill(cr);
    }
    if (v->frame) {
        draw_video_layers(v, cr);
        cairo_set_source_surface(cr, v->frame, 0, 0);
        cairo_paint(cr);
    }
    ns_trace_complete("frame", "present", trace_start, NULL);
}

/* Runs a load deferred for lack of size, else updates viewport and re-renders. */
static void
on_resize(GtkDrawingArea *area, int width, int height, gpointer data)
{
    (void)area;
    NsProcView *v = data;
    if (v->deferred_url && width > 1 && height > 1) {
        char *u = v->deferred_url;
        gboolean rec = v->deferred_record;
        gboolean hist = v->deferred_history;
        gboolean activated = v->deferred_user_activated;
        v->deferred_url = NULL;
        do_load(v, u, rec, hist, activated);
        g_free(u);
        return;
    }
    if (v->opened) {
        if (maybe_update_viewport(v))
            return;
        configure_adjustments(v);
        request_render(v);
    }
}

/* Sends a wheel scroll at the pointer to the engine, accumulating deltas while in flight. */
static void
start_scroll(NsProcView *v, double dx, double dy)
{
    if (v->scroll_inflight) {
        v->scroll_pending = TRUE;
        v->scroll_pending_dx += dx;
        v->scroll_pending_dy += dy;
        return;
    }
    v->scroll_inflight = TRUE;
    double s = cur_scale(v);
    Req *req = g_new0(Req, 1);
    req->type = REQ_SCROLL;
    req->seq = v->scroll_seq;
    req->x = v->scroll_x + (int)(v->pointer_x / s);
    req->y = v->scroll_y + (int)(v->pointer_y / s);
    req->dx = (int)(dx * 60.0 / s);
    req->dy = (int)(dy * 60.0 / s);
    req->fallback_x = dx;
    req->fallback_y = dy;
    push_req(v, req);
}

/* Ctrl+wheel zooms; otherwise forwards the scroll to the engine. */
static gboolean
on_scroll(GtkEventControllerScroll *ctrl, double dx, double dy, gpointer data)
{
    NsProcView *v = data;
    if (!v->opened)
        return FALSE;
    GdkModifierType mods =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(ctrl));
    GdkEvent *ev =
        gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctrl));
    if (ev)
        mods |= gdk_event_get_modifier_state(ev);
    if (mods & GDK_CONTROL_MASK) {
        double delta = dy != 0.0 ? dy : dx;
        if (delta < 0)
            ns_proc_view_zoom_in(v);
        else if (delta > 0)
            ns_proc_view_zoom_out(v);
        return TRUE;
    }
    if (v->opened) {
        start_scroll(v, dx, dy);
        return TRUE;
    }
    gtk_adjustment_set_value(v->hadj,
                             gtk_adjustment_get_value(v->hadj) + dx * 60.0);
    gtk_adjustment_set_value(v->vadj,
                             gtk_adjustment_get_value(v->vadj) + dy * 60.0);
    return TRUE;
}

typedef struct {
    GPtrArray      *pages;
    ns_print_setup  setup;
    char           *title;
} PrintJob;

static void
print_job_free(gpointer data)
{
    PrintJob *job = data;
    for (guint i = 0; i < job->pages->len; i++)
        cairo_surface_destroy(g_ptr_array_index(job->pages, i));
    g_ptr_array_free(job->pages, TRUE);
    g_free(job->title);
    g_free(job);
}

static void
on_print_begin(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer data)
{
    (void)ctx;
    PrintJob *job = data;
    gtk_print_operation_set_n_pages(op, (int)job->pages->len);
}

/* Paints one rendered print page, scaled from CSS px to the print context width. */
static void
on_print_draw(GtkPrintOperation *op, GtkPrintContext *ctx, int page_nr,
              gpointer data)
{
    (void)op;
    PrintJob *job = data;
    if (page_nr < 0 || (guint)page_nr >= job->pages->len)
        return;
    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    double surface_w = gtk_print_context_get_width(ctx);
    double scale = job->setup.width > 0 ? surface_w / job->setup.width : 1.0;
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, g_ptr_array_index(job->pages, page_nr), 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

/* Runs a GTK print dialog over pre-rendered page surfaces; takes ownership of pages. */
static void
print_run(NsProcView *v, GPtrArray *pages, const ns_print_setup *setup)
{
    PrintJob *job = g_new0(PrintJob, 1);
    job->pages = pages;
    job->setup = *setup;
    job->title = g_strdup((v->current_title && *v->current_title)
                          ? v->current_title : "page");

    GtkPageSetup *page_setup = gtk_page_setup_new();
    GtkPaperSize *paper = gtk_paper_size_new_custom(
        "northstar", "Northstar Web Browser", setup->width * 72.0 / 96.0,
        setup->height * 72.0 / 96.0, GTK_UNIT_POINTS);
    gtk_page_setup_set_paper_size(page_setup, paper);
    gtk_page_setup_set_orientation(page_setup,
        setup->width > setup->height ? GTK_PAGE_ORIENTATION_LANDSCAPE
                                     : GTK_PAGE_ORIENTATION_PORTRAIT);
    gtk_paper_size_free(paper);

    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_default_page_setup(op, page_setup);
    gtk_print_operation_set_use_full_page(op, TRUE);
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_job_name(op, job->title);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    g_signal_connect(op, "begin-print", G_CALLBACK(on_print_begin), job);
    g_signal_connect(op, "draw-page", G_CALLBACK(on_print_draw), job);
    g_object_set_data_full(G_OBJECT(op), "ns-print-job", job, print_job_free);

    GtkRoot *root = v->area ? gtk_widget_get_root(v->area) : NULL;
    GError *err = NULL;
    gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                            GTK_WINDOW(root), &err);
    if (err) {
        post_emit(v, NS_PROC_EVT_STATUS, err->message);
        g_error_free(err);
    }
    g_object_unref(page_setup);
    g_object_unref(op);
}

typedef struct { NsProcView *view; gboolean pdf; } ExportCtx;

/* Save dialog callback: posts REQ_EXPORT to a temp runtime file to be copied to the choice. */
static void
on_save_dialog_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ExportCtx *c = ud;
    NsProcView *v = c->view;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    if (file) {
        char *dest = g_file_get_path(file);
        if (dest && v->opened) {
            static int export_counter = 0;
            char *base = g_strdup_printf(
                "northstar-export-%" G_GINT64_FORMAT "-%d.%s",
                g_get_monotonic_time(), ++export_counter,
                c->pdf ? "pdf" : "png");
            Req *req = g_new0(Req, 1);
            req->type = REQ_EXPORT;
            req->url = g_build_filename(g_get_user_runtime_dir(), base, NULL);
            req->export_dest = g_strdup(dest);
            push_req(v, req);
            g_free(base);
        }
        g_free(dest);
        g_object_unref(file);
    }
    g_clear_error(&err);
    pv_unref(v);
    g_free(c);
}

/* Opens a save dialog for exporting the page as PDF or PNG, seeded with the page title. */
static void
view_save(NsProcView *v, gboolean pdf)
{
    if (!v->opened || !v->area)
        return;
    GtkRoot *root = gtk_widget_get_root(v->area);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog,
        pdf ? ns_i18n("Save page as PDF")
                                      : ns_i18n("Save page as PNG"));
    const char *t = (v->current_title && *v->current_title)
        ? v->current_title : "page";
    char *name = g_strdup_printf("%s.%s", t, pdf ? "pdf" : "png");
    g_strdelimit(name, "/", '_');
    gtk_file_dialog_set_initial_name(dialog, name);
    ExportCtx *c = g_new0(ExportCtx, 1);
    c->view = pv_ref(v);
    c->pdf = pdf;
    gtk_file_dialog_save(dialog, GTK_WINDOW(root), NULL,
                         on_save_dialog_done, c);
    g_object_unref(dialog);
    g_free(name);
}

static void
ctx_set_clipboard(NsProcView *v, const char *text, const char *status)
{
    if (!text || !*text || !v->area)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area), text);
    post_emit(v, NS_PROC_EVT_STATUS, status);
}

static void
on_ctx_back(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_back(ud); }

static void
on_ctx_forward(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_forward(ud); }

static void
on_ctx_reload(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_reload(ud); }

static void
on_ctx_copy_url(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    ctx_set_clipboard(v, v->current_url, ns_i18n("Copied page address"));
}

static void
on_ctx_open_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    if (v->ctx_link && *v->ctx_link)
        ns_proc_view_load(v, v->ctx_link);
}

static void
on_ctx_copy_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    ctx_set_clipboard(v, v->ctx_link, ns_i18n("Copied link address"));
}

static void
on_ctx_copy_sel(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; start_select(ud, 4, 0, 0); }

static void
on_ctx_select_all(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    v->has_selection = TRUE;
    start_select(v, 3, 0, 0);
}

void
ns_proc_view_print(NsProcView *view)
{
    if (!view || !view->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_PRINT;
    push_req(view, req);
}

void
ns_proc_view_save_pdf(NsProcView *view)
{ if (view) view_save(view, TRUE); }

void
ns_proc_view_save_image(NsProcView *view)
{ if (view) view_save(view, FALSE); }

static void
on_ctx_save_pdf(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; view_save(ud, TRUE); }

static void
on_ctx_save_png(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; view_save(ud, FALSE); }

static void
ctx_action_enable(NsProcView *v, const char *name, gboolean on)
{
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(v->ctx_actions),
                                              name);
    if (act)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(act), on);
}

/* Registers the "ctx." context-menu action group on the drawing area. */
static void
ctx_install_actions(NsProcView *v)
{
    static const GActionEntry entries[] = {
        { "back",        on_ctx_back,        NULL, NULL, NULL, {0} },
        { "forward",     on_ctx_forward,     NULL, NULL, NULL, {0} },
        { "reload",      on_ctx_reload,      NULL, NULL, NULL, {0} },
        { "copy-url",    on_ctx_copy_url,    NULL, NULL, NULL, {0} },
        { "open-link",   on_ctx_open_link,   NULL, NULL, NULL, {0} },
        { "copy-link",   on_ctx_copy_link,   NULL, NULL, NULL, {0} },
        { "copy-sel",    on_ctx_copy_sel,    NULL, NULL, NULL, {0} },
        { "select-all",  on_ctx_select_all,  NULL, NULL, NULL, {0} },
        { "save-pdf",    on_ctx_save_pdf,    NULL, NULL, NULL, {0} },
        { "save-png",    on_ctx_save_png,    NULL, NULL, NULL, {0} },
    };
    v->ctx_actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(v->ctx_actions), entries,
                                    G_N_ELEMENTS(entries), v);
    gtk_widget_insert_action_group(v->area, "ctx",
                                   G_ACTION_GROUP(v->ctx_actions));
}

/* Re-presents a mapped popover from idle so GTK recomputes its size and position. */
static gboolean
popover_present_idle(gpointer data)
{
    GtkWidget *pop = data;
    if (gtk_widget_get_mapped(pop))
        gtk_popover_present(GTK_POPOVER(pop));
    g_object_unref(pop);
    return G_SOURCE_REMOVE;
}

static void
on_popover_menu_mapped(GtkWidget *pop, gpointer user_data)
{
    (void)user_data;
    g_idle_add(popover_present_idle, g_object_ref(pop));
}

/* Makes a popover re-present itself once mapped so it fits after its contents settle. */
void
ns_popover_menu_fit(GtkWidget *popover)
{
    if (!GTK_IS_POPOVER(popover)) return;
    g_signal_connect(popover, "map", G_CALLBACK(on_popover_menu_mapped), NULL);
}

/* Builds and pops up the page context menu at the click point, with link/selection items. */
static void
show_context_menu(NsProcView *v, const char *href)
{
    g_free(v->ctx_link);
    v->ctx_link = (href && *href) ? g_strdup(href) : NULL;

    ctx_action_enable(v, "back", ns_proc_view_can_back(v));
    ctx_action_enable(v, "forward", ns_proc_view_can_forward(v));
    ctx_action_enable(v, "open-link", v->ctx_link != NULL);
    ctx_action_enable(v, "copy-link", v->ctx_link != NULL);
    ctx_action_enable(v, "copy-sel", v->has_selection);

    GMenu *menu = g_menu_new();
    if (v->ctx_link) {
        GMenu *s = g_menu_new();
        g_menu_append(s, ns_i18n("Open Link"), "ctx.open-link");
        g_menu_append(s, ns_i18n("Copy Link Address"), "ctx.copy-link");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
        g_object_unref(s);
    }
    if (v->has_selection) {
        GMenu *s = g_menu_new();
        g_menu_append(s, ns_i18n("Copy"), "ctx.copy-sel");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
        g_object_unref(s);
    }
    GMenu *nav = g_menu_new();
    g_menu_append(nav, ns_i18n("Back"), "ctx.back");
    g_menu_append(nav, ns_i18n("Forward"), "ctx.forward");
    g_menu_append(nav, ns_i18n("Reload"), "ctx.reload");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(nav));
    g_object_unref(nav);
    GMenu *page = g_menu_new();
    g_menu_append(page, ns_i18n("Select All"), "ctx.select-all");
    g_menu_append(page, ns_i18n("Copy Page Address"), "ctx.copy-url");
    g_menu_append(page, ns_i18n("Save Page as PDF…"), "ctx.save-pdf");
    g_menu_append(page, ns_i18n("Save Page as Image…"), "ctx.save-png");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(page));
    g_object_unref(page);

    if (v->ctx_popover)
        gtk_widget_unparent(v->ctx_popover);
    v->ctx_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(v->ctx_popover, v->area);
    ns_popover_menu_fit(v->ctx_popover);
    gtk_popover_set_has_arrow(GTK_POPOVER(v->ctx_popover), FALSE);
    gtk_popover_set_pointing_to(GTK_POPOVER(v->ctx_popover),
        &(GdkRectangle){ (int)v->ctx_x, (int)v->ctx_y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(v->ctx_popover));
}

/* Right-click: has the engine dispatch contextmenu before any menu is shown. */
static void
on_secondary_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                     gpointer data)
{
    (void)n_press;
    NsProcView *v = data;
    if (!v->opened)
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    v->ctx_x = x;
    v->ctx_y = y;
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    request_link(v, px, py, ACT_CONTEXT);
}

/* Posts a find-in-page query (0 = refresh, 1 = next, 2 = previous) from the current scroll. */
static void
request_find(NsProcView *v, int direction)
{
    if (!v->opened || !v->search_entry)
        return;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search_entry));
    Req *req = g_new0(Req, 1);
    req->type = REQ_FIND;
    req->seq = ++v->find_seq;
    req->query = g_strdup(q ? q : "");
    req->find_dir = direction;
    req->find_from_y = v->scroll_y;
    req->find_case = v->find_case;
    push_req(v, req);
}

/* Reveals the find bar, focuses and selects its text, and re-runs any existing query. */
static void
search_open(NsProcView *v)
{
    if (!v->search_revealer)
        return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), TRUE);
    gtk_widget_grab_focus(v->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(v->search_entry), 0, -1);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search_entry));
    if (q && *q)
        request_find(v, 0);
}

/* Hides the find bar, clears highlights with an empty query and refocuses the page. */
static void
search_close(NsProcView *v)
{
    if (!v->search_revealer)
        return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), FALSE);
    gtk_label_set_text(GTK_LABEL(v->search_label), "");
    Req *req = g_new0(Req, 1);
    req->type = REQ_FIND;
    req->seq = ++v->find_seq;
    req->query = g_strdup("");
    req->find_from_y = v->scroll_y;
    push_req(v, req);
    gtk_widget_grab_focus(v->area);
}

void
ns_proc_view_find_open(NsProcView *v)
{
    search_open(v);
}

static void
on_search_changed(GtkSearchEntry *e, gpointer data)
{ (void)e; request_find(data, 0); }

static void
on_search_next(GtkWidget *w, gpointer data)
{ (void)w; request_find(data, 1); }

static void
on_search_prev(GtkWidget *w, gpointer data)
{ (void)w; request_find(data, 2); }

/* Closes the find bar if visible; TRUE if it was open (so Esc was consumed). */
gboolean
ns_proc_view_find_close(NsProcView *view)
{
    if (!view || !view->search_revealer ||
        !gtk_revealer_get_reveal_child(GTK_REVEALER(view->search_revealer)))
        return FALSE;
    search_close(view);
    return TRUE;
}

static void
on_search_stop(GtkSearchEntry *e, gpointer data)
{ (void)e; search_close(data); }

static void
on_search_close_clicked(GtkButton *b, gpointer data)
{ (void)b; search_close(data); }

/* Press: Ctrl or middle button navigates links, else click and shift/word/line select. */
static void
on_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
           gpointer data)
{
    NsProcView *v = data;
    if (!v->opened)
        return;
    GdkModifierType mods =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(gesture));
    guint button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));
    gtk_widget_grab_focus(v->area);
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    if (button == GDK_BUTTON_MIDDLE || (mods & GDK_CONTROL_MASK)) {
        request_link(v, px, py, ACT_NAVIGATE);
        return;
    }
    int kmods = ((mods & GDK_SHIFT_MASK)   ? 1 : 0) |
                ((mods & GDK_CONTROL_MASK) ? 2 : 0) |
                ((mods & GDK_ALT_MASK)     ? 4 : 0) |
                ((mods & GDK_META_MASK)    ? 8 : 0);
    if (!(mods & GDK_SHIFT_MASK))
        v->has_selection = FALSE;
    start_click(v, px, py, kmods);
    v->multi_click = n_press >= 2;
    if (mods & GDK_SHIFT_MASK) {
        start_select(v, 1, px, py);
        v->has_selection = TRUE;
    } else if (n_press == 2) {
        start_select(v, 5, px, py);
        v->has_selection = TRUE;
    } else if (n_press >= 3) {
        start_select(v, 6, px, py);
        v->has_selection = TRUE;
    }
}

/* Sends pointer release to the engine and refreshes the hover link state. */
static void
on_released(GtkGestureClick *gesture, int n_press, double x, double y,
            gpointer data)
{
    (void)gesture; (void)n_press;
    NsProcView *v = data;
    if (v->opened) {
        double s = cur_scale(v);
        int px = v->scroll_x + (int)(x / s);
        int py = v->scroll_y + (int)(y / s);
        start_release(v, px, py);
        request_link(v, px, py, ACT_HOVER);
    }
}

/* Tracks the pointer and sends a coalesced hover query in page coordinates. */
static void
on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    (void)ctrl;
    NsProcView *v = data;
    v->pointer_x = x;
    v->pointer_y = y;
    if (v->opened) {
        double s = cur_scale(v);
        int px = v->scroll_x + (int)(x / s);
        int py = v->scroll_y + (int)(y / s);
        request_hover(v, px, py);
    }
}

static void
push_scrollbar(NsProcView *v, int kind, int px, int py)
{
    Req *req = g_new0(Req, 1);
    req->type = REQ_SCROLLBAR;
    req->kind = kind;
    req->x = px;
    req->y = py;
    push_req(v, req);
}

/* Starts a drag by probing whether the press hit a page scrollbar. */
static void
on_drag_begin(GtkGestureDrag *g, double sx, double sy, gpointer data)
{
    (void)g;
    NsProcView *v = data;
    v->drag_start_x = sx;
    v->drag_start_y = sy;
    v->drag_anchored = FALSE;
    v->sb_dragging = FALSE;
    v->sb_have_last = FALSE;
    v->sb_probe = FALSE;
    if (v->opened) {
        double s = cur_scale(v);
        v->sb_probe = TRUE;
        push_scrollbar(v, 0, v->scroll_x + (int)(sx / s),
                       v->scroll_y + (int)(sy / s));
    }
}

/* Drag: moves a grabbed scrollbar, buffers while probing, else extends the text selection. */
static void
on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    NsProcView *v = data;
    if (!v->opened)
        return;
    double s = cur_scale(v);
    double wx = v->drag_start_x + ox;
    double wy = v->drag_start_y + oy;
    if (v->sb_dragging) {
        push_scrollbar(v, 1, v->scroll_x + (int)(wx / s),
                       v->scroll_y + (int)(wy / s));
        return;
    }
    if (v->sb_probe) {
        v->sb_last_x = wx;
        v->sb_last_y = wy;
        v->sb_have_last = TRUE;
        return;
    }
    if (v->multi_click) {
        start_select(v, 1, v->scroll_x + (int)(wx / s),
                     v->scroll_y + (int)(wy / s));
        return;
    }
    if (!v->drag_anchored) {
        start_select(v, 0, v->scroll_x + (int)(v->drag_start_x / s),
                     v->scroll_y + (int)(v->drag_start_y / s));
        v->drag_anchored = TRUE;
    }
    start_select(v, 1, v->scroll_x + (int)(wx / s),
                 v->scroll_y + (int)(wy / s));
    v->has_selection = TRUE;
}

/* Ends a scrollbar drag or probe in the engine. */
static void
on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    (void)ox;
    (void)oy;
    NsProcView *v = data;
    if (v->sb_dragging || v->sb_probe) {
        push_scrollbar(v, 2, 0, 0);
        v->sb_dragging = FALSE;
        v->sb_probe = FALSE;
        v->sb_have_last = FALSE;
    }
}

/* Clipboard read callback: delivers pasted text to the page as text input. */
static void
on_paste_text_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    NsProcView *v = data;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && *text && v->opened)
        start_key_text(v, 2, text);
    g_free(text);
    pv_unref(v);
}

/* Key press: devtools/zoom/find/clipboard shortcuts, else DOM key events, scroll fallback. */
static gboolean
on_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
       GdkModifierType state, gpointer data)
{
    (void)ctrl;
    (void)keycode;
    NsProcView *v = data;
    if (!v->opened)
        return FALSE;
    if (keyval == GDK_KEY_F12) {
        console_set_open(v, !v->console_open);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_j || keyval == GDK_KEY_J)) {
        console_set_open(v, !v->console_open);
        return TRUE;
    }
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc && uc != ' ' && !g_unichar_iscntrl(uc) &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK))) {
        start_key(v, 0, keyval, state);
        start_key(v, 3, keyval, state);
        return FALSE;
    }
    if (state & GDK_CONTROL_MASK) {
        start_key(v, 0, keyval, state);
        switch (keyval) {
        case GDK_KEY_c:
        case GDK_KEY_C:          start_select(v, 4, 0, 0); return TRUE;
        case GDK_KEY_v:
        case GDK_KEY_V:
            gdk_clipboard_read_text_async(gtk_widget_get_clipboard(v->area),
                                          NULL, on_paste_text_ready, pv_ref(v));
            return TRUE;
        case GDK_KEY_a:
        case GDK_KEY_A:          start_select(v, 3, 0, 0); return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:      ns_proc_view_zoom_in(v); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: ns_proc_view_zoom_out(v); return TRUE;
        case GDK_KEY_0:
        case GDK_KEY_KP_0:        ns_proc_view_zoom_reset(v); return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F:           search_open(v); return TRUE;
        case GDK_KEY_g:
        case GDK_KEY_G:
            request_find(v, (state & GDK_SHIFT_MASK) ? 2 : 1);
            return TRUE;
        case GDK_KEY_p:
        case GDK_KEY_P:           view_save(v, TRUE); return TRUE;
        default: return FALSE;
        }
    }
    if ((state & GDK_ALT_MASK) && !(state & GDK_CONTROL_MASK) &&
        !(state & GDK_META_MASK)) {
        gunichar a = gdk_keyval_to_unicode(keyval);
        if (a && !g_unichar_iscntrl(a)) {
            start_key(v, 0, keyval, state);
            return TRUE;
        }
    }
    double line = 60.0;
    double page = viewport_h(v) / cur_scale(v) - line;
    if (page < line) page = line;
    double vy = gtk_adjustment_get_value(v->vadj);
    double vx = gtk_adjustment_get_value(v->hadj);
    double tx = vx, ty = vy;
    switch (keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: start_key(v, 0, keyval, state); return TRUE;
    case GDK_KEY_Down:       ty = vy + line; break;
    case GDK_KEY_Up:         ty = vy - line; break;
    case GDK_KEY_Right:      tx = vx + line; break;
    case GDK_KEY_Left:       tx = vx - line; break;
    case GDK_KEY_Page_Down:
    case GDK_KEY_space:      ty = vy + page; break;
    case GDK_KEY_Page_Up:    ty = vy - page; break;
    case GDK_KEY_Home:       ty = 0; break;
    case GDK_KEY_End:        ty = gtk_adjustment_get_upper(v->vadj); break;
    default:                 start_key(v, 0, keyval, state); return FALSE;
    }
    start_key_full(v, 0, keyval, state, 1, tx, ty);
    return TRUE;
}

static void
on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
    (void)ctrl;
    (void)keycode;
    NsProcView *v = data;
    if (v->opened)
        start_key(v, 1, keyval, state);
}

/* Tears down UI when the drawing area dies and posts REQ_CLOSE to free the session. */
static void
on_area_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    NsProcView *v = data;
    v->closed = TRUE;
    g_clear_object(&v->im);
    disarm_anim(v);
    if (v->console_poll_id) {
        g_source_remove(v->console_poll_id);
        v->console_poll_id = 0;
    }
    if (v->console_window) {
        GtkWidget *win = v->console_window;
        v->console_window = NULL;
        v->console_notebook = NULL;
        v->console_entry = NULL;
        v->console_view = NULL;
        v->console_buffer = NULL;
        v->net_view = NULL;
        v->net_buffer = NULL;
        v->perf_view = NULL;
        v->perf_buffer = NULL;
        v->layout_view = NULL;
        v->layout_buffer = NULL;
        v->elements_view = NULL;
        v->elements_buffer = NULL;
        v->inspect_entry = NULL;
        gtk_window_destroy(GTK_WINDOW(win));
    }
    if (v->ctx_popover) {
        gtk_widget_unparent(v->ctx_popover);
        v->ctx_popover = NULL;
    }
    v->area = NULL;
    v->perm_revealer = NULL;
    v->perm_label = NULL;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CLOSE;
    push_req(v, req);
    pv_unref(v);
}

/* Appends text to the devtools console buffer and scrolls it to the end. */
static void
console_append(NsProcView *v, const char *text)
{
    if (!v->console_buffer || !text || !*text)
        return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(v->console_buffer, &end);
    gtk_text_buffer_insert(v->console_buffer, &end, text, -1);
    if (v->console_view) {
        gtk_text_buffer_get_end_iter(v->console_buffer, &end);
        GtkTextMark *m = gtk_text_buffer_create_mark(v->console_buffer, NULL,
                                                     &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(v->console_view), m);
        gtk_text_buffer_delete_mark(v->console_buffer, m);
    }
}

/* Periodically requests new console output while the devtools window is open. */
static gboolean
console_poll_cb(gpointer data)
{
    NsProcView *v = data;
    if (!v->console_open || !v->opened)
        return G_SOURCE_CONTINUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CONSOLE;
    push_req(v, req);
    return G_SOURCE_CONTINUE;
}

/* Echoes the entry text to the console and posts it to the engine for JS evaluation. */
static void
on_console_eval(GtkEntry *entry, gpointer data)
{
    NsProcView *v = data;
    const char *src = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!src || !*src || !v->opened)
        return;
    char *echo = g_strdup_printf("> %s\n", src);
    console_append(v, echo);
    g_free(echo);
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->query = g_strdup(src);
    push_req(v, req);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void build_console_window(NsProcView *v);

/* Shows or hides the devtools window, starting or stopping the console poll timer. */
static void
console_set_open(NsProcView *v, gboolean open)
{
    if (open && !v->console_window)
        build_console_window(v);
    if (!v->console_window)
        return;
    v->console_open = open;
    if (open) {
        GtkRoot *root = gtk_widget_get_root(v->area);
        if (GTK_IS_WINDOW(root))
            gtk_window_set_transient_for(GTK_WINDOW(v->console_window),
                                         GTK_WINDOW(root));
        gtk_window_present(GTK_WINDOW(v->console_window));
        if (!v->console_poll_id)
            v->console_poll_id = g_timeout_add(NS_PROC_CONSOLE_POLL_MS, console_poll_cb, v);
        gtk_widget_grab_focus(v->console_entry);
    } else {
        gtk_widget_set_visible(v->console_window, FALSE);
        if (v->console_poll_id) {
            g_source_remove(v->console_poll_id);
            v->console_poll_id = 0;
        }
        if (v->area)
            gtk_widget_grab_focus(v->area);
    }
}

/* Requests the network/performance/layout/DOM dump for a devtools tab. */
static void
console_request_dump(NsProcView *v, int tab)
{
    const char *kind = NULL;
    switch (tab) {
    case DEV_TAB_NETWORK:     kind = "network";     break;
    case DEV_TAB_PERFORMANCE: kind = "performance"; break;
    case DEV_TAB_LAYOUT:      kind = "layout";      break;
    case DEV_TAB_ELEMENTS:    kind = "dom";         break;
    default: return;
    }
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_DUMP;
    req->dump_tab = tab;
    req->query = g_strdup(kind);
    push_req(v, req);
}

static int
console_current_tab(NsProcView *v)
{
    if (!v->console_notebook)
        return DEV_TAB_CONSOLE;
    return gtk_notebook_get_current_page(GTK_NOTEBOOK(v->console_notebook));
}

/* Clears the text buffer of the currently selected devtools tab. */
static void
on_console_clear(GtkButton *b, gpointer data)
{
    (void)b;
    NsProcView *v = data;
    GtkTextBuffer *buf = NULL;
    switch (console_current_tab(v)) {
    case DEV_TAB_CONSOLE:     buf = v->console_buffer;  break;
    case DEV_TAB_NETWORK:     buf = v->net_buffer;      break;
    case DEV_TAB_PERFORMANCE: buf = v->perf_buffer;     break;
    case DEV_TAB_LAYOUT:      buf = v->layout_buffer;   break;
    case DEV_TAB_ELEMENTS:    buf = v->elements_buffer; break;
    default: break;
    }
    if (buf)
        gtk_text_buffer_set_text(buf, "", 0);
}

static void
on_console_refresh(GtkButton *b, gpointer data)
{
    (void)b;
    NsProcView *v = data;
    console_request_dump(v, console_current_tab(v));
}

static void
on_console_notebook_switch(GtkNotebook *nb, GtkWidget *page, guint num,
                           gpointer data)
{
    (void)nb;
    (void)page;
    NsProcView *v = data;
    console_request_dump(v, (int)num);
}

/* Evaluates querySelector on the entered selector and shows its outerHTML (20k cap). */
static void
on_inspect_activate(GtkEntry *entry, gpointer data)
{
    NsProcView *v = data;
    const char *sel = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!sel || !*sel || !v->opened)
        return;
    char *esc = g_strescape(sel, NULL);
    char *src = g_strdup_printf(
        "(function(){try{var e=document.querySelector(\"%s\");"
        "if(!e)return \"\";var s=e.outerHTML;"
        "return s.length>20000?s.slice(0,20000)+\"\\n\\u2026(truncated)\":s;}"
        "catch(err){return \"Error: \"+err;}})()",
        esc);
    g_free(esc);
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->inspect = TRUE;
    req->dump_tab = DEV_TAB_ELEMENTS;
    req->query = src;
    push_req(v, req);
}

static gboolean
on_console_close_request(GtkWindow *win, gpointer data)
{
    (void)win;
    console_set_open(data, FALSE);
    return TRUE;
}

/* Creates a read-only monospace text view in a scroller; returns the scroller. */
static GtkWidget *
console_make_view(GtkWidget **out_view, GtkTextBuffer **out_buffer,
                  gboolean wrap)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                                wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    *out_view = view;
    *out_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static void
console_add_tab(NsProcView *v, GtkWidget *child, const char *title)
{
    gtk_notebook_append_page(GTK_NOTEBOOK(v->console_notebook), child,
                             gtk_label_new(ns_i18n(title)));
}

/* Builds the devtools window: Console, Network, Performance, Layout and Elements tabs. */
static void
build_console_window(NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(header, "toolbar");
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, ns_i18n("Refresh"));
    set_accessible_label(refresh, ns_i18n("Refresh"));
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_console_refresh), v);
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, ns_i18n("Clear"));
    set_accessible_label(clear, ns_i18n("Clear"));
    g_signal_connect(clear, "clicked", G_CALLBACK(on_console_clear), v);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), refresh);
    gtk_box_append(GTK_BOX(header), clear);

    v->console_notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(v->console_notebook, TRUE);

    GtkWidget *console_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *console_scroll =
        console_make_view(&v->console_view, &v->console_buffer, TRUE);
    v->console_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->console_entry),
                                   ns_i18n("Evaluate JavaScript and press Enter"));
    g_signal_connect(v->console_entry, "activate",
                     G_CALLBACK(on_console_eval), v);
    gtk_box_append(GTK_BOX(console_page), console_scroll);
    gtk_box_append(GTK_BOX(console_page), v->console_entry);
    console_add_tab(v, console_page, "Console");

    console_add_tab(v, console_make_view(&v->net_view, &v->net_buffer, FALSE),
                    "Network");
    console_add_tab(v, console_make_view(&v->perf_view, &v->perf_buffer, FALSE),
                    "Performance");
    console_add_tab(v,
                    console_make_view(&v->layout_view, &v->layout_buffer, FALSE),
                    "Layout");

    GtkWidget *elements_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *elements_scroll =
        console_make_view(&v->elements_view, &v->elements_buffer, FALSE);
    v->inspect_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(v->inspect_entry),
        ns_i18n("Inspect: CSS selector, then Enter"));
    g_signal_connect(v->inspect_entry, "activate",
                     G_CALLBACK(on_inspect_activate), v);
    gtk_box_append(GTK_BOX(elements_page), elements_scroll);
    gtk_box_append(GTK_BOX(elements_page), v->inspect_entry);
    console_add_tab(v, elements_page, "Elements");

    g_signal_connect(v->console_notebook, "switch-page",
                     G_CALLBACK(on_console_notebook_switch), v);

    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), v->console_notebook);

    v->console_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(v->console_window),
                         ns_i18n("Developer Tools"));
    gtk_window_set_default_size(GTK_WINDOW(v->console_window), 720, 420);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(v->console_window), TRUE);
    gtk_window_set_child(GTK_WINDOW(v->console_window), box);
    g_signal_connect(v->console_window, "close-request",
                     G_CALLBACK(on_console_close_request), v);
}

/* Builds the find-in-page bar as a top-right overlay revealer. */
static void
build_search_bar(NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(box, "toolbar");
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_end(box, 6);

    v->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(v->search_entry, 220, -1);
    set_accessible_label(v->search_entry, ns_i18n("Find in page"));
    g_signal_connect(v->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), v);
    g_signal_connect(v->search_entry, "activate",
                     G_CALLBACK(on_search_next), v);
    g_signal_connect(v->search_entry, "next-match",
                     G_CALLBACK(on_search_next), v);
    g_signal_connect(v->search_entry, "previous-match",
                     G_CALLBACK(on_search_prev), v);
    g_signal_connect(v->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), v);

    v->search_label = gtk_label_new("");
    gtk_widget_set_size_request(v->search_label, 56, -1);

    GtkWidget *prev = gtk_button_new_from_icon_name("go-up-symbolic");
    GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic");
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(prev, ns_i18n("Previous match (Shift+Enter)"));
    gtk_widget_set_tooltip_text(next, ns_i18n("Next match (Enter)"));
    gtk_widget_set_tooltip_text(close, ns_i18n("Close (Esc)"));
    set_accessible_label(prev, ns_i18n("Previous match"));
    set_accessible_label(next, ns_i18n("Next match"));
    set_accessible_label(close, ns_i18n("Close search"));
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search_prev), v);
    g_signal_connect(next, "clicked", G_CALLBACK(on_search_next), v);
    g_signal_connect(close, "clicked", G_CALLBACK(on_search_close_clicked), v);

    gtk_box_append(GTK_BOX(box), v->search_entry);
    gtk_box_append(GTK_BOX(box), v->search_label);
    gtk_box_append(GTK_BOX(box), prev);
    gtk_box_append(GTK_BOX(box), next);
    gtk_box_append(GTK_BOX(box), close);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    v->search_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(v->search_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(v->search_revealer), frame);
    gtk_widget_set_halign(v->search_revealer, GTK_ALIGN_END);
    gtk_widget_set_valign(v->search_revealer, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(v->overlay), v->search_revealer);
}

/* Builds the camera/microphone permission bar with Allow / Not now / dismiss. */
static void
build_perm_bar(NsProcView *v)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);

    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-question-symbolic");

    v->perm_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->perm_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(v->perm_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(v->perm_label, TRUE);

    GtkWidget *allow = gtk_button_new_with_label(ns_i18n("Allow"));
    gtk_widget_add_css_class(allow, "suggested-action");
    gtk_widget_set_tooltip_text(allow, ns_i18n("Allow and trust this site"));
    GtkWidget *deny = gtk_button_new_with_label(ns_i18n("Not now"));
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    set_accessible_label(close, ns_i18n("Dismiss"));
    g_signal_connect(allow, "clicked", G_CALLBACK(on_perm_allow), v);
    g_signal_connect(deny, "clicked", G_CALLBACK(on_perm_deny), v);
    g_signal_connect(close, "clicked", G_CALLBACK(on_perm_deny), v);

    gtk_box_append(GTK_BOX(bar), icon);
    gtk_box_append(GTK_BOX(bar), v->perm_label);
    gtk_box_append(GTK_BOX(bar), allow);
    gtk_box_append(GTK_BOX(bar), deny);
    gtk_box_append(GTK_BOX(bar), close);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), bar);

    v->perm_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(v->perm_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(v->perm_revealer), frame);
}

/* Collects dropped local file paths and forwards them to the page at the drop point. */
static gboolean
on_file_drop(GtkDropTarget *target, const GValue *value, double x, double y,
             gpointer data)
{
    (void)target;
    NsProcView *v = data;
    if (!v->opened || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    GdkFileList *fl = g_value_get_boxed(value);
    if (!fl)
        return FALSE;
    GSList *files = gdk_file_list_get_files(fl);
    GString *paths = g_string_new(NULL);
    for (GSList *l = files; l; l = l->next) {
        char *p = g_file_get_path(G_FILE(l->data));
        if (!p)
            continue;
        if (paths->len)
            g_string_append_c(paths, '\n');
        g_string_append(paths, p);
        g_free(p);
    }
    g_slist_free(files);
    if (paths->len == 0) {
        g_string_free(paths, TRUE);
        return FALSE;
    }
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    start_dropfiles(v, px, py, g_string_free(paths, FALSE));
    return TRUE;
}

/* Creates the page view widget tree, scrollbars, overlays and all input controllers. */
NsProcView *
ns_proc_view_new(void)
{
    NsProcView *v = g_new0(NsProcView, 1);
    g_atomic_ref_count_init(&v->rc);
    v->history = g_ptr_array_new_with_free_func(g_free);
    v->hist_index = -1;
    v->link_pending_action = ACT_HOVER;
    v->pending_record = TRUE;
    v->scale = 1.0;

    v->hadj = gtk_adjustment_new(0, 0, 1, 60, 60, 1);
    v->vadj = gtk_adjustment_new(0, 0, 1, 60, 60, 1);
    g_signal_connect(v->hadj, "value-changed", G_CALLBACK(on_adj_changed), v);
    g_signal_connect(v->vadj, "value-changed", G_CALLBACK(on_adj_changed), v);

    v->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(v->area, TRUE);
    gtk_widget_set_vexpand(v->area, TRUE);
    gtk_widget_set_focusable(v->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->area), on_draw, v, NULL);
    g_signal_connect(v->area, "resize", G_CALLBACK(on_resize), v);
    g_signal_connect(v->area, "notify::has-focus",
                     G_CALLBACK(on_area_focus_notify), v);

    GtkWidget *grid = gtk_grid_new();
    v->vscroll =
        gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->vadj);
    v->hscroll =
        gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, v->hadj);
    gtk_grid_attach(GTK_GRID(grid), v->area, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v->vscroll, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v->hscroll, 0, 1, 1, 1);
    gtk_widget_set_visible(v->vscroll, FALSE);
    gtk_widget_set_visible(v->hscroll, FALSE);

    v->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(v->overlay), grid);
    gtk_widget_set_vexpand(v->overlay, TRUE);
    build_perm_bar(v);
    v->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(v->root), v->perm_revealer);
    gtk_box_append(GTK_BOX(v->root), v->overlay);
    gtk_widget_set_vexpand(v->root, TRUE);
    build_search_bar(v);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), v);
    g_signal_connect(click, "released", G_CALLBACK(on_released), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(drag));

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                              GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_file_drop), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(drop));

    GtkGesture *middle = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
    g_signal_connect(middle, "pressed", G_CALLBACK(on_pressed), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(middle));

    ctx_install_actions(v);
    GtkGesture *secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(on_secondary_pressed), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(secondary));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    gtk_widget_add_controller(v->area, motion);

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), v);
    gtk_widget_add_controller(v->area, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), v);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), v);
    v->im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(v->im, v->area);
    g_signal_connect(v->im, "commit", G_CALLBACK(on_im_commit), v);
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key),
                                            v->im);
    gtk_widget_add_controller(v->area, key);

    g_signal_connect(v->area, "destroy", G_CALLBACK(on_area_destroy), v);

    return v;
}

GtkWidget *ns_proc_view_widget(NsProcView *v) { return v->root; }

void
ns_proc_view_set_notify(NsProcView *v, NsProcNotify cb, gpointer ud)
{
    v->notify = cb;
    v->notify_ud = ud;
}

void
ns_proc_view_set_private(NsProcView *v, gboolean private_mode)
{
    if (v)
        v->private_mode = private_mode;
}

gboolean
ns_proc_view_is_private(NsProcView *v)
{
    return v && v->private_mode;
}
