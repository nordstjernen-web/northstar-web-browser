/* Northstar Web Browser — refcounted per-document fetch policy and its block/allow verdicts.
 * Holds the document URL and CSP; network, media and worker loads check it before fetching.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_FETCH_POLICY_H
#define NS_FETCH_POLICY_H

#include <glib.h>

#include "csp.h"

G_BEGIN_DECLS

typedef enum {
    NS_FETCH_DEST_DEFAULT = 0,
    NS_FETCH_DEST_SCRIPT,
    NS_FETCH_DEST_STYLE,
    NS_FETCH_DEST_IMAGE,
    NS_FETCH_DEST_FONT,
    NS_FETCH_DEST_MEDIA,
    NS_FETCH_DEST_FRAME,
    NS_FETCH_DEST_WORKER,
    NS_FETCH_DEST_CONNECT,
    NS_FETCH_DEST_DOCUMENT,
    NS_FETCH_DEST_OBJECT,
} ns_fetch_destination;

typedef enum {
    NS_FETCH_ALLOWED,
    NS_FETCH_UPGRADED,
    NS_FETCH_BLOCKED_SCHEME,
    NS_FETCH_BLOCKED_MIXED,
    NS_FETCH_BLOCKED_CSP,
} ns_fetch_verdict;

typedef struct ns_fetch_policy ns_fetch_policy;

ns_fetch_policy *ns_fetch_policy_new(const char *document_url,
                                     const ns_csp *csp);
ns_fetch_policy *ns_fetch_policy_ref(ns_fetch_policy *policy);
void             ns_fetch_policy_unref(ns_fetch_policy *policy);
const char      *ns_fetch_policy_document_url(const ns_fetch_policy *policy);
/* A copy of policy that also holds script loads to script-src at every
 * hop, for a script the page's policy admitted by its URL (one admitted
 * by a nonce or 'strict-dynamic' may be redirected anywhere). */
ns_fetch_policy *ns_fetch_policy_new_script_hops(const ns_fetch_policy *policy);

ns_fetch_verdict ns_fetch_policy_check(const ns_fetch_policy *policy,
                                       ns_fetch_destination dest,
                                       const char *document_url,
                                       const char *url,
                                       char **out_upgraded_url);
gboolean         ns_fetch_verdict_blocks(ns_fetch_verdict verdict);
char            *ns_fetch_verdict_message(ns_fetch_verdict verdict,
                                          ns_fetch_destination dest,
                                          const char *url);

G_END_DECLS

#endif
