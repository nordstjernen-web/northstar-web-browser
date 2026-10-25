/* Northstar Web Browser — WebIDL receiver checks for the members of node interfaces.
 * Wraps Node-derived prototype members so calls on a wrong receiver throw Illegal invocation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "js_receiver.h"

#include <string.h>
#include <glib.h>

enum {
    RECEIVER_NODE    = 1 << 0,
    RECEIVER_ATTR    = 1 << 1,
    RECEIVER_REJECTS = 1 << 2,
    RECEIVER_LENIENT = 1 << 3,
    RECEIVER_SETTER  = 1 << 4,
    RECEIVER_KINDS   = 1 << 5,
};

enum {
    RECEIVER_DATA_TARGET,
    RECEIVER_DATA_NODE_CLASS,
    RECEIVER_DATA_ATTR_CLASS,
    RECEIVER_DATA_NODE_PROTO,
    RECEIVER_DATA_NAME,
    RECEIVER_DATA_LEN,
};

typedef struct {
    JSContext *ctx;
    JSClassID node_class;
    JSClassID attr_class;
    JSValueConst node_proto;
    GHashTable *made[RECEIVER_KINDS];
    GHashTable *made_setters;
    GHashTable *event_target_names;
} receiver_pass;

static const char *const receiver_promise_members[] = {
    "decode", "exitFullscreen", "exitPictureInPicture", "hasStorageAccess",
    "play", "requestFullscreen", "requestPictureInPicture",
    "requestPointerLock", "requestStorageAccess", "setMediaKeys",
    "setSinkId",
};

static const char *const receiver_lenient_members[] = {
    "onmouseenter", "onmouseleave", "onreadystatechange",
};

static gboolean
receiver_name_in(const char *name, const char *const *names, gsize n)
{
    for (gsize i = 0; i < n; i++)
        if (strcmp(name, names[i]) == 0) return TRUE;
    return FALSE;
}

/* True if this_val's class is the Node or Attr class accepted by kind. */
static gboolean
receiver_matches(JSValueConst this_val, int kind, JSValueConst *data)
{
    if (!JS_IsObject(this_val)) return FALSE;
    JSClassID id = JS_GetClassID(this_val);
    JSClassID node_class =
        (JSClassID)JS_VALUE_GET_INT(data[RECEIVER_DATA_NODE_CLASS]);
    JSClassID attr_class =
        (JSClassID)JS_VALUE_GET_INT(data[RECEIVER_DATA_ATTR_CLASS]);
    if ((kind & RECEIVER_NODE) && id == node_class) return TRUE;
    if ((kind & RECEIVER_ATTR) && id == attr_class) return TRUE;
    return FALSE;
}

/* Returns a promise rejected with an Illegal invocation TypeError. */
static JSValue
receiver_rejected_promise(JSContext *ctx)
{
    JSValue settle[2];
    JSValue promise = JS_NewPromiseCapability(ctx, settle);
    if (JS_IsException(promise)) return promise;
    JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValue error = JS_GetException(ctx);
    JSValue r = JS_Call(ctx, settle[1], JS_UNDEFINED, 1, (JSValueConst *)&error);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, error);
    JS_FreeValue(ctx, settle[0]);
    JS_FreeValue(ctx, settle[1]);
    return promise;
}

static gboolean
receiver_proto_extends(JSContext *ctx, JSValueConst proto, JSValueConst base);

/* True for a plain object whose prototype chain includes Node.prototype. */
static gboolean
receiver_is_derived_ordinary_object(JSContext *ctx, JSValueConst this_val,
                                    JSValueConst *data)
{
    JSClassID id = JS_GetClassID(this_val);
    if (id == (JSClassID)JS_VALUE_GET_INT(data[RECEIVER_DATA_NODE_CLASS]) ||
        id == (JSClassID)JS_VALUE_GET_INT(data[RECEIVER_DATA_ATTR_CLASS]) ||
        JS_IsFunction(ctx, this_val))
        return FALSE;
    JSValue proto = JS_GetPrototype(ctx, this_val);
    gboolean derived = receiver_proto_extends(ctx, proto,
                                              data[RECEIVER_DATA_NODE_PROTO]);
    JS_FreeValue(ctx, proto);
    return derived;
}

