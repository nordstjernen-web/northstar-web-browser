/* Northstar Web Browser — calc() tree simplification and specified-value serialization.
 * Parses math functions into trees, simplifies per CSS Values 4 and prints the canonical form.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "css.h"

#include <math.h>
#include <string.h>

typedef enum {
    CALC_NODE_NUMERIC,
    CALC_NODE_SUM,
    CALC_NODE_PRODUCT,
    CALC_NODE_NEGATE,
    CALC_NODE_INVERT,
    CALC_NODE_FUNCTION,
    CALC_NODE_OPAQUE,
} calc_node_kind;

typedef struct calc_node {
    calc_node_kind kind;
    double value;
    char *unit;
    char *text;
    GPtrArray *children;
} calc_node;

typedef struct {
    const char *p;
    const char *end;
    int depth;
} calc_cursor;

#define CALC_MAX_DEPTH 32

static void
calc_node_free(gpointer data)
{
    calc_node *n = data;
    if (!n) return;
    g_free(n->unit);
    g_free(n->text);
    if (n->children) g_ptr_array_free(n->children, TRUE);
    g_free(n);
}

/* Allocates a calc tree node; non-leaf kinds get an owning children array. */
static calc_node *
calc_node_new(calc_node_kind kind)
{
    calc_node *n = g_new0(calc_node, 1);
    n->kind = kind;
    if (kind != CALC_NODE_NUMERIC && kind != CALC_NODE_OPAQUE)
        n->children = g_ptr_array_new_with_free_func(calc_node_free);
    return n;
}

static calc_node *
calc_numeric(double value, const char *unit)
{
    calc_node *n = calc_node_new(CALC_NODE_NUMERIC);
    n->value = value;
    n->unit = g_strdup(unit);
    return n;
}

static calc_node *
calc_wrap(calc_node_kind kind, calc_node *child)
{
    calc_node *n = calc_node_new(kind);
    g_ptr_array_add(n->children, child);
    return n;
}

static calc_node *
calc_child(const calc_node *n, guint i)
{
    return g_ptr_array_index(n->children, i);
}

/* Detaches child i from n (leaving NULL) and returns it to the caller. */
static calc_node *
calc_take_child(calc_node *n, guint i)
{
    calc_node *c = g_ptr_array_index(n->children, i);
    n->children->pdata[i] = NULL;
    return c;
}

/* Skips ASCII whitespace; returns TRUE if any was consumed. */
static gboolean
calc_skip_ws(calc_cursor *c)
{
    const char *start = c->p;
    while (c->p < c->end && ns_is_ascii_ws(*c->p)) c->p++;
    return c->p > start;
}

static gboolean
calc_ident_char(char ch)
{
    return g_ascii_isalnum(ch) || ch == '-' || ch == '_' ||
           (unsigned char)ch >= 0x80;
}

/* Reads an identifier as a lowercased string, or NULL if none; caller frees. */
static char *
calc_read_ident(calc_cursor *c)
{
    const char *start = c->p;
    while (c->p < c->end && calc_ident_char(*c->p)) c->p++;
    if (c->p == start) return NULL;
    return g_ascii_strdown(start, c->p - start);
}

static const struct {
    const char *unit;
    const char *canonical;
    double factor;
} calc_units[] = {
    { "px", "px", 1.0 },
    { "cm", "px", 96.0 / 2.54 },
    { "mm", "px", 96.0 / 25.4 },
    { "q", "px", 96.0 / 101.6 },
    { "in", "px", 96.0 },
    { "pt", "px", 96.0 / 72.0 },
    { "pc", "px", 16.0 },
    { "deg", "deg", 1.0 },
    { "grad", "deg", 0.9 },
    { "rad", "deg", 180.0 / G_PI },
    { "turn", "deg", 360.0 },
    { "s", "s", 1.0 },
    { "ms", "s", 0.001 },
    { "hz", "hz", 1.0 },
    { "khz", "hz", 1000.0 },
    { "dppx", "dppx", 1.0 },
    { "x", "dppx", 1.0 },
    { "dpi", "dppx", 1.0 / 96.0 },
    { "dpcm", "dppx", 2.54 / 96.0 },
};

