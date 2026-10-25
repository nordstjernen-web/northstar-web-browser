/* Northstar Web Browser — Web Cryptography API bindings: SubtleCrypto and CryptoKey (QuickJS).
 * Normalizes algorithms, imports/exports raw, SPKI, PKCS#8 and JWK keys over webcrypto.c.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "quickjs_compat.h"
#include "js_internal.h"
#include "webcrypto.h"

#include <math.h>
#include <string.h>

enum {
    NS_USAGE_ENCAPSULATE_KEY  = 1u << 8,
    NS_USAGE_ENCAPSULATE_BITS = 1u << 9,
    NS_USAGE_DECAPSULATE_KEY  = 1u << 10,
    NS_USAGE_DECAPSULATE_BITS = 1u << 11,
};

#define NS_USAGES_CIPHER (NS_USAGE_ENCRYPT | NS_USAGE_DECRYPT | \
                          NS_USAGE_WRAP | NS_USAGE_UNWRAP)
#define NS_USAGES_SIGN   (NS_USAGE_SIGN | NS_USAGE_VERIFY)
#define NS_USAGES_DERIVE (NS_USAGE_DERIVE_KEY | NS_USAGE_DERIVE_BITS)

static const struct {
    guint32 bit;
    const char *name;
} ns_wc_usages[] = {
    { NS_USAGE_ENCRYPT, "encrypt" },
    { NS_USAGE_DECRYPT, "decrypt" },
    { NS_USAGE_SIGN, "sign" },
    { NS_USAGE_VERIFY, "verify" },
    { NS_USAGE_DERIVE_KEY, "deriveKey" },
    { NS_USAGE_DERIVE_BITS, "deriveBits" },
    { NS_USAGE_WRAP, "wrapKey" },
    { NS_USAGE_UNWRAP, "unwrapKey" },
    { NS_USAGE_ENCAPSULATE_KEY, "encapsulateKey" },
    { NS_USAGE_ENCAPSULATE_BITS, "encapsulateBits" },
    { NS_USAGE_DECAPSULATE_KEY, "decapsulateKey" },
    { NS_USAGE_DECAPSULATE_BITS, "decapsulateBits" },
};

typedef enum {
    NS_OP_ENCRYPT,
    NS_OP_DECRYPT,
    NS_OP_SIGN,
    NS_OP_VERIFY,
    NS_OP_DIGEST,
    NS_OP_GENERATE_KEY,
    NS_OP_DERIVE_BITS,
    NS_OP_IMPORT_KEY,
    NS_OP_EXPORT_KEY,
    NS_OP_WRAP_KEY,
    NS_OP_UNWRAP_KEY,
    NS_OP_GET_KEY_LENGTH,
} ns_wc_op;

typedef enum {
    NS_P_NONE,
    NS_P_AES_LENGTH,
    NS_P_AES_CBC,
    NS_P_AES_CTR,
    NS_P_AES_GCM,
    NS_P_HMAC,
    NS_P_RSA_KEY_GEN,
    NS_P_HASH,
    NS_P_RSA_PSS,
    NS_P_RSA_OAEP,
    NS_P_EC_CURVE,
    NS_P_ECDH,
    NS_P_HKDF,
    NS_P_PBKDF2,
    NS_P_ED448,
} ns_wc_params;

typedef enum {
    NS_FMT_RAW,
    NS_FMT_SPKI,
    NS_FMT_PKCS8,
    NS_FMT_JWK,
} ns_wc_format;

static const struct {
    const char *name;
    guint8 op;
    guint8 params;
} ns_wc_registry[] = {
    { "RSASSA-PKCS1-v1_5", NS_OP_SIGN, NS_P_NONE },
    { "RSASSA-PKCS1-v1_5", NS_OP_VERIFY, NS_P_NONE },
    { "RSASSA-PKCS1-v1_5", NS_OP_GENERATE_KEY, NS_P_RSA_KEY_GEN },
    { "RSASSA-PKCS1-v1_5", NS_OP_IMPORT_KEY, NS_P_HASH },
    { "RSASSA-PKCS1-v1_5", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "RSA-PSS", NS_OP_SIGN, NS_P_RSA_PSS },
    { "RSA-PSS", NS_OP_VERIFY, NS_P_RSA_PSS },
    { "RSA-PSS", NS_OP_GENERATE_KEY, NS_P_RSA_KEY_GEN },
    { "RSA-PSS", NS_OP_IMPORT_KEY, NS_P_HASH },
    { "RSA-PSS", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "RSA-OAEP", NS_OP_ENCRYPT, NS_P_RSA_OAEP },
    { "RSA-OAEP", NS_OP_DECRYPT, NS_P_RSA_OAEP },
    { "RSA-OAEP", NS_OP_GENERATE_KEY, NS_P_RSA_KEY_GEN },
    { "RSA-OAEP", NS_OP_IMPORT_KEY, NS_P_HASH },
    { "RSA-OAEP", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "ECDSA", NS_OP_SIGN, NS_P_HASH },
    { "ECDSA", NS_OP_VERIFY, NS_P_HASH },
    { "ECDSA", NS_OP_GENERATE_KEY, NS_P_EC_CURVE },
    { "ECDSA", NS_OP_IMPORT_KEY, NS_P_EC_CURVE },
    { "ECDSA", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "ECDH", NS_OP_DERIVE_BITS, NS_P_ECDH },
    { "ECDH", NS_OP_GENERATE_KEY, NS_P_EC_CURVE },
    { "ECDH", NS_OP_IMPORT_KEY, NS_P_EC_CURVE },
    { "ECDH", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "Ed25519", NS_OP_SIGN, NS_P_NONE },
    { "Ed25519", NS_OP_VERIFY, NS_P_NONE },
    { "Ed25519", NS_OP_GENERATE_KEY, NS_P_NONE },
    { "Ed25519", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "Ed25519", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "X25519", NS_OP_DERIVE_BITS, NS_P_ECDH },
    { "X25519", NS_OP_GENERATE_KEY, NS_P_NONE },
    { "X25519", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "X25519", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "Ed448", NS_OP_SIGN, NS_P_ED448 },
    { "Ed448", NS_OP_VERIFY, NS_P_ED448 },
    { "Ed448", NS_OP_GENERATE_KEY, NS_P_NONE },
    { "Ed448", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "Ed448", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "X448", NS_OP_DERIVE_BITS, NS_P_ECDH },
    { "X448", NS_OP_GENERATE_KEY, NS_P_NONE },
    { "X448", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "X448", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "AES-CTR", NS_OP_ENCRYPT, NS_P_AES_CTR },
    { "AES-CTR", NS_OP_DECRYPT, NS_P_AES_CTR },
    { "AES-CTR", NS_OP_GENERATE_KEY, NS_P_AES_LENGTH },
    { "AES-CTR", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "AES-CTR", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "AES-CTR", NS_OP_GET_KEY_LENGTH, NS_P_AES_LENGTH },
    { "AES-CBC", NS_OP_ENCRYPT, NS_P_AES_CBC },
    { "AES-CBC", NS_OP_DECRYPT, NS_P_AES_CBC },
    { "AES-CBC", NS_OP_GENERATE_KEY, NS_P_AES_LENGTH },
    { "AES-CBC", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "AES-CBC", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "AES-CBC", NS_OP_GET_KEY_LENGTH, NS_P_AES_LENGTH },
    { "AES-GCM", NS_OP_ENCRYPT, NS_P_AES_GCM },
    { "AES-GCM", NS_OP_DECRYPT, NS_P_AES_GCM },
    { "AES-GCM", NS_OP_GENERATE_KEY, NS_P_AES_LENGTH },
    { "AES-GCM", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "AES-GCM", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "AES-GCM", NS_OP_GET_KEY_LENGTH, NS_P_AES_LENGTH },
    { "AES-KW", NS_OP_WRAP_KEY, NS_P_NONE },
    { "AES-KW", NS_OP_UNWRAP_KEY, NS_P_NONE },
    { "AES-KW", NS_OP_GENERATE_KEY, NS_P_AES_LENGTH },
    { "AES-KW", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "AES-KW", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "AES-KW", NS_OP_GET_KEY_LENGTH, NS_P_AES_LENGTH },
    { "HMAC", NS_OP_SIGN, NS_P_NONE },
    { "HMAC", NS_OP_VERIFY, NS_P_NONE },
    { "HMAC", NS_OP_GENERATE_KEY, NS_P_HMAC },
    { "HMAC", NS_OP_IMPORT_KEY, NS_P_HMAC },
    { "HMAC", NS_OP_EXPORT_KEY, NS_P_NONE },
    { "HMAC", NS_OP_GET_KEY_LENGTH, NS_P_HMAC },
    { "SHA-1", NS_OP_DIGEST, NS_P_NONE },
    { "SHA-256", NS_OP_DIGEST, NS_P_NONE },
    { "SHA-384", NS_OP_DIGEST, NS_P_NONE },
    { "SHA-512", NS_OP_DIGEST, NS_P_NONE },
    { "SHA3-256", NS_OP_DIGEST, NS_P_NONE },
    { "SHA3-384", NS_OP_DIGEST, NS_P_NONE },
    { "SHA3-512", NS_OP_DIGEST, NS_P_NONE },
    { "HKDF", NS_OP_DERIVE_BITS, NS_P_HKDF },
    { "HKDF", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "HKDF", NS_OP_GET_KEY_LENGTH, NS_P_NONE },
    { "PBKDF2", NS_OP_DERIVE_BITS, NS_P_PBKDF2 },
    { "PBKDF2", NS_OP_IMPORT_KEY, NS_P_NONE },
    { "PBKDF2", NS_OP_GET_KEY_LENGTH, NS_P_NONE },
};

typedef struct {
    guint8 *data;
    gsize len;
} ns_wc_buf;

typedef struct {
    const char *name;
    const char *hash;
    char *named_curve;
    gboolean has_length;
    guint32 length;
    guint32 modulus_length;
    guint32 tag_length;
    gboolean has_tag_length;
    guint32 salt_length;
    guint32 iterations;
    ns_wc_buf public_exponent;
    ns_wc_buf iv;
    ns_wc_buf counter;
    ns_wc_buf additional_data;
    ns_wc_buf label;
    ns_wc_buf salt;
    ns_wc_buf info;
    ns_wc_buf context;
    ns_crypto_key *public_key;
} ns_wc_alg;

typedef struct {
    char *kty, *use, *alg, *crv, *x, *y, *d, *n, *e, *p, *q, *dp, *dq, *qi, *k;
    gboolean has_ext;
    gboolean ext;
    gboolean has_key_ops;
    gboolean key_ops_repeated;
    guint32 key_ops;
    gboolean has_oth;
} ns_wc_jwk;

typedef struct {
    ns_crypto_key *key;
    JSValue algorithm;
    JSValue usages;
} ns_wc_key_ref;

static JSClassID ns_cryptokey_class_id;
static JSClassID ns_subtle_class_id;

static void
ns_cryptokey_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wc_key_ref *ref = JS_GetOpaque(val, ns_cryptokey_class_id);
    if (!ref) return;
    JS_FreeValueRT(rt, ref->algorithm);
    JS_FreeValueRT(rt, ref->usages);
    ns_crypto_key_unref(ref->key);
    g_free(ref);
}

/* GC mark hook keeping the CryptoKey's cached algorithm and usages objects alive. */
static void
ns_cryptokey_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wc_key_ref *ref = JS_GetOpaque(val, ns_cryptokey_class_id);
    if (!ref) return;
    JS_MarkValue(rt, ref->algorithm, mark_func);
    JS_MarkValue(rt, ref->usages, mark_func);
}

static JSClassDef ns_cryptokey_class = {
    "CryptoKey",
    .finalizer = ns_cryptokey_finalizer,
    .gc_mark = ns_cryptokey_mark,
};

static JSClassDef ns_subtle_class = {
    "SubtleCrypto",
    .finalizer = NULL,
};

/* Throws a TypeError or a DOMException with the given name and message. */
static JSValue
ns_wc_throw(JSContext *ctx, const char *name, const char *message)
{
    if (!strcmp(name, "TypeError"))
        return JS_ThrowTypeError(ctx, "%s", message);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMException");
    JS_FreeValue(ctx, global);
    JSValue err;
    if (JS_IsFunction(ctx, ctor)) {
        JSValue args[2] = { JS_NewString(ctx, message), JS_NewString(ctx, name) };
        err = JS_CallConstructor(ctx, ctor, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    } else {
        err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message));
    }
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(err)) return err;
    return JS_Throw(ctx, err);
}

/* Throws from an "XxxError: msg" string, using the prefix as DOMException name; frees err. */
static JSValue
ns_wc_throw_err(JSContext *ctx, char *err, const char *fallback)
{
    const char *name = fallback;
    const char *message = err ? err : fallback;
    char *owned = NULL;
    const char *colon = err ? strchr(err, ':') : NULL;
    if (colon && colon - err > 5 && colon - err < 40 &&
        !strncmp(colon - 5, "Error", 5)) {
        owned = g_strndup(err, (gsize)(colon - err));
        name = owned;
        message = colon + 1;
        while (*message == ' ') message++;
    }
    JSValue r = ns_wc_throw(ctx, name, message);
    g_free(owned);
    g_free(err);
    return r;
}

/* Zeroes and frees a buffer so key material does not linger in memory. */
static void
ns_wc_buf_clear(ns_wc_buf *b)
{
    if (b->data) {
        memset(b->data, 0, b->len);
        g_free(b->data);
    }
    b->data = NULL;
    b->len = 0;
}

