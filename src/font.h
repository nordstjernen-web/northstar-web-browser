/* Northstar Web Browser — @font-face web font API: request a family's source and track loading.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_FONT_H
#define NS_FONT_H

#include <glib.h>

#include "fetch_policy.h"

G_BEGIN_DECLS

void     ns_font_init(void);

gboolean ns_font_available(void);


gboolean ns_font_family_loaded(const char *family);
guint    ns_font_generation(void);

void     ns_font_request(const char *family, const char *src_url,
                         const char *base_url, ns_fetch_policy *policy);

guint    ns_font_pending_count(void);

typedef void (*ns_font_idle_cb)(gpointer user_data);
void     ns_font_add_idle_cb(ns_font_idle_cb cb, gpointer user_data);
void     ns_font_remove_idle_cb(ns_font_idle_cb cb, gpointer user_data);

G_END_DECLS

#endif
