/* Northstar Web Browser — one page host session: open page, back/forward cache, last frame.
 * Damage-tracked repaint, scroll reuse, input routing and dumps; used by headless and procview.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "page_session.h"
#include "damage.h"
#include "libnorthstar.h"
#include "mainctx.h"
#include "net.h"
#include "paint.h"
#include "videolayer.h"
#include "trace.h"

#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NS_BFCACHE_MAX 4
#define NS_DAMAGE_MAX_FRACTION 0.6
#define NS_DAMAGE_EDGE_PX 48
#define NS_DAMAGE_MAX_RECTS 8

struct ns_page_session {
    unsigned char *fb;
    int            max_w;
    int            max_h;
    ns_browser    *cur;
    ns_browser    *bf[NS_BFCACHE_MAX];
    int            bf_n;
    int            tick_budget_ms;
    int            frame_valid;
    long           frame_sx;
    long           frame_sy;
    int            frame_w;
    int            frame_h;
    double         frame_scale;
    guint64        frame_gen;
    unsigned char *scratch_fb;
    unsigned char *verify_fb;
    char          *page_nav;
    char          *post_url;
    char          *post_body;
    size_t         post_len;
    char          *post_ct;
    GSource       *wake;
    ns_page_session_wake_cb wake_cb;
    gpointer       wake_data;
    gboolean       rendering;
    gboolean       wake_posted;
    gboolean       video_layers_enabled;
    GArray        *video_layers;
};

typedef struct {
    GSource          source;
    ns_page_session *session;
} session_wake_source;

/* Returns TRUE when the page needs a frame and no wake is posted or render in progress. */
static gboolean
session_wake_due(ns_page_session *s)
{
    return s->cur && s->wake_cb && !s->rendering && !s->wake_posted &&
           ns_browser_needs_frame(s->cur);
}

/* GSource prepare: fires when a frame is due, else times out at the page's next wake time. */
static gboolean
session_wake_prepare(GSource *source, gint *timeout)
{
    ns_page_session *s = ((session_wake_source *)source)->session;
    *timeout = -1;
    if (session_wake_due(s))
        return TRUE;
    gint64 due = s->cur && !s->wake_posted ? ns_browser_next_wake_us(s->cur)
                                           : 0;
    if (due > 0) {
        gint64 wait_ms = (due - g_get_monotonic_time() + 999) / 1000;
        *timeout = (gint)CLAMP(wait_ms, 0, G_MAXINT);
    }
    return FALSE;
}

static gboolean
session_wake_check(GSource *source)
{
    return session_wake_due(((session_wake_source *)source)->session);
}

