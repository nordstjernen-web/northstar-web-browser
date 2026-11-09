/* Northstar Web Browser — W3C WebDriver server over a headless page session.
 * Loopback HTTP/1.1 transport, sessions, navigation, elements, scripts, input,
 * cookies, screenshots and print; the page-side half is data/js/webdriver.js.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "webdriver.h"

#include <cairo.h>
#include <cairo-pdf.h>
#include <gio/gio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef G_OS_WIN32
#include <glib-unix.h>
#include <signal.h>
#endif

#include "json.h"
#include "mainctx.h"
#include "net.h"
#include "page_session.h"
#include "print.h"
#include "version.h"
#include "webdriver_js.h"

#define WD_ELEMENT "element-6066-11e4-a52e-4f735466cecf"
#define WD_SHADOW  "shadow-6066-11e4-a52e-4f735466cecf"
#define WD_WINDOW  "window-fcc6-11e5-b4f8-330a88ab9d7f"
#define WD_FRAME   "frame-075b-4da1-b6ba-e579c2d3230a"

#define WD_MAX_HEADER (64 * 1024)
#define WD_MAX_BODY   (64 * 1024 * 1024)
#define WD_MAX_WIDTH  2560
#define WD_MAX_HEIGHT 1600
#define WD_SCREEN_W   1920
#define WD_SCREEN_H   1080
#define WD_FRAME_US   16000
#define WD_MAX_SAFE_INT 9007199254740991.0

/* ------------------------------------------------------------------ */
/* HTTP transport                                                      */
/* ------------------------------------------------------------------ */

/* One HTTP request handed from a connection thread to the main thread,
 * which owns the page engine; the connection thread waits for the reply. */
typedef struct wd_request {
    char    *method;
    char    *path;
    char    *body;
    gsize    body_len;
    int      status;
    char    *response;
    gboolean done;
    GMutex   lock;
    GCond    cond;
} wd_request;

static GAsyncQueue *g_wd_queue;
static gint g_wd_quit;
static gboolean g_wd_log;

static void
wd_request_free(wd_request *r)
{
    if (!r) return;
    g_free(r->method);
    g_free(r->path);
    g_free(r->body);
    g_free(r->response);
    g_mutex_clear(&r->lock);
    g_cond_clear(&r->cond);
    g_free(r);
}

/* Main thread: publishes the reply and wakes the waiting connection thread. */
static void
wd_request_complete(wd_request *r, int status, char *response)
{
    g_mutex_lock(&r->lock);
    r->status = status;
    r->response = response;
    r->done = TRUE;
    g_cond_signal(&r->cond);
    g_mutex_unlock(&r->lock);
}

static const char *
wd_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

/* Writes all of buf to the socket; FALSE on error. */
static gboolean
wd_send_all(GSocket *sock, const char *buf, gsize len)
{
    while (len > 0) {
        gssize n = g_socket_send(sock, buf, len, NULL, NULL);
        if (n <= 0) return FALSE;
        buf += n;
        len -= (gsize)n;
    }
    return TRUE;
}

static gboolean
wd_send_response(GSocket *sock, int status, const char *body,
                 gboolean keep_alive)
{
    gsize blen = body ? strlen(body) : 0;
    char *head = g_strdup_printf(
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: %s\r\n\r\n",
        status, wd_reason(status), blen, keep_alive ? "keep-alive" : "close");
    gboolean ok = wd_send_all(sock, head, strlen(head)) &&
                  (blen == 0 || wd_send_all(sock, body, blen));
    g_free(head);
    return ok;
}

/* A JSON error body {"value":{"error":...}} for transport-level failures. */
static char *
wd_error_body(const char *code, const char *message)
{
    GString *out = g_string_new("{\"value\":{\"error\":");
    ns_json_write_string(out, code);
    g_string_append(out, ",\"message\":");
    ns_json_write_string(out, message);
    g_string_append(out, ",\"stacktrace\":\"\"}}");
    return g_string_free(out, FALSE);
}

/* Buffered reader over a blocking socket. */
typedef struct {
    GSocket    *sock;
    GByteArray *buf;
} wd_reader;

/* Reads more bytes into the buffer; FALSE on EOF or error. */
static gboolean
wd_read_more(wd_reader *rd)
{
    char tmp[16384];
    gssize n = g_socket_receive(rd->sock, tmp, sizeof tmp, NULL, NULL);
    if (n <= 0) return FALSE;
    g_byte_array_append(rd->buf, (const guint8 *)tmp, (guint)n);
    return TRUE;
}

/* Offset just past the first CRLF CRLF in the buffer, or -1. */
static gssize
wd_find_header_end(GByteArray *buf)
{
    for (guint i = 0; i + 3 < buf->len; i++)
        if (buf->data[i] == '\r' && buf->data[i + 1] == '\n' &&
            buf->data[i + 2] == '\r' && buf->data[i + 3] == '\n')
            return (gssize)i + 4;
    return -1;
}

/* Ensures at least n bytes are buffered. */
static gboolean
wd_read_at_least(wd_reader *rd, gsize n)
{
    while (rd->buf->len < n)
        if (!wd_read_more(rd)) return FALSE;
    return TRUE;
}

/* Reads a line ending in CRLF from the buffer front; g_malloc'd, NULL on EOF. */
static char *
wd_read_line(wd_reader *rd)
{
    for (;;) {
        for (guint i = 0; i + 1 < rd->buf->len; i++) {
            if (rd->buf->data[i] == '\r' && rd->buf->data[i + 1] == '\n') {
                char *line = g_strndup((const char *)rd->buf->data, i);
                g_byte_array_remove_range(rd->buf, 0, i + 2);
                return line;
            }
        }
        if (rd->buf->len > WD_MAX_HEADER || !wd_read_more(rd)) return NULL;
    }
}

/* Decodes a chunked request body; NULL on malformed input or overflow. */
static GByteArray *
wd_read_chunked(wd_reader *rd)
{
    GByteArray *body = g_byte_array_new();
    for (;;) {
        char *line = wd_read_line(rd);
        if (!line) break;
        gchar *end = NULL;
        guint64 size = g_ascii_strtoull(line, &end, 16);
        gboolean bad = end == line;
        g_free(line);
        if (bad || size > WD_MAX_BODY || body->len + size > WD_MAX_BODY) break;
        if (size == 0) {
            for (;;) {
                char *trailer = wd_read_line(rd);
                if (!trailer) goto fail;
                gboolean last = !*trailer;
                g_free(trailer);
                if (last) return body;
            }
        }
        if (!wd_read_at_least(rd, (gsize)size + 2)) break;
        g_byte_array_append(body, rd->buf->data, (guint)size);
        g_byte_array_remove_range(rd->buf, 0, (guint)size + 2);
    }
fail:
    g_byte_array_free(body, TRUE);
    return NULL;
}

/* TRUE when a Host header names this machine's loopback interface, so a
 * page elsewhere cannot reach the server through DNS rebinding. */
static gboolean
wd_host_allowed(const char *host)
{
    if (!host) return TRUE;
    g_autofree char *h = g_ascii_strdown(host, -1);
    char *name = g_strstrip(h);
    if (name[0] == '[') {
        char *close = strchr(name, ']');
        if (!close) return FALSE;
        *close = '\0';
        name++;
    } else {
        char *colon = strrchr(name, ':');
        if (colon && strchr(name, ':') == colon) *colon = '\0';
    }
    return strcmp(name, "localhost") == 0 || strcmp(name, "127.0.0.1") == 0 ||
           strcmp(name, "::1") == 0;
}

/* Rejects the content types a web page may send cross-origin without a
 * CORS preflight, so no page can drive the browser through a form post. */
static gboolean
wd_content_type_allowed(const char *ct)
{
    if (!ct) return TRUE;
    g_autofree char *c = g_ascii_strdown(ct, -1);
    char *semi = strchr(c, ';');
    if (semi) *semi = '\0';
    g_strstrip(c);
    return strcmp(c, "application/x-www-form-urlencoded") != 0 &&
           strcmp(c, "multipart/form-data") != 0 &&
           strcmp(c, "text/plain") != 0;
}

/* Serves requests on one connection until it closes. */
static gpointer
wd_connection_thread(gpointer data)
{
    GSocketConnection *conn = data;
    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_blocking(sock, TRUE);
    g_socket_set_timeout(sock, 0);
    wd_reader rd = { sock, g_byte_array_new() };
    gboolean keep_alive = TRUE;
    while (keep_alive && !g_atomic_int_get(&g_wd_quit)) {
        gssize hend;
        while ((hend = wd_find_header_end(rd.buf)) < 0) {
            if (rd.buf->len > WD_MAX_HEADER || !wd_read_more(&rd)) goto done;
        }
        char *head = g_strndup((const char *)rd.buf->data, (gsize)hend - 4);
        g_byte_array_remove_range(rd.buf, 0, (guint)hend);
        char **lines = g_strsplit(head, "\r\n", -1);
        g_free(head);
        char **req = lines[0] ? g_strsplit(lines[0], " ", 3) : NULL;
        if (!req || g_strv_length(req) != 3) {
            g_strfreev(req);
            g_strfreev(lines);
            char *b = wd_error_body("unknown error", "Malformed HTTP request line");
            wd_send_response(sock, 400, b, FALSE);
            g_free(b);
            goto done;
        }
        gboolean http10 = strcmp(req[2], "HTTP/1.0") == 0;
        keep_alive = !http10;
        const char *host = NULL, *origin = NULL, *ctype = NULL, *te = NULL,
                   *expect = NULL;
        gint64 clen = -1;
        for (int i = 1; lines[i]; i++) {
            char *colon = strchr(lines[i], ':');
            if (!colon) continue;
            *colon = '\0';
            char *name = g_strstrip(lines[i]);
            char *value = g_strstrip(colon + 1);
            if (g_ascii_strcasecmp(name, "Host") == 0) host = value;
            else if (g_ascii_strcasecmp(name, "Origin") == 0) origin = value;
            else if (g_ascii_strcasecmp(name, "Content-Type") == 0) ctype = value;
            else if (g_ascii_strcasecmp(name, "Transfer-Encoding") == 0) te = value;
            else if (g_ascii_strcasecmp(name, "Expect") == 0) expect = value;
            else if (g_ascii_strcasecmp(name, "Content-Length") == 0)
                clen = g_ascii_strtoll(value, NULL, 10);
            else if (g_ascii_strcasecmp(name, "Connection") == 0) {
                if (g_ascii_strcasecmp(value, "close") == 0) keep_alive = FALSE;
                else if (g_ascii_strcasecmp(value, "keep-alive") == 0)
                    keep_alive = TRUE;
            }
        }
        if (expect && g_ascii_strcasecmp(expect, "100-continue") == 0)
            wd_send_all(sock, "HTTP/1.1 100 Continue\r\n\r\n", 25);
        GByteArray *body = NULL;
        int early_status = 0;
        const char *early_msg = NULL;
        if (te && g_ascii_strcasecmp(te, "chunked") == 0) {
            body = wd_read_chunked(&rd);
            if (!body) {
                early_status = 400;
                early_msg = "Malformed chunked request body";
                keep_alive = FALSE;
            }
        } else if (clen > WD_MAX_BODY) {
            early_status = 413;
            early_msg = "Request body is too large";
            keep_alive = FALSE;
        } else if (clen > 0) {
            if (!wd_read_at_least(&rd, (gsize)clen)) {
                g_strfreev(req);
                g_strfreev(lines);
                goto done;
            }
            body = g_byte_array_new();
            g_byte_array_append(body, rd.buf->data, (guint)clen);
            g_byte_array_remove_range(rd.buf, 0, (guint)clen);
        }
        if (!early_status && !wd_host_allowed(host)) {
            early_status = 403;
            early_msg = "The Host header does not name a loopback address";
        } else if (!early_status && origin) {
            early_status = 403;
            early_msg = "Requests from web pages (with an Origin header) are refused";
        } else if (!early_status && strcmp(req[0], "POST") == 0 &&
                   !wd_content_type_allowed(ctype)) {
            early_status = 400;
            early_msg = "POST bodies must be application/json";
        }
        if (early_status) {
            char *b = wd_error_body(early_status == 400 || early_status == 413
                                    ? "invalid argument" : "unknown error",
                                    early_msg);
            gboolean ok = wd_send_response(sock, early_status, b, keep_alive);
            g_free(b);
            if (body) g_byte_array_free(body, TRUE);
            g_strfreev(req);
            g_strfreev(lines);
            if (!ok) goto done;
            continue;
        }
        wd_request *r = g_new0(wd_request, 1);
        g_mutex_init(&r->lock);
        g_cond_init(&r->cond);
        r->method = g_strdup(req[0]);
        char *q = strchr(req[1], '?');
        r->path = q ? g_strndup(req[1], (gsize)(q - req[1])) : g_strdup(req[1]);
        if (body) {
            r->body_len = body->len;
            g_byte_array_append(body, (const guint8 *)"", 1);
            r->body = (char *)g_byte_array_free(body, FALSE);
        }
        g_strfreev(req);
        g_strfreev(lines);
        g_async_queue_push(g_wd_queue, r);
        g_main_context_wakeup(ns_engine_context());
        g_mutex_lock(&r->lock);
        while (!r->done)
            g_cond_wait(&r->cond, &r->lock);
        g_mutex_unlock(&r->lock);
        gboolean ok = wd_send_response(sock, r->status, r->response,
                                       keep_alive);
        wd_request_free(r);
        if (!ok) break;
    }
done:
    g_byte_array_free(rd.buf, TRUE);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
    return NULL;
}

