/* Northstar Web Browser — browsing history API: record visits, substring search, about:history.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_HISTORY_H
#define NS_HISTORY_H

#include <glib.h>

G_BEGIN_DECLS

void   ns_history_init(void);
void   ns_history_shutdown(void);

void   ns_history_clear(void);
void   ns_history_record(const char *url, const char *title);

typedef struct ns_history_match {
    char *url;
    char *title;
} ns_history_match;

GPtrArray *ns_history_search(const char *query, guint limit);

char  *ns_history_html_page(void);

G_END_DECLS

#endif