/* Replaces the buffer with a NUL-terminated copy of data, wiping old contents. */
static void
ns_wc_buf_set(ns_wc_buf *b, const guint8 *data, gsize len)
{
    ns_wc_buf_clear(b);
    b->data = g_malloc(len + 1);
    if (len) memcpy(b->data, data, len);
    b->data[len] = 0;
    b->len = len;
}

/* True if v is an ArrayBuffer or ArrayBufferView. */
static gboolean
ns_wc_is_buffer_source(JSContext *ctx, JSValueConst v)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    JSValue hold;
    gboolean ok = ns_js_buffer_source_bytes(ctx, v, &data, &len, &hold);
    JS_FreeValue(ctx, hold);
    return ok;
}

/* Copies the bytes of a BufferSource into out (empty if not a buffer). */
static void
ns_wc_copy_buffer(JSContext *ctx, JSValueConst v, ns_wc_buf *out)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    JSValue hold;
    ns_js_buffer_source_bytes(ctx, v, &data, &len, &hold);
    ns_wc_buf_set(out, data, data ? len : 0);
    JS_FreeValue(ctx, hold);
}

/* Wipes and frees all owned members of a normalized algorithm. */
static void
ns_wc_alg_clear(ns_wc_alg *a)
{
    g_free(a->named_curve);
    ns_wc_buf_clear(&a->public_exponent);
    ns_wc_buf_clear(&a->iv);
    ns_wc_buf_clear(&a->counter);
    ns_wc_buf_clear(&a->additional_data);
    ns_wc_buf_clear(&a->label);
    ns_wc_buf_clear(&a->salt);
    ns_wc_buf_clear(&a->info);
    ns_wc_buf_clear(&a->context);
    ns_crypto_key_unref(a->public_key);
    memset(a, 0, sizeof *a);
}

/* Wipes and frees all JWK string members, zeroing key material first. */
static void
ns_wc_jwk_clear(ns_wc_jwk *j)
{
    char **fields[] = { &j->kty, &j->use, &j->alg, &j->crv, &j->x, &j->y,
                        &j->d, &j->n, &j->e, &j->p, &j->q, &j->dp, &j->dq,
                        &j->qi, &j->k };
    for (gsize i = 0; i < G_N_ELEMENTS(fields); i++) {
        if (*fields[i]) memset(*fields[i], 0, strlen(*fields[i]));
        g_free(*fields[i]);
    }
    memset(j, 0, sizeof *j);
}

static gboolean
ns_wc_is_aes(const char *name)
{
    return g_str_has_prefix(name, "AES-");
}

static gboolean
ns_wc_is_rsa(const char *name)
{
    return !strcmp(name, "RSASSA-PKCS1-v1_5") || !strcmp(name, "RSA-PSS") ||
           !strcmp(name, "RSA-OAEP");
}

static gboolean
ns_wc_is_ec(const char *name)
{
    return !strcmp(name, "ECDSA") || !strcmp(name, "ECDH");
}

static gboolean
ns_wc_is_kdf(const char *name)
{
    return !strcmp(name, "HKDF") || !strcmp(name, "PBKDF2");
}

static gboolean
ns_wc_is_eddsa(const char *name)
{
    return !strcmp(name, "Ed25519") || !strcmp(name, "Ed448");
}

static gboolean
ns_wc_is_xdh(const char *name)
{
    return !strcmp(name, "X25519") || !strcmp(name, "X448");
}

/* Raw key size in bytes for an OKP curve: 57 for Ed448, 56 for X448, else 32. */
static gsize
ns_wc_okp_key_bytes(const char *name)
{
    if (!strcmp(name, "Ed448")) return 57;
    if (!strcmp(name, "X448")) return 56;
    return 32;
}

/* Usages permitted for a secret key of the given algorithm. */
static guint32
ns_wc_secret_usages(const char *name)
{
    if (!strcmp(name, "AES-KW")) return NS_USAGE_WRAP | NS_USAGE_UNWRAP;
    if (ns_wc_is_aes(name)) return NS_USAGES_CIPHER;
    if (!strcmp(name, "HMAC")) return NS_USAGES_SIGN;
    return NS_USAGES_DERIVE;
}

/* Usages permitted for a public key of the given algorithm. */
static guint32
ns_wc_public_usages(const char *name)
{
    if (!strcmp(name, "RSA-OAEP")) return NS_USAGE_ENCRYPT | NS_USAGE_WRAP;
    if (!strcmp(name, "ECDH") || ns_wc_is_xdh(name)) return 0;
    return NS_USAGE_VERIFY;
}

/* Usages permitted for a private key of the given algorithm. */
static guint32
ns_wc_private_usages(const char *name)
{
    if (!strcmp(name, "RSA-OAEP")) return NS_USAGE_DECRYPT | NS_USAGE_UNWRAP;
    if (!strcmp(name, "ECDH") || ns_wc_is_xdh(name)) return NS_USAGES_DERIVE;
    return NS_USAGE_SIGN;
}

/* Block size in bits of a SHA hash: 1024 for SHA-384/512, else 512 (HMAC default length). */
static int
ns_wc_hash_block_bits(const char *hash)
{
    return (!g_strcmp0(hash, "SHA-384") || !g_strcmp0(hash, "SHA-512")) ? 1024
                                                                         : 512;
}

/* JWK alg suffix for a SHA hash name (e.g. "256"), or NULL if unknown. */
static const char *
ns_wc_hash_suffix(const char *hash)
{
    if (!g_strcmp0(hash, "SHA-1")) return "1";
    if (!g_strcmp0(hash, "SHA-256")) return "256";
    if (!g_strcmp0(hash, "SHA-384")) return "384";
    if (!g_strcmp0(hash, "SHA-512")) return "512";
    return NULL;
}

/* True for the supported NIST curves P-256, P-384 and P-521. */
static gboolean
ns_wc_curve_supported(const char *curve)
{
    return curve && (!strcmp(curve, "P-256") || !strcmp(curve, "P-384") ||
                     !strcmp(curve, "P-521"));
}

/* Field element size in bytes for a NIST curve; 0 if unknown. */
static gsize
ns_wc_curve_bytes(const char *curve)
{
    if (!g_strcmp0(curve, "P-256")) return 32;
    if (!g_strcmp0(curve, "P-384")) return 48;
    if (!g_strcmp0(curve, "P-521")) return 66;
    return 0;
}

/* Reads a dictionary member; returns 1 present, 0 absent, -1 on error or missing required. */
static int
ns_wc_member(JSContext *ctx, JSValueConst obj, const char *key,
             gboolean required, JSValue *out)
{
    *out = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(*out)) return -1;
    if (!JS_IsUndefined(*out)) return 1;
    if (required) {
        JS_ThrowTypeError(ctx, "Algorithm: member '%s' is required", key);
        return -1;
    }
    return 0;
}

/* WebIDL [EnforceRange] conversion to an unsigned integer bounded by max. */
static int
ns_wc_to_enforced(JSContext *ctx, JSValueConst v, double max, guint32 *out)
{
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
    if (!isfinite(d)) {
        JS_ThrowTypeError(ctx, "value is not a finite number");
        return -1;
    }
    d = trunc(d);
    if (d < 0 || d > max) {
        JS_ThrowTypeError(ctx, "value is out of range");
        return -1;
    }
    *out = (guint32)d;
    return 0;
}

/* Reads an [EnforceRange] unsigned dictionary member; reports presence via present. */
static int
ns_wc_member_uint(JSContext *ctx, JSValueConst obj, const char *key,
                  gboolean required, double max, guint32 *out,
                  gboolean *present)
{
    JSValue v;
    int r = ns_wc_member(ctx, obj, key, required, &v);
    if (present) *present = r > 0;
    if (r <= 0) return r;
    r = ns_wc_to_enforced(ctx, v, max, out);
    JS_FreeValue(ctx, v);
    return r < 0 ? -1 : 1;
}

/* Reads a BufferSource dictionary member into out, copying the bytes. */
static int
ns_wc_member_buffer(JSContext *ctx, JSValueConst obj, const char *key,
                    gboolean required, ns_wc_buf *out)
{
    JSValue v;
    int r = ns_wc_member(ctx, obj, key, required, &v);
    if (r <= 0) return r;
    if (!ns_wc_is_buffer_source(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "Algorithm: member '%s' is not a BufferSource",
                          key);
        return -1;
    }
    ns_wc_copy_buffer(ctx, v, out);
    JS_FreeValue(ctx, v);
    return 1;
}

/* Reads a dictionary member converted to a string; caller frees out. */
static int
ns_wc_member_string(JSContext *ctx, JSValueConst obj, const char *key,
                    gboolean required, char **out)
{
    JSValue v;
    int r = ns_wc_member(ctx, obj, key, required, &v);
    if (r <= 0) return r;
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return -1;
    *out = g_strdup(s);
    JS_FreeCString(ctx, s);
    return 1;
}

/* Reads an AlgorithmIdentifier member, stringifying non-objects. */
static int
ns_wc_member_identifier(JSContext *ctx, JSValueConst obj, const char *key,
                        JSValue *out)
{
    int r = ns_wc_member(ctx, obj, key, TRUE, out);
    if (r <= 0 || JS_IsObject(*out)) return r;
    JSValue s = JS_ToString(ctx, *out);
    JS_FreeValue(ctx, *out);
    *out = s;
    return JS_IsException(s) ? -1 : 1;
}

/* Reads a BigInteger (Uint8Array) dictionary member into out. */
static int
ns_wc_member_big_integer(JSContext *ctx, JSValueConst obj, const char *key,
                         ns_wc_buf *out)
{
    JSValue v;
    int r = ns_wc_member(ctx, obj, key, TRUE, &v);
    if (r <= 0) return r;
    if (JS_GetTypedArrayType(v) != JS_TYPED_ARRAY_UINT8) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "Algorithm: member '%s' is not a Uint8Array", key);
        return -1;
    }
    ns_wc_copy_buffer(ctx, v, out);
    JS_FreeValue(ctx, v);
    return 1;
}

/* Returns the native key behind a CryptoKey object, or NULL. */
static ns_crypto_key *
ns_wc_key_of(JSValueConst v)
{
    ns_wc_key_ref *ref = JS_GetOpaque(v, ns_cryptokey_class_id);
    return ref ? ref->key : NULL;
}

/* Reads a CryptoKey dictionary member; out receives a new reference. */
static int
ns_wc_member_key(JSContext *ctx, JSValueConst obj, const char *key,
                 ns_crypto_key **out)
{
    JSValue v;
    int r = ns_wc_member(ctx, obj, key, TRUE, &v);
    if (r <= 0) return r;
    *out = ns_wc_key_of(v);
    JS_FreeValue(ctx, v);
    if (!*out) {
        JS_ThrowTypeError(ctx, "Algorithm: member '%s' is not a CryptoKey", key);
        return -1;
    }
    (*out)->refcount++;
    return 1;
}

static int ns_wc_normalize(JSContext *ctx, JSValueConst alg, ns_wc_op op,
                           ns_wc_alg *out);

/* Converts the algorithm-specific dictionary members for a params type, normalizing hash. */
static int
ns_wc_convert_params(JSContext *ctx, JSValueConst obj, int params, ns_wc_alg *a)
{
    JSValue hash = JS_UNDEFINED;
    int r = 0;
    switch (params) {
    case NS_P_AES_LENGTH:
        r = ns_wc_member_uint(ctx, obj, "length", TRUE, 65535, &a->length,
                              &a->has_length);
        break;
    case NS_P_AES_CBC:
        r = ns_wc_member_buffer(ctx, obj, "iv", TRUE, &a->iv);
        break;
    case NS_P_AES_CTR:
        r = ns_wc_member_buffer(ctx, obj, "counter", TRUE, &a->counter);
        if (r >= 0)
            r = ns_wc_member_uint(ctx, obj, "length", TRUE, 255, &a->length,
                                  &a->has_length);
        break;
    case NS_P_AES_GCM:
        r = ns_wc_member_buffer(ctx, obj, "additionalData", FALSE,
                                &a->additional_data);
        if (r >= 0) r = ns_wc_member_buffer(ctx, obj, "iv", TRUE, &a->iv);
        if (r >= 0)
            r = ns_wc_member_uint(ctx, obj, "tagLength", FALSE, 255,
                                  &a->tag_length, &a->has_tag_length);
        break;
    case NS_P_HMAC:
        r = ns_wc_member_identifier(ctx, obj, "hash", &hash);
        if (r >= 0)
            r = ns_wc_member_uint(ctx, obj, "length", FALSE, 4294967295.0,
                                  &a->length, &a->has_length);
        break;
    case NS_P_RSA_KEY_GEN:
        r = ns_wc_member_uint(ctx, obj, "modulusLength", TRUE, 4294967295.0,
                              &a->modulus_length, NULL);
        if (r >= 0)
            r = ns_wc_member_big_integer(ctx, obj, "publicExponent",
                                         &a->public_exponent);
        if (r >= 0) r = ns_wc_member_identifier(ctx, obj, "hash", &hash);
        break;
    case NS_P_HASH:
        r = ns_wc_member_identifier(ctx, obj, "hash", &hash);
        break;
    case NS_P_RSA_PSS:
        r = ns_wc_member_uint(ctx, obj, "saltLength", TRUE, 4294967295.0,
                              &a->salt_length, NULL);
        break;
    case NS_P_RSA_OAEP:
        r = ns_wc_member_buffer(ctx, obj, "label", FALSE, &a->label);
        break;
    case NS_P_EC_CURVE:
        r = ns_wc_member_string(ctx, obj, "namedCurve", TRUE, &a->named_curve);
        break;
    case NS_P_ECDH:
        r = ns_wc_member_key(ctx, obj, "public", &a->public_key);
        break;
    case NS_P_HKDF:
        r = ns_wc_member_identifier(ctx, obj, "hash", &hash);
        if (r >= 0) r = ns_wc_member_buffer(ctx, obj, "info", TRUE, &a->info);
        if (r >= 0) r = ns_wc_member_buffer(ctx, obj, "salt", TRUE, &a->salt);
        break;
    case NS_P_PBKDF2:
        r = ns_wc_member_identifier(ctx, obj, "hash", &hash);
        if (r >= 0)
            r = ns_wc_member_uint(ctx, obj, "iterations", TRUE, 4294967295.0,
                                  &a->iterations, NULL);
        if (r >= 0) r = ns_wc_member_buffer(ctx, obj, "salt", TRUE, &a->salt);
        break;
    case NS_P_ED448:
        r = ns_wc_member_buffer(ctx, obj, "context", FALSE, &a->context);
        break;
    default:
        break;
    }
    if (r >= 0 && !JS_IsUndefined(hash)) {
        ns_wc_alg h;
        r = ns_wc_normalize(ctx, hash, NS_OP_DIGEST, &h);
        if (r >= 0 && g_str_has_prefix(h.name, "SHA3-")) {
            ns_wc_throw(ctx, "NotSupportedError",
                        "the hash is not supported for this algorithm");
            r = -1;
        }
        if (r >= 0) a->hash = h.name;
        ns_wc_alg_clear(&h);
    }
    JS_FreeValue(ctx, hash);
    return r < 0 ? -1 : 0;
}

