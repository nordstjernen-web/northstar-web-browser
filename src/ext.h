/* Northstar Web Browser — WebExtensions API: content scripts per URL, blocking, resources.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_EXT_H
#define NS_EXT_H

#include <glib.h>
#include "quickjs_compat.h"

G_BEGIN_DECLS

/* Move extensions from their previous location inside the writable profile
 * directory; call before the sandbox is set up. */
void     ns_ext_migrate_default_dir(void);
/* The content scripts for url, as the source of an array of
 * [extension id or null, function] pairs; each function takes the object
 * ns_ext_content_script_helpers() makes for that id as its only argument.
 * NULL when none apply. */
char    *ns_ext_content_scripts_for_url(const char *url, gboolean at_start);
/* The native helpers for one extension, bound to its id: they act for
 * that extension whatever id the calling code passes. */
JSValue  ns_ext_content_script_helpers(JSContext *ctx, const char *ext_id);
gboolean ns_ext_should_block(const char *url, const char *initiator);
gboolean ns_ext_resource_load(const char *url, GBytes **bytes,
                              char **content_type, char **error);
G_END_DECLS

#endif
