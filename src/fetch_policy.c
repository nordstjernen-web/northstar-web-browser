/* Northstar Web Browser — the policy check every subresource request passes.
 * file: access, mixed-content blocking or upgrade, and per-destination CSP directives.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fetch_policy.h"

#include <string.h>

#include "net.h"

struct ns_fetch_policy {
    gatomicrefcount rc;
    char           *document_url;
    char           *top_url;
    ns_csp         *csp;
    gboolean        check_scripts;
};

/* Creates a refcounted fetch policy holding a copy of the document URL and CSP. */
ns_fetch_policy *
ns_fetch_policy_new(const char *document_url, const ns_csp *csp)
{
    ns_fetch_policy *policy = g_new0(ns_fetch_policy, 1);
    g_atomic_ref_count_init(&policy->rc);
    policy->document_url = g_strdup(document_url);
    policy->csp = csp ? ns_csp_copy(csp) : NULL;
    return policy;
}

/* Copy of a policy that also applies script-src to script fetches (e.g. redirect hops). */
ns_fetch_policy *
ns_fetch_policy_new_script_hops(const ns_fetch_policy *policy)
{
    if (!policy) return NULL;
    ns_fetch_policy *copy = ns_fetch_policy_new(policy->document_url,
                                                policy->csp);
    copy->top_url = g_strdup(policy->top_url);
    copy->check_scripts = TRUE;
    return copy;
}

void
ns_fetch_policy_set_top_url(ns_fetch_policy *policy, const char *top_url)
{
    if (!policy) return;
    g_free(policy->top_url);
    policy->top_url = top_url && *top_url ? g_strdup(top_url) : NULL;
}

const char *
ns_fetch_policy_top_url(const ns_fetch_policy *policy)
{
    return policy ? policy->top_url : NULL;
}

ns_fetch_policy *
ns_fetch_policy_ref(ns_fetch_policy *policy)
{
    if (policy) g_atomic_ref_count_inc(&policy->rc);
    return policy;
}

void
ns_fetch_policy_unref(ns_fetch_policy *policy)
{
    if (!policy || !g_atomic_ref_count_dec(&policy->rc)) return;
    g_free(policy->document_url);
    g_free(policy->top_url);
    ns_csp_free(policy->csp);
    g_free(policy);
}

const char *
ns_fetch_policy_document_url(const ns_fetch_policy *policy)
{
    return policy ? policy->document_url : NULL;
}

/* Human-readable destination name used in console messages. */
static const char *
ns_fetch_destination_name(ns_fetch_destination dest)
{
    switch (dest) {
    case NS_FETCH_DEST_SCRIPT:   return "script";
    case NS_FETCH_DEST_STYLE:    return "stylesheet";
    case NS_FETCH_DEST_IMAGE:    return "image";
    case NS_FETCH_DEST_FONT:     return "font";
    case NS_FETCH_DEST_MEDIA:    return "media";
    case NS_FETCH_DEST_FRAME:    return "frame";
    case NS_FETCH_DEST_WORKER:   return "worker";
    case NS_FETCH_DEST_CONNECT:  return "connection";
    case NS_FETCH_DEST_DOCUMENT: return "document";
    case NS_FETCH_DEST_OBJECT:   return "object";
    default:                     return "resource";
    }
}

/* CSP directive kind checked for a fetch destination; scripts are checked by the caller. */
static ns_csp_kind
destination_csp_kind(ns_fetch_destination dest)
{
    /* Scripts are left to the caller, which knows the element's nonce
     * and re-checks the final URL after redirects. */
    switch (dest) {
    case NS_FETCH_DEST_STYLE:   return NS_CSP_STYLE;
    case NS_FETCH_DEST_IMAGE:   return NS_CSP_IMG;
    case NS_FETCH_DEST_MEDIA:   return NS_CSP_MEDIA;
    case NS_FETCH_DEST_FONT:    return NS_CSP_FONT;
    case NS_FETCH_DEST_CONNECT: return NS_CSP_CONNECT;
    case NS_FETCH_DEST_WORKER:  return NS_CSP_WORKER;
    case NS_FETCH_DEST_FRAME:   return NS_CSP_FRAME;
    case NS_FETCH_DEST_OBJECT:  return NS_CSP_OBJECT;
    default:                    return NS_CSP_KIND_COUNT;
    }
}

/* CSP directive name reported for a blocked fetch destination. */
static const char *
destination_csp_directive(ns_fetch_destination dest)
{
    switch (dest) {
    case NS_FETCH_DEST_STYLE:   return "style-src";
    case NS_FETCH_DEST_IMAGE:   return "img-src";
    case NS_FETCH_DEST_MEDIA:   return "media-src";
    case NS_FETCH_DEST_FONT:    return "font-src";
    case NS_FETCH_DEST_CONNECT: return "connect-src";
    case NS_FETCH_DEST_WORKER:  return "worker-src";
    case NS_FETCH_DEST_FRAME:   return "frame-src";
    case NS_FETCH_DEST_OBJECT:  return "object-src";
    case NS_FETCH_DEST_SCRIPT:  return "script-src";
    default:                    return "default-src";
    }
}