/* Posts the wake callback once per frame request until the next render. */
static gboolean
session_wake_dispatch(GSource *source, GSourceFunc callback,
                      gpointer user_data)
{
    (void)callback;
    (void)user_data;
    ns_page_session *s = ((session_wake_source *)source)->session;
    if (session_wake_due(s)) {
        s->wake_posted = TRUE;
        s->wake_cb(s->wake_data);
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs session_wake_funcs = {
    .prepare = session_wake_prepare,
    .check = session_wake_check,
    .dispatch = session_wake_dispatch,
};

/* Sets the frame-wake callback, attaching the wake GSource to the engine context once. */
void
ns_page_session_set_wake(ns_page_session *s, ns_page_session_wake_cb cb,
                         gpointer user_data)
{
    if (!s)
        return;
    s->wake_cb = cb;
    s->wake_data = user_data;
    s->wake_posted = FALSE;
    if (s->wake || !cb)
        return;
    s->wake = g_source_new(&session_wake_funcs, sizeof(session_wake_source));
    ((session_wake_source *)s->wake)->session = s;
    g_source_attach(s->wake, ns_engine_context());
}

void
ns_page_session_set_frame_time(ns_page_session *s, gint64 frame_time_us)
{
    if (s && s->cur)
        ns_browser_set_frame_time(s->cur, frame_time_us);
}

/* Parks an eligible page in the back/forward cache, evicting the oldest; else closes it. */
static void
session_bfcache_park_or_close(ns_page_session *s, ns_browser *b)
{
    if (!b)
        return;
    if (!ns_browser_bfcache_eligible(b)) {
        ns_browser_close(b);
        return;
    }
    ns_browser_bfcache_park(b);
    if (s->bf_n >= NS_BFCACHE_MAX) {
        ns_browser_close(s->bf[0]);
        for (int i = 1; i < s->bf_n; i++)
            s->bf[i - 1] = s->bf[i];
        s->bf_n--;
    }
    s->bf[s->bf_n++] = b;
}

/* Removes and returns the newest bfcache page whose URL equals url, or NULL. */
static ns_browser *
session_bfcache_take(ns_page_session *s, const char *url)
{
    if (!url)
        return NULL;
    for (int i = s->bf_n - 1; i >= 0; i--) {
        char *u = ns_browser_url(s->bf[i]);
        int match = u && strcmp(u, url) == 0;
        free(u);
        if (!match)
            continue;
        ns_browser *b = s->bf[i];
        for (int j = i + 1; j < s->bf_n; j++)
            s->bf[j - 1] = s->bf[j];
        s->bf_n--;
        return b;
    }
    return NULL;
}

/* Records the navigation href the page asked for and saves its pending form POST body. */
static void
session_stash_post(ns_page_session *s, const char *href)
{
    if (!s->cur)
        return;
    free(s->page_nav);
    s->page_nav = (href && *href) ? strdup(href) : NULL;
    size_t len = 0;
    char *ct = NULL;
    char *pb = ns_browser_take_post(s->cur, &len, &ct);
    if (!pb)
        return;
    free(s->post_url);
    free(s->post_body);
    free(s->post_ct);
    s->post_url = (href && *href) ? strdup(href) : NULL;
    s->post_body = pb;
    s->post_len = len;
    s->post_ct = ct;
}

static void
session_clear_post(ns_page_session *s)
{
    free(s->post_url);
    free(s->post_body);
    free(s->post_ct);
    s->post_url = NULL;
    s->post_body = NULL;
    s->post_ct = NULL;
    s->post_len = 0;
}

static int
clamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* Frees an empty string and returns NULL; otherwise returns s unchanged. */
static char *
empty_to_null(char *s)
{
    if (s && !*s) {
        free(s);
        return NULL;
    }
    return s;
}

/* Replaces CR and LF characters with spaces in place. */
static void
scrub_line_breaks(char *s)
{
    if (!s) return;
    for (char *p = s; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
}

static void
engine_init_once(void)
{
    static gsize inited;
    if (g_once_init_enter(&inited)) {
        ns_browser_init();
        g_once_init_leave(&inited, 1);
    }
}

/* Creates a session with a framebuffer for max size plus damage edge rows. */
ns_page_session *
ns_page_session_new(int max_width, int max_height)
{
    if (max_width <= 0 || max_height <= 0)
        return NULL;
    engine_init_once();
    ns_page_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->fb = malloc((size_t)max_width * 4u *
                   (size_t)(max_height + 2 * NS_DAMAGE_EDGE_PX));
    if (!s->fb) {
        free(s);
        return NULL;
    }
    s->max_w = max_width;
    s->max_h = max_height;
    s->tick_budget_ms = 16;
    s->frame_scale = 1.0;
    return s;
}

/* Destroys the wake source, closes current and cached pages and frees buffers. */
void
ns_page_session_free(ns_page_session *s)
{
    if (!s)
        return;
    if (s->wake) {
        g_source_destroy(s->wake);
        g_source_unref(s->wake);
        s->wake = NULL;
    }
    session_clear_post(s);
    free(s->page_nav);
    for (int i = 0; i < s->bf_n; i++)
        ns_browser_close(s->bf[i]);
    if (s->cur) {
        ns_browser *cur = s->cur;
        s->cur = NULL;
        ns_browser_close(cur);
    }
    if (s->video_layers)
        g_array_free(s->video_layers, TRUE);
    free(s->fb);
    free(s->scratch_fb);
    free(s->verify_fb);
    free(s);
}

/* Fills page info (size, title, URL, pending nav, TLS state, IP) from the current page. */
static void
fill_page_info(ns_page_session *s, const char *fallback_url,
               ns_page_info *out)
{
    memset(out, 0, sizeof *out);
    if (!s->cur) {
        out->url = fallback_url ? strdup(fallback_url) : NULL;
        return;
    }
    const char *ip = NULL;
    out->ok = 1;
    ns_browser_page_size(s->cur, &out->page_width, &out->page_height);
    out->title = ns_browser_title(s->cur);
    out->url = ns_browser_url(s->cur);
    if (!out->url && fallback_url)
        out->url = strdup(fallback_url);
    out->nav = empty_to_null(ns_browser_take_pending_nav(s->cur));
    out->security = ns_browser_security(s->cur, &ip);
    out->remote_ip = ip && *ip ? strdup(ip) : NULL;
}

void
ns_page_info_clear(ns_page_info *info)
{
    if (!info)
        return;
    free(info->title);
    free(info->url);
    free(info->nav);
    free(info->remote_ip);
    memset(info, 0, sizeof *info);
}

/* Navigates: restores from bfcache on history, replays a stashed POST, else opens url. */
int
ns_page_session_open(ns_page_session *s, const char *url, int width,
                     int height, int settle_ms, int history,
                     int user_activated, ns_page_info *out)
{
    if (!s || !out)
        return -1;
    int vw = clamp(width, 1, s->max_w);
    int vh = clamp(height, 1, s->max_h);
    ns_browser *restored = (history && url) ? session_bfcache_take(s, url)
                                            : NULL;
    /* A user-activated load the page did not ask for comes from the
     * browser UI (address bar, menu, bookmarks), which has no initiator. */
    gboolean page_asked = s->page_nav && url && strcmp(s->page_nav, url) == 0;
    gboolean from_page = !history && (!user_activated || page_asked);
    free(s->page_nav);
    s->page_nav = NULL;
    char *referrer = (from_page && s->cur) ? ns_browser_url(s->cur) : NULL;
    char *initiator = s->cur ? ns_browser_take_pending_initiator(s->cur) : NULL;
    if (initiator && from_page) {
        free(referrer);
        referrer = initiator;
    } else {
        free(initiator);
    }
    if (s->cur) {
        ns_browser *previous = s->cur;
        s->cur = NULL;
        session_bfcache_park_or_close(s, previous);
    }
    /* Always pass the initiating document: the network layer needs it to
     * send truthful Origin / Sec-Fetch-Site headers and to withhold
     * cookies from cross-site form posts. */
    if (!restored)
        ns_browser_set_next_navigation(referrer, user_activated != 0);
    free(referrer);
    s->frame_valid = 0;
    ns_net_log_clear();
    if (restored) {
        ns_browser_bfcache_restore(restored, vw, (double)vh);
        s->cur = restored;
    } else if (url && s->post_body && s->post_url &&
               strcmp(url, s->post_url) == 0) {
        s->cur = ns_browser_open_post_viewport(url, vw, vh, settle_ms,
                                               s->post_body, s->post_len,
                                               s->post_ct);
    } else {
        s->cur = url ? ns_browser_open_viewport(url, vw, vh, settle_ms)
                     : NULL;
    }
    session_clear_post(s);
    s->wake_posted = FALSE;
    fill_page_info(s, url, out);
    return s->cur ? 0 : -1;
}

/* Resizes the current page's viewport and reports the new page size. */
int
ns_page_session_set_viewport(ns_page_session *s, int width, int height,
                             ns_page_info *out)
{
    if (out)
        memset(out, 0, sizeof *out);
    if (!s || !s->cur)
        return -1;
    int vw = clamp(width, 1, s->max_w);
    int vh = clamp(height, 1, s->max_h);
    s->frame_valid = 0;
    if (ns_browser_set_viewport(s->cur, vw, vh) != 0)
        return -1;
    if (out) {
        out->ok = 1;
        ns_browser_page_size(s->cur, &out->page_width, &out->page_height);
    }
    return 0;
}

void
ns_page_session_invalidate_frame(ns_page_session *s)
{
    if (s)
        s->frame_valid = 0;
}

void
ns_page_frame_clear(ns_page_frame *frame)
{
    if (!frame)
        return;
    free(frame->nav);
    free(frame->url);
    free(frame->title);
    free(frame->camera);
    free(frame->download);
    free(frame->damage);
    if (frame->video_layers)
        g_array_free(frame->video_layers, TRUE);
    memset(frame, 0, sizeof *frame);
}

static GArray *
video_layers_new(void)
{
    GArray *a = g_array_new(FALSE, TRUE, sizeof(ns_video_layer_rect));
    g_array_set_clear_func(a, ns_video_layer_rect_clear);
    return a;
}

void
ns_page_session_set_video_layers(ns_page_session *s, gboolean enabled)
{
    if (s) s->video_layers_enabled = enabled;
}

/* Merges painted video layers into the session's set and copies live ones to the frame. */
static void
session_merge_video_layers(ns_page_session *s, GArray *painted, gboolean full,
                           double ox, double oy, ns_page_frame *out)
{
    if (!s->video_layers)
        s->video_layers = video_layers_new();
    if (full)
        g_array_set_size(s->video_layers, 0);
    for (guint i = 0; painted && i < painted->len; i++) {
        ns_video_layer_rect r = g_array_index(painted, ns_video_layer_rect, i);
        r.layer = ns_video_layer_ref(r.layer);
        r.x += ox;
        r.y += oy;
        guint j = 0;
        while (j < s->video_layers->len &&
               g_array_index(s->video_layers, ns_video_layer_rect, j).layer !=
                   r.layer)
            j++;
        if (j < s->video_layers->len)
            g_array_remove_index(s->video_layers, j);
        g_array_append_val(s->video_layers, r);
    }
    for (guint i = 0; i < s->video_layers->len;) {
        if (ns_video_layer_detached(
                g_array_index(s->video_layers, ns_video_layer_rect, i).layer))
            g_array_remove_index(s->video_layers, i);
        else
            i++;
    }
    if (s->video_layers->len == 0)
        return;
    out->video_layers = video_layers_new();
    for (guint i = 0; i < s->video_layers->len; i++) {
        ns_video_layer_rect r =
            g_array_index(s->video_layers, ns_video_layer_rect, i);
        r.layer = ns_video_layer_ref(r.layer);
        r.x -= ox;
        r.y -= oy;
        g_array_append_val(out->video_layers, r);
    }
}

/* Reads NS_DAMAGE once: 0 disables partial repaint, "verify" checks it, default enables it. */
static int
session_damage_mode(void)
{
    static gint mode = -1;
    if (G_UNLIKELY(mode < 0)) {
        const char *v = g_getenv("NS_DAMAGE");
        mode = v && strcmp(v, "0") == 0 ? 0
             : v && strcmp(v, "verify") == 0 ? 2 : 1;
    }
    return mode;
}

/* Adds a CSS-pixel rect, scaled outward to device pixels and clipped, to a region. */
static void
region_add_css_rect(cairo_region_t *region, double x, double y, double w,
                    double h, double scale, int vw, int vh)
{
    double x0 = floor(x * scale), y0 = floor(y * scale);
    double x1 = ceil((x + w) * scale), y1 = ceil((y + h) * scale);
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, vw);
    y1 = MIN(y1, vh);
    if (!(x1 > x0) || !(y1 > y0)) return;
    cairo_rectangle_int_t r = { (int)x0, (int)y0, (int)(x1 - x0),
                                (int)(y1 - y0) };
    cairo_region_union_rectangle(region, &r);
}

/* Returns the start of the visible area inside an edge-padded framebuffer. */
static unsigned char *
session_view(unsigned char *padded, int vw)
{
    return padded + (size_t)NS_DAMAGE_EDGE_PX * (size_t)vw * 4u;
}

/* Shifts the visible framebuffer by (dx, dy) pixels in place for scrolling. */
static void
session_blit(ns_page_session *s, int dx, int dy, int vw, int vh)
{
    unsigned char *view = session_view(s->fb, vw);
    size_t stride = (size_t)vw * 4u;
    int w = vw - abs(dx);
    int src_x = dx > 0 ? dx : 0;
    int dst_x = dx > 0 ? 0 : -dx;
    for (int i = 0; i < vh - abs(dy); i++) {
        int y = dy > 0 ? i : vh - 1 - i;
        unsigned char *dst = view + (size_t)y * stride + (size_t)dst_x * 4u;
        const unsigned char *src = view + (size_t)(y + dy) * stride +
                                   (size_t)src_x * 4u;
        memmove(dst, src, (size_t)w * 4u);
    }
}

/* Reuses pixels on integer-pixel scrolls, adding exposed strips and fixed bands to damage. */
static gboolean
session_scroll_blit(ns_page_session *s, long sx, long sy, int vw, int vh,
                    double scale, cairo_region_t *region)
{
    double ddx = (double)(sx - s->frame_sx) * scale;
    double ddy = (double)(sy - s->frame_sy) * scale;
    if (fabs(ddx - round(ddx)) > 1e-6 || fabs(ddy - round(ddy)) > 1e-6)
        return FALSE;
    int dx = (int)lround(ddx), dy = (int)lround(ddy);
    if (abs(dx) >= vw || abs(dy) >= vh) return FALSE;
    GArray *bands = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
    if (!ns_browser_scroll_blit_bands(s->cur, bands)) {
        g_array_free(bands, TRUE);
        return FALSE;
    }
    session_blit(s, dx, dy, vw, vh);
    cairo_rectangle_int_t exposed[2] = {
        { dx > 0 ? vw - dx : 0, 0, abs(dx), vh },
        { 0, dy > 0 ? vh - dy : 0, vw, abs(dy) },
    };
    for (int i = 0; i < 2; i++)
        if (exposed[i].width > 0 && exposed[i].height > 0)
            cairo_region_union_rectangle(region, &exposed[i]);
    for (guint i = 0; i < bands->len; i++) {
        const ns_damage_rect *b = &g_array_index(bands, ns_damage_rect, i);
        region_add_css_rect(region, b->x, b->y, b->w, b->h, scale, vw, vh);
        region_add_css_rect(region, b->x - dx / scale, b->y - dy / scale,
                            b->w, b->h, scale, vw, vh);
    }
    g_array_free(bands, TRUE);
    return TRUE;
}

/* Builds the device-pixel repaint region; NULL means a full repaint is needed. */
static cairo_region_t *
session_damage_region(ns_page_session *s, GArray *damage, long sx, long sy,
                      int vw, int vh, double scale)
{
    cairo_region_t *region = cairo_region_create();
    if ((sx != s->frame_sx || sy != s->frame_sy) &&
        !session_scroll_blit(s, sx, sy, vw, vh, scale, region)) {
        cairo_region_destroy(region);
        return NULL;
    }
    for (guint i = 0; i < damage->len; i++) {
        const ns_damage_rect *d = &g_array_index(damage, ns_damage_rect, i);
        region_add_css_rect(region, d->x, d->y, d->w, d->h, scale, vw, vh);
    }
    if (cairo_region_num_rectangles(region) > NS_DAMAGE_MAX_RECTS) {
        cairo_rectangle_int_t box;
        cairo_region_get_extents(region, &box);
        cairo_region_destroy(region);
        region = cairo_region_create_rectangle(&box);
    }
    double area = 0;
    int n = cairo_region_num_rectangles(region);
    for (int i = 0; i < n; i++) {
        cairo_rectangle_int_t r;
        cairo_region_get_rectangle(region, i, &r);
        area += (double)r.width * r.height;
    }
    if (area > NS_DAMAGE_MAX_FRACTION * vw * vh) {
        cairo_region_destroy(region);
        return NULL;
    }
    return region;
}

/* Lazily allocates a padded framebuffer-sized scratch buffer. */
static unsigned char *
session_scratch(ns_page_session *s, unsigned char **buf)
{
    if (!*buf)
        *buf = malloc((size_t)s->max_w * 4u *
                      (size_t)(s->max_h + 2 * NS_DAMAGE_EDGE_PX));
    return *buf;
}

/* Renders a band, grown by the damage edge margin, into a scratch buffer. */
static int
session_paint_band(ns_page_session *s, unsigned char *scratch, int x0, int x1,
                   int y0, int y1, long sx, long sy, int vw, int vh,
                   double scale)
{
    x0 = MAX(x0 - NS_DAMAGE_EDGE_PX, 0);
    x1 = MIN(x1 + NS_DAMAGE_EDGE_PX, vw);
    y0 = MAX(y0 - NS_DAMAGE_EDGE_PX, -NS_DAMAGE_EDGE_PX);
    y1 = MIN(y1 + NS_DAMAGE_EDGE_PX, vh + NS_DAMAGE_EDGE_PX);
    int band[4] = { x0, y0, x1 - x0, y1 - y0 };
    return ns_browser_render_argb32_rects(s->cur, (int)sx, (int)sy, vw, vh,
                                          NS_DAMAGE_EDGE_PX, scale, scratch,
                                          vw * 4, band, 1);
}

/* NS_DAMAGE=verify: compares with a full repaint, logs differences and fixes the frame. */
static void
session_verify_region(ns_page_session *s, long sx, long sy, int vw, int vh,
                      double scale)
{
    size_t size = (size_t)vw * 4u * (size_t)vh;
    if (!session_scratch(s, &s->verify_fb) ||
        session_paint_band(s, s->verify_fb, 0, vw, 0, vh, sx, sy, vw, vh,
                           scale) != 0)
        return;
    const unsigned char *reference = session_view(s->verify_fb, vw);
    unsigned char *view = session_view(s->fb, vw);
    const guint32 *a = (const guint32 *)(const void *)view;
    const guint32 *b = (const guint32 *)(const void *)reference;
    int x0 = vw, y0 = vh, x1 = -1, y1 = -1, delta = 0;
    long diff = 0;
    for (int y = 0; y < vh; y++)
        for (int x = 0; x < vw; x++) {
            guint32 pa = a[(size_t)y * vw + x], pb = b[(size_t)y * vw + x];
            if (pa == pb) continue;
            diff++;
            x0 = MIN(x0, x);
            y0 = MIN(y0, y);
            x1 = MAX(x1, x);
            y1 = MAX(y1, y);
            for (int shift = 0; shift < 32; shift += 8)
                delta = MAX(delta, abs((int)((pa >> shift) & 0xff) -
                                       (int)((pb >> shift) & 0xff)));
        }
    if (diff > 0) {
        g_printerr("[damage] %ld pixels differ from a full repaint by up to "
                   "%d in %d,%d %dx%d (scroll %ld,%ld)\n", diff, delta, x0,
                   y0, x1 - x0 + 1, y1 - y0 + 1, sx, sy);
        memcpy(view, reference, size);
    }
}

static int
rect_row_cmp(const void *pa, const void *pb)
{
    const cairo_rectangle_int_t *a = pa, *b = pb;
    return a->y < b->y ? -1 : a->y > b->y;
}

/* Paints a row group once into scratch and copies just its damage rects into the frame. */
static int
session_paint_rows(ns_page_session *s, const cairo_rectangle_int_t *rects,
                   int n, int y0, int y1, long sx, long sy, int vw, int vh,
                   double scale)
{
    int x0 = vw, x1 = 0;
    for (int i = 0; i < n; i++) {
        x0 = MIN(x0, rects[i].x);
        x1 = MAX(x1, rects[i].x + rects[i].width);
    }
    if (session_paint_band(s, s->scratch_fb, x0, x1, y0, y1, sx, sy, vw, vh,
                           scale) != 0)
        return -1;
    size_t stride = (size_t)vw * 4u;
    const unsigned char *origin = session_view(s->scratch_fb, vw);
    unsigned char *view = session_view(s->fb, vw);
    for (int i = 0; i < n; i++) {
        size_t row = (size_t)rects[i].width * 4u;
        for (int y = rects[i].y; y < rects[i].y + rects[i].height; y++) {
            size_t off = (size_t)y * stride + (size_t)rects[i].x * 4u;
            memcpy(view + off, origin + off, row);
        }
    }
    return 0;
}

/* Repaints a damage region in row groups of nearby rects, verifying when enabled. */
static int
session_paint_region(ns_page_session *s, cairo_region_t *region, long sx,
                     long sy, int vw, int vh, double scale)
{
    int n = cairo_region_num_rectangles(region);
    if (n == 0) return 0;
    if (!session_scratch(s, &s->scratch_fb)) return -1;
    cairo_rectangle_int_t *rects = g_new(cairo_rectangle_int_t, n);
    for (int i = 0; i < n; i++)
        cairo_region_get_rectangle(region, i, &rects[i]);
    qsort(rects, (size_t)n, sizeof *rects, rect_row_cmp);
    int rc = 0;
    for (int first = 0; first < n && rc == 0;) {
        int y0 = rects[first].y;
        int y1 = rects[first].y + rects[first].height;
        int last = first + 1;
        while (last < n && rects[last].y <= y1 + 2 * NS_DAMAGE_EDGE_PX) {
            y1 = MAX(y1, rects[last].y + rects[last].height);
            last++;
        }
        rc = session_paint_rows(s, rects + first, last - first, y0, y1, sx,
                                sy, vw, vh, scale);
        first = last;
    }
    g_free(rects);
    if (rc == 0 && session_damage_mode() == 2)
        session_verify_region(s, sx, sy, vw, vh, scale);
    return rc;
}

/* Copies region rectangles into the frame as x, y, w, h integer quadruples. */
static void
session_frame_damage(ns_page_frame *out, cairo_region_t *region)
{
    int n = cairo_region_num_rectangles(region);
    if (n <= 0) return;
    out->damage = malloc(sizeof(int) * 4u * (size_t)n);
    if (!out->damage) return;
    for (int i = 0; i < n; i++) {
        cairo_rectangle_int_t r;
        cairo_region_get_rectangle(region, i, &r);
        out->damage[4 * i] = r.x;
        out->damage[4 * i + 1] = r.y;
        out->damage[4 * i + 2] = r.width;
        out->damage[4 * i + 3] = r.height;
    }
    out->n_damage = n;
}

/* Ticks the page, applies scroll requests and snapping, and paints a full or damaged frame. */
int
ns_page_session_render(ns_page_session *s, int width, int height,
                       int scroll_x, int scroll_y, double scale,
                       int caret_active, ns_page_frame *out)
{
    if (!s || !out)
        return -1;
    memset(out, 0, sizeof *out);
    out->scroll_x = -1;
    out->scroll_y = -1;
    if (!s->cur)
        return -1;
    if (!(scale > 0))
        scale = 1.0;
    int vw = clamp(width, 1, s->max_w);
    int vh = clamp(height, 1, s->max_h);
    int stride = vw * 4;
    long sx = scroll_x, sy = scroll_y;
    s->rendering = TRUE;
    gint64 frame_start = ns_trace_now();
    gint64 phase_start = frame_start;
    int ticked = s->frame_valid ? ns_browser_tick(s->cur, s->tick_budget_ms)
                                : 0;
    ns_trace_complete("frame", "tick", phase_start, NULL);
    int requested_scroll_x = -1;
    int requested_scroll_y = -1;
    ns_browser_take_pending_scroll(s->cur, &requested_scroll_x,
                                   &requested_scroll_y);
    int page_w = 0, page_h = 0;
    ns_browser_page_size(s->cur, &page_w, &page_h);
    if (requested_scroll_y >= 0) {
        int max_scroll_y = page_h - (int)ceil((double)vh / scale);
        if (max_scroll_y < 0) max_scroll_y = 0;
        if (requested_scroll_y > max_scroll_y)
            requested_scroll_y = max_scroll_y;
        sy = requested_scroll_y;
    }
    if (requested_scroll_x >= 0) {
        int max_scroll_x = page_w - (int)ceil((double)vw / scale);
        if (max_scroll_x < 0) max_scroll_x = 0;
        if (requested_scroll_x > max_scroll_x)
            requested_scroll_x = max_scroll_x;
        sx = requested_scroll_x;
    }
    int snap_x = (int)sx, snap_y = (int)sy;
    if (ns_browser_snap_document(s->cur, (double)vw / scale,
                                 (double)vh / scale, (int)s->frame_sx,
                                 (int)s->frame_sy, &snap_x, &snap_y)) {
        if (snap_x != (int)sx) {
            sx = snap_x;
            requested_scroll_x = snap_x;
        }
        if (snap_y != (int)sy) {
            sy = snap_y;
            requested_scroll_y = snap_y;
        }
    }
    int caret_changed = ns_browser_set_caret_blink_active(s->cur,
                                                          caret_active != 0);
    int unchanged = s->frame_valid && ticked == 0 && !caret_changed &&
                    sx == s->frame_sx && sy == s->frame_sy &&
                    vw == s->frame_w && vh == s->frame_h &&
                    scale == s->frame_scale;
    GArray *damage = g_array_new(FALSE, FALSE, sizeof(ns_damage_rect));
    int damage_full = ns_browser_take_damage(s->cur, (int)sx, (int)sy, damage);
    cairo_region_t *region = NULL;
    if (!unchanged && !damage_full && session_damage_mode() > 0 &&
        s->frame_valid && vw == s->frame_w && vh == s->frame_h &&
        scale == s->frame_scale)
        region = session_damage_region(s, damage, sx, sy, vw, vh, scale);
    g_array_free(damage, TRUE);
    gboolean scrolled = sx != s->frame_sx || sy != s->frame_sy;
    if (region && cairo_region_is_empty(region) && !scrolled) {
        cairo_region_destroy(region);
        region = NULL;
        unchanged = 1;
    }
    GArray *video_painted = s->video_layers_enabled && !unchanged
        ? video_layers_new() : NULL;
    if (video_painted)
        ns_paint_set_video_layers(video_painted);
    gboolean video_full = !region;
    if (!unchanged) {
        phase_start = ns_trace_now();
        int painted;
        if (region) {
            painted = session_paint_region(s, region, sx, sy, vw, vh, scale);
            if (!scrolled)
                session_frame_damage(out, region);
        } else {
            painted = session_paint_band(s, s->fb, 0, vw, 0, vh, sx, sy,
                                         vw, vh, scale);
        }
        ns_trace_complete("frame", region ? "paint (damage)" : "paint",
                          phase_start, NULL);
        if (region) cairo_region_destroy(region);
        if (painted == 0) {
            out->base_gen = s->frame_gen;
            s->frame_gen++;
            s->frame_valid = 1;
            s->frame_sx = sx;
            s->frame_sy = sy;
            s->frame_w = vw;
            s->frame_h = vh;
            s->frame_scale = scale;
        } else {
            memset(session_view(s->fb, vw), 0xff,
                   (size_t)stride * (size_t)vh);
            s->frame_valid = 0;
            out->n_damage = 0;
            g_clear_pointer(&out->damage, free);
        }
    }
    if (video_painted)
        ns_paint_set_video_layers(NULL);
    if (s->video_layers_enabled)
        session_merge_video_layers(s, video_painted, !unchanged && video_full,
                                   (double)sx * scale, (double)sy * scale,
                                   out);
    if (video_painted)
        g_array_free(video_painted, TRUE);
    out->gen = s->frame_gen;
    out->ok = 1;
    out->width = vw;
    out->height = vh;
    out->stride = stride;
    out->page_w = page_w;
    out->page_h = page_h;
    out->scroll_y = requested_scroll_y;
    out->scroll_x = requested_scroll_x;
    out->unchanged = unchanged;
    out->pixels = session_view(s->fb, vw);
    out->nav = empty_to_null(ns_browser_take_pending_nav(s->cur));
    scrub_line_breaks(out->nav);
    if (out->nav)
        session_stash_post(s, out->nav);
    out->url = empty_to_null(ns_browser_url(s->cur));
    out->url_pushed = ns_browser_take_url_pushed(s->cur);
    out->title = ns_browser_title(s->cur);
    out->camera = empty_to_null(ns_browser_take_pending_camera(s->cur));
    scrub_line_breaks(out->camera);
    out->download = empty_to_null(ns_browser_take_pending_download(s->cur));
    scrub_line_breaks(out->download);
    out->animating = ns_browser_continuous(s->cur) ? 1 : 0;
    out->caret_blinking = ns_browser_caret_blinking(s->cur) ? 1 : 0;
    out->clipboard = ns_browser_has_pending_clipboard(s->cur) ? 1 : 0;
    ns_trace_complete("frame", unchanged ? "frame (unchanged)" : "frame",
                      frame_start, NULL);
    s->rendering = FALSE;
    s->wake_posted = FALSE;
    return 0;
}

/* Returns the link URL at a point and optionally the CSS cursor; caller frees. */
char *
ns_page_session_link_at(ns_page_session *s, int x, int y, char **out_cursor)
{
    if (out_cursor)
        *out_cursor = NULL;
    if (!s || !s->cur)
        return NULL;
    if (out_cursor)
        *out_cursor = empty_to_null(ns_browser_cursor_at(s->cur, x, y));
    return empty_to_null(ns_browser_link_at(s->cur, x, y));
}

/* Updates hover state at a point; returns 1 if a repaint is needed, plus link and cursor. */
int
ns_page_session_hover(ns_page_session *s, int x, int y, char **out_href,
                      char **out_cursor)
{
    if (out_href)
        *out_href = NULL;
    if (out_cursor)
        *out_cursor = NULL;
    if (!s || !s->cur)
        return -1;
    int changed = ns_browser_hover(s->cur, x, y);
    if (changed > 0)
        s->frame_valid = 0;
    char *href = ns_page_session_link_at(s, x, y, out_cursor);
    if (out_href)
        *out_href = href;
    else
        free(href);
    return changed > 0 ? 1 : 0;
}

/* Sends a pointer press; returns any navigation href, stashing a form POST body. */
char *
ns_page_session_click(ns_page_session *s, int x, int y, int mods)
{
    if (!s || !s->cur)
        return NULL;
    s->frame_valid = 0;
    char *href = ns_browser_press(s->cur, x, y, mods);
    session_stash_post(s, href);
    return empty_to_null(href);
}

/* Sends a pointer release; returns any navigation href and whether the page changed. */
char *
ns_page_session_release(ns_page_session *s, int *out_changed)
{
    if (out_changed)
        *out_changed = 0;
    if (!s || !s->cur)
        return NULL;
    s->frame_valid = 0;
    int changed = 0;
    char *href = ns_browser_release_click(s->cur, &changed);
    session_stash_post(s, href);
    if (out_changed)
        *out_changed = changed > 0 ? 1 : 0;
    return empty_to_null(href);
}

/* Sends a pointer release at a page point, so a drag ends where it was dropped. */
char *
ns_page_session_release_at(ns_page_session *s, int x, int y, int *out_changed)
{
    if (out_changed)
        *out_changed = 0;
    if (!s || !s->cur)
        return NULL;
    s->frame_valid = 0;
    int changed = 0;
    char *href = ns_browser_release_at(s->cur, x, y, &changed);
    session_stash_post(s, href);
    if (out_changed)
        *out_changed = changed > 0 ? 1 : 0;
    return empty_to_null(href);
}

char *
ns_page_session_select(ns_page_session *s, int kind, int x, int y)
{
    if (!s || !s->cur)
        return NULL;
    s->frame_valid = 0;
    return empty_to_null(ns_browser_select(s->cur, kind, x, y));
}

/* Sends a key event; returns any navigation href and whether it was prevented. */
char *
ns_page_session_key(ns_page_session *s, int kind, const char *key,
                    const char *code, int keycode, int mods,
                    int *out_prevented)
{
    if (out_prevented)
        *out_prevented = 0;
    if (!s || !s->cur)
        return NULL;
    s->frame_valid = 0;
    int prevented = 0;
    char *href = ns_browser_key_full(s->cur, kind, key ? key : "",
                                     code ? code : "", keycode, mods,
                                     &prevented);
    session_stash_post(s, href);
    if (out_prevented)
        *out_prevented = prevented;
    return empty_to_null(href);
}

int
ns_page_session_contextmenu(ns_page_session *s, int x, int y)
{
    if (!s || !s->cur)
        return 0;
    return ns_browser_contextmenu(s->cur, x, y);
}

int
ns_page_session_scroll(ns_page_session *s, int x, int y, int dx, int dy)
{
    if (!s || !s->cur)
        return 0;
    int consumed = ns_browser_scroll_at(s->cur, x, y, dx, dy);
    if (consumed)
        s->frame_valid = 0;
    return consumed ? 1 : 0;
}

/* Routes scrollbar press (0), drag (1) or release (2); returns 1 if a scrollbar was hit. */
int
ns_page_session_scrollbar(ns_page_session *s, int kind, int x, int y)
{
    if (!s || !s->cur)
        return 0;
    if (kind == 2) {
        ns_browser_scrollbar_release(s->cur);
        return 1;
    }
    int hit = kind == 0 ? ns_browser_scrollbar_press(s->cur, x, y)
                        : ns_browser_scrollbar_drag(s->cur, x, y);
    if (hit)
        s->frame_valid = 0;
    return hit ? 1 : 0;
}

int
ns_page_session_drop_files(ns_page_session *s, int x, int y,
                           const char *const *paths, int n_paths)
{
    if (!s || !s->cur || !paths || n_paths <= 0)
        return 0;
    int changed = ns_browser_drop_files(s->cur, x, y, paths, n_paths);
    if (changed > 0)
        s->frame_valid = 0;
    return changed > 0 ? 1 : 0;
}

/* Runs find-in-page and reports match total, current index and scroll target. */
int
ns_page_session_find(ns_page_session *s, const char *query,
                     int case_sensitive, int direction, int from_y,
                     int *out_total, int *out_current, int *out_scroll_y)
{
    if (out_total) *out_total = 0;
    if (out_current) *out_current = 0;
    if (out_scroll_y) *out_scroll_y = 0;
    if (!s || !s->cur)
        return -1;
    s->frame_valid = 0;
    int total = 0, current = 0, scroll_y = 0;
    ns_browser_find(s->cur, query ? query : "", case_sensitive, direction,
                    from_y, &total, &current, &scroll_y);
    if (out_total) *out_total = total;
    if (out_current) *out_current = current;
    if (out_scroll_y) *out_scroll_y = scroll_y;
    return 0;
}

char *
ns_page_session_clipboard(ns_page_session *s)
{
    if (!s || !s->cur)
        return NULL;
    return ns_browser_take_pending_clipboard(s->cur);
}

/* Returns the media URL at a point and whether it is video or a stream; caller frees. */
char *
ns_page_session_media_at(ns_page_session *s, int x, int y, int *out_is_video,
                         int *out_stream)
{
    if (out_is_video) *out_is_video = 0;
    if (out_stream) *out_stream = 0;
    if (!s || !s->cur)
        return NULL;
    int is_video = 0, stream = 0;
    char *url = ns_browser_media_at(s->cur, x, y, &is_video, &stream);
    if (out_is_video) *out_is_video = is_video;
    if (out_stream) *out_stream = stream;
    return empty_to_null(url);
}

void
ns_page_session_resolve_camera(ns_page_session *s, const char *origin,
                               int allow)
{
    if (!s || !s->cur || !origin)
        return;
    ns_browser_resolve_camera(s->cur, origin, allow);
    s->frame_valid = 0;
}

char *
ns_page_session_eval(ns_page_session *s, const char *src)
{
    if (!s || !s->cur)
        return NULL;
    s->frame_valid = 0;
    return ns_browser_eval(s->cur, src ? src : "");
}

/* Returns a text dump by kind: network, dom, layout, text or performance; caller frees. */
char *
ns_page_session_dump(ns_page_session *s, const char *kind)
{
    if (!kind)
        return NULL;
    if (strcmp(kind, "network") == 0)
        return ns_net_log_dump();
    if (!s || !s->cur)
        return NULL;
    if (strcmp(kind, "dom") == 0)
        return ns_browser_dump_dom(s->cur);
    if (strcmp(kind, "layout") == 0)
        return ns_browser_dump_layout(s->cur);
    if (strcmp(kind, "text") == 0)
        return ns_browser_render_text(s->cur);
    if (strcmp(kind, "performance") == 0)
        return ns_browser_dump_performance(s->cur);
    return NULL;
}

char *
ns_page_session_console(ns_page_session *s)
{
    if (!s || !s->cur)
        return NULL;
    return ns_browser_console_drain(s->cur);
}

int
ns_page_session_export(ns_page_session *s, const char *path)
{
    if (!s || !s->cur || !path)
        return -1;
    s->frame_valid = 0;
    return ns_browser_render_image(s->cur, path);
}

GPtrArray *
ns_page_session_print(ns_page_session *s, ns_print_setup *out_setup)
{
    if (!s || !s->cur || !out_setup)
        return NULL;
    s->frame_valid = 0;
    return ns_browser_print_pages(s->cur, out_setup);
}