static const char *const calc_relative_units[] = {
    "em", "rem", "ex", "rex", "ch", "rch", "cap", "rcap", "ic", "ric",
    "lh", "rlh", "vw", "vh", "vi", "vb", "vmin", "vmax", "svw", "svh",
    "svi", "svb", "svmin", "svmax", "lvw", "lvh", "lvi", "lvb", "lvmin",
    "lvmax", "dvw", "dvh", "dvi", "dvb", "dvmin", "dvmax", "cqw", "cqh",
    "cqi", "cqb", "cqmin", "cqmax", "fr",
};

/* Converts absolute units to canonical ones, keeps relative units; FALSE if unknown. */
static gboolean
calc_canonical_unit(char **unit, double *value)
{
    if (!**unit || strcmp(*unit, "%") == 0) return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(calc_units); i++) {
        if (strcmp(*unit, calc_units[i].unit) != 0) continue;
        *value *= calc_units[i].factor;
        g_free(*unit);
        *unit = g_strdup(calc_units[i].canonical);
        return TRUE;
    }
    for (gsize i = 0; i < G_N_ELEMENTS(calc_relative_units); i++)
        if (strcmp(*unit, calc_relative_units[i]) == 0) return TRUE;
    return FALSE;
}

static const char *const calc_math_functions[] = {
    "calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign",
    "hypot", "pow", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan",
    "atan2", "exp", "log",
};

static gboolean
calc_is_math_function(const char *name)
{
    for (gsize i = 0; i < G_N_ELEMENTS(calc_math_functions); i++)
        if (strcmp(name, calc_math_functions[i]) == 0) return TRUE;
    return FALSE;
}

static calc_node *calc_parse_sum(calc_cursor *c);

/* Returns the ')' closing the paren at p, or NULL if unbalanced or a string appears. */
static const char *
calc_matching_paren(const char *p, const char *end)
{
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '"' || *p == '\'') return NULL;
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
    }
    return NULL;
}

/* Parses math function arguments, allowing round() strategy and clamp() none keywords. */
static calc_node *
calc_parse_function(calc_cursor *c, char *name)
{
    if (!calc_is_math_function(name)) {
        g_free(name);
        return NULL;
    }
    c->p++;
    calc_node *n = calc_node_new(CALC_NODE_FUNCTION);
    n->text = name;
    while (TRUE) {
        calc_skip_ws(c);
        calc_node *arg = NULL;
        const char *save = c->p;
        char *word = calc_read_ident(c);
        gboolean keyword_arg = word &&
            ((strcmp(n->text, "round") == 0 && n->children->len == 0 &&
              (strcmp(word, "nearest") == 0 || strcmp(word, "up") == 0 ||
               strcmp(word, "down") == 0 || strcmp(word, "to-zero") == 0)) ||
             (strcmp(n->text, "clamp") == 0 && strcmp(word, "none") == 0));
        if (keyword_arg) {
            arg = calc_node_new(CALC_NODE_OPAQUE);
            arg->text = word;
        } else {
            g_free(word);
            c->p = save;
            arg = calc_parse_sum(c);
        }
        if (!arg) {
            calc_node_free(n);
            return NULL;
        }
        g_ptr_array_add(n->children, arg);
        calc_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ')') {
            c->p++;
            return n;
        }
        calc_node_free(n);
        return NULL;
    }
}

