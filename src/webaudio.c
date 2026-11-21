/* Northstar Web Browser — Web Audio graph rendering for AudioContext and OfflineAudioContext.
 * Renders oscillator, buffer source, constant, gain, biquad, delay, compressor and
 * waveshaper nodes in 128-frame blocks with AudioParam automation.  A realtime
 * AudioContext renders a little ahead of the output clock into a mixer stream.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "quickjs_compat.h"
#include "js_internal.h"

#include <math.h>
#include <string.h>

#include <glib.h>

#include "audio/audio.h"
#include "config.h"
#include "mainctx.h"

#define NS_WA_MAX_DEPTH        32
#define NS_WA_MAX_NODE_RENDERS 4096     /* per render block */
#define NS_WA_BLOCK            128
#define NS_WA_MAX_EVENTS       4096     /* per AudioParam */
#define NS_WA_MAX_DELAY_S      30.0
#define NS_WA_RT_RATE          44100.0
#define NS_WA_RT_LEAD_S        0.1      /* how far ahead of the output clock to render */
#define NS_WA_RT_CATCHUP_S     0.5      /* most audio rendered in one poll */
#define NS_WA_RT_POLL_MS       10
#define NS_WA_RT_MAX_CONTEXTS  16       /* per realm, like Chrome's limit on live contexts */

enum {
    NS_WA_EV_SET,
    NS_WA_EV_LINEAR,
    NS_WA_EV_EXPO,
    NS_WA_EV_TARGET,
};

typedef struct ns_wa_walk {
    int     depth;
    guint   renders_left;
    double  blk;        /* unique id of the block being rendered */
    double  t0;         /* context time of the block's first frame */
} ns_wa_walk;

static void ns_wa_render(JSContext *ctx, JSValueConst node, uint32_t frames,
                         double rate, float *out, ns_wa_walk *walk);

/* Returns a fresh id for a render block; nodes use it to tell blocks apart. */
static double
ns_wa_next_block(void)
{
    static gsize serial;
    return (double)(g_atomic_pointer_add(&serial, 1) + 1);
}

/* Reads a numeric property, returning dflt when missing or not convertible. */
static double
ns_wa_num(JSContext *ctx, JSValueConst obj, const char *name, double dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    double d = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToFloat64(ctx, &d, v) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        d = dflt;
    }
    JS_FreeValue(ctx, v);
    return d;
}

/* Returns a property as a newly allocated string ("" if absent); caller frees. */
static char *
ns_wa_str(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    char *r = g_strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return r;
}

/* Borrows a typed array's aligned data pointer and element count; NULL if invalid.
 * The pointer is only valid until script next runs. */
static void *
ns_wa_typed(JSContext *ctx, JSValueConst arr, int type, size_t elem,
            uint32_t *count)
{
    *count = 0;
    if (!JS_IsObject(arr) || JS_GetTypedArrayType(arr) != type) return NULL;
    const uint8_t *data = NULL;
    size_t len = 0;
    JSValue hold = JS_UNDEFINED;
    ns_js_buffer_source_bytes(ctx, arr, &data, &len, &hold);
    JS_FreeValue(ctx, hold);
    if (!data || ((uintptr_t)data % elem) != 0) return NULL;
    *count = (uint32_t)MIN(len / elem, (size_t)G_MAXUINT32);
    return (void *)data;
}

static float *
ns_wa_float32(JSContext *ctx, JSValueConst arr, uint32_t *count)
{
    return ns_wa_typed(ctx, arr, JS_TYPED_ARRAY_FLOAT32, sizeof(float), count);
}

static double *
ns_wa_float64(JSContext *ctx, JSValueConst arr, uint32_t *count)
{
    return ns_wa_typed(ctx, arr, JS_TYPED_ARRAY_FLOAT64, sizeof(double), count);
}

/* Creates a zeroed typed array via the realm's constructor (e.g. "Float64Array"). */
static JSValue
ns_wa_new_typed(JSContext *ctx, const char *ctor_name, uint32_t n)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, ctor_name);
    JS_FreeValue(ctx, global);
    JSValue arg = JS_NewInt64(ctx, n);
    JSValue arr = JS_IsFunction(ctx, ctor)
        ? JS_CallConstructor(ctx, ctor, 1, &arg) : JS_UNDEFINED;
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        arr = JS_UNDEFINED;
    }
    return arr;
}

/* ---- AudioParam automation ------------------------------------------------
 * Events live in param._ev, a Float64Array of [type, value, time, timeConstant]
 * quads sorted by time, with param._evn in use.  param._base is the value
 * before the first event.  While events are pending the renderer writes the
 * computed value to param.value and remembers it in param._shown; a script
 * assignment to value (which no longer matches _shown) cancels the automation,
 * as setting value does in browsers. */

/* Reads the event count and drops the automation if script assigned value. */
static uint32_t
ns_wa_param_sync(JSContext *ctx, JSValueConst p)
{
    double count = ns_wa_num(ctx, p, "_evn", 0.0);
    if (!(count >= 1.0)) return 0;
    uint32_t n = count >= NS_WA_MAX_EVENTS ? NS_WA_MAX_EVENTS : (uint32_t)count;
    double shown = ns_wa_num(ctx, p, "_shown", NAN);
    double value = ns_wa_num(ctx, p, "value", NAN);
    if (!isnan(shown) && value != shown) {
        JS_SetPropertyStr(ctx, p, "_evn", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, p, "_shown", JS_UNDEFINED);
        return 0;
    }
    return n;
}

/* Value of the automation at time t.  With prune, drops events that no longer
 * matter (times only move forward while rendering) and returns the new count. */
