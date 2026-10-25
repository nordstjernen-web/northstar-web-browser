/* Northstar Web Browser — native helpers behind the JS MediaSource and SourceBuffer shim.
 * Maps script-side ids and blob URLs to mse.c sources; append, remove and buffered ranges.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "js_internal.h"

#include <math.h>
#include <string.h>

#include "mse.h"
#include "trace.h"

typedef struct {
    ns_mse_source *source;
    ns_mse_buffer *buffer;
} mse_handle;

static void
mse_handle_free(gpointer data)
{
    mse_handle *handle = data;
    if (!handle) return;
    ns_mse_source_unref(handle->source);
    g_free(handle);
}

/* Lazily creates the realm's id-to-handle and blob-URL-to-source tables. */
static void
mse_ensure_tables(ns_js *js)
{
    if (!js->mse_objects)
        js->mse_objects = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, mse_handle_free);
    if (!js->mse_urls)
        js->mse_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)ns_mse_source_unref);
}

static mse_handle *
mse_find(JSContext *ctx, uint32_t id)
{
    ns_js *js = js_from_ctx(ctx);
    if (!js || !js->mse_objects) return NULL;
    return g_hash_table_lookup(js->mse_objects, GUINT_TO_POINTER(id));
}

/* Looks up an MSE handle by the numeric id passed from script. */
static mse_handle *
mse_lookup(JSContext *ctx, JSValueConst id_value)
{
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, id_value)) return NULL;
    return mse_find(ctx, id);
}

/* Registers a source (and optional buffer) under a new numeric id for script use. */
static guint
mse_register(ns_js *js, ns_mse_source *source, ns_mse_buffer *buffer)
{
    mse_ensure_tables(js);
    mse_handle *handle = g_new0(mse_handle, 1);
    handle->source = ns_mse_source_ref(source);
    handle->buffer = buffer;
    guint id = ++js->next_mse_id;
    g_hash_table_insert(js->mse_objects, GUINT_TO_POINTER(id), handle);
    return id;
}

/* Gets the bytes of an ArrayBuffer or view; *holder keeps the backing buffer alive. */
static gboolean
mse_bytes(JSContext *ctx, JSValueConst value, const guint8 **data, size_t *len,
          JSValue *holder)
{
    *data = NULL;
    *len = 0;
    *holder = JS_UNDEFINED;
    if (JS_IsArrayBuffer(value)) {
        size_t total = 0;
        guint8 *base = JS_GetArrayBuffer(ctx, &total, value);
        if (!base) return FALSE;
        *data = base;
        *len = total;
        *holder = JS_DupValue(ctx, value);
        return TRUE;
    }
    size_t offset = 0, byte_len = 0, element = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &byte_len, &element);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return FALSE;
    }
    size_t total = 0;
    guint8 *base = JS_GetArrayBuffer(ctx, &total, buffer);
    if (!base || offset + byte_len > total) {
        JS_FreeValue(ctx, buffer);
        return FALSE;
    }
    *data = base + offset;
    *len = byte_len;
    *holder = buffer;
    return TRUE;
}

/* Converts flat [start, end] range pairs to a JS array; frees the GArray. */
static JSValue
ranges_to_array(JSContext *ctx, GArray *ranges)
{
    JSValue array = JS_NewArray(ctx);
    for (guint i = 0; i < ranges->len; i++)
        JS_SetPropertyUint32(ctx, array, i,
                             JS_NewFloat64(ctx, g_array_index(ranges, double, i)));
    g_array_free(ranges, TRUE);
    return array;
}

/* Builds the {status, offset, ranges} result returned to the JS SourceBuffer shim. */
static JSValue
mse_append_result(JSContext *ctx, const char *status, double offset,
                  GArray *ranges)
{
    JSValue result = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, result, "status", JS_NewString(ctx, status),
                              JS_PROP_C_W_E);
    if (ranges) {
        JS_DefinePropertyValueStr(ctx, result, "offset",
                                  JS_NewFloat64(ctx, offset), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, result, "ranges",
                                  ranges_to_array(ctx, ranges), JS_PROP_C_W_E);
    }
    return result;
}

static JSValue
js_mse_type_supported(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_EXCEPTION;
    gboolean ok = ns_mse_type_supported(type);
    JS_FreeCString(ctx, type);
    return JS_NewBool(ctx, ok);
}

