/* Northstar Web Browser — native ShadowRealm over the QuickJS C API.
 * Child context in the same runtime; callables cross as wrapped functions; importValue is a stub.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "quickjs_compat.h"
#include "js_internal.h"
#include "js_realm.h"

#include <string.h>
#include <glib.h>

static JSClassID ns_shadowrealm_class_id;
static JSClassID ns_shadowrealm_handle_class_id;

/* Shared by the ShadowRealm object and the handles its child-side wrappers
 * hold; freed when the last of them goes. */
typedef struct {
    int        refcount;
    JSContext *child;   /* NULL once the ShadowRealm object is finalized */
    JSContext *parent;
    JSValue    self;    /* the ShadowRealm object, not owned; valid while child */
} ns_shadowrealm;

static void
ns_shadowrealm_unref(ns_shadowrealm *r)
{
    if (r && --r->refcount == 0)
        g_free(r);
}

static void
ns_shadowrealm_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_shadowrealm *r = JS_GetOpaque(val, ns_shadowrealm_class_id);
    if (!r) return;
    JSContext *child = r->child;
    r->child = NULL;
    r->self = JS_UNDEFINED;
    if (child) JS_FreeContext(child);
    ns_shadowrealm_unref(r);
}

static void
ns_shadowrealm_handle_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_shadowrealm_unref(JS_GetOpaque(val, ns_shadowrealm_handle_class_id));
}

static JSClassDef ns_shadowrealm_class = {
    "ShadowRealm", .finalizer = ns_shadowrealm_finalizer,
};

static JSClassDef ns_shadowrealm_handle_class = {
    "ShadowRealmHandle", .finalizer = ns_shadowrealm_handle_finalizer,
};

/* The realm behind a wrapper's data slot: the ShadowRealm object itself
 * (parent-side wrappers) or a weak handle (child-side wrappers). */
static ns_shadowrealm *
realm_from_data(JSValueConst v)
{
    ns_shadowrealm *r = JS_GetOpaque(v, ns_shadowrealm_class_id);
    return r ? r : JS_GetOpaque(v, ns_shadowrealm_handle_class_id);
}

static JSValue realm_wrap_value(JSContext *dst, ns_shadowrealm *r, JSValue v);

/* A wrapped function lives in one realm (the one it was created in) and
 * forwards to a callable in the other.  Callable arguments are wrapped for
 * the target realm in turn, so neither side ever receives a raw function
 * object -- and with it the other realm's Function constructor. */
static JSValue
realm_wrapped_call(JSContext *ctx, JSValueConst this_val, int argc,
                   JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val; (void)magic;
    JSValueConst target = data[0];
    ns_shadowrealm *r = realm_from_data(data[1]);
    if (!r || !r->child || !r->parent)
        return JS_ThrowTypeError(ctx, "ShadowRealm is no longer available");
    JSContext *tctx = ctx == r->child ? r->parent : r->child;
    for (int i = 0; i < argc; i++) {
        if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i]))
            return JS_ThrowTypeError(ctx,
                "ShadowRealm wrapped function: only primitives and callables "
                "may cross the realm boundary");
    }
    JSValue *args = argc > 0 ? g_new0(JSValue, argc) : NULL;
    for (int i = 0; i < argc; i++) {
        args[i] = realm_wrap_value(tctx, r, JS_DupValue(ctx, argv[i]));
        if (JS_IsException(args[i])) {
            JSValue exc = JS_GetException(tctx);
            JS_FreeValue(tctx, exc);
            for (int j = 0; j < i; j++) JS_FreeValue(tctx, args[j]);
            g_free(args);
            return JS_ThrowTypeError(ctx,
                "ShadowRealm wrapped function: argument could not be wrapped");
        }
    }
    JSValue res = JS_Call(tctx, target, JS_UNDEFINED, argc,
                          (JSValueConst *)args);
    for (int i = 0; i < argc; i++) JS_FreeValue(tctx, args[i]);
    g_free(args);
    if (JS_IsException(res)) {
        JSValue exc = JS_GetException(tctx);
        const char *msg = JS_ToCString(tctx, exc);
        JSValue te = JS_ThrowTypeError(ctx, "%s",
                                       msg ? msg : "ShadowRealm callable threw");
        if (msg) JS_FreeCString(tctx, msg);
        JS_FreeValue(tctx, exc);
        return te;
    }
    return realm_wrap_value(ctx, r, res);
}

/* Wraps a value crossing into dst: primitives pass, callables get wrappers, objects throw.
 * A wrapper in the parent keeps the ShadowRealm object (and so the child
 * context) alive.  A wrapper in the child must not: the object owns the
 * child context through its opaque pointer, which the GC cannot see, so a
 * child global holding such a wrapper (globalThis.k = f) would keep the
 * object, and with it the whole child realm, alive forever.  Child-side
 * wrappers hold a weak handle instead and throw once the realm is gone. */