/* WebCrypto "normalize an algorithm": registry lookup by op plus parameter conversion. */
static int
ns_wc_normalize(JSContext *ctx, JSValueConst alg, ns_wc_op op, ns_wc_alg *out)
{
    memset(out, 0, sizeof *out);
    JSValue obj;
    if (JS_IsObject(alg)) {
        obj = JS_DupValue(ctx, alg);
    } else {
        JSValue s = JS_ToString(ctx, alg);
        if (JS_IsException(s)) return -1;
        obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", s);
    }
    JSValue name_v = JS_GetPropertyStr(ctx, obj, "name");
    if (JS_IsException(name_v)) {
        JS_FreeValue(ctx, obj);
        return -1;
    }
    if (JS_IsUndefined(name_v)) {
        JS_FreeValue(ctx, obj);
        JS_ThrowTypeError(ctx, "Algorithm: member 'name' is required");
        return -1;
    }
    const char *name = JS_ToCString(ctx, name_v);
    JS_FreeValue(ctx, name_v);
    if (!name) {
        JS_FreeValue(ctx, obj);
        return -1;
    }
    int params = -1;
    for (gsize i = 0; i < G_N_ELEMENTS(ns_wc_registry); i++) {
        if (ns_wc_registry[i].op == op &&
            !g_ascii_strcasecmp(ns_wc_registry[i].name, name)) {
            out->name = ns_wc_registry[i].name;
            params = ns_wc_registry[i].params;
            break;
        }
    }
    JS_FreeCString(ctx, name);
    if (params < 0) {
        JS_FreeValue(ctx, obj);
        ns_wc_throw(ctx, "NotSupportedError",
                    "the algorithm is not supported for this operation");
        return -1;
    }
    int r = ns_wc_convert_params(ctx, obj, params, out);
    JS_FreeValue(ctx, obj);
    if (r < 0) ns_wc_alg_clear(out);
    return r;
}

/* True if the registry lists the algorithm for the operation. */
static gboolean
ns_wc_supports(const char *name, ns_wc_op op)
{
    for (gsize i = 0; i < G_N_ELEMENTS(ns_wc_registry); i++)
        if (ns_wc_registry[i].op == op && !strcmp(ns_wc_registry[i].name, name))
            return TRUE;
    return FALSE;
}

/* Converts a sequence of KeyUsage strings to a bitmask; TypeError on unknown entries. */
static int
ns_wc_usages_from(JSContext *ctx, JSValueConst v, guint32 *out)
{
    *out = 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "keyUsages is not a sequence");
        return -1;
    }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t n = 0;
    int r = JS_IsException(lv) ? -1 : JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    if (r < 0) return -1;
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        if (JS_IsException(e)) return -1;
        const char *s = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!s) return -1;
        guint32 bit = 0;
        for (gsize k = 0; k < G_N_ELEMENTS(ns_wc_usages); k++)
            if (!strcmp(s, ns_wc_usages[k].name)) bit = ns_wc_usages[k].bit;
        JS_FreeCString(ctx, s);
        if (!bit) {
            JS_ThrowTypeError(ctx, "keyUsages contains an invalid KeyUsage");
            return -1;
        }
        *out |= bit;
    }
    return 0;
}

/* Builds a JS array of usage names from a bitmask, in canonical order. */
static JSValue
ns_wc_usages_array(JSContext *ctx, guint32 usages)
{
    JSValue a = JS_NewArray(ctx);
    uint32_t i = 0;
    for (gsize k = 0; k < G_N_ELEMENTS(ns_wc_usages); k++)
        if (usages & ns_wc_usages[k].bit)
            JS_SetPropertyUint32(ctx, a, i++,
                                 JS_NewString(ctx, ns_wc_usages[k].name));
    return a;
}

/* Parses a KeyFormat string (raw, spki, pkcs8, jwk). */
static int
ns_wc_format_from(JSContext *ctx, JSValueConst v, ns_wc_format *out)
{
    static const char *const names[] = { "raw", "spki", "pkcs8", "jwk" };
    const char *s = JS_ToCString(ctx, v);
    if (!s) return -1;
    int found = -1;
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++)
        if (!strcmp(s, names[i])) found = (int)i;
    JS_FreeCString(ctx, s);
    if (found < 0) {
        JS_ThrowTypeError(ctx, "format is not a valid KeyFormat");
        return -1;
    }
    *out = (ns_wc_format)found;
    return 0;
}

/* Unwraps a CryptoKey argument, throwing TypeError naming what if it is not one. */
static ns_crypto_key *
ns_wc_key_arg(JSContext *ctx, JSValueConst v, const char *what)
{
    ns_crypto_key *k = ns_wc_key_of(v);
    if (!k) JS_ThrowTypeError(ctx, "%s is not a CryptoKey", what);
    return k;
}

/* Wraps a native key in a new CryptoKey object, taking ownership of k. */
static JSValue
ns_wc_new_key(JSContext *ctx, ns_crypto_key *k)
{
    JSValue o = JS_NewObjectClass(ctx, (int)ns_cryptokey_class_id);
    if (JS_IsException(o)) {
        ns_crypto_key_unref(k);
        return o;
    }
    ns_wc_key_ref *ref = g_new0(ns_wc_key_ref, 1);
    ref->key = k;
    ref->algorithm = JS_UNDEFINED;
    ref->usages = JS_UNDEFINED;
    JS_SetOpaque(o, ref);
    return o;
}

/* Structured-clone helper: new CryptoKey sharing the same native key. */
JSValue
ns_webcrypto_clone_key(JSContext *ctx, JSValueConst v)
{
    ns_crypto_key *k = ns_wc_key_of(v);
    if (!k) return JS_UNDEFINED;
    k->refcount++;
    return ns_wc_new_key(ctx, k);
}

static JSValue
ns_wc_hash_object(JSContext *ctx, const char *hash)
{
    JSValue h = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, h, "name", JS_NewString(ctx, hash ? hash : ""));
    return h;
}

/* Builds the KeyAlgorithm dictionary exposed as CryptoKey.algorithm. */
static JSValue
ns_wc_key_algorithm(JSContext *ctx, const ns_crypto_key *k)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, k->algo));
    if (ns_wc_is_aes(k->algo)) {
        JS_SetPropertyStr(ctx, o, "length", JS_NewInt32(ctx, k->bits));
    } else if (!strcmp(k->algo, "HMAC")) {
        JS_SetPropertyStr(ctx, o, "length", JS_NewInt64(ctx, k->bits));
        JS_SetPropertyStr(ctx, o, "hash", ns_wc_hash_object(ctx, k->hash));
    } else if (ns_wc_is_rsa(k->algo)) {
        JS_SetPropertyStr(ctx, o, "modulusLength", JS_NewInt32(ctx, k->bits));
        gsize n = 0;
        guint8 *e = ns_crypto_rsa_exponent(k, &n);
        JS_SetPropertyStr(ctx, o, "publicExponent",
                          JS_NewUint8ArrayCopy(ctx, e ? e : (guint8 *)"", n));
        g_free(e);
        JS_SetPropertyStr(ctx, o, "hash", ns_wc_hash_object(ctx, k->hash));
    } else if (ns_wc_is_ec(k->algo)) {
        JS_SetPropertyStr(ctx, o, "namedCurve",
                          JS_NewString(ctx, k->curve ? k->curve : ""));
    }
    return o;
}

static ns_wc_key_ref *
ns_wc_this_key(JSContext *ctx, JSValueConst this_val)
{
    return JS_GetOpaque2(ctx, this_val, ns_cryptokey_class_id);
}

static JSValue
ns_cryptokey_get_type(JSContext *ctx, JSValueConst this_val)
{
    ns_wc_key_ref *ref = ns_wc_this_key(ctx, this_val);
    if (!ref) return JS_EXCEPTION;
    return JS_NewString(ctx, ref->key->type == NS_CK_PRIVATE ? "private"
                           : ref->key->type == NS_CK_PUBLIC ? "public"
                                                            : "secret");
}

static JSValue
ns_cryptokey_get_extractable(JSContext *ctx, JSValueConst this_val)
{
    ns_wc_key_ref *ref = ns_wc_this_key(ctx, this_val);
    if (!ref) return JS_EXCEPTION;
    return JS_NewBool(ctx, ref->key->extractable);
}

/* CryptoKey.algorithm getter; caches the object so identity is stable. */
static JSValue
ns_cryptokey_get_algorithm(JSContext *ctx, JSValueConst this_val)
{
    ns_wc_key_ref *ref = ns_wc_this_key(ctx, this_val);
    if (!ref) return JS_EXCEPTION;
    if (JS_IsUndefined(ref->algorithm))
        ref->algorithm = ns_wc_key_algorithm(ctx, ref->key);
    return JS_DupValue(ctx, ref->algorithm);
}

/* CryptoKey.usages getter; caches the array so identity is stable. */
static JSValue
ns_cryptokey_get_usages(JSContext *ctx, JSValueConst this_val)
{
    ns_wc_key_ref *ref = ns_wc_this_key(ctx, this_val);
    if (!ref) return JS_EXCEPTION;
    if (JS_IsUndefined(ref->usages))
        ref->usages = ns_wc_usages_array(ctx, ref->key->usages);
    return JS_DupValue(ctx, ref->usages);
}

/* Decodes unpadded base64url into out; -1 on invalid characters or length. */
static int
ns_wc_b64url_decode(const char *s, ns_wc_buf *out)
{
    gsize n = strlen(s);
    if (n % 4 == 1) return -1;
    GString *t = g_string_sized_new(n + 4);
    for (gsize i = 0; i < n; i++) {
        char c = s[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        else if (!g_ascii_isalnum(c)) {
            g_string_free(t, TRUE);
            return -1;
        }
        g_string_append_c(t, c);
    }
    while (t->len % 4) g_string_append_c(t, '=');
    gsize len = 0;
    guint8 *d = g_base64_decode(t->str, &len);
    g_string_free(t, TRUE);
    ns_wc_buf_set(out, d, len);
    if (d) {
        memset(d, 0, len);
        g_free(d);
    }
    return 0;
}

/* Encodes bytes as unpadded base64url; caller frees. */
static char *
ns_wc_b64url_encode(const guint8 *b, gsize len)
{
    char *s = g_base64_encode(b, len);
    gsize n = strlen(s);
    for (gsize i = 0; i < n; i++) {
        if (s[i] == '+') s[i] = '-';
        else if (s[i] == '/') s[i] = '_';
    }
    while (n > 0 && s[n - 1] == '=') s[--n] = '\0';
    return s;
}

/* Copies an optional JWK string member into out; caller frees. */
static int
ns_wc_jwk_string(JSContext *ctx, JSValueConst obj, const char *key, char **out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) return 0;
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return -1;
    *out = g_strdup(s);
    JS_FreeCString(ctx, s);
    return 0;
}

/* Reads JWK key_ops into a usage mask, noting repeated entries. */
static int
ns_wc_jwk_key_ops(JSContext *ctx, JSValueConst obj, ns_wc_jwk *jwk)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, "key_ops");
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) return 0;
    if (!JS_IsObject(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "JsonWebKey: key_ops is not a sequence");
        return -1;
    }
    jwk->has_key_ops = TRUE;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t n = 0;
    int r = JS_IsException(lv) ? -1 : JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);
    for (uint32_t i = 0; r >= 0 && i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        const char *s = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!s) {
            r = -1;
            break;
        }
        if (g_hash_table_contains(seen, s)) jwk->key_ops_repeated = TRUE;
        g_hash_table_add(seen, g_strdup(s));
        for (gsize k = 0; k < G_N_ELEMENTS(ns_wc_usages); k++)
            if (!strcmp(s, ns_wc_usages[k].name))
                jwk->key_ops |= ns_wc_usages[k].bit;
        JS_FreeCString(ctx, s);
    }
    g_hash_table_destroy(seen);
    JS_FreeValue(ctx, v);
    return r < 0 ? -1 : 0;
}