/* __ndMseCreate: creates a native MediaSource and returns its id. */
static JSValue
js_mse_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    ns_js *js = js_from_ctx(ctx);
    if (!js) return JS_NewUint32(ctx, 0);
    ns_mse_source *source = ns_mse_source_new();
    guint id = mse_register(js, source, NULL);
    ns_mse_source_unref(source);
    return JS_NewUint32(ctx, id);
}

/* __ndMseBindUrl: maps an object URL to a MediaSource for media element src. */
static JSValue
js_mse_bind_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    uint32_t id = 0;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0])) return JS_FALSE;
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!url) return JS_EXCEPTION;
    mse_handle *handle = mse_find(ctx, id);
    if (!js || !handle || handle->buffer) {
        JS_FreeCString(ctx, url);
        return JS_FALSE;
    }
    g_hash_table_replace(js->mse_urls, g_strdup(url),
                         ns_mse_source_ref(handle->source));
    JS_FreeCString(ctx, url);
    return JS_TRUE;
}

/* __ndMseAddBuffer: adds a SourceBuffer of a MIME type; returns its id or 0. */
static JSValue
js_mse_add_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    uint32_t id = 0;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0])) return JS_NewUint32(ctx, 0);
    const char *type = JS_ToCString(ctx, argv[1]);
    if (!type) return JS_EXCEPTION;
    mse_handle *handle = mse_find(ctx, id);
    ns_mse_buffer *buffer = js && handle && !handle->buffer
                                ? ns_mse_source_add_buffer(handle->source, type)
                                : NULL;
    JS_FreeCString(ctx, type);
    if (!buffer) return JS_NewUint32(ctx, 0);
    return JS_NewUint32(ctx, mse_register(js, handle->source, buffer));
}

/* __ndMseRemoveBuffer: detaches and frees a SourceBuffer by id. */
static JSValue
js_mse_remove_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    uint32_t id = 0;
    if (argc < 1 || JS_ToUint32(ctx, &id, argv[0])) return JS_FALSE;
    mse_handle *handle = mse_find(ctx, id);
    if (!js || !handle || !handle->buffer) return JS_FALSE;
    ns_mse_source_remove_buffer(handle->source, handle->buffer);
    g_hash_table_remove(js->mse_objects, GUINT_TO_POINTER(id));
    return JS_TRUE;
}

/* __ndMseAppend: appends bytes with offset, mode and window; returns status and ranges. */
static JSValue
js_mse_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    uint32_t id = 0;
    double offset = 0, window_start = 0, window_end = INFINITY;
    if (argc < 6 || JS_ToUint32(ctx, &id, argv[0]) ||
        JS_ToFloat64(ctx, &offset, argv[2]) ||
        JS_ToFloat64(ctx, &window_start, argv[4]) ||
        JS_ToFloat64(ctx, &window_end, argv[5]))
        return argc < 6 ? mse_append_result(ctx, "error", 0, NULL) : JS_EXCEPTION;
    gboolean sequence = JS_ToBool(ctx, argv[3]) > 0;
    mse_handle *handle = mse_find(ctx, id);
    const guint8 *data = NULL;
    size_t len = 0;
    JSValue holder = JS_UNDEFINED;
    if (!handle || !handle->buffer ||
        !mse_bytes(ctx, argv[1], &data, &len, &holder))
        return mse_append_result(ctx, "error", 0, NULL);
    gint64 trace_start = ns_trace_now();
    ns_mse_append_result status = ns_mse_buffer_append(handle->buffer, data, len,
                                                       &offset, sequence,
                                                       window_start, window_end);
    ns_trace_completef("media", "MSE append", trace_start, "%zu bytes", len);
    JS_FreeValue(ctx, holder);
    const char *name = status == NS_MSE_APPEND_OK ? "ok"
                     : status == NS_MSE_APPEND_QUOTA_EXCEEDED ? "quota" : "error";
    return mse_append_result(ctx, name, offset,
                             ns_mse_buffer_ranges(handle->buffer));
}

/* __ndMseRemove: removes a time range and returns the remaining buffered ranges. */
static JSValue
js_mse_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    uint32_t id = 0;
    double start = 0, end = 0;
    if (argc < 3) return JS_NewArray(ctx);
    if (JS_ToUint32(ctx, &id, argv[0]) || JS_ToFloat64(ctx, &start, argv[1]) ||
        JS_ToFloat64(ctx, &end, argv[2]))
        return JS_EXCEPTION;
    mse_handle *handle = mse_find(ctx, id);
    if (!handle || !handle->buffer) return JS_NewArray(ctx);
    ns_mse_buffer_remove(handle->buffer, start, end);
    return ranges_to_array(ctx, ns_mse_buffer_ranges(handle->buffer));
}