static double
ns_wa_param_eval(double *ev, uint32_t *pn, double base, double t,
                 gboolean prune)
{
    uint32_t n = *pn;
    double pv = base, pt = 0.0;
    gboolean target = FALSE;
    double tv = 0.0, tc = 1.0, tt = 0.0, tv0 = 0.0;
    gboolean settled = FALSE;
    uint32_t settle = 0;
    double v = base;
    uint32_t i = 0;
    for (; i < n; i++) {
        const double *e = ev + 4 * i;
        int type = (int)e[0];
        double ev_v = e[1], et = e[2];
        gboolean ramp = type == NS_WA_EV_LINEAR || type == NS_WA_EV_EXPO;
        if (target && !ramp) {
            double until = MIN(et, t);
            pv = tv + (tv0 - tv) * exp(-(until - tt) / tc);
            pt = until;
            target = FALSE;
        } else if (target) {
            target = FALSE;     /* a ramp takes over from the target's start */
        }
        if (et > t) {
            double span = et - pt;
            double frac = span > 0 ? (t - pt) / span : 1.0;
            frac = CLAMP(frac, 0.0, 1.0);
            if (type == NS_WA_EV_LINEAR)
                v = pv + (ev_v - pv) * frac;
            else if (type == NS_WA_EV_EXPO && pv * ev_v > 0)
                v = pv * pow(ev_v / pv, frac);
            else
                v = pv;
            break;
        }
        if (type == NS_WA_EV_TARGET) {
            target = TRUE;
            tv = ev_v;
            tc = e[3] > 0 ? e[3] : 1e-9;
            tt = et;
            tv0 = pv;
            pt = et;
        } else {
            pv = ev_v;
            pt = et;
            settled = TRUE;
            settle = i;
        }
    }
    if (i == n) {
        v = target ? tv + (tv0 - tv) * exp(-(t - tt) / tc) : pv;
        if (prune && (!target || (t - tt) > 10.0 * tc)) {
            /* Every event has passed: the param holds a plain value again. */
            *pn = 0;
            return target && (t - tt) > 10.0 * tc ? tv : v;
        }
    }
    if (prune && settled) {
        if (settle > 0) {
            memmove(ev, ev + 4 * settle, (size_t)(n - settle) * 4 * sizeof(double));
            n -= settle;
        }
        ev[0] = NS_WA_EV_SET;
        *pn = n;
    }
    return v;
}

/* Reads node[name] as an AudioParam's value at time t, applying automation. */
static double
ns_wa_param_at(JSContext *ctx, JSValueConst node, const char *name,
               double dflt, double t)
{
    JSValue p = JS_GetPropertyStr(ctx, node, name);
    if (!JS_IsObject(p)) { JS_FreeValue(ctx, p); return dflt; }
    double v = ns_wa_num(ctx, p, "value", dflt);
    uint32_t n = ns_wa_param_sync(ctx, p);
    if (n) {
        double base = ns_wa_num(ctx, p, "_base", v);
        JSValue arr = JS_GetPropertyStr(ctx, p, "_ev");
        uint32_t cap = 0;
        double *ev = ns_wa_float64(ctx, arr, &cap);
        if (ev && n <= cap / 4) {
            v = ns_wa_param_eval(ev, &n, base, t, TRUE);
            if (!isfinite(v)) v = dflt;
            JS_SetPropertyStr(ctx, p, "_evn", JS_NewInt32(ctx, (int32_t)n));
            JS_SetPropertyStr(ctx, p, "value", JS_NewFloat64(ctx, v));
            JS_SetPropertyStr(ctx, p, "_shown",
                              n ? JS_NewFloat64(ctx, v) : JS_UNDEFINED);
        } else {
            JS_SetPropertyStr(ctx, p, "_evn", JS_NewInt32(ctx, 0));
        }
        JS_FreeValue(ctx, arr);
    }
    JS_FreeValue(ctx, p);
    return v;
}

/* Reads an argument as a finite double, throwing TypeError otherwise. */
static int
ns_wa_arg_finite(JSContext *ctx, int argc, JSValueConst *argv, int i,
                 double *out)
{
    double d = NAN;
    if (i < argc && JS_ToFloat64(ctx, &d, argv[i]) < 0) return -1;
    if (!isfinite(d)) {
        JS_ThrowTypeError(ctx, "AudioParam: argument %d is not a finite number",
                          i + 1);
        return -1;
    }
    *out = d;
    return 0;
}

/* Inserts an event after any existing events at the same time. */
static void
ns_wa_param_insert(JSContext *ctx, JSValueConst p, int type, double value,
                   double time, double tc)
{
    uint32_t n = ns_wa_param_sync(ctx, p);
    if (n == 0) {
        double cur = ns_wa_num(ctx, p, "value", 0.0);
        JS_SetPropertyStr(ctx, p, "_base", JS_NewFloat64(ctx, cur));
        /* A ramp or target with nothing before it starts now, from the
         * current value, not from time zero. */
        if (type != NS_WA_EV_SET) {
            JSValue ac = JS_GetPropertyStr(ctx, p, "_ac");
            double now = JS_IsObject(ac) ? ns_wa_num(ctx, ac, "currentTime", 0.0)
                                         : 0.0;
            JS_FreeValue(ctx, ac);
            if (isfinite(now) && now > 0 && now < time) {
                ns_wa_param_insert(ctx, p, NS_WA_EV_SET, cur, now, 0.0);
                n = ns_wa_param_sync(ctx, p);
            }
        }
    }
    if (n >= NS_WA_MAX_EVENTS) return;
    JSValue arr = JS_GetPropertyStr(ctx, p, "_ev");
    uint32_t cap = 0;
    if (!ns_wa_float64(ctx, arr, &cap) || cap < (n + 1) * 4) {
        uint32_t want = MAX(16u, (n + 1) * 8);
        JSValue grown = ns_wa_new_typed(ctx, "Float64Array", want);
        uint32_t gcap = 0, ocap = 0;
        double *gev = ns_wa_float64(ctx, grown, &gcap);
        const double *old = ns_wa_float64(ctx, arr, &ocap);
        if (!gev || gcap < want) {
            JS_FreeValue(ctx, grown);
            JS_FreeValue(ctx, arr);
            return;
        }
        if (old && n <= ocap / 4) memcpy(gev, old, (size_t)n * 4 * sizeof(double));
        else n = 0;
        JS_FreeValue(ctx, arr);
        JS_SetPropertyStr(ctx, p, "_ev", JS_DupValue(ctx, grown));
        arr = grown;
    }
    /* Borrow only now: setting _ev above may have run script. */
    double *ev = ns_wa_float64(ctx, arr, &cap);
    if (!ev || cap < (n + 1) * 4) {
        JS_FreeValue(ctx, arr);
        return;
    }
    uint32_t at = n;
    while (at > 0 && ev[4 * (at - 1) + 2] > time) at--;
    memmove(ev + 4 * (at + 1), ev + 4 * at, (size_t)(n - at) * 4 * sizeof(double));
    ev[4 * at + 0] = type;
    ev[4 * at + 1] = value;
    ev[4 * at + 2] = time;
    ev[4 * at + 3] = tc;
    JS_FreeValue(ctx, arr);
    JS_SetPropertyStr(ctx, p, "_evn", JS_NewInt32(ctx, (int32_t)(n + 1)));
    double shown = ns_wa_num(ctx, p, "value", 0.0);
    JS_SetPropertyStr(ctx, p, "_shown", JS_NewFloat64(ctx, shown));
}