/* Parses a calc value: parenthesized sum, dimension, nested function or constant (e, pi...). */
static calc_node *
calc_parse_value(calc_cursor *c)
{
    calc_skip_ws(c);
    if (c->p >= c->end || c->depth > CALC_MAX_DEPTH) return NULL;
    char ch = *c->p;
    if (ch == '(') {
        c->p++;
        c->depth++;
        calc_node *inner = calc_parse_sum(c);
        c->depth--;
        calc_skip_ws(c);
        if (!inner || c->p >= c->end || *c->p != ')') {
            calc_node_free(inner);
            return NULL;
        }
        c->p++;
        return inner;
    }
    gboolean signed_number = (ch == '+' || ch == '-') && c->p + 1 < c->end &&
        (g_ascii_isdigit(c->p[1]) || c->p[1] == '.');
    if (g_ascii_isdigit(ch) || ch == '.' || signed_number) {
        char *num_end = NULL;
        char *copy = g_strndup(c->p, c->end - c->p);
        double v = g_ascii_strtod(copy, &num_end);
        gsize used = num_end ? (gsize)(num_end - copy) : 0;
        g_free(copy);
        if (used == 0) return NULL;
        c->p += used;
        char *unit = NULL;
        if (c->p < c->end && *c->p == '%') {
            c->p++;
            unit = g_strdup("%");
        } else {
            unit = calc_read_ident(c);
            if (!unit) unit = g_strdup("");
        }
        if (!calc_canonical_unit(&unit, &v)) {
            g_free(unit);
            return NULL;
        }
        calc_node *n = calc_numeric(v, unit);
        g_free(unit);
        return n;
    }
    char *word = calc_read_ident(c);
    if (!word) return NULL;
    if (c->p < c->end && *c->p == '(') {
        c->depth++;
        calc_node *f = calc_parse_function(c, word);
        c->depth--;
        return f;
    }
    double constant = NAN;
    gboolean known = TRUE;
    if (strcmp(word, "e") == 0) constant = G_E;
    else if (strcmp(word, "pi") == 0) constant = G_PI;
    else if (strcmp(word, "infinity") == 0) constant = INFINITY;
    else if (strcmp(word, "-infinity") == 0) constant = -INFINITY;
    else if (strcmp(word, "nan") == 0) constant = NAN;
    else known = FALSE;
    g_free(word);
    return known ? calc_numeric(constant, "") : NULL;
}

/* Parses a "*" and "/" chain into a product node, wrapping divisors in INVERT. */
static calc_node *
calc_parse_product(calc_cursor *c)
{
    calc_node *first = calc_parse_value(c);
    if (!first) return NULL;
    calc_node *product = NULL;
    while (TRUE) {
        const char *save = c->p;
        calc_skip_ws(c);
        if (c->p >= c->end || (*c->p != '*' && *c->p != '/')) {
            c->p = save;
            break;
        }
        gboolean divide = *c->p == '/';
        c->p++;
        calc_node *rhs = calc_parse_value(c);
        if (!rhs) {
            calc_node_free(product ? product : first);
            return NULL;
        }
        if (!product) {
            product = calc_node_new(CALC_NODE_PRODUCT);
            g_ptr_array_add(product->children, first);
        }
        g_ptr_array_add(product->children,
                        divide ? calc_wrap(CALC_NODE_INVERT, rhs) : rhs);
    }
    return product ? product : first;
}

/* Parses a "+"/"-" chain (operators need whitespace on both sides) into a sum node. */
static calc_node *
calc_parse_sum(calc_cursor *c)
{
    calc_node *first = calc_parse_product(c);
    if (!first) return NULL;
    calc_node *sum = NULL;
    while (TRUE) {
        const char *save = c->p;
        gboolean spaced = calc_skip_ws(c);
        if (c->p >= c->end || (*c->p != '+' && *c->p != '-') || !spaced ||
            c->p + 1 >= c->end || !ns_is_ascii_ws(c->p[1])) {
            c->p = save;
            break;
        }
        gboolean minus = *c->p == '-';
        c->p++;
        calc_node *rhs = calc_parse_product(c);
        if (!rhs) {
            calc_node_free(sum ? sum : first);
            return NULL;
        }
        if (!sum) {
            sum = calc_node_new(CALC_NODE_SUM);
            g_ptr_array_add(sum->children, first);
        }
        g_ptr_array_add(sum->children,
                        minus ? calc_wrap(CALC_NODE_NEGATE, rhs) : rhs);
    }
    return sum ? sum : first;
}

