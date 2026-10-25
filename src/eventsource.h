/* Northstar Web Browser — Server-Sent Events connection API with engine-thread callbacks.
 * Backs the JS EventSource binding; each connection runs on its own worker thread.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_EVENTSOURCE_H
#define NS_EVENTSOURCE_H

#include <glib.h>

#include "fetch_policy.h"

G_BEGIN_DECLS

typedef struct ns_es ns_es;

typedef struct ns_es_callbacks {
    void (*on_open)    (gpointer user_data);
    void (*on_message) (const char *event, const char *data,
                        const char *last_id, gpointer user_data);
    void (*on_error)   (gboolean fatal, gpointer user_data);
    gboolean (*busy)   (gpointer user_data);
} ns_es_callbacks;

ns_es *ns_es_new(const char *url, const char *origin, const char *last_event_id,
                 ns_fetch_policy *policy,
                 const ns_es_callbacks *cbs, gpointer user_data);

void ns_es_close(ns_es *es);
void ns_es_free(ns_es *es);

G_END_DECLS

#endif