/* Removes events at or after time; with hold, keeps the value reached there. */
static void
ns_wa_param_cancel(JSContext *ctx, JSValueConst p, double time, gboolean hold)
{
    uint32_t n = ns_wa_param_sync(ctx, p);
    if (!n) return;
    JSValue arr = JS_GetPropertyStr(ctx, p, "_ev");
    uint32_t cap = 0;
    double *ev = ns_wa_float64(ctx, arr, &cap);
    double held = NAN;
    if (ev && n <= cap / 4) {
        if (hold) {
            uint32_t m = n;
            double base = ns_wa_num(ctx, p, "_base", 0.0);
            ev = ns_wa_float64(ctx, arr, &cap);
            if (ev) held = ns_wa_param_eval(ev, &m, base, time, FALSE);
        }
        ev = ns_wa_float64(ctx, arr, &cap);
        uint32_t keep = 0;
        while (ev && keep < n && ev[4 * keep + 2] < time) keep++;
        n = ev ? keep : 0;
    } else {
        n = 0;
    }
    JS_FreeValue(ctx, arr);
    JS_SetPropertyStr(ctx, p, "_evn", JS_NewInt32(ctx, (int32_t)n));
    if (hold && isfinite(held))
        ns_wa_param_insert(ctx, p, NS_WA_EV_SET, held, time, 0.0);
}

/* AudioParam automation methods; magic selects the method.  Each returns this. */
static JSValue
ns_wa_param_method(JSContext *ctx, JSValueConst this_val, int argc,
                   JSValueConst *argv, int magic)
{
    double value = 0.0, time = 0.0, extra = 0.0;
    switch (magic) {
    case 0: /* setValueAtTime(value, startTime) */
    case 1: /* linearRampToValueAtTime(value, endTime) */
    case 2: /* exponentialRampToValueAtTime(value, endTime) */
        if (ns_wa_arg_finite(ctx, argc, argv, 0, &value) < 0 ||
            ns_wa_arg_finite(ctx, argc, argv, 1, &time) < 0)
            return JS_EXCEPTION;
        if (time < 0)
            return JS_ThrowRangeError(ctx, "AudioParam: time must not be negative");
        if (magic == 2 && value == 0.0)
            return JS_ThrowRangeError(ctx,
                "exponentialRampToValueAtTime: value must not be zero");
        ns_wa_param_insert(ctx, this_val,
                           magic == 0 ? NS_WA_EV_SET
                           : magic == 1 ? NS_WA_EV_LINEAR : NS_WA_EV_EXPO,
                           value, time, 0.0);
        break;
    case 3: /* setTargetAtTime(target, startTime, timeConstant) */
        if (ns_wa_arg_finite(ctx, argc, argv, 0, &value) < 0 ||
            ns_wa_arg_finite(ctx, argc, argv, 1, &time) < 0 ||
            ns_wa_arg_finite(ctx, argc, argv, 2, &extra) < 0)
            return JS_EXCEPTION;
        if (time < 0 || extra < 0)
            return JS_ThrowRangeError(ctx,
                "setTargetAtTime: time and timeConstant must not be negative");
        if (extra == 0)
            ns_wa_param_insert(ctx, this_val, NS_WA_EV_SET, value, time, 0.0);
        else
            ns_wa_param_insert(ctx, this_val, NS_WA_EV_TARGET, value, time, extra);
        break;
    case 4: { /* setValueCurveAtTime(values, startTime, duration) */
        if (ns_wa_arg_finite(ctx, argc, argv, 1, &time) < 0 ||
            ns_wa_arg_finite(ctx, argc, argv, 2, &extra) < 0)
            return JS_EXCEPTION;
        if (time < 0 || extra <= 0)
            return JS_ThrowRangeError(ctx,
                "setValueCurveAtTime: bad start time or duration");
        uint32_t len = argc >= 1 ? ns_js_array_length(ctx, argv[0]) : 0;
        if (len < 2)
            return JS_ThrowTypeError(ctx,
                "setValueCurveAtTime: curve needs at least two values");
        /* Sample the curve into at most 64 linear segments. */
        uint32_t steps = MIN(len - 1, 64u);
        for (uint32_t s = 0; s <= steps; s++) {
            uint32_t k = (uint32_t)((uint64_t)s * (len - 1) / steps);
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], k);
            double cv = 0.0;
            int rc = JS_ToFloat64(ctx, &cv, item);
            JS_FreeValue(ctx, item);
            if (rc < 0) return JS_EXCEPTION;
            if (!isfinite(cv)) cv = 0.0;
            ns_wa_param_insert(ctx, this_val,
                               s == 0 ? NS_WA_EV_SET : NS_WA_EV_LINEAR, cv,
                               time + extra * s / steps, 0.0);
        }
        break;
    }
    case 5: /* cancelScheduledValues(cancelTime) */
    case 6: /* cancelAndHoldAtTime(cancelTime) */
        if (ns_wa_arg_finite(ctx, argc, argv, 0, &time) < 0)
            return JS_EXCEPTION;
        if (time < 0)
            return JS_ThrowRangeError(ctx, "AudioParam: time must not be negative");
        ns_wa_param_cancel(ctx, this_val, time, magic == 6);
        break;
    default:
        break;
    }
    return JS_DupValue(ctx, this_val);
}