/* Converts a JsonWebKey dictionary, reading members in WebIDL lexicographic order. */
static int
ns_wc_jwk_from(JSContext *ctx, JSValueConst obj, ns_wc_jwk *jwk)
{
    memset(jwk, 0, sizeof *jwk);
    if (!JS_IsObject(obj)) return 0;
    struct { const char *key; char **field; } strings[] = {
        { "alg", &jwk->alg }, { "crv", &jwk->crv }, { "d", &jwk->d },
        { "dp", &jwk->dp }, { "dq", &jwk->dq }, { "e", &jwk->e },
    };
    struct { const char *key; char **field; } strings2[] = {
        { "k", &jwk->k },
    };
    struct { const char *key; char **field; } strings3[] = {
        { "kty", &jwk->kty }, { "n", &jwk->n },
    };
    struct { const char *key; char **field; } strings4[] = {
        { "p", &jwk->p }, { "q", &jwk->q }, { "qi", &jwk->qi },
        { "use", &jwk->use }, { "x", &jwk->x }, { "y", &jwk->y },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(strings); i++)
        if (ns_wc_jwk_string(ctx, obj, strings[i].key, strings[i].field) < 0)
            return -1;
    JSValue ext = JS_GetPropertyStr(ctx, obj, "ext");
    if (JS_IsException(ext)) return -1;
    if (!JS_IsUndefined(ext)) {
        jwk->has_ext = TRUE;
        jwk->ext = JS_ToBool(ctx, ext) > 0;
    }
    JS_FreeValue(ctx, ext);
    for (gsize i = 0; i < G_N_ELEMENTS(strings2); i++)
        if (ns_wc_jwk_string(ctx, obj, strings2[i].key, strings2[i].field) < 0)
            return -1;
    if (ns_wc_jwk_key_ops(ctx, obj, jwk) < 0) return -1;
    for (gsize i = 0; i < G_N_ELEMENTS(strings3); i++)
        if (ns_wc_jwk_string(ctx, obj, strings3[i].key, strings3[i].field) < 0)
            return -1;
    JSValue oth = JS_GetPropertyStr(ctx, obj, "oth");
    if (JS_IsException(oth)) return -1;
    jwk->has_oth = !JS_IsUndefined(oth);
    JS_FreeValue(ctx, oth);
    for (gsize i = 0; i < G_N_ELEMENTS(strings4); i++)
        if (ns_wc_jwk_string(ctx, obj, strings4[i].key, strings4[i].field) < 0)
            return -1;
    return 0;
}

/* Validates JWK kty, use, key_ops and ext against the import request; throws DataError. */
static gboolean
ns_wc_jwk_check(JSContext *ctx, const ns_wc_jwk *jwk, const char *kty,
                const char *use, gboolean extractable, guint32 usages)
{
    if (!jwk->kty || strcmp(jwk->kty, kty) != 0) {
        ns_wc_throw(ctx, "DataError", "JWK: kty does not match the algorithm");
        return FALSE;
    }
    if (usages && jwk->use && strcmp(jwk->use, use) != 0) {
        ns_wc_throw(ctx, "DataError", "JWK: use does not permit the usages");
        return FALSE;
    }
    if (jwk->has_key_ops &&
        (jwk->key_ops_repeated || (usages & ~jwk->key_ops))) {
        ns_wc_throw(ctx, "DataError", "JWK: key_ops does not permit the usages");
        return FALSE;
    }
    if (jwk->has_ext && !jwk->ext && extractable) {
        ns_wc_throw(ctx, "DataError", "JWK: the key is not extractable");
        return FALSE;
    }
    return TRUE;
}

/* Decodes a required base64url JWK member; throws DataError if missing or bad. */
static gboolean
ns_wc_jwk_bytes(JSContext *ctx, const char *field, ns_wc_buf *out)
{
    if (field && ns_wc_b64url_decode(field, out) == 0) return TRUE;
    ns_wc_throw(ctx, "DataError", "JWK: a required member is missing or invalid");
    return FALSE;
}

/* Throws SyntaxError unless all requested usages are within allowed. */
static gboolean
ns_wc_check_usages(JSContext *ctx, guint32 usages, guint32 allowed)
{
    if (!(usages & ~allowed)) return TRUE;
    ns_wc_throw(ctx, "SyntaxError", "the usages are not valid for this key");
    return FALSE;
}

/* Imports an AES key from raw or JWK, checking size and the JWK alg (e.g. A256GCM). */
static ns_crypto_key *
ns_wc_import_aes(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                 const ns_wc_jwk *jwk, const ns_wc_alg *a, gboolean extractable,
                 guint32 usages)
{
    if (!ns_wc_check_usages(ctx, usages, ns_wc_secret_usages(a->name)))
        return NULL;
    ns_wc_buf bytes = { 0 };
    if (fmt == NS_FMT_RAW) {
        ns_wc_buf_set(&bytes, data->data, data->len);
    } else if (fmt == NS_FMT_JWK) {
        if (!jwk->kty || strcmp(jwk->kty, "oct") != 0) {
            ns_wc_throw(ctx, "DataError", "JWK: kty must be oct");
            return NULL;
        }
        if (!ns_wc_jwk_bytes(ctx, jwk->k, &bytes)) return NULL;
        gsize bits = bytes.len * 8;
        if (bits == 128 || bits == 192 || bits == 256) {
            char *alg = g_strdup_printf("A%u%s", (unsigned)bits, a->name + 4);
            gboolean bad = jwk->alg && strcmp(jwk->alg, alg) != 0;
            g_free(alg);
            if (bad) {
                ns_wc_buf_clear(&bytes);
                ns_wc_throw(ctx, "DataError", "JWK: alg does not match the key");
                return NULL;
            }
        }
        if (!ns_wc_jwk_check(ctx, jwk, "oct", "enc", extractable, usages)) {
            ns_wc_buf_clear(&bytes);
            return NULL;
        }
    } else {
        ns_wc_throw(ctx, "NotSupportedError", "AES keys import raw or jwk only");
        return NULL;
    }
    gsize bits = bytes.len * 8;
    if (bits != 128 && bits != 192 && bits != 256) {
        ns_wc_buf_clear(&bytes);
        ns_wc_throw(ctx, "DataError", "AES keys are 128, 192 or 256 bits");
        return NULL;
    }
    ns_crypto_key *k = ns_crypto_secret_new(a->name, NULL, bytes.data, bytes.len,
                                            (int)bits, extractable, usages);
    ns_wc_buf_clear(&bytes);
    return k;
}

/* Imports an HMAC key from raw or JWK, honoring an explicit bit length. */
static ns_crypto_key *
ns_wc_import_hmac(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                  const ns_wc_jwk *jwk, const ns_wc_alg *a, gboolean extractable,
                  guint32 usages)
{
    if (!ns_wc_check_usages(ctx, usages, NS_USAGES_SIGN)) return NULL;
    ns_wc_buf bytes = { 0 };
    if (fmt == NS_FMT_RAW) {
        ns_wc_buf_set(&bytes, data->data, data->len);
    } else if (fmt == NS_FMT_JWK) {
        if (!jwk->kty || strcmp(jwk->kty, "oct") != 0) {
            ns_wc_throw(ctx, "DataError", "JWK: kty must be oct");
            return NULL;
        }
        if (!ns_wc_jwk_bytes(ctx, jwk->k, &bytes)) return NULL;
        char *alg = g_strdup_printf("HS%s", ns_wc_hash_suffix(a->hash));
        gboolean bad = jwk->alg && strcmp(jwk->alg, alg) != 0;
        g_free(alg);
        if (bad) {
            ns_wc_buf_clear(&bytes);
            ns_wc_throw(ctx, "DataError", "JWK: alg does not match the hash");
            return NULL;
        }
        if (!ns_wc_jwk_check(ctx, jwk, "oct", "sig", extractable, usages)) {
            ns_wc_buf_clear(&bytes);
            return NULL;
        }
    } else {
        ns_wc_throw(ctx, "NotSupportedError", "HMAC keys import raw or jwk only");
        return NULL;
    }
    guint64 length = (guint64)bytes.len * 8;
    if (length == 0) {
        ns_wc_buf_clear(&bytes);
        ns_wc_throw(ctx, "DataError", "HMAC keys cannot be empty");
        return NULL;
    }
    if (a->has_length) {
        if (a->length > length || a->length + 8 <= length) {
            ns_wc_buf_clear(&bytes);
            ns_wc_throw(ctx, "DataError", "HMAC length does not match the key");
            return NULL;
        }
        length = a->length;
    }
    ns_crypto_key *k = ns_crypto_secret_new("HMAC", a->hash, bytes.data,
                                            (gsize)((length + 7) / 8), (int)length,
                                            extractable, usages);
    ns_wc_buf_clear(&bytes);
    return k;
}

/* Imports a raw, non-extractable HKDF/PBKDF2 base key. */
static ns_crypto_key *
ns_wc_import_kdf(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                 const ns_wc_alg *a, gboolean extractable, guint32 usages)
{
    if (fmt != NS_FMT_RAW) {
        ns_wc_throw(ctx, "NotSupportedError", "KDF keys import raw only");
        return NULL;
    }
    if (!ns_wc_check_usages(ctx, usages, NS_USAGES_DERIVE)) return NULL;
    if (extractable) {
        ns_wc_throw(ctx, "SyntaxError", "KDF keys cannot be extractable");
        return NULL;
    }
    return ns_crypto_secret_new(a->name, NULL, data->data, data->len,
                                (int)(data->len * 8), FALSE, usages);
}

/* Imports an SPKI or PKCS#8 key and checks it is of the expected kind. */
static ns_crypto_key *
ns_wc_import_der(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                 const ns_wc_alg *a, const char *kind, gboolean extractable,
                 guint32 usages)
{
    char *err = NULL;
    ns_crypto_key *k = ns_crypto_import_raw(fmt == NS_FMT_SPKI ? "spki" : "pkcs8",
                                            data->data, data->len, a->name,
                                            a->hash, a->named_curve, extractable,
                                            usages, &err);
    g_free(err);
    if (!k || g_strcmp0(ns_crypto_key_kind(k), kind) != 0) {
        ns_crypto_key_unref(k);
        ns_wc_throw(ctx, "DataError", "the key data is not a valid key");
        return NULL;
    }
    return k;
}