static gboolean
calc_is_number(const calc_node *n)
{
    return n->kind == CALC_NODE_NUMERIC && !*n->unit;
}

static calc_node *calc_simplify(calc_node *n, gboolean *ok);

/* Splices children of the same kind as n into n, flattening nested sums or products. */
static void
calc_flatten(calc_node *n)
{
    GPtrArray *flat = g_ptr_array_new_with_free_func(calc_node_free);
    for (guint i = 0; i < n->children->len; i++) {
        calc_node *child = calc_take_child(n, i);
        if (child->kind == n->kind) {
            for (guint k = 0; k < child->children->len; k++)
                g_ptr_array_add(flat, calc_take_child(child, k));
            calc_node_free(child);
        } else {
            g_ptr_array_add(flat, child);
        }
    }
    g_ptr_array_free(n->children, TRUE);
    n->children = flat;
}

/* Frees n and returns its only child. */
static calc_node *
calc_single_child(calc_node *n)
{
    calc_node *only = calc_take_child(n, 0);
    calc_node_free(n);
    return only;
}

/* Combines same-unit terms; clears ok when numbers mix with dimensions. */
static calc_node *
calc_simplify_sum(calc_node *n, gboolean *ok)
{
    calc_flatten(n);
    for (guint i = 0; i < n->children->len; i++) {
        calc_node *a = calc_child(n, i);
        if (a->kind != CALC_NODE_NUMERIC) continue;
        for (guint k = i + 1; k < n->children->len; k++) {
            calc_node *b = calc_child(n, k);
            if (b->kind != CALC_NODE_NUMERIC || strcmp(a->unit, b->unit) != 0)
                continue;
            a->value += b->value;
            g_ptr_array_remove_index(n->children, k--);
        }
    }
    gboolean number = FALSE, dimension = FALSE;
    for (guint i = 0; i < n->children->len; i++) {
        calc_node *a = calc_child(n, i);
        if (a->kind != CALC_NODE_NUMERIC) continue;
        if (*a->unit) dimension = TRUE;
        else number = TRUE;
    }
    if (number && dimension) *ok = FALSE;
    return n->children->len == 1 ? calc_single_child(n) : n;
}

/* Folds plain-number factors and distributes them into a single numeric or sum child. */
static calc_node *
calc_simplify_product(calc_node *n)
{
    calc_flatten(n);
    double factor = 1;
    int numbers = 0;
    for (guint i = 0; i < n->children->len; i++) {
        calc_node *a = calc_child(n, i);
        if (!calc_is_number(a)) continue;
        factor *= a->value;
        numbers++;
        g_ptr_array_remove_index(n->children, i--);
    }
    if (n->children->len == 0) {
        calc_node_free(n);
        return calc_numeric(factor, "");
    }
    if (n->children->len == 1) {
        calc_node *other = calc_child(n, 0);
        if (other->kind == CALC_NODE_NUMERIC) {
            other->value *= factor;
            return calc_single_child(n);
        }
        if (other->kind == CALC_NODE_SUM) {
            gboolean all_numeric = TRUE;
            for (guint i = 0; i < other->children->len; i++)
                if (calc_child(other, i)->kind != CALC_NODE_NUMERIC)
                    all_numeric = FALSE;
            if (all_numeric) {
                for (guint i = 0; i < other->children->len; i++)
                    calc_child(other, i)->value *= factor;
                return calc_single_child(n);
            }
        }
        if (numbers == 0 || factor == 1) return calc_single_child(n);
    }
    if (numbers > 0 && !(factor == 1 && n->children->len > 0))
        g_ptr_array_insert(n->children, 0, calc_numeric(factor, ""));
    return n;
}