void
ns_webaudio_bind_param(JSContext *ctx, JSValueConst param)
{
    static const struct { const char *name; int argc; } methods[] = {
        { "setValueAtTime", 2 },
        { "linearRampToValueAtTime", 2 },
        { "exponentialRampToValueAtTime", 2 },
        { "setTargetAtTime", 3 },
        { "setValueCurveAtTime", 3 },
        { "cancelScheduledValues", 1 },
        { "cancelAndHoldAtTime", 1 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(methods); i++)
        JS_SetPropertyStr(ctx, param, methods[i].name,
            JS_NewCFunctionMagic(ctx, ns_wa_param_method, methods[i].name,
                                 methods[i].argc, JS_CFUNC_generic_magic,
                                 (int)i));
}

/* ---- Node rendering ------------------------------------------------------- */

/* Per-node state that survives between blocks: a Float64Array of 2n doubles
 * (state at block start, state at block end).  A node rendered twice in one
 * block (fan-out) starts both times from the block-start state, so it advances
 * once.  Call after the node's other property reads: the pointer is borrowed
 * and stays valid until *hold is freed, as long as no script runs. */
static double *
ns_wa_state(JSContext *ctx, JSValueConst node, uint32_t n,
            const ns_wa_walk *walk, JSValue *hold)
{
    *hold = JS_UNDEFINED;
    JSValue arr = JS_GetPropertyStr(ctx, node, "_st");
    uint32_t count = 0;
    if (!ns_wa_float64(ctx, arr, &count) || count != 2 * n) {
        JS_FreeValue(ctx, arr);
        arr = ns_wa_new_typed(ctx, "Float64Array", 2 * n);
        JS_SetPropertyStr(ctx, node, "_st", JS_DupValue(ctx, arr));
    }
    gboolean new_block = ns_wa_num(ctx, node, "_sblk", -1.0) != walk->blk;
    if (new_block)
        JS_SetPropertyStr(ctx, node, "_sblk", JS_NewFloat64(ctx, walk->blk));
    double *d = ns_wa_float64(ctx, arr, &count);
    if (!d || count != 2 * n) { JS_FreeValue(ctx, arr); return NULL; }
    *hold = arr;
    if (new_block) memcpy(d, d + n, n * sizeof(double));
    memcpy(d + n, d, n * sizeof(double));
    return d + n;
}

/* Converts a source node's start/stop times into a frame range of this block.
 * A source that was never started plays nothing.  Returns TRUE once the source
 * has stopped for good, so its output can be dropped from the graph. */
static gboolean
ns_wa_window(JSContext *ctx, JSValueConst node, uint32_t frames, double rate,
             const ns_wa_walk *walk, uint32_t *first, uint32_t *last)
{
    double start = ns_wa_num(ctx, node, "_startTime", -1.0);
    double stop = ns_wa_num(ctx, node, "_stopTime", -1.0);
    *first = *last = 0;
    if (start < 0) return FALSE;
    double t0 = walk->t0;
    double t1 = t0 + frames / rate;
    if (stop >= 0 && stop <= t0) return TRUE;
    double f = (start - t0) * rate;
    *first = f <= 0 ? 0 : start >= t1 ? frames : (uint32_t)ceil(f);
    *last = frames;
    if (stop >= 0 && stop < t1) {
        double e = (stop - t0) * rate;
        *last = e <= 0 ? 0 : (uint32_t)ceil(e);
    }
    if (*first > frames) *first = frames;
    if (*last > frames) *last = frames;
    if (*last < *first) *last = *first;
    return FALSE;
}

/* Renders a sine/square/sawtooth/triangle oscillator with detune, clamped to Nyquist. */
static gboolean
ns_wa_oscillator(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out, const ns_wa_walk *walk)
{
    uint32_t first, last;
    if (ns_wa_window(ctx, node, frames, rate, walk, &first, &last)) return TRUE;
    if (first >= last) return FALSE;
    double freq = ns_wa_param_at(ctx, node, "frequency", 440.0, walk->t0);
    double detune = ns_wa_param_at(ctx, node, "detune", 0.0, walk->t0);
    double f = freq * pow(2.0, detune / 1200.0);
    if (!(f > 0)) f = 0.0;
    if (f > rate * 0.5) f = rate * 0.5;
    char *type = ns_wa_str(ctx, node, "type");
    int shape = strcmp(type, "square") == 0 ? 1
              : strcmp(type, "sawtooth") == 0 ? 2
              : strcmp(type, "triangle") == 0 ? 3 : 0;
    g_free(type);
    JSValue hold;
    double *st = ns_wa_state(ctx, node, 1, walk, &hold);
    if (!st) return FALSE;
    double step = f / rate, phase = st[0];
    if (!(phase >= 0 && phase < 1)) phase = 0.0;
    for (uint32_t i = first; i < last; i++) {
        double v;
        switch (shape) {
        case 1:  v = phase < 0.5 ? 1.0 : -1.0; break;
        case 2:  v = 2.0 * phase - 1.0; break;
        case 3:  v = phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase; break;
        default: v = sin(2.0 * G_PI * phase); break;
        }
        out[i] = (float)v;
        phase += step;
        if (phase >= 1.0) phase -= floor(phase);
    }
    st[0] = phase;
    JS_FreeValue(ctx, hold);
    return FALSE;
}

/* Renders channel 0 of an AudioBufferSourceNode with playbackRate, resampling and loop. */
static gboolean
ns_wa_buffer_source(JSContext *ctx, JSValueConst node, uint32_t frames,
                    double rate, float *out, const ns_wa_walk *walk)
{
    uint32_t first, last;
    if (ns_wa_window(ctx, node, frames, rate, walk, &first, &last)) return TRUE;
    if (first >= last) return FALSE;
    JSValue buf = JS_GetPropertyStr(ctx, node, "buffer");
    if (!JS_IsObject(buf)) { JS_FreeValue(ctx, buf); return FALSE; }
    double ratio = ns_wa_param_at(ctx, node, "playbackRate", 1.0, walk->t0);
    double brate = ns_wa_num(ctx, buf, "sampleRate", rate);
    if (!(ratio > 0)) ratio = 1.0;
    double step = ratio * (brate > 0 ? brate / rate : 1.0);
    if (!isfinite(step)) step = 1.0;
    JSValue loop_v = JS_GetPropertyStr(ctx, node, "loop");
    gboolean loop = JS_ToBool(ctx, loop_v);
    JS_FreeValue(ctx, loop_v);
    JSValue chans = JS_GetPropertyStr(ctx, buf, "_chans");
    JSValue ch0 = JS_GetPropertyUint32(ctx, chans, 0);
    JSValue hold;
    double *st = ns_wa_state(ctx, node, 1, walk, &hold);
    uint32_t n = 0;
    const float *src = st ? ns_wa_float32(ctx, ch0, &n) : NULL;
    gboolean done = FALSE;
    if (src && n) {
        double pos = st[0];
        if (!(pos >= 0)) pos = 0.0;
        for (uint32_t i = first; i < last; i++) {
            if (pos >= n) {
                if (!loop) { done = TRUE; break; }
                pos = fmod(pos, (double)n);
            }
            out[i] = src[(uint32_t)pos];
            pos += step;
        }
        if (!loop && pos >= n) done = TRUE;
        st[0] = pos;
    }
    JS_FreeValue(ctx, hold);
    JS_FreeValue(ctx, ch0);
    JS_FreeValue(ctx, chans);
    JS_FreeValue(ctx, buf);
    return done;
}

/* Renders each connected input node and mixes them into out.  Sources that
 * have finished for good are dropped from the inputs, as a browser would
 * collect them, so long-running pages do not keep rendering dead notes. */
static void
ns_wa_sum_inputs(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out, ns_wa_walk *walk)
{
    JSValue ins = JS_GetPropertyStr(ctx, node, "_inputs");
    if (!JS_IsObject(ins)) { JS_FreeValue(ctx, ins); return; }
    uint32_t n = ns_js_array_length(ctx, ins);
    if (!n) { JS_FreeValue(ctx, ins); return; }
    float tmp[NS_WA_BLOCK];
    if (frames > NS_WA_BLOCK) frames = NS_WA_BLOCK;
    GPtrArray *finished = NULL;
    walk->depth++;
    for (uint32_t i = 0; i < n && walk->renders_left > 0; i++) {
        JSValue src = JS_GetPropertyUint32(ctx, ins, i);
        if (JS_IsObject(src)) {
            memset(tmp, 0, frames * sizeof(float));
            walk->renders_left--;
            char *kind = ns_wa_str(ctx, src, "_kind");
            gboolean done = FALSE;
            if (walk->depth <= NS_WA_MAX_DEPTH) {
                if (strcmp(kind, "oscillator") == 0)
                    done = ns_wa_oscillator(ctx, src, frames, rate, tmp, walk);
                else if (strcmp(kind, "buffersource") == 0)
                    done = ns_wa_buffer_source(ctx, src, frames, rate, tmp, walk);
                else
                    ns_wa_render(ctx, src, frames, rate, tmp, walk);
            }
            g_free(kind);
            for (uint32_t j = 0; j < frames; j++) out[j] += tmp[j];
            if (done) {
                if (!finished) finished = g_ptr_array_new();
                g_ptr_array_add(finished, JS_VALUE_GET_PTR(src));
            }
        }
        JS_FreeValue(ctx, src);
    }
    walk->depth--;
    if (finished) {
        JSValue kept = JS_NewArray(ctx);
        uint32_t k = 0;
        n = ns_js_array_length(ctx, ins);
        for (uint32_t i = 0; i < n; i++) {
            JSValue src = JS_GetPropertyUint32(ctx, ins, i);
            gboolean drop = FALSE;
            for (guint f = 0; JS_IsObject(src) && f < finished->len; f++)
                if (g_ptr_array_index(finished, f) == JS_VALUE_GET_PTR(src))
                    drop = TRUE;
            if (drop) JS_FreeValue(ctx, src);
            else JS_SetPropertyUint32(ctx, kept, k++, src);
        }
        JS_SetPropertyStr(ctx, node, "_inputs", kept);
        g_ptr_array_free(finished, TRUE);
    }
    JS_FreeValue(ctx, ins);
}

/* Applies a soft-knee dynamics compressor with attack/release envelope smoothing. */
static void
ns_wa_compressor(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out, const ns_wa_walk *walk)
{
    double t0 = walk->t0;
    double threshold = ns_wa_param_at(ctx, node, "threshold", -24.0, t0);
    double knee = ns_wa_param_at(ctx, node, "knee", 30.0, t0);
    double ratio = ns_wa_param_at(ctx, node, "ratio", 12.0, t0);
    double attack = ns_wa_param_at(ctx, node, "attack", 0.003, t0);
    double release = ns_wa_param_at(ctx, node, "release", 0.25, t0);
    if (ratio < 1.0) ratio = 1.0;
    if (knee < 0.0) knee = 0.0;
    double atk = attack > 0 ? exp(-1.0 / (attack * rate)) : 0.0;
    double rel = release > 0 ? exp(-1.0 / (release * rate)) : 0.0;
    JSValue hold;
    double *st = ns_wa_state(ctx, node, 1, walk, &hold);
    if (!st) return;
    double env = isfinite(st[0]) ? st[0] : 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = fabs((double)out[i]);
        double db = x > 1e-9 ? 20.0 * log10(x) : -180.0;
        double over = db - threshold;
        double reduction;
        if (knee > 0 && over > -knee * 0.5 && over < knee * 0.5) {
            double t = over + knee * 0.5;
            reduction = (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
        } else if (over <= 0) {
            reduction = 0.0;
        } else {
            reduction = over * (1.0 / ratio - 1.0);
        }
        double coeff = reduction < env ? atk : rel;
        env = coeff * env + (1.0 - coeff) * reduction;
        out[i] = (float)((double)out[i] * pow(10.0, env / 20.0));
    }
    st[0] = env;
    JS_FreeValue(ctx, hold);
}

/* Applies a biquad filter (Audio EQ Cookbook coefficients); lowpass by default. */
static void
ns_wa_biquad(JSContext *ctx, JSValueConst node, uint32_t frames,
             double rate, float *out, const ns_wa_walk *walk)
{
    double t0 = walk->t0;
    double f0 = ns_wa_param_at(ctx, node, "frequency", 350.0, t0);
    double q = ns_wa_param_at(ctx, node, "Q", 1.0, t0);
    double gain_db = ns_wa_param_at(ctx, node, "gain", 0.0, t0);
    char *type = ns_wa_str(ctx, node, "type");
    if (!(f0 > 0)) f0 = 1.0;
    if (f0 > rate * 0.5) f0 = rate * 0.5;
    if (!(q > 0)) q = 1e-4;
    double w0 = 2.0 * G_PI * f0 / rate;
    double cw = cos(w0), sw = sin(w0), alpha = sw / (2.0 * q);
    double a0, a1, a2, b0, b1, b2;
    if (strcmp(type, "highpass") == 0) {
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "bandpass") == 0) {
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "notch") == 0) {
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "allpass") == 0) {
        b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "peaking") == 0) {
        double A = pow(10.0, gain_db / 40.0);
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    }
    g_free(type);
    JSValue hold;
    double *st = ns_wa_state(ctx, node, 4, walk, &hold);
    if (!st) return;
    double x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    if (!isfinite(x1 + x2 + y1 + y2)) x1 = x2 = y1 = y2 = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = out[i];
        double y = (b0 / a0) * x + (b1 / a0) * x1 + (b2 / a0) * x2
                 - (a1 / a0) * y1 - (a2 / a0) * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        out[i] = (float)y;
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
    JS_FreeValue(ctx, hold);
}