/* JWK alg name (RS256, PS384, RSA-OAEP-256, ...) for an RSA algorithm and hash, or NULL. */
static const char *
ns_wc_rsa_jwk_alg(const char *name, const char *hash)
{
    static const struct { const char *name, *hash, *alg; } algs[] = {
        { "RSASSA-PKCS1-v1_5", "SHA-1", "RS1" },
        { "RSASSA-PKCS1-v1_5", "SHA-256", "RS256" },
        { "RSASSA-PKCS1-v1_5", "SHA-384", "RS384" },
        { "RSASSA-PKCS1-v1_5", "SHA-512", "RS512" },
        { "RSA-PSS", "SHA-1", "PS1" },
        { "RSA-PSS", "SHA-256", "PS256" },
        { "RSA-PSS", "SHA-384", "PS384" },
        { "RSA-PSS", "SHA-512", "PS512" },
        { "RSA-OAEP", "SHA-1", "RSA-OAEP" },
        { "RSA-OAEP", "SHA-256", "RSA-OAEP-256" },
        { "RSA-OAEP", "SHA-384", "RSA-OAEP-384" },
        { "RSA-OAEP", "SHA-512", "RSA-OAEP-512" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(algs); i++)
        if (!strcmp(algs[i].name, name) && !g_strcmp0(algs[i].hash, hash))
            return algs[i].alg;
    return NULL;
}

/* True if alg is a valid JWK alg for this RSA algorithm under any SHA hash. */
static gboolean
ns_wc_rsa_jwk_alg_known(const char *name, const char *alg)
{
    static const char *hashes[] = { "SHA-1", "SHA-256", "SHA-384", "SHA-512" };
    for (gsize i = 0; i < G_N_ELEMENTS(hashes); i++)
        if (!g_strcmp0(ns_wc_rsa_jwk_alg(name, hashes[i]), alg)) return TRUE;
    return FALSE;
}

/* Imports an RSA public or private JWK, validating alg/hash and CRT completeness. */
static ns_crypto_key *
ns_wc_import_rsa_jwk(JSContext *ctx, const ns_wc_jwk *jwk, const ns_wc_alg *a,
                     gboolean extractable, guint32 usages)
{
    const char *use = !strcmp(a->name, "RSA-OAEP") ? "enc" : "sig";
    if (!ns_wc_jwk_check(ctx, jwk, "RSA", use, extractable, usages)) return NULL;
    if (jwk->alg) {
        if (!ns_wc_rsa_jwk_alg_known(a->name, jwk->alg)) {
            ns_wc_throw(ctx, "DataError", "JWK: alg is not valid for the algorithm");
            return NULL;
        }
        if (g_strcmp0(ns_wc_rsa_jwk_alg(a->name, a->hash), jwk->alg) != 0) {
            ns_wc_throw(ctx, "DataError", "JWK: alg does not match the hash");
            return NULL;
        }
    }
    gboolean private = jwk->d != NULL;
    gboolean crt = jwk->p || jwk->q || jwk->dp || jwk->dq || jwk->qi;
    if (private && crt &&
        !(jwk->p && jwk->q && jwk->dp && jwk->dq && jwk->qi)) {
        ns_wc_throw(ctx, "DataError", "JWK: incomplete RSA private key");
        return NULL;
    }
    if (jwk->has_oth) {
        ns_wc_throw(ctx, "NotSupportedError", "JWK: multi-prime RSA keys");
        return NULL;
    }
    ns_wc_buf f[8] = { { 0 } };
    const char *src[8] = { jwk->n, jwk->e, jwk->d, jwk->p, jwk->q, jwk->dp,
                           jwk->dq, jwk->qi };
    gboolean ok = TRUE;
    for (int i = 0; i < 8 && ok; i++) {
        if (!src[i]) continue;
        if (ns_wc_b64url_decode(src[i], &f[i]) < 0 || f[i].len == 0) ok = FALSE;
    }
    if (!f[0].data || !f[1].data) ok = FALSE;
    ns_crypto_key *k = NULL;
    if (ok) {
        char *err = NULL;
        k = ns_crypto_import_rsa_jwk(f[0].data, f[0].len, f[1].data, f[1].len,
                                     f[2].data, f[2].len, f[3].data, f[3].len,
                                     f[4].data, f[4].len, f[5].data, f[5].len,
                                     f[6].data, f[6].len, f[7].data, f[7].len,
                                     a->name, a->hash, extractable, usages, &err);
        g_free(err);
    }
    for (int i = 0; i < 8; i++) ns_wc_buf_clear(&f[i]);
    if (!k) ns_wc_throw(ctx, "DataError", "JWK: invalid RSA key");
    return k;
}

/* Imports an RSA key from SPKI, PKCS#8 or JWK after checking usages. */
static ns_crypto_key *
ns_wc_import_rsa(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                 const ns_wc_jwk *jwk, const ns_wc_alg *a, gboolean extractable,
                 guint32 usages)
{
    guint32 pub = ns_wc_public_usages(a->name);
    guint32 priv = ns_wc_private_usages(a->name);
    if (fmt == NS_FMT_SPKI || fmt == NS_FMT_PKCS8) {
        if (!ns_wc_check_usages(ctx, usages, fmt == NS_FMT_SPKI ? pub : priv))
            return NULL;
        return ns_wc_import_der(ctx, fmt, data, a, "RSA", extractable, usages);
    }
    if (fmt == NS_FMT_JWK) {
        if (!ns_wc_check_usages(ctx, usages, jwk->d ? priv : pub)) return NULL;
        return ns_wc_import_rsa_jwk(ctx, jwk, a, extractable, usages);
    }
    ns_wc_throw(ctx, "NotSupportedError", "RSA keys do not import raw");
    return NULL;
}

/* Imports an EC JWK, checking crv, ES* alg and coordinate sizes. */
static ns_crypto_key *
ns_wc_import_ec_jwk(JSContext *ctx, const ns_wc_jwk *jwk, const ns_wc_alg *a,
                    gboolean extractable, guint32 usages)
{
    gboolean ecdsa = !strcmp(a->name, "ECDSA");
    if (!ns_wc_jwk_check(ctx, jwk, "EC", ecdsa ? "sig" : "enc", extractable,
                         usages))
        return NULL;
    if (!jwk->crv || strcmp(jwk->crv, a->named_curve) != 0) {
        ns_wc_throw(ctx, "DataError", "JWK: crv does not match namedCurve");
        return NULL;
    }
    if (ecdsa && jwk->alg) {
        const char *want = !strcmp(a->named_curve, "P-256") ? "ES256"
                         : !strcmp(a->named_curve, "P-384") ? "ES384" : "ES512";
        if (strcmp(jwk->alg, want) != 0) {
            ns_wc_throw(ctx, "DataError", "JWK: alg does not match the curve");
            return NULL;
        }
    }
    gsize size = ns_wc_curve_bytes(a->named_curve);
    ns_wc_buf x = { 0 }, y = { 0 }, d = { 0 };
    gboolean ok = jwk->x && jwk->y &&
                  ns_wc_b64url_decode(jwk->x, &x) == 0 && x.len == size &&
                  ns_wc_b64url_decode(jwk->y, &y) == 0 && y.len == size &&
                  (!jwk->d || (ns_wc_b64url_decode(jwk->d, &d) == 0 &&
                               d.len == size));
    ns_crypto_key *k = NULL;
    if (ok) {
        char *err = NULL;
        k = ns_crypto_import_ec_jwk(a->named_curve, x.data, x.len, y.data, y.len,
                                    d.data, d.len, a->name, extractable, usages,
                                    &err);
        g_free(err);
    }
    ns_wc_buf_clear(&x);
    ns_wc_buf_clear(&y);
    ns_wc_buf_clear(&d);
    if (!k) ns_wc_throw(ctx, "DataError", "JWK: invalid EC key");
    return k;
}

/* Imports an ECDSA/ECDH key from raw point, SPKI, PKCS#8 or JWK, checking the curve. */
static ns_crypto_key *
ns_wc_import_ec(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                const ns_wc_jwk *jwk, const ns_wc_alg *a, gboolean extractable,
                guint32 usages)
{
    guint32 pub = ns_wc_public_usages(a->name);
    guint32 priv = ns_wc_private_usages(a->name);
    guint32 allowed = fmt == NS_FMT_PKCS8 ? priv
                    : fmt == NS_FMT_JWK ? (jwk->d ? priv : pub) : pub;
    if (!ns_wc_check_usages(ctx, usages, allowed)) return NULL;
    if (!ns_wc_curve_supported(a->named_curve)) {
        ns_wc_throw(ctx, "NotSupportedError", "the named curve is not supported");
        return NULL;
    }
    if (fmt == NS_FMT_JWK)
        return ns_wc_import_ec_jwk(ctx, jwk, a, extractable, usages);
    ns_crypto_key *k = NULL;
    if (fmt == NS_FMT_RAW) {
        char *err = NULL;
        k = ns_crypto_import_raw("raw", data->data, data->len, a->name, NULL,
                                 a->named_curve, extractable, usages, &err);
        g_free(err);
        gsize size = ns_wc_curve_bytes(a->named_curve);
        gboolean uncompressed = data->len == 1 + 2 * size && data->data[0] == 0x04;
        gboolean compressed = data->len == 1 + size &&
                              (data->data[0] == 0x02 || data->data[0] == 0x03);
        if (k && !uncompressed && !compressed) {
            ns_crypto_key_unref(k);
            k = NULL;
        }
        if (!k) ns_wc_throw(ctx, "DataError", "invalid EC public key");
        return k;
    }
    k = ns_wc_import_der(ctx, fmt, data, a, "EC", extractable, usages);
    if (k && g_strcmp0(ns_crypto_key_curve(k), a->named_curve) != 0) {
        ns_crypto_key_unref(k);
        ns_wc_throw(ctx, "DataError", "the key's curve does not match namedCurve");
        return NULL;
    }
    return k;
}

/* Imports an Ed/X25519/448 JWK, verifying a private key's x matches its derived public key. */
static ns_crypto_key *
ns_wc_import_okp_jwk(JSContext *ctx, const ns_wc_jwk *jwk, const ns_wc_alg *a,
                     gboolean extractable, guint32 usages)
{
    gboolean ed = ns_wc_is_eddsa(a->name);
    if (!jwk->kty || strcmp(jwk->kty, "OKP") != 0) {
        ns_wc_throw(ctx, "DataError", "JWK: kty must be OKP");
        return NULL;
    }
    if (!jwk->crv || strcmp(jwk->crv, a->name) != 0) {
        ns_wc_throw(ctx, "DataError", "JWK: crv does not match the algorithm");
        return NULL;
    }
    if (ed && jwk->alg && strcmp(jwk->alg, a->name) != 0 &&
        strcmp(jwk->alg, "EdDSA") != 0) {
        ns_wc_throw(ctx, "DataError", "JWK: alg does not match the algorithm");
        return NULL;
    }
    if (!ns_wc_jwk_check(ctx, jwk, "OKP", ed ? "sig" : "enc", extractable,
                         usages))
        return NULL;
    gsize size = ns_wc_okp_key_bytes(a->name);
    ns_wc_buf x = { 0 }, d = { 0 };
    gboolean ok = jwk->x && ns_wc_b64url_decode(jwk->x, &x) == 0 &&
                  x.len == size &&
                  (!jwk->d || (ns_wc_b64url_decode(jwk->d, &d) == 0 &&
                               d.len == size));
    ns_crypto_key *k = NULL;
    if (ok) {
        char *err = NULL;
        k = ns_crypto_import_okp_jwk(a->name, x.data, x.len, d.data, d.len,
                                     a->name, extractable, usages, &err);
        g_free(err);
    }
    if (k && jwk->d) {
        guint8 *pub = NULL, *priv = NULL;
        gsize pub_len = 0, priv_len = 0;
        char *err = NULL;
        gboolean match = ns_crypto_export_okp_jwk(k, &pub, &pub_len, &priv,
                                                  &priv_len, &err) &&
                         pub_len == x.len && memcmp(pub, x.data, x.len) == 0;
        g_free(err);
        g_free(pub);
        if (priv) memset(priv, 0, priv_len);
        g_free(priv);
        if (!match) {
            ns_crypto_key_unref(k);
            k = NULL;
        }
    }
    ns_wc_buf_clear(&x);
    ns_wc_buf_clear(&d);
    if (!k) ns_wc_throw(ctx, "DataError", "JWK: invalid OKP key");
    return k;
}

/* Imports an OKP (EdDSA/XDH) key from raw, SPKI, PKCS#8 or JWK. */
static ns_crypto_key *
ns_wc_import_okp(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
                 const ns_wc_jwk *jwk, const ns_wc_alg *a, gboolean extractable,
                 guint32 usages)
{
    guint32 pub = ns_wc_public_usages(a->name);
    guint32 priv = ns_wc_private_usages(a->name);
    guint32 allowed = fmt == NS_FMT_PKCS8 ? priv
                    : fmt == NS_FMT_JWK ? (jwk->d ? priv : pub) : pub;
    if (!ns_wc_check_usages(ctx, usages, allowed)) return NULL;
    if (fmt == NS_FMT_JWK)
        return ns_wc_import_okp_jwk(ctx, jwk, a, extractable, usages);
    if (fmt == NS_FMT_RAW) {
        char *err = NULL;
        ns_crypto_key *k = data->len == ns_wc_okp_key_bytes(a->name)
            ? ns_crypto_import_raw("raw", data->data, data->len, a->name, NULL,
                                   NULL, extractable, usages, &err)
            : NULL;
        g_free(err);
        if (!k) ns_wc_throw(ctx, "DataError", "invalid public key");
        return k;
    }
    return ns_wc_import_der(ctx, fmt, data, a, a->name, extractable, usages);
}

/* Dispatches key import to the per-algorithm-family importer. */
static ns_crypto_key *
ns_wc_import(JSContext *ctx, ns_wc_format fmt, const ns_wc_buf *data,
             const ns_wc_jwk *jwk, const ns_wc_alg *a, gboolean extractable,
             guint32 usages)
{
    if (ns_wc_is_aes(a->name))
        return ns_wc_import_aes(ctx, fmt, data, jwk, a, extractable, usages);
    if (!strcmp(a->name, "HMAC"))
        return ns_wc_import_hmac(ctx, fmt, data, jwk, a, extractable, usages);
    if (ns_wc_is_kdf(a->name))
        return ns_wc_import_kdf(ctx, fmt, data, a, extractable, usages);
    if (ns_wc_is_rsa(a->name))
        return ns_wc_import_rsa(ctx, fmt, data, jwk, a, extractable, usages);
    if (ns_wc_is_ec(a->name))
        return ns_wc_import_ec(ctx, fmt, data, jwk, a, extractable, usages);
    return ns_wc_import_okp(ctx, fmt, data, jwk, a, extractable, usages);
}

/* Wraps an imported/generated key, rejecting secret/private keys with no usages. */
static JSValue
ns_wc_finish_key(JSContext *ctx, ns_crypto_key *k)
{
    if (!k) return JS_EXCEPTION;
    if (k->type != NS_CK_PUBLIC && !k->usages) {
        ns_crypto_key_unref(k);
        return ns_wc_throw(ctx, "SyntaxError",
                           "secret and private keys need at least one usage");
    }
    return ns_wc_new_key(ctx, k);
}

/* Imports key data (BufferSource or JWK object) into a new CryptoKey value. */
static JSValue
ns_wc_import_value(JSContext *ctx, ns_wc_format fmt, JSValueConst key_data,
                   const ns_wc_alg *a, gboolean extractable, guint32 usages)
{
    ns_wc_buf data = { 0 };
    ns_wc_jwk jwk = { 0 };
    if (fmt == NS_FMT_JWK) {
        if (ns_wc_jwk_from(ctx, key_data, &jwk) < 0) {
            ns_wc_jwk_clear(&jwk);
            return JS_EXCEPTION;
        }
    } else {
        ns_wc_copy_buffer(ctx, key_data, &data);
    }
    ns_crypto_key *k = ns_wc_import(ctx, fmt, &data, &jwk, a, extractable,
                                    usages);
    ns_wc_buf_clear(&data);
    ns_wc_jwk_clear(&jwk);
    return ns_wc_finish_key(ctx, k);
}

/* SubtleCrypto.importKey body: validates arguments and imports the key. */
static JSValue
ns_wc_import_key(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "importKey: 5 arguments required");
    ns_wc_format fmt;
    if (ns_wc_format_from(ctx, argv[0], &fmt) < 0) return JS_EXCEPTION;
    gboolean is_buffer = ns_wc_is_buffer_source(ctx, argv[1]);
    if (fmt == NS_FMT_JWK
            ? (is_buffer || (!JS_IsObject(argv[1]) && !JS_IsUndefined(argv[1]) &&
                             !JS_IsNull(argv[1])))
            : !is_buffer)
        return JS_ThrowTypeError(ctx, "importKey: keyData does not match format");
    gboolean extractable = JS_ToBool(ctx, argv[3]) > 0;
    guint32 usages;
    if (ns_wc_usages_from(ctx, argv[4], &usages) < 0) return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize(ctx, argv[2], NS_OP_IMPORT_KEY, &a) < 0)
        return JS_EXCEPTION;
    JSValue r = ns_wc_import_value(ctx, fmt, argv[1], &a, extractable, usages);
    ns_wc_alg_clear(&a);
    return r;
}

