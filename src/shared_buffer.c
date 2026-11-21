/* Northstar Web Browser — process-wide SharedArrayBuffer storage.
 * QuickJS allocates SharedArrayBuffer memory through per-runtime hooks;
 * every runtime here uses this one pool, keyed by data pointer, so a
 * SharedArrayBuffer cloned into a worker (JS_WRITE_OBJ_SAB) refers to the
 * same memory and the last runtime to drop it frees it.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shared_buffer.h"

typedef struct {
    gint64 ref;
    /* external memory (a shared WebAssembly.Memory): released, not freed */
    void (*release)(gpointer user);
    gpointer user;
} ns_shared_buffer_entry;

static GMutex ns_shared_buffer_lock;
static GHashTable *ns_shared_buffer_entries; /* data pointer -> entry */

static GHashTable *
ns_shared_buffer_table(void)
{
    if (!ns_shared_buffer_entries)
        ns_shared_buffer_entries = g_hash_table_new(NULL, NULL);
    return ns_shared_buffer_entries;
}

void
ns_shared_buffer_retain(void *data)
{
    if (!data) return;
    g_mutex_lock(&ns_shared_buffer_lock);
    ns_shared_buffer_entry *e = g_hash_table_lookup(ns_shared_buffer_table(),
                                                    data);
    if (e) e->ref++;
    g_mutex_unlock(&ns_shared_buffer_lock);
}

void
ns_shared_buffer_release(void *data)
{
    if (!data) return;
    g_mutex_lock(&ns_shared_buffer_lock);
    ns_shared_buffer_entry *e = g_hash_table_lookup(ns_shared_buffer_table(),
                                                    data);
    if (e && --e->ref <= 0)
        g_hash_table_remove(ns_shared_buffer_table(), data);
    else
        e = NULL;
    g_mutex_unlock(&ns_shared_buffer_lock);
    if (!e) return;
    if (e->release)
        e->release(e->user);
    else
        g_free(data);
    g_free(e);
}

static void *
ns_shared_buffer_alloc_hook(void *opaque, size_t size)
{
    (void)opaque;
    void *data = g_try_malloc0(size ? size : 1);
    if (!data) return NULL;
    ns_shared_buffer_entry *e = g_new0(ns_shared_buffer_entry, 1);
    e->ref = 1;
    g_mutex_lock(&ns_shared_buffer_lock);
    g_hash_table_insert(ns_shared_buffer_table(), data, e);
    g_mutex_unlock(&ns_shared_buffer_lock);
    return data;
}

static void
ns_shared_buffer_free_hook(void *opaque, void *ptr)
{
    (void)opaque;
    ns_shared_buffer_release(ptr);
}

static void
ns_shared_buffer_dup_hook(void *opaque, void *ptr)
{
    (void)opaque;
    ns_shared_buffer_retain(ptr);
}

static const JSSharedArrayBufferFunctions ns_shared_buffer_funcs = {
    .sab_alloc = ns_shared_buffer_alloc_hook,
    .sab_free = ns_shared_buffer_free_hook,
    .sab_dup = ns_shared_buffer_dup_hook,
    .sab_opaque = NULL,
};

void
ns_shared_buffer_install(JSRuntime *rt)
{
    if (rt)
        JS_SetSharedArrayBufferFunctions(rt, &ns_shared_buffer_funcs);
}

JSValue
ns_shared_buffer_new_external(JSContext *ctx, guint8 *base, size_t len,
                              void (*release)(gpointer user), gpointer user,
                              gboolean *took_user)
{
    gboolean took = FALSE;
    g_mutex_lock(&ns_shared_buffer_lock);
    ns_shared_buffer_entry *e = g_hash_table_lookup(ns_shared_buffer_table(),
                                                    base);
    if (e) {
        e->ref++;
    } else {
        e = g_new0(ns_shared_buffer_entry, 1);
        e->ref = 1;
        e->release = release;
        e->user = user;
        g_hash_table_insert(ns_shared_buffer_table(), base, e);
        took = TRUE;
    }
    g_mutex_unlock(&ns_shared_buffer_lock);
    if (took_user) *took_user = took;
    /* the buffer takes its own reference through the dup hook */
    JSValue buf = JS_NewArrayBuffer(ctx, base, len, 0, NULL, NULL, true);
    ns_shared_buffer_release(base);
    return buf;
}

gboolean
ns_shared_buffer_is(JSContext *ctx, JSValueConst v)
{
    /* QuickJS numbers its SharedArrayBuffer class right after ArrayBuffer */
    static JSClassID sab_class;
    if (!sab_class) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return FALSE;
        }
        sab_class = JS_GetClassID(ab) + 1;
        JS_FreeValue(ctx, ab);
    }
    return JS_IsObject(v) && JS_GetClassID(v) == sab_class;
}