/* Accepts connections, one thread each, until the server stops. */
static gpointer
wd_accept_thread(gpointer data)
{
    GSocketListener *listener = data;
    while (!g_atomic_int_get(&g_wd_quit)) {
        GError *err = NULL;
        GSocketConnection *conn =
            g_socket_listener_accept(listener, NULL, NULL, &err);
        if (!conn) {
            if (g_atomic_int_get(&g_wd_quit)) {
                g_clear_error(&err);
                break;
            }
            g_clear_error(&err);
            g_usleep(10000);
            continue;
        }
        GThread *t = g_thread_try_new("wd-conn", wd_connection_thread, conn,
                                      NULL);
        if (t)
            g_thread_unref(t);
        else
            g_object_unref(conn);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    WD_SRC_NONE,
    WD_SRC_KEY,
    WD_SRC_POINTER,
    WD_SRC_WHEEL,
} wd_source_type;

/* An input source of the actions API: its pressed keys or buttons and,
 * for a pointer, its position in viewport coordinates. */
typedef struct wd_source {
    char           *id;
    wd_source_type  type;
    char           *pointer_type;
    GPtrArray      *pressed;
    guint           buttons;
    double          x;
    double          y;
    gboolean        moved;
    gint64          last_click_ms;
    int             clicks;
    double          click_x;
    double          click_y;
} wd_source;

/* A top-level browsing context: one page session with its own history. */
typedef struct wd_window {
    char            *handle;
    ns_page_session *page;
    GPtrArray       *history;
    int              hist_index;
    int              scroll_x;
    int              scroll_y;
    int              page_w;
    int              page_h;
    GPtrArray       *frame_path;
    gboolean         navigated;
} wd_window;

typedef struct wd_session {
    char       *id;
    GPtrArray  *windows;
    wd_window  *current;
    int         vw;
    int         vh;
    int         win_x;
    int         win_y;
    int         settle_ms;
    gint64      t_script;
    gint64      t_page_load;
    gint64      t_implicit;
    char       *load_strategy;
    char       *prompt_behavior;
    gboolean    accept_insecure;
    gboolean    prev_ignore_certs;
    gboolean    strict_file;
    ns_json    *proxy;
    GHashTable *issued;
    GPtrArray  *sources;
    gint64      next_frame_us;
} wd_session;

/* One command being handled: its path, body and result or error. */
typedef struct wd_call {
    const char *method;
    char      **seg;
    int         nseg;
    ns_json    *body;
    ns_json    *value;
    char       *error;
    char       *message;
    char       *stacktrace;
} wd_call;

static wd_session *g_session;
static ns_webdriver_opts g_opts;

static void
wd_fail(wd_call *c, const char *code, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

static void
wd_fail(wd_call *c, const char *code, const char *fmt, ...)
{
    if (c->error) return;
    va_list ap;
    va_start(ap, fmt);
    c->error = g_strdup(code);
    c->message = g_strdup_vprintf(fmt, ap);
    va_end(ap);
}

static void
wd_set_value(wd_call *c, ns_json *v)
{
    ns_json_free(c->value);
    c->value = v;
}

static gboolean wd_failed(const wd_call *c) { return c->error != NULL; }

/* A random identifier in the UUID v4 format. */
static char *
wd_uuid(void)
{
    return g_uuid_string_random();
}

static gint64
wd_now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static void
wd_source_free(wd_source *src)
{
    if (!src) return;
    g_free(src->id);
    g_free(src->pointer_type);
    if (src->pressed) g_ptr_array_free(src->pressed, TRUE);
    g_free(src);
}

static void
wd_window_free(wd_window *w)
{
    if (!w) return;
    g_free(w->handle);
    if (w->page) ns_page_session_free(w->page);
    if (w->history) g_ptr_array_free(w->history, TRUE);
    if (w->frame_path) g_ptr_array_free(w->frame_path, TRUE);
    g_free(w);
}

static void
wd_session_free(wd_session *s)
{
    if (!s) return;
    g_free(s->id);
    if (s->windows) g_ptr_array_free(s->windows, TRUE);
    g_free(s->load_strategy);
    g_free(s->prompt_behavior);
    ns_json_free(s->proxy);
    if (s->issued) g_hash_table_destroy(s->issued);
    if (s->sources) g_ptr_array_free(s->sources, TRUE);
    g_free(s);
}

static void
wd_history_push(wd_window *w, const char *url)
{
    if (!url || !*url) return;
    if (w->hist_index >= 0 && w->hist_index < (int)w->history->len &&
        strcmp(g_ptr_array_index(w->history, w->hist_index), url) == 0)
        return;
    while ((int)w->history->len > w->hist_index + 1)
        g_ptr_array_remove_index(w->history, w->history->len - 1);
    g_ptr_array_add(w->history, g_strdup(url));
    w->hist_index = (int)w->history->len - 1;
}

/* Resets per-document state after a window's page was replaced. */
static void
wd_window_new_document(wd_window *w)
{
    g_ptr_array_set_size(w->frame_path, 0);
    w->scroll_x = 0;
    w->scroll_y = 0;
    w->navigated = TRUE;
}

/* Opens url in a window, following up to eight navigations the page asks
 * for while it loads; history_load restores from the back/forward cache. */
static gboolean
wd_window_open(wd_session *s, wd_window *w, const char *url,
               gboolean history_load, gboolean user_activated, gboolean push)
{
    char *href = g_strdup(url);
    gboolean ok = TRUE;
    for (int hops = 0; href && hops < 8; hops++) {
        ns_page_info pg;
        int rc = ns_page_session_open(w->page, href, s->vw, s->vh,
                                      s->settle_ms, history_load ? 1 : 0,
                                      user_activated ? 1 : 0, &pg);
        if (rc != 0) ok = FALSE;
        wd_window_new_document(w);
        if (push || hops > 0)
            wd_history_push(w, pg.url ? pg.url : href);
        if (g_wd_log)
            g_printerr("[webdriver] loaded %s\n", pg.url ? pg.url : href);
        g_free(href);
        href = pg.nav ? g_strdup(pg.nav) : NULL;
        ns_page_info_clear(&pg);
        history_load = FALSE;
        user_activated = FALSE;
    }
    g_free(href);
    return ok;
}

/* Renders one frame of a window: ticks the page, applies the scroll it
 * asked for, and follows a navigation it started.  The frame is returned
 * in out when given (caller clears it); TRUE when the page navigated. */
static gboolean
wd_window_render(wd_session *s, wd_window *w, ns_page_frame *out)
{
    ns_page_frame fr;
    if (ns_page_session_render(w->page, s->vw, s->vh, w->scroll_x,
                               w->scroll_y, 1.0, 0, &fr) != 0) {
        if (out) memset(out, 0, sizeof *out);
        return FALSE;
    }
    if (fr.scroll_x >= 0) w->scroll_x = fr.scroll_x;
    if (fr.scroll_y >= 0) w->scroll_y = fr.scroll_y;
    w->page_w = fr.page_w;
    w->page_h = fr.page_h;
    char *nav = fr.nav ? g_strdup(fr.nav) : NULL;
    if (out && !nav) {
        *out = fr;
        return FALSE;
    }
    ns_page_frame_clear(&fr);
    if (out) memset(out, 0, sizeof *out);
    if (!nav) return FALSE;
    wd_window_open(s, w, nav, FALSE, FALSE, TRUE);
    g_free(nav);
    return TRUE;
}

/* Runs the engine's pending events and, at most once a frame, renders
 * every window, the way the GUI's engine thread and frame clock do. */
static void
wd_pump_step(wd_session *s)
{
    GMainContext *ctx = ns_engine_context();
    for (int i = 0; i < 64 && g_main_context_iteration(ctx, FALSE); i++)
        ;
    if (!s) return;
    gint64 now = g_get_monotonic_time();
    if (now < s->next_frame_us) return;
    s->next_frame_us = now + WD_FRAME_US;
    for (guint i = 0; i < s->windows->len; i++)
        wd_window_render(s, g_ptr_array_index(s->windows, i), NULL);
}

/* Lets the page run for ms milliseconds. */
static void
wd_pump(wd_session *s, int ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    do {
        wd_pump_step(s);
        gint64 left = end - g_get_monotonic_time();
        if (left > 0) g_usleep((gulong)MIN(left, 4000));
    } while (g_get_monotonic_time() < end);
}

/* Records element and shadow root references found in an atoms result. */
static void
wd_collect_issued(wd_session *s, const ns_json *v)
{
    if (!v) return;
    if (v->type == NS_JSON_ARRAY) {
        for (guint i = 0; i < v->items->len; i++)
            wd_collect_issued(s, g_ptr_array_index(v->items, i));
    } else if (v->type == NS_JSON_OBJECT) {
        const char *e = ns_json_get_string(v, WD_ELEMENT);
        const char *sh = ns_json_get_string(v, WD_SHADOW);
        if (e) g_hash_table_add(s->issued, g_strdup(e));
        if (sh) g_hash_table_add(s->issued, g_strdup(sh));
        for (guint i = 0; i < v->items->len; i++)
            wd_collect_issued(s, g_ptr_array_index(v->items, i));
    }
}

/* Runs an atoms command in a window and returns its raw reply object
 * ({"value"}, {"pending"} or {"error"}), or NULL with c failed when the
 * page could not run it.  top ignores the current frame.  Takes req. */
static ns_json *
wd_atom_raw(wd_session *s, wd_window *w, ns_json *req, gboolean top,
            wd_call *c)
{
    (void)s;
    ns_json *ctx = ns_json_array();
    if (!top)
        for (guint i = 0; i < w->frame_path->len; i++)
            ns_json_array_add(ctx,
                              ns_json_string(g_ptr_array_index(w->frame_path, i)));
    ns_json_object_set(req, "ctx", ctx);
    ns_json_object_set(req, "handle", ns_json_string(w->handle));
    char *json = ns_json_to_string(req);
    ns_json_free(req);
    char *src = g_strdup_printf("(function(wd){return wd.run(%s);})", json);
    g_free(json);
    char *res = ns_page_session_webdriver(w->page, ns_webdriver_atoms_src, src);
    g_free(src);
    if (!res) {
        wd_fail(c, "unknown error",
                "The page has no script context (JavaScript is off or halted)");
        return NULL;
    }
    if (g_str_has_prefix(res, "error: ")) {
        wd_fail(c, "javascript error", "%s", res + 7);
        g_free(res);
        return NULL;
    }
    char *perr = NULL;
    ns_json *reply = ns_json_parse(res, strlen(res), &perr);
    g_free(res);
    if (!reply || reply->type != NS_JSON_OBJECT) {
        wd_fail(c, "unknown error", "Malformed reply from the page: %s",
                perr ? perr : "not an object");
        g_free(perr);
        ns_json_free(reply);
        return NULL;
    }
    return reply;
}

/* Turns an atoms error reply into the call's error; a reference the page
 * does not know but this session handed out is a stale reference. */
static void
wd_atom_error(wd_session *s, const ns_json *reply, wd_call *c)
{
    const char *code = ns_json_get_string(reply, "error");
    const char *msg = ns_json_get_string(reply, "message");
    const char *unknown = ns_json_get_string(reply, "unknownRef");
    if (unknown && g_hash_table_contains(s->issued, unknown)) {
        if (g_strcmp0(code, "no such shadow root") == 0) {
            code = "detached shadow root";
            msg = "The shadow root belongs to a document that is gone";
        } else {
            code = "stale element reference";
            msg = "The element belongs to a document that is gone";
        }
    }
    wd_fail(c, code ? code : "unknown error", "%s", msg ? msg : "");
    const char *st = ns_json_get_string(reply, "stacktrace");
    if (st && !c->stacktrace) c->stacktrace = g_strdup(st);
}

/* Runs an atoms command and returns its value (owned), or NULL with c
 * failed.  Takes req. */
static ns_json *
wd_atom(wd_session *s, wd_window *w, ns_json *req, gboolean top, wd_call *c)
{
    ns_json *reply = wd_atom_raw(s, w, req, top, c);
    if (!reply) return NULL;
    if (ns_json_object_has(reply, "error")) {
        wd_atom_error(s, reply, c);
        ns_json_free(reply);
        return NULL;
    }
    ns_json *v = ns_json_object_get(reply, "value");
    ns_json *out = v ? ns_json_copy(v) : ns_json_null();
    ns_json_free(reply);
    wd_collect_issued(s, out);
    return out;
}

/* A request object {"cmd": cmd} for the atoms. */
static ns_json *
wd_req(const char *cmd)
{
    ns_json *r = ns_json_object();
    ns_json_object_set(r, "cmd", ns_json_string(cmd));
    return r;
}

static ns_json *
wd_req_id(const char *cmd, const char *id)
{
    ns_json *r = wd_req(cmd);
    ns_json_object_set(r, "id", ns_json_string(id));
    return r;
}

/* Waits for the current document per the page load strategy, failing with
 * "timeout" once the page load timeout from start_ms has passed. */
static gboolean
wd_wait_for_load(wd_session *s, wd_window *w, gint64 start_ms, wd_call *c)
{
    if (g_strcmp0(s->load_strategy, "none") == 0) return TRUE;
    gboolean eager = g_strcmp0(s->load_strategy, "eager") == 0;
    for (;;) {
        wd_call probe = {0};
        ns_json *info = wd_atom(s, w, wd_req("info"), TRUE, &probe);
        const char *state = info ? ns_json_get_string(info, "readyState") : NULL;
        gboolean ready = !info || !state || strcmp(state, "complete") == 0 ||
                         (eager && strcmp(state, "interactive") == 0);
        ns_json_free(info);
        g_free(probe.error);
        g_free(probe.message);
        g_free(probe.stacktrace);
        if (ready) return TRUE;
        if (wd_now_ms() - start_ms > s->t_page_load) {
            wd_fail(c, "timeout", "The page did not finish loading within %"
                    G_GINT64_FORMAT " ms", s->t_page_load);
            return FALSE;
        }
        wd_pump(s, 10);
    }
}

/* Follows a navigation an interaction caused and waits for it to load. */
static void
wd_settle_after_input(wd_session *s, wd_window *w, char *href,
                      gint64 start_ms, wd_call *c)
{
    w->navigated = FALSE;
    if (href && *href)
        wd_window_open(s, w, href, FALSE, TRUE, TRUE);
    g_free(href);
    wd_window_render(s, w, NULL);
    if (w->navigated)
        wd_wait_for_load(s, w, start_ms, c);
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    gunichar    cp;
    const char *key;
    const char *code;
    int         keycode;
} wd_special_key;

static const wd_special_key wd_special_keys[] = {
    { 0xE000, "Unidentified", "", 0 },
    { 0xE001, "Cancel", "", 3 },
    { 0xE002, "Help", "Help", 6 },
    { 0xE003, "Backspace", "Backspace", 8 },
    { 0xE004, "Tab", "Tab", 9 },
    { 0xE005, "Clear", "NumpadClear", 12 },
    { 0xE006, "Enter", "Enter", 13 },
    { 0xE007, "Enter", "NumpadEnter", 13 },
    { 0xE008, "Shift", "ShiftLeft", 16 },
    { 0xE009, "Control", "ControlLeft", 17 },
    { 0xE00A, "Alt", "AltLeft", 18 },
    { 0xE00B, "Pause", "Pause", 19 },
    { 0xE00C, "Escape", "Escape", 27 },
    { 0xE00D, " ", "Space", 32 },
    { 0xE00E, "PageUp", "PageUp", 33 },
    { 0xE00F, "PageDown", "PageDown", 34 },
    { 0xE010, "End", "End", 35 },
    { 0xE011, "Home", "Home", 36 },
    { 0xE012, "ArrowLeft", "ArrowLeft", 37 },
    { 0xE013, "ArrowUp", "ArrowUp", 38 },
    { 0xE014, "ArrowRight", "ArrowRight", 39 },
    { 0xE015, "ArrowDown", "ArrowDown", 40 },
    { 0xE016, "Insert", "Insert", 45 },
    { 0xE017, "Delete", "Delete", 46 },
    { 0xE018, ";", "Semicolon", 186 },
    { 0xE019, "=", "Equal", 187 },
    { 0xE01A, "0", "Numpad0", 96 },
    { 0xE01B, "1", "Numpad1", 97 },
    { 0xE01C, "2", "Numpad2", 98 },
    { 0xE01D, "3", "Numpad3", 99 },
    { 0xE01E, "4", "Numpad4", 100 },
    { 0xE01F, "5", "Numpad5", 101 },
    { 0xE020, "6", "Numpad6", 102 },
    { 0xE021, "7", "Numpad7", 103 },
    { 0xE022, "8", "Numpad8", 104 },
    { 0xE023, "9", "Numpad9", 105 },
    { 0xE024, "*", "NumpadMultiply", 106 },
    { 0xE025, "+", "NumpadAdd", 107 },
    { 0xE026, ",", "NumpadComma", 108 },
    { 0xE027, "-", "NumpadSubtract", 109 },
    { 0xE028, ".", "NumpadDecimal", 110 },
    { 0xE029, "/", "NumpadDivide", 111 },
    { 0xE031, "F1", "F1", 112 },
    { 0xE032, "F2", "F2", 113 },
    { 0xE033, "F3", "F3", 114 },
    { 0xE034, "F4", "F4", 115 },
    { 0xE035, "F5", "F5", 116 },
    { 0xE036, "F6", "F6", 117 },
    { 0xE037, "F7", "F7", 118 },
    { 0xE038, "F8", "F8", 119 },
    { 0xE039, "F9", "F9", 120 },
    { 0xE03A, "F10", "F10", 121 },
    { 0xE03B, "F11", "F11", 122 },
    { 0xE03C, "F12", "F12", 123 },
    { 0xE03D, "Meta", "MetaLeft", 91 },
    { 0xE040, "ZenkakuHankaku", "Lang1", 0 },
    { 0xE050, "Shift", "ShiftRight", 16 },
    { 0xE051, "Control", "ControlRight", 17 },
    { 0xE052, "Alt", "AltRight", 18 },
    { 0xE053, "Meta", "MetaRight", 92 },
    { 0xE054, "PageUp", "Numpad9", 33 },
    { 0xE055, "PageDown", "Numpad3", 34 },
    { 0xE056, "End", "Numpad1", 35 },
    { 0xE057, "Home", "Numpad7", 36 },
    { 0xE058, "ArrowLeft", "Numpad4", 37 },
    { 0xE059, "ArrowUp", "Numpad8", 38 },
    { 0xE05A, "ArrowRight", "Numpad6", 39 },
    { 0xE05B, "ArrowDown", "Numpad2", 40 },
    { 0xE05C, "Insert", "Numpad0", 45 },
    { 0xE05D, "Delete", "NumpadDecimal", 46 },
};

/* US-layout punctuation: unshifted and shifted characters with their code. */
static const struct {
    char        plain;
    char        shifted;
    const char *code;
    int         keycode;
} wd_punct_keys[] = {
    { '`', '~', "Backquote", 192 },   { '1', '!', "Digit1", 49 },
    { '2', '@', "Digit2", 50 },       { '3', '#', "Digit3", 51 },
    { '4', '$', "Digit4", 52 },       { '5', '%', "Digit5", 53 },
    { '6', '^', "Digit6", 54 },       { '7', '&', "Digit7", 55 },
    { '8', '*', "Digit8", 56 },       { '9', '(', "Digit9", 57 },
    { '0', ')', "Digit0", 48 },       { '-', '_', "Minus", 189 },
    { '=', '+', "Equal", 187 },       { '[', '{', "BracketLeft", 219 },
    { ']', '}', "BracketRight", 221 }, { '\\', '|', "Backslash", 220 },
    { ';', ':', "Semicolon", 186 },   { '\'', '"', "Quote", 222 },
    { ',', '<', "Comma", 188 },       { '.', '>', "Period", 190 },
    { '/', '?', "Slash", 191 },
};

/* A key as the page sees it: DOM key and code values, legacy keyCode,
 * whether it types text and whether it needs Shift on a US layout. */
typedef struct {
    char        key[32];
    const char *code;
    int         keycode;
    gboolean    printable;
    gboolean    shifted;
    gboolean    modifier;
    int         modifier_bit;
} wd_key;

/* Looks up the key for one WebDriver key value code point; shift_down
 * gives the shifted character of a letter or US punctuation key. */
static void
wd_key_lookup(gunichar ch, gboolean shift_down, wd_key *k)
{
    memset(k, 0, sizeof *k);
    k->code = "";
    if (ch == '\n' || ch == '\r') ch = 0xE006;
    else if (ch == '\t') ch = 0xE004;
    for (gsize i = 0; i < G_N_ELEMENTS(wd_special_keys); i++) {
        if (wd_special_keys[i].cp != ch) continue;
        g_strlcpy(k->key, wd_special_keys[i].key, sizeof k->key);
        k->code = wd_special_keys[i].code;
        k->keycode = wd_special_keys[i].keycode;
        k->printable = g_utf8_strlen(k->key, -1) == 1;
        if (strcmp(k->key, "Shift") == 0) k->modifier_bit = 1;
        else if (strcmp(k->key, "Control") == 0) k->modifier_bit = 2;
        else if (strcmp(k->key, "Alt") == 0) k->modifier_bit = 4;
        else if (strcmp(k->key, "Meta") == 0) k->modifier_bit = 8;
        k->modifier = k->modifier_bit != 0;
        return;
    }
    k->printable = TRUE;
    if (ch >= 'a' && ch <= 'z') {
        if (shift_down) ch = g_unichar_toupper(ch);
    } else if (ch < 0x80 && shift_down) {
        for (gsize i = 0; i < G_N_ELEMENTS(wd_punct_keys); i++)
            if (wd_punct_keys[i].plain == (char)ch) {
                ch = (gunichar)(unsigned char)wd_punct_keys[i].shifted;
                break;
            }
    }
    int n = g_unichar_to_utf8(ch, k->key);
    k->key[n] = '\0';
    if (ch >= 'a' && ch <= 'z') {
        k->keycode = 65 + (int)(ch - 'a');
    } else if (ch >= 'A' && ch <= 'Z') {
        k->keycode = 65 + (int)(ch - 'A');
        k->shifted = TRUE;
    } else if (ch == ' ') {
        k->code = "Space";
        k->keycode = 32;
    } else if (ch < 0x80) {
        for (gsize i = 0; i < G_N_ELEMENTS(wd_punct_keys); i++) {
            if (wd_punct_keys[i].plain == (char)ch ||
                wd_punct_keys[i].shifted == (char)ch) {
                k->code = wd_punct_keys[i].code;
                k->keycode = wd_punct_keys[i].keycode;
                k->shifted = wd_punct_keys[i].shifted == (char)ch;
                break;
            }
        }
    }
    if (k->keycode >= 65 && k->keycode <= 90) {
        static const char *const letters[] = {
            "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH",
            "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN", "KeyO", "KeyP",
            "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX",
            "KeyY", "KeyZ",
        };
        k->code = letters[k->keycode - 65];
    }
}

/* The modifier bits (1 shift, 2 control, 4 alt, 8 meta) held by every key
 * source of the session. */
static int
wd_session_mods(wd_session *s)
{
    int mods = 0;
    for (guint i = 0; i < s->sources->len; i++) {
        wd_source *src = g_ptr_array_index(s->sources, i);
        if (src->type != WD_SRC_KEY) continue;
        for (guint j = 0; j < src->pressed->len; j++) {
            const char *k = g_ptr_array_index(src->pressed, j);
            if (strcmp(k, "Shift") == 0) mods |= 1;
            else if (strcmp(k, "Control") == 0) mods |= 2;
            else if (strcmp(k, "Alt") == 0) mods |= 4;
            else if (strcmp(k, "Meta") == 0) mods |= 8;
        }
    }
    return mods;
}

/* Sends a key down to the page: a printable key types its character
 * unless Control, Alt or Meta is held or the page cancels the keydown.
 * Returns a navigation the key caused (caller frees). */
static char *
wd_send_key_down(wd_window *w, const wd_key *k, int mods)
{
    int prevented = 0;
    if (k->printable && !(mods & (2 | 4 | 8))) {
        char *h = ns_page_session_key(w->page, 3, k->key, k->code, k->keycode,
                                      mods, &prevented);
        if (!prevented) {
            char *h2 = ns_page_session_key(w->page, 2, k->key, "", 0, 0, NULL);
            if (!h) h = h2;
            else g_free(h2);
        }
        return h;
    }
    return ns_page_session_key(w->page, 0, k->key, k->code, k->keycode, mods,
                               &prevented);
}

static char *
wd_send_key_up(wd_window *w, const wd_key *k, int mods)
{
    return ns_page_session_key(w->page, 1, k->key, k->code, k->keycode, mods,
                               NULL);
}

/* Types a WebDriver key string into the focused element, Element Send
 * Keys style: modifier keys toggle, U+E000 releases them, and anything
 * still held is released at the end.  Returns a navigation it caused. */
static char *
wd_type_string(wd_window *w, const char *text)
{
    int mods = 0;
    GArray *held = g_array_new(FALSE, FALSE, sizeof(gunichar));
    char *nav = NULL;
    for (const char *p = text; *p && !nav; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        wd_key k;
        wd_key_lookup(ch, (mods & 1) != 0, &k);
        if (ch == 0xE000) {
            for (guint i = held->len; i > 0; i--) {
                wd_key mk;
                wd_key_lookup(g_array_index(held, gunichar, i - 1), FALSE, &mk);
                mods &= ~mk.modifier_bit;
                g_free(wd_send_key_up(w, &mk, mods));
            }
            g_array_set_size(held, 0);
            continue;
        }
        if (k.modifier) {
            guint at = held->len;
            for (guint i = 0; i < held->len; i++) {
                wd_key mk;
                wd_key_lookup(g_array_index(held, gunichar, i), FALSE, &mk);
                if (mk.modifier_bit == k.modifier_bit) at = i;
            }
            if (at < held->len) {
                mods &= ~k.modifier_bit;
                g_array_remove_index(held, at);
                g_free(wd_send_key_up(w, &k, mods));
            } else {
                mods |= k.modifier_bit;
                g_array_append_val(held, ch);
                nav = wd_send_key_down(w, &k, mods);
            }
            continue;
        }
        int m = mods | (k.shifted ? 1 : 0);
        nav = wd_send_key_down(w, &k, m);
        char *up = wd_send_key_up(w, &k, m);
        if (!nav) nav = up;
        else g_free(up);
    }
    for (guint i = held->len; i > 0; i--) {
        wd_key mk;
        wd_key_lookup(g_array_index(held, gunichar, i - 1), FALSE, &mk);
        mods &= ~mk.modifier_bit;
        g_free(wd_send_key_up(w, &mk, mods));
    }
    g_array_free(held, TRUE);
    return nav;
}

/* ------------------------------------------------------------------ */
/* Command helpers                                                     */
/* ------------------------------------------------------------------ */

static ns_json *
wd_param(wd_call *c, const char *name)
{
    return ns_json_object_get(c->body, name);
}

/* A required string parameter, or NULL with invalid argument. */
static const char *
wd_param_string(wd_call *c, const char *name)
{
    ns_json *v = wd_param(c, name);
    if (!ns_json_is(v, NS_JSON_STRING)) {
        wd_fail(c, "invalid argument", "'%s' must be a string", name);
        return NULL;
    }
    return v->string;
}

/* TRUE when v is an integer number within [lo, hi]. */
static gboolean
wd_is_int(const ns_json *v, double lo, double hi)
{
    return ns_json_is(v, NS_JSON_NUMBER) && isfinite(v->number) &&
           v->number == floor(v->number) && v->number >= lo &&
           v->number <= hi;
}

static wd_window *
wd_current(wd_session *s, wd_call *c)
{
    if (!s->current) {
        wd_fail(c, "no such window", "The current window is closed");
        return NULL;
    }
    return s->current;
}

static const char *
wd_platform_name(void)
{
#if defined(G_OS_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#else
    return "unix";
#endif
}

static ns_json *
wd_timeouts_json(wd_session *s)
{
    ns_json *t = ns_json_object();
    ns_json_object_set(t, "script", s->t_script < 0
                       ? ns_json_null() : ns_json_number((double)s->t_script));
    ns_json_object_set(t, "pageLoad", ns_json_number((double)s->t_page_load));
    ns_json_object_set(t, "implicit", ns_json_number((double)s->t_implicit));
    return t;
}

/* Validates a timeouts object; applies it to s when s is given. */
static gboolean
wd_apply_timeouts(wd_session *s, const ns_json *t, wd_call *c)
{
    if (!ns_json_is(t, NS_JSON_OBJECT)) {
        wd_fail(c, "invalid argument", "timeouts must be an object");
        return FALSE;
    }
    gint64 script = s ? s->t_script : 0, page = s ? s->t_page_load : 0,
           implicit = s ? s->t_implicit : 0;
    const char *legacy = ns_json_get_string(t, "type");
    ns_json *ms = ns_json_object_get(t, "ms");
    if (legacy && ms) {
        if (!wd_is_int(ms, 0, WD_MAX_SAFE_INT)) {
            wd_fail(c, "invalid argument", "ms must be a non-negative integer");
            return FALSE;
        }
        if (strcmp(legacy, "script") == 0) script = (gint64)ms->number;
        else if (strcmp(legacy, "implicit") == 0) implicit = (gint64)ms->number;
        else if (strcmp(legacy, "page load") == 0 ||
                 strcmp(legacy, "pageLoad") == 0) page = (gint64)ms->number;
        else {
            wd_fail(c, "invalid argument", "unknown timeout type %s", legacy);
            return FALSE;
        }
    } else {
        for (guint i = 0; i < t->keys->len; i++) {
            const char *k = g_ptr_array_index(t->keys, i);
            ns_json *v = g_ptr_array_index(t->items, i);
            if (strcmp(k, "script") == 0) {
                if (ns_json_is(v, NS_JSON_NULL)) { script = -1; continue; }
            } else if (strcmp(k, "pageLoad") != 0 && strcmp(k, "implicit") != 0) {
                continue;
            }
            if (!wd_is_int(v, 0, WD_MAX_SAFE_INT)) {
                wd_fail(c, "invalid argument",
                        "timeout '%s' must be a non-negative integer", k);
                return FALSE;
            }
            if (strcmp(k, "script") == 0) script = (gint64)v->number;
            else if (strcmp(k, "pageLoad") == 0) page = (gint64)v->number;
            else implicit = (gint64)v->number;
        }
    }
    if (s) {
        s->t_script = script;
        s->t_page_load = page;
        s->t_implicit = implicit;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* New Session                                                         */
/* ------------------------------------------------------------------ */

static const char *const wd_prompt_behaviors[] = {
    "dismiss", "accept", "dismiss and notify", "accept and notify", "ignore",
};

/* Validates one capabilities object, dropping null entries; NULL and c
 * failed when it is not valid. */
static ns_json *
wd_validate_caps(const ns_json *caps, wd_call *c)
{
    if (!ns_json_is(caps, NS_JSON_OBJECT)) {
        wd_fail(c, "invalid argument", "capabilities must be an object");
        return NULL;
    }
    ns_json *out = ns_json_object();
    for (guint i = 0; i < caps->keys->len; i++) {
        const char *k = g_ptr_array_index(caps->keys, i);
        ns_json *v = g_ptr_array_index(caps->items, i);
        if (ns_json_is(v, NS_JSON_NULL)) continue;
        gboolean ok = TRUE;
        if (strcmp(k, "acceptInsecureCerts") == 0 ||
            strcmp(k, "strictFileInteractability") == 0 ||
            strcmp(k, "setWindowRect") == 0 || strcmp(k, "webSocketUrl") == 0) {
            ok = ns_json_is(v, NS_JSON_BOOL);
        } else if (strcmp(k, "browserName") == 0 ||
                   strcmp(k, "browserVersion") == 0 ||
                   strcmp(k, "platformName") == 0 ||
                   strcmp(k, "userAgent") == 0) {
            ok = ns_json_is(v, NS_JSON_STRING);
        } else if (strcmp(k, "pageLoadStrategy") == 0) {
            ok = ns_json_is(v, NS_JSON_STRING) &&
                 (strcmp(v->string, "none") == 0 ||
                  strcmp(v->string, "eager") == 0 ||
                  strcmp(v->string, "normal") == 0);
        } else if (strcmp(k, "proxy") == 0) {
            const char *type = ns_json_get_string(v, "proxyType");
            ok = ns_json_is(v, NS_JSON_OBJECT) && type &&
                 (strcmp(type, "pac") == 0 || strcmp(type, "direct") == 0 ||
                  strcmp(type, "autodetect") == 0 ||
                  strcmp(type, "system") == 0 || strcmp(type, "manual") == 0);
        } else if (strcmp(k, "timeouts") == 0) {
            wd_call tc = {0};
            ok = wd_apply_timeouts(NULL, v, &tc);
            g_free(tc.error);
            g_free(tc.message);
        } else if (strcmp(k, "unhandledPromptBehavior") == 0) {
            ok = ns_json_is(v, NS_JSON_OBJECT);
            if (ns_json_is(v, NS_JSON_STRING))
                for (gsize j = 0; j < G_N_ELEMENTS(wd_prompt_behaviors); j++)
                    if (strcmp(v->string, wd_prompt_behaviors[j]) == 0) ok = TRUE;
        } else if (!strchr(k, ':')) {
            wd_fail(c, "invalid argument", "unknown capability '%s'", k);
            ns_json_free(out);
            return NULL;
        }
        if (!ok) {
            wd_fail(c, "invalid argument", "capability '%s' has an invalid value", k);
            ns_json_free(out);
            return NULL;
        }
        ns_json_object_set(out, k, ns_json_copy(v));
    }
    return out;
}

/* TRUE when this browser can satisfy a merged capabilities object. */
static gboolean
wd_caps_match(const ns_json *caps)
{
    const char *name = ns_json_get_string(caps, "browserName");
    if (name && g_ascii_strcasecmp(name, "northstar") != 0) return FALSE;
    const char *ver = ns_json_get_string(caps, "browserVersion");
    if (ver && !g_str_has_prefix(NS_VERSION, ver)) return FALSE;
    const char *plat = ns_json_get_string(caps, "platformName");
    if (plat && g_ascii_strcasecmp(plat, wd_platform_name()) != 0 &&
        g_ascii_strcasecmp(plat, "any") != 0)
        return FALSE;
    ns_json *proxy = ns_json_object_get(caps, "proxy");
    const char *ptype = proxy ? ns_json_get_string(proxy, "proxyType") : NULL;
    if (ptype && (strcmp(ptype, "pac") == 0 || strcmp(ptype, "autodetect") == 0))
        return FALSE;
    return TRUE;
}

/* Proxy URL for a manual proxy capability ("host:port" or a URL). */
static char *
wd_proxy_url(const ns_json *proxy)
{
    const char *p = ns_json_get_string(proxy, "sslProxy");
    if (!p) p = ns_json_get_string(proxy, "httpProxy");
    if (!p || !*p) return NULL;
    return strstr(p, "://") ? g_strdup(p) : g_strdup_printf("http://%s", p);
}

static wd_window *
wd_window_new(wd_session *s)
{
    wd_window *w = g_new0(wd_window, 1);
    w->handle = wd_uuid();
    w->page = ns_page_session_new(MAX(WD_MAX_WIDTH, s->vw),
                                  MAX(WD_MAX_HEIGHT, s->vh));
    w->history = g_ptr_array_new_with_free_func(g_free);
    w->hist_index = -1;
    w->frame_path = g_ptr_array_new_with_free_func(g_free);
    if (!w->page) {
        wd_window_free(w);
        return NULL;
    }
    ns_page_info pg;
    ns_page_session_open(w->page, "about:blank", s->vw, s->vh, 0, 0, 1, &pg);
    ns_page_info_clear(&pg);
    wd_history_push(w, "about:blank");
    w->navigated = FALSE;
    g_ptr_array_add(s->windows, w);
    return w;
}

static void
wd_cmd_new_session(wd_session *unused, wd_call *c)
{
    (void)unused;
    if (g_session) {
        wd_fail(c, "session not created",
                "Maximum number of active sessions (1) reached");
        return;
    }
    /* A legacy body with only desiredCapabilities matches as empty. */
    ns_json *caps = wd_param(c, "capabilities");
    ns_json *empty = NULL;
    if (!caps) caps = empty = ns_json_object();
    if (!ns_json_is(caps, NS_JSON_OBJECT)) {
        wd_fail(c, "invalid argument", "capabilities must be an object");
        return;
    }
    ns_json *always_raw = ns_json_object_get(caps, "alwaysMatch");
    ns_json *always = always_raw ? wd_validate_caps(always_raw, c)
                                 : ns_json_object();
    if (!always) {
        ns_json_free(empty);
        return;
    }
    ns_json *first = ns_json_object_get(caps, "firstMatch");
    GPtrArray *merged = g_ptr_array_new_with_free_func((GDestroyNotify)ns_json_free);
    if (first && (!ns_json_is(first, NS_JSON_ARRAY) || first->items->len == 0)) {
        wd_fail(c, "invalid argument", "firstMatch must be a non-empty array");
    } else if (!first) {
        g_ptr_array_add(merged, ns_json_copy(always));
    } else {
        for (guint i = 0; i < first->items->len && !wd_failed(c); i++) {
            ns_json *f = wd_validate_caps(g_ptr_array_index(first->items, i), c);
            if (!f) break;
            for (guint j = 0; j < f->keys->len; j++) {
                const char *k = g_ptr_array_index(f->keys, j);
                if (ns_json_object_has(always, k)) {
                    wd_fail(c, "invalid argument",
                            "capability '%s' is in both alwaysMatch and firstMatch", k);
                    break;
                }
            }
            if (wd_failed(c)) {
                ns_json_free(f);
                break;
            }
            ns_json *m = ns_json_copy(always);
            for (guint j = 0; j < f->keys->len; j++)
                ns_json_object_set(m, g_ptr_array_index(f->keys, j),
                                   ns_json_copy(g_ptr_array_index(f->items, j)));
            ns_json_free(f);
            g_ptr_array_add(merged, m);
        }
    }
    ns_json_free(always);
    ns_json_free(empty);
    if (wd_failed(c)) {
        g_ptr_array_free(merged, TRUE);
        return;
    }
    ns_json *match = NULL;
    for (guint i = 0; i < merged->len && !match; i++)
        if (wd_caps_match(g_ptr_array_index(merged, i)))
            match = ns_json_copy(g_ptr_array_index(merged, i));
    g_ptr_array_free(merged, TRUE);
    if (!match) {
        wd_fail(c, "session not created",
                "No matching capabilities: this is browserName \"northstar\" "
                "%s on %s", NS_VERSION, wd_platform_name());
        return;
    }

    wd_session *s = g_new0(wd_session, 1);
    s->id = wd_uuid();
    s->windows = g_ptr_array_new_with_free_func((GDestroyNotify)wd_window_free);
    s->vw = g_opts.viewport_width;
    s->vh = g_opts.viewport_height;
    s->settle_ms = g_opts.settle_ms;
    s->t_script = 30000;
    s->t_page_load = 300000;
    s->t_implicit = 0;
    s->issued = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->sources = g_ptr_array_new_with_free_func((GDestroyNotify)wd_source_free);
    const char *strategy = ns_json_get_string(match, "pageLoadStrategy");
    s->load_strategy = g_strdup(strategy ? strategy : "normal");
    s->prompt_behavior = g_strdup("dismiss and notify");
    ns_json *v = ns_json_object_get(match, "acceptInsecureCerts");
    s->accept_insecure = v && v->boolean;
    v = ns_json_object_get(match, "strictFileInteractability");
    s->strict_file = v && v->boolean;
    v = ns_json_object_get(match, "timeouts");
    if (v) wd_apply_timeouts(s, v, c);
    v = ns_json_object_get(match, "proxy");
    if (v) {
        s->proxy = ns_json_copy(v);
        const char *ptype = ns_json_get_string(v, "proxyType");
        if (ptype && strcmp(ptype, "manual") == 0) {
            char *url = wd_proxy_url(v);
            ns_net_set_proxy_override(url);
            g_free(url);
        }
    }
    s->prev_ignore_certs = ns_net_ignore_cert_errors();
    if (s->accept_insecure) ns_net_set_ignore_cert_errors(TRUE);

    s->current = wd_window_new(s);
    if (!s->current) {
        wd_session_free(s);
        wd_fail(c, "session not created", "Could not create a page session");
        return;
    }
    g_session = s;

    ns_json *rc = ns_json_object();
    ns_json_object_set(rc, "browserName", ns_json_string("northstar"));
    ns_json_object_set(rc, "browserVersion", ns_json_string(NS_VERSION));
    ns_json_object_set(rc, "platformName", ns_json_string(wd_platform_name()));
    ns_json_object_set(rc, "acceptInsecureCerts", ns_json_bool(s->accept_insecure));
    ns_json_object_set(rc, "pageLoadStrategy", ns_json_string(s->load_strategy));
    ns_json_object_set(rc, "proxy", s->proxy ? ns_json_copy(s->proxy) : ns_json_object());
    ns_json_object_set(rc, "setWindowRect", ns_json_bool(TRUE));
    ns_json_object_set(rc, "timeouts", wd_timeouts_json(s));
    ns_json_object_set(rc, "strictFileInteractability", ns_json_bool(s->strict_file));
    ns_json_object_set(rc, "unhandledPromptBehavior", ns_json_string(s->prompt_behavior));
    ns_json_object_set(rc, "userAgent", ns_json_string(ns_user_agent_for_mode(NULL)));
    ns_json_object_set(rc, "ns:headless", ns_json_bool(TRUE));
    ns_json_free(match);
    ns_json *out = ns_json_object();
    ns_json_object_set(out, "sessionId", ns_json_string(s->id));
    ns_json_object_set(out, "capabilities", rc);
    wd_set_value(c, out);
    if (g_wd_log) g_printerr("[webdriver] session %s created\n", s->id);
}

static void
wd_end_session(void)
{
    wd_session *s = g_session;
    if (!s) return;
    g_session = NULL;
    ns_net_set_ignore_cert_errors(s->prev_ignore_certs);
    if (s->proxy) ns_net_set_proxy_override(NULL);
    if (g_wd_log) g_printerr("[webdriver] session %s ended\n", s->id);
    wd_session_free(s);
}

static void
wd_cmd_delete_session(wd_session *s, wd_call *c)
{
    (void)s;
    wd_end_session();
    wd_set_value(c, ns_json_null());
}

static void
wd_cmd_status(wd_session *s, wd_call *c)
{
    (void)s;
    ns_json *v = ns_json_object();
    ns_json_object_set(v, "ready", ns_json_bool(g_session == NULL));
    ns_json_object_set(v, "message", ns_json_string(
        g_session ? "A session is already running" : "Northstar is ready"));
    ns_json *build = ns_json_object();
    ns_json_object_set(build, "version", ns_json_string(NS_VERSION));
    ns_json_object_set(v, "build", build);
    ns_json *os = ns_json_object();
    ns_json_object_set(os, "name", ns_json_string(wd_platform_name()));
    ns_json_object_set(v, "os", os);
    wd_set_value(c, v);
}

static void
wd_cmd_get_timeouts(wd_session *s, wd_call *c)
{
    wd_set_value(c, wd_timeouts_json(s));
}

static void
wd_cmd_set_timeouts(wd_session *s, wd_call *c)
{
    if (wd_apply_timeouts(s, c->body, c)) wd_set_value(c, ns_json_null());
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

/* An absolute URL to load, or NULL when url is not one. */
static char *
wd_absolute_url(const char *url)
{
    if (!url || !*url) return NULL;
    if (g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:") ||
        g_str_has_prefix(url, "javascript:"))
        return g_strdup(url);
    char *scheme = g_uri_parse_scheme(url);
    if (!scheme) return NULL;
    g_free(scheme);
    return g_strdup(url);
}

static void
wd_cmd_navigate(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    const char *url = w ? wd_param_string(c, "url") : NULL;
    if (!url) return;
    char *abs = wd_absolute_url(url);
    if (!abs) {
        wd_fail(c, "invalid argument", "'%s' is not an absolute URL", url);
        return;
    }
    gint64 start = wd_now_ms();
    if (g_str_has_prefix(abs, "javascript:")) {
        ns_json *req = wd_req("execute");
        ns_json_object_set(req, "script", ns_json_string(abs + 11));
        ns_json_object_set(req, "args", ns_json_array());
        ns_json_free(wd_atom(s, w, req, TRUE, c));
        g_clear_pointer(&c->error, g_free);
        g_clear_pointer(&c->message, g_free);
        wd_settle_after_input(s, w, NULL, start, c);
    } else {
        wd_window_open(s, w, abs, FALSE, TRUE, TRUE);
        wd_window_render(s, w, NULL);
        wd_wait_for_load(s, w, start, c);
    }
    g_free(abs);
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

static void
wd_cmd_get_url(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *info = wd_atom(s, w, wd_req("info"), TRUE, c);
    const char *url = info ? ns_json_get_string(info, "url") : NULL;
    if (url && *url) {
        wd_set_value(c, ns_json_string(url));
    } else {
        g_clear_pointer(&c->error, g_free);
        g_clear_pointer(&c->message, g_free);
        char *u = ns_page_session_url(w->page);
        wd_set_value(c, ns_json_string(u ? u : "about:blank"));
        free(u);
    }
    ns_json_free(info);
}

static void
wd_cmd_get_title(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *info = wd_atom(s, w, wd_req("info"), TRUE, c);
    const char *t = info ? ns_json_get_string(info, "title") : NULL;
    if (info) {
        wd_set_value(c, ns_json_string(t ? t : ""));
    } else {
        g_clear_pointer(&c->error, g_free);
        g_clear_pointer(&c->message, g_free);
        char *title = ns_page_session_title(w->page);
        wd_set_value(c, ns_json_string(title ? title : ""));
        free(title);
    }
    ns_json_free(info);
}

/* Back (delta -1) or Forward (+1): same-document entries first, through
 * the page's own history, then the window's list of documents. */
static void
wd_traverse(wd_session *s, wd_call *c, int delta)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    ns_json *req = wd_req("execute");
    ns_json_object_set(req, "script", ns_json_string(delta < 0
        ? "var n=window.navigation;if(n&&n.canGoBack){history.back();return true}return false"
        : "var n=window.navigation;if(n&&n.canGoForward){history.forward();return true}return false"));
    ns_json_object_set(req, "args", ns_json_array());
    wd_call probe = {0};
    ns_json *same_doc = wd_atom(s, w, req, TRUE, &probe);
    gboolean handled = ns_json_is(same_doc, NS_JSON_BOOL) && same_doc->boolean;
    ns_json_free(same_doc);
    g_free(probe.error);
    g_free(probe.message);
    g_free(probe.stacktrace);
    if (handled) {
        wd_pump(s, 20);
        wd_set_value(c, ns_json_null());
        return;
    }
    int target = w->hist_index + delta;
    if (target >= 0 && target < (int)w->history->len) {
        w->hist_index = target;
        char *url = g_strdup(g_ptr_array_index(w->history, target));
        wd_window_open(s, w, url, TRUE, TRUE, FALSE);
        g_free(url);
        wd_window_render(s, w, NULL);
        wd_wait_for_load(s, w, start, c);
    }
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

static void wd_cmd_back(wd_session *s, wd_call *c) { wd_traverse(s, c, -1); }
static void wd_cmd_forward(wd_session *s, wd_call *c) { wd_traverse(s, c, 1); }

static void
wd_cmd_refresh(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    char *url = ns_page_session_url(w->page);
    if (!url && w->hist_index >= 0)
        url = strdup(g_ptr_array_index(w->history, w->hist_index));
    if (url) {
        wd_window_open(s, w, url, FALSE, TRUE, FALSE);
        wd_window_render(s, w, NULL);
        wd_wait_for_load(s, w, start, c);
    }
    free(url);
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

/* ------------------------------------------------------------------ */
/* Windows and frames                                                  */
/* ------------------------------------------------------------------ */

static ns_json *
wd_handles(wd_session *s)
{
    ns_json *a = ns_json_array();
    for (guint i = 0; s && i < s->windows->len; i++)
        ns_json_array_add(a, ns_json_string(
            ((wd_window *)g_ptr_array_index(s->windows, i))->handle));
    return a;
}

static void
wd_cmd_get_window(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (w) wd_set_value(c, ns_json_string(w->handle));
}

static void
wd_cmd_get_handles(wd_session *s, wd_call *c)
{
    wd_set_value(c, wd_handles(s));
}

static void
wd_cmd_close_window(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    g_ptr_array_remove(s->windows, w);
    s->current = NULL;
    ns_json *handles = wd_handles(s);
    if (s->windows->len == 0) wd_end_session();
    wd_set_value(c, handles);
}

static void
wd_cmd_switch_window(wd_session *s, wd_call *c)
{
    /* "name" is what Selenium 3 clients send. */
    const char *h = ns_json_get_string(c->body, "handle");
    if (!h) h = ns_json_get_string(c->body, "name");
    if (!h) {
        wd_fail(c, "invalid argument", "'handle' must be a string");
        return;
    }
    for (guint i = 0; i < s->windows->len; i++) {
        wd_window *w = g_ptr_array_index(s->windows, i);
        if (strcmp(w->handle, h) == 0) {
            s->current = w;
            g_ptr_array_set_size(w->frame_path, 0);
            wd_set_value(c, ns_json_null());
            return;
        }
    }
    wd_fail(c, "no such window", "No window with handle %s", h);
}

static void
wd_cmd_new_window(wd_session *s, wd_call *c)
{
    ns_json *type = wd_param(c, "type");
    if (type && !ns_json_is(type, NS_JSON_STRING) && !ns_json_is(type, NS_JSON_NULL)) {
        wd_fail(c, "invalid argument", "type must be a string");
        return;
    }
    wd_window *w = wd_window_new(s);
    if (!w) {
        wd_fail(c, "unknown error", "Could not open a new window");
        return;
    }
    ns_json *v = ns_json_object();
    ns_json_object_set(v, "handle", ns_json_string(w->handle));
    ns_json_object_set(v, "type", ns_json_string("window"));
    wd_set_value(c, v);
}

static void
wd_cmd_switch_frame(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *id = wd_param(c, "id");
    if (!c->body || !ns_json_object_has(c->body, "id")) {
        wd_fail(c, "invalid argument", "'id' is required");
        return;
    }
    if (ns_json_is(id, NS_JSON_NULL)) {
        g_ptr_array_set_size(w->frame_path, 0);
        wd_set_value(c, ns_json_null());
        return;
    }
    ns_json *req = wd_req("frame");
    if (wd_is_int(id, 0, 65535)) {
        ns_json_object_set(req, "id", ns_json_number(id->number));
    } else if (ns_json_is(id, NS_JSON_OBJECT) &&
               ns_json_get_string(id, WD_ELEMENT)) {
        ns_json_object_set(req, "id", ns_json_copy(id));
    } else {
        ns_json_free(req);
        wd_fail(c, "invalid argument",
                "'id' must be null, an integer 0-65535 or an element reference");
        return;
    }
    ns_json *frame = wd_atom(s, w, req, FALSE, c);
    if (ns_json_is(frame, NS_JSON_STRING)) {
        g_ptr_array_add(w->frame_path, g_strdup(frame->string));
        wd_set_value(c, ns_json_null());
    }
    ns_json_free(frame);
}

static void
wd_cmd_parent_frame(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    if (w->frame_path->len > 0)
        g_ptr_array_remove_index(w->frame_path, w->frame_path->len - 1);
    wd_set_value(c, ns_json_null());
}

static ns_json *
wd_rect_json(wd_session *s)
{
    ns_json *r = ns_json_object();
    ns_json_object_set(r, "x", ns_json_number(s->win_x));
    ns_json_object_set(r, "y", ns_json_number(s->win_y));
    ns_json_object_set(r, "width", ns_json_number(s->vw));
    ns_json_object_set(r, "height", ns_json_number(s->vh));
    return r;
}

/* Resizes every window's viewport (the headless window has no chrome, so
 * the outer size is the viewport size). */
static void
wd_resize(wd_session *s, int width, int height)
{
    s->vw = CLAMP(width, 1, MAX(WD_MAX_WIDTH, g_opts.viewport_width));
    s->vh = CLAMP(height, 1, MAX(WD_MAX_HEIGHT, g_opts.viewport_height));
    for (guint i = 0; i < s->windows->len; i++) {
        wd_window *w = g_ptr_array_index(s->windows, i);
        ns_page_session_set_viewport(w->page, s->vw, s->vh, NULL);
        wd_window_render(s, w, NULL);
    }
}

static void
wd_cmd_get_rect(wd_session *s, wd_call *c)
{
    if (wd_current(s, c)) wd_set_value(c, wd_rect_json(s));
}

static void
wd_cmd_set_rect(wd_session *s, wd_call *c)
{
    if (!wd_current(s, c)) return;
    static const char *const keys[] = { "x", "y", "width", "height" };
    double vals[4] = { 0 };
    gboolean have[4] = { FALSE };
    for (int i = 0; i < 4; i++) {
        ns_json *v = wd_param(c, keys[i]);
        if (!v || ns_json_is(v, NS_JSON_NULL)) continue;
        double lo = i < 2 ? -2147483648.0 : 0, hi = i < 2 ? 2147483647.0 : 2147483647.0;
        if (!wd_is_int(v, lo, hi)) {
            wd_fail(c, "invalid argument", "'%s' must be an integer", keys[i]);
            return;
        }
        vals[i] = v->number;
        have[i] = TRUE;
    }
    if (have[2] || have[3])
        wd_resize(s, have[2] ? (int)vals[2] : s->vw, have[3] ? (int)vals[3] : s->vh);
    if (have[0]) s->win_x = (int)vals[0];
    if (have[1]) s->win_y = (int)vals[1];
    wd_set_value(c, wd_rect_json(s));
}

static void
wd_cmd_maximize(wd_session *s, wd_call *c)
{
    if (!wd_current(s, c)) return;
    s->win_x = 0;
    s->win_y = 0;
    wd_resize(s, WD_SCREEN_W, WD_SCREEN_H);
    wd_set_value(c, wd_rect_json(s));
}

static void
wd_cmd_minimize(wd_session *s, wd_call *c)
{
    if (wd_current(s, c)) wd_set_value(c, wd_rect_json(s));
}

/* ------------------------------------------------------------------ */
/* Elements                                                            */
/* ------------------------------------------------------------------ */

static gboolean
wd_valid_strategy(const char *using)
{
    return using && (strcmp(using, "css selector") == 0 ||
                     strcmp(using, "link text") == 0 ||
                     strcmp(using, "partial link text") == 0 ||
                     strcmp(using, "tag name") == 0 ||
                     strcmp(using, "xpath") == 0);
}

/* Find Element(s), from the document or an element or shadow root, with
 * the session's implicit wait. */
static void
wd_find(wd_session *s, wd_call *c, const char *from_key, const char *from_id,
        gboolean multiple)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *using = wd_param_string(c, "using");
    if (!using) return;
    if (!wd_valid_strategy(using)) {
        wd_fail(c, "invalid argument", "Unknown location strategy '%s'", using);
        return;
    }
    const char *value = wd_param_string(c, "value");
    if (!value) return;
    gint64 deadline = wd_now_ms() + s->t_implicit;
    for (;;) {
        ns_json *req = wd_req("find");
        ns_json_object_set(req, "using", ns_json_string(using));
        ns_json_object_set(req, "value", ns_json_string(value));
        ns_json_object_set(req, "multiple", ns_json_bool(multiple));
        if (from_key) {
            ns_json *from = ns_json_object();
            ns_json_object_set(from, from_key, ns_json_string(from_id));
            ns_json_object_set(req, "from", from);
        }
        ns_json *found = wd_atom(s, w, req, FALSE, c);
        gboolean empty = multiple ? ns_json_array_len(found) == 0
                                  : g_strcmp0(c->error, "no such element") == 0;
        if (!empty || wd_now_ms() >= deadline) {
            if (found) wd_set_value(c, found);
            return;
        }
        ns_json_free(found);
        g_clear_pointer(&c->error, g_free);
        g_clear_pointer(&c->message, g_free);
        g_clear_pointer(&c->stacktrace, g_free);
        wd_pump(s, 50);
        if (!s->current || s->current != w) {
            wd_fail(c, "no such window", "The window closed while searching");
            return;
        }
    }
}

static void wd_cmd_find(wd_session *s, wd_call *c) { wd_find(s, c, NULL, NULL, FALSE); }
static void wd_cmd_find_all(wd_session *s, wd_call *c) { wd_find(s, c, NULL, NULL, TRUE); }
static void wd_cmd_find_from(wd_session *s, wd_call *c) { wd_find(s, c, WD_ELEMENT, c->seg[3], FALSE); }
static void wd_cmd_find_all_from(wd_session *s, wd_call *c) { wd_find(s, c, WD_ELEMENT, c->seg[3], TRUE); }
static void wd_cmd_find_from_shadow(wd_session *s, wd_call *c) { wd_find(s, c, WD_SHADOW, c->seg[3], FALSE); }
static void wd_cmd_find_all_from_shadow(wd_session *s, wd_call *c) { wd_find(s, c, WD_SHADOW, c->seg[3], TRUE); }

/* Runs a read-only element atom and returns its value as the result. */
static void
wd_element_query(wd_session *s, wd_call *c, const char *cmd, const char *name)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *req = wd_req_id(cmd, c->seg[3]);
    if (name) ns_json_object_set(req, "name", ns_json_string(name));
    ns_json *v = wd_atom(s, w, req, FALSE, c);
    if (v) wd_set_value(c, v);
}

static void wd_cmd_active(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *v = wd_atom(s, w, wd_req("active"), FALSE, c);
    if (v) wd_set_value(c, v);
}
static void wd_cmd_shadow(wd_session *s, wd_call *c) { wd_element_query(s, c, "shadow", NULL); }
static void wd_cmd_selected(wd_session *s, wd_call *c) { wd_element_query(s, c, "selected", NULL); }
static void wd_cmd_attribute(wd_session *s, wd_call *c) { wd_element_query(s, c, "attribute", c->seg[5]); }
static void wd_cmd_property(wd_session *s, wd_call *c) { wd_element_query(s, c, "property", c->seg[5]); }
static void wd_cmd_css(wd_session *s, wd_call *c) { wd_element_query(s, c, "css", c->seg[5]); }
static void wd_cmd_text(wd_session *s, wd_call *c) { wd_element_query(s, c, "text", NULL); }
static void wd_cmd_tag(wd_session *s, wd_call *c) { wd_element_query(s, c, "name", NULL); }
static void wd_cmd_rect(wd_session *s, wd_call *c) { wd_element_query(s, c, "rect", NULL); }
static void wd_cmd_enabled(wd_session *s, wd_call *c) { wd_element_query(s, c, "enabled", NULL); }
static void wd_cmd_displayed(wd_session *s, wd_call *c) { wd_element_query(s, c, "displayed", NULL); }
static void wd_cmd_role(wd_session *s, wd_call *c) { wd_element_query(s, c, "role", NULL); }
static void wd_cmd_label(wd_session *s, wd_call *c) { wd_element_query(s, c, "label", NULL); }

/* Moves the mouse to a page point and clicks the primary button there.
 * Returns a navigation the click caused (caller frees). */
static char *
wd_mouse_click(wd_window *w, int px, int py, int mods)
{
    ns_page_session_hover(w->page, px, py, NULL, NULL);
    g_free(ns_page_session_click(w->page, px, py, mods));
    int changed = 0;
    return ns_page_session_release(w->page, &changed);
}

static void
wd_cmd_click(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    ns_json *pt = wd_atom(s, w, wd_req_id("clickPoint", c->seg[3]), FALSE, c);
    if (!pt) return;
    char *nav = NULL;
    ns_json *done = ns_json_object_get(pt, "done");
    if (!(done && done->boolean)) {
        ns_json *x = ns_json_object_get(pt, "x"), *y = ns_json_object_get(pt, "y");
        double vx = x && x->type == NS_JSON_NUMBER ? x->number : 0;
        double vy = y && y->type == NS_JSON_NUMBER ? y->number : 0;
        /* Apply the scroll the atoms asked for before hit testing. */
        if (!wd_window_render(s, w, NULL))
            nav = wd_mouse_click(w, (int)vx + w->scroll_x, (int)vy + w->scroll_y,
                                 wd_session_mods(s));
    }
    ns_json_free(pt);
    wd_settle_after_input(s, w, nav, start, c);
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

static void
wd_cmd_clear(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    gint64 start = wd_now_ms();
    ns_json *v = wd_atom(s, w, wd_req_id("clear", c->seg[3]), FALSE, c);
    if (!v) return;
    ns_json_free(v);
    wd_settle_after_input(s, w, NULL, start, c);
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

static void
wd_cmd_send_keys(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *text = wd_param_string(c, "text");
    if (!text) {
        /* Selenium 3 clients send the keys as a "value" array. */
        ns_json *arr = wd_param(c, "value");
        if (!ns_json_is(arr, NS_JSON_ARRAY)) return;
        g_clear_pointer(&c->error, g_free);
        g_clear_pointer(&c->message, g_free);
        GString *joined = g_string_new(NULL);
        for (guint i = 0; i < arr->items->len; i++) {
            ns_json *it = g_ptr_array_index(arr->items, i);
            if (ns_json_is(it, NS_JSON_STRING)) g_string_append(joined, it->string);
        }
        ns_json_object_set(c->body, "text", ns_json_string_take(g_string_free(joined, FALSE)));
        text = ns_json_get_string(c->body, "text");
    }
    gint64 start = wd_now_ms();
    ns_json *req = wd_req_id("keysPrepare", c->seg[3]);
    ns_json_object_set(req, "strictFile", ns_json_bool(s->strict_file));
    ns_json *prep = wd_atom(s, w, req, FALSE, c);
    if (!prep) return;
    ns_json *file = ns_json_object_get(prep, "file");
    gboolean is_file = file && file->boolean;
    ns_json_free(prep);
    if (is_file) {
        wd_fail(c, "unsupported operation",
                "File inputs cannot be given files in this browser");
        return;
    }
    wd_window_render(s, w, NULL);
    ns_page_session_caret_to_end(w->page);
    char *nav = wd_type_string(w, text);
    wd_settle_after_input(s, w, nav, start, c);
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

static void
wd_cmd_source(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *v = wd_atom(s, w, wd_req("source"), FALSE, c);
    if (v) wd_set_value(c, v);
}

/* Execute Script, sync or async: a script still waiting on a promise or
 * its callback is polled while the page runs, up to the script timeout. */
static void
wd_execute(wd_session *s, wd_call *c, gboolean async)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    const char *script = wd_param_string(c, "script");
    if (!script) return;
    ns_json *args = wd_param(c, "args");
    if (!ns_json_is(args, NS_JSON_ARRAY)) {
        wd_fail(c, "invalid argument", "'args' must be an array");
        return;
    }
    ns_json *req = wd_req("execute");
    ns_json_object_set(req, "script", ns_json_string(script));
    ns_json_object_set(req, "args", ns_json_copy(args));
    ns_json_object_set(req, "async", ns_json_bool(async));
    gint64 start = wd_now_ms();
    ns_json *reply = wd_atom_raw(s, w, req, FALSE, c);
    while (reply && ns_json_get_string(reply, "pending")) {
        char *token = g_strdup(ns_json_get_string(reply, "pending"));
        ns_json_free(reply);
        reply = NULL;
        if (s->t_script >= 0 && wd_now_ms() - start >= s->t_script) {
            ns_json *cancel = wd_req("cancel");
            ns_json_object_set(cancel, "token", ns_json_string(token));
            ns_json_free(wd_atom_raw(s, w, cancel, TRUE, c));
            g_free(token);
            wd_fail(c, "script timeout",
                    "The script did not finish within %" G_GINT64_FORMAT " ms",
                    s->t_script);
            return;
        }
        wd_pump(s, 5);
        if (s->current != w) {
            g_free(token);
            wd_fail(c, "no such window", "The window closed while the script ran");
            return;
        }
        ns_json *poll = wd_req("poll");
        ns_json_object_set(poll, "token", ns_json_string(token));
        g_free(token);
        reply = wd_atom_raw(s, w, poll, TRUE, c);
    }
    if (!reply) return;
    if (ns_json_object_has(reply, "error")) {
        wd_atom_error(s, reply, c);
    } else {
        ns_json *v = ns_json_object_get(reply, "value");
        ns_json *out = v ? ns_json_copy(v) : ns_json_null();
        wd_collect_issued(s, out);
        wd_set_value(c, out);
    }
    ns_json_free(reply);
    /* A script that navigated (location, form.submit()) lands now. */
    w->navigated = FALSE;
    wd_window_render(s, w, NULL);
}

static void wd_cmd_execute_sync(wd_session *s, wd_call *c) { wd_execute(s, c, FALSE); }
static void wd_cmd_execute_async(wd_session *s, wd_call *c) { wd_execute(s, c, TRUE); }

/* ------------------------------------------------------------------ */
/* Cookies                                                             */
/* ------------------------------------------------------------------ */

static char *
wd_document_url(wd_session *s, wd_window *w)
{
    wd_call probe = {0};
    ns_json *info = wd_atom(s, w, wd_req("info"), TRUE, &probe);
    const char *u = info ? ns_json_get_string(info, "url") : NULL;
    char *url = u && *u ? g_strdup(u) : NULL;
    ns_json_free(info);
    g_free(probe.error);
    g_free(probe.message);
    g_free(probe.stacktrace);
    if (!url) {
        char *raw = ns_page_session_url(w->page);
        url = raw ? g_strdup(raw) : NULL;
        free(raw);
    }
    return url;
}

static ns_json *
wd_cookie_json(const ns_net_cookie *ck)
{
    ns_json *o = ns_json_object();
    ns_json_object_set(o, "name", ns_json_string(ck->name));
    ns_json_object_set(o, "value", ns_json_string(ck->value));
    ns_json_object_set(o, "path", ns_json_string(ck->path));
    ns_json_object_set(o, "domain", ns_json_string(ck->domain));
    ns_json_object_set(o, "secure", ns_json_bool(ck->secure));
    ns_json_object_set(o, "httpOnly", ns_json_bool(ck->http_only));
    if (ck->expiry > 0)
        ns_json_object_set(o, "expiry", ns_json_number((double)ck->expiry));
    ns_json_object_set(o, "sameSite", ns_json_string("None"));
    return o;
}

static void
wd_cmd_get_cookies(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    char *url = wd_document_url(s, w);
    GPtrArray *list = ns_net_cookies_list(url);
    ns_json *out = ns_json_array();
    for (guint i = 0; i < list->len; i++)
        ns_json_array_add(out, wd_cookie_json(g_ptr_array_index(list, i)));
    g_ptr_array_free(list, TRUE);
    g_free(url);
    wd_set_value(c, out);
}

static void
wd_cmd_get_cookie(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    char *url = wd_document_url(s, w);
    GPtrArray *list = ns_net_cookies_list(url);
    for (guint i = 0; i < list->len && !c->value; i++) {
        ns_net_cookie *ck = g_ptr_array_index(list, i);
        if (strcmp(ck->name, c->seg[3]) == 0) wd_set_value(c, wd_cookie_json(ck));
    }
    g_ptr_array_free(list, TRUE);
    g_free(url);
    if (!c->value) wd_fail(c, "no such cookie", "No cookie named %s", c->seg[3]);
}

static void
wd_cmd_add_cookie(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *ck = wd_param(c, "cookie");
    if (!ns_json_is(ck, NS_JSON_OBJECT)) {
        wd_fail(c, "invalid argument", "'cookie' must be an object");
        return;
    }
    const char *name = ns_json_get_string(ck, "name");
    const char *value = ns_json_get_string(ck, "value");
    if (!name || !value) {
        wd_fail(c, "invalid argument", "a cookie needs a string name and value");
        return;
    }
    ns_net_cookie add = { 0 };
    add.name = (char *)name;
    add.value = (char *)value;
    ns_json *v;
    if ((v = ns_json_object_get(ck, "path")) && !ns_json_is(v, NS_JSON_NULL)) {
        if (!ns_json_is(v, NS_JSON_STRING)) goto bad;
        add.path = v->string;
    }
    if ((v = ns_json_object_get(ck, "domain")) && !ns_json_is(v, NS_JSON_NULL)) {
        if (!ns_json_is(v, NS_JSON_STRING)) goto bad;
        add.domain = v->string;
    }
    if ((v = ns_json_object_get(ck, "secure")) && !ns_json_is(v, NS_JSON_NULL)) {
        if (!ns_json_is(v, NS_JSON_BOOL)) goto bad;
        add.secure = v->boolean;
    }
    if ((v = ns_json_object_get(ck, "httpOnly")) && !ns_json_is(v, NS_JSON_NULL)) {
        if (!ns_json_is(v, NS_JSON_BOOL)) goto bad;
        add.http_only = v->boolean;
    }
    if ((v = ns_json_object_get(ck, "expiry")) && !ns_json_is(v, NS_JSON_NULL)) {
        if (!wd_is_int(v, 0, WD_MAX_SAFE_INT)) goto bad;
        add.expiry = (gint64)v->number;
    }
    if ((v = ns_json_object_get(ck, "sameSite")) && !ns_json_is(v, NS_JSON_NULL)) {
        if (!ns_json_is(v, NS_JSON_STRING) ||
            (strcmp(v->string, "Lax") != 0 && strcmp(v->string, "Strict") != 0 &&
             strcmp(v->string, "None") != 0))
            goto bad;
    }
    {
        char *url = wd_document_url(s, w);
        const char *err = NULL;
        if (!ns_net_cookie_add(url, &add, &err))
            wd_fail(c, err ? err : "unable to set cookie", "The cookie %s was not set", name);
        else
            wd_set_value(c, ns_json_null());
        g_free(url);
    }
    return;
bad:
    wd_fail(c, "invalid argument", "a cookie field has the wrong type");
}

static void
wd_cmd_delete_cookie(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    char *url = wd_document_url(s, w);
    ns_net_cookie_delete(url, c->nseg > 3 ? c->seg[3] : NULL);
    g_free(url);
    wd_set_value(c, ns_json_null());
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static wd_source *
wd_source_get(wd_session *s, const char *id, wd_source_type type,
              const char *pointer_type, wd_call *c)
{
    for (guint i = 0; i < s->sources->len; i++) {
        wd_source *src = g_ptr_array_index(s->sources, i);
        if (strcmp(src->id, id) != 0) continue;
        if (src->type != type ||
            (type == WD_SRC_POINTER && g_strcmp0(src->pointer_type, pointer_type) != 0)) {
            wd_fail(c, "invalid argument",
                    "input source %s was used with a different type", id);
            return NULL;
        }
        return src;
    }
    wd_source *src = g_new0(wd_source, 1);
    src->id = g_strdup(id);
    src->type = type;
    src->pointer_type = g_strdup(pointer_type);
    src->pressed = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(s->sources, src);
    return src;
}

/* The key a keyDown/keyUp value names, or FALSE when it is not one
 * grapheme. */
static gboolean
wd_action_key(const char *value, gboolean shift_down, wd_key *k)
{
    if (!value || !*value) return FALSE;
    gunichar ch = g_utf8_get_char(value);
    const char *next = g_utf8_next_char(value);
    /* A grapheme cluster: one base character and combining marks. */
    for (const char *p = next; *p; p = g_utf8_next_char(p)) {
        GUnicodeType t = g_unichar_type(g_utf8_get_char(p));
        if (t != G_UNICODE_NON_SPACING_MARK && t != G_UNICODE_SPACING_MARK &&
            t != G_UNICODE_ENCLOSING_MARK && g_utf8_get_char(p) != 0x200D &&
            g_utf8_get_char(p) != 0xFE0F)
            return FALSE;
    }
    wd_key_lookup(ch, shift_down, k);
    if (*next && k->printable && !k->modifier && ch < 0xE000)
        g_strlcpy(k->key, value, sizeof k->key);
    return TRUE;
}

static gboolean
wd_validate_action(const char *src_type, const ns_json *a, wd_call *c)
{
    if (!ns_json_is(a, NS_JSON_OBJECT)) {
        wd_fail(c, "invalid argument", "an action must be an object");
        return FALSE;
    }
    const char *type = ns_json_get_string(a, "type");
    if (!type) {
        wd_fail(c, "invalid argument", "an action needs a type");
        return FALSE;
    }
    ns_json *dur = ns_json_object_get(a, "duration");
    if (dur && !ns_json_is(dur, NS_JSON_NULL) && !wd_is_int(dur, 0, WD_MAX_SAFE_INT)) {
        wd_fail(c, "invalid argument", "duration must be a non-negative integer");
        return FALSE;
    }
    if (strcmp(type, "pause") == 0) return TRUE;
    if (strcmp(src_type, "key") == 0) {
        if (strcmp(type, "keyDown") != 0 && strcmp(type, "keyUp") != 0) goto unknown;
        wd_key k;
        if (!wd_action_key(ns_json_get_string(a, "value"), FALSE, &k)) {
            wd_fail(c, "invalid argument", "a key action value must be a single key");
            return FALSE;
        }
        return TRUE;
    }
    if (strcmp(src_type, "pointer") == 0) {
        if (strcmp(type, "pointerDown") == 0 || strcmp(type, "pointerUp") == 0) {
            if (!wd_is_int(ns_json_object_get(a, "button"), 0, WD_MAX_SAFE_INT)) {
                wd_fail(c, "invalid argument", "button must be a non-negative integer");
                return FALSE;
            }
            return TRUE;
        }
        if (strcmp(type, "pointerCancel") == 0) return TRUE;
        if (strcmp(type, "pointerMove") != 0) goto unknown;
    } else if (strcmp(src_type, "wheel") == 0) {
        if (strcmp(type, "scroll") != 0) goto unknown;
        static const char *const deltas[] = { "deltaX", "deltaY" };
        for (int i = 0; i < 2; i++)
            if (!wd_is_int(ns_json_object_get(a, deltas[i]), -WD_MAX_SAFE_INT, WD_MAX_SAFE_INT)) {
                wd_fail(c, "invalid argument", "%s must be an integer", deltas[i]);
                return FALSE;
            }
    } else {
        goto unknown;
    }
    static const char *const xy[] = { "x", "y" };
    for (int i = 0; i < 2; i++) {
        ns_json *v = ns_json_object_get(a, xy[i]);
        if (v && !ns_json_is(v, NS_JSON_NULL) &&
            !(ns_json_is(v, NS_JSON_NUMBER) && isfinite(v->number))) {
            wd_fail(c, "invalid argument", "%s must be a number", xy[i]);
            return FALSE;
        }
        if (!v && strcmp(type, "scroll") == 0) {
            wd_fail(c, "invalid argument", "a scroll needs x and y");
            return FALSE;
        }
    }
    ns_json *origin = ns_json_object_get(a, "origin");
    if (origin && !ns_json_is(origin, NS_JSON_NULL)) {
        gboolean ok = ns_json_is(origin, NS_JSON_STRING)
            ? (strcmp(origin->string, "viewport") == 0 ||
               (strcmp(origin->string, "pointer") == 0 && strcmp(type, "scroll") != 0))
            : ns_json_is(origin, NS_JSON_OBJECT) && ns_json_get_string(origin, WD_ELEMENT);
        if (!ok) {
            wd_fail(c, "invalid argument", "origin must be viewport, pointer or an element");
            return FALSE;
        }
    }
    return TRUE;
unknown:
    wd_fail(c, "invalid argument", "unknown %s action type '%s'", src_type, type);
    return FALSE;
}

/* Resolves an action's target point (viewport coordinates) from its
 * origin; FALSE with c failed when outside the viewport. */
static gboolean
wd_action_point(wd_session *s, wd_window *w, wd_source *src, const ns_json *a,
                double *out_x, double *out_y, wd_call *c)
{
    ns_json *xv = ns_json_object_get(a, "x"), *yv = ns_json_object_get(a, "y");
    double x = ns_json_is(xv, NS_JSON_NUMBER) ? xv->number : 0;
    double y = ns_json_is(yv, NS_JSON_NUMBER) ? yv->number : 0;
    ns_json *origin = ns_json_object_get(a, "origin");
    if (ns_json_is(origin, NS_JSON_STRING) && strcmp(origin->string, "pointer") == 0) {
        x += src ? src->x : 0;
        y += src ? src->y : 0;
    } else if (ns_json_is(origin, NS_JSON_OBJECT)) {
        ns_json *ctr = wd_atom(s, w, wd_req_id("center", ns_json_get_string(origin, WD_ELEMENT)),
                               FALSE, c);
        if (!ctr) return FALSE;
        ns_json *cx = ns_json_object_get(ctr, "x"), *cy = ns_json_object_get(ctr, "y");
        x += ns_json_is(cx, NS_JSON_NUMBER) ? cx->number : 0;
        y += ns_json_is(cy, NS_JSON_NUMBER) ? cy->number : 0;
        ns_json_free(ctr);
    }
    if (x < 0 || y < 0 || x >= s->vw || y >= s->vh) {
        wd_fail(c, "move target out of bounds",
                "(%g, %g) is outside the %dx%d viewport", x, y, s->vw, s->vh);
        return FALSE;
    }
    *out_x = x;
    *out_y = y;
    return TRUE;
}

static void
wd_pointer_move_to(wd_window *w, wd_source *src, double x, double y)
{
    if (x != src->x || y != src->y) src->moved = TRUE;
    src->x = x;
    src->y = y;
    ns_page_session_hover(w->page, (int)x + w->scroll_x, (int)y + w->scroll_y,
                          NULL, NULL);
}

/* Dispatches one action; returns its duration in ms.  A navigation it
 * causes is followed at once. */
static gint64
wd_dispatch_action(wd_session *s, wd_window *w, wd_source *src,
                   const ns_json *a, gint64 tick_start, wd_call *c)
{
    const char *type = ns_json_get_string(a, "type");
    ns_json *dv = ns_json_object_get(a, "duration");
    gint64 duration = ns_json_is(dv, NS_JSON_NUMBER) ? (gint64)dv->number : 0;
    char *nav = NULL;
    if (strcmp(type, "pause") == 0) return duration;
    if (strcmp(type, "keyDown") == 0 || strcmp(type, "keyUp") == 0) {
        int mods = wd_session_mods(s);
        wd_key k;
        wd_action_key(ns_json_get_string(a, "value"), (mods & 1) != 0, &k);
        const char *name = k.key;
        guint at = src->pressed->len;
        for (guint i = 0; i < src->pressed->len; i++)
            if (strcmp(g_ptr_array_index(src->pressed, i), name) == 0) at = i;
        if (strcmp(type, "keyDown") == 0) {
            if (at == src->pressed->len) g_ptr_array_add(src->pressed, g_strdup(name));
            nav = wd_send_key_down(w, &k, wd_session_mods(s));
        } else {
            if (at == src->pressed->len) {
                /* A shifted letter may have been pressed unshifted. */
                for (guint i = 0; i < src->pressed->len; i++)
                    if (g_ascii_strcasecmp(g_ptr_array_index(src->pressed, i), name) == 0)
                        at = i;
                if (at == src->pressed->len) return 0;
            }
            g_ptr_array_remove_index(src->pressed, at);
            nav = wd_send_key_up(w, &k, wd_session_mods(s));
        }
    } else if (strcmp(type, "pointerDown") == 0 || strcmp(type, "pointerUp") == 0) {
        guint button = (guint)ns_json_object_get(a, "button")->number;
        guint bit = button < 31 ? 1u << button : 0;
        int px = (int)src->x + w->scroll_x, py = (int)src->y + w->scroll_y;
        int mods = wd_session_mods(s);
        if (strcmp(type, "pointerDown") == 0) {
            if (src->buttons & bit) return 0;
            src->buttons |= bit;
            src->moved = FALSE;
            if (button == 0)
                nav = ns_page_session_click(w->page, px, py, mods);
            else if (button == 2)
                ns_page_session_contextmenu(w->page, px, py);
        } else {
            if (!(src->buttons & bit)) return 0;
            src->buttons &= ~bit;
            if (button == 0) {
                int changed = 0;
                g_free(nav);
                nav = src->moved ? ns_page_session_release_at(w->page, px, py, &changed)
                                 : ns_page_session_release(w->page, &changed);
                /* A second click within 500 ms and 4 px of the first is a
                 * double click, which the page gets as dblclick. */
                gint64 now = wd_now_ms();
                if (!src->moved && src->clicks == 1 && now - src->last_click_ms <= 500 &&
                    fabs(src->x - src->click_x) <= 4 && fabs(src->y - src->click_y) <= 4) {
                    src->clicks = 0;
                    if (!nav) {
                        ns_json *req = wd_req("dblclick");
                        ns_json_object_set(req, "x", ns_json_number(src->x));
                        ns_json_object_set(req, "y", ns_json_number(src->y));
                        ns_json_object_set(req, "mods", ns_json_number(mods));
                        ns_json_free(wd_atom(s, w, req, TRUE, c));
                    }
                } else if (!src->moved) {
                    src->clicks = 1;
                    src->last_click_ms = now;
                    src->click_x = src->x;
                    src->click_y = src->y;
                } else {
                    src->clicks = 0;
                }
            }
        }
    } else if (strcmp(type, "pointerMove") == 0) {
        double tx, ty;
        if (!wd_action_point(s, w, src, a, &tx, &ty, c)) return 0;
        double sx = src->x, sy = src->y;
        if (duration > 0) {
            gint64 t0 = wd_now_ms();
            for (;;) {
                gint64 el = wd_now_ms() - t0;
                if (el >= duration) break;
                double f = (double)el / (double)duration;
                wd_pointer_move_to(w, src, floor(sx + (tx - sx) * f),
                                   floor(sy + (ty - sy) * f));
                wd_pump(s, 16);
            }
        }
        wd_pointer_move_to(w, src, tx, ty);
    } else if (strcmp(type, "scroll") == 0) {
        double tx, ty;
        if (!wd_action_point(s, w, NULL, a, &tx, &ty, c)) return 0;
        int dx = (int)ns_json_object_get(a, "deltaX")->number;
        int dy = (int)ns_json_object_get(a, "deltaY")->number;
        ns_json *req = wd_req("wheel");
        ns_json_object_set(req, "x", ns_json_number(tx));
        ns_json_object_set(req, "y", ns_json_number(ty));
        ns_json_object_set(req, "dx", ns_json_number(dx));
        ns_json_object_set(req, "dy", ns_json_number(dy));
        ns_json_object_set(req, "mods", ns_json_number(wd_session_mods(s)));
        ns_json *r = wd_atom(s, w, req, TRUE, c);
        ns_json *prevented = r ? ns_json_object_get(r, "prevented") : NULL;
        gboolean cancel = prevented && prevented->boolean;
        ns_json_free(r);
        if (wd_failed(c)) return 0;
        if (!cancel &&
            !ns_page_session_scroll(w->page, (int)tx + w->scroll_x,
                                    (int)ty + w->scroll_y, dx, dy)) {
            int max_x = MAX(0, w->page_w - s->vw), max_y = MAX(0, w->page_h - s->vh);
            w->scroll_x = CLAMP(w->scroll_x + dx, 0, max_x);
            w->scroll_y = CLAMP(w->scroll_y + dy, 0, max_y);
            /* The first frame paints at the new offset; the second one's
             * tick tells the page (scrollY, scroll events). */
            if (!wd_window_render(s, w, NULL)) wd_window_render(s, w, NULL);
        }
    }
    (void)tick_start;
    if (nav && *nav) {
        wd_window_open(s, w, nav, FALSE, TRUE, TRUE);
        wd_wait_for_load(s, w, wd_now_ms(), c);
    }
    g_free(nav);
    return duration;
}

typedef struct {
    wd_source *src;
    ns_json   *list;
} wd_sequence;

static void
wd_cmd_perform_actions(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *actions = wd_param(c, "actions");
    if (!ns_json_is(actions, NS_JSON_ARRAY)) {
        wd_fail(c, "invalid argument", "'actions' must be an array");
        return;
    }
    GArray *seqs = g_array_new(FALSE, FALSE, sizeof(wd_sequence));
    guint ticks = 0;
    for (guint i = 0; i < actions->items->len && !wd_failed(c); i++) {
        ns_json *seq = g_ptr_array_index(actions->items, i);
        const char *type = ns_json_get_string(seq, "type");
        const char *id = ns_json_get_string(seq, "id");
        ns_json *list = ns_json_object_get(seq, "actions");
        if (!type || !id || !ns_json_is(list, NS_JSON_ARRAY)) {
            wd_fail(c, "invalid argument",
                    "an action sequence needs a type, an id and an actions array");
            break;
        }
        wd_source_type st;
        if (strcmp(type, "none") == 0) st = WD_SRC_NONE;
        else if (strcmp(type, "key") == 0) st = WD_SRC_KEY;
        else if (strcmp(type, "pointer") == 0) st = WD_SRC_POINTER;
        else if (strcmp(type, "wheel") == 0) st = WD_SRC_WHEEL;
        else {
            wd_fail(c, "invalid argument", "unknown input source type '%s'", type);
            break;
        }
        const char *ptype = NULL;
        if (st == WD_SRC_POINTER) {
            ns_json *params = ns_json_object_get(seq, "parameters");
            ptype = params ? ns_json_get_string(params, "pointerType") : NULL;
            if (!ptype) ptype = "mouse";
            if (strcmp(ptype, "mouse") != 0 && strcmp(ptype, "pen") != 0 &&
                strcmp(ptype, "touch") != 0) {
                wd_fail(c, "invalid argument", "unknown pointerType '%s'", ptype);
                break;
            }
        }
        for (guint j = 0; j < list->items->len; j++)
            if (!wd_validate_action(type, g_ptr_array_index(list->items, j), c)) break;
        if (wd_failed(c)) break;
        wd_source *src = wd_source_get(s, id, st, ptype, c);
        if (!src) break;
        wd_sequence sq = { src, list };
        g_array_append_val(seqs, sq);
        ticks = MAX(ticks, list->items->len);
    }
    for (guint t = 0; t < ticks && !wd_failed(c); t++) {
        gint64 tick_start = wd_now_ms(), tick_ms = 0;
        for (guint i = 0; i < seqs->len && !wd_failed(c); i++) {
            wd_sequence *sq = &g_array_index(seqs, wd_sequence, i);
            if (t >= sq->list->items->len) continue;
            if (s->current != w) break;
            gint64 d = wd_dispatch_action(s, w, sq->src,
                                          g_ptr_array_index(sq->list->items, t),
                                          tick_start, c);
            tick_ms = MAX(tick_ms, d);
        }
        if (s->current != w) break;
        wd_window_render(s, w, NULL);
        gint64 left = tick_start + tick_ms - wd_now_ms();
        if (left > 0) wd_pump(s, (int)left);
    }
    g_array_free(seqs, TRUE);
    if (!wd_failed(c)) wd_set_value(c, ns_json_null());
}

static void
wd_cmd_release_actions(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    for (guint i = s->sources->len; i > 0; i--) {
        wd_source *src = g_ptr_array_index(s->sources, i - 1);
        while (src->type == WD_SRC_KEY && src->pressed->len > 0) {
            char *name = g_ptr_array_steal_index(src->pressed, src->pressed->len - 1);
            wd_key k;
            wd_action_key(name, FALSE, &k);
            g_free(wd_send_key_up(w, &k, wd_session_mods(s)));
            g_free(name);
        }
        if (src->type == WD_SRC_POINTER && (src->buttons & 1)) {
            int changed = 0;
            g_free(ns_page_session_release_at(w->page, (int)src->x + w->scroll_x,
                                              (int)src->y + w->scroll_y, &changed));
        }
        src->buttons = 0;
    }
    g_ptr_array_set_size(s->sources, 0);
    wd_window_render(s, w, NULL);
    wd_set_value(c, ns_json_null());
}

/* ------------------------------------------------------------------ */
/* Alerts, screenshots and print                                       */
/* ------------------------------------------------------------------ */

/* alert(), confirm() and prompt() never block in this browser (alert
 * logs, confirm answers true, prompt returns its default), so there is
 * never a user prompt open. */
static void
wd_cmd_alert(wd_session *s, wd_call *c)
{
    if (!wd_current(s, c)) return;
    wd_fail(c, "no such alert", "No user prompt is open");
}

static cairo_status_t
wd_byte_sink(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

/* Base64 PNG of a clipped rectangle of the window's current frame. */
static char *
wd_capture(wd_session *s, wd_window *w, double x, double y, double width,
           double height)
{
    ns_page_frame fr;
    if (wd_window_render(s, w, NULL)) wd_window_render(s, w, NULL);
    wd_window_render(s, w, &fr);
    if (!fr.ok || !fr.pixels) {
        ns_page_frame_clear(&fr);
        return NULL;
    }
    int x0 = (int)floor(MAX(x, 0)), y0 = (int)floor(MAX(y, 0));
    int x1 = (int)ceil(MIN(x + width, (double)fr.width));
    int y1 = (int)ceil(MIN(y + height, (double)fr.height));
    char *b64 = NULL;
    if (x1 > x0 && y1 > y0) {
        cairo_surface_t *src = cairo_image_surface_create_for_data(
            (unsigned char *)fr.pixels, CAIRO_FORMAT_ARGB32, fr.width, fr.height,
            fr.stride);
        cairo_surface_t *dst =
            cairo_image_surface_create(CAIRO_FORMAT_RGB24, x1 - x0, y1 - y0);
        cairo_t *cr = cairo_create(dst);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_set_source_surface(cr, src, -x0, -y0);
        cairo_paint(cr);
        cairo_destroy(cr);
        GByteArray *buf = g_byte_array_new();
        if (cairo_surface_write_to_png_stream(dst, wd_byte_sink, buf) ==
                CAIRO_STATUS_SUCCESS && buf->len)
            b64 = g_base64_encode(buf->data, buf->len);
        g_byte_array_free(buf, TRUE);
        cairo_surface_destroy(dst);
        cairo_surface_destroy(src);
    }
    ns_page_frame_clear(&fr);
    return b64;
}

static void
wd_cmd_screenshot(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    char *b64 = wd_capture(s, w, 0, 0, s->vw, s->vh);
    if (!b64) {
        wd_fail(c, "unable to capture screen", "The page could not be rendered");
        return;
    }
    wd_set_value(c, ns_json_string_take(b64));
}

static void
wd_cmd_element_screenshot(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *box = wd_atom(s, w, wd_req_id("box", c->seg[3]), FALSE, c);
    if (!box) return;
    double v[4];
    static const char *const keys[] = { "x", "y", "width", "height" };
    for (int i = 0; i < 4; i++) {
        ns_json *n = ns_json_object_get(box, keys[i]);
        v[i] = ns_json_is(n, NS_JSON_NUMBER) ? n->number : 0;
    }
    ns_json_free(box);
    char *b64 = wd_capture(s, w, v[0], v[1], v[2], v[3]);
    if (!b64) {
        wd_fail(c, "unable to capture screen",
                "The element has no visible area in the viewport");
        return;
    }
    wd_set_value(c, ns_json_string_take(b64));
}

/* A non-negative number member of an object, or def; FALSE when present
 * with the wrong type. */
static gboolean
wd_number_param(const ns_json *o, const char *key, double def, double min,
                double *out)
{
    ns_json *v = o ? ns_json_object_get(o, key) : NULL;
    if (!v) {
        *out = def;
        return TRUE;
    }
    if (!ns_json_is(v, NS_JSON_NUMBER) || !isfinite(v->number) || v->number < min)
        return FALSE;
    *out = v->number;
    return TRUE;
}

/* Parses pageRanges into a page-selection mask over n pages. */
static gboolean
wd_page_ranges(const ns_json *ranges, guint n, gboolean *keep)
{
    if (!ranges || ranges->items->len == 0) {
        for (guint i = 0; i < n; i++) keep[i] = TRUE;
        return TRUE;
    }
    for (guint i = 0; i < n; i++) keep[i] = FALSE;
    for (guint i = 0; i < ranges->items->len; i++) {
        ns_json *r = g_ptr_array_index(ranges->items, i);
        double lo, hi;
        if (wd_is_int(r, 1, WD_MAX_SAFE_INT)) {
            lo = hi = r->number;
        } else if (ns_json_is(r, NS_JSON_STRING)) {
            const char *dash = strchr(r->string, '-');
            char *end = NULL;
            if (!dash) {
                lo = hi = g_ascii_strtod(r->string, &end);
                if (end == r->string || *g_strchug(end)) return FALSE;
            } else {
                g_autofree char *a = g_strstrip(g_strndup(r->string, (gsize)(dash - r->string)));
                g_autofree char *b = g_strstrip(g_strdup(dash + 1));
                lo = *a ? g_ascii_strtod(a, &end) : 1;
                if (*a && *end) return FALSE;
                hi = *b ? g_ascii_strtod(b, &end) : (double)n;
                if (*b && *end) return FALSE;
            }
            if (lo < 1 || hi < lo || lo != floor(lo) || hi != floor(hi)) return FALSE;
        } else {
            return FALSE;
        }
        for (double p = lo; p <= hi && p <= n; p++) keep[(guint)p - 1] = TRUE;
    }
    return TRUE;
}

static void
wd_cmd_print(wd_session *s, wd_call *c)
{
    wd_window *w = wd_current(s, c);
    if (!w) return;
    ns_json *body = c->body;
    const char *orient = ns_json_get_string(body, "orientation");
    if (ns_json_object_has(body, "orientation") &&
        (!orient || (strcmp(orient, "portrait") != 0 && strcmp(orient, "landscape") != 0))) {
        wd_fail(c, "invalid argument", "orientation must be portrait or landscape");
        return;
    }
    double scale, pw, ph, mt, mb, ml, mr;
    ns_json *page = ns_json_object_get(body, "page");
    ns_json *margin = ns_json_object_get(body, "margin");
    ns_json *ranges = ns_json_object_get(body, "pageRanges");
    if ((page && !ns_json_is(page, NS_JSON_OBJECT)) ||
        (margin && !ns_json_is(margin, NS_JSON_OBJECT)) ||
        (ranges && !ns_json_is(ranges, NS_JSON_ARRAY)) ||
        !wd_number_param(body, "scale", 1.0, 0.1, &scale) || scale > 2.0 ||
        !wd_number_param(page, "width", 21.59, 2.54 / 72.0, &pw) ||
        !wd_number_param(page, "height", 27.94, 2.54 / 72.0, &ph) ||
        !wd_number_param(margin, "top", 1.0, 0, &mt) ||
        !wd_number_param(margin, "bottom", 1.0, 0, &mb) ||
        !wd_number_param(margin, "left", 1.0, 0, &ml) ||
        !wd_number_param(margin, "right", 1.0, 0, &mr)) {
        wd_fail(c, "invalid argument", "invalid print parameters");
        return;
    }
    static const char *const bools[] = { "background", "shrinkToFit" };
    for (int i = 0; i < 2; i++) {
        ns_json *v = ns_json_object_get(body, bools[i]);
        if (v && !ns_json_is(v, NS_JSON_BOOL)) {
            wd_fail(c, "invalid argument", "%s must be a boolean", bools[i]);
            return;
        }
    }
    double px_per_cm = 96.0 / 2.54;
    ns_print_setup setup;
    gboolean landscape = orient && strcmp(orient, "landscape") == 0;
    setup.width = (landscape ? ph : pw) * px_per_cm;
    setup.height = (landscape ? pw : ph) * px_per_cm;
    setup.margin_top = mt * px_per_cm;
    setup.margin_bottom = mb * px_per_cm;
    setup.margin_left = ml * px_per_cm;
    setup.margin_right = mr * px_per_cm;
    if (setup.width - setup.margin_left - setup.margin_right < 1 ||
        setup.height - setup.margin_top - setup.margin_bottom < 1) {
        wd_fail(c, "unsupported operation", "The margins leave no room on the page");
        return;
    }
    ns_print_setup used;
    GPtrArray *pages = ns_page_session_print_setup(w->page, &setup, &used);
    if (!pages) {
        wd_fail(c, "unknown error", "The page could not be paginated");
        return;
    }
    gboolean *keep = g_new0(gboolean, MAX(pages->len, 1));
    if (!wd_page_ranges(ranges, pages->len, keep)) {
        for (guint i = 0; i < pages->len; i++)
            cairo_surface_destroy(g_ptr_array_index(pages, i));
        g_ptr_array_free(pages, TRUE);
        g_free(keep);
        wd_fail(c, "invalid argument", "invalid pageRanges");
        return;
    }
    GByteArray *pdf = g_byte_array_new();
    double pt = 72.0 / 96.0;
    cairo_surface_t *surf = cairo_pdf_surface_create_for_stream(
        wd_byte_sink, pdf, used.width * pt, used.height * pt);
    cairo_t *cr = cairo_create(surf);
    guint printed = 0;
    for (guint i = 0; i < pages->len; i++) {
        cairo_surface_t *rec = g_ptr_array_index(pages, i);
        if (keep[i]) {
            cairo_save(cr);
            cairo_scale(cr, pt, pt);
            cairo_set_source_surface(cr, rec, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_show_page(cr);
            printed++;
        }
        cairo_surface_destroy(rec);
    }
    if (printed == 0) cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_surface_destroy(surf);
    g_ptr_array_free(pages, TRUE);
    g_free(keep);
    wd_window_render(s, w, NULL);
    wd_set_value(c, ns_json_string_take(g_base64_encode(pdf->data, pdf->len)));
    g_byte_array_free(pdf, TRUE);
}

/* ------------------------------------------------------------------ */
/* Routing                                                             */
/* ------------------------------------------------------------------ */

typedef void (*wd_fn)(wd_session *s, wd_call *c);

typedef struct {
    const char *method;
    const char *pattern;
    wd_fn       fn;
} wd_route;

/* "*" matches one path segment.  A route below "session/<id>" needs a
 * session with that id. */
static const wd_route wd_routes[] = {
    { "POST",   "session", wd_cmd_new_session },
    { "GET",    "status", wd_cmd_status },
    { "DELETE", "session/*", wd_cmd_delete_session },
    { "GET",    "session/*/timeouts", wd_cmd_get_timeouts },
    { "POST",   "session/*/timeouts", wd_cmd_set_timeouts },
    { "POST",   "session/*/url", wd_cmd_navigate },
    { "GET",    "session/*/url", wd_cmd_get_url },
    { "POST",   "session/*/back", wd_cmd_back },
    { "POST",   "session/*/forward", wd_cmd_forward },
    { "POST",   "session/*/refresh", wd_cmd_refresh },
    { "GET",    "session/*/title", wd_cmd_get_title },
    { "GET",    "session/*/window", wd_cmd_get_window },
    { "DELETE", "session/*/window", wd_cmd_close_window },
    { "POST",   "session/*/window", wd_cmd_switch_window },
    { "GET",    "session/*/window/handles", wd_cmd_get_handles },
    { "POST",   "session/*/window/new", wd_cmd_new_window },
    { "POST",   "session/*/frame", wd_cmd_switch_frame },
    { "POST",   "session/*/frame/parent", wd_cmd_parent_frame },
    { "GET",    "session/*/window/rect", wd_cmd_get_rect },
    { "POST",   "session/*/window/rect", wd_cmd_set_rect },
    { "POST",   "session/*/window/maximize", wd_cmd_maximize },
    { "POST",   "session/*/window/minimize", wd_cmd_minimize },
    { "POST",   "session/*/window/fullscreen", wd_cmd_maximize },
    { "GET",    "session/*/element/active", wd_cmd_active },
    { "POST",   "session/*/element", wd_cmd_find },
    { "POST",   "session/*/elements", wd_cmd_find_all },
    { "POST",   "session/*/element/*/element", wd_cmd_find_from },
    { "POST",   "session/*/element/*/elements", wd_cmd_find_all_from },
    { "GET",    "session/*/element/*/shadow", wd_cmd_shadow },
    { "POST",   "session/*/shadow/*/element", wd_cmd_find_from_shadow },
    { "POST",   "session/*/shadow/*/elements", wd_cmd_find_all_from_shadow },
    { "GET",    "session/*/element/*/selected", wd_cmd_selected },
    { "GET",    "session/*/element/*/attribute/*", wd_cmd_attribute },
    { "GET",    "session/*/element/*/property/*", wd_cmd_property },
    { "GET",    "session/*/element/*/css/*", wd_cmd_css },
    { "GET",    "session/*/element/*/text", wd_cmd_text },
    { "GET",    "session/*/element/*/name", wd_cmd_tag },
    { "GET",    "session/*/element/*/rect", wd_cmd_rect },
    { "GET",    "session/*/element/*/enabled", wd_cmd_enabled },
    { "GET",    "session/*/element/*/displayed", wd_cmd_displayed },
    { "GET",    "session/*/element/*/computedrole", wd_cmd_role },
    { "GET",    "session/*/element/*/computedlabel", wd_cmd_label },
    { "POST",   "session/*/element/*/click", wd_cmd_click },
    { "POST",   "session/*/element/*/clear", wd_cmd_clear },
    { "POST",   "session/*/element/*/value", wd_cmd_send_keys },
    { "GET",    "session/*/element/*/screenshot", wd_cmd_element_screenshot },
    { "GET",    "session/*/source", wd_cmd_source },
    { "POST",   "session/*/execute/sync", wd_cmd_execute_sync },
    { "POST",   "session/*/execute/async", wd_cmd_execute_async },
    { "GET",    "session/*/cookie", wd_cmd_get_cookies },
    { "GET",    "session/*/cookie/*", wd_cmd_get_cookie },
    { "POST",   "session/*/cookie", wd_cmd_add_cookie },
    { "DELETE", "session/*/cookie/*", wd_cmd_delete_cookie },
    { "DELETE", "session/*/cookie", wd_cmd_delete_cookie },
    { "POST",   "session/*/actions", wd_cmd_perform_actions },
    { "DELETE", "session/*/actions", wd_cmd_release_actions },
    { "POST",   "session/*/alert/dismiss", wd_cmd_alert },
    { "POST",   "session/*/alert/accept", wd_cmd_alert },
    { "GET",    "session/*/alert/text", wd_cmd_alert },
    { "POST",   "session/*/alert/text", wd_cmd_alert },
    { "GET",    "session/*/screenshot", wd_cmd_screenshot },
    { "POST",   "session/*/print", wd_cmd_print },
};

static gboolean
wd_route_matches(const char *pattern, char **seg, int nseg)
{
    char **pat = g_strsplit(pattern, "/", -1);
    int n = (int)g_strv_length(pat);
    gboolean ok = n == nseg;
    for (int i = 0; ok && i < n; i++)
        if (strcmp(pat[i], "*") != 0 && strcmp(pat[i], seg[i]) != 0) ok = FALSE;
    g_strfreev(pat);
    return ok;
}

static int
wd_error_status(const char *code)
{
    static const struct { const char *code; int status; } map[] = {
        { "element click intercepted", 400 }, { "element not interactable", 400 },
        { "insecure certificate", 400 }, { "invalid argument", 400 },
        { "invalid cookie domain", 400 }, { "invalid element state", 400 },
        { "invalid selector", 400 }, { "invalid session id", 404 },
        { "javascript error", 500 }, { "move target out of bounds", 500 },
        { "no such alert", 404 }, { "no such cookie", 404 },
        { "no such element", 404 }, { "no such frame", 404 },
        { "no such window", 404 }, { "no such shadow root", 404 },
        { "script timeout", 500 }, { "session not created", 500 },
        { "stale element reference", 404 }, { "detached shadow root", 404 },
        { "timeout", 500 }, { "unable to set cookie", 500 },
        { "unable to capture screen", 500 }, { "unexpected alert open", 500 },
        { "unknown command", 404 }, { "unknown error", 500 },
        { "unknown method", 405 }, { "unsupported operation", 500 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (strcmp(map[i].code, code) == 0) return map[i].status;
    return 500;
}

/* Handles one request on the main thread and completes it. */
static void
wd_handle(wd_request *r)
{
    wd_call c = { 0 };
    c.method = r->method;
    char **raw = g_strsplit(r->path, "/", -1);
    GPtrArray *segs = g_ptr_array_new();
    for (int i = 0; raw[i]; i++)
        if (*raw[i]) g_ptr_array_add(segs, g_uri_unescape_string(raw[i], NULL));
    g_strfreev(raw);
    /* Tolerate a "/wd/hub" prefix, as Selenium Grid URLs carry. */
    if (segs->len >= 2 && g_strcmp0(g_ptr_array_index(segs, 0), "wd") == 0 &&
        g_strcmp0(g_ptr_array_index(segs, 1), "hub") == 0) {
        g_free(g_ptr_array_index(segs, 0));
        g_free(g_ptr_array_index(segs, 1));
        g_ptr_array_remove_range(segs, 0, 2);
    }
    for (guint i = 0; i < segs->len; i++)
        if (!g_ptr_array_index(segs, i)) g_ptr_array_index(segs, i) = g_strdup("");
    g_ptr_array_add(segs, NULL);
    c.nseg = (int)segs->len - 1;
    c.seg = (char **)g_ptr_array_free(segs, FALSE);

    if (strcmp(r->method, "POST") == 0) {
        if (r->body_len == 0) {
            c.body = ns_json_object();
        } else {
            char *perr = NULL;
            c.body = ns_json_parse(r->body, r->body_len, &perr);
            if (!c.body)
                wd_fail(&c, "invalid argument", "The body is not JSON: %s", perr);
            else if (c.body->type != NS_JSON_OBJECT)
                wd_fail(&c, "invalid argument", "The body must be a JSON object");
            g_free(perr);
        }
    }
    if (g_wd_log)
        g_printerr("[webdriver] -> %s %s %.*s\n", r->method, r->path,
                   (int)MIN(r->body_len, 400), r->body ? r->body : "");

    const wd_route *route = NULL;
    gboolean path_known = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(wd_routes) && !wd_failed(&c); i++) {
        if (!wd_route_matches(wd_routes[i].pattern, c.seg, c.nseg)) continue;
        path_known = TRUE;
        if (strcmp(wd_routes[i].method, r->method) == 0) {
            route = &wd_routes[i];
            break;
        }
    }
    if (!wd_failed(&c)) {
        if (!route) {
            if (path_known)
                wd_fail(&c, "unknown method", "%s is not supported for %s",
                        r->method, r->path);
            else
                wd_fail(&c, "unknown command", "Unknown command %s %s",
                        r->method, r->path);
        } else if (c.nseg >= 2 && strcmp(c.seg[0], "session") == 0 &&
                   (!g_session || strcmp(g_session->id, c.seg[1]) != 0)) {
            wd_fail(&c, "invalid session id", "No active session with id %s",
                    c.seg[1]);
        } else {
            route->fn(g_session, &c);
        }
    }

    GString *out = g_string_new("{\"value\":");
    int status = 200;
    if (c.error) {
        status = wd_error_status(c.error);
        ns_json *e = ns_json_object();
        ns_json_object_set(e, "error", ns_json_string(c.error));
        ns_json_object_set(e, "message", ns_json_string(c.message ? c.message : ""));
        ns_json_object_set(e, "stacktrace", ns_json_string(c.stacktrace ? c.stacktrace : ""));
        ns_json_write(out, e);
        ns_json_free(e);
    } else {
        ns_json_write(out, c.value);
    }
    g_string_append_c(out, '}');
    if (g_wd_log)
        g_printerr("[webdriver] <- %d %.*s\n", status,
                   (int)MIN(out->len, 400), out->str);
    ns_json_free(c.body);
    ns_json_free(c.value);
    g_free(c.error);
    g_free(c.message);
    g_free(c.stacktrace);
    g_strfreev(c.seg);
    wd_request_complete(r, status, g_string_free(out, FALSE));
}

#ifndef G_OS_WIN32
static gboolean
wd_on_signal(gpointer data)
{
    (void)data;
    g_atomic_int_set(&g_wd_quit, 1);
    return G_SOURCE_CONTINUE;
}
#endif

/* Serves WebDriver on the loopback interface until SIGINT or SIGTERM. */
int
ns_webdriver_run(const ns_webdriver_opts *opts)
{
    g_opts = *opts;
    if (g_opts.viewport_width <= 0) g_opts.viewport_width = 1280;
    if (g_opts.viewport_height <= 0) g_opts.viewport_height = 800;
    if (g_opts.settle_ms < 0) g_opts.settle_ms = 0;
    g_wd_log = opts->log;

    GSocketListener *listener = g_socket_listener_new();
    GError *err = NULL;
    GInetAddress *lo4 = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *addr = g_inet_socket_address_new(lo4, (guint16)opts->port);
    GSocketAddress *bound = NULL;
    gboolean ok = g_socket_listener_add_address(listener, addr,
                                                G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_TCP, NULL,
                                                &bound, &err);
    g_object_unref(addr);
    g_object_unref(lo4);
    if (!ok) {
        g_printerr("northstar: cannot listen for WebDriver on 127.0.0.1:%d: %s\n",
                   opts->port, err ? err->message : "unknown error");
        g_clear_error(&err);
        g_object_unref(listener);
        return 1;
    }
    guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound);
    /* Also listen on ::1 when there is IPv6, for clients that resolve
     * "localhost" to it first. */
    GInetAddress *lo6 = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV6);
    GSocketAddress *addr6 = g_inet_socket_address_new(lo6, port);
    g_socket_listener_add_address(listener, addr6, G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_TCP, NULL, NULL, NULL);
    g_object_unref(addr6);
    g_object_unref(lo6);

    g_wd_queue = g_async_queue_new();
    g_atomic_int_set(&g_wd_quit, 0);
#ifndef G_OS_WIN32
    guint sig_int = g_unix_signal_add(SIGINT, wd_on_signal, NULL);
    guint sig_term = g_unix_signal_add(SIGTERM, wd_on_signal, NULL);
#endif
    g_print("Northstar WebDriver %s listening on http://127.0.0.1:%u\n",
            NS_VERSION, port);
    fflush(stdout);
    GThread *acceptor = g_thread_new("wd-accept", wd_accept_thread, listener);

    while (!g_atomic_int_get(&g_wd_quit)) {
        wd_request *r = g_async_queue_timeout_pop(g_wd_queue, 4000);
        if (r) {
            wd_handle(r);
            continue;
        }
        wd_pump_step(g_session);
    }

    wd_end_session();
    g_socket_listener_close(listener);
    for (wd_request *r; (r = g_async_queue_try_pop(g_wd_queue));) {
        char *b = wd_error_body("unknown error", "The WebDriver server is stopping");
        wd_request_complete(r, 503, b);
    }
#ifndef G_OS_WIN32
    g_source_remove(sig_int);
    g_source_remove(sig_term);
#endif
    (void)acceptor;
    return 0;
}