/* Exports a key as raw/SPKI/PKCS#8 bytes after format and type checks; caller frees. */
static guint8 *
ns_wc_export_bytes(JSContext *ctx, ns_wc_format fmt, const ns_crypto_key *k,
                   gsize *out_len)
{
    const char *why = NULL;
    const char *name = "NotSupportedError";
    if (fmt == NS_FMT_RAW) {
        if (k->type == NS_CK_SECRET) {
            if (ns_wc_is_kdf(k->algo)) why = "the key cannot be exported";
        } else if (ns_wc_is_rsa(k->algo)) {
            why = "RSA keys do not export raw";
        } else if (k->type != NS_CK_PUBLIC) {
            name = "InvalidAccessError";
            why = "only public keys export raw";
        }
    } else if (k->type == NS_CK_SECRET) {
        why = "secret keys export raw or jwk only";
    } else if (k->type != (fmt == NS_FMT_SPKI ? NS_CK_PUBLIC : NS_CK_PRIVATE)) {
        name = "InvalidAccessError";
        why = "the key type does not match the format";
    }
    if (why) {
        ns_wc_throw(ctx, name, why);
        return NULL;
    }
    char *err = NULL;
    guint8 *out = ns_crypto_export_raw(fmt == NS_FMT_RAW ? "raw"
                                       : fmt == NS_FMT_SPKI ? "spki" : "pkcs8",
                                       k, out_len, &err);
    if (!out) ns_wc_throw_err(ctx, err, "OperationError");
    else g_free(err);
    return out;
}

/* Sets a base64url-encoded property on o when data is non-NULL. */
static void
ns_wc_set_b64(JSContext *ctx, JSValueConst o, const char *key,
              const guint8 *data, gsize len)
{
    if (!data) return;
    char *s = ns_wc_b64url_encode(data, len);
    JS_SetPropertyStr(ctx, o, key, JS_NewString(ctx, s));
    g_free(s);
}

/* Exports a key as a JsonWebKey object including key_ops and ext. */
static JSValue
ns_wc_export_jwk(JSContext *ctx, const ns_crypto_key *k)
{
    JSValue o = JS_NewObject(ctx);
    char *err = NULL;
    gboolean ok = TRUE;
    if (k->type == NS_CK_SECRET) {
        JS_SetPropertyStr(ctx, o, "kty", JS_NewString(ctx, "oct"));
        ns_wc_set_b64(ctx, o, "k", k->raw, k->raw_len);
        char *alg = ns_wc_is_aes(k->algo)
            ? g_strdup_printf("A%d%s", k->bits, k->algo + 4)
            : g_strdup_printf("HS%s", ns_wc_hash_suffix(k->hash));
        JS_SetPropertyStr(ctx, o, "alg", JS_NewString(ctx, alg));
        g_free(alg);
    } else if (ns_wc_is_rsa(k->algo)) {
        guint8 *f[8] = { NULL };
        gsize l[8] = { 0 };
        ok = ns_crypto_export_rsa_jwk(k, &f[0], &l[0], &f[1], &l[1], &f[2], &l[2],
                                      &f[3], &l[3], &f[4], &l[4], &f[5], &l[5],
                                      &f[6], &l[6], &f[7], &l[7], &err);
        if (ok) {
            static const char *keys[8] = { "n", "e", "d", "p", "q", "dp", "dq",
                                           "qi" };
            JS_SetPropertyStr(ctx, o, "kty", JS_NewString(ctx, "RSA"));
            const char *alg = ns_wc_rsa_jwk_alg(k->algo, k->hash);
            if (alg) JS_SetPropertyStr(ctx, o, "alg", JS_NewString(ctx, alg));
            for (int i = 0; i < 8; i++) ns_wc_set_b64(ctx, o, keys[i], f[i], l[i]);
        }
        for (int i = 0; i < 8; i++) {
            if (f[i]) memset(f[i], 0, l[i]);
            g_free(f[i]);
        }
    } else if (ns_wc_is_ec(k->algo)) {
        guint8 *x = NULL, *y = NULL, *d = NULL;
        gsize xl = 0, yl = 0, dl = 0;
        ok = ns_crypto_export_ec_jwk(k, &x, &xl, &y, &yl, &d, &dl, &err);
        if (ok) {
            JS_SetPropertyStr(ctx, o, "kty", JS_NewString(ctx, "EC"));
            JS_SetPropertyStr(ctx, o, "crv", JS_NewString(ctx, k->curve));
            ns_wc_set_b64(ctx, o, "x", x, xl);
            ns_wc_set_b64(ctx, o, "y", y, yl);
            ns_wc_set_b64(ctx, o, "d", d, dl);
        }
        if (d) memset(d, 0, dl);
        g_free(x); g_free(y); g_free(d);
    } else {
        guint8 *x = NULL, *d = NULL;
        gsize xl = 0, dl = 0;
        ok = ns_crypto_export_okp_jwk(k, &x, &xl, &d, &dl, &err);
        if (ok) {
            JS_SetPropertyStr(ctx, o, "kty", JS_NewString(ctx, "OKP"));
            JS_SetPropertyStr(ctx, o, "crv", JS_NewString(ctx, k->algo));
            if (ns_wc_is_eddsa(k->algo))
                JS_SetPropertyStr(ctx, o, "alg", JS_NewString(ctx, k->algo));
            ns_wc_set_b64(ctx, o, "x", x, xl);
            ns_wc_set_b64(ctx, o, "d", d, dl);
        }
        if (d) memset(d, 0, dl);
        g_free(x); g_free(d);
    }
    if (!ok) {
        JS_FreeValue(ctx, o);
        return ns_wc_throw_err(ctx, err, "OperationError");
    }
    g_free(err);
    JS_SetPropertyStr(ctx, o, "key_ops", ns_wc_usages_array(ctx, k->usages));
    JS_SetPropertyStr(ctx, o, "ext", JS_NewBool(ctx, k->extractable));
    return o;
}

/* Exports an extractable key in the requested format as ArrayBuffer or JWK. */
static JSValue
ns_wc_export(JSContext *ctx, ns_wc_format fmt, const ns_crypto_key *k)
{
    if (!ns_wc_supports(k->algo, NS_OP_EXPORT_KEY))
        return ns_wc_throw(ctx, "NotSupportedError", "the key cannot be exported");
    if (!k->extractable)
        return ns_wc_throw(ctx, "InvalidAccessError", "the key is not extractable");
    if (fmt == NS_FMT_JWK) return ns_wc_export_jwk(ctx, k);
    gsize len = 0;
    guint8 *out = ns_wc_export_bytes(ctx, fmt, k, &len);
    if (!out) return JS_EXCEPTION;
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, len);
    memset(out, 0, len);
    g_free(out);
    return ab;
}

/* SubtleCrypto.exportKey body. */
static JSValue
ns_wc_export_key(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "exportKey: 2 arguments required");
    ns_wc_format fmt;
    if (ns_wc_format_from(ctx, argv[0], &fmt) < 0) return JS_EXCEPTION;
    ns_crypto_key *k = ns_wc_key_arg(ctx, argv[1], "exportKey: key");
    if (!k) return JS_EXCEPTION;
    return ns_wc_export(ctx, fmt, k);
}

/* Big-endian RSA public exponent as a uint32; 0 if it overflows. */
static guint32
ns_wc_exponent_value(const ns_wc_buf *e)
{
    guint64 v = 0;
    for (gsize i = 0; i < e->len; i++) {
        v = (v << 8) | e->data[i];
        if (v > G_MAXUINT32) return 0;
    }
    return (guint32)v;
}

/* Generates a secret key or key pair, splitting usages between public and private halves. */
static JSValue
ns_wc_generate(JSContext *ctx, const ns_wc_alg *a, gboolean extractable,
               guint32 usages)
{
    char *err = NULL;
    if (ns_wc_is_aes(a->name) || !strcmp(a->name, "HMAC")) {
        if (!ns_wc_check_usages(ctx, usages, ns_wc_secret_usages(a->name)))
            return JS_EXCEPTION;
        guint32 bits = a->length;
        if (ns_wc_is_aes(a->name)) {
            if (bits != 128 && bits != 192 && bits != 256)
                return ns_wc_throw(ctx, "OperationError",
                                   "AES keys are 128, 192 or 256 bits");
        } else if (!a->has_length) {
            bits = (guint32)ns_wc_hash_block_bits(a->hash);
        } else if (bits == 0) {
            return ns_wc_throw(ctx, "OperationError", "HMAC length is zero");
        }
        ns_crypto_key *k = ns_crypto_generate_secret(a->name, a->hash, (int)bits,
                                                     extractable, usages, &err);
        if (!k) return ns_wc_throw_err(ctx, err, "OperationError");
        return ns_wc_finish_key(ctx, k);
    }
    guint32 pub_usages = ns_wc_public_usages(a->name);
    guint32 priv_usages = ns_wc_private_usages(a->name);
    if (!ns_wc_check_usages(ctx, usages, pub_usages | priv_usages))
        return JS_EXCEPTION;
    guint32 exponent = 0;
    if (ns_wc_is_rsa(a->name)) {
        exponent = ns_wc_exponent_value(&a->public_exponent);
        if (exponent != 3 && exponent != 65537)
            return ns_wc_throw(ctx, "OperationError",
                               "the public exponent must be 3 or 65537");
    } else if (ns_wc_is_ec(a->name) && !ns_wc_curve_supported(a->named_curve)) {
        return ns_wc_throw(ctx, "NotSupportedError",
                           "the named curve is not supported");
    }
    ns_crypto_key *pub = NULL, *priv = NULL;
    if (!ns_crypto_generate_keypair(a->name, a->hash, a->named_curve,
                                    (int)MIN(a->modulus_length, (guint32)G_MAXINT),
                                    exponent, extractable, usages, &pub, &priv,
                                    &err))
        return ns_wc_throw_err(ctx, err, "OperationError");
    pub->usages = usages & pub_usages;
    pub->extractable = TRUE;
    priv->usages = usages & priv_usages;
    priv->extractable = extractable;
    if (!priv->usages) {
        ns_crypto_key_unref(pub);
        ns_crypto_key_unref(priv);
        return ns_wc_throw(ctx, "SyntaxError",
                           "the private key needs at least one usage");
    }
    JSValue pair = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pair, "publicKey", ns_wc_new_key(ctx, pub));
    JS_SetPropertyStr(ctx, pair, "privateKey", ns_wc_new_key(ctx, priv));
    return pair;
}

/* SubtleCrypto.generateKey body. */
static JSValue
ns_wc_generate_key(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "generateKey: 3 arguments required");
    gboolean extractable = JS_ToBool(ctx, argv[1]) > 0;
    guint32 usages;
    if (ns_wc_usages_from(ctx, argv[2], &usages) < 0) return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize(ctx, argv[0], NS_OP_GENERATE_KEY, &a) < 0)
        return JS_EXCEPTION;
    JSValue r = ns_wc_generate(ctx, &a, extractable, usages);
    ns_wc_alg_clear(&a);
    return r;
}

/* Throws InvalidAccessError unless the key matches the algorithm and permits usage. */
static gboolean
ns_wc_check_key(JSContext *ctx, const ns_wc_alg *a, const ns_crypto_key *k,
                guint32 usage)
{
    if (strcmp(a->name, k->algo) != 0) {
        ns_wc_throw(ctx, "InvalidAccessError",
                    "the key does not belong to the algorithm");
        return FALSE;
    }
    if (!(k->usages & usage)) {
        ns_wc_throw(ctx, "InvalidAccessError",
                    "the key does not permit this operation");
        return FALSE;
    }
    return TRUE;
}

/* Throws InvalidAccessError unless the key is of the given type. */
static gboolean
ns_wc_check_type(JSContext *ctx, const ns_crypto_key *k, ns_ck_type type)
{
    if (k->type == type) return TRUE;
    ns_wc_throw(ctx, "InvalidAccessError", "the key type does not permit this");
    return FALSE;
}

/* Throws TypeError unless v is a BufferSource. */
static int
ns_wc_data_arg(JSContext *ctx, JSValueConst v, const char *what)
{
    if (ns_wc_is_buffer_source(ctx, v)) return 0;
    JS_ThrowTypeError(ctx, "%s is not a BufferSource", what);
    return -1;
}

/* Runs RSA-OAEP or AES-CBC/CTR/GCM encrypt or decrypt after parameter checks. */
static JSValue
ns_wc_cipher(JSContext *ctx, const ns_wc_alg *a, const ns_crypto_key *k,
             const ns_wc_buf *data, gboolean encrypt)
{
    ns_crypto_params p = { 0 };
    if (!strcmp(a->name, "RSA-OAEP")) {
        if (!ns_wc_check_type(ctx, k, encrypt ? NS_CK_PUBLIC : NS_CK_PRIVATE))
            return JS_EXCEPTION;
        p.label = a->label.data;
        p.label_len = a->label.len;
    } else if (!strcmp(a->name, "AES-CBC")) {
        if (a->iv.len != 16)
            return ns_wc_throw(ctx, "OperationError", "the iv must be 16 bytes");
        p.iv = a->iv.data;
        p.iv_len = a->iv.len;
    } else if (!strcmp(a->name, "AES-CTR")) {
        if (a->counter.len != 16)
            return ns_wc_throw(ctx, "OperationError", "the counter must be 16 bytes");
        if (a->length == 0 || a->length > 128)
            return ns_wc_throw(ctx, "OperationError",
                               "the counter length must be 1 to 128 bits");
        p.iv = a->counter.data;
        p.iv_len = a->counter.len;
        p.counter_bits = (int)a->length;
    } else if (!strcmp(a->name, "AES-GCM")) {
        guint32 tag = a->has_tag_length ? a->tag_length : 128;
        if (tag != 32 && tag != 64 && tag != 96 && tag != 104 && tag != 112 &&
            tag != 120 && tag != 128)
            return ns_wc_throw(ctx, "OperationError", "invalid AES-GCM tag length");
        if (!encrypt && data->len < tag / 8)
            return ns_wc_throw(ctx, "OperationError", "the data is too short");
        p.iv = a->iv.data;
        p.iv_len = a->iv.len;
        p.aad = a->additional_data.data;
        p.aad_len = a->additional_data.len;
        p.tag_bits = (int)tag;
    }
    gsize out_len = 0;
    char *err = NULL;
    guint8 *out = encrypt
        ? ns_crypto_encrypt(k, &p, data->data, data->len, &out_len, &err)
        : ns_crypto_decrypt(k, &p, data->data, data->len, &out_len, &err);
    if (!out) {
        g_free(err);
        return ns_wc_throw(ctx, "OperationError", encrypt ? "encryption failed"
                                                          : "decryption failed");
    }
    g_free(err);
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, out_len);
    memset(out, 0, out_len);
    g_free(out);
    return ab;
}

