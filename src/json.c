/* Northstar Web Browser — small JSON value tree: parse, build and serialize.
 * RFC 8259 parser with a nesting limit, and a serializer that never emits NaN.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NS_JSON_MAX_DEPTH 256

typedef struct {
    const char *p;
    const char *end;
    char       *error;
} json_parser;

static ns_json *
json_new(ns_json_type type)
{
    ns_json *v = g_new0(ns_json, 1);
    v->type = type;
    if (type == NS_JSON_ARRAY || type == NS_JSON_OBJECT)
        v->items = g_ptr_array_new_with_free_func((GDestroyNotify)ns_json_free);
    if (type == NS_JSON_OBJECT)
        v->keys = g_ptr_array_new_with_free_func(g_free);
    return v;
}

ns_json *ns_json_null(void) { return json_new(NS_JSON_NULL); }

ns_json *
ns_json_bool(gboolean b)
{
    ns_json *v = json_new(NS_JSON_BOOL);
    v->boolean = b ? TRUE : FALSE;
    return v;
}

ns_json *
ns_json_number(double n)
{
    ns_json *v = json_new(NS_JSON_NUMBER);
    v->number = n;
    return v;
}

ns_json *
ns_json_string(const char *s)
{
    return ns_json_string_take(g_strdup(s ? s : ""));
}

/* String value owning s (g_malloc'd); NULL becomes the empty string. */
ns_json *
ns_json_string_take(char *s)
{
    ns_json *v = json_new(NS_JSON_STRING);
    v->string = s ? s : g_strdup("");
    return v;
}

ns_json *ns_json_array(void) { return json_new(NS_JSON_ARRAY); }
ns_json *ns_json_object(void) { return json_new(NS_JSON_OBJECT); }

void
ns_json_free(ns_json *v)
{
    if (!v) return;
    g_free(v->string);
    if (v->items) g_ptr_array_free(v->items, TRUE);
    if (v->keys) g_ptr_array_free(v->keys, TRUE);
    g_free(v);
}

/* Deep copy of a value tree. */
ns_json *
ns_json_copy(const ns_json *v)
{
    if (!v) return NULL;
    ns_json *c = json_new(v->type);
    c->boolean = v->boolean;
    c->number = v->number;
    c->string = g_strdup(v->string);
    for (guint i = 0; v->items && i < v->items->len; i++)
        g_ptr_array_add(c->items, ns_json_copy(g_ptr_array_index(v->items, i)));
    for (guint i = 0; v->keys && i < v->keys->len; i++)
        g_ptr_array_add(c->keys, g_strdup(g_ptr_array_index(v->keys, i)));
    return c;
}

gboolean
ns_json_is(const ns_json *v, ns_json_type type)
{
    return v && v->type == type;
}

void
ns_json_array_add(ns_json *array, ns_json *value)
{
    if (!ns_json_is(array, NS_JSON_ARRAY) || !value) {
        ns_json_free(value);
        return;
    }
    g_ptr_array_add(array->items, value);
}

guint
ns_json_array_len(const ns_json *array)
{
    return ns_json_is(array, NS_JSON_ARRAY) ? array->items->len : 0;
}

ns_json *
ns_json_array_get(const ns_json *array, guint index)
{
    if (!ns_json_is(array, NS_JSON_ARRAY) || index >= array->items->len)
        return NULL;
    return g_ptr_array_index(array->items, index);
}

/* Index of a member name in an object, or -1. */
static int
json_object_index(const ns_json *object, const char *key)
{
    if (!ns_json_is(object, NS_JSON_OBJECT) || !key) return -1;
    for (guint i = 0; i < object->keys->len; i++)
        if (strcmp(g_ptr_array_index(object->keys, i), key) == 0)
            return (int)i;
    return -1;
}

/* Sets a member, replacing an existing one in place; takes value. */
void
ns_json_object_set(ns_json *object, const char *key, ns_json *value)
{
    if (!ns_json_is(object, NS_JSON_OBJECT) || !key || !value) {
        ns_json_free(value);
        return;
    }
    int i = json_object_index(object, key);
    if (i >= 0) {
        ns_json_free(g_ptr_array_index(object->items, i));
        g_ptr_array_index(object->items, i) = value;
        return;
    }
    g_ptr_array_add(object->keys, g_strdup(key));
    g_ptr_array_add(object->items, value);
}

ns_json *
ns_json_object_get(const ns_json *object, const char *key)
{
    int i = json_object_index(object, key);
    return i >= 0 ? g_ptr_array_index(object->items, i) : NULL;
}

gboolean
ns_json_object_has(const ns_json *object, const char *key)
{
    return json_object_index(object, key) >= 0;
}

void
ns_json_object_remove(ns_json *object, const char *key)
{
    int i = json_object_index(object, key);
    if (i < 0) return;
    g_ptr_array_remove_index(object->keys, (guint)i);
    g_ptr_array_remove_index(object->items, (guint)i);
}

