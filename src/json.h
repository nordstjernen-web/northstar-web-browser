/* Northstar Web Browser — small JSON value tree: parse, build and serialize.
 * Used by the WebDriver server for request and response bodies.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_JSON_H
#define NS_JSON_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_json_type {
    NS_JSON_NULL,
    NS_JSON_BOOL,
    NS_JSON_NUMBER,
    NS_JSON_STRING,
    NS_JSON_ARRAY,
    NS_JSON_OBJECT,
} ns_json_type;

typedef struct ns_json ns_json;

/* A JSON value.  Arrays keep their elements in items; objects keep their
 * values in items and the matching member names, in order, in keys. */
struct ns_json {
    ns_json_type type;
    gboolean     boolean;
    double       number;
    char        *string;
    GPtrArray   *items;
    GPtrArray   *keys;
};

ns_json *ns_json_parse(const char *text, gsize len, char **out_error);
void     ns_json_free(ns_json *v);
ns_json *ns_json_copy(const ns_json *v);

ns_json *ns_json_null(void);
ns_json *ns_json_bool(gboolean b);
ns_json *ns_json_number(double n);
ns_json *ns_json_string(const char *s);
ns_json *ns_json_string_take(char *s);
ns_json *ns_json_array(void);
ns_json *ns_json_object(void);

void     ns_json_array_add(ns_json *array, ns_json *value);
guint    ns_json_array_len(const ns_json *array);
ns_json *ns_json_array_get(const ns_json *array, guint index);

void     ns_json_object_set(ns_json *object, const char *key, ns_json *value);
ns_json *ns_json_object_get(const ns_json *object, const char *key);
gboolean ns_json_object_has(const ns_json *object, const char *key);
void     ns_json_object_remove(ns_json *object, const char *key);
guint    ns_json_object_len(const ns_json *object);

gboolean    ns_json_is(const ns_json *v, ns_json_type type);
const char *ns_json_get_string(const ns_json *object, const char *key);

void  ns_json_write(GString *out, const ns_json *v);
char *ns_json_to_string(const ns_json *v);
void  ns_json_write_string(GString *out, const char *s);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ns_json, ns_json_free)

G_END_DECLS

#endif
