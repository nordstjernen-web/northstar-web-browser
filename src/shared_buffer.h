/* Northstar Web Browser — process-wide SharedArrayBuffer storage.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_SHARED_BUFFER_H
#define NS_SHARED_BUFFER_H

#include <glib.h>

#include "quickjs_compat.h"

/* Makes the runtime allocate SharedArrayBuffer storage from one
 * reference-counted pool shared by every runtime of the process, so a
 * SharedArrayBuffer posted to a worker keeps its memory alive and shared
 * across threads. */
void ns_shared_buffer_install(JSRuntime *rt);

/* Creates a SharedArrayBuffer over external memory at base (len bytes).
 * The first buffer over a base takes a reference on it through
 * release(user) being owed: release runs once the last SharedArrayBuffer
 * over base, in any runtime, is freed. */
JSValue ns_shared_buffer_new_external(JSContext *ctx, guint8 *base, size_t len,
                                      void (*release)(gpointer user),
                                      gpointer user, gboolean *took_user);

/* Keeps the storage behind a SharedArrayBuffer data pointer alive, for
 * a message in flight to another thread; balance with _release. */
void ns_shared_buffer_retain(void *data);
void ns_shared_buffer_release(void *data);

/* TRUE if v is a SharedArrayBuffer. */
gboolean ns_shared_buffer_is(JSContext *ctx, JSValueConst v);

#endif
