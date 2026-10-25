/* Northstar Web Browser — JavaScript bytecode cache API (in-memory + on-disk).
 * Get/put compiled scripts keyed by top-level site, and queue background precompilation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_BYTECODE_CACHE_H
#define NS_BYTECODE_CACHE_H

#include <glib.h>

G_BEGIN_DECLS

void          ns_bytecode_cache_init(void);
void          ns_bytecode_cache_shutdown(void);

/* Entries are keyed by the top-level site that loaded the script, the
 * name the source was compiled under (its URL, which ends up in the
 * bytecode and in Error.stack) and the source itself. */
guint8 *ns_bytecode_cache_get(const char *top_url, const char *name,
                              const char *src, gsize src_len, gsize *out_len);
void    ns_bytecode_cache_put(const char *top_url, const char *name,
                              const char *src, gsize src_len,
                              const guint8 *bc, gsize bc_len);
void    ns_bytecode_cache_precompile(const char *top_url, const char *url,
                                     const guint8 *src, gsize len);

G_END_DECLS

#endif
