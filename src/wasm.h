/* Northstar Web Browser — installs the WebAssembly JS namespace into a QuickJS global.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_WASM_H
#define NS_WASM_H

#include "quickjs_compat.h"

#include <glib.h>

void ns_wasm_install(JSContext *ctx, JSValueConst global);

/* A WebAssembly.Module or WebAssembly.Memory, which postMessage to a
 * worker passes by handle instead of serializing. */
gboolean ns_wasm_is_cross_thread_object(JSValueConst v);
/* Registers v for another thread and returns its handle id; 0 with a
 * pending exception when v cannot be posted (a non-shared Memory). */
guint64 ns_wasm_cross_thread_export(JSContext *ctx, JSValueConst v);
/* A new JS object in ctx over the module or memory of handle id. */
JSValue ns_wasm_cross_thread_import(JSContext *ctx, guint64 id);
/* Releases handle id once its message is delivered or dropped. */
void ns_wasm_cross_thread_drop(guint64 id);

#endif
