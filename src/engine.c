/* Northstar Web Browser — page pipeline: blocking fetch, stylesheet collection, cascade, layout.
 * Also speculative preloads, image fetching and decode, and PNG/PDF/text output of the box tree.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainctx.h"
#include "engine.h"

#include <glib/gstdio.h>
#include <cairo-pdf.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bytecode_cache.h"
#include "config.h"
#include "css.h"
#include "css_syntax.h"
#include "debuglog.h"
#include "image.h"
#include "paint.h"
#include "print.h"
#include "render.h"
#include "security.h"
#include "trace.h"

typedef struct fetch_state {
    GMainLoop  *loop;
    ns_response *resp;
    GError      *error;
} fetch_state;

static int g_engine_blocking_depth;

/* True while a synchronous fetch is spinning its isolated main loop. */
gboolean
ns_engine_in_blocking_fetch(void)
{
    return g_engine_blocking_depth > 0;
}

static guint64 g_engine_relayout_count;
static gint64  g_engine_relayout_us;

/* Depth-first search for the first <base href>, skipping templates and nested documents. */
static const char *
engine_base_href_walk(const ns_node *n, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (ns_node_is_element_named(n, "base")) {
        const char *href = ns_element_get_attr(n, "href");
        if (href && *href) return href;
    }
    if (ns_node_is_element_named(n, "template")) return NULL;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_DOCUMENT) continue;
        const char *href = engine_base_href_walk(c, depth + 1);
        if (href) return href;
    }
    return NULL;
}

/* Returns the first non-empty <base href> in the document, using the tag index if present. */
static const char *
engine_document_base_href(const ns_node *doc)
{
    if (!doc) return NULL;
    if (!doc->tag_index) return engine_base_href_walk(doc, 0);
    GPtrArray *bases = ns_doc_tag_index_lookup(doc, "base");
    for (guint i = 0; bases && i < bases->len; i++) {
        const char *href =
            ns_element_get_attr(g_ptr_array_index(bases, i), "href");
        if (href && *href) return href;
    }
    return NULL;
}

const char *
ns_engine_document_base_href(const ns_node *doc)
{
    return engine_document_base_href(doc);
}

/* Resolves the document's <base href> against fallback; caller frees, NULL if neither. */
static char *
engine_document_base_url(ns_node *doc, const char *fallback)
{
    const char *href = engine_document_base_href(doc);
    if (href) {
        if (fallback && *fallback) {
            char *resolved = ns_url_resolve(fallback, href);
            if (resolved) return resolved;
        }
        return g_strdup(href);
    }
    return fallback && *fallback ? g_strdup(fallback) : NULL;
}

static void
ns_engine_perf_add_relayout(gint64 elapsed_us)
{
    g_engine_relayout_count++;
    g_engine_relayout_us += elapsed_us;
}

void
ns_engine_layout_perf(guint64 *relayouts, double *total_ms)
{
    if (relayouts) *relayouts = g_engine_relayout_count;
    if (total_ms)  *total_ms  = g_engine_relayout_us / 1000.0;
}

/* Async fetch callback storing the response or error and quitting the wait loop. */
static void
on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    fetch_state *st = user_data;
    st->resp = ns_net_fetch_finish(result, &st->error);
    g_main_loop_quit(st->loop);
}

/* Pushes an isolated main context for a blocking wait and returns a loop on it. */
static GMainLoop *
engine_blocking_wait_begin(void)
{
    GMainContext *isolated = g_main_context_new();
    g_main_context_push_thread_default(isolated);
    g_engine_blocking_depth++;
    GMainLoop *loop = g_main_loop_new(isolated, FALSE);
    g_main_context_unref(isolated);
    return loop;
}

/* Pops the isolated context pushed by engine_blocking_wait_begin and frees the loop. */
static void
engine_blocking_wait_end(GMainLoop *loop)
{
    g_engine_blocking_depth--;
    g_main_context_pop_thread_default(g_main_loop_get_context(loop));
    g_main_loop_unref(loop);
}