/* SubtleCrypto.encrypt/decrypt body. */
static JSValue
ns_wc_encrypt_decrypt(JSContext *ctx, int argc, JSValueConst *argv,
                      gboolean encrypt)
{
    if (argc < 3) return JS_ThrowTypeError(ctx, "3 arguments required");
    ns_crypto_key *k = ns_wc_key_arg(ctx, argv[1], "key");
    if (!k || ns_wc_data_arg(ctx, argv[2], "data") < 0) return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize(ctx, argv[0], encrypt ? NS_OP_ENCRYPT : NS_OP_DECRYPT,
                        &a) < 0)
        return JS_EXCEPTION;
    ns_wc_buf data = { 0 };
    ns_wc_copy_buffer(ctx, argv[2], &data);
    JSValue r = JS_EXCEPTION;
    if (ns_wc_check_key(ctx, &a, k, encrypt ? NS_USAGE_ENCRYPT : NS_USAGE_DECRYPT))
        r = ns_wc_cipher(ctx, &a, k, &data, encrypt);
    ns_wc_buf_clear(&data);
    ns_wc_alg_clear(&a);
    return r;
}

/* Raw (r, s) ECDSA signature length for the key's curve. */
static gsize
ns_wc_ecdsa_signature_len(const ns_crypto_key *k)
{
    return 2 * ns_wc_curve_bytes(k->curve);
}

/* Validates an Ed448 context: at most 255 bytes and supported by OpenSSL. */
static gboolean
ns_wc_check_context(JSContext *ctx, const ns_wc_alg *a)
{
    if (a->context.len > 255) {
        ns_wc_throw(ctx, "OperationError", "the context is longer than 255 bytes");
        return FALSE;
    }
    if (a->context.len && !ns_crypto_eddsa_context_supported()) {
        ns_wc_throw(ctx, "NotSupportedError",
                    "this OpenSSL cannot sign or verify with an Ed448 context");
        return FALSE;
    }
    return TRUE;
}

/* SubtleCrypto.sign/verify body; wrong-length ECDSA/EdDSA signatures verify false. */
static JSValue
ns_wc_sign_verify(JSContext *ctx, int argc, JSValueConst *argv, gboolean sign)
{
    int need = sign ? 3 : 4;
    if (argc < need)
        return JS_ThrowTypeError(ctx, "%d arguments required", need);
    ns_crypto_key *k = ns_wc_key_arg(ctx, argv[1], "key");
    if (!k) return JS_EXCEPTION;
    if (!sign && ns_wc_data_arg(ctx, argv[2], "signature") < 0)
        return JS_EXCEPTION;
    if (ns_wc_data_arg(ctx, argv[sign ? 2 : 3], "data") < 0) return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize(ctx, argv[0], sign ? NS_OP_SIGN : NS_OP_VERIFY, &a) < 0)
        return JS_EXCEPTION;
    ns_wc_buf signature = { 0 }, data = { 0 };
    if (!sign) ns_wc_copy_buffer(ctx, argv[2], &signature);
    ns_wc_copy_buffer(ctx, argv[sign ? 2 : 3], &data);
    JSValue r = JS_EXCEPTION;
    ns_ck_type type = strcmp(a.name, "HMAC") == 0 ? NS_CK_SECRET
                    : sign ? NS_CK_PRIVATE : NS_CK_PUBLIC;
    if (ns_wc_check_key(ctx, &a, k, sign ? NS_USAGE_SIGN : NS_USAGE_VERIFY) &&
        ns_wc_check_type(ctx, k, type) && ns_wc_check_context(ctx, &a)) {
        ns_crypto_params p = { 0 };
        p.sign_hash = a.hash;
        p.pss_salt_len = !strcmp(a.name, "RSA-PSS") ? (int)a.salt_length : -1;
        p.context = a.context.data;
        p.context_len = a.context.len;
        char *err = NULL;
        if (sign) {
            gsize out_len = 0;
            guint8 *out = ns_crypto_sign(k, &p, data.data, data.len, &out_len, &err);
            if (out) {
                r = JS_NewArrayBufferCopy(ctx, out, out_len);
                g_free(out);
                g_free(err);
            } else {
                g_free(err);
                r = ns_wc_throw(ctx, "OperationError", "signing failed");
            }
        } else if ((!strcmp(a.name, "ECDSA") &&
                    signature.len != ns_wc_ecdsa_signature_len(k)) ||
                   (ns_wc_is_eddsa(a.name) &&
                    signature.len != 2 * ns_wc_okp_key_bytes(a.name))) {
            r = JS_FALSE;
        } else {
            int v = ns_crypto_verify(k, &p, signature.data, signature.len,
                                     data.data, data.len, &err);
            g_free(err);
            r = v < 0 ? ns_wc_throw(ctx, "OperationError", "verification failed")
                      : JS_NewBool(ctx, v == 1);
        }
    }
    ns_wc_buf_clear(&signature);
    ns_wc_buf_clear(&data);
    ns_wc_alg_clear(&a);
    return r;
}

/* Truncates a buffer to bits, masking off trailing bits of the last byte. */
static void
ns_wc_truncate_bits(ns_wc_buf *b, guint32 bits)
{
    gsize bytes = (bits + 7) / 8;
    b->len = bytes;
    if (bits % 8) b->data[bytes - 1] &= (guint8)(0xFF << (8 - bits % 8));
}

/* Derives bits via ECDH/XDH key agreement or HKDF/PBKDF2 into out. */
static int
ns_wc_derive(JSContext *ctx, const ns_wc_alg *a, const ns_crypto_key *k,
             gboolean has_length, guint32 length, ns_wc_buf *out)
{
    ns_crypto_params p = { 0 };
    char *err = NULL;
    gsize n = 0;
    guint8 *bits = NULL;
    if (!strcmp(a->name, "ECDH") || ns_wc_is_xdh(a->name)) {
        const ns_crypto_key *peer = a->public_key;
        if (peer->type != NS_CK_PUBLIC || strcmp(peer->algo, k->algo) != 0 ||
            (!strcmp(a->name, "ECDH") && g_strcmp0(peer->curve, k->curve) != 0)) {
            ns_wc_throw(ctx, "InvalidAccessError",
                        "the public key does not match the base key");
            return -1;
        }
        if (k->type != NS_CK_PRIVATE) {
            ns_wc_throw(ctx, "InvalidAccessError", "the base key is not private");
            return -1;
        }
        p.peer = (ns_crypto_key *)peer;
        bits = ns_crypto_derive_bits(k, &p, 0, &n, &err);
        g_free(err);
        if (!bits) {
            ns_wc_throw(ctx, "OperationError", "key agreement failed");
            return -1;
        }
        ns_wc_buf_set(out, bits, n);
        memset(bits, 0, n);
        g_free(bits);
        if (has_length) {
            if ((guint64)length > (guint64)n * 8) {
                ns_wc_buf_clear(out);
                ns_wc_throw(ctx, "OperationError",
                            "the length exceeds the shared secret");
                return -1;
            }
            ns_wc_truncate_bits(out, length);
        }
        return 0;
    }
    if (!has_length || length % 8) {
        ns_wc_throw(ctx, "OperationError", "the length must be a multiple of 8");
        return -1;
    }
    if (!strcmp(a->name, "PBKDF2") && a->iterations == 0) {
        ns_wc_throw(ctx, "OperationError", "iterations must not be zero");
        return -1;
    }
    if (length == 0) {
        ns_wc_buf_set(out, NULL, 0);
        return 0;
    }
    p.salt = a->salt.data;
    p.salt_len = a->salt.len;
    p.info = a->info.data;
    p.info_len = a->info.len;
    p.iterations = (int)MIN(a->iterations, (guint32)G_MAXINT);
    p.kdf_hash = a->hash;
    bits = ns_crypto_derive_bits(k, &p, (int)MIN(length, (guint32)G_MAXINT), &n,
                                 &err);
    g_free(err);
    if (!bits) {
        ns_wc_throw(ctx, "OperationError", "key derivation failed");
        return -1;
    }
    ns_wc_buf_set(out, bits, n);
    memset(bits, 0, n);
    g_free(bits);
    return 0;
}

/* Reads an optional [EnforceRange] length argument (undefined/null means absent). */
static int
ns_wc_optional_length(JSContext *ctx, int argc, JSValueConst *argv, int index,
                      gboolean *has_length, guint32 *length)
{
    *has_length = FALSE;
    *length = 0;
    if (argc <= index || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index]))
        return 0;
    *has_length = TRUE;
    return ns_wc_to_enforced(ctx, argv[index], 4294967295.0, length);
}

/* SubtleCrypto.deriveBits body. */
static JSValue
ns_wc_derive_bits(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "deriveBits: 2 arguments required");
    ns_crypto_key *k = ns_wc_key_arg(ctx, argv[1], "deriveBits: baseKey");
    if (!k) return JS_EXCEPTION;
    gboolean has_length;
    guint32 length;
    if (ns_wc_optional_length(ctx, argc, argv, 2, &has_length, &length) < 0)
        return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize(ctx, argv[0], NS_OP_DERIVE_BITS, &a) < 0)
        return JS_EXCEPTION;
    JSValue r = JS_EXCEPTION;
    ns_wc_buf out = { 0 };
    if (ns_wc_check_key(ctx, &a, k, NS_USAGE_DERIVE_BITS) &&
        ns_wc_derive(ctx, &a, k, has_length, length, &out) == 0)
        r = JS_NewArrayBufferCopy(ctx, out.data, out.len);
    ns_wc_buf_clear(&out);
    ns_wc_alg_clear(&a);
    return r;
}

/* "Get key length" for the derived key algorithm (AES length or HMAC default). */
static int
ns_wc_key_length(JSContext *ctx, const ns_wc_alg *a, gboolean *has_length,
                 guint32 *length)
{
    *has_length = TRUE;
    if (ns_wc_is_aes(a->name)) {
        if (a->length != 128 && a->length != 192 && a->length != 256) {
            ns_wc_throw(ctx, "OperationError", "AES keys are 128, 192 or 256 bits");
            return -1;
        }
        *length = a->length;
    } else if (!strcmp(a->name, "HMAC")) {
        if (!a->has_length) {
            *length = (guint32)ns_wc_hash_block_bits(a->hash);
        } else if (a->length) {
            *length = a->length;
        } else {
            JS_ThrowTypeError(ctx, "HMAC length is zero");
            return -1;
        }
    } else {
        *has_length = FALSE;
        *length = 0;
    }
    return 0;
}

/* SubtleCrypto.deriveKey: derives bits of the key length and imports them raw. */
static JSValue
ns_wc_derive_key(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 5) return JS_ThrowTypeError(ctx, "deriveKey: 5 arguments required");
    ns_crypto_key *k = ns_wc_key_arg(ctx, argv[1], "deriveKey: baseKey");
    if (!k) return JS_EXCEPTION;
    gboolean extractable = JS_ToBool(ctx, argv[3]) > 0;
    guint32 usages;
    if (ns_wc_usages_from(ctx, argv[4], &usages) < 0) return JS_EXCEPTION;
    ns_wc_alg a, imp, len_alg;
    if (ns_wc_normalize(ctx, argv[0], NS_OP_DERIVE_BITS, &a) < 0)
        return JS_EXCEPTION;
    if (ns_wc_normalize(ctx, argv[2], NS_OP_IMPORT_KEY, &imp) < 0) {
        ns_wc_alg_clear(&a);
        return JS_EXCEPTION;
    }
    if (ns_wc_normalize(ctx, argv[2], NS_OP_GET_KEY_LENGTH, &len_alg) < 0) {
        ns_wc_alg_clear(&a);
        ns_wc_alg_clear(&imp);
        return JS_EXCEPTION;
    }
    JSValue r = JS_EXCEPTION;
    gboolean has_length;
    guint32 length;
    ns_wc_buf secret = { 0 };
    if (ns_wc_check_key(ctx, &a, k, NS_USAGE_DERIVE_KEY) &&
        ns_wc_key_length(ctx, &len_alg, &has_length, &length) == 0 &&
        ns_wc_derive(ctx, &a, k, has_length, length, &secret) == 0) {
        ns_crypto_key *nk = ns_wc_import(ctx, NS_FMT_RAW, &secret, NULL, &imp,
                                         extractable, usages);
        r = ns_wc_finish_key(ctx, nk);
    }
    ns_wc_buf_clear(&secret);
    ns_wc_alg_clear(&a);
    ns_wc_alg_clear(&imp);
    ns_wc_alg_clear(&len_alg);
    return r;
}