static JSValue
realm_wrap_value(JSContext *dst, ns_shadowrealm *r, JSValue v)
{
    if (!JS_IsObject(v))
        return v;
    if (JS_IsFunction(dst, v)) {
        if (!r->child) {
            JS_FreeValue(dst, v);
            return JS_ThrowTypeError(dst, "ShadowRealm is no longer available");
        }
        JSValue owner;
        if (dst == r->child) {
            owner = JS_NewObjectClass(dst, ns_shadowrealm_handle_class_id);
            if (JS_IsException(owner)) {
                JS_FreeValue(dst, v);
                return owner;
            }
            r->refcount++;
            JS_SetOpaque(owner, r);
        } else {
            owner = JS_DupValue(dst, r->self);
        }
        JSValueConst data[2] = { v, owner };
        JSValue fn = JS_NewCFunctionData(dst, realm_wrapped_call, 0, 0, 2, data);
        JS_FreeValue(dst, owner);
        JS_FreeValue(dst, v);
        return fn;
    }
    JS_FreeValue(dst, v);
    return JS_ThrowTypeError(dst,
        "ShadowRealm: evaluation result must be a primitive or a callable");
}

/* ShadowRealm constructor: creates a fresh child context in the same runtime. */
static JSValue
ns_shadowrealm_ctor(JSContext *ctx, JSValueConst new_target,
                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSContext *child = JS_NewContext(JS_GetRuntime(ctx));
    if (!child) return JS_ThrowOutOfMemory(ctx);
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(ctx, proto, ns_shadowrealm_class_id)
        : JS_NewObjectClass(ctx, ns_shadowrealm_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { JS_FreeContext(child); return obj; }
    ns_shadowrealm *r = g_new0(ns_shadowrealm, 1);
    r->refcount = 1;
    r->child = child;
    r->parent = ctx;
    r->self = obj;
    JS_SetOpaque(obj, r);
    return obj;
}

/* ShadowRealm.prototype.evaluate: evals in the child realm (CSP eval check), wraps result. */
static JSValue
ns_shadowrealm_evaluate(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    ns_shadowrealm *r = JS_GetOpaque2(ctx, this_val, ns_shadowrealm_class_id);
    if (!r) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "ShadowRealm.prototype.evaluate expects a string");
    if (!ns_js_eval_allowed(ctx))
        return JS_ThrowTypeError(ctx,
            "ShadowRealm.prototype.evaluate: refused by Content-Security-Policy "
            "(no 'unsafe-eval')");
    size_t len = 0;
    const char *src = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!src) return JS_EXCEPTION;
    JSValue res = JS_Eval(r->child, src, len, "<shadowrealm>",
                          JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, src);
    if (JS_IsException(res)) {
        JSValue exc = JS_GetException(r->child);
        const char *msg = JS_ToCString(r->child, exc);
        JSValue te = JS_ThrowTypeError(ctx, "%s",
                                       msg ? msg : "ShadowRealm evaluate threw");
        if (msg) JS_FreeCString(r->child, msg);
        JS_FreeValue(r->child, exc);
        return te;
    }
    return realm_wrap_value(ctx, r, res);
}

/* ShadowRealm.prototype.importValue stub returning a rejected promise. */
static JSValue
ns_shadowrealm_importValue(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    ns_shadowrealm *r = JS_GetOpaque2(ctx, this_val, ns_shadowrealm_class_id);
    if (!r) return JS_EXCEPTION;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message",
        JS_NewString(ctx, "ShadowRealm.prototype.importValue is not supported"));
    JSValue ret = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static void
realm_bind(JSContext *ctx, JSValueConst obj, const char *name,
           JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

/* Installs a native ShadowRealm global when the engine does not provide one. */
void
ns_js_realm_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    JSAtom sr_atom = JS_NewAtom(ctx, "ShadowRealm");
    int has_sr = JS_HasProperty(ctx, global, sr_atom);
    JS_FreeAtom(ctx, sr_atom);
    if (has_sr <= 0) {
        ns_new_class_id(&ns_shadowrealm_class_id);
        if (!JS_IsRegisteredClass(rt, ns_shadowrealm_class_id))
            JS_NewClass(rt, ns_shadowrealm_class_id, &ns_shadowrealm_class);
        ns_new_class_id(&ns_shadowrealm_handle_class_id);
        if (!JS_IsRegisteredClass(rt, ns_shadowrealm_handle_class_id))
            JS_NewClass(rt, ns_shadowrealm_handle_class_id,
                        &ns_shadowrealm_handle_class);
        JSValue ctor = JS_NewCFunction2(ctx, ns_shadowrealm_ctor, "ShadowRealm",
                                        0, JS_CFUNC_constructor, 0);
        JSValue proto = JS_NewObject(ctx);
        realm_bind(ctx, proto, "evaluate", ns_shadowrealm_evaluate, 1);
        realm_bind(ctx, proto, "importValue", ns_shadowrealm_importValue, 2);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, global, "ShadowRealm", ctor);
    }
}