/* Fetches url synchronously with destination-specific Accept headers; caller frees. */
static ns_response *
ns_engine_fetch_blocking_for(const char *url, const char *top_url,
                             ns_fetch_destination dest, GError **error)
{
    fetch_state st = {0};
    st.loop = engine_blocking_wait_begin();
    ns_net_request_async(url, top_url, "GET", NULL, 0, NULL,
                         ns_net_accept_headers_for(dest), dest, NULL,
                         NULL, on_fetch_done, &st);
    g_main_loop_run(st.loop);
    engine_blocking_wait_end(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

ns_response *
ns_engine_fetch_blocking(const char *url, const char *top_url, GError **error)
{
    return ns_engine_fetch_blocking_for(url, top_url, NS_FETCH_DEST_DOCUMENT,
                                        error);
}

/* POSTs body synchronously on an isolated main loop; caller frees the response. */
ns_response *
ns_engine_post_blocking(const char *url, const char *top_url,
                        const void *body, gsize body_len,
                        const char *content_type, GError **error)
{
    fetch_state st = {0};
    st.loop = engine_blocking_wait_begin();
    ns_net_post_async(url, top_url, body, body_len, content_type,
                      NULL, on_fetch_done, &st);
    g_main_loop_run(st.loop);
    engine_blocking_wait_end(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

/* True if the Content-Type is text/css or absent. */
static gboolean
content_type_is_css(const char *ct)
{
    if (!ct || !*ct) return TRUE;
    while (*ct == ' ' || *ct == '\t') ct++;
    return g_ascii_strncasecmp(ct, "text/css", 8) == 0 &&
           (ct[8] == '\0' || ct[8] == ';' || ct[8] == ' ' || ct[8] == '\t');
}

#define NS_LINKED_CSS_MAX 128

static GHashTable *g_linked_css;

/* Keeps a linked stylesheet's bytes for CSSOM readback, capped at 128 entries. */
static void
engine_remember_linked_css(const char *url, GBytes *bytes)
{
    if (!url || !*url || !bytes) return;
    if (!g_linked_css)
        g_linked_css = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_bytes_unref);
    if (g_hash_table_size(g_linked_css) >= NS_LINKED_CSS_MAX &&
        !g_hash_table_contains(g_linked_css, url))
        g_hash_table_remove_all(g_linked_css);
    g_hash_table_insert(g_linked_css, g_strdup(url), g_bytes_ref(bytes));
}

/* Returns a copy of remembered same-origin linked CSS text for url; caller frees. */
char *
ns_engine_linked_css_text(const char *url)
{
    if (!url || !*url || !g_linked_css) return NULL;
    GBytes *b = g_hash_table_lookup(g_linked_css, url);
    if (!b) return NULL;
    gsize len = 0;
    const char *data = g_bytes_get_data(b, &len);
    if (!data || len == 0) return NULL;
    return g_strndup(data, len);
}

/* Fetches a stylesheet with MIME checks for cross-origin/nosniff; caches up to 3 failures. */
static GBytes *
fetch_css_bytes(const char *url, const char *top_url, GHashTable *cache,
                gboolean strict_mime, gboolean *out_cross_origin)
{
    if (out_cross_origin) *out_cross_origin = FALSE;
    if (!url || !*url) return NULL;
    guint8 attempts = 0;
    if (cache) {
        GBytes *hit = g_hash_table_lookup(cache, url);
        if (hit) {
            gsize hsize = 0;
            const guint8 *hdata = g_bytes_get_data(hit, &hsize);
            gboolean fail_marker = hsize == 0 ||
                (hsize == 1 && hdata && hdata[0] <= 8);
            if (!fail_marker) {
                /* Cached by an earlier fetch whose final origin was not
                 * recorded: be conservative about exposing it. */
                if (out_cross_origin) *out_cross_origin = TRUE;
                return g_bytes_ref(hit);
            }
            attempts = hsize == 1 ? hdata[0] : 1;
            if (attempts >= 3) return NULL;
        }
    }
    ns_response *resp = ns_engine_fetch_blocking_for(
        url, top_url, NS_FETCH_DEST_STYLE, NULL);
    GBytes *bytes = NULL;
    /* The quirks-mode MIME leniency only applies to same-origin sheets;
     * otherwise any cross-origin HTML/JSON response would be parsed (and
     * its contents exposed) as CSS.  Judge by where the response finally
     * came from, not by the URL that was requested: a same-origin URL can
     * redirect anywhere. */
    const char *final_url = resp && resp->final_url ? resp->final_url : url;
    gboolean cross_origin = !top_url ||
        !ns_url_same_origin(top_url, url) ||
        !ns_url_same_origin(top_url, final_url);
    if (out_cross_origin) *out_cross_origin = cross_origin;
    gboolean enforce_mime = strict_mime ||
        (resp && ns_net_header_is_nosniff(resp->x_content_type_options)) ||
        cross_origin;
    gboolean mime_ok = !enforce_mime || !resp ||
                       content_type_is_css(resp->content_type);
    if (resp && !resp->error && resp->status < 400 && mime_ok &&
        resp->body && resp->body->len > 0) {
        bytes = g_bytes_new(resp->body->data, resp->body->len);
        if (cache)
            g_hash_table_insert(cache, g_strdup(url), g_bytes_ref(bytes));
    } else if (cache) {
        guint8 marker = attempts + 1;
        g_hash_table_insert(cache, g_strdup(url),
                            g_bytes_new(&marker, 1));
    }
    if (resp) ns_response_free(resp);
    return bytes;
}

/* True if the space-separated rel list contains token (ASCII case-insensitive). */
static gboolean
rel_has_token(const char *rel, const char *token)
{
    if (!rel || !token || !*token) return FALSE;
    gchar **parts = g_strsplit_set(rel, " \t\r\n\f", -1);
    gboolean found = FALSE;
    for (gchar **p = parts; *p; p++) {
        if (**p && g_ascii_strcasecmp(*p, token) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return found;
}

/* True for rel containing "stylesheet" but not "alternate". */
static gboolean
rel_is_stylesheet(const char *rel)
{
    return rel_has_token(rel, "stylesheet") &&
           !rel_has_token(rel, "alternate");
}

typedef struct {
    char                *url;
    ns_fetch_destination dest;
} preload_target;

static void
preload_target_free(gpointer p)
{
    preload_target *t = p;
    if (!t) return;
    g_free(t->url);
    g_free(t);
}

/* Maps a preload link's rel/as to a fetch destination. */
static ns_fetch_destination
preload_destination_for(const char *rel, const char *as)
{
    if (rel_has_token(rel, "modulepreload")) return NS_FETCH_DEST_SCRIPT;
    if (!as || !*as) return NS_FETCH_DEST_DEFAULT;
    if (g_ascii_strcasecmp(as, "script") == 0) return NS_FETCH_DEST_SCRIPT;
    if (g_ascii_strcasecmp(as, "style") == 0)  return NS_FETCH_DEST_STYLE;
    return NS_FETCH_DEST_DEFAULT;
}

/* Resolves and queues a unique http(s) preload target per destination; skips data: URLs. */
static void
preload_add(GPtrArray *out, GHashTable *seen, const char *base,
            const char *ref, ns_fetch_destination dest)
{
    if (!ref || !*ref || g_str_has_prefix(ref, "data:")) return;
    char *abs = ns_url_resolve(base, ref);
    if (!abs) return;
    char *slot = g_strdup_printf("%d\x1f%s", (int)dest, abs);
    if (!ns_url_is_http_or_https(abs) || g_hash_table_contains(seen, slot)) {
        g_free(slot);
        g_free(abs);
        return;
    }
    g_hash_table_add(seen, slot);
    preload_target *target = g_new0(preload_target, 1);
    target->url  = abs;
    target->dest = dest;
    g_ptr_array_add(out, target);
}

/* Scans the tree for scripts, stylesheets, preloads, eager images and preconnect origins. */
static void
preload_collect(const ns_node *n, const char *base, gboolean include_images,
                GPtrArray *out, GHashTable *seen,
                GPtrArray *connect_out, GHashTable *connect_seen, int depth)
{
    if (!n || depth >= 512) return;
    if (include_images && ns_node_is_element_named(n, "img")) {
        const char *loading = ns_element_get_attr(n, "loading");
        if (!loading || g_ascii_strcasecmp(loading, "lazy") != 0)
            preload_add(out, seen, base, ns_element_get_attr(n, "src"),
                        NS_FETCH_DEST_DEFAULT);
    } else if (ns_node_is_element_named(n, "script")) {
        preload_add(out, seen, base, ns_element_get_attr(n, "src"),
                    NS_FETCH_DEST_SCRIPT);
    } else if (ns_node_is_element_named(n, "link")) {
        const char *rel = ns_element_get_attr(n, "rel");
        if (rel && (rel_has_token(rel, "preconnect") ||
                    rel_has_token(rel, "dns-prefetch"))) {
            const char *href = ns_element_get_attr(n, "href");
            char *abs = (href && *href) ? ns_url_resolve(base, href) : NULL;
            char *origin = abs ? ns_url_origin_from(abs) : NULL;
            if (origin && ns_url_is_http_or_https(origin) &&
                !g_hash_table_contains(connect_seen, origin)) {
                g_hash_table_add(connect_seen, g_strdup(origin));
                g_ptr_array_add(connect_out, origin);
                origin = NULL;
            }
            g_free(origin);
            g_free(abs);
        } else if (rel && rel_has_token(rel, "stylesheet")) {
            preload_add(out, seen, base, ns_element_get_attr(n, "href"),
                        NS_FETCH_DEST_STYLE);
        } else if (rel && rel_has_token(rel, "prefetch")) {
            preload_add(out, seen, base, ns_element_get_attr(n, "href"),
                        NS_FETCH_DEST_DEFAULT);
        } else if (rel && (rel_has_token(rel, "preload") ||
                           rel_has_token(rel, "modulepreload"))) {
            preload_add(out, seen, base, ns_element_get_attr(n, "href"),
                        preload_destination_for(rel,
                                                ns_element_get_attr(n, "as")));
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        preload_collect(c, base, include_images, out, seen,
                        connect_out, connect_seen, depth + 1);
}

typedef struct {
    ns_fetch_destination dest;
    char *url;
    char *top_url;
} preload_request;

/* Preload completion: precompiles successful script bodies into the bytecode cache. */
static void
on_preload_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    ns_response *resp = ns_net_fetch_finish(res, NULL);
    preload_request *req = user_data;
    if (resp && req->dest == NS_FETCH_DEST_SCRIPT &&
        resp->status == 200 && resp->body)
        ns_bytecode_cache_precompile(req->top_url, req->url, resp->body->data,
                                     resp->body->len);
    g_free(req->top_url);
    g_free(req->url);
    g_free(req);
    if (resp) ns_response_free(resp);
}

/* Parses a CSP string and merges it into *csp, creating it if needed. */
static void
preload_add_csp(ns_csp **csp, const char *text)
{
    if (!text || !*text) return;
    ns_csp *parsed = ns_csp_parse(text);
    if (!parsed) return;
    if (!*csp) {
        *csp = parsed;
        return;
    }
    ns_csp_merge(*csp, parsed);
    ns_csp_free(parsed);
}

/* Merges <meta http-equiv=content-security-policy> policies outside iframes/templates. */
static void
preload_collect_meta_csp(const ns_node *n, ns_csp **csp, int depth)
{
    if (!n || depth > 256) return;
    if (ns_node_is_element_named(n, "iframe") ||
        ns_node_is_element_named(n, "template"))
        return;
    if (ns_node_is_element_named(n, "meta")) {
        const char *he = ns_element_get_attr(n, "http-equiv");
        if (he && g_ascii_strcasecmp(he, "content-security-policy") == 0)
            preload_add_csp(csp, ns_element_get_attr(n, "content"));
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        preload_collect_meta_csp(c, csp, depth + 1);
}

/* Starts speculative fetches for the document's subresources under the page's CSP. */
void
ns_engine_speculative_preload(ns_node *doc, const char *base_url,
                              const char *csp_header,
                              gboolean include_images)
{
    if (!doc || !base_url || !ns_url_is_http_or_https(base_url))
        return;
    /* Speculative loads are requests the page made: they get the page's
     * policy, which the scanner runs ahead of. */
    ns_csp *csp = NULL;
    preload_add_csp(&csp, csp_header);
    preload_collect_meta_csp(doc, &csp, 0);
    ns_fetch_policy *policy = ns_fetch_policy_new(base_url, csp);
    GPtrArray *urls = g_ptr_array_new_with_free_func(preload_target_free);
    GPtrArray *connects = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GHashTable *connect_seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
    g_autofree char *document_base = engine_document_base_url(doc, base_url);
    preload_collect(doc, document_base, include_images, urls, seen,
                    connects, connect_seen, 0);
    g_hash_table_destroy(seen);
    g_hash_table_destroy(connect_seen);
    ns_net_preload_clear();
    /* A preconnect is only a hint; under a policy it is not worth judging
     * which hosts the page may contact. */
    for (guint i = 0; !csp && i < connects->len; i++)
        ns_net_preconnect_async(g_ptr_array_index(connects, i));
    for (guint i = 0; i < urls->len; i++) {
        const preload_target *target = g_ptr_array_index(urls, i);
        if (ns_fetch_verdict_blocks(ns_fetch_policy_check(
                policy, target->dest == NS_FETCH_DEST_DEFAULT
                            ? NS_FETCH_DEST_CONNECT : target->dest,
                base_url, target->url, NULL)))
            continue;
        /* Scripts are judged by their element's nonce, which the scanner
         * does not have: under a policy, preload only a script its URL
         * admits, and keep it within script-src at every hop. */
        gboolean script = target->dest == NS_FETCH_DEST_SCRIPT;
        if (script && csp &&
            !ns_csp_allows(csp, NS_CSP_SCRIPT, target->url, base_url))
            continue;
        const char *const *headers = ns_net_accept_headers_for(target->dest);
        if (target->dest != NS_FETCH_DEST_DEFAULT) {
            char *key = ns_net_request_key(target->url, base_url, "GET", headers);
            ns_net_preload_expect(key);
            g_free(key);
        }
        preload_request *req = g_new0(preload_request, 1);
        req->dest = target->dest;
        req->url = g_strdup(target->url);
        req->top_url = g_strdup(base_url);
        ns_fetch_policy *req_policy = script && csp
            ? ns_fetch_policy_new_script_hops(policy)
            : ns_fetch_policy_ref(policy);
        ns_net_request_async(target->url, base_url, "GET", NULL, 0, NULL,
                             headers, target->dest, req_policy, NULL,
                             on_preload_fetched, req);
        ns_fetch_policy_unref(req_policy);
    }
    ns_fetch_policy_unref(policy);
    if (csp) ns_csp_free(csp);
    g_ptr_array_free(urls, TRUE);
    g_ptr_array_free(connects, TRUE);
}

/* Appends a sheet after recursively fetching its media-matching @imports; resolves URLs. */
static void
append_stylesheet_expanded(GPtrArray *out, ns_css_stylesheet *sh,
                           const char *base_url, const char *top_url,
                           GHashTable *seen, GHashTable *cache, int depth)
{
    if (!out || !sh) return;
    if (depth < NS_CSS_IMPORT_MAX_DEPTH && sh->imports) {
        for (guint i = 0; i < sh->imports->len; i++) {
            ns_css_import *im = &g_array_index(sh->imports, ns_css_import, i);
            if (!im->url || !*im->url) continue;
            if (im->media && *im->media &&
                !ns_css_media_query_matches(im->media))
                continue;
            char *abs = ns_url_resolve(base_url, im->url);
            if (!abs) continue;
            if (seen && g_hash_table_contains(seen, abs)) {
                g_free(abs);
                continue;
            }
            if (seen) g_hash_table_add(seen, g_strdup(abs));
            GBytes *bytes = fetch_css_bytes(abs, top_url, cache, TRUE, NULL);
            if (bytes) {
                ns_css_stylesheet *child =
                    ns_css_stylesheet_parse_import_cached(abs, im->layer_name,
                                                          bytes);
                if (child)
                    append_stylesheet_expanded(out, child, abs, top_url, seen,
                                               cache, depth + 1);
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    ns_css_stylesheet_resolve_urls(sh, base_url);
    g_ptr_array_add(out, sh);
}

typedef struct {
    GPtrArray  *out;
    GPtrArray  *out_docs;
    const ns_node *doc;
    GHashTable *cache;
    GString    *run;
    const char *run_base;
    const char *top_url;
    gboolean    strict_css_mime;
    gboolean    media_seen;
} sheet_collect_ctx;

/* Pads out_docs so each collected sheet records its owning document. */
static void
sheet_note_documents(sheet_collect_ctx *cc)
{
    if (!cc->out_docs) return;
    while (cc->out_docs->len < cc->out->len)
        g_ptr_array_add(cc->out_docs, (gpointer)cc->doc);
}

/* Parses the batched run of self-contained <style> text as one merged stylesheet. */
static void
sheet_run_flush(sheet_collect_ctx *cc)
{
    if (!cc->run || cc->run->len == 0) return;
    ns_css_stylesheet *sh =
        ns_css_merged_styles_cached(cc->run->str, (gssize)cc->run->len,
                                    cc->run_base);
    if (sh) {
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
        append_stylesheet_expanded(cc->out, sh, cc->run_base, cc->top_url, seen,
                                   cc->cache, 0);
        g_hash_table_destroy(seen);
    }
    g_string_set_size(cc->run, 0);
    cc->run_base = NULL;
}

/* Extracts a px width/height declaration from a frame's inline style attribute. */
static gboolean
frame_dimension_from_style_attr(const ns_node *frame, const char *prop,
                                double *out)
{
    const char *style = ns_element_get_attr(frame, "style");
    if (!style) return FALSE;
    gsize plen = strlen(prop);
    for (const char *p = style; (p = strstr(p, prop)) != NULL; p += plen) {
        if (p != style && (g_ascii_isalnum(p[-1]) || p[-1] == '-'))
            continue;
        const char *q = p + plen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        char *end = NULL;
        double v = g_ascii_strtod(q, &end);
        if (end && end != q && v >= 0 &&
            (g_ascii_strncasecmp(end, "px", 2) == 0 ||
             *end == ';' || *end == '\0' || *end == ' ')) {
            *out = v;
            return TRUE;
        }
    }
    return FALSE;
}

/* Parses a non-negative numeric frame width/height attribute. */
static gboolean
frame_dimension_from_attr(const ns_node *frame, const char *attr, double *out)
{
    const char *av = ns_element_get_attr(frame, attr);
    if (!av || !*av) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(av, &end);
    if (end == av || v < 0) return FALSE;
    *out = v;
    return TRUE;
}

typedef struct {
    double w, h;
} ns_collect_frame_vp;

/* Heuristic: true if any @media prelude mentions width, height, aspect-ratio or orientation. */
static gboolean
css_has_viewport_media(const char *css)
{
    if (!css) return FALSE;
    for (const char *p = css; (p = strstr(p, "@media")) != NULL; p += 6) {
        const char *end = strchr(p, '{');
        if (!end) break;
        for (const char *q = p; q < end; q++)
            if (g_ascii_strncasecmp(q, "width", 5) == 0 ||
                g_ascii_strncasecmp(q, "height", 6) == 0 ||
                g_ascii_strncasecmp(q, "aspect-ratio", 12) == 0 ||
                g_ascii_strncasecmp(q, "orientation", 11) == 0)
                return TRUE;
    }
    return FALSE;
}

static GHashTable *g_collect_frame_vp;
static GHashTable *g_viewport_media_memo;

/* Memoized css_has_viewport_media over stylesheet bytes (keyed by GBytes pointer). */
static gboolean
css_bytes_have_viewport_media(GBytes *bytes)
{
    if (!g_viewport_media_memo)
        g_viewport_media_memo =
            g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                  (GDestroyNotify)g_bytes_unref, NULL);
    gpointer memo = g_hash_table_lookup(g_viewport_media_memo, bytes);
    if (memo) return GPOINTER_TO_INT(memo) == 2;
    if (g_hash_table_size(g_viewport_media_memo) >= 256)
        g_hash_table_remove_all(g_viewport_media_memo);
    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);
    char *terminated = g_strndup(data, len);
    gboolean seen = css_has_viewport_media(terminated);
    g_free(terminated);
    g_hash_table_insert(g_viewport_media_memo, g_bytes_ref(bytes),
                        GINT_TO_POINTER(seen ? 2 : 1));
    return seen;
}

/* Frame viewport from inline style, layout, then attributes; defaults to 300x150. */
static void
frame_viewport_px(const ns_node *frame, double *w, double *h)
{
    gboolean have_w = frame_dimension_from_style_attr(frame, "width", w);
    gboolean have_h = frame_dimension_from_style_attr(frame, "height", h);
    if (!have_w || !have_h) {
        double lw = 0, lh = 0;
        if (ns_layout_frame_viewport(frame, &lw, &lh)) {
            if (!have_w) { *w = lw; have_w = TRUE; }
            if (!have_h) { *h = lh; have_h = TRUE; }
        }
    }
    if (!have_w && !frame_dimension_from_attr(frame, "width", w)) *w = 300;
    if (!have_h && !frame_dimension_from_attr(frame, "height", h)) *h = 150;
}

/* Like frame_viewport_px but yields 0x0 when either dimension is unknown. */
static void
frame_viewport_measured(const ns_node *frame, double *w, double *h)
{
    *w = 0;
    *h = 0;
    gboolean have_w = frame_dimension_from_style_attr(frame, "width", w);
    gboolean have_h = frame_dimension_from_style_attr(frame, "height", h);
    if (!have_w || !have_h) {
        double lw = 0, lh = 0;
        if (ns_layout_frame_viewport(frame, &lw, &lh)) {
            if (!have_w) { *w = lw; have_w = TRUE; }
            if (!have_h) { *h = lh; have_h = TRUE; }
        }
    }
    if (!have_w) have_w = frame_dimension_from_attr(frame, "width", w);
    if (!have_h) have_h = frame_dimension_from_attr(frame, "height", h);
    if (!have_w || !have_h) { *w = 0; *h = 0; }
}

/* Records the viewport size used to evaluate a frame's media queries. */
static void
frame_viewport_record(const ns_node *frame, double w, double h)
{
    if (!g_collect_frame_vp) return;
    ns_collect_frame_vp *e = g_new0(ns_collect_frame_vp, 1);
    e->w = w;
    e->h = h;
    g_hash_table_insert(g_collect_frame_vp, (gpointer)frame, e);
}

/* True if a recorded frame viewport differs from its laid-out size (needs re-cascade). */
static gboolean
frame_viewports_disagree_with_layout(void)
{
    if (!g_collect_frame_vp) return FALSE;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_collect_frame_vp);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const ns_collect_frame_vp *e = v;
        double lw = 0, lh = 0;
        if (!ns_layout_frame_viewport(k, &lw, &lh)) continue;
        if (fabs(e->w - lw) > 0.01 || fabs(e->h - lh) > 0.01) return TRUE;
    }
    return FALSE;
}

/* True if a style/link type attribute is empty or text/css (parameters ignored). */
static gboolean
sheet_type_is_css(const char *type)
{
    if (!type || !*type) return TRUE;
    gsize n = strcspn(type, ";");
    while (n > 0 && g_ascii_isspace(type[n - 1])) n--;
    return n == 8 && g_ascii_strncasecmp(type, "text/css", 8) == 0;
}

/* True if a <style> element is not disabled and has a CSS type. */
static gboolean
style_sheet_enabled(const ns_node *style)
{
    return !(style->flags & NS_NODE_SHEET_DISABLED) &&
           sheet_type_is_css(ns_element_get_attr(style, "type"));
}

/* True if a <link> lacks the disabled attribute and has a CSS type. */
static gboolean
link_sheet_enabled(const ns_node *link)
{
    return !ns_element_get_attr(link, "disabled") &&
           sheet_type_is_css(ns_element_get_attr(link, "type"));
}

/* Collects enabled <style>/<link> sheets in tree order, recursing into frames. */
static void
collect_stylesheets_walk(ns_node *n, const char *base_url,
                         sheet_collect_ctx *cc, int depth)
{
    if (!n || depth >= 512 ||
        (n->kind != NS_NODE_ELEMENT && n->kind != NS_NODE_DOCUMENT))
        return;
    const char *nm = n->kind == NS_NODE_ELEMENT && n->name ? n->name : "";
    if (nm[0] == 'n' && strcmp(nm, "noscript") == 0) return;
    gboolean is_style = nm[0] == 's' && strcmp(nm, "style") == 0;
    gboolean is_link = nm[0] == 'l' && strcmp(nm, "link") == 0;
    if ((nm[0] == 'i' && strcmp(nm, "iframe") == 0) ||
        (nm[0] == 'f' && strcmp(nm, "frame") == 0) ||
        (nm[0] == 'o' && strcmp(nm, "object") == 0)) {
        sheet_run_flush(cc);
        const char *furl = ns_element_get_attr(n, "data-nd-frame-url");
        if (furl && *furl) base_url = furl;
        gboolean has_doc = FALSE;
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_DOCUMENT) { has_doc = TRUE; break; }
        if (has_doc) {
            double fw, fh;
            frame_viewport_px(n, &fw, &fh);
            gboolean outer_media = cc->media_seen;
            cc->media_seen = FALSE;
            ns_css_media_viewport_push(fw, fh);
            sheet_note_documents(cc);
            const ns_node *outer_doc = cc->doc;
            for (ns_node *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind == NS_NODE_DOCUMENT) cc->doc = c;
                collect_stylesheets_walk(c, base_url, cc, depth + 1);
                sheet_run_flush(cc);
                sheet_note_documents(cc);
                cc->doc = outer_doc;
            }
            gboolean frame_media = cc->media_seen;
            ns_css_media_viewport_pop();
            cc->media_seen = outer_media || frame_media;
            if (frame_media) frame_viewport_record(n, fw, fh);
            return;
        }
    }
    GPtrArray *out = cc->out;
    GHashTable *cache = cc->cache;
    if (is_style && style_sheet_enabled(n)) {
        char *css = ns_css_style_element_text(n);
        if (css) {
            if (css_has_viewport_media(css)) cc->media_seen = TRUE;
            if (cc->run_base && cc->run_base != base_url)
                sheet_run_flush(cc);
            gboolean host_scoped =
                strstr(css, "[" NS_HOST_SCOPE_ATTR "=\"") != NULL;
            if (host_scoped || strstr(css, "@import") ||
                !ns_css_syntax_is_self_contained(css, strlen(css))) {
                sheet_run_flush(cc);
                ns_css_stylesheet *sh =
                    ns_css_stylesheet_from_style_text_cached(n, css);
                if (sh) {
                    GHashTable *seen =
                        g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
                    append_stylesheet_expanded(out, sh, base_url, cc->top_url,
                                               seen, cache, 0);
                    g_hash_table_destroy(seen);
                }
            } else {
                g_string_append(cc->run, css);
                g_string_append_c(cc->run, '\n');
                cc->run_base = base_url;
            }
            g_free(css);
        }
    } else if (is_link && base_url) {
        sheet_run_flush(cc);
        const char *rel = ns_element_get_attr(n, "rel");
        const char *href = ns_element_get_attr(n, "href");
        const char *media = ns_element_get_attr(n, "media");
        if (href && *href && rel_is_stylesheet(rel) &&
            link_sheet_enabled(n) &&
            (!media || !*media || ns_css_media_query_matches(media))) {
            char *abs = ns_url_resolve(base_url, href);
            gboolean cross_origin = FALSE;
            GBytes *bytes = fetch_css_bytes(abs, cc->top_url, cache,
                                            cc->strict_css_mime, &cross_origin);
            /* Subresource Integrity: a sheet whose bytes do not match the
             * link's integrity is not applied and not kept for CSSOM. */
            const char *integrity = ns_element_get_attr(n, "integrity");
            if (bytes && !ns_security_sri_check(integrity,
                                                g_bytes_get_data(bytes, NULL),
                                                g_bytes_get_size(bytes))) {
                ns_debug_log_emit(NS_DLOG_WARN, "sri",
                                  "SRI mismatch: stylesheet %s", abs);
                g_clear_pointer(&bytes, g_bytes_unref);
            }
            if (bytes) {
                gsize len = 0;
                const char *data = g_bytes_get_data(bytes, &len);
                /* Only same-origin sheets may be read back by page script
                 * (CSSOM); never remember a cross-origin body. */
                if (!cross_origin)
                    engine_remember_linked_css(abs, bytes);
                if (css_bytes_have_viewport_media(bytes)) cc->media_seen = TRUE;
                ns_css_stylesheet *sh =
                    ns_css_stylesheet_parse_url_cached(abs, data, (gssize)len);
                if (sh) {
                    GHashTable *seen =
                        g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
                    if (abs) g_hash_table_add(seen, g_strdup(abs));
                    append_stylesheet_expanded(out, sh, abs, cc->top_url, seen,
                                               cache, 0);
                    g_hash_table_destroy(seen);
                }
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_stylesheets_walk(c, base_url, cc, depth + 1);
}

/* Collects all document and frame stylesheets in cascade order, with owning documents. */
static void
ns_engine_collect_stylesheets(ns_node *doc, const char *base_url,
                              GPtrArray *out, GPtrArray *out_docs,
                              GHashTable *css_cache)
{
    g_autofree char *document_base = engine_document_base_url(doc, base_url);
    if (!g_collect_frame_vp)
        g_collect_frame_vp = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                   NULL, g_free);
    else
        g_hash_table_remove_all(g_collect_frame_vp);
    sheet_collect_ctx cc = {
        .out = out, .out_docs = out_docs, .doc = doc, .cache = css_cache,
        .run = g_string_new(NULL), .run_base = NULL,
        .top_url = base_url,
        .strict_css_mime = doc && !(doc->flags & NS_NODE_QUIRKS),
    };
    collect_stylesheets_walk(doc, document_base, &cc, 0);
    sheet_run_flush(&cc);
    sheet_note_documents(&cc);
    g_string_free(cc.run, TRUE);
}

/* Collects sheets, loads keyframes and computes styles; returns a new node->style map. */
GHashTable *
ns_engine_compute_cascade(ns_node *doc, const char *page_url,
                          GHashTable *css_cache, ns_anim *anim)
{
    g_autofree char *base_url = g_strdup(page_url);
    ns_css_set_frame_viewport_cb(frame_viewport_measured);
    g_autofree char *document_base = engine_document_base_url(doc, base_url);
    ns_css_relayout_enter();
    ns_css_set_doc_base(document_base);
    ns_css_style_element_cache_begin();
    GPtrArray *page_sheets = g_ptr_array_new();
    GPtrArray *sheet_docs = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, page_sheets, sheet_docs,
                                  css_cache);
    if (anim)
        for (guint i = 0; i < page_sheets->len; i++)
            ns_anim_load_from_stylesheet(anim, g_ptr_array_index(page_sheets, i));
    GHashTable *styles = ns_css_compute_scoped(doc,
        (const ns_css_stylesheet *const *)page_sheets->pdata,
        (const ns_node *const *)sheet_docs->pdata,
        page_sheets->len);
    for (guint i = 0; i < page_sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
    g_ptr_array_free(page_sheets, TRUE);
    g_ptr_array_free(sheet_docs, TRUE);
    ns_css_relayout_leave();
    return styles;
}

/* Runs the cascade while recording per-node style changes into changes. */
GHashTable *
ns_engine_compute_cascade_delta(ns_node *doc, const char *page_url,
                                GHashTable *css_cache, ns_anim *anim,
                                GPtrArray *changes)
{
    ns_css_compute_want_delta(changes);
    GHashTable *styles = ns_engine_compute_cascade(doc, page_url, css_cache,
                                                   anim);
    ns_css_compute_want_delta(NULL);
    return styles;
}

/* Full style+layout pass; redoes it when frame viewports used for media queries changed. */
GHashTable *
ns_engine_relayout(ns_node *doc, const char *page_url,
                   int viewport_width, double viewport_height,
                   ns_image_cache *images, ns_anim *anim,
                   ns_js *js, GHashTable *css_cache,
                   const ns_node *focused, const ns_node *hover,
                   gsize caret_byte,
                   gsize sel_anchor_byte, ns_box **out_layout)
{
    g_autofree char *base_url = g_strdup(page_url);
    ns_css_set_frame_viewport_cb(frame_viewport_measured);
    g_autofree char *document_base = engine_document_base_url(doc, base_url);
    ns_css_relayout_enter();
    ns_css_set_doc_base(document_base);
    ns_css_style_element_cache_begin();
    GPtrArray *sheets = g_ptr_array_new();
    GPtrArray *sheet_docs = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, sheets, sheet_docs, css_cache);

    ns_render_ctx rc = {
        .doc             = doc,
        .sheets          = (const ns_css_stylesheet *const *)sheets->pdata,
        .sheet_docs      = (const ns_node *const *)sheet_docs->pdata,
        .n_sheets        = sheets->len,
        .viewport_width  = (double)viewport_width,
        .viewport_height = viewport_height > 0 ? viewport_height
                                               : (double)viewport_width * 0.75,
        .zoom            = 1.0,
        .images          = images,
        .base_url        = base_url,
        .anim            = anim,
        .js              = js,
        .focused_input   = focused,
        .hover_node      = hover,
        .caret_byte      = caret_byte,
        .sel_anchor_byte = sel_anchor_byte,
    };
    static int profile_env = -1;
    if (profile_env < 0)
        profile_env = g_getenv("NS_PROFILE") != NULL ? 1 : 0;
    GHashTable *styles;
    gint64 relayout_t0 = g_get_monotonic_time();
    if (profile_env) {
        ns_render_profile prof;
        gint64 t0 = g_get_monotonic_time();
        styles = ns_render_relayout_profile(&rc, out_layout, &prof);
        gint64 total = g_get_monotonic_time() - t0;
        guint nstyles = styles ? g_hash_table_size(styles) : 0u;
        g_printerr("[profile] relayout vw=%d nodes=%u total=%.2fms "
                   "css=%.2f style=%.2f layout=%.2f",
                   viewport_width, nstyles, total / 1000.0,
                   prof.css1_us / 1000.0, prof.style1_us / 1000.0,
                   prof.layout1_us / 1000.0);
        if (prof.container_pass)
            g_printerr(" | containers=%u cq_collect=%.2f css2=%.2f style2=%.2f "
                       "layout2=%.2f",
                       prof.containers, prof.container_us / 1000.0,
                       prof.css2_us / 1000.0, prof.style2_us / 1000.0,
                       prof.layout2_us / 1000.0);
        g_printerr("\n");
    } else {
        styles = ns_render_relayout(&rc, out_layout);
    }
    if (frame_viewports_disagree_with_layout()) {
        if (js) {
            ns_js_set_layout_root(js, NULL);
            ns_js_set_style_table(js, NULL);
        }
        if (out_layout && *out_layout) {
            ns_box_free(*out_layout);
            *out_layout = NULL;
        }
        if (styles) g_hash_table_destroy(styles);
        for (guint i = 0; i < sheets->len; i++)
            ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
        g_ptr_array_set_size(sheets, 0);
        g_ptr_array_set_size(sheet_docs, 0);
        ns_css_style_element_cache_begin();
        ns_engine_collect_stylesheets(doc, base_url, sheets, sheet_docs,
                                      css_cache);
        rc.sheets     = (const ns_css_stylesheet *const *)sheets->pdata;
        rc.sheet_docs = (const ns_node *const *)sheet_docs->pdata;
        rc.n_sheets   = sheets->len;
        styles = ns_render_relayout(&rc, out_layout);
    }
    ns_engine_perf_add_relayout(g_get_monotonic_time() - relayout_t0);
    ns_debug_log_emit(NS_DLOG_RENDER, "relayout", "styles=%u vw=%d",
                      styles ? g_hash_table_size(styles) : 0u, viewport_width);

    for (guint i = 0; i < sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
    g_ptr_array_free(sheet_docs, TRUE);
    ns_css_relayout_leave();
    return styles;
}

/* Registers @keyframes from all of the document's stylesheets with the animation engine. */
void
ns_engine_load_keyframes(ns_anim *anim, ns_node *doc, const char *base_url,
                         GHashTable *css_cache)
{
    if (!anim) return;
    ns_css_relayout_enter();
    GPtrArray *sheets = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, sheets, NULL, css_cache);
    for (guint i = 0; i < sheets->len; i++) {
        const ns_css_stylesheet *sh = g_ptr_array_index(sheets, i);
        if (sh) ns_anim_load_from_stylesheet(anim, sh);
    }
    for (guint i = 0; i < sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
    ns_css_relayout_leave();
}

void
ns_engine_anim_observe(ns_anim *anim, GHashTable *styles, gint64 now_us)
{
    ns_anim_observe_all(anim, styles, now_us);
}

typedef struct {
    GMainLoop      *loop;
    int             pending;
    ns_image_cache *cache;
} imgs_fetch_state;

typedef struct {
    imgs_fetch_state *st;
    char             *abs;
} img_fetch_item;

/* Records final URL and CORS allow-origin on a newly cached image (first time only). */
static void
img_record_response(ns_image *img, const char *final_url,
                    const char *cors_allow_origin)
{
    if (!img || img->final_url) return;
    img->final_url = g_strdup(final_url);
    img->cors_allow_origin = g_strdup(cors_allow_origin);
}

/* Blocking image fetch callback: caches the body and quits the loop when all are done. */
static void
on_image_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    img_fetch_item *it = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (resp && !resp->error && resp->body && resp->body->len > 0) {
        img_record_response(
            ns_image_cache_insert_encoded(it->st->cache, it->abs,
                                          resp->body->data, resp->body->len),
            resp->final_url, resp->cors_allow_origin);
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    if (--it->st->pending == 0)
        g_main_loop_quit(it->st->loop);
    g_free(it->abs);
    g_free(it);
}

#define NS_LAZY_IMAGE_MARGIN_PX 4000.0

/* Returns the nearest enclosing frame's document URL, else dflt. */
static const char *
engine_node_frame_base(const ns_node *n, const char *dflt)
{
    for (const ns_node *p = n ? n->parent : NULL; p; p = p->parent) {
        if (ns_node_is_element_named(p, "iframe") ||
            ns_node_is_element_named(p, "frame") ||
            ns_node_is_element_named(p, "object")) {
            const char *fu = ns_element_get_attr(p, "data-nd-frame-url");
            if (fu && *fu) return fu;
        }
    }
    return dflt;
}

/* True for an image box whose element has loading=lazy. */
static gboolean
engine_image_is_lazy(const ns_box *box)
{
    if (!box || box->kind != NS_BOX_IMAGE || !box->dom) return FALSE;
    const char *l = ns_element_get_attr(box->dom, "loading");
    return l && g_ascii_strcasecmp(l, "lazy") == 0;
}

typedef struct {
    ns_fetch_destination dest;
    ns_fetch_policy     *policy;
} wanted_image;

static void
wanted_image_free(gpointer data)
{
    wanted_image *w = data;
    ns_fetch_policy_unref(w->policy);
    g_free(w);
}

/* Maps uncached image/video URLs in the layout to fetch info; defers far lazy images. */
static GHashTable *
engine_collect_wanted_images(ns_box *root, const char *base_url,
                             ns_image_cache *cache, ns_js *js,
                             double scroll_y, double viewport_h,
                             gboolean *deferred_any)
{
    GPtrArray *imgs = g_ptr_array_new();
    ns_layout_collect_images(root, imgs);
    ns_layout_collect_videos(root, imgs);
    GHashTable *wanted = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, wanted_image_free);
    double lazy_limit = (viewport_h > 0.0)
        ? scroll_y + viewport_h + NS_LAZY_IMAGE_MARGIN_PX : G_MAXDOUBLE;
    for (guint i = 0; i < imgs->len; i++) {
        ns_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        if (box->y > lazy_limit && engine_image_is_lazy(box)) {
            if (deferred_any) *deferred_any = TRUE;
            continue;
        }
        GPtrArray *srcs = g_ptr_array_new();
        if (box->media->image_src)
            g_ptr_array_add(srcs, box->media->image_src);
        else if (box->media->bg_layer_srcs) {
            for (guint li = 0; li < box->media->bg_layer_srcs->len; li++) {
                char *lsrc = g_ptr_array_index(box->media->bg_layer_srcs, li);
                if (lsrc) g_ptr_array_add(srcs, lsrc);
            }
        } else if (box->media->bg_image_src)
            g_ptr_array_add(srcs, box->media->bg_image_src);
        if (box->media->marker_image_src)
            g_ptr_array_add(srcs, box->media->marker_image_src);
        if (box->media->border_image_src)
            g_ptr_array_add(srcs, box->media->border_image_src);
        if (box->media->video_src)
            g_ptr_array_add(srcs, box->media->video_src);
        const char *box_base = engine_node_frame_base(box->dom, base_url);
        for (guint si = 0; si < srcs->len; si++) {
            const char *src = g_ptr_array_index(srcs, si);
            if (g_str_has_prefix(src, "nd-inline-svg:")) continue;
            char *abs = ns_url_resolve(box_base, src);
            if (!abs) continue;
            if (ns_image_cache_peek(cache, abs) ||
                g_hash_table_contains(wanted, abs)) {
                g_free(abs);
                continue;
            }
            wanted_image *w = g_new0(wanted_image, 1);
            w->dest = src == box->media->video_src ? NS_FETCH_DEST_MEDIA
                                                    : NS_FETCH_DEST_IMAGE;
            w->policy = ns_fetch_policy_ref(ns_js_fetch_policy(js, box->dom));
            g_hash_table_insert(wanted, abs, w);
        }
        g_ptr_array_free(srcs, TRUE);
    }
    g_ptr_array_free(imgs, TRUE);
    return wanted;
}

struct ns_engine_img_session {
    int             refs;
    gboolean        dead;
    int             outstanding;
    ns_image_cache *cache;
    void          (*arrived_cb)(gpointer user_data);
    gpointer        user_data;
};

/* Drops a reference on an image fetch session, freeing it at zero. */
static void
img_session_unref(ns_engine_img_session *s)
{
    if (--s->refs == 0)
        g_free(s);
}

typedef struct img_async_item {
    ns_engine_img_session *session;
    char                  *abs;
    char                  *final_url;
    char                  *cors_allow_origin;
} img_async_item;

/* Completes one async image request: notifies the session unless closed, frees the item. */
static void
img_async_item_finish(img_async_item *it)
{
    ns_engine_img_session *s = it->session;
    if (s->outstanding > 0) s->outstanding--;
    if (!s->dead && s->arrived_cb)
        s->arrived_cb(s->user_data);
    img_session_unref(s);
    g_free(it->abs);
    g_free(it->final_url);
    g_free(it->cors_allow_origin);
    g_free(it);
}

/* GTask worker that decodes encoded image bytes off the main thread. */
static void
image_decode_in_worker(GTask *task, gpointer source, gpointer task_data,
                       GCancellable *cancellable)
{
    (void)source;
    (void)cancellable;
    gsize len = 0;
    const guchar *data = g_bytes_get_data(task_data, &len);
    gint64 trace_start = ns_trace_now();
    ns_image_decoding *decoding = ns_image_decode_encoded(data, len);
    ns_trace_complete("decode", "decode image", trace_start, NULL);
    g_task_return_pointer(task, decoding,
                          (GDestroyNotify)ns_image_decoding_free);
}

/* Inserts a worker-decoded image into the cache unless the session was closed. */
static void
on_image_decoded_async(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    img_async_item *it = user_data;
    ns_engine_img_session *s = it->session;
    ns_image_decoding *decoding =
        g_task_propagate_pointer(G_TASK(result), NULL);
    if (s->dead)
        ns_image_decoding_free(decoding);
    else
        img_record_response(
            ns_image_cache_insert_decoding(s->cache, it->abs, decoding),
            it->final_url, it->cors_allow_origin);
    img_async_item_finish(it);
}

/* Async image fetch callback: decodes in a worker if enabled, else caches inline. */
static void
on_image_fetch_async_done(GObject *src, GAsyncResult *result,
                          gpointer user_data)
{
    (void)src;
    img_async_item *it = user_data;
    ns_engine_img_session *s = it->session;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    g_clear_error(&err);
    gboolean usable = !s->dead && resp && !resp->error && resp->body &&
                      resp->body->len > 0;
    if (usable && ns_config_get()->async_image_decode) {
        GBytes *body = g_bytes_new(resp->body->data, resp->body->len);
        it->final_url = g_strdup(resp->final_url);
        it->cors_allow_origin = g_strdup(resp->cors_allow_origin);
        ns_response_free(resp);
        GTask *task = g_task_new(NULL, NULL, on_image_decoded_async, it);
        g_task_set_task_data(task, body, (GDestroyNotify)g_bytes_unref);
        g_task_run_in_thread(task, image_decode_in_worker);
        g_object_unref(task);
        return;
    }
    if (usable)
        img_record_response(
            ns_image_cache_insert_encoded(s->cache, it->abs,
                                          resp->body->data, resp->body->len),
            resp->final_url, resp->cors_allow_origin);
    if (resp) ns_response_free(resp);
    img_async_item_finish(it);
}

/* Starts async fetches for the layout's wanted images not yet requested; NULL if none. */
ns_engine_img_session *
ns_engine_fetch_images_start(ns_box *root, const char *base_url,
                             ns_image_cache *cache, ns_js *js,
                             GHashTable *requested,
                             double scroll_y, double viewport_h,
                             gboolean *deferred_any,
                             void (*arrived_cb)(gpointer user_data),
                             gpointer user_data)
{
    if (!root || !base_url || !cache) return NULL;
    GHashTable *wanted = engine_collect_wanted_images(root, base_url, cache,
                                                      js, scroll_y, viewport_h,
                                                      deferred_any);
    if (requested) {
        GHashTableIter rit;
        gpointer rkey;
        g_hash_table_iter_init(&rit, wanted);
        while (g_hash_table_iter_next(&rit, &rkey, NULL))
            if (g_hash_table_contains(requested, rkey))
                g_hash_table_iter_remove(&rit);
        g_hash_table_iter_init(&rit, wanted);
        while (g_hash_table_iter_next(&rit, &rkey, NULL))
            g_hash_table_add(requested, g_strdup(rkey));
    }
    guint n = g_hash_table_size(wanted);
    if (n == 0) {
        g_hash_table_destroy(wanted);
        return NULL;
    }

    ns_engine_img_session *s = g_new0(ns_engine_img_session, 1);
    s->refs = 1;
    s->outstanding = (int)n;
    s->cache = cache;
    s->arrived_cb = arrived_cb;
    s->user_data = user_data;

    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, wanted);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const wanted_image *w = value;
        img_async_item *item = g_new0(img_async_item, 1);
        item->session = s;
        item->abs = g_strdup(key);
        s->refs++;
        ns_net_request_async(
            item->abs, base_url, "GET", NULL, 0, NULL,
            ns_net_accept_headers_for(NS_FETCH_DEST_IMAGE), w->dest,
            w->policy, NULL, on_image_fetch_async_done, item);
    }
    g_hash_table_destroy(wanted);
    return s;
}

int
ns_engine_img_session_outstanding(const ns_engine_img_session *s)
{
    return s ? s->outstanding : 0;
}

/* Marks a session dead so late arrivals are ignored, and drops the caller's reference. */
void
ns_engine_img_session_close(ns_engine_img_session *s)
{
    if (!s) return;
    s->dead = TRUE;
    s->arrived_cb = NULL;
    img_session_unref(s);
}

/* Fetches all wanted images for the layout synchronously into the cache. */
void
ns_engine_fetch_images(ns_box *root, const char *base_url,
                       ns_image_cache *cache, ns_js *js)
{
    if (!root || !base_url || !cache) return;
    GHashTable *wanted = engine_collect_wanted_images(root, base_url, cache,
                                                      js, 0.0, 0.0, NULL);

    guint n = g_hash_table_size(wanted);
    if (n == 0) {
        g_hash_table_destroy(wanted);
        return;
    }

    imgs_fetch_state st = {0};
    st.loop = engine_blocking_wait_begin();
    st.pending = (int)n;
    st.cache = cache;

    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, wanted);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const wanted_image *w = value;
        img_fetch_item *item = g_new0(img_fetch_item, 1);
        item->st = &st;
        item->abs = g_strdup(key);
        ns_net_request_async(
            item->abs, base_url, "GET", NULL, 0, NULL,
            ns_net_accept_headers_for(NS_FETCH_DEST_IMAGE), w->dest,
            w->policy, NULL, on_image_fetch_done, item);
    }
    g_main_loop_run(st.loop);
    engine_blocking_wait_end(st.loop);
    g_hash_table_destroy(wanted);
}

static GHashTable *g_preopened_outputs;  /* path -> FILE* */

/* Opens an output file early (e.g. before sandboxing) for later PNG/PDF writes. */
gboolean
ns_engine_preopen_output(const char *path)
{
    if (!path || !*path) return FALSE;
    if (!g_preopened_outputs)
        g_preopened_outputs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    g_free, NULL);
    if (g_hash_table_contains(g_preopened_outputs, path)) return TRUE;
    FILE *f = g_fopen(path, "wb");
    if (!f) return FALSE;
    g_hash_table_insert(g_preopened_outputs, g_strdup(path), f);
    return TRUE;
}

