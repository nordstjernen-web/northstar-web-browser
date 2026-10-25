/* Northstar Web Browser — fetch/cascade/layout/capture pipeline shared by the browser drivers.
 * Blocking fetch, cascade and relayout entry points, image fetch sessions, PNG/PDF/text dumps.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_ENGINE_H
#define NS_ENGINE_H

#include <glib.h>

#include "anim.h"
#include "dom.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"

G_BEGIN_DECLS

struct ns_print_setup;

ns_response *ns_engine_fetch_blocking(const char *url, const char *top_url,
                                      GError **error);

gboolean ns_engine_in_blocking_fetch(void);

ns_response *ns_engine_post_blocking(const char *url, const char *top_url,
                                     const void *body, gsize body_len,
                                     const char *content_type, GError **error);

char *ns_engine_linked_css_text(const char *url);

/* csp_header is the response's Content-Security-Policy (or NULL); the
 * document's <meta http-equiv> policies are added to it. */
void ns_engine_speculative_preload(ns_node *doc, const char *base_url,
                                   const char *csp_header,
                                   gboolean include_images);

GHashTable *ns_engine_compute_cascade(ns_node *doc, const char *base_url,
                                      GHashTable *css_cache, ns_anim *anim);
GHashTable *ns_engine_compute_cascade_delta(ns_node *doc, const char *base_url,
                                            GHashTable *css_cache,
                                            ns_anim *anim, GPtrArray *changes);

const char *ns_engine_document_base_href(const ns_node *doc);
GHashTable *ns_engine_relayout(ns_node *doc, const char *base_url,
                               int viewport_width, double viewport_height,
                               ns_image_cache *images, ns_anim *anim,
                               ns_js *js, GHashTable *css_cache,
                               const ns_node *focused, const ns_node *hover,
                               gsize caret_byte,
                               gsize sel_anchor_byte, ns_box **out_layout);

void ns_engine_layout_perf(guint64 *relayouts, double *total_ms);

void ns_engine_load_keyframes(ns_anim *anim, ns_node *doc, const char *base_url,
                              GHashTable *css_cache);

void ns_engine_anim_observe(ns_anim *anim, GHashTable *styles, gint64 now_us);

void ns_engine_fetch_images(ns_box *root, const char *base_url,
                            ns_image_cache *cache, ns_js *js);

typedef struct ns_engine_img_session ns_engine_img_session;

ns_engine_img_session *ns_engine_fetch_images_start(
    ns_box *root, const char *base_url, ns_image_cache *cache, ns_js *js,
    GHashTable *requested, double scroll_y, double viewport_h,
    gboolean *deferred_any,
    void (*arrived_cb)(gpointer user_data), gpointer user_data);
int  ns_engine_img_session_outstanding(const ns_engine_img_session *s);
void ns_engine_img_session_close(ns_engine_img_session *s);

/* Open an output file named on the command line (--dump=png:, pdf:,
 * print:, --screenshot=) before the sandbox is set up, so the sandbox need
 * not grant write access to its whole directory; the writers below use
 * the open file when given that path. */
gboolean ns_engine_preopen_output(const char *path);
int ns_engine_write_png(const ns_box *root, const char *path);
int ns_engine_write_pdf(const ns_box *root, const char *path);
int ns_engine_write_pdf_paged(const ns_box *root, const char *path,
                              const struct ns_print_setup *setup);

GPtrArray *ns_engine_print_recordings(const ns_box *root,
                                      const struct ns_print_setup *setup);

void ns_engine_dump_text(const ns_box *root, GString *out);
void ns_engine_dump_layout(const ns_box *root, int indent, GString *out);

char *ns_engine_suffix_before_ext(const char *path, const char *suffix);

G_END_DECLS

#endif