/* Returns TRUE if children from index on are numerics sharing one non-% unit. */
static gboolean
calc_same_unit_numerics(const calc_node *n, guint from)
{
    const char *unit = NULL;
    for (guint i = from; i < n->children->len; i++) {
        const calc_node *a = calc_child(n, i);
        if (a->kind != CALC_NODE_NUMERIC) return FALSE;
        if (unit && strcmp(unit, a->unit) != 0) return FALSE;
        unit = a->unit;
    }
    return unit && strcmp(unit, "%") != 0;
}

/* Less-than that orders -0 before +0, as min()/max() require. */
static gboolean
calc_less_signed(double a, double b)
{
    return a < b || (a == 0 && b == 0 && signbit(a) && !signbit(b));
}

/* Simplifies calc()/min()/max()/abs() whose arguments are already resolvable. */
static calc_node *
calc_simplify_function(calc_node *n)
{
    const char *name = n->text;
    if (strcmp(name, "calc") == 0 && n->children->len == 1)
        return calc_single_child(n);
    if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) &&
        n->children->len == 1)
        return calc_single_child(n);
    if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) &&
        calc_same_unit_numerics(n, 0)) {
        gboolean is_min = name[1] == 'i';
        guint best = 0;
        for (guint i = 1; i < n->children->len; i++) {
            double v = calc_child(n, i)->value, b = calc_child(n, best)->value;
            if (isnan(v) || isnan(b)) {
                calc_child(n, 0)->value = NAN;
                best = 0;
                break;
            }
            if (is_min ? calc_less_signed(v, b) : calc_less_signed(b, v))
                best = i;
        }
        calc_node *r = calc_take_child(n, best);
        calc_node_free(n);
        return r;
    }
    if (strcmp(name, "abs") == 0 && n->children->len == 1 &&
        calc_child(n, 0)->kind == CALC_NODE_NUMERIC &&
        strcmp(calc_child(n, 0)->unit, "%") != 0) {
        calc_node *r = calc_single_child(n);
        r->value = fabs(r->value);
        return r;
    }
    return n;
}

/* Recursively simplifies a calc tree per CSS Values 4 calc simplification. */
static calc_node *
calc_simplify(calc_node *n, gboolean *ok)
{
    if (n->children)
        for (guint i = 0; i < n->children->len; i++)
            n->children->pdata[i] = calc_simplify(calc_child(n, i), ok);
    switch (n->kind) {
    case CALC_NODE_NEGATE: {
        calc_node *child = calc_child(n, 0);
        if (child->kind == CALC_NODE_NUMERIC) {
            child->value = -child->value;
            return calc_single_child(n);
        }
        if (child->kind == CALC_NODE_NEGATE) {
            calc_node *inner = calc_take_child(child, 0);
            calc_node_free(n);
            return inner;
        }
        return n;
    }
    case CALC_NODE_INVERT: {
        calc_node *child = calc_child(n, 0);
        if (calc_is_number(child)) {
            child->value = 1.0 / child->value;
            return calc_single_child(n);
        }
        if (child->kind == CALC_NODE_INVERT) {
            calc_node *inner = calc_take_child(child, 0);
            calc_node_free(n);
            return inner;
        }
        return n;
    }
    case CALC_NODE_SUM:
        return calc_simplify_sum(n, ok);
    case CALC_NODE_PRODUCT:
        return calc_simplify_product(n);
    case CALC_NODE_FUNCTION:
        return calc_simplify_function(n);
    default:
        return n;
    }
}

/* Sort rank for serialization: number, percentage, dimension, then other nodes. */
static int
calc_sort_rank(const calc_node *n)
{
    if (n->kind != CALC_NODE_NUMERIC) return 3;
    if (!*n->unit) return 0;
    if (strcmp(n->unit, "%") == 0) return 1;
    return 2;
}