guint
ns_json_object_len(const ns_json *object)
{
    return ns_json_is(object, NS_JSON_OBJECT) ? object->keys->len : 0;
}

/* A member's string value, or NULL when absent or not a string. */
const char *
ns_json_get_string(const ns_json *object, const char *key)
{
    ns_json *v = ns_json_object_get(object, key);
    return ns_json_is(v, NS_JSON_STRING) ? v->string : NULL;
}

/* ---- parser ---- */

static void
json_fail(json_parser *ps, const char *what)
{
    if (!ps->error) ps->error = g_strdup(what);
}

static void
json_skip_ws(json_parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int
json_hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* Parses a string literal at ps->p (on the opening quote); g_malloc'd UTF-8. */
static char *
json_parse_string_raw(json_parser *ps)
{
    ps->p++;
    GString *s = g_string_new(NULL);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            return g_string_free(s, FALSE);
        }
        if (c < 0x20) {
            json_fail(ps, "control character in string");
            break;
        }
        if (c != '\\') {
            g_string_append_c(s, (char)c);
            ps->p++;
            continue;
        }
        if (ps->end - ps->p < 2) break;
        char e = ps->p[1];
        ps->p += 2;
        switch (e) {
        case '"':  g_string_append_c(s, '"'); break;
        case '\\': g_string_append_c(s, '\\'); break;
        case '/':  g_string_append_c(s, '/'); break;
        case 'b':  g_string_append_c(s, '\b'); break;
        case 'f':  g_string_append_c(s, '\f'); break;
        case 'n':  g_string_append_c(s, '\n'); break;
        case 'r':  g_string_append_c(s, '\r'); break;
        case 't':  g_string_append_c(s, '\t'); break;
        case 'u': {
            if (ps->end - ps->p < 4) goto bad;
            int u = json_hex4(ps->p);
            if (u < 0) goto bad;
            ps->p += 4;
            gunichar ch = (gunichar)u;
            if (u >= 0xD800 && u <= 0xDBFF && ps->end - ps->p >= 6 &&
                ps->p[0] == '\\' && ps->p[1] == 'u') {
                int lo = json_hex4(ps->p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    ch = 0x10000 + (((gunichar)u - 0xD800) << 10) +
                         ((gunichar)lo - 0xDC00);
                    ps->p += 6;
                }
            }
            if (ch >= 0xD800 && ch <= 0xDFFF) ch = 0xFFFD;
            g_string_append_unichar(s, ch);
            break;
        }
        default:
            goto bad;
        }
    }
    json_fail(ps, "unterminated string");
    g_string_free(s, TRUE);
    return NULL;
bad:
    json_fail(ps, "bad escape in string");
    g_string_free(s, TRUE);
    return NULL;
}

static ns_json *json_parse_value(json_parser *ps, int depth);

static ns_json *
json_parse_number(json_parser *ps)
{
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end || !g_ascii_isdigit(*ps->p)) {
        json_fail(ps, "bad number");
        return NULL;
    }
    if (*ps->p == '0') ps->p++;
    else while (ps->p < ps->end && g_ascii_isdigit(*ps->p)) ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || !g_ascii_isdigit(*ps->p)) {
            json_fail(ps, "bad number");
            return NULL;
        }
        while (ps->p < ps->end && g_ascii_isdigit(*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || !g_ascii_isdigit(*ps->p)) {
            json_fail(ps, "bad number");
            return NULL;
        }
        while (ps->p < ps->end && g_ascii_isdigit(*ps->p)) ps->p++;
    }
    char *tmp = g_strndup(start, (gsize)(ps->p - start));
    double d = g_ascii_strtod(tmp, NULL);
    g_free(tmp);
    return ns_json_number(d);
}

static gboolean
json_literal(json_parser *ps, const char *word)
{
    gsize n = strlen(word);
    if ((gsize)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
        return FALSE;
    ps->p += n;
    return TRUE;
}

static ns_json *
json_parse_array(json_parser *ps, int depth)
{
    ps->p++;
    ns_json *arr = ns_json_array();
    json_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        ns_json *item = json_parse_value(ps, depth + 1);
        if (!item) break;
        g_ptr_array_add(arr->items, item);
        json_skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return arr;
        }
        json_fail(ps, "expected ',' or ']'");
        break;
    }
    ns_json_free(arr);
    return NULL;
}

static ns_json *
json_parse_object(json_parser *ps, int depth)
{
    ps->p++;
    ns_json *obj = ns_json_object();
    json_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        json_skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            json_fail(ps, "expected member name");
            break;
        }
        char *key = json_parse_string_raw(ps);
        if (!key) break;
        json_skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            g_free(key);
            json_fail(ps, "expected ':'");
            break;
        }
        ps->p++;
        ns_json *val = json_parse_value(ps, depth + 1);
        if (!val) {
            g_free(key);
            break;
        }
        ns_json_object_set(obj, key, val);
        g_free(key);
        json_skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return obj;
        }
        json_fail(ps, "expected ',' or '}'");
        break;
    }
    ns_json_free(obj);
    return NULL;
}