static JSValue
js_mse_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    mse_handle *handle = argc >= 1 ? mse_lookup(ctx, argv[0]) : NULL;
    if (handle && handle->buffer) ns_mse_buffer_abort(handle->buffer);
    return JS_UNDEFINED;
}

static JSValue
js_mse_set_duration(JSContext *ctx, JSValueConst this_val, int argc,
                    JSValueConst *argv)
{
    (void)this_val;
    uint32_t id = 0;
    double duration = NAN;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0]) ||
        JS_ToFloat64(ctx, &duration, argv[1]))
        return JS_UNDEFINED;
    mse_handle *handle = mse_find(ctx, id);
    if (!handle) return JS_UNDEFINED;
    ns_mse_source_set_duration(handle->source, duration);
    return JS_UNDEFINED;
}

static JSValue
js_mse_set_ended(JSContext *ctx, JSValueConst this_val, int argc,
                 JSValueConst *argv)
{
    (void)this_val;
    mse_handle *handle = argc >= 2 ? mse_lookup(ctx, argv[0]) : NULL;
    if (handle) ns_mse_source_set_ended(handle->source, JS_ToBool(ctx, argv[1]) > 0);
    return JS_UNDEFINED;
}

/* __ndMseSetLiveSeekableRange: sets, or clears without range args, the live range. */
static JSValue
js_mse_set_live_seekable_range(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    (void)this_val;
    uint32_t id = 0;
    double start = 0.0, end = 0.0;
    if (argc < 1 || JS_ToUint32(ctx, &id, argv[0])) return JS_UNDEFINED;
    gboolean range = argc >= 3 && !JS_ToFloat64(ctx, &start, argv[1]) &&
                     !JS_ToFloat64(ctx, &end, argv[2]);
    mse_handle *handle = mse_find(ctx, id);
    if (!handle) return JS_UNDEFINED;
    if (!range)
        ns_mse_source_clear_live_seekable_range(handle->source);
    else
        ns_mse_source_set_live_seekable_range(handle->source, start, end);
    return JS_UNDEFINED;
}

/* MediaSource bound to an object URL in this realm (borrowed), or NULL. */
ns_mse_source *
ns_js_mse_source_for_url(ns_js *js, const char *url)
{
    if (!js || !js->mse_urls || !url) return NULL;
    return g_hash_table_lookup(js->mse_urls, url);
}

/* Drops all MediaSource handles and URL bindings of the realm. */
void
ns_js_mse_teardown(ns_js *js)
{
    if (!js) return;
    g_clear_pointer(&js->mse_urls, g_hash_table_destroy);
    g_clear_pointer(&js->mse_objects, g_hash_table_destroy);
}

/* Installs the __ndMse* native helpers used by the JS MediaSource shim. */
void
ns_js_mse_install(JSContext *ctx, JSValueConst global)
{
    ns_bind_fn(ctx, global, "__ndMseTypeSupported", js_mse_type_supported, 1);
    ns_bind_fn(ctx, global, "__ndMseCreate",        js_mse_create,         0);
    ns_bind_fn(ctx, global, "__ndMseBindUrl",       js_mse_bind_url,       2);
    ns_bind_fn(ctx, global, "__ndMseAddBuffer",     js_mse_add_buffer,     2);
    ns_bind_fn(ctx, global, "__ndMseRemoveBuffer",  js_mse_remove_buffer,  1);
    ns_bind_fn(ctx, global, "__ndMseAppend",        js_mse_append,         6);
    ns_bind_fn(ctx, global, "__ndMseRemove",        js_mse_remove,         3);
    ns_bind_fn(ctx, global, "__ndMseAbort",         js_mse_abort,          1);
    ns_bind_fn(ctx, global, "__ndMseSetDuration",   js_mse_set_duration,   2);
    ns_bind_fn(ctx, global, "__ndMseSetEnded",      js_mse_set_ended,      2);
    ns_bind_fn(ctx, global, "__ndMseSetLiveSeekableRange",
               js_mse_set_live_seekable_range, 3);
}