/* Setter fallback that defines an own data property on the receiver. */
static JSValue
receiver_shadowing_set(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv, JSValueConst *data)
{
    JSAtom atom = JS_ValueToAtom(ctx, data[RECEIVER_DATA_NAME]);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue val = argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    int ok = JS_DefinePropertyValue(ctx, this_val, atom, val,
                                    JS_PROP_C_W_E | JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
    return ok < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* Wrapper body: forwards on a valid receiver, else throws, rejects or ignores. */
static JSValue
receiver_checked_call(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv, int kind, JSValueConst *data)
{
    if (receiver_matches(this_val, kind, data))
        return JS_Call(ctx, data[RECEIVER_DATA_TARGET], this_val, argc, argv);
    if ((kind & RECEIVER_SETTER) && JS_IsObject(this_val) &&
        receiver_is_derived_ordinary_object(ctx, this_val, data))
        return receiver_shadowing_set(ctx, this_val, argc, argv, data);
    if (kind & RECEIVER_LENIENT) return JS_UNDEFINED;
    if (kind & RECEIVER_REJECTS) return receiver_rejected_promise(ctx);
    return JS_ThrowTypeError(ctx, "Illegal invocation");
}

/* Reads a function's length property, defaulting to 0 on error. */
static int
receiver_function_length(JSContext *ctx, JSValueConst fn)
{
    JSValue len = JS_GetPropertyStr(ctx, fn, "length");
    int32_t n = 0;
    if (JS_IsException(len) || JS_ToInt32(ctx, &n, len) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        n = 0;
    }
    JS_FreeValue(ctx, len);
    return n;
}

/* Returns a cached receiver-checking wrapper for fn, preserving name and length. */
static JSValue
receiver_checked(receiver_pass *pass, JSValueConst fn, int kind, JSAtom atom)
{
    JSContext *ctx = pass->ctx;
    if (!JS_IsFunction(ctx, fn)) return JS_DupValue(ctx, fn);
    GHashTable *made_for_kind = pass->made[kind];
    gpointer key = JS_VALUE_GET_PTR(fn);
    char *setter_key = NULL;
    if (kind & RECEIVER_SETTER) {
        made_for_kind = pass->made_setters;
        setter_key = g_strdup_printf("%p/%d/%u", key, kind, (unsigned)atom);
        key = setter_key;
    }
    gpointer hit = g_hash_table_lookup(made_for_kind, key);
    if (hit) {
        g_free(setter_key);
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, hit));
    }
    JSValue name_value = JS_AtomToString(ctx, atom);
    JSValueConst data[RECEIVER_DATA_LEN] = {
        [RECEIVER_DATA_TARGET] = fn,
        [RECEIVER_DATA_NODE_CLASS] = JS_NewInt32(ctx, (int32_t)pass->node_class),
        [RECEIVER_DATA_ATTR_CLASS] = JS_NewInt32(ctx, (int32_t)pass->attr_class),
        [RECEIVER_DATA_NODE_PROTO] = pass->node_proto,
        [RECEIVER_DATA_NAME] = name_value,
    };
    JSValue made = JS_NewCFunctionData(ctx, receiver_checked_call,
                               receiver_function_length(ctx, fn), kind,
                               RECEIVER_DATA_LEN, data);
    JS_FreeValue(ctx, name_value);
    if (JS_IsException(made)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        g_free(setter_key);
        return JS_DupValue(ctx, fn);
    }
    JSValue name = JS_GetPropertyStr(ctx, fn, "name");
    if (JS_IsString(name))
        JS_DefinePropertyValueStr(ctx, made, "name", name, JS_PROP_CONFIGURABLE);
    else
        JS_FreeValue(ctx, name);
    g_hash_table_insert(made_for_kind,
                        setter_key ? setter_key : JS_VALUE_GET_PTR(fn),
                        JS_VALUE_GET_PTR(JS_DupValue(ctx, made)));
    return made;
}