/* The pre-opened file for path, handed over to the caller to close. */
static FILE *
engine_take_preopened(const char *path)
{
    if (!g_preopened_outputs || !path) return NULL;
    gpointer key = NULL, f = NULL;
    if (!g_hash_table_lookup_extended(g_preopened_outputs, path, &key, &f))
        return NULL;
    g_hash_table_steal(g_preopened_outputs, path);
    g_free(key);
    return f;
}

/* cairo write callback that fwrites to the FILE* closure. */
static cairo_status_t
engine_write_to_file(void *closure, const unsigned char *data,
                     unsigned int length)
{
    return fwrite(data, 1, length, closure) == length
        ? CAIRO_STATUS_SUCCESS : CAIRO_STATUS_WRITE_ERROR;
}

/* Creates a PDF surface on the pre-opened file for path if any, else by path. */
static cairo_surface_t *
engine_pdf_surface_create(const char *path, double w, double h, FILE **out)
{
    *out = engine_take_preopened(path);
    return *out ? cairo_pdf_surface_create_for_stream(engine_write_to_file,
                                                      *out, w, h)
                : cairo_pdf_surface_create(path, w, h);
}

/* Paints the layout to a PNG capped at cairo's 30000 px limit; returns 0 or 2. */
int
ns_engine_write_png(const ns_box *root, const char *path)
{
    if (!root || !path) return 2;
    const int kCairoMax = 30000;
    double cw = root->content_width;
    if (!(cw > 0)) cw = 1024;
    if (cw > kCairoMax) cw = kCairoMax;
    int w = (int)cw;
    double max_bottom = ns_box_max_bottom(root, root->content_height);
    if (!(max_bottom > 0)) max_bottom = 0;
    if (max_bottom > (double)kCairoMax) max_bottom = kCairoMax;
    int h = (int)max_bottom + 32;
    if (h <= 0) h = 768;
    if (w > kCairoMax) w = kCairoMax;
    if (h > kCairoMax) {
        fprintf(stderr,
            "engine: page is %d px tall; PNG capped at %d (cairo limit)\n",
            h, kCairoMax);
        h = kCairoMax;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "engine: failed to create PNG surface\n");
        return 2;
    }
    cairo_t *cr = cairo_create(surf);
    ns_paint(cr, root, NULL);
    cairo_destroy(cr);
    FILE *f = engine_take_preopened(path);
    cairo_status_t st = f
        ? cairo_surface_write_to_png_stream(surf, engine_write_to_file, f)
        : cairo_surface_write_to_png(surf, path);
    if (f && fclose(f) != 0 && st == CAIRO_STATUS_SUCCESS)
        st = CAIRO_STATUS_WRITE_ERROR;
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "engine: PNG write failed: %s\n",
                cairo_status_to_string(st));
        return 2;
    }
    return 0;
}

