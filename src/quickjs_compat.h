/* Northstar Web Browser — common QuickJS API over quickjs-ng or original QuickJS.
 * Engine is chosen at build time with NS_QUICKJS_ORIGINAL; declares the browser's patched hooks.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_QUICKJS_COMPAT_H
#define NS_QUICKJS_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#include <quickjs.h>

JSContext *JS_GetCallerRealm(JSContext *ctx);
JSContext *JS_GetFunctionRealm(JSContext *ctx, JSValueConst func_obj);
/* From the cross-realm-compile-check patch applied to both engines. */
void JS_SetCrossRealmCompileCheck(int (*cb)(JSContext *target, JSContext *caller));
JSContext *JS_GetCallerRealmOrNull(JSContext *ctx);
void JS_SetEvalStringCheck(int (*cb)(JSContext *ctx));

int JS_RepointArrayBuffer(JSContext *ctx, JSValueConst obj, uint8_t *data,
                          size_t byte_length);

JSClassID ns_new_class_id(JSClassID *pclass_id);

JSValue ns_js_compile_without_source(JSContext *ctx, const char *input, size_t input_len,
                                     const char *filename);
JSValue ns_js_eval_without_source(JSContext *ctx, const char *input, size_t input_len,
                                  const char *filename);

JSContext *ns_quickjs_new_context(JSRuntime *rt);
#define JS_NewContext(rt) ns_quickjs_new_context(rt)

#define ns_js_set_interrupt_handler(rt, ctx_cb, rt_cb, opaque)          \
    JS_SetInterruptHandler((rt),                                       \
        _Generic((JSInterruptHandler *)0,                              \
                 int (*)(JSContext *, void *): (ctx_cb),               \
                 default: (rt_cb)),                                    \
        (opaque))

#ifdef NS_QUICKJS_ORIGINAL

#define NS_QUICKJS_ENGINE_NAME "QuickJS"

typedef JS_BOOL ns_js_bool;

#define JS_EVAL_OPTIONS_VERSION 1

typedef struct JSEvalOptions {
    int version;
    int eval_flags;
    const char *filename;
    int line_num;
} JSEvalOptions;

typedef void *JSReallocArrayBufferDataFunc(JSRuntime *rt, void *opaque,
                                           void *ptr, size_t size);

JSValue ns_quickjs_new_array_buffer(JSContext *ctx, uint8_t *buf, size_t len,
                                    size_t max_len,
                                    JSReallocArrayBufferDataFunc *realloc_func,
                                    void *opaque, bool is_shared);
JSValue ns_quickjs_new_typed_array(JSContext *ctx, int argc, JSValueConst *argv,
                                   JSTypedArrayEnum type);
bool ns_quickjs_is_array(JSValueConst val);
bool ns_quickjs_is_error(JSValueConst val);

static inline bool
ns_quickjs_is_big_int(JSValueConst val)
{
    int tag = JS_VALUE_GET_TAG(val);
    return tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static inline bool
JS_IsStrictEqual(JSContext *ctx, JSValueConst op1, JSValueConst op2)
{
    return JS_StrictEq(ctx, op1, op2);
}

#define JS_NewArrayBuffer(ctx, buf, len, max_len, realloc_func, opaque, is_shared) \
    ns_quickjs_new_array_buffer((ctx), (buf), (len), (max_len), (realloc_func), \
                                (opaque), (is_shared))
#define JS_NewTypedArray(ctx, argc, argv, type) \
    ns_quickjs_new_typed_array((ctx), (argc), (argv), (type))
#define JS_IsArray(val) ns_quickjs_is_array(val)
#define JS_IsError(val) ns_quickjs_is_error(val)
#define JS_IsBigInt(val) ns_quickjs_is_big_int(val)

const char *JS_GetVersion(void);
bool JS_IsArrayBuffer(JSValueConst obj);
int JS_GetTypedArrayType(JSValueConst obj);
bool JS_IsDataView(JSValueConst obj);
JSValue JS_NewUint8ArrayCopy(JSContext *ctx, const uint8_t *buf, size_t len);
uint8_t *JS_GetUint8Array(JSContext *ctx, size_t *psize, JSValueConst obj);
JSValue JS_ToObject(JSContext *ctx, JSValueConst val);
JSValue __js_printf_like(3, 4) JS_ThrowDOMException(JSContext *ctx,
                                                    const char *name,
                                                    const char *fmt, ...);
JSValue JS_EvalThis2(JSContext *ctx, JSValueConst this_obj, const char *input,
                     size_t input_len, JSEvalOptions *options);

#else

#define NS_QUICKJS_ENGINE_NAME "quickjs-ng"

typedef bool ns_js_bool;

#endif

#endif