/* Adds lenient-this or promise-rejecting behavior for specific member names. */
static int
receiver_kind_for(const char *name, int accepted, gboolean accessor)
{
    int kind = accepted;
    if (accessor &&
        receiver_name_in(name, receiver_lenient_members,
                         G_N_ELEMENTS(receiver_lenient_members)))
        kind |= RECEIVER_LENIENT;
    if (!accessor &&
        receiver_name_in(name, receiver_promise_members,
                         G_N_ELEMENTS(receiver_promise_members)))
        kind |= RECEIVER_REJECTS;
    return kind;
}

/* Replaces a configurable method or accessor on a prototype with checked wrappers. */
static void
receiver_check_member(receiver_pass *pass, JSValueConst proto, JSAtom atom,
                      int accepted)
{
    JSContext *ctx = pass->ctx;
    JSPropertyDescriptor desc;
    if (JS_GetOwnProperty(ctx, &desc, proto, atom) <= 0) return;
    const char *name = JS_AtomToCString(ctx, atom);
    gboolean configurable = (desc.flags & JS_PROP_CONFIGURABLE) != 0;
    gboolean accessor = (desc.flags & JS_PROP_GETSET) != 0;
    gboolean inherited_from_event_target =
        g_hash_table_contains(pass->event_target_names, GUINT_TO_POINTER(atom));
    if (name && configurable && !inherited_from_event_target &&
        strcmp(name, "constructor") != 0) {
        int kind = receiver_kind_for(name, accepted, accessor);
        if (accessor) {
            JSValue get = receiver_checked(pass, desc.getter, kind, atom);
            JSValue set = receiver_checked(pass, desc.setter,
                                           kind | RECEIVER_SETTER, atom);
            JS_DefinePropertyGetSet(ctx, proto, atom, get, set,
                                    desc.flags & (JS_PROP_CONFIGURABLE |
                                                  JS_PROP_ENUMERABLE));
        } else if (JS_IsFunction(ctx, desc.value)) {
            JSValue fn = receiver_checked(pass, desc.value, kind, atom);
            JS_DefinePropertyValue(ctx, proto, atom, fn,
                                   desc.flags & (JS_PROP_CONFIGURABLE |
                                                 JS_PROP_ENUMERABLE |
                                                 JS_PROP_WRITABLE));
        }
    }
    if (name) JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, desc.value);
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
}

/* Wraps every own string-keyed member of a prototype. */
static void
receiver_check_proto(receiver_pass *pass, JSValueConst proto, int accepted)
{
    JSContext *ctx = pass->ctx;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, proto, JS_GPN_STRING_MASK) != 0)
        return;
    for (uint32_t i = 0; i < len; i++) {
        receiver_check_member(pass, proto, tab[i].atom, accepted);
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
}

/* Records EventTarget.prototype member atoms so they are left unwrapped. */
static void
receiver_note_event_target_names(receiver_pass *pass, JSValueConst target_proto)
{
    JSContext *ctx = pass->ctx;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (!JS_IsObject(target_proto) ||
        JS_GetOwnPropertyNames(ctx, &tab, &len, target_proto,
                               JS_GPN_STRING_MASK) != 0)
        return;
    for (uint32_t i = 0; i < len; i++)
        g_hash_table_add(pass->event_target_names,
                         GUINT_TO_POINTER(tab[i].atom));
    js_free(ctx, tab);
}

/* Frees the recorded EventTarget member atoms and their set. */
static void
receiver_free_event_target_names(receiver_pass *pass)
{
    GHashTableIter it;
    gpointer atom;
    g_hash_table_iter_init(&it, pass->event_target_names);
    while (g_hash_table_iter_next(&it, &atom, NULL))
        JS_FreeAtom(pass->ctx, GPOINTER_TO_UINT(atom));
    g_hash_table_destroy(pass->event_target_names);
}

/* prototype of the global interface object named by atom, or undefined. */
static JSValue
receiver_interface_proto(JSContext *ctx, JSValueConst global, JSAtom atom)
{
    JSPropertyDescriptor desc;
    if (JS_GetOwnProperty(ctx, &desc, global, atom) <= 0) return JS_UNDEFINED;
    JSValue proto = JS_UNDEFINED;
    if (!(desc.flags & JS_PROP_GETSET) && JS_IsFunction(ctx, desc.value))
        proto = JS_GetPropertyStr(ctx, desc.value, "prototype");
    JS_FreeValue(ctx, desc.value);
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return proto;
}

