/* Northstar Web Browser — progressive MP4/WebM download for <video src> into an MSE source.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_PROGRESSIVE_H
#define NS_PROGRESSIVE_H

#include <glib.h>

#include "fetch_policy.h"
#include "mse.h"

G_BEGIN_DECLS

typedef struct ns_progressive ns_progressive;

typedef enum {
    NS_PROGRESSIVE_PROBING,
    NS_PROGRESSIVE_READY,
    NS_PROGRESSIVE_OTHER_FORMAT,
    NS_PROGRESSIVE_FAILED,
} ns_progressive_state;

ns_progressive      *ns_progressive_new(const char *url, const char *top_url,
                                        ns_fetch_policy *policy);
void                 ns_progressive_free(ns_progressive *file);
ns_mse_source       *ns_progressive_source(const ns_progressive *file);
ns_progressive_state ns_progressive_get_state(const ns_progressive *file);
GBytes              *ns_progressive_take_body(ns_progressive *file);
void                 ns_progressive_update(ns_progressive *file,
                                           double position, gboolean playing);

G_END_DECLS

#endif