static ns_json *
json_parse_value(json_parser *ps, int depth)
{
    if (depth > NS_JSON_MAX_DEPTH) {
        json_fail(ps, "nesting too deep");
        return NULL;
    }
    json_skip_ws(ps);
    if (ps->p >= ps->end) {
        json_fail(ps, "unexpected end of input");
        return NULL;
    }
    switch (*ps->p) {
    case '{': return json_parse_object(ps, depth);
    case '[': return json_parse_array(ps, depth);
    case '"': {
        char *s = json_parse_string_raw(ps);
        return s ? ns_json_string_take(s) : NULL;
    }
    case 't':
        if (json_literal(ps, "true")) return ns_json_bool(TRUE);
        break;
    case 'f':
        if (json_literal(ps, "false")) return ns_json_bool(FALSE);
        break;
    case 'n':
        if (json_literal(ps, "null")) return ns_json_null();
        break;
    default:
        if (*ps->p == '-' || g_ascii_isdigit(*ps->p))
            return json_parse_number(ps);
        break;
    }
    json_fail(ps, "unexpected character");
    return NULL;
}

/* Parses one JSON text (UTF-8); NULL and an error message on failure. */
ns_json *
ns_json_parse(const char *text, gsize len, char **out_error)
{
    if (out_error) *out_error = NULL;
    if (!text) {
        if (out_error) *out_error = g_strdup("no input");
        return NULL;
    }
    if (!g_utf8_validate(text, (gssize)len, NULL)) {
        if (out_error) *out_error = g_strdup("input is not valid UTF-8");
        return NULL;
    }
    json_parser ps = { text, text + len, NULL };
    ns_json *v = json_parse_value(&ps, 0);
    if (v) {
        json_skip_ws(&ps);
        if (ps.p != ps.end) {
            ns_json_free(v);
            v = NULL;
            json_fail(&ps, "trailing characters after JSON value");
        }
    }
    if (!v) {
        if (out_error) *out_error = ps.error ? ps.error : g_strdup("bad JSON");
        else g_free(ps.error);
        return NULL;
    }
    g_free(ps.error);
    return v;
}

/* ---- serializer ---- */

/* Appends s as a quoted JSON string, escaping controls and U+2028/U+2029. */
void
ns_json_write_string(GString *out, const char *s)
{
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        case '\b': g_string_append(out, "\\b"); break;
        case '\f': g_string_append(out, "\\f"); break;
        default:
            if (c < 0x20) {
                g_string_append_printf(out, "\\u%04x", c);
            } else if (c == 0xE2 && p[1] == 0x80 &&
                       (p[2] == 0xA8 || p[2] == 0xA9)) {
                g_string_append(out, p[2] == 0xA8 ? "\\u2028" : "\\u2029");
                p += 2;
            } else {
                g_string_append_c(out, (char)c);
            }
        }
    }
    g_string_append_c(out, '"');
}

static void
json_write_number(GString *out, double d)
{
    if (!isfinite(d)) {
        g_string_append(out, "null");
        return;
    }
    if (d == floor(d) && fabs(d) < 9007199254740992.0) {
        g_string_append_printf(out, "%.0f", d);
        return;
    }
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%.17g", d);
    g_string_append(out, buf);
}

void
ns_json_write(GString *out, const ns_json *v)
{
    if (!v) {
        g_string_append(out, "null");
        return;
    }
    switch (v->type) {
    case NS_JSON_NULL:
        g_string_append(out, "null");
        break;
    case NS_JSON_BOOL:
        g_string_append(out, v->boolean ? "true" : "false");
        break;
    case NS_JSON_NUMBER:
        json_write_number(out, v->number);
        break;
    case NS_JSON_STRING:
        ns_json_write_string(out, v->string);
        break;
    case NS_JSON_ARRAY:
        g_string_append_c(out, '[');
        for (guint i = 0; i < v->items->len; i++) {
            if (i) g_string_append_c(out, ',');
            ns_json_write(out, g_ptr_array_index(v->items, i));
        }
        g_string_append_c(out, ']');
        break;
    case NS_JSON_OBJECT:
        g_string_append_c(out, '{');
        for (guint i = 0; i < v->keys->len; i++) {
            if (i) g_string_append_c(out, ',');
            ns_json_write_string(out, g_ptr_array_index(v->keys, i));
            g_string_append_c(out, ':');
            ns_json_write(out, g_ptr_array_index(v->items, i));
        }
        g_string_append_c(out, '}');
        break;
    }
}

char *
ns_json_to_string(const ns_json *v)
{
    GString *out = g_string_new(NULL);
    ns_json_write(out, v);
    return g_string_free(out, FALSE);
}