/* Reorders children by rank, sorting dimensions by unit name, for canonical output. */
static void
calc_sort_children(calc_node *n)
{
    GPtrArray *sorted = g_ptr_array_new_with_free_func(calc_node_free);
    for (int rank = 0; rank <= 3; rank++) {
        GPtrArray *bucket = g_ptr_array_new();
        for (guint i = 0; i < n->children->len; i++) {
            calc_node *c = calc_child(n, i);
            if (c && calc_sort_rank(c) == rank) g_ptr_array_add(bucket, c);
        }
        if (rank == 2) {
            for (guint i = 1; i < bucket->len; i++)
                for (guint k = i; k > 0; k--) {
                    calc_node *a = g_ptr_array_index(bucket, k - 1);
                    calc_node *b = g_ptr_array_index(bucket, k);
                    if (g_ascii_strcasecmp(a->unit, b->unit) <= 0) break;
                    bucket->pdata[k - 1] = b;
                    bucket->pdata[k] = a;
                }
        }
        for (guint i = 0; i < bucket->len; i++)
            g_ptr_array_add(sorted, g_ptr_array_index(bucket, i));
        g_ptr_array_free(bucket, TRUE);
    }
    g_ptr_array_set_free_func(n->children, NULL);
    g_ptr_array_free(n->children, TRUE);
    n->children = sorted;
}

/* Appends a number with NaN/infinity/-0 spellings; clears exact if %g loses precision. */
static void
calc_append_number(GString *out, double v, gboolean *exact)
{
    if (isnan(v)) {
        g_string_append(out, "NaN");
    } else if (isinf(v)) {
        g_string_append(out, v < 0 ? "-infinity" : "infinity");
    } else if (v == 0) {
        g_string_append(out, signbit(v) ? "-0" : "0");
    } else {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buf, sizeof buf, "%g", v);
        double back = g_ascii_strtod(buf, NULL);
        if (fabs(back - v) > fabs(v) * 1e-9) *exact = FALSE;
        g_string_append(out, buf);
    }
}

/* Appends a numeric with unit, writing non-finite dimensions as "value * 1unit". */
static void
calc_append_numeric(GString *out, const calc_node *n, gboolean *exact)
{
    if (!isfinite(n->value) && *n->unit) {
        calc_append_number(out, n->value, exact);
        g_string_append(out, " * 1");
        g_string_append(out, n->unit);
        return;
    }
    calc_append_number(out, n->value, exact);
    g_string_append(out, n->unit);
}

static void calc_serialize(GString *out, calc_node *n, gboolean *exact);

/* Serializes a node and strips one pair of outer parentheses. */
static void
calc_serialize_unwrapped(GString *out, calc_node *n, gboolean *exact)
{
    GString *inner = g_string_new(NULL);
    calc_serialize(inner, n, exact);
    if (inner->len >= 2 && inner->str[0] == '(' &&
        inner->str[inner->len - 1] == ')')
        g_string_append_len(out, inner->str + 1, (gssize)inner->len - 2);
    else
        g_string_append(out, inner->str);
    g_string_free(inner, TRUE);
}

static gboolean
calc_is_negative(const calc_node *n)
{
    return n->kind == CALC_NODE_NUMERIC &&
           (n->value < 0 || (n->value == 0 && signbit(n->value)));
}