/* True if url starts with scheme (ASCII case-insensitive). */
static gboolean
url_has_scheme(const char *url, const char *scheme)
{
    return url && g_ascii_strncasecmp(url, scheme, strlen(scheme)) == 0;
}

/* True if the URL's host is a loopback address. */
static gboolean
url_is_loopback(const char *url)
{
    char *host = ns_url_host_from(url);
    gboolean loopback = host && ns_url_host_is_loopback(host);
    g_free(host);
    return loopback;
}

/* Rewrites an http: URL to https:, dropping an explicit :80 port; caller frees. */
static char *
upgrade_to_https(const char *url)
{
    const char *rest = url + strlen("http://");
    gsize authority = strcspn(rest, "/?#");
    if (authority > 3 && strncmp(rest + authority - 3, ":80", 3) == 0) {
        char *host = g_strndup(rest, authority - 3);
        char *out = g_strconcat("https://", host, rest + authority, NULL);
        g_free(host);
        return out;
    }
    return g_strconcat("https://", rest, NULL);
}

/* Checks a subresource fetch for file: access, mixed content (upgrading media) and CSP. */
ns_fetch_verdict
ns_fetch_policy_check(const ns_fetch_policy *policy, ns_fetch_destination dest,
                      const char *document_url, const char *url,
                      char **out_upgraded_url)
{
    if (out_upgraded_url) *out_upgraded_url = NULL;
    if (!url || dest == NS_FETCH_DEST_DOCUMENT) return NS_FETCH_ALLOWED;
    if (policy && policy->document_url) document_url = policy->document_url;

    if (url_has_scheme(url, "file:") &&
        (!ns_net_file_urls_allowed() ||
         (document_url && *document_url &&
          !url_has_scheme(document_url, "file:"))))
        return NS_FETCH_BLOCKED_SCHEME;

    char *upgraded = NULL;
    if (url_has_scheme(document_url, "https:") &&
        (url_has_scheme(url, "http:") || url_has_scheme(url, "ws:")) &&
        !url_is_loopback(url)) {
        gboolean upgradable = url_has_scheme(url, "http:") &&
                              (dest == NS_FETCH_DEST_IMAGE ||
                               dest == NS_FETCH_DEST_MEDIA);
        if (!upgradable) return NS_FETCH_BLOCKED_MIXED;
        upgraded = upgrade_to_https(url);
    }

    ns_csp_kind kind = destination_csp_kind(dest);
    if (dest == NS_FETCH_DEST_SCRIPT && policy && policy->check_scripts)
        kind = NS_CSP_SCRIPT;
    if (policy && policy->csp && kind != NS_CSP_KIND_COUNT &&
        !ns_csp_allows(policy->csp, kind, upgraded ? upgraded : url,
                       document_url)) {
        g_free(upgraded);
        return NS_FETCH_BLOCKED_CSP;
    }
    if (!upgraded) return NS_FETCH_ALLOWED;
    if (out_upgraded_url) *out_upgraded_url = upgraded;
    else g_free(upgraded);
    return NS_FETCH_UPGRADED;
}

/* True if the verdict blocks the request. */
gboolean
ns_fetch_verdict_blocks(ns_fetch_verdict verdict)
{
    return verdict == NS_FETCH_BLOCKED_SCHEME ||
           verdict == NS_FETCH_BLOCKED_MIXED ||
           verdict == NS_FETCH_BLOCKED_CSP;
}

/* Console message explaining a blocking verdict; caller frees, NULL if not blocked. */
char *
ns_fetch_verdict_message(ns_fetch_verdict verdict, ns_fetch_destination dest,
                         const char *url)
{
    const char *what = ns_fetch_destination_name(dest);
    if (!url) url = "";
    switch (verdict) {
    case NS_FETCH_BLOCKED_SCHEME:
        return g_strdup_printf("Refused to load local %s %s from a page "
                               "that is not a local file", what, url);
    case NS_FETCH_BLOCKED_MIXED:
        return g_strdup_printf("Blocked mixed content: the secure page "
                               "requested the insecure %s %s", what, url);
    case NS_FETCH_BLOCKED_CSP:
        return g_strdup_printf("Refused to load the %s %s because it "
                               "violates the Content-Security-Policy "
                               "directive %s", what, url,
                               destination_csp_directive(dest));
    default:
        return NULL;
    }
}