/* Delays the signal by delayTime through a ring buffer kept on the node. */
static void
ns_wa_delay(JSContext *ctx, JSValueConst node, uint32_t frames,
            double rate, float *out, const ns_wa_walk *walk)
{
    double t = ns_wa_param_at(ctx, node, "delayTime", 0.0, walk->t0);
    double max_t = ns_wa_num(ctx, node, "_maxDelay", 1.0);
    if (!(max_t > 0)) max_t = 1.0;
    if (max_t > NS_WA_MAX_DELAY_S) max_t = NS_WA_MAX_DELAY_S;
    if (!(t > 0)) t = 0.0;
    if (t > max_t) t = max_t;
    uint32_t size = (uint32_t)ceil(max_t * rate) + NS_WA_BLOCK + 1;
    JSValue ring = JS_GetPropertyStr(ctx, node, "_ring");
    uint32_t count = 0;
    if (!ns_wa_float32(ctx, ring, &count) || count != size) {
        JS_FreeValue(ctx, ring);
        ring = ns_wa_new_typed(ctx, "Float32Array", size);
        JS_SetPropertyStr(ctx, node, "_ring", JS_DupValue(ctx, ring));
    }
    JSValue hold;
    double *st = ns_wa_state(ctx, node, 1, walk, &hold);
    float *buf = st ? ns_wa_float32(ctx, ring, &count) : NULL;
    if (!buf || count != size) {
        JS_FreeValue(ctx, hold);
        JS_FreeValue(ctx, ring);
        return;
    }
    uint32_t d = (uint32_t)(t * rate);
    uint32_t w = (uint32_t)st[0] % size;
    for (uint32_t i = 0; i < frames; i++) {
        buf[w] = out[i];
        uint32_t r = (w + size - d) % size;
        out[i] = buf[r];
        w = (w + 1) % size;
    }
    st[0] = w;
    JS_FreeValue(ctx, hold);
    JS_FreeValue(ctx, ring);
}

