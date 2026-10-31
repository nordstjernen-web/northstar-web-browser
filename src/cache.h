/* Northstar Web Browser — HTTP cache API: lookup, freshness check, store and 304 promotion.
 * Entries are keyed by URL plus partition key and matched against the request's Vary headers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_CACHE_H
#define NS_CACHE_H

#include <glib.h>

G_BEGIN_DECLS

/* Response headers that carry security policy. A cache hit must restore them,
 * or a revisit would drop the page's CSP, framing and nosniff rules. */
typedef struct ns_cache_policy_headers {
    char *csp_header;
    char *xframe_options;
    char *x_content_type_options;
    char *content_disposition;
    char *refresh;
    char *content_language;
} ns_cache_policy_headers;

typedef struct ns_cache_entry {
    char       *final_url;
    long        status;
    char       *content_type;
    char       *cors_allow_origin;
    char       *etag;
    char       *last_modified;
    ns_cache_policy_headers policy;
    gint64      expires_at;
    gint64      fetched_at;
    GByteArray *body;
} ns_cache_entry;

void   ns_cache_init(void);
void   ns_cache_shutdown(void);
void   ns_cache_clear(void);

ns_cache_entry *ns_cache_get(const char *url, const char *partition,
                             const char *const *request_headers);
gboolean        ns_cache_is_fresh(const ns_cache_entry *e);
void   ns_cache_entry_free(ns_cache_entry *e);

void   ns_cache_put(const char *url,
                    const char *partition,
                    const char *final_url,
                    long status,
                    const char *content_type,
                    const char *cors_allow_origin,
                    const ns_cache_policy_headers *policy,
                    const char *etag,
                    const char *last_modified,
                    const char *cache_control,
                    const char *expires_header,
                    const char *vary,
                    const char *const *request_headers,
                    const void *body, gsize body_len);

void   ns_cache_promote_304(const char *url,
                            const char *partition,
                            const char *const *request_headers,
                            const char *cache_control,
                            const char *expires_header);


G_END_DECLS

#endif