/* Sets the PDF creator and UTC creation date metadata. */
static void
pdf_set_metadata(cairo_surface_t *surf)
{
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATOR,
                                   "Northstar Web Browser");
    time_t now = time(NULL);
    struct tm tm_utc;
#if defined(_WIN32)
    int have_tm = gmtime_s(&tm_utc, &now) == 0;
#else
    int have_tm = gmtime_r(&now, &tm_utc) != NULL;
#endif
    if (have_tm) {
        char iso[32];
        if (strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0)
            cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATE_DATE,
                                           iso);
    }
}

/* Paints the layout as a single-page PDF sized to the content; returns 0 or 2. */
int
ns_engine_write_pdf(const ns_box *root, const char *path)
{
    if (!root || !path) return 2;
    double w = root->content_width > 0 ? root->content_width : 595.0;
    double h = root->content_height > 0 ? (root->content_height + 32) : 842.0;
    FILE *f = NULL;
    cairo_surface_t *surf = engine_pdf_surface_create(path, w, h, &f);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        if (f) fclose(f);
        fprintf(stderr, "engine: failed to create PDF surface\n");
        return 2;
    }
    pdf_set_metadata(surf);
    cairo_t *cr = cairo_create(surf);
    ns_paint(cr, root, NULL);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    if (f) fclose(f);
    return 0;
}