/* Maps samples through the WaveShaper curve with linear interpolation. */
static void
ns_wa_waveshaper(JSContext *ctx, JSValueConst node, uint32_t frames,
                 float *out)
{
    JSValue curve = JS_GetPropertyStr(ctx, node, "curve");
    uint32_t n = 0;
    const float *c = ns_wa_float32(ctx, curve, &n);
    if (c && n >= 2) {
        for (uint32_t i = 0; i < frames; i++) {
            double x = out[i];
            if (x < -1.0) x = -1.0;
            if (x > 1.0) x = 1.0;
            double pos = (x + 1.0) * 0.5 * (n - 1);
            uint32_t k = (uint32_t)pos;
            if (k >= n - 1) { out[i] = c[n - 1]; continue; }
            double frac = pos - k;
            out[i] = (float)(c[k] * (1.0 - frac) + c[k + 1] * frac);
        }
    }
    JS_FreeValue(ctx, curve);
}

/* Renders one block (at most NS_WA_BLOCK frames) of a node by _kind. */
static void
ns_wa_render(JSContext *ctx, JSValueConst node, uint32_t frames,
             double rate, float *out, ns_wa_walk *walk)
{
    if (walk->depth > NS_WA_MAX_DEPTH || !JS_IsObject(node)) return;
    char *kind = ns_wa_str(ctx, node, "_kind");

    if (strcmp(kind, "oscillator") == 0) {
        ns_wa_oscillator(ctx, node, frames, rate, out, walk);
    } else if (strcmp(kind, "buffersource") == 0) {
        ns_wa_buffer_source(ctx, node, frames, rate, out, walk);
    } else if (strcmp(kind, "constant") == 0) {
        uint32_t first, last;
        ns_wa_window(ctx, node, frames, rate, walk, &first, &last);
        if (first < last) {
            double v = ns_wa_param_at(ctx, node, "offset", 1.0, walk->t0);
            for (uint32_t i = first; i < last; i++) out[i] = (float)v;
        }
    } else {
        ns_wa_sum_inputs(ctx, node, frames, rate, out, walk);
        if (strcmp(kind, "gain") == 0) {
            double g = ns_wa_param_at(ctx, node, "gain", 1.0, walk->t0);
            for (uint32_t i = 0; i < frames; i++) out[i] = (float)(out[i] * g);
        } else if (strcmp(kind, "compressor") == 0) {
            ns_wa_compressor(ctx, node, frames, rate, out, walk);
        } else if (strcmp(kind, "biquad") == 0) {
            ns_wa_biquad(ctx, node, frames, rate, out, walk);
        } else if (strcmp(kind, "delay") == 0) {
            ns_wa_delay(ctx, node, frames, rate, out, walk);
        } else if (strcmp(kind, "waveshaper") == 0) {
            ns_wa_waveshaper(ctx, node, frames, out);
        } else if (strcmp(kind, "stereopanner") == 0 ||
                   strcmp(kind, "panner") == 0) {
            double pan = ns_wa_param_at(ctx, node, "pan", 0.0, walk->t0);
            double g = 1.0 - fabs(pan) * 0.5;
            for (uint32_t i = 0; i < frames; i++) out[i] = (float)(out[i] * g);
        }
    }
    g_free(kind);
}

/* Renders frames of the graph feeding destination, starting at context time t0. */
static void
ns_wa_render_span(JSContext *ctx, JSValueConst destination, uint32_t frames,
                  double rate, double t0, float *out)
{
    memset(out, 0, (size_t)frames * sizeof(float));
    for (uint32_t off = 0; off < frames; off += NS_WA_BLOCK) {
        uint32_t n = MIN((uint32_t)NS_WA_BLOCK, frames - off);
        ns_wa_walk walk = { 0, NS_WA_MAX_NODE_RENDERS, ns_wa_next_block(),
                            t0 + off / rate };
        ns_wa_render(ctx, destination, n, rate, out + off, &walk);
    }
    for (uint32_t i = 0; i < frames; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
        else if (out[i] != out[i]) out[i] = 0.0f;
    }
}

/* Renders the audio graph feeding destination into out, clamped to [-1, 1]. */
gboolean
ns_webaudio_render_offline(JSContext *ctx, JSValueConst destination,
                           uint32_t frames, double rate, float *out)
{
    if (!ctx || !out || !frames || !(rate > 0)) return FALSE;
    memset(out, 0, frames * sizeof(float));
    if (!JS_IsObject(destination)) return FALSE;
    ns_wa_render_span(ctx, destination, frames, rate, 0.0, out);
    return TRUE;
}

/* ---- Realtime AudioContext ------------------------------------------------ */

typedef struct ns_wa_rt {
    ns_js     *js;
    JSContext *ctx;
    JSValue    obj;
    guint      id;
    char       token[24];
    double     rate;
    guint64    rendered;        /* frames pushed to the mixer stream */
    gboolean   stream_ok;
    gboolean   running;
    gboolean   want_resume;     /* resume() called while autoplay was blocked */
    gboolean   statechange;     /* a statechange event is due */
    int        busy;            /* rendering; script may close it meanwhile */
    gboolean   dead;            /* closed while busy: free when done */
    gint64     wall_start;      /* fallback clock when no stream could open */
    double     wall_time;
} ns_wa_rt;