/* Serializes a simplified calc tree, sorting sum and product terms canonically. */
static void
calc_serialize(GString *out, calc_node *n, gboolean *exact)
{
    switch (n->kind) {
    case CALC_NODE_NUMERIC:
        calc_append_numeric(out, n, exact);
        return;
    case CALC_NODE_OPAQUE:
        g_string_append(out, n->text);
        return;
    case CALC_NODE_FUNCTION:
        g_string_append(out, n->text);
        g_string_append_c(out, '(');
        for (guint i = 0; i < n->children->len; i++) {
            if (i) g_string_append(out, ", ");
            calc_serialize_unwrapped(out, calc_child(n, i), exact);
        }
        g_string_append_c(out, ')');
        return;
    case CALC_NODE_NEGATE:
        g_string_append(out, "(-1 * ");
        calc_serialize(out, calc_child(n, 0), exact);
        g_string_append_c(out, ')');
        return;
    case CALC_NODE_INVERT:
        g_string_append(out, "(1 / ");
        calc_serialize(out, calc_child(n, 0), exact);
        g_string_append_c(out, ')');
        return;
    case CALC_NODE_SUM:
        calc_sort_children(n);
        g_string_append_c(out, '(');
        for (guint i = 0; i < n->children->len; i++) {
            calc_node *c = calc_child(n, i);
            if (i && c->kind == CALC_NODE_NEGATE) {
                g_string_append(out, " - ");
                calc_serialize(out, calc_child(c, 0), exact);
            } else if (i && calc_is_negative(c)) {
                g_string_append(out, " - ");
                c->value = -c->value;
                calc_append_numeric(out, c, exact);
                c->value = -c->value;
            } else {
                if (i) g_string_append(out, " + ");
                calc_serialize(out, c, exact);
            }
        }
        g_string_append_c(out, ')');
        return;
    case CALC_NODE_PRODUCT:
        calc_sort_children(n);
        g_string_append_c(out, '(');
        for (guint i = 0; i < n->children->len; i++) {
            calc_node *c = calc_child(n, i);
            if (i && c->kind == CALC_NODE_INVERT) {
                g_string_append(out, " / ");
                calc_serialize(out, calc_child(c, 0), exact);
            } else {
                if (i) g_string_append(out, " * ");
                calc_serialize(out, c, exact);
            }
        }
        g_string_append_c(out, ')');
        return;
    }
}

/* Parses, simplifies and serializes a math function; NULL if invalid or inexact. */
static char *
calc_function_canonical(const char *start, const char *end, gboolean keep_calc)
{
    calc_cursor c = { start, end, 0 };
    calc_node *root = calc_parse_value(&c);
    calc_skip_ws(&c);
    if (!root || c.p != end) {
        calc_node_free(root);
        return NULL;
    }
    gboolean ok = TRUE;
    root = calc_simplify(root, &ok);
    if (!ok) {
        calc_node_free(root);
        return NULL;
    }
    GString *out = g_string_new(NULL);
    gboolean exact = TRUE;
    gboolean comparison = root->kind == CALC_NODE_FUNCTION &&
        (strcmp(root->text, "min") == 0 || strcmp(root->text, "max") == 0 ||
         strcmp(root->text, "clamp") == 0);
    if (root->kind == CALC_NODE_FUNCTION && (!keep_calc || comparison)) {
        calc_serialize(out, root, &exact);
    } else {
        g_string_append(out, "calc(");
        calc_serialize_unwrapped(out, root, &exact);
        g_string_append_c(out, ')');
    }
    calc_node_free(root);
    if (!exact) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

/* Returns the canonical serialization of a calc()/min()/max()/clamp() value; caller frees. */
char *
ns_css_calc_canonical(const char *text)
{
    if (!text) return NULL;
    const char *p = text;
    while (ns_is_ascii_ws(*p)) p++;
    const char *end = p + strlen(p);
    while (end > p && ns_is_ascii_ws(end[-1])) end--;
    const char *name_end = p;
    while (name_end < end && calc_ident_char(*name_end)) name_end++;
    if (name_end == p || name_end >= end || *name_end != '(') return NULL;
    char *name = g_ascii_strdown(p, name_end - p);
    gboolean calc = strcmp(name, "calc") == 0;
    gboolean math = calc || strcmp(name, "min") == 0 ||
                    strcmp(name, "max") == 0 || strcmp(name, "clamp") == 0;
    g_free(name);
    if (!math || calc_matching_paren(name_end, end) != end - 1) return NULL;
    return calc_function_canonical(p, end, calc);
}