/* Normalizes for wrapKey/unwrapKey, falling back to encrypt/decrypt algorithms. */
static int
ns_wc_normalize_wrap(JSContext *ctx, JSValueConst alg, gboolean wrap,
                     ns_wc_alg *out)
{
    if (ns_wc_normalize(ctx, alg, wrap ? NS_OP_WRAP_KEY : NS_OP_UNWRAP_KEY,
                        out) == 0)
        return 0;
    JS_FreeValue(ctx, JS_GetException(ctx));
    return ns_wc_normalize(ctx, alg, wrap ? NS_OP_ENCRYPT : NS_OP_DECRYPT, out);
}

/* AES-KW wraps or unwraps data; wrapping requires a multiple of 8 bytes. */
static JSValue
ns_wc_aes_kw(JSContext *ctx, const ns_crypto_key *k, const ns_wc_buf *data,
             gboolean wrap)
{
    if (wrap && data->len % 8)
        return ns_wc_throw(ctx, "OperationError",
                           "AES-KW needs a multiple of 8 bytes");
    ns_crypto_params p = { 0 };
    gsize out_len = 0;
    char *err = NULL;
    guint8 *out = wrap
        ? ns_crypto_encrypt(k, &p, data->data, data->len, &out_len, &err)
        : ns_crypto_decrypt(k, &p, data->data, data->len, &out_len, &err);
    g_free(err);
    if (!out) return ns_wc_throw(ctx, "OperationError", "AES-KW failed");
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, out_len);
    memset(out, 0, out_len);
    g_free(out);
    return ab;
}

/* SubtleCrypto.wrapKey: exports the key (JWK as JSON text) then encrypts it. */
static JSValue
ns_wc_wrap_key(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_ThrowTypeError(ctx, "wrapKey: 4 arguments required");
    ns_wc_format fmt;
    if (ns_wc_format_from(ctx, argv[0], &fmt) < 0) return JS_EXCEPTION;
    ns_crypto_key *key = ns_wc_key_arg(ctx, argv[1], "wrapKey: key");
    if (!key) return JS_EXCEPTION;
    ns_crypto_key *wk = ns_wc_key_arg(ctx, argv[2], "wrapKey: wrappingKey");
    if (!wk) return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize_wrap(ctx, argv[3], TRUE, &a) < 0) return JS_EXCEPTION;
    JSValue r = JS_EXCEPTION;
    if (ns_wc_check_key(ctx, &a, wk, NS_USAGE_WRAP)) {
        JSValue exported = ns_wc_export(ctx, fmt, key);
        ns_wc_buf bytes = { 0 };
        if (!JS_IsException(exported)) {
            if (fmt == NS_FMT_JWK) {
                JSValue json = JS_JSONStringify(ctx, exported, JS_UNDEFINED,
                                                JS_UNDEFINED);
                size_t len = 0;
                const char *s = JS_IsException(json) ? NULL
                                                     : JS_ToCStringLen(ctx, &len, json);
                if (s) {
                    ns_wc_buf_set(&bytes, (const guint8 *)s, len);
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, json);
            } else {
                ns_wc_copy_buffer(ctx, exported, &bytes);
            }
            JS_FreeValue(ctx, exported);
            if (bytes.data)
                r = !strcmp(a.name, "AES-KW")
                    ? ns_wc_aes_kw(ctx, wk, &bytes, TRUE)
                    : ns_wc_cipher(ctx, &a, wk, &bytes, TRUE);
        }
        ns_wc_buf_clear(&bytes);
    }
    ns_wc_alg_clear(&a);
    return r;
}

/* SubtleCrypto.unwrapKey: decrypts, parses JWK JSON if needed, then imports. */
static JSValue
ns_wc_unwrap_key(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 7) return JS_ThrowTypeError(ctx, "unwrapKey: 7 arguments required");
    ns_wc_format fmt;
    if (ns_wc_format_from(ctx, argv[0], &fmt) < 0) return JS_EXCEPTION;
    if (ns_wc_data_arg(ctx, argv[1], "unwrapKey: wrappedKey") < 0)
        return JS_EXCEPTION;
    ns_crypto_key *uk = ns_wc_key_arg(ctx, argv[2], "unwrapKey: unwrappingKey");
    if (!uk) return JS_EXCEPTION;
    gboolean extractable = JS_ToBool(ctx, argv[5]) > 0;
    guint32 usages;
    if (ns_wc_usages_from(ctx, argv[6], &usages) < 0) return JS_EXCEPTION;
    ns_wc_alg a, imp;
    if (ns_wc_normalize_wrap(ctx, argv[3], FALSE, &a) < 0) return JS_EXCEPTION;
    if (ns_wc_normalize(ctx, argv[4], NS_OP_IMPORT_KEY, &imp) < 0) {
        ns_wc_alg_clear(&a);
        return JS_EXCEPTION;
    }
    ns_wc_buf wrapped = { 0 };
    ns_wc_copy_buffer(ctx, argv[1], &wrapped);
    JSValue r = JS_EXCEPTION;
    if (ns_wc_check_key(ctx, &a, uk, NS_USAGE_UNWRAP)) {
        JSValue plain = !strcmp(a.name, "AES-KW")
            ? ns_wc_aes_kw(ctx, uk, &wrapped, FALSE)
            : ns_wc_cipher(ctx, &a, uk, &wrapped, FALSE);
        if (!JS_IsException(plain)) {
            JSValue key_data = plain;
            if (fmt == NS_FMT_JWK) {
                ns_wc_buf text = { 0 };
                ns_wc_copy_buffer(ctx, plain, &text);
                key_data = JS_ParseJSON(ctx, (const char *)text.data, text.len,
                                        "<unwrapKey>");
                ns_wc_buf_clear(&text);
                JS_FreeValue(ctx, plain);
                if (JS_IsException(key_data)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    key_data = JS_UNDEFINED;
                    ns_wc_throw(ctx, "DataError", "the unwrapped key is not JSON");
                } else if (!JS_IsObject(key_data)) {
                    JS_FreeValue(ctx, key_data);
                    key_data = JS_UNDEFINED;
                    ns_wc_throw(ctx, "DataError", "the unwrapped key is not a JWK");
                }
            }
            if (!JS_IsUndefined(key_data))
                r = ns_wc_import_value(ctx, fmt, key_data, &imp, extractable,
                                       usages);
            JS_FreeValue(ctx, key_data);
        }
    }
    ns_wc_buf_clear(&wrapped);
    ns_wc_alg_clear(&a);
    ns_wc_alg_clear(&imp);
    return r;
}

/* SubtleCrypto.digest body. */
static JSValue
ns_wc_digest(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "digest: 2 arguments required");
    if (ns_wc_data_arg(ctx, argv[1], "digest: data") < 0) return JS_EXCEPTION;
    ns_wc_alg a;
    if (ns_wc_normalize(ctx, argv[0], NS_OP_DIGEST, &a) < 0) return JS_EXCEPTION;
    ns_wc_buf data = { 0 };
    ns_wc_copy_buffer(ctx, argv[1], &data);
    gsize out_len = 0;
    guint8 *out = ns_crypto_digest(a.name, data.data, data.len, &out_len);
    ns_wc_buf_clear(&data);
    ns_wc_alg_clear(&a);
    if (!out) return ns_wc_throw(ctx, "OperationError", "digest failed");
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, out_len);
    g_free(out);
    return ab;
}

/* Converts a synchronous result or pending exception into a settled promise. */
static JSValue
ns_wc_settle(JSContext *ctx, JSValue result)
{
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, result);
        return promise;
    }
    gboolean rejected = JS_IsException(result);
    JSValue value = rejected ? JS_GetException(ctx) : result;
    JSValue r = JS_Call(ctx, resolvers[rejected ? 1 : 0], JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

enum {
    NS_WC_ENCRYPT,
    NS_WC_DECRYPT,
    NS_WC_SIGN,
    NS_WC_VERIFY,
    NS_WC_DIGEST,
    NS_WC_GENERATE_KEY,
    NS_WC_DERIVE_KEY,
    NS_WC_DERIVE_BITS,
    NS_WC_IMPORT_KEY,
    NS_WC_EXPORT_KEY,
    NS_WC_WRAP_KEY,
    NS_WC_UNWRAP_KEY,
};

/* Shared SubtleCrypto method entry; dispatches on magic and returns a promise. */
static JSValue
ns_subtle_method(JSContext *ctx, JSValueConst this_val, int argc,
                 JSValueConst *argv, int magic)
{
    (void)this_val;
    JSValue result;
    switch (magic) {
    case NS_WC_ENCRYPT: result = ns_wc_encrypt_decrypt(ctx, argc, argv, TRUE); break;
    case NS_WC_DECRYPT: result = ns_wc_encrypt_decrypt(ctx, argc, argv, FALSE); break;
    case NS_WC_SIGN: result = ns_wc_sign_verify(ctx, argc, argv, TRUE); break;
    case NS_WC_VERIFY: result = ns_wc_sign_verify(ctx, argc, argv, FALSE); break;
    case NS_WC_DIGEST: result = ns_wc_digest(ctx, argc, argv); break;
    case NS_WC_GENERATE_KEY: result = ns_wc_generate_key(ctx, argc, argv); break;
    case NS_WC_DERIVE_KEY: result = ns_wc_derive_key(ctx, argc, argv); break;
    case NS_WC_DERIVE_BITS: result = ns_wc_derive_bits(ctx, argc, argv); break;
    case NS_WC_IMPORT_KEY: result = ns_wc_import_key(ctx, argc, argv); break;
    case NS_WC_EXPORT_KEY: result = ns_wc_export_key(ctx, argc, argv); break;
    case NS_WC_WRAP_KEY: result = ns_wc_wrap_key(ctx, argc, argv); break;
    default: result = ns_wc_unwrap_key(ctx, argc, argv); break;
    }
    return ns_wc_settle(ctx, result);
}

static JSValue
ns_wc_illegal_constructor(JSContext *ctx, JSValueConst new_target, int argc,
                          JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

#define NS_WC_METHOD(name, length, magic) \
    { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, \
      JS_DEF_CFUNC, magic, \
      { .func = { length, JS_CFUNC_generic_magic, \
                  { .generic_magic = ns_subtle_method } } } }
#define NS_WC_GETTER(name, fn) \
    { name, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET, 0, \
      { .getset = { .get = { .getter = fn }, .set = { .setter = NULL } } } }

static const JSCFunctionListEntry ns_cryptokey_proto_funcs[] = {
    NS_WC_GETTER("type", ns_cryptokey_get_type),
    NS_WC_GETTER("extractable", ns_cryptokey_get_extractable),
    NS_WC_GETTER("algorithm", ns_cryptokey_get_algorithm),
    NS_WC_GETTER("usages", ns_cryptokey_get_usages),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "CryptoKey", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry ns_subtle_proto_funcs[] = {
    NS_WC_METHOD("encrypt", 3, NS_WC_ENCRYPT),
    NS_WC_METHOD("decrypt", 3, NS_WC_DECRYPT),
    NS_WC_METHOD("sign", 3, NS_WC_SIGN),
    NS_WC_METHOD("verify", 4, NS_WC_VERIFY),
    NS_WC_METHOD("digest", 2, NS_WC_DIGEST),
    NS_WC_METHOD("generateKey", 3, NS_WC_GENERATE_KEY),
    NS_WC_METHOD("deriveKey", 5, NS_WC_DERIVE_KEY),
    NS_WC_METHOD("deriveBits", 2, NS_WC_DERIVE_BITS),
    NS_WC_METHOD("importKey", 5, NS_WC_IMPORT_KEY),
    NS_WC_METHOD("exportKey", 2, NS_WC_EXPORT_KEY),
    NS_WC_METHOD("wrapKey", 4, NS_WC_WRAP_KEY),
    NS_WC_METHOD("unwrapKey", 7, NS_WC_UNWRAP_KEY),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "SubtleCrypto",
                       JS_PROP_CONFIGURABLE),
};

/* Registers a class with an illegal-constructor interface object on global. */
static void
ns_wc_install_interface(JSContext *ctx, JSValueConst global, const char *name,
                        JSClassID *class_id, JSClassDef *def,
                        const JSCFunctionListEntry *funcs, int nfuncs)
{
    ns_new_class_id(class_id);
    JS_NewClass(JS_GetRuntime(ctx), *class_id, def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, funcs, nfuncs);
    JSValue ctor = JS_NewCFunction2(ctx, ns_wc_illegal_constructor, name, 0,
                                    JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, *class_id, proto);
    JS_DefinePropertyValueStr(ctx, global, name, ctor,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

/* Installs CryptoKey and SubtleCrypto and defines crypto.subtle. */
void
ns_webcrypto_install(JSContext *ctx, JSValueConst global, JSValueConst crypto)
{
    ns_wc_install_interface(ctx, global, "CryptoKey", &ns_cryptokey_class_id,
                            &ns_cryptokey_class, ns_cryptokey_proto_funcs,
                            (int)G_N_ELEMENTS(ns_cryptokey_proto_funcs));
    ns_wc_install_interface(ctx, global, "SubtleCrypto", &ns_subtle_class_id,
                            &ns_subtle_class, ns_subtle_proto_funcs,
                            (int)G_N_ELEMENTS(ns_subtle_proto_funcs));
    if (!JS_IsObject(crypto)) return;
    JSValue subtle = JS_NewObjectClass(ctx, (int)ns_subtle_class_id);
    JS_DefinePropertyValueStr(ctx, crypto, "subtle", subtle,
                              JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}