static gboolean ns_wa_poll(gpointer data);

static ns_wa_rt *
ns_wa_rt_find(ns_js *js, JSContext *ctx, JSValueConst obj)
{
    (void)ctx;
    if (!js || !js->webaudio_contexts || !JS_IsObject(obj)) return NULL;
    for (guint i = 0; i < js->webaudio_contexts->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_contexts, i);
        if (JS_VALUE_GET_PTR(rt->obj) == JS_VALUE_GET_PTR(obj)) return rt;
    }
    return NULL;
}

/* Context time in seconds: the mixer stream's played position, or a wall clock
 * that runs while the context does when no audio device is available. */
static double
ns_wa_rt_time(ns_wa_rt *rt)
{
    if (rt->stream_ok) {
        NsAudioStatus st;
        if (ns_audio_context_status(rt->js->audio_context, rt->token, &st) &&
            st.state == NS_AUDIO_PLAYER_READY)
            return MIN(st.position, (double)rt->rendered / rt->rate);
    }
    double t = rt->wall_time;
    if (rt->running && rt->wall_start)
        t += (double)(g_get_monotonic_time() - rt->wall_start) / 1e6;
    return t;
}

static void
ns_wa_set_state(ns_wa_rt *rt, const char *state)
{
    char *old = ns_wa_str(rt->ctx, rt->obj, "state");
    if (strcmp(old, state) != 0) {
        JS_SetPropertyStr(rt->ctx, rt->obj, "state", JS_NewString(rt->ctx, state));
        rt->statechange = TRUE;
    }
    g_free(old);
}

static void
ns_wa_poll_ensure(ns_js *js)
{
    if (!js->webaudio_poll_source)
        js->webaudio_poll_source =
            ns_engine_timeout_add(NS_WA_RT_POLL_MS, ns_wa_poll, js);
}

/* True if the autoplay policy lets this realm start sound now. */
static gboolean
ns_wa_allowed(ns_js *js)
{
    const ns_config *cfg = ns_config_get();
    return !cfg || cfg->autoplay_enabled || js->user_ever_activated ||
           ns_js_has_transient_activation(js);
}

static void ns_wa_rt_free(ns_wa_rt *rt);

/* Renders up to the lead time past the output clock and pushes it to the mixer.
 * Returns FALSE if script closed the context while it rendered (rt is freed). */
static gboolean
ns_wa_rt_pump(ns_wa_rt *rt)
{
    if (!rt->running || !rt->stream_ok || rt->busy) return TRUE;
    double now = ns_wa_rt_time(rt);
    guint64 want = (guint64)ceil((now + NS_WA_RT_LEAD_S) * rt->rate);
    guint64 cap = rt->rendered + (guint64)(NS_WA_RT_CATCHUP_S * rt->rate);
    if (want > cap) want = cap;
    if (want <= rt->rendered) return TRUE;
    guint64 frames64 = want - rt->rendered;
    frames64 = (frames64 + NS_WA_BLOCK - 1) / NS_WA_BLOCK * NS_WA_BLOCK;
    uint32_t frames = (uint32_t)frames64;
    float *buf = g_try_new(float, frames);
    if (!buf) return TRUE;
    rt->busy++;
    JSValue dest = JS_GetPropertyStr(rt->ctx, rt->obj, "destination");
    ns_wa_render_span(rt->ctx, dest, frames, rt->rate,
                      (double)rt->rendered / rt->rate, buf);
    JS_FreeValue(rt->ctx, dest);
    rt->busy--;
    if (rt->dead) {
        g_free(buf);
        ns_wa_rt_free(rt);
        return FALSE;
    }
    if (ns_audio_context_stream_push(rt->js->audio_context, rt->token, buf,
                                     (int)frames, 1))
        rt->rendered += frames;
    g_free(buf);
    return TRUE;
}

/* Moves a context to running: opens its mixer stream and starts output.
 * Returns FALSE if script closed the context meanwhile (rt is freed). */
static gboolean
ns_wa_rt_start(ns_wa_rt *rt)
{
    if (rt->running) return TRUE;
    rt->want_resume = FALSE;
    if (!rt->stream_ok) {
        NsAudioContext *audio = ns_js_media_audio_context(rt->js);
        rt->stream_ok = audio &&
            ns_audio_context_open_stream(audio, rt->token, (int)rt->rate, 1);
        if (rt->stream_ok) ns_audio_context_stream_end(audio, rt->token, FALSE);
    }
    rt->running = TRUE;
    rt->wall_start = g_get_monotonic_time();
    ns_wa_set_state(rt, "running");
    ns_js *js = rt->js;
    ns_wa_poll_ensure(js);
    if (!ns_wa_rt_pump(rt)) return FALSE;
    if (rt->stream_ok) ns_audio_context_play(js->audio_context, rt->token);
    return TRUE;
}

static void
ns_wa_rt_stop(ns_wa_rt *rt, const char *state)
{
    if (rt->running) {
        rt->wall_time += (double)(g_get_monotonic_time() - rt->wall_start) / 1e6;
        rt->wall_start = 0;
        rt->running = FALSE;
        if (rt->stream_ok)
            ns_audio_context_pause(rt->js->audio_context, rt->token);
    }
    ns_wa_set_state(rt, state);
}

static void
ns_wa_rt_free(ns_wa_rt *rt)
{
    if (rt->stream_ok && rt->js->audio_context)
        ns_audio_context_close(rt->js->audio_context, rt->token);
    JS_FreeValue(rt->ctx, rt->obj);
    JS_FreeContext(rt->ctx);
    g_free(rt);
}