#define NS_PRINT_PT_PER_PX (72.0 / 96.0)

/* Writes a paginated PDF using the print setup's page size and margins. */
int
ns_engine_write_pdf_paged(const ns_box *root, const char *path,
                          const ns_print_setup *setup)
{
    if (!root || !path || !setup) return 2;
    double page_h = setup->height - setup->margin_top - setup->margin_bottom;
    GArray *offsets = ns_print_page_offsets(root, page_h);
    FILE *f = NULL;
    cairo_surface_t *surf = engine_pdf_surface_create(
        path, setup->width * NS_PRINT_PT_PER_PX,
        setup->height * NS_PRINT_PT_PER_PX, &f);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        if (f) fclose(f);
        g_array_free(offsets, TRUE);
        fprintf(stderr, "engine: failed to create PDF surface\n");
        return 2;
    }
    pdf_set_metadata(surf);
    cairo_t *cr = cairo_create(surf);
    for (guint i = 0; i < offsets->len; i++) {
        double top = g_array_index(offsets, double, i);
        ns_print_draw_page(cr, root, setup, NS_PRINT_PT_PER_PX, top,
                           ns_print_page_bottom(offsets, i, page_h));
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    if (f) fclose(f);
    g_array_free(offsets, TRUE);
    return 0;
}