static JSValue
receiver_named_proto(JSContext *ctx, JSValueConst global, const char *name)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue proto = receiver_interface_proto(ctx, global, atom);
    JS_FreeAtom(ctx, atom);
    return proto;
}

/* True if base appears on proto's prototype chain (depth-limited). */
static gboolean
receiver_proto_extends(JSContext *ctx, JSValueConst proto, JSValueConst base)
{
    JSValue p = JS_DupValue(ctx, proto);
    gboolean found = FALSE;
    for (int depth = 0; JS_IsObject(p) && depth < 64 && !found; depth++) {
        found = JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(base);
        JSValue next = JS_GetPrototype(ctx, p);
        JS_FreeValue(ctx, p);
        p = next;
    }
    JS_FreeValue(ctx, p);
    return found;
}

/* Wraps members of every global interface prototype that inherits from Node. */
static void
receiver_check_interfaces(receiver_pass *pass, JSValueConst global,
                          JSValueConst node_proto, JSValueConst attr_proto)
{
    JSContext *ctx = pass->ctx;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, global, JS_GPN_STRING_MASK) != 0)
        return;
    GHashTable *done = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (uint32_t i = 0; i < len; i++) {
        JSValue proto = receiver_interface_proto(ctx, global, tab[i].atom);
        if (JS_IsObject(proto) &&
            !g_hash_table_contains(done, JS_VALUE_GET_PTR(proto)) &&
            receiver_proto_extends(ctx, proto, node_proto)) {
            g_hash_table_add(done, JS_VALUE_GET_PTR(proto));
            int accepted = JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(node_proto)
                ? RECEIVER_NODE | RECEIVER_ATTR
                : JS_IsObject(attr_proto) &&
                  JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(attr_proto)
                ? RECEIVER_ATTR : RECEIVER_NODE;
            receiver_check_proto(pass, proto, accepted);
        }
        JS_FreeValue(ctx, proto);
        JS_FreeAtom(ctx, tab[i].atom);
    }
    g_hash_table_destroy(done);
    js_free(ctx, tab);
}

/* Enforces WebIDL receiver checks on Node-derived interface members. */
void
ns_js_require_node_receivers(JSContext *ctx, JSValueConst global,
                             JSClassID node_class, JSClassID attr_class)
{
    JSValue node_proto = receiver_named_proto(ctx, global, "Node");
    if (!JS_IsObject(node_proto)) {
        JS_FreeValue(ctx, node_proto);
        return;
    }
    JSValue attr_proto = receiver_named_proto(ctx, global, "Attr");
    JSValue target_proto = receiver_named_proto(ctx, global, "EventTarget");
    receiver_pass pass = {
        .ctx = ctx, .node_class = node_class, .attr_class = attr_class,
        .node_proto = node_proto,
        .made_setters = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL),
        .event_target_names = g_hash_table_new(g_direct_hash, g_direct_equal),
    };
    for (int k = 0; k < RECEIVER_KINDS; k++)
        pass.made[k] = g_hash_table_new(g_direct_hash, g_direct_equal);
    receiver_note_event_target_names(&pass, target_proto);
    receiver_check_interfaces(&pass, global, node_proto, attr_proto);
    for (int k = 0; k < RECEIVER_KINDS; k++) {
        GHashTableIter it;
        gpointer key, made;
        g_hash_table_iter_init(&it, pass.made[k]);
        while (g_hash_table_iter_next(&it, &key, &made))
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, made));
        g_hash_table_destroy(pass.made[k]);
    }
    GHashTableIter setters;
    gpointer setter_key, setter;
    g_hash_table_iter_init(&setters, pass.made_setters);
    while (g_hash_table_iter_next(&setters, &setter_key, &setter))
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, setter));
    g_hash_table_destroy(pass.made_setters);
    receiver_free_event_target_names(&pass);
    JS_FreeValue(ctx, target_proto);
    JS_FreeValue(ctx, attr_proto);
    JS_FreeValue(ctx, node_proto);
}