/* Engine timer: keeps running contexts fed and delivers statechange events. */
static gboolean
ns_wa_poll(gpointer data)
{
    ns_js *js = data;
    gboolean keep = FALSE;
    gboolean fired = FALSE;
    GPtrArray *list = js->webaudio_contexts;
    /* Rendering reads page objects, so script can close contexts meanwhile;
     * walk a snapshot and skip any that are no longer listed. */
    GPtrArray *snap = list ? g_ptr_array_copy(list, NULL, NULL) : NULL;
    for (guint i = 0; snap && i < snap->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(snap, i);
        if (!js->webaudio_contexts ||
            !g_ptr_array_find(js->webaudio_contexts, rt, NULL))
            continue;
        if (rt->want_resume && ns_wa_allowed(js) && !ns_wa_rt_start(rt))
            continue;
        if (!ns_wa_rt_pump(rt)) continue;
        if (rt->running || rt->want_resume) keep = TRUE;
    }
    if (snap) g_ptr_array_free(snap, TRUE);
    list = js->webaudio_contexts;
    /* Events can run script that closes contexts, so fire them last, one pass,
     * holding a reference to each target. */
    for (guint i = 0; list && i < list->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(list, i);
        if (!rt->statechange) continue;
        rt->statechange = FALSE;
        JSContext *ctx = rt->ctx;
        JSValue obj = JS_DupValue(ctx, rt->obj);
        ns_js_target_fire_event(ctx, obj, "statechange");
        JS_FreeValue(ctx, obj);
        fired = TRUE;
        list = js->webaudio_contexts;
    }
    if (fired) ns_drain_microtasks(js);
    for (guint i = 0; js->webaudio_contexts && i < js->webaudio_contexts->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_contexts, i);
        if (rt->running || rt->want_resume || rt->statechange) keep = TRUE;
    }
    if (!keep) {
        js->webaudio_poll_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static JSValue
ns_wa_resolved(JSContext *ctx)
{
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) return promise;
    JSValue r = JS_Call(ctx, resolvers[0], JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static JSValue
ns_wa_closed_error(JSContext *ctx)
{
    return ns_promise_reject_dom(ctx, "InvalidStateError",
                                 "The AudioContext is closed");
}

static JSValue
ns_wa_get_current_time(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_wa_rt *rt = ns_wa_rt_find(js_from_ctx(ctx), ctx, this_val);
    if (!rt) return JS_NewFloat64(ctx, ns_wa_num(ctx, this_val, "_ct", 0.0));
    /* Like browsers, report time in whole render quanta. */
    double t = ns_wa_rt_time(rt);
    return JS_NewFloat64(ctx, floor(t * rt->rate / NS_WA_BLOCK) * NS_WA_BLOCK /
                              rt->rate);
}

static JSValue
ns_wa_resume(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    ns_wa_rt *rt = ns_wa_rt_find(js, ctx, this_val);
    if (!rt) return ns_wa_closed_error(ctx);
    if (ns_wa_allowed(js)) {
        if (!ns_wa_rt_start(rt)) return ns_wa_resolved(ctx);
    } else {
        rt->want_resume = TRUE;
        ns_wa_poll_ensure(js);
    }
    return ns_wa_resolved(ctx);
}

static JSValue
ns_wa_suspend(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    ns_wa_rt *rt = ns_wa_rt_find(js, ctx, this_val);
    if (!rt) return ns_wa_closed_error(ctx);
    rt->want_resume = FALSE;
    ns_wa_rt_stop(rt, "suspended");
    ns_wa_poll_ensure(js);
    return ns_wa_resolved(ctx);
}

static JSValue
ns_wa_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    ns_wa_rt *rt = ns_wa_rt_find(js, ctx, this_val);
    if (!rt) return ns_wa_closed_error(ctx);
    JS_SetPropertyStr(ctx, this_val, "_ct",
                      JS_NewFloat64(ctx, ns_wa_rt_time(rt)));
    ns_wa_rt_stop(rt, "closed");
    g_ptr_array_remove(js->webaudio_contexts, rt);
    JSValue obj = JS_DupValue(ctx, rt->obj);
    gboolean changed = rt->statechange;
    if (rt->busy) {
        if (rt->stream_ok) ns_audio_context_close(js->audio_context, rt->token);
        rt->stream_ok = FALSE;
        rt->dead = TRUE;
    } else {
        ns_wa_rt_free(rt);
    }
    JSValue promise = ns_wa_resolved(ctx);
    if (changed) ns_js_target_fire_event(ctx, obj, "statechange");
    JS_FreeValue(ctx, obj);
    return promise;
}

/* Makes an AudioContext object (built by js.c) produce sound: binds resume,
 * suspend, close and a live currentTime, and starts it when autoplay allows. */
JSValue
ns_webaudio_realtime_init(JSContext *ctx, JSValue a)
{
    ns_js *js = js_from_ctx(ctx);
    if (!js) return a;
    if (!js->webaudio_contexts) js->webaudio_contexts = g_ptr_array_new();
    if (js->webaudio_contexts->len >= NS_WA_RT_MAX_CONTEXTS) {
        JS_FreeValue(ctx, a);
        return ns_throw_dom_exception(ctx, "NotSupportedError", 9,
            "Too many AudioContexts are open; close some first");
    }
    ns_wa_rt *rt = g_new0(ns_wa_rt, 1);
    rt->js = js;
    rt->ctx = JS_DupContext(ctx);
    rt->obj = JS_DupValue(ctx, a);
    rt->id = ++js->next_webaudio_id;
    rt->rate = NS_WA_RT_RATE;
    g_snprintf(rt->token, sizeof rt->token, "wa%u", rt->id);
    g_ptr_array_add(js->webaudio_contexts, rt);

    JS_SetPropertyStr(ctx, a, "sampleRate", JS_NewFloat64(ctx, rt->rate));
    JS_SetPropertyStr(ctx, a, "state", JS_NewString(ctx, "suspended"));
    JSAtom atom = JS_NewAtom(ctx, "currentTime");
    JS_DefinePropertyGetSet(ctx, a, atom,
        JS_NewCFunction2(ctx, ns_wa_get_current_time, "get currentTime", 0,
                         JS_CFUNC_generic, 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    ns_bind_fn(ctx, a, "resume",  ns_wa_resume,  0);
    ns_bind_fn(ctx, a, "suspend", ns_wa_suspend, 0);
    ns_bind_fn(ctx, a, "close",   ns_wa_close,   0);
    if (ns_wa_allowed(js)) ns_wa_rt_start(rt);
    return a;   /* the caller's reference; rt holds its own */
}

/* Silences every realtime context, e.g. when the page goes into the bfcache. */
void
ns_webaudio_suspend_all(ns_js *js)
{
    for (guint i = 0; js && js->webaudio_contexts &&
                      i < js->webaudio_contexts->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_contexts, i);
        rt->want_resume = FALSE;
        ns_wa_rt_stop(rt, "suspended");
    }
}

/* Closes every realtime context and stops the poll timer. */
void
ns_webaudio_teardown(ns_js *js)
{
    if (!js) return;
    if (js->webaudio_poll_source) {
        ns_engine_source_remove(js->webaudio_poll_source);
        js->webaudio_poll_source = 0;
    }
    if (js->webaudio_contexts) {
        GPtrArray *list = js->webaudio_contexts;
        js->webaudio_contexts = NULL;
        for (guint i = 0; i < list->len; i++)
            ns_wa_rt_free(g_ptr_array_index(list, i));
        g_ptr_array_free(list, TRUE);
    }
}