/* Renders each print page to a recording surface; caller owns the array and surfaces. */
GPtrArray *
ns_engine_print_recordings(const ns_box *root, const ns_print_setup *setup)
{
    if (!root || !setup) return NULL;
    double page_h = setup->height - setup->margin_top - setup->margin_bottom;
    GArray *offsets = ns_print_page_offsets(root, page_h);
    GPtrArray *pages = g_ptr_array_new();
    cairo_rectangle_t extent = { 0, 0, setup->width, setup->height };
    for (guint i = 0; i < offsets->len; i++) {
        double top = g_array_index(offsets, double, i);
        cairo_surface_t *rec =
            cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &extent);
        cairo_t *cr = cairo_create(rec);
        ns_print_draw_page(cr, root, setup, 1.0, top,
                           ns_print_page_bottom(offsets, i, page_h));
        cairo_destroy(cr);
        g_ptr_array_add(pages, rec);
    }
    g_array_free(offsets, TRUE);
    return pages;
}

/* Appends the layout's text runs and image alt/src placeholders, one per line. */
void
ns_engine_dump_text(const ns_box *b, GString *out)
{
    if (!b) return;
    if (b->kind == NS_BOX_INLINE && b->text && *b->text) {
        g_string_append(out, b->text);
        g_string_append_c(out, '\n');
    } else if (b->kind == NS_BOX_IMAGE && b->dom) {
        const char *alt = ns_element_get_attr(b->dom, "alt");
        const char *src = b->media ? b->media->image_src : NULL;
        if (alt && *alt) g_string_append_printf(out, "[image: %s]\n", alt);
        else if (src)    g_string_append_printf(out, "[image: %s]\n", src);
        else             g_string_append(out, "[image]\n");
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        ns_engine_dump_text(c, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            ns_engine_dump_text(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, out);
}

/* Appends an indented debug dump of the box tree with geometry, element and text. */
void
ns_engine_dump_layout(const ns_box *b, int indent, GString *out)
{
    if (!b) return;
    for (int i = 0; i < indent; i++) g_string_append_c(out, ' ');
    g_string_append_printf(out, "%s @(%.0f,%.0f) %.0fx%.0f",
        ns_box_kind_name(b->kind), b->x, b->y,
        b->content_width, b->content_height);
    if (b->dom && b->dom->name) {
        const char *id = b->dom->kind == NS_NODE_ELEMENT
                       ? ns_element_get_attr(b->dom, "id") : NULL;
        if (id && *id)
            g_string_append_printf(out, " <%s#%s>", b->dom->name, id);
        else
            g_string_append_printf(out, " <%s>", b->dom->name);
    }
    if (b->media && b->media->image_src)
        g_string_append_printf(out, " img=%s", b->media->image_src);
    if (b->text && *b->text) {
        gsize n = strlen(b->text);
        if (n > 40) {
            g_string_append_printf(out, " text=\"%.40s…\"", b->text);
        } else {
            g_string_append_printf(out, " text=\"%s\"", b->text);
        }
    }
    g_string_append_c(out, '\n');
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        ns_engine_dump_layout(c, indent + 2, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            ns_engine_dump_layout(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                indent + 2, out);
}

/* Inserts suffix before the file extension of path; caller frees. */
char *
ns_engine_suffix_before_ext(const char *path, const char *suffix)
{
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    const char *back  = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash))
        return g_strdup_printf("%.*s%s%s",
                               (int)(dot - path), path, suffix, dot);
    return g_strconcat(path, suffix, NULL);
}
