/* Northstar Web Browser — DOM node tree: nodes, attributes, namespaces and id/class/tag indexes.
 * Also form-control state and validity, textContent, and HTML/XML serialization.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dom.h"

#include "css.h"
#include "datetime.h"

#include <errno.h>
#include <math.h>
#include <string.h>

static void ns_doc_id_index_register(ns_node *doc, const char *id, ns_node *node);
static void ns_doc_id_index_unregister(ns_node *doc, const char *id,
                                       const ns_node *node);

static void ns_class_set_clear(ns_node *el);
static void ns_doc_id_index_attr_write(ns_node *el, const char *name,
                                       const char *value);

static gboolean
ns_str_is_ascii_lower(const char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') return FALSE;
    return TRUE;
}

/* Parses a decimal int after leading blanks, clamped to [min_v,max_v]; dflt if no digits. */
int
ns_parse_int(const char *s, int dflt, int min_v, int max_v)
{
    if (!s || !*s) return dflt;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return dflt;
    errno = 0;
    char *end = NULL;
    gint64 v = g_ascii_strtoll(s, &end, 10);
    if (end == s) return dflt;
    if (errno == ERANGE) v = (v < 0) ? min_v : max_v;
    if (v < (gint64)min_v) v = min_v;
    if (v > (gint64)max_v) v = max_v;
    return (int)v;
}

typedef enum {
    NS_FORM_INPUT_OTHER,
    NS_FORM_INPUT_NUMBER,
    NS_FORM_INPUT_RANGE,
    NS_FORM_INPUT_DATE,
    NS_FORM_INPUT_MONTH,
    NS_FORM_INPUT_WEEK,
    NS_FORM_INPUT_TIME,
    NS_FORM_INPUT_DATETIME,
} ns_form_input_kind;

/* Maps an input type attribute to the numeric-value kind used by step/range logic. */
static ns_form_input_kind
ns_form_input_kind_of_type(const char *type)
{
    if (!type) return NS_FORM_INPUT_OTHER;
    if (!g_ascii_strcasecmp(type, "number"))         return NS_FORM_INPUT_NUMBER;
    if (!g_ascii_strcasecmp(type, "range"))          return NS_FORM_INPUT_RANGE;
    if (!g_ascii_strcasecmp(type, "date"))           return NS_FORM_INPUT_DATE;
    if (!g_ascii_strcasecmp(type, "month"))          return NS_FORM_INPUT_MONTH;
    if (!g_ascii_strcasecmp(type, "week"))           return NS_FORM_INPUT_WEEK;
    if (!g_ascii_strcasecmp(type, "time"))           return NS_FORM_INPUT_TIME;
    if (!g_ascii_strcasecmp(type, "datetime-local")) return NS_FORM_INPUT_DATETIME;
    return NS_FORM_INPUT_OTHER;
}

gboolean
ns_input_type_has_number_value(const char *type)
{
    return ns_form_input_kind_of_type(type) != NS_FORM_INPUT_OTHER;
}

/* True for input types where the readonly attribute applies (empty type counts as text). */
gboolean
ns_input_type_supports_readonly(const char *type)
{
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0 ||
           g_ascii_strcasecmp(type, "url") == 0 ||
           g_ascii_strcasecmp(type, "tel") == 0 ||
           g_ascii_strcasecmp(type, "email") == 0 ||
           g_ascii_strcasecmp(type, "password") == 0 ||
           g_ascii_strcasecmp(type, "date") == 0 ||
           g_ascii_strcasecmp(type, "month") == 0 ||
           g_ascii_strcasecmp(type, "week") == 0 ||
           g_ascii_strcasecmp(type, "time") == 0 ||
           g_ascii_strcasecmp(type, "datetime-local") == 0 ||
           g_ascii_strcasecmp(type, "number") == 0;
}

/* True for text-like input types where minlength/maxlength/pattern apply. */
gboolean
ns_input_type_supports_text_constraints(const char *type)
{
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0 ||
           g_ascii_strcasecmp(type, "url") == 0 ||
           g_ascii_strcasecmp(type, "tel") == 0 ||
           g_ascii_strcasecmp(type, "email") == 0 ||
           g_ascii_strcasecmp(type, "password") == 0;
}

/* True when a readonly textarea/input bars the control from constraint validation. */
gboolean
ns_form_control_readonly_bars_validation(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (!ns_element_get_attr(control, "readonly")) return FALSE;
    if (strcmp(control->name, "textarea") == 0) return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    return ns_input_type_supports_readonly(ns_element_get_attr(control, "type"));
}

gboolean
ns_form_control_length_limits_apply(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "textarea") == 0) return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    return ns_input_type_supports_text_constraints(ns_element_get_attr(control, "type"));
}

/* True when the required attribute applies: textarea, select, and most input types. */
gboolean
ns_form_control_supports_required(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "textarea") == 0 ||
        strcmp(control->name, "select") == 0)
        return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(control, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "hidden") != 0 &&
           g_ascii_strcasecmp(type, "range") != 0 &&
           g_ascii_strcasecmp(type, "color") != 0 &&
           g_ascii_strcasecmp(type, "submit") != 0 &&
           g_ascii_strcasecmp(type, "image") != 0 &&
           g_ascii_strcasecmp(type, "reset") != 0 &&
           g_ascii_strcasecmp(type, "button") != 0;
}

/* Checks the HTML "valid floating-point number" grammar (no leading +, no bare dot). */
static gboolean
ns_is_valid_floating_point_number(const char *v)
{
    const char *p = v;
    if (*p == '-') p++;
    gboolean digits = FALSE;
    while (g_ascii_isdigit((guchar)*p)) { p++; digits = TRUE; }
    if (*p == '.') {
        p++;
        if (!g_ascii_isdigit((guchar)*p)) return FALSE;
        while (g_ascii_isdigit((guchar)*p)) p++;
        digits = TRUE;
    }
    if (!digits) return FALSE;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '-' || *p == '+') p++;
        if (!g_ascii_isdigit((guchar)*p)) return FALSE;
        while (g_ascii_isdigit((guchar)*p)) p++;
    }
    return *p == '\0';
}

/* Parses an HTML floating-point number; FALSE if invalid or non-finite. */
static gboolean
ns_form_parse_finite_double(const char *v, double *out)
{
    if (!v || !ns_is_valid_floating_point_number(v)) return FALSE;
    double d = g_ascii_strtod(v, NULL);
    if (!isfinite(d)) return FALSE;
    if (out) *out = d;
    return TRUE;
}

/* Formats a double like JS Number.prototype.toString (shortest round-trip digits). */
void
ns_num_to_str(double d, char *buf, size_t n)
{
    if (d == 0 || !isfinite(d)) {
        g_ascii_formatd(buf, (gint)n, "%g", d == 0 ? 0 : d);
        return;
    }
    char sci[64];
    int prec = 1;
    for (; prec < 17; prec++) {
        char fmt[8];
        g_snprintf(fmt, sizeof fmt, "%%.%de", prec - 1);
        g_ascii_formatd(sci, sizeof sci, fmt, d);
        if (g_ascii_strtod(sci, NULL) == d) break;
    }
    if (prec == 17) g_ascii_formatd(sci, sizeof sci, "%.16e", d);
    char digits[32];
    int nd = 0;
    const char *p = sci;
    if (*p == '-') p++;
    for (; *p && *p != 'e'; p++)
        if (g_ascii_isdigit((guchar)*p)) digits[nd++] = *p;
    int point = atoi(p + 1) + 1;
    GString *out = g_string_new(d < 0 ? "-" : "");
    if (point >= nd && point <= 21) {
        g_string_append_len(out, digits, nd);
        for (int i = nd; i < point; i++) g_string_append_c(out, '0');
    } else if (point > 0 && point <= 21) {
        g_string_append_len(out, digits, point);
        g_string_append_c(out, '.');
        g_string_append_len(out, digits + point, nd - point);
    } else if (point > -6 && point <= 0) {
        g_string_append(out, "0.");
        for (int i = point; i < 0; i++) g_string_append_c(out, '0');
        g_string_append_len(out, digits, nd);
    } else {
        g_string_append_c(out, digits[0]);
        if (nd > 1) {
            g_string_append_c(out, '.');
            g_string_append_len(out, digits + 1, nd - 1);
        }
        g_string_append_printf(out, "e%s%d", point - 1 >= 0 ? "+" : "",
                               point - 1);
    }
    g_strlcpy(buf, out->str, n);
    g_string_free(out, TRUE);
}

/* Counts fractional digits in a numeric string, adjusted by its exponent; never negative. */
static int
ns_decimal_places(const char *v)
{
    if (!v) return 0;
    const char *dot = strchr(v, '.');
    int places = 0;
    for (const char *p = dot ? dot + 1 : v; dot && g_ascii_isdigit((guchar)*p); p++)
        places++;
    const char *e = strpbrk(v, "eE");
    if (e) places -= atoi(e + 1);
    return places < 0 ? 0 : places;
}

/* Rounds to the given decimal places to strip float noise; no-op outside 1..15. */
static double
ns_round_to_places(double v, int places)
{
    if (places <= 0 || places > 15) return v;
    double scale = pow(10.0, places);
    return round(v * scale) / scale;
}


/* Converts an input value to its numeric form per type (ms for dates/times, months, etc.). */
gboolean
ns_input_value_to_number(const char *type, const char *value, double *out)
{
    if (!value || !*value) return FALSE;
    ns_form_input_kind kind = ns_form_input_kind_of_type(type);
    int y, m, d, ms;
    switch (kind) {
    case NS_FORM_INPUT_NUMBER:
    case NS_FORM_INPUT_RANGE:
        return ns_form_parse_finite_double(value, out);
    case NS_FORM_INPUT_DATE: {
        const char *p = ns_dt_rd_date(value, &y, &m, &d);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ns_dt_days_from_civil(y, m, d) * 86400000.0;
        return TRUE;
    }
    case NS_FORM_INPUT_MONTH: {
        const char *p = ns_dt_rd_digits(value, 4, 9, &y);
        if (!p || *p != '-') return FALSE;
        p++;
        p = ns_dt_rd_digits(p, 2, 2, &m);
        if (!p || *p != '\0' || y < 1 || y > NS_DT_MAX_YEAR ||
            m < 1 || m > 12) return FALSE;
        if (out) *out = (double)((y - 1970) * 12 + (m - 1));
        return TRUE;
    }
    case NS_FORM_INPUT_WEEK: {
        const char *p = ns_dt_rd_digits(value, 4, 9, &y);
        if (!p || *p != '-' || *(p + 1) != 'W') return FALSE;
        p += 2;
        int w;
        p = ns_dt_rd_digits(p, 2, 2, &w);
        if (!p || *p != '\0' || y < 1 || y > NS_DT_MAX_YEAR || w < 1 ||
            w > ns_dt_iso_weeks_in_year(y))
            return FALSE;
        long monday = ns_dt_iso_week1_monday(y) + (long)(w - 1) * 7;
        if (out) *out = (double)monday * 86400000.0;
        return TRUE;
    }
    case NS_FORM_INPUT_TIME: {
        const char *p = ns_dt_rd_time(value, &ms);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ms;
        return TRUE;
    }
    case NS_FORM_INPUT_DATETIME: {
        const char *p = ns_dt_rd_date(value, &y, &m, &d);
        if (!p || (*p != 'T' && *p != ' ')) return FALSE;
        p++;
        p = ns_dt_rd_time(p, &ms);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ns_dt_days_from_civil(y, m, d) * 86400000.0 + ms;
        return TRUE;
    }
    default:
        return FALSE;
    }
}

/* Reports rangeUnderflow/rangeOverflow against min/max; FALSE if value is not numeric. */
gboolean
ns_input_value_range_state(const ns_node *input, const char *value,
                           gboolean *underflow, gboolean *overflow)
{
    if (underflow) *underflow = FALSE;
    if (overflow) *overflow = FALSE;
    if (!input || !ns_node_is_element_named(input, "input")) return FALSE;
    const char *type = ns_element_get_attr(input, "type");
    double v;
    if (!ns_input_value_to_number(type, value, &v)) return FALSE;
    double bound;
    const char *min = ns_element_get_attr(input, "min");
    const char *max = ns_element_get_attr(input, "max");
    if (ns_input_value_to_number(type, min, &bound) && v < bound) {
        if (underflow) *underflow = TRUE;
    }
    if (ns_input_value_to_number(type, max, &bound) && v > bound) {
        if (overflow) *overflow = TRUE;
    }
    return TRUE;
}

/* Step scale factor per input kind (ms per day/week/second), per HTML step semantics. */
static double
ns_form_step_scale(ns_form_input_kind kind)
{
    switch (kind) {
    case NS_FORM_INPUT_DATE:     return 86400000.0;
    case NS_FORM_INPUT_WEEK:     return 604800000.0;
    case NS_FORM_INPUT_TIME:
    case NS_FORM_INPUT_DATETIME: return 1000.0;
    default:                     return 1.0;
    }
}

/* Default step per input kind: 60 seconds for time inputs, otherwise 1. */
static double
ns_form_default_step(ns_form_input_kind kind)
{
    switch (kind) {
    case NS_FORM_INPUT_TIME:
    case NS_FORM_INPUT_DATETIME:
        return 60.0;
    default:
        return 1.0;
    }
}

/* Appends ms-since-midnight as HH:MM, adding :SS and .mmm only when nonzero. */
static void
ns_form_serialize_time(GString *out, int ms)
{
    int h = ms / 3600000;
    int mi = (ms / 60000) % 60;
    int s = (ms / 1000) % 60;
    int frac = ms % 1000;
    g_string_append_printf(out, "%02d:%02d", h, mi);
    if (s || frac) g_string_append_printf(out, ":%02d", s);
    if (frac) g_string_append_printf(out, ".%03d", frac);
}

/* Serializes a number back to the input type's string form in buf; FALSE if invalid. */
static gboolean
ns_input_number_to_value(ns_form_input_kind kind, double v, char *buf,
                         gsize buflen)
{
    if (!isfinite(v)) return FALSE;
    GString *out = g_string_new(NULL);
    gboolean ok = TRUE;
    int y, m, d;
    switch (kind) {
    case NS_FORM_INPUT_NUMBER:
    case NS_FORM_INPUT_RANGE: {
        char num[G_ASCII_DTOSTR_BUF_SIZE];
        ns_num_to_str(v == 0 ? 0 : v, num, sizeof num);
        g_string_append(out, num);
        break;
    }
    case NS_FORM_INPUT_DATE: {
        double days = floor(v / 86400000.0 + 0.5);
        ns_dt_civil_from_days((long)days, &y, &m, &d);
        ok = y >= 1 && y <= NS_DT_MAX_YEAR;
        if (ok) g_string_append_printf(out, "%04d-%02d-%02d", y, m, d);
        break;
    }
    case NS_FORM_INPUT_MONTH: {
        long months = (long)floor(v + 0.5);
        y = 1970 + (int)(months >= 0 ? months / 12
                                     : -((-months + 11) / 12));
        m = (int)ns_dt_floormod(months, 12) + 1;
        ok = y >= 1 && y <= NS_DT_MAX_YEAR;
        if (ok) g_string_append_printf(out, "%04d-%02d", y, m);
        break;
    }
    case NS_FORM_INPUT_WEEK: {
        long monday = (long)floor(v / 604800000.0 + 0.5) * 7 - 3;
        ns_dt_civil_from_days(monday + 3, &y, &m, &d);
        long week1 = ns_dt_iso_week1_monday(y);
        int w = (int)((monday - week1) / 7) + 1;
        ok = y >= 1 && y <= NS_DT_MAX_YEAR &&
             w >= 1 && w <= ns_dt_iso_weeks_in_year(y);
        if (ok) g_string_append_printf(out, "%04d-W%02d", y, w);
        break;
    }
    case NS_FORM_INPUT_TIME: {
        long ms = (long)floor(v + 0.5);
        ms = ns_dt_floormod(ms, 86400000L);
        ns_form_serialize_time(out, (int)ms);
        break;
    }
    case NS_FORM_INPUT_DATETIME: {
        double total = floor(v + 0.5);
        long days = (long)floor(total / 86400000.0);
        long ms = (long)(total - (double)days * 86400000.0);
        ns_dt_civil_from_days(days, &y, &m, &d);
        ok = y >= 1 && y <= NS_DT_MAX_YEAR;
        if (ok) {
            g_string_append_printf(out, "%04d-%02d-%02dT", y, m, d);
            ns_form_serialize_time(out, (int)ms);
        }
        break;
    }
    default:
        ok = FALSE;
        break;
    }
    ok = ok && out->len < buflen;
    if (ok) memcpy(buf, out->str, out->len + 1);
    g_string_free(out, TRUE);
    return ok;
}

/* stepUp/stepDown: snaps to the step base, clamps to min/max, writes new value to buf. */
int
ns_input_step_apply(const ns_node *input, int sign, double n, char *buf,
                    gsize buflen)
{
    if (!input || !ns_node_is_element_named(input, "input"))
        return NS_STEP_NOT_APPLICABLE;
    const char *type = ns_element_get_attr(input, "type");
    ns_form_input_kind kind = ns_form_input_kind_of_type(type);
    if (kind == NS_FORM_INPUT_OTHER)
        return NS_STEP_NOT_APPLICABLE;
    const char *step_attr = ns_element_get_attr(input, "step");
    if (step_attr && g_ascii_strcasecmp(step_attr, "any") == 0)
        return NS_STEP_NO_STEP;
    double step_value = ns_form_default_step(kind);
    double parsed;
    if (ns_form_parse_finite_double(step_attr, &parsed) && parsed > 0)
        step_value = parsed;
    double step = step_value * ns_form_step_scale(kind);
    if (step <= 0 || !isfinite(step))
        return NS_STEP_NO_STEP;
    double minv, maxv;
    gboolean has_min = ns_input_value_to_number(
        type, ns_element_get_attr(input, "min"), &minv);
    gboolean has_max = ns_input_value_to_number(
        type, ns_element_get_attr(input, "max"), &maxv);
    if (has_min && has_max && minv > maxv)
        return NS_STEP_UNCHANGED;
    double value = 0;
    if (!ns_input_value_to_number(type, ns_input_used_value(input), &value))
        value = 0;
    double before = value;
    double base = kind == NS_FORM_INPUT_WEEK ? -259200000.0 : 0.0;
    if (ns_input_value_to_number(type, ns_element_get_attr(input, "min"),
                                 &parsed))
        base = parsed;
    double q = (value - base) / step;
    double nearest = round(q);
    double tolerance = 1e-7 * (fabs(q) > 1.0 ? fabs(q) : 1.0);
    if (fabs(q - nearest) > tolerance)
        value = base + (sign > 0 ? ceil(q) : floor(q)) * step;
    else
        value += (double)sign * n * step;
    double bound;
    if (ns_input_value_to_number(type, ns_element_get_attr(input, "min"),
                                 &bound) && value < bound)
        value = base + ceil((bound - base) / step) * step;
    if (ns_input_value_to_number(type, ns_element_get_attr(input, "max"),
                                 &bound) && value > bound)
        value = base + floor((bound - base) / step) * step;
    if ((sign > 0 && value < before) || (sign < 0 && value > before))
        return NS_STEP_UNCHANGED;
    if (kind == NS_FORM_INPUT_NUMBER || kind == NS_FORM_INPUT_RANGE) {
        int places = ns_decimal_places(step_attr);
        int value_places = ns_decimal_places(ns_input_used_value(input));
        int min_places = ns_decimal_places(ns_element_get_attr(input, "min"));
        if (value_places > places) places = value_places;
        if (min_places > places) places = min_places;
        value = ns_round_to_places(value, places);
    }
    if (!ns_input_number_to_value(kind, value, buf, buflen))
        return NS_STEP_UNCHANGED;
    return NS_STEP_OK;
}

/* True when the value is not an integral number of steps from the step base (stepMismatch). */
gboolean
ns_input_value_step_mismatch(const ns_node *input, const char *value)
{
    if (!input || !ns_node_is_element_named(input, "input")) return FALSE;
    const char *type = ns_element_get_attr(input, "type");
    ns_form_input_kind kind = ns_form_input_kind_of_type(type);
    if (kind == NS_FORM_INPUT_OTHER) return FALSE;
    const char *step_attr = ns_element_get_attr(input, "step");
    if (step_attr && g_ascii_strcasecmp(step_attr, "any") == 0)
        return FALSE;
    double v;
    if (!ns_input_value_to_number(type, value, &v)) return FALSE;
    double step_value = ns_form_default_step(kind);
    double parsed;
    if (ns_form_parse_finite_double(step_attr, &parsed) && parsed > 0)
        step_value = parsed;
    double step = step_value * ns_form_step_scale(kind);
    if (step <= 0 || !isfinite(step)) return FALSE;
    double base = kind == NS_FORM_INPUT_WEEK ? -259200000.0 : 0.0;
    if (ns_input_value_to_number(type, ns_element_get_attr(input, "min"), &parsed))
        base = parsed;
    double q = (v - base) / step;
    double nearest = round(q);
    double scale = fabs(q) > 1.0 ? fabs(q) : 1.0;
    return fabs(q - nearest) > 1e-7 * scale;
}

static gboolean
ns_input_type_is(const ns_node *node, const char *want)
{
    if (!ns_node_is_element_named(node, "input")) return FALSE;
    const char *type = ns_element_get_attr(node, "type");
    return type && g_ascii_strcasecmp(type, want) == 0;
}

/* Recursively searches for a checked radio with the same name and form owner. */
static gboolean
ns_radio_group_has_checked(const ns_node *scan, const ns_node *doc,
                           const ns_node *owner,
                           const char *name, int depth)
{
    if (!scan || depth >= 512) return FALSE;
    if (ns_input_type_is(scan, "radio")) {
        const char *scan_name = ns_element_get_attr(scan, "name");
        if (!scan_name) scan_name = "";
        if (strcmp(scan_name, name) == 0 &&
            ns_form_owner(scan, doc) == owner &&
            ns_input_is_checked(scan))
            return TRUE;
    }
    for (const ns_node *c = scan->first_child; c; c = c->next_sibling)
        if (ns_radio_group_has_checked(c, doc, owner, name, depth + 1))
            return TRUE;
    return FALSE;
}

/* Computes valueMissing: unchecked checkbox, radio group with none checked, or empty value. */
gboolean
ns_form_control_value_missing(const ns_node *control, const char *value,
                              const ns_node *doc)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (ns_input_type_is(control, "checkbox"))
        return !ns_input_is_checked(control);
    if (ns_input_type_is(control, "radio")) {
        const char *name = ns_element_get_attr(control, "name");
        if (!name) name = "";
        const ns_node *root = doc ? doc : ns_node_root(control);
        const ns_node *owner = ns_form_owner(control, root);
        return !ns_radio_group_has_checked(root ? root : control,
                                           root, owner, name, 0);
    }
    return !value || !*value;
}

static gboolean
ns_email_local_char(guchar c)
{
    return g_ascii_isalnum(c) || c == '.' || c == '!' || c == '#' ||
           c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
           c == '+' || c == '/' || c == '=' || c == '?' || c == '^' ||
           c == '_' || c == '`' || c == '{' || c == '|' || c == '}' ||
           c == '~' || c == '-';
}

/* Validates a dotted domain: labels 1-63 chars of alnum/hyphen, alnum at both ends. */
static gboolean
ns_email_domain_valid(const char *domain)
{
    const char *label = domain;
    while (label && *label) {
        const char *dot = strchr(label, '.');
        gsize len = dot ? (gsize)(dot - label) : strlen(label);
        if (len == 0 || len > 63 || !g_ascii_isalnum((guchar)label[0]) ||
            !g_ascii_isalnum((guchar)label[len - 1]))
            return FALSE;
        for (gsize i = 1; i + 1 < len; i++)
            if (!g_ascii_isalnum((guchar)label[i]) && label[i] != '-')
                return FALSE;
        if (!dot) return TRUE;
        label = dot + 1;
    }
    return FALSE;
}

/* Validates one trimmed email address token: local@domain with a single @. */
static gboolean
ns_email_token_valid(const char *start, gsize len)
{
    char *token = g_strndup(start, len);
    char *trimmed = g_strstrip(token);
    const char *at = strchr(trimmed, '@');
    gboolean ok = at && at != trimmed && at[1] && !strchr(at + 1, '@');
    for (const char *p = trimmed; ok && p < at; p++)
        if (!ns_email_local_char((guchar)*p)) ok = FALSE;
    if (ok) ok = ns_email_domain_valid(at + 1);
    g_free(token);
    return ok;
}

/* Validates an email input value; comma-separated list when the multiple attribute is set. */
gboolean
ns_input_email_value_valid(const ns_node *input, const char *value)
{
    if (!value || !*value) return TRUE;
    if (!input || !ns_element_get_attr(input, "multiple"))
        return ns_email_token_valid(value, strlen(value));
    const char *p = value;
    while (TRUE) {
        const char *comma = strchr(p, ',');
        gsize len = comma ? (gsize)(comma - p) : strlen(p);
        if (!ns_email_token_valid(p, len)) return FALSE;
        if (!comma) return TRUE;
        p = comma + 1;
    }
}

/* True when a contenteditable attribute value makes the element editable. */
gboolean
ns_ce_attr_enables(const char *ce)
{
    return ce && (!*ce ||
                  g_ascii_strcasecmp(ce, "true") == 0 ||
                  g_ascii_strcasecmp(ce, "plaintext-only") == 0);
}

/* True for textarea and text-entry input types (text, search, email, url, tel, etc.). */
gboolean
ns_node_is_text_input(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "textarea") == 0) return TRUE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text")     == 0 ||
           g_ascii_strcasecmp(type, "search")   == 0 ||
           g_ascii_strcasecmp(type, "email")    == 0 ||
           g_ascii_strcasecmp(type, "url")      == 0 ||
           g_ascii_strcasecmp(type, "tel")      == 0 ||
           g_ascii_strcasecmp(type, "number")   == 0 ||
           g_ascii_strcasecmp(type, "password") == 0;
}

/* True for non-form elements whose own contenteditable attribute enables editing. */
gboolean
ns_node_is_contenteditable_host(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "input") == 0 || strcmp(n->name, "textarea") == 0)
        return FALSE;
    return ns_ce_attr_enables(ns_element_get_attr(n, "contenteditable"));
}

gboolean
ns_node_is_editable(const ns_node *n)
{
    return ns_node_is_text_input(n) || ns_node_is_contenteditable_host(n);
}

/* Resolves inherited spellcheck state from the nearest ancestor attribute; defaults to TRUE. */
gboolean
ns_node_spellcheck_used(const ns_node *n)
{
    for (const ns_node *cur = n; cur; cur = cur->parent) {
        if (cur->kind != NS_NODE_ELEMENT) continue;
        const char *v = ns_element_get_attr(cur, "spellcheck");
        if (!v) continue;
        if (!g_ascii_strcasecmp(v, "false")) return FALSE;
        if (!*v || !g_ascii_strcasecmp(v, "true")) return TRUE;
    }
    return TRUE;
}

/* Returns the nearest editable ancestor if spellchecking is enabled for it, else NULL. */
const ns_node *
ns_node_spellcheck_host(const ns_node *n)
{
    for (const ns_node *cur = n; cur; cur = cur->parent)
        if (ns_node_is_editable(cur))
            return ns_node_spellcheck_used(cur) ? cur : NULL;
    return NULL;
}

/* True for input types using the "value" value mode, where edits go to data-nd-value. */
gboolean
ns_input_value_is_dirty_mode(const ns_node *n)
{
    if (!n || !n->name || strcmp(n->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    if (!type) return TRUE;
    return !(g_ascii_strcasecmp(type, "checkbox") == 0 ||
             g_ascii_strcasecmp(type, "radio")    == 0 ||
             g_ascii_strcasecmp(type, "submit")   == 0 ||
             g_ascii_strcasecmp(type, "reset")    == 0 ||
             g_ascii_strcasecmp(type, "button")   == 0 ||
             g_ascii_strcasecmp(type, "image")    == 0 ||
             g_ascii_strcasecmp(type, "file")     == 0 ||
             g_ascii_strcasecmp(type, "hidden")   == 0);
}

/* Current value of an input/textarea: dirty edited value if set, else the default value. */
const char *
ns_input_used_value(const ns_node *n)
{
    if (!n) return NULL;
    if (n->name && strcmp(n->name, "textarea") == 0) {
        const char *dirty = ns_element_get_attr(n, "data-nd-vdirty");
        if (dirty) {
            const char *value = ns_element_get_attr(n, "data-nd-value");
            return value ? value : "";
        }
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_TEXT && c->text)
                return c->text;
        return "";
    }
    if (ns_input_value_is_dirty_mode(n)) {
        const char *dirty = ns_element_get_attr(n, "data-nd-value");
        if (dirty) return dirty;
    }
    return ns_element_get_attr(n, "value");
}

/* Concatenates a textarea's text children (its default value); caller frees. */
char *
ns_textarea_default_value_dup(const ns_node *n)
{
    GString *value = g_string_new(NULL);
    if (n && n->name && strcmp(n->name, "textarea") == 0) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_TEXT && c->text)
                g_string_append(value, c->text);
    }
    return g_string_free(value, FALSE);
}

/* Normalizes CR and CRLF newlines to LF; caller frees. */
static char *
ns_textarea_normalized_value_dup(const char *value)
{
    GString *normalized = g_string_new(NULL);
    for (const char *p = value ? value : ""; *p; p++) {
        if (*p == '\r') {
            if (p[1] == '\n') p++;
            g_string_append_c(normalized, '\n');
        } else {
            g_string_append_c(normalized, *p);
        }
    }
    return g_string_free(normalized, FALSE);
}

/* Returns the textarea's current value with newlines normalized; caller frees. */
char *
ns_textarea_value_dup(const ns_node *n)
{
    const char *dirty = n ? ns_element_get_attr(n, "data-nd-vdirty") : NULL;
    if (dirty) {
        const char *value = ns_element_get_attr(n, "data-nd-value");
        return ns_textarea_normalized_value_dup(value ? value : "");
    }
    char *default_value = ns_textarea_default_value_dup(n);
    char *value = ns_textarea_normalized_value_dup(default_value);
    g_free(default_value);
    return value;
}

/* Checkedness: the dirty data-nd-checked state if present, else the checked attribute. */
gboolean
ns_input_is_checked(const ns_node *n)
{
    if (!n) return FALSE;
    const char *dirty = ns_element_get_attr(n, "data-nd-checked");
    if (dirty) return strcmp(dirty, "1") == 0;
    return ns_element_get_attr(n, "checked") != NULL;
}

/* Current text of a textarea, contenteditable host (first text child) or input; never NULL. */
const char *
ns_node_editable_value(const ns_node *n)
{
    if (!n) return "";
    if (n->name && strcmp(n->name, "textarea") == 0)
        return ns_input_used_value(n);
    if (ns_node_is_contenteditable_host(n)) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_TEXT && c->text)
                return c->text;
        return "";
    }
    const char *v = ns_input_used_value(n);
    return v ? v : "";
}

/* Stores a user-edited value; a contenteditable host gets one text child instead. */
void
ns_node_set_editable_value(ns_node *n, const char *value)
{
    if (!n) return;
    if (n->name && strcmp(n->name, "textarea") == 0) {
        char *normalized = ns_textarea_normalized_value_dup(value);
        ns_element_set_attr(n, "data-nd-value", normalized);
        ns_element_set_attr(n, "data-nd-vdirty", "1");
        ns_element_set_attr(n, "data-nd-user-edited", "1");
        g_free(normalized);
    } else if (ns_node_is_contenteditable_host(n)) {
        ns_node *doc = (ns_node *)ns_node_root(n);
        for (ns_node *c = n->first_child; c; ) {
            ns_node *next = c->next_sibling;
            ns_node_remove(c);
            if (doc && doc != c) {
                ns_doc_id_index_subtree_removed(doc, c);
                ns_doc_class_index_subtree_removed(doc, c);
                ns_doc_tag_index_subtree_removed(doc, c);
            }
            ns_node_free(c);
            c = next;
        }
        ns_node_append_child(n, ns_node_new_text(g_strdup(value ? value : "")));
    } else if (ns_input_value_is_dirty_mode(n)) {
        ns_element_set_attr(n, "data-nd-value", value ? value : "");
        ns_element_set_attr(n, "data-nd-user-edited", "1");
    } else {
        ns_element_set_attr(n, "value", value ? value : "");
    }
}

/* Collapses a contenteditable host's subtree into a single text node of its text. */
void
ns_node_flatten_editable(ns_node *n)
{
    if (!ns_node_is_contenteditable_host(n)) return;
    char *txt = ns_node_collect_text(n);
    ns_node_set_editable_value(n, txt ? txt : "");
    g_free(txt);
}

gboolean
ns_node_is_numeric_input(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(control, "type");
    return type && g_ascii_strcasecmp(type, "number") == 0;
}

/* Filters inserted text to characters valid in a number input; caller frees. */
char *
ns_numeric_filter_insert(const char *insert, gsize len, gsize *out_len)
{
    GString *s = g_string_sized_new(len);
    for (gsize i = 0; insert && i < len; i++) {
        char c = insert[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
            c == 'e' || c == 'E')
            g_string_append_c(s, c);
    }
    if (out_len) *out_len = s->len;
    return g_string_free(s, FALSE);
}


static ns_node *
ns_node_new(ns_node_kind kind)
{
    ns_node *n = g_new0(ns_node, 1);
    n->kind = kind;
    return n;
}

ns_node *
ns_node_new_document(void)
{
    return ns_node_new(NS_NODE_DOCUMENT);
}

/* Creates a doctype node; takes ownership of the name and id strings. */
ns_node *
ns_node_new_doctype(char *name, char *public_id, char *system_id)
{
    ns_node *n = ns_node_new(NS_NODE_DOCTYPE);
    n->name = name;
    n->public_id = public_id;
    n->system_id = system_id;
    n->flags |= NS_NODE_OWN_NAME;
    return n;
}

/* Creates an element node that takes ownership of name. */
ns_node *
ns_node_new_element(char *name)
{
    ns_node *n = ns_node_new(NS_NODE_ELEMENT);
    n->name = name;
    n->flags |= NS_NODE_OWN_NAME;
    return n;
}

/* Creates a text node that takes ownership of text. */
ns_node *
ns_node_new_text_len(char *text, guint32 len)
{
    ns_node *n = ns_node_new(NS_NODE_TEXT);
    n->text = text;
    n->text_len = len;
    n->flags |= NS_NODE_OWN_TEXT;
    return n;
}

ns_node *
ns_node_new_text(char *text)
{
    return ns_node_new_text_len(text, text ? (guint32)strlen(text) : 0);
}

/* Creates a comment node that takes ownership of text. */
ns_node *
ns_node_new_comment_len(char *text, guint32 len)
{
    ns_node *n = ns_node_new(NS_NODE_COMMENT);
    n->text = text;
    n->text_len = len;
    n->flags |= NS_NODE_OWN_TEXT;
    return n;
}

ns_node *
ns_node_new_comment(char *text)
{
    return ns_node_new_comment_len(text, text ? (guint32)strlen(text) : 0);
}

/* Points the node name at a borrowed string, freeing any previously owned name. */
void
ns_node_set_name_borrow(ns_node *n, const char *name)
{
    if (!n) return;
    if (n->flags & NS_NODE_OWN_NAME)
        g_free(n->name);
    n->name = (char *)name;
    n->flags &= ~NS_NODE_OWN_NAME;
}

/* Replaces the node name with an owned string; frees name if node is NULL. */
void
ns_node_set_name_owned(ns_node *n, char *name)
{
    if (!n) {
        g_free(name);
        return;
    }
    if (n->flags & NS_NODE_OWN_NAME)
        g_free(n->name);
    n->name = name;
    n->flags |= NS_NODE_OWN_NAME;
}

/* Points the node text at a borrowed string, freeing any previously owned text. */
void
ns_node_set_text_borrow(ns_node *n, const char *text)
{
    if (!n) return;
    if (n->flags & NS_NODE_OWN_TEXT)
        g_free(n->text);
    n->text = (char *)text;
    n->text_len = text ? (guint32)strlen(text) : 0;
    n->flags &= ~NS_NODE_OWN_TEXT;
}

/* Replaces node text with an owned buffer; frees text if node is NULL. */
void
ns_node_replace_text_len_owned(ns_node *n, char *text, guint32 len)
{
    if (!n) {
        g_free(text);
        return;
    }
    if (n->flags & NS_NODE_OWN_TEXT)
        g_free(n->text);
    n->text = text;
    n->text_len = len;
    n->flags |= NS_NODE_OWN_TEXT;
}

void
ns_node_replace_text_owned(ns_node *n, char *text)
{
    ns_node_replace_text_len_owned(n, text, text ? (guint32)strlen(text) : 0);
}

/* Duplicates any borrowed name/text/attribute strings so the node owns them. */
static void
ns_node_own_strings_one(ns_node *n)
{
    ns_class_set_clear(n);
    if (n->name && !(n->flags & NS_NODE_OWN_NAME)) {
        n->name = g_strdup(n->name);
        n->flags |= NS_NODE_OWN_NAME;
    }
    if (n->text && !(n->flags & NS_NODE_OWN_TEXT)) {
        n->text = g_strdup(n->text);
        n->flags |= NS_NODE_OWN_TEXT;
    }
    for (ns_attr *a = n->attrs; a; a = a->next) {
        if (a->name && !(a->flags & NS_ATTR_OWN_NAME)) {
            a->name = g_strdup(a->name);
            a->flags |= NS_ATTR_OWN_NAME;
        }
        if (a->value && !(a->flags & NS_ATTR_OWN_VALUE)) {
            a->value = ns_value_dup_len(a->value, a->value_len);
            a->flags |= NS_ATTR_OWN_VALUE;
        }
    }
}

/* Makes a subtree (including template contents) own all of its strings, iteratively. */
void
ns_node_own_strings_deep(ns_node *n)
{
    if (!n) return;
    ns_node *cur = n;
    for (;;) {
        ns_node_own_strings_one(cur);
        if (cur->tpl_content) ns_node_own_strings_deep(cur->tpl_content);
        if (cur->first_child) {
            cur = cur->first_child;
            continue;
        }
        while (cur != n && !cur->next_sibling) cur = cur->parent;
        if (cur == n) break;
        cur = cur->next_sibling;
    }
}

/* Appends an attribute with borrowed strings, invalidating class set, bloom and id index. */
void
ns_element_append_attr_borrow(ns_node *el, const char *name, const char *value)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !name) return;
    if (el->class_set) ns_class_set_clear(el);
    ns_doc_id_index_attr_write(el, name, value);
    el->attr_bloom = 0;
    el->attr_gen++;
    ns_attr *a = g_new0(ns_attr, 1);
    a->name  = (char *)name;
    a->value = (char *)(value ? value : "");
    a->value_len = value ? (guint)strlen(value) : 0;
    a->flags = ns_str_is_ascii_lower(name) ? NS_ATTR_NAME_LOWER : 0;
    ns_attr *tail = NULL;
    for (ns_attr *cur = el->attrs; cur; cur = cur->next) tail = cur;
    if (tail) tail->next = a;
    else      el->attrs = a;
}

/* Attaches a backing buffer to a root node, destroying any previous one. */
void
ns_node_attach_backing(ns_node *root, void *backing, void (*destroy)(void *))
{
    if (!root) {
        if (backing && destroy) destroy(backing);
        return;
    }
    if (root->backing && root->backing_free)
        root->backing_free(root->backing);
    root->backing = backing;
    root->backing_free = destroy;
}

static void
ns_attr_free_one(ns_attr *a)
{
    if (!a) return;
    if (a->flags & NS_ATTR_OWN_NAME)  g_free(a->name);
    if (a->flags & NS_ATTR_OWN_VALUE) g_free(a->value);
    g_free(a->namespace_uri);
    g_free(a->prefix);
    g_free(a->local_name);
    g_free(a);
}

static void
ns_attr_free(ns_attr *a)
{
    while (a) {
        ns_attr *next = a->next;
        ns_attr_free_one(a);
        a = next;
    }
}

typedef struct ns_class_set {
    guint n;
    ns_class_token tok[];
} ns_class_set;

static ns_class_set g_nd_empty_class_set;

/* Tokenizes a class attribute on ASCII whitespace into a set of borrowed slices. */
static ns_class_set *
ns_class_set_build(const char *cls)
{
    guint n = 0;
    for (const char *s = cls; *s; ) {
        while (*s && ns_is_ascii_ws(*s)) s++;
        if (!*s) break;
        while (*s && !ns_is_ascii_ws(*s)) s++;
        n++;
    }
    if (n == 0) return &g_nd_empty_class_set;
    ns_class_set *cs = g_malloc(sizeof *cs + (gsize)n * sizeof cs->tok[0]);
    cs->n = 0;
    for (const char *s = cls; *s; ) {
        while (*s && ns_is_ascii_ws(*s)) s++;
        if (!*s) break;
        const char *t = s;
        while (*s && !ns_is_ascii_ws(*s)) s++;
        cs->tok[cs->n].p = t;
        cs->tok[cs->n].len = (guint)(s - t);
        cs->n++;
    }
    return cs;
}

static void
ns_class_set_clear(ns_node *el)
{
    if (el->class_set && el->class_set != &g_nd_empty_class_set)
        g_free(el->class_set);
    el->class_set = NULL;
}

/* Lazily builds and caches the element's class token set. */
static ns_class_set *
ns_class_set_ensure(const ns_node *el)
{
    ns_class_set *cs = el->class_set;
    if (!cs) {
        const char *cls = ns_element_get_attr(el, "class");
        cs = (cls && *cls) ? ns_class_set_build(cls) : &g_nd_empty_class_set;
        ((ns_node *)el)->class_set = cs;
    }
    return cs;
}

/* Returns the element's cached class tokens and count; NULL for non-elements. */
const ns_class_token *
ns_node_class_tokens(const ns_node *el, guint *n_out)
{
    if (!el || el->kind != NS_NODE_ELEMENT) {
        if (n_out) *n_out = 0;
        return NULL;
    }
    ns_class_set *cs = ns_class_set_ensure(el);
    if (n_out) *n_out = cs->n;
    return cs->tok;
}

/* True if the element's class list contains the given token (case-sensitive). */
gboolean
ns_node_has_class(const ns_node *el, const char *name, gsize len)
{
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    ns_class_set *cs = ns_class_set_ensure(el);
    for (guint i = 0; i < cs->n; i++)
        if (cs->tok[i].len == len && memcmp(cs->tok[i].p, name, len) == 0)
            return TRUE;
    return FALSE;
}

void (*ns_node_free_hook)(ns_node *node);
void (*ns_node_detach_hook)(ns_node *parent);

/* Frees a subtree iteratively (no recursion), running free/JS hooks and dropping indexes. */
void
ns_node_free(ns_node *node)
{
    if (!node)
        return;

    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, node);

    while (stack->len > 0) {
        ns_node *cur = g_ptr_array_index(stack, stack->len - 1);
        if (cur->tpl_content) {
            ns_node *tc = cur->tpl_content;
            cur->tpl_content = NULL;
            g_ptr_array_add(stack, tc);
            continue;
        }
        if (cur->first_child) {
            ns_node *c = cur->first_child;
            cur->first_child = NULL;
            while (c) {
                ns_node *next = c->next_sibling;
                c->next_sibling = NULL;
                c->parent = NULL;
                g_ptr_array_add(stack, c);
                c = next;
            }
            continue;
        }
        g_ptr_array_set_size(stack, stack->len - 1);
        if (ns_node_free_hook)
            ns_node_free_hook(cur);
        if (cur->js_invalidate)
            cur->js_invalidate(cur);
        ns_css_forget_node(cur);
        if (cur->flags & NS_NODE_OWN_NAME) g_free(cur->name);
        if (cur->flags & NS_NODE_OWN_TEXT) g_free(cur->text);
        g_free(cur->public_id);
        g_free(cur->system_id);
        ns_class_set_clear(cur);
        ns_attr_free(cur->attrs);
        if (cur->backing && cur->backing_free)
            cur->backing_free(cur->backing);
        if (cur->id_index) {
            g_hash_table_destroy(cur->id_index);
            cur->id_index = NULL;
        }
        if (cur->id_counts) {
            g_hash_table_destroy(cur->id_counts);
            cur->id_counts = NULL;
        }
        if (cur->class_index) {
            g_hash_table_destroy(cur->class_index);
            cur->class_index = NULL;
        }
        if (cur->tag_index) {
            g_hash_table_destroy(cur->tag_index);
            cur->tag_index = NULL;
        }
        g_free(cur);
    }
    g_ptr_array_free(stack, TRUE);
}

/* Unlinks a node from its parent and siblings, notifying the detach hook first. */
static void
ns_node_detach(ns_node *child)
{
    ns_node *p = child->parent;
    if (!p)
        return;
    if (ns_node_detach_hook)
        ns_node_detach_hook(p);
    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        p->first_child = child->next_sibling;
    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;
    else
        p->last_child = child->prev_sibling;
    child->parent = NULL;
    child->prev_sibling = NULL;
    child->next_sibling = NULL;
}

/* Appends child as parent's last child, detaching it from any previous parent. */
void
ns_node_append_child(ns_node *parent, ns_node *child)
{
    g_return_if_fail(parent != NULL);
    g_return_if_fail(child != NULL);

    ns_node_detach(child);
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

void
ns_node_remove(ns_node *child)
{
    ns_node_detach(child);
}

/* True for engine-private data-nd-* attributes hidden from page script. */
gboolean
ns_attr_name_is_internal(const char *name)
{
    return name && g_ascii_strncasecmp(name, "data-nd-", 8) == 0;
}

/* Attribute local name, falling back to the qualified name; never NULL. */
const char *
ns_attr_local_name(const ns_attr *attr)
{
    if (!attr) return "";
    return attr->local_name ? attr->local_name : (attr->name ? attr->name : "");
}

static const char *
ns_attr_normalize_namespace(const char *namespace_uri)
{
    return (namespace_uri && *namespace_uri) ? namespace_uri : NULL;
}

/* Compares attribute namespaces, treating NULL and empty string as no namespace. */
static gboolean
ns_attr_namespace_equal(const char *a, const char *b)
{
    a = ns_attr_normalize_namespace(a);
    b = ns_attr_normalize_namespace(b);
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* True if the attribute has the given namespace and local name. */
static gboolean
ns_attr_matches_ns(const ns_attr *attr, const char *namespace_uri,
                   const char *local_name)
{
    if (!attr || !local_name) return FALSE;
    return ns_attr_namespace_equal(attr->namespace_uri, namespace_uri) &&
           strcmp(ns_attr_local_name(attr), local_name) == 0;
}

/* Copies len bytes into a new NUL-terminated buffer; caller frees. */
char *
ns_value_dup_len(const char *value, gsize len)
{
    char *v = g_malloc(len + 1);
    if (len && value) memcpy(v, value, len);
    v[len] = '\0';
    return v;
}

/* Updates the document id index when an element's id attribute changes. */
static void
ns_doc_id_index_attr_write(ns_node *el, const char *name, const char *value)
{
    if (!name || g_ascii_strcasecmp(name, "id") != 0) return;
    ns_node *doc = el;
    while (doc->parent) doc = doc->parent;
    if (!doc->id_index) return;
    const char *old = ns_element_get_attr(el, "id");
    if (old && *old) ns_doc_id_index_unregister(doc, old, el);
    if (value && *value) ns_doc_id_index_register(doc, value, el);
}

/* Sets or replaces an attribute (ASCII case-insensitive name); len -1 means strlen. */
void
ns_element_set_attr_len(ns_node *el, const char *name,
                        const char *value, gssize len)
{
    g_return_if_fail(el != NULL);
    g_return_if_fail(el->kind == NS_NODE_ELEMENT);
    g_return_if_fail(name != NULL);

    gsize vlen = len < 0 ? (value ? strlen(value) : 0) : (gsize)len;
    if (el->class_set && g_ascii_strcasecmp(name, "class") == 0)
        ns_class_set_clear(el);
    ns_doc_id_index_attr_write(el, name, value);
    el->attr_bloom = 0;
    el->attr_gen++;

    ns_attr *tail = NULL;
    for (ns_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            if (a->flags & NS_ATTR_OWN_VALUE) g_free(a->value);
            a->value = ns_value_dup_len(value, vlen);
            a->value_len = (guint)vlen;
            a->flags |= NS_ATTR_OWN_VALUE;
            return;
        }
        tail = a;
    }
    ns_attr *a = g_new0(ns_attr, 1);
    a->name = g_strdup(name);
    a->value = ns_value_dup_len(value, vlen);
    a->value_len = (guint)vlen;
    a->flags = NS_ATTR_OWN_NAME | NS_ATTR_OWN_VALUE |
               (ns_str_is_ascii_lower(name) ? NS_ATTR_NAME_LOWER : 0);
    a->next = NULL;
    if (tail) tail->next = a;
    else      el->attrs = a;
}

void
ns_element_set_attr(ns_node *el, const char *name, const char *value)
{
    ns_element_set_attr_len(el, name, value, -1);
}

/* Sets an attribute matched by namespace and local name, as in setAttributeNS. */
void
ns_element_set_attr_ns(ns_node *el, const char *namespace_uri,
                       const char *prefix, const char *local_name,
                       const char *name, const char *value)
{
    g_return_if_fail(el != NULL);
    g_return_if_fail(el->kind == NS_NODE_ELEMENT);
    g_return_if_fail(local_name != NULL);

    const char *ns = ns_attr_normalize_namespace(namespace_uri);
    const char *pfx = prefix && *prefix ? prefix : NULL;
    const char *qualified = name && *name ? name : local_name;

    if (el->class_set && g_ascii_strcasecmp(local_name, "class") == 0)
        ns_class_set_clear(el);
    ns_doc_id_index_attr_write(el, qualified, value);
    el->attr_bloom = 0;
    el->attr_gen++;

    gsize vlen = value ? strlen(value) : 0;
    ns_attr *tail = NULL;
    for (ns_attr *a = el->attrs; a; a = a->next) {
        if (ns_attr_matches_ns(a, ns, local_name)) {
            if (a->flags & NS_ATTR_OWN_VALUE) g_free(a->value);
            a->value = ns_value_dup_len(value, vlen);
            a->value_len = (guint)vlen;
            a->flags |= NS_ATTR_OWN_VALUE;
            return;
        }
        tail = a;
    }

    ns_attr *a = g_new0(ns_attr, 1);
    a->name = g_strdup(qualified);
    a->value = ns_value_dup_len(value, vlen);
    a->value_len = (guint)vlen;
    a->namespace_uri = ns ? g_strdup(ns) : NULL;
    a->prefix = pfx ? g_strdup(pfx) : NULL;
    a->local_name = g_strdup(local_name);
    a->flags = NS_ATTR_OWN_NAME | NS_ATTR_OWN_VALUE |
               (ns_str_is_ascii_lower(qualified) ? NS_ATTR_NAME_LOWER : 0);
    if (tail) tail->next = a;
    else      el->attrs = a;
}

/* Removes the first attribute with this name (case-insensitive), updating caches. */
void
ns_element_remove_attr(ns_node *el, const char *name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !name) return;
    if (el->class_set && g_ascii_strcasecmp(name, "class") == 0)
        ns_class_set_clear(el);
    ns_doc_id_index_attr_write(el, name, NULL);
    el->attr_bloom = 0;
    el->attr_gen++;
    ns_attr **link = &el->attrs;
    while (*link) {
        if (g_ascii_strcasecmp((*link)->name, name) == 0) {
            ns_attr *dead = *link;
            *link = dead->next;
            ns_attr_free_one(dead);
            return;
        }
        link = &(*link)->next;
    }
}

/* Removes the attribute matching namespace and local name, updating caches. */
void
ns_element_remove_attr_ns(ns_node *el, const char *namespace_uri,
                          const char *local_name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !local_name) return;
    const char *ns = ns_attr_normalize_namespace(namespace_uri);
    if (el->class_set && g_ascii_strcasecmp(local_name, "class") == 0)
        ns_class_set_clear(el);
    ns_doc_id_index_attr_write(el, local_name, NULL);
    el->attr_bloom = 0;
    el->attr_gen++;
    ns_attr **link = &el->attrs;
    while (*link) {
        if (ns_attr_matches_ns(*link, ns, local_name)) {
            ns_attr *dead = *link;
            *link = dead->next;
            ns_attr_free_one(dead);
            return;
        }
        link = &(*link)->next;
    }
}

#define NS_DOM_MAX_DEPTH 512

/* Clones a node (and optionally its subtree and template contents) up to max depth. */
static ns_node *
ns_node_clone_depth(const ns_node *src, gboolean deep, int depth)
{
    if (!src || depth >= NS_DOM_MAX_DEPTH) return NULL;
    ns_node *out = NULL;
    switch (src->kind) {
    case NS_NODE_ELEMENT:
        out = ns_node_new_element(src->name ? g_strdup(src->name) : g_strdup(""));
        out->flags |= src->flags & (NS_NODE_SVG_NS | NS_NODE_FOREIGN_NS |
                                    NS_NODE_KEEP_CASE |
                                    NS_NODE_INPUT_INDETERMINATE);
        for (const ns_attr *a = src->attrs; a; a = a->next)
            ns_element_set_attr_ns(out, a->namespace_uri, a->prefix,
                                   ns_attr_local_name(a), a->name,
                                   a->value ? a->value : "");
        break;
    case NS_NODE_TEXT:
        out = ns_node_new_text_len(src->text ? g_memdup2(src->text, src->text_len + 1) : g_strdup(""),
                                  src->text_len);
        break;
    case NS_NODE_DOCTYPE:
        out = ns_node_new_doctype(g_strdup(src->name ? src->name : ""),
                                  g_strdup(src->public_id ? src->public_id : ""),
                                  g_strdup(src->system_id ? src->system_id : ""));
        break;
    case NS_NODE_DOCUMENT:
    case NS_NODE_COMMENT:
        out = ns_node_new(src->kind);
        if (src->text) {
            out->text = g_memdup2(src->text, src->text_len + 1);
            out->text_len = src->text_len;
            out->flags |= NS_NODE_OWN_TEXT;
        }
        if (src->name) {
            out->name = g_strdup(src->name);
            out->flags |= NS_NODE_OWN_NAME;
        }
        for (const ns_attr *a = src->attrs; a; a = a->next)
            ns_element_set_attr_ns(out, a->namespace_uri, a->prefix,
                                   ns_attr_local_name(a), a->name,
                                   a->value ? a->value : "");
        break;
    }
    if (out) out->flags |= src->flags & (NS_NODE_FRAGMENT | NS_NODE_CDATA |
                                         NS_NODE_PI);
    if (deep && out) {
        for (const ns_node *c = src->first_child; c; c = c->next_sibling) {
            /* A frame's document is not a child of the cloned subtree. */
            if (ns_node_is_embedded_document(c)) continue;
            ns_node *cc = ns_node_clone_depth(c, TRUE, depth + 1);
            if (cc) ns_node_append_child(out, cc);
        }
        if (src->tpl_content)
            out->tpl_content = ns_node_clone_depth(src->tpl_content, TRUE,
                                                   depth + 1);
    }
    return out;
}

ns_node *
ns_node_clone(const ns_node *src, gboolean deep)
{
    return ns_node_clone_depth(src, deep, 0);
}

/* Returns the template's content fragment, creating it lazily. */
ns_node *
ns_template_content_get(ns_node *tpl)
{
    if (!tpl) return NULL;
    if (!tpl->tpl_content) {
        ns_node *frag = ns_node_new_document();
        if (!frag) return NULL;
        frag->flags |= NS_NODE_FRAGMENT | NS_NODE_TEMPLATE_CONTENT;
        tpl->tpl_content = frag;
    }
    return tpl->tpl_content;
}

/* FNV-1a hash of an attribute name mapped to one bit of a 64-bit bloom filter. */
guint64
ns_attr_name_bloom_bit(const char *name)
{
    guint32 h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return (guint64)1 << (h & 63);
}

/* Returns the element's cached attribute-name bloom filter, computing it on demand. */
guint64
ns_node_attr_bloom(const ns_node *el)
{
    if (el->attr_bloom) return el->attr_bloom;
    guint64 b = 0;
    for (const ns_attr *a = el->attrs; a; a = a->next)
        if (a->name) b |= ns_attr_name_bloom_bit(a->name);
    ((ns_node *)el)->attr_bloom = b;
    return b;
}

/* Finds an attribute by name: exact match for SVG, ASCII case-insensitive otherwise. */
const ns_attr *
ns_element_find_attr(const ns_node *el, const char *name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !name)
        return NULL;
    if (el->flags & NS_NODE_SVG_NS) {
        for (const ns_attr *a = el->attrs; a; a = a->next)
            if (a->name && strcmp(a->name, name) == 0)
                return a;
        return NULL;
    }
    if (ns_str_is_ascii_lower(name)) {
        char c0 = name[0];
        for (const ns_attr *a = el->attrs; a; a = a->next) {
            const char *an = a->name;
            if (!an) continue;
            if (a->flags & NS_ATTR_NAME_LOWER) {
                if (an[0] == c0 && strcmp(an, name) == 0) return a;
            } else if (g_ascii_strcasecmp(an, name) == 0) {
                return a;
            }
        }
        return NULL;
    }
    for (const ns_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0)
            return a;
    }
    return NULL;
}

const char *
ns_element_get_attr(const ns_node *el, const char *name)
{
    const ns_attr *a = ns_element_find_attr(el, name);
    return a ? a->value : NULL;
}

const char *
ns_element_get_attr_len(const ns_node *el, const char *name, gsize *out_len)
{
    const ns_attr *a = ns_element_find_attr(el, name);
    if (!a) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = a->value_len;
    return a->value;
}

/* Finds an attribute by namespace URI and local name. */
const ns_attr *
ns_element_find_attr_ns(const ns_node *el, const char *namespace_uri,
                        const char *local_name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !local_name)
        return NULL;
    const char *ns = ns_attr_normalize_namespace(namespace_uri);
    for (const ns_attr *a = el->attrs; a; a = a->next)
        if (ns_attr_matches_ns(a, ns, local_name))
            return a;
    return NULL;
}

gboolean
ns_node_is_element_named(const ns_node *n, const char *tag)
{
    return n && n->kind == NS_NODE_ELEMENT && n->name && tag &&
           strcmp(n->name, tag) == 0;
}

/* True for input types in a one-line text state (text, search, email, url, etc.). */
gboolean
ns_input_is_one_line_text(const ns_node *n)
{
    if (!ns_node_is_element_named(n, "input")) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    if (!type) return TRUE;
    static const char *const other_states[] = {
        "button", "checkbox", "color", "date", "datetime-local", "file",
        "hidden", "image", "month", "number", "radio", "range", "reset",
        "submit", "time", "week",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(other_states); i++)
        if (g_ascii_strcasecmp(type, other_states[i]) == 0) return FALSE;
    return TRUE;
}

/* True for a document node parented under a frame element. */
gboolean
ns_node_is_embedded_document(const ns_node *n)
{
    return n && n->kind == NS_NODE_DOCUMENT && n->parent != NULL;
}

/* Topmost ancestor, crossing frame documents; bounded by the max DOM depth. */
const ns_node *
ns_node_root(const ns_node *n)
{
    if (!n) return NULL;
    int depth = 0;
    while (n->parent && depth++ < NS_DOM_MAX_DEPTH) n = n->parent;
    return n;
}

/* Root of the node's own document tree.  Unlike ns_node_root(), stops at
 * a document node: a frame's document hangs under its <iframe> element,
 * and DOM lookups (getRootNode, label/form owners, id lookups) from inside
 * the frame must not reach the embedding page. */
const ns_node *
ns_node_document_root(const ns_node *n)
{
    if (!n) return NULL;
    int depth = 0;
    while (n->parent && depth++ < NS_DOM_MAX_DEPTH &&
           !(n->kind == NS_NODE_DOCUMENT && !(n->flags & NS_NODE_FRAGMENT)))
        n = n->parent;
    return n;
}

/* Depth-first search for the first element with this tag, skipping frame documents. */
static ns_node *
ns_node_find_first_element_depth(const ns_node *root, const char *tag, int depth)
{
    if (!root || !tag || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (ns_node_is_element_named(root, tag))
        return (ns_node *)root;
    for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
        if (ns_node_is_embedded_document(c)) continue;
        ns_node *m = ns_node_find_first_element_depth(c, tag, depth + 1);
        if (m) return m;
    }
    return NULL;
}

/* First element with this tag, via the tag index when the root has one. */
ns_node *
ns_node_find_first_element(const ns_node *root, const char *tag)
{
    if (root && tag && *tag && root->tag_index) {
        GPtrArray *list = ns_doc_tag_index_lookup(root, tag);
        if (list && list->len > 0) return g_ptr_array_index(list, 0);
        return NULL;
    }
    return ns_node_find_first_element_depth(root, tag, 0);
}


/* True for elements marked as shadow roots by the internal shadow attribute. */
gboolean
ns_node_is_shadow_root(const ns_node *n)
{
    return n && n->kind == NS_NODE_ELEMENT &&
           ns_element_get_attr(n, NS_SHADOW_ATTR) != NULL;
}

/* Tree walk for an id, not descending into shadow roots, templates or frame docs. */
static ns_node *
ns_node_find_by_id_depth(const ns_node *root, const char *id, int depth)
{
    if (!root || !id || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (depth > 0 && ns_node_is_shadow_root(root)) return NULL;
    if (root->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(root, "id");
        if (eid && strcmp(eid, id) == 0) return (ns_node *)root;
    }
    if (ns_node_is_element_named(root, "template")) return NULL;
    for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
        if (ns_node_is_embedded_document(c)) continue;
        ns_node *m = ns_node_find_by_id_depth(c, id, depth + 1);
        if (m) return m;
    }
    return NULL;
}

#define NS_ID_COUNT_MASK  0x7fffffffu
#define NS_ID_FIRST_KNOWN 0x80000000u

/* Packed id-count entry for id: occurrence count plus a first-match-known flag bit. */
static guint
ns_doc_id_entry(const ns_node *doc, const char *id)
{
    return doc->id_counts
        ? GPOINTER_TO_UINT(g_hash_table_lookup(doc->id_counts, id)) : 0;
}

/* Clears the first-match-known flag for an id after its index entry changed. */
static void
ns_doc_id_forget_first(const ns_node *doc, const char *id)
{
    guint entry = ns_doc_id_entry(doc, id);
    if (entry & NS_ID_FIRST_KNOWN)
        g_hash_table_insert(doc->id_counts, g_strdup(id),
                            GUINT_TO_POINTER(entry & NS_ID_COUNT_MASK));
}

/* Adjusts the per-id occurrence count by delta, removing it when it drops to zero. */
static void
ns_doc_id_index_count(ns_node *doc, const char *id, int delta)
{
    if (!doc->id_counts)
        doc->id_counts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    guint cur = ns_doc_id_entry(doc, id) & NS_ID_COUNT_MASK;
    if (delta > 0)
        g_hash_table_insert(doc->id_counts, g_strdup(id),
                            GUINT_TO_POINTER(cur + 1));
    else if (cur <= 1)
        g_hash_table_remove(doc->id_counts, id);
    else
        g_hash_table_insert(doc->id_counts, g_strdup(id),
                            GUINT_TO_POINTER(cur - 1));
}

/* Counts an id occurrence and indexes node if no element holds that id yet. */
static void
ns_doc_id_index_register(ns_node *doc, const char *id, ns_node *node)
{
    if (!doc || !doc->id_index || !id || !*id || !node) return;
    ns_doc_id_index_count(doc, id, 1);
    if (g_hash_table_contains(doc->id_index, id)) return;
    g_hash_table_insert(doc->id_index, g_strdup(id), node);
}

/* Decrements an id count and drops the index entry if it pointed at node. */
static void
ns_doc_id_index_unregister(ns_node *doc, const char *id, const ns_node *node)
{
    if (!doc || !doc->id_index || !id || !*id) return;
    ns_doc_id_index_count(doc, id, -1);
    gpointer cur = g_hash_table_lookup(doc->id_index, id);
    if (cur == node) g_hash_table_remove(doc->id_index, id);
}

/* Registers ids in a subtree, skipping shadow roots and frame documents. */
static void
ns_doc_id_index_add_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n != doc && ns_node_is_embedded_document(n)) return;
    if (n != doc && ns_node_is_shadow_root(n)) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(n, "id");
        if (eid && *eid) ns_doc_id_index_register(doc, eid, n);
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_id_index_add_subtree(doc, c, depth + 1);
}

/* (Re)builds the document's id index and id counts from scratch. */
void
ns_doc_id_index_build(ns_node *doc)
{
    if (!doc) return;
    if (doc->id_index) {
        g_hash_table_remove_all(doc->id_index);
    } else {
        doc->id_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    }
    if (doc->id_counts)
        g_hash_table_remove_all(doc->id_counts);
    ns_doc_id_index_add_subtree(doc, doc, 0);
}

/* Unregisters ids in a subtree, skipping shadow roots and frame documents. */
static void
ns_doc_id_index_remove_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n != doc && ns_node_is_embedded_document(n)) return;
    if (n != doc && ns_node_is_shadow_root(n)) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(n, "id");
        if (eid && *eid) ns_doc_id_index_unregister(doc, eid, n);
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_id_index_remove_subtree(doc, c, depth + 1);
}

/* True if any ancestor of n below stop is a shadow root. */
static gboolean
ns_node_inside_shadow_root(const ns_node *n, const ns_node *stop)
{
    for (const ns_node *p = n ? n->parent : NULL; p && p != stop; p = p->parent)
        if (ns_node_is_shadow_root(p)) return TRUE;
    return FALSE;
}

/* Indexes ids of a newly inserted subtree unless it lives in a shadow root. */
void
ns_doc_id_index_subtree_added(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->id_index || !root) return;
    if (ns_node_inside_shadow_root(root, doc)) return;
    ns_doc_id_index_add_subtree(doc, root, 0);
}

/* Unindexes ids of a removed subtree unless it lives in a shadow root. */
void
ns_doc_id_index_subtree_removed(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->id_index || !root) return;
    if (ns_node_inside_shadow_root(root, doc)) return;
    ns_doc_id_index_remove_subtree(doc, root, 0);
}

#define NS_DOC_INDEX_SCAN_MAX 64

typedef struct ns_doc_index_bucket {
    GPtrArray  *nodes;
    GHashTable *members;
    gboolean    unsorted;
} ns_doc_index_bucket;

static ns_doc_index_bucket *
ns_doc_index_bucket_new(void)
{
    ns_doc_index_bucket *b = g_new0(ns_doc_index_bucket, 1);
    b->nodes = g_ptr_array_new();
    return b;
}

static void
ns_doc_index_bucket_free(gpointer p)
{
    ns_doc_index_bucket *b = p;
    g_ptr_array_free(b->nodes, TRUE);
    if (b->members) g_hash_table_destroy(b->members);
    g_free(b);
}

/* Compares two nodes in tree order (-1/0/1); ancestors sort before descendants. */
int
ns_node_document_order_cmp(const ns_node *a, const ns_node *b)
{
    if (a == b) return 0;
    int da = 0, db = 0;
    for (const ns_node *p = a; p && da < NS_DOM_MAX_DEPTH; p = p->parent) da++;
    for (const ns_node *p = b; p && db < NS_DOM_MAX_DEPTH; p = p->parent) db++;
    const ns_node *ca = a, *cb = b;
    int x = da, y = db;
    while (x > y && ca) { ca = ca->parent; x--; }
    while (y > x && cb) { cb = cb->parent; y--; }
    if (ca == cb) return da < db ? -1 : 1;
    int guard = 0;
    while (ca && cb && ca->parent != cb->parent && guard++ < NS_DOM_MAX_DEPTH) {
        ca = ca->parent;
        cb = cb->parent;
    }
    if (!ca || !cb) return (a < b) ? -1 : 1;
    const ns_node *par = ca->parent;
    if (par) {
        if (ca == par->first_child || cb == par->last_child) return -1;
        if (cb == par->first_child || ca == par->last_child) return 1;
    }
    const ns_node *fwd = ca->next_sibling;
    const ns_node *back = ca->prev_sibling;
    while (fwd || back) {
        if (fwd == cb) return -1;
        if (back == cb) return 1;
        if (fwd) fwd = fwd->next_sibling;
        if (back) back = back->prev_sibling;
    }
    return 0;
}

static gboolean g_doc_index_building;

/* Lazily builds the membership hash set for an index bucket. */
static void
ns_doc_index_bucket_track_members(ns_doc_index_bucket *b)
{
    if (b->members) return;
    b->members = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < b->nodes->len; i++)
        g_hash_table_add(b->members, g_ptr_array_index(b->nodes, i));
}

/* Inserts a node in document order; large buckets switch to an unsorted set. */
static void
ns_doc_index_bucket_add(ns_doc_index_bucket *b, ns_node *node)
{
    GPtrArray *arr = b->nodes;
    if (g_doc_index_building) {
        if (arr->len == 0 ||
            g_ptr_array_index(arr, arr->len - 1) != node) {
            g_ptr_array_add(arr, node);
            if (b->members) g_hash_table_add(b->members, node);
        }
        return;
    }
    if (b->unsorted) {
        if (g_hash_table_add(b->members, node))
            g_ptr_array_add(arr, node);
        return;
    }
    if (arr->len == 0 ||
        ns_node_document_order_cmp(node, g_ptr_array_index(arr, arr->len - 1)) > 0) {
        g_ptr_array_add(arr, node);
        if (b->members) g_hash_table_add(b->members, node);
        return;
    }
    if (arr->len > NS_DOC_INDEX_SCAN_MAX) {
        ns_doc_index_bucket_track_members(b);
        if (g_hash_table_add(b->members, node)) {
            g_ptr_array_add(arr, node);
            b->unsorted = TRUE;
        }
        return;
    }
    guint lo = 0, hi = arr->len;
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        int c = ns_node_document_order_cmp(node,
                                           g_ptr_array_index(arr, mid));
        if (c == 0)
            return;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    g_ptr_array_insert(arr, (gint)lo, node);
    if (b->members) g_hash_table_add(b->members, node);
}

/* Removes a node from a bucket, marking large buckets unsorted rather than shifting. */
static void
ns_doc_index_bucket_remove(ns_doc_index_bucket *b, ns_node *node)
{
    GPtrArray *arr = b->nodes;
    if (!b->unsorted && arr->len > 0 &&
        g_ptr_array_index(arr, arr->len - 1) == node) {
        g_ptr_array_set_size(arr, arr->len - 1);
        if (b->members) g_hash_table_remove(b->members, node);
        return;
    }
    if (!b->unsorted && arr->len <= NS_DOC_INDEX_SCAN_MAX) {
        for (guint k = 0; k < arr->len; k++) {
            if (g_ptr_array_index(arr, k) == node) {
                g_ptr_array_remove_index(arr, k);
                if (b->members) g_hash_table_remove(b->members, node);
                break;
            }
        }
        return;
    }
    ns_doc_index_bucket_track_members(b);
    if (g_hash_table_remove(b->members, node))
        b->unsorted = TRUE;
}

static gint
ns_doc_index_order_compare(gconstpointer a, gconstpointer b)
{
    return ns_node_document_order_cmp(*(const ns_node *const *)a,
                                      *(const ns_node *const *)b);
}

/* Rebuilds bucket order by walking the document; sorts any members not found. */
static void
ns_doc_index_bucket_collect_in_order(const ns_node *doc,
                                     ns_doc_index_bucket *b)
{
    GPtrArray *arr = b->nodes;
    guint want = g_hash_table_size(b->members);
    const ns_node *n = doc;
    while (n && arr->len < want) {
        if (g_hash_table_contains(b->members, n))
            g_ptr_array_add(arr, (gpointer)n);
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n && n != doc && !n->next_sibling) n = n->parent;
        n = (n && n != doc) ? n->next_sibling : NULL;
    }
    if (arr->len == want) return;
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < arr->len; i++)
        g_hash_table_add(seen, g_ptr_array_index(arr, i));
    GPtrArray *rest = g_ptr_array_new();
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, b->members);
    while (g_hash_table_iter_next(&it, &key, NULL))
        if (!g_hash_table_contains(seen, key)) g_ptr_array_add(rest, key);
    g_ptr_array_sort(rest, ns_doc_index_order_compare);
    for (guint i = 0; i < rest->len; i++)
        g_ptr_array_add(arr, g_ptr_array_index(rest, i));
    g_ptr_array_free(rest, TRUE);
    g_hash_table_destroy(seen);
}

/* Returns the bucket's nodes in document order, re-sorting if it was marked unsorted. */
static GPtrArray *
ns_doc_index_bucket_nodes(const ns_node *doc, ns_doc_index_bucket *b)
{
    if (!b) return NULL;
    if (!b->unsorted) return b->nodes;
    g_ptr_array_set_size(b->nodes, 0);
    if (g_hash_table_size(b->members) <= NS_DOC_INDEX_SCAN_MAX) {
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, b->members);
        while (g_hash_table_iter_next(&it, &key, NULL))
            g_ptr_array_add(b->nodes, key);
        g_ptr_array_sort(b->nodes, ns_doc_index_order_compare);
    } else {
        ns_doc_index_bucket_collect_in_order(doc, b);
    }
    b->unsorted = FALSE;
    return b->nodes;
}

/* Adds or removes node from the class index bucket for one class token. */
static void
ns_doc_class_index_update_token(GHashTable *map, const char *tok,
                                gsize tok_len, ns_node *node, gboolean add)
{
    if (tok_len == 0) return;
    char stack[96];
    gchar *key = tok_len < sizeof(stack) ? stack : g_strndup(tok, tok_len);
    if (key == stack) {
        memcpy(stack, tok, tok_len);
        stack[tok_len] = '\0';
    }
    ns_doc_index_bucket *bucket = g_hash_table_lookup(map, key);
    if (!bucket && add) {
        bucket = ns_doc_index_bucket_new();
        g_ptr_array_add(bucket->nodes, node);
        g_hash_table_insert(map, key == stack ? g_strndup(tok, tok_len) : key,
                            bucket);
        return;
    }
    if (key != stack) g_free(key);
    if (!bucket) return;
    if (add) ns_doc_index_bucket_add(bucket, node);
    else ns_doc_index_bucket_remove(bucket, node);
}

/* Splits a class attribute on whitespace and updates the index for each token. */
static void
ns_doc_class_index_update(ns_node *doc, const char *class_attr, ns_node *node,
                          gboolean add)
{
    if (!doc || !doc->class_index || !class_attr || !node) return;
    const char *p = class_attr;
    while (*p) {
        while (*p && ns_is_ascii_ws(*p)) p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && !ns_is_ascii_ws(*p)) p++;
        ns_doc_class_index_update_token(doc->class_index, tok,
                                        (gsize)(p - tok), node, add);
    }
}

void
ns_doc_class_index_register(ns_node *doc, const char *class_attr, ns_node *node)
{
    ns_doc_class_index_update(doc, class_attr, node, TRUE);
}

void
ns_doc_class_index_unregister(ns_node *doc, const char *class_attr, ns_node *node)
{
    ns_doc_class_index_update(doc, class_attr, node, FALSE);
}

/* Registers class tokens for a subtree, not entering templates or frame documents. */
static void
ns_doc_class_index_add_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n != doc && ns_node_is_embedded_document(n)) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *cls = ns_element_get_attr(n, "class");
        if (cls && *cls) ns_doc_class_index_register(doc, cls, n);
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_class_index_add_subtree(doc, c, depth + 1);
}

/* Unregisters class tokens for a subtree, not entering templates or frame documents. */
static void
ns_doc_class_index_remove_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n != doc && ns_node_is_embedded_document(n)) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *cls = ns_element_get_attr(n, "class");
        if (cls && *cls) ns_doc_class_index_unregister(doc, cls, n);
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_class_index_remove_subtree(doc, c, depth + 1);
}

/* (Re)builds the document's class index in a single in-order pass. */
void
ns_doc_class_index_build(ns_node *doc)
{
    if (!doc) return;
    if (doc->class_index) {
        g_hash_table_remove_all(doc->class_index);
    } else {
        doc->class_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, ns_doc_index_bucket_free);
    }
    g_doc_index_building = TRUE;
    ns_doc_class_index_add_subtree(doc, doc, 0);
    g_doc_index_building = FALSE;
}

void
ns_doc_class_index_subtree_added(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->class_index || !root) return;
    ns_doc_class_index_add_subtree(doc, root, 0);
}

void
ns_doc_class_index_subtree_removed(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->class_index || !root) return;
    ns_doc_class_index_remove_subtree(doc, root, 0);
}

/* Elements with this class token in document order; NULL if unindexed or absent. */
GPtrArray *
ns_doc_class_index_lookup(const ns_node *doc, const char *cls)
{
    if (!doc || !doc->class_index || !cls || !*cls) return NULL;
    return ns_doc_index_bucket_nodes(doc,
                                     g_hash_table_lookup(doc->class_index, cls));
}

/* Adds node to the tag index under the ASCII-lowercased tag name. */
static void
ns_doc_tag_index_add_single(GHashTable *map, const char *tag, ns_node *node)
{
    if (!tag || !*tag) return;
    gboolean is_lower = ns_str_is_ascii_lower(tag);
    ns_doc_index_bucket *bucket = is_lower
        ? g_hash_table_lookup(map, tag)
        : NULL;
    if (!bucket && !is_lower) {
        gchar *probe = g_ascii_strdown(tag, -1);
        bucket = g_hash_table_lookup(map, probe);
        g_free(probe);
    }
    if (bucket) {
        ns_doc_index_bucket_add(bucket, node);
        return;
    }
    bucket = ns_doc_index_bucket_new();
    g_ptr_array_add(bucket->nodes, node);
    g_hash_table_insert(map,
        is_lower ? g_strdup(tag) : g_ascii_strdown(tag, -1), bucket);
}

/* Removes node from the tag index bucket for its lowercased tag name. */
static void
ns_doc_tag_index_remove_single(GHashTable *map, const char *tag, ns_node *node)
{
    if (!tag || !*tag) return;
    ns_doc_index_bucket *bucket;
    if (ns_str_is_ascii_lower(tag)) {
        bucket = g_hash_table_lookup(map, tag);
    } else {
        gchar *key = g_ascii_strdown(tag, -1);
        bucket = g_hash_table_lookup(map, key);
        g_free(key);
    }
    if (bucket) ns_doc_index_bucket_remove(bucket, node);
}

/* Registers element tags for a subtree, not entering templates or frame documents. */
static void
ns_doc_tag_index_add_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n != doc && ns_node_is_embedded_document(n)) return;
    if (n->kind == NS_NODE_ELEMENT && n->name)
        ns_doc_tag_index_add_single(doc->tag_index, n->name, n);
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_tag_index_add_subtree(doc, c, depth + 1);
}

/* Unregisters element tags for a subtree, not entering templates or frame documents. */
static void
ns_doc_tag_index_remove_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n != doc && ns_node_is_embedded_document(n)) return;
    if (n->kind == NS_NODE_ELEMENT && n->name)
        ns_doc_tag_index_remove_single(doc->tag_index, n->name, n);
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_tag_index_remove_subtree(doc, c, depth + 1);
}

/* (Re)builds the document's tag-name index in a single in-order pass. */
void
ns_doc_tag_index_build(ns_node *doc)
{
    if (!doc) return;
    if (doc->tag_index) {
        g_hash_table_remove_all(doc->tag_index);
    } else {
        doc->tag_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, ns_doc_index_bucket_free);
    }
    g_doc_index_building = TRUE;
    ns_doc_tag_index_add_subtree(doc, doc, 0);
    g_doc_index_building = FALSE;
}

void
ns_doc_tag_index_subtree_added(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->tag_index || !root) return;
    ns_doc_tag_index_add_subtree(doc, root, 0);
}

void
ns_doc_tag_index_subtree_removed(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->tag_index || !root) return;
    ns_doc_tag_index_remove_subtree(doc, root, 0);
}

/* Elements with this tag (ASCII case-insensitive) in document order, or NULL. */
GPtrArray *
ns_doc_tag_index_lookup(const ns_node *doc, const char *tag)
{
    if (!doc || !doc->tag_index || !tag || !*tag) return NULL;
    if (ns_str_is_ascii_lower(tag))
        return ns_doc_index_bucket_nodes(doc,
                                         g_hash_table_lookup(doc->tag_index, tag));
    gchar *key = g_ascii_strdown(tag, -1);
    ns_doc_index_bucket *bucket = g_hash_table_lookup(doc->tag_index, key);
    g_free(key);
    return ns_doc_index_bucket_nodes(doc, bucket);
}

/* Checks a cached id hit still carries the id, is under root and outside shadow roots. */
static gboolean
ns_node_id_hit_valid(const ns_node *root, const ns_node *hit, const char *id)
{
    if (!hit) return FALSE;
    const char *hid = ns_element_get_attr(hit, "id");
    if (!hid || strcmp(hid, id) != 0) return FALSE;
    if (!ns_node_contains(root, hit)) return FALSE;
    return !ns_node_inside_shadow_root(hit, root);
}

/* True if the cached id hit is valid and the id is not duplicated in the document. */
static gboolean
ns_node_id_hit_usable(const ns_node *root, const ns_node *doc,
                      const ns_node *hit, const char *id)
{
    if ((ns_doc_id_entry(doc, id) & NS_ID_COUNT_MASK) > 1) return FALSE;
    return ns_node_id_hit_valid(root, hit, id);
}

/* getElementById: uses and repairs the id index, falling back to a tree walk. */
ns_node *
ns_node_find_by_id(const ns_node *root, const char *id)
{
    if (!root || !id || !*id) return NULL;
    if (root->id_index) {
        ns_node *hit = g_hash_table_lookup(root->id_index, id);
        guint entry = ns_doc_id_entry(root, id);
        if (entry & NS_ID_FIRST_KNOWN) {
            if (!hit || ns_node_id_hit_valid(root, hit, id))
                return hit;
        } else if (ns_node_id_hit_usable(root, root, hit, id)) {
            return hit;
        }
        if (!hit && !entry)
            return NULL;
        ns_node *found = ns_node_find_by_id_depth(root, id, 0);
        if (found) {
            g_hash_table_replace(root->id_index, g_strdup(id), found);
        } else if (hit) {
            g_hash_table_remove(root->id_index, id);
        }
        if ((entry & NS_ID_COUNT_MASK) > 1)
            g_hash_table_insert(root->id_counts, g_strdup(id),
                                GUINT_TO_POINTER((entry & NS_ID_COUNT_MASK) |
                                                 NS_ID_FIRST_KNOWN));
        return found;
    }
    const ns_node *doc = root;
    while (doc && doc->parent && doc->kind != NS_NODE_DOCUMENT)
        doc = doc->parent;
    if (doc && doc != root && doc->id_index) {
        ns_node *hit = g_hash_table_lookup(doc->id_index, id);
        if (ns_node_id_hit_usable(root, doc, hit, id))
            return hit;
        ns_node *found = ns_node_find_by_id_depth(root, id, 0);
        if (found) {
            g_hash_table_replace(doc->id_index, g_strdup(id), found);
            ns_doc_id_forget_first(doc, id);
        }
        return found;
    }
    return ns_node_find_by_id_depth(root, id, 0);
}

/* Finds the first a element whose name attribute equals name, skipping templates. */
static ns_node *
ns_node_find_anchor_name_depth(const ns_node *root, const char *name, int depth)
{
    if (!root || !name || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (ns_node_is_element_named(root, "a")) {
        const char *n = ns_element_get_attr(root, "name");
        if (n && strcmp(n, name) == 0) return (ns_node *)root;
    }
    if (ns_node_is_element_named(root, "template")) return NULL;
    for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
        ns_node *m = ns_node_find_anchor_name_depth(c, name, depth + 1);
        if (m) return m;
    }
    return NULL;
}

/* Finds a URL fragment's target: element by id, else an a element with that name. */
ns_node *
ns_node_find_fragment_target(const ns_node *root, const char *frag)
{
    if (!root || !frag || !*frag) return NULL;
    ns_node *by_id = ns_node_find_by_id(root, frag);
    if (by_id) return by_id;
    return ns_node_find_anchor_name_depth(root, frag, 0);
}

/* True when the element has hidden="until-found". */
gboolean
ns_element_hidden_until_found(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    const char *hidden = ns_element_get_attr(el, "hidden");
    return hidden && g_ascii_strcasecmp(hidden, "until-found") == 0;
}

/* True if a closed details must open to reveal target (target is not inside its summary). */
gboolean
ns_details_fragment_needs_open(const ns_node *details, const ns_node *target)
{
    if (!ns_node_is_element_named(details, "details") ||
        !target || target == details ||
        ns_element_get_attr(details, "open"))
        return FALSE;
    const ns_node *child = target;
    int g = 0;
    while (child && child->parent != details && g++ < NS_DOM_MAX_DEPTH) child = child->parent;
    if (!child || child->parent != details) return FALSE;
    const ns_node *summary = NULL;
    for (const ns_node *c = details->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "summary")) {
            summary = c;
            break;
        }
    }
    return !summary || child != summary;
}

/* Appends descendant text to out, skipping script element subtrees. */
static void
collect_descendant_text_skip_script(const ns_node *n, GString *out, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_TEXT) {
            if (c->text) g_string_append(out, c->text);
        } else if (c->kind == NS_NODE_ELEMENT && c->name &&
                   g_ascii_strcasecmp(c->name, "script") == 0) {
            continue;
        } else {
            collect_descendant_text_skip_script(c, out, depth + 1);
        }
    }
}

static char *
ns_node_collect_descendant_text_skip_script(const ns_node *root)
{
    GString *out = g_string_new(NULL);
    collect_descendant_text_skip_script(root, out, 0);
    return g_string_free(out, FALSE);
}

/* Strips and collapses ASCII whitespace runs to single spaces; caller frees. */
static char *
ns_strip_and_collapse_ascii_ws(const char *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_new(NULL);
    gboolean in_ws = TRUE;
    const char *end = s + strlen(s);
    for (const char *p = s; *p; ) {
        gunichar ch = g_utf8_get_char(p);
        const char *next = g_utf8_next_char(p);
        if (next > end) next = end;
        gboolean is_ws = (ch == 0x09 || ch == 0x0A || ch == 0x0C ||
                          ch == 0x0D || ch == 0x20);
        if (is_ws) {
            if (!in_ws) { g_string_append_c(out, ' '); in_ws = TRUE; }
        } else {
            g_string_append_len(out, p, next - p);
            in_ws = FALSE;
        }
        p = next;
    }
    if (out->len > 0 && out->str[out->len - 1] == ' ')
        g_string_truncate(out, out->len - 1);
    return g_string_free(out, FALSE);
}

/* Option text per HTML: descendant text minus scripts, whitespace collapsed; caller frees. */
char *
ns_option_text_dup(const ns_node *option)
{
    if (!option) return g_strdup("");
    g_autofree char *raw = ns_node_collect_descendant_text_skip_script(option);
    return ns_strip_and_collapse_ascii_ws(raw);
}

/* Option label: the label attribute if present, else its text; caller frees. */
char *
ns_option_label_dup(const ns_node *option)
{
    if (!option) return g_strdup("");
    const char *lbl = ns_element_get_attr(option, "label");
    if (lbl) return g_strdup(lbl);
    return ns_option_text_dup(option);
}

/* Option value: the value attribute if present, else its text; caller frees. */
char *
ns_option_value_dup(const ns_node *option)
{
    if (!option) return g_strdup("");
    const char *v = ns_element_get_attr(option, "value");
    if (v) return g_strdup(v);
    return ns_option_text_dup(option);
}

/* Selectedness: dirty data-nd-selected state if set, else the selected attribute. */
gboolean
ns_option_is_selected(const ns_node *option)
{
    if (!option) return FALSE;
    const char *dirty = ns_element_get_attr(option, "data-nd-selected");
    if (dirty) return strcmp(dirty, "1") == 0;
    return ns_element_get_attr(option, "selected") != NULL;
}

/* Appends a select's list of options (direct children and optgroup children) to out. */
void
ns_select_collect_options(const ns_node *select, GPtrArray *out)
{
    for (const ns_node *c = select ? select->first_child : NULL; c;
         c = c->next_sibling) {
        if (ns_node_is_element_named(c, "option")) {
            g_ptr_array_add(out, (gpointer)c);
        } else if (ns_node_is_element_named(c, "optgroup")) {
            for (const ns_node *o = c->first_child; o; o = o->next_sibling)
                if (ns_node_is_element_named(o, "option"))
                    g_ptr_array_add(out, (gpointer)o);
        }
    }
}

/* Selected option of a select: last selected for single-select, first for multiple. */
const ns_node *
ns_select_first_selected_option(const ns_node *select)
{
    if (!select) return NULL;
    gboolean last_wins = !ns_element_get_attr(select, "multiple");
    const ns_node *found = NULL;
    GPtrArray *opts = g_ptr_array_new();
    ns_select_collect_options(select, opts);
    for (guint i = 0; i < opts->len; i++) {
        const ns_node *o = g_ptr_array_index(opts, i);
        if (!ns_option_is_selected(o)) continue;
        found = o;
        if (!last_wins) break;
    }
    g_ptr_array_free(opts, TRUE);
    return found;
}

/* Returns the select owning an option (directly or via optgroup), or NULL. */
static ns_node *
ns_option_select(const ns_node *option)
{
    ns_node *select = option ? option->parent : NULL;
    if (ns_node_is_element_named(select, "optgroup")) select = select->parent;
    return ns_node_is_element_named(select, "select") ? select : NULL;
}

/* Marks chosen as the only selected option; NULL records an explicit no-selection. */
void
ns_select_set_selected_option(ns_node *select, const ns_node *chosen)
{
    if (!select) return;
    GPtrArray *opts = g_ptr_array_new();
    ns_select_collect_options(select, opts);
    for (guint i = 0; i < opts->len; i++) {
        ns_node *o = g_ptr_array_index(opts, i);
        ns_element_set_attr(o, "data-nd-selected", o == chosen ? "1" : "0");
    }
    g_ptr_array_free(opts, TRUE);
    if (chosen) ns_element_remove_attr(select, "data-nd-noselect");
    else        ns_element_set_attr(select, "data-nd-noselect", "1");
}

/* Sets an option's selectedness, deselecting siblings in a single-select. */
void
ns_option_set_selected(ns_node *option, gboolean on)
{
    if (!ns_node_is_element_named(option, "option")) return;
    ns_node *select = ns_option_select(option);
    if (on && select && !ns_element_get_attr(select, "multiple")) {
        ns_select_set_selected_option(select, option);
        return;
    }
    ns_element_set_attr(option, "data-nd-selected", on ? "1" : "0");
    if (select) ns_element_remove_attr(select, "data-nd-noselect");
}

/* Displayed option: selected one, else first enabled option for a drop-down select. */
const ns_node *
ns_select_chosen_option(const ns_node *select)
{
    if (!select) return NULL;
    const ns_node *selected = ns_select_first_selected_option(select);
    if (selected) return selected;
    if (ns_element_get_attr(select, "data-nd-noselect")) return NULL;
    if (ns_element_get_attr(select, "multiple")) return NULL;
    const char *size = ns_element_get_attr(select, "size");
    if (size) {
        char *end = NULL;
        long v = strtol(size, &end, 10);
        if (end != size && v > 1) return NULL;
    }
    const ns_node *first = NULL;
    for (const ns_node *c = select->first_child; c && !first; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "optgroup")) {
            if (ns_element_get_attr(c, "disabled")) continue;
            for (const ns_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (ns_node_is_element_named(cc, "option") &&
                    !ns_element_get_attr(cc, "disabled")) {
                    first = cc;
                    break;
                }
            }
        } else if (ns_node_is_element_named(c, "option") &&
                   !ns_element_get_attr(c, "disabled")) {
            first = c;
        }
    }
    return first;
}

/* Resolves a control's form owner via the form attribute or nearest form ancestor. */
const ns_node *
ns_form_owner(const ns_node *control, const ns_node *doc)
{
    if (!control || control->kind != NS_NODE_ELEMENT) return NULL;
    if (!doc) doc = ns_node_root(control);
    const char *form_id = ns_element_get_attr(control, "form");
    const ns_node *root = ns_node_root(control);
    gboolean connected = root && root->kind == NS_NODE_DOCUMENT &&
                         !(root->flags & NS_NODE_FRAGMENT);
    if (form_id && connected) {
        if (*form_id) {
            const ns_node *tree_root = control;
            while (tree_root->parent &&
                   !(tree_root->kind == NS_NODE_ELEMENT &&
                     ns_element_get_attr(tree_root, "data-nd-shadow-root") != NULL)) {
                tree_root = tree_root->parent;
            }
            ns_node *owner = ns_node_find_by_id(tree_root, form_id);
            if (ns_node_is_element_named(owner, "form")) return owner;
        }
        return NULL;
    }
    int depth = 0;
    for (const ns_node *p = control->parent; p && depth++ < NS_DOM_MAX_DEPTH; p = p->parent) {
        if (p->kind == NS_NODE_ELEMENT &&
            ns_element_get_attr(p, "data-nd-shadow-root") != NULL)
            break;
        if (ns_node_is_element_named(p, "form")) return p;
    }
    return NULL;
}

/* Resets a control to defaults by dropping its dirty value/checked/selected state. */
static void
ns_form_reset_control(ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return;
    if (strcmp(n->name, "input") == 0 ||
        strcmp(n->name, "textarea") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                     g_ascii_strcasecmp(type, "radio") == 0)) {
            ns_element_remove_attr(n, "data-nd-checked");
        }
        ns_element_remove_attr(n, "data-nd-value");
        ns_element_remove_attr(n, "data-nd-vdirty");
        ns_element_remove_attr(n, "data-nd-user-edited");
    } else if (strcmp(n->name, "select") == 0) {
        ns_element_remove_attr(n, "data-nd-noselect");
        GPtrArray *opts = g_ptr_array_new();
        ns_select_collect_options(n, opts);
        for (guint i = 0; i < opts->len; i++)
            ns_element_remove_attr(g_ptr_array_index(opts, i),
                                   "data-nd-selected");
        g_ptr_array_free(opts, TRUE);
    }
}

/* Recursively resets every control under scan whose form owner is form. */
static void
ns_form_reset_walk(ns_node *form, ns_node *scan, const ns_node *doc, int depth)
{
    if (!form || !scan || depth >= 512) return;
    if (scan->kind == NS_NODE_ELEMENT && scan->name &&
        ns_form_owner(scan, doc) == form)
        ns_form_reset_control(scan);
    for (ns_node *c = scan->first_child; c; c = c->next_sibling)
        ns_form_reset_walk(form, c, doc, depth + 1);
}

/* Implements form reset for all controls owned by form under root. */
void
ns_form_reset_owned_controls(ns_node *form, ns_node *root, const ns_node *doc)
{
    ns_form_reset_walk(form, root ? root : form, doc ? doc : form, 0);
}

/* True if node is ancestor or a descendant of it (inclusive). */
gboolean
ns_node_contains(const ns_node *ancestor, const ns_node *node)
{
    for (const ns_node *p = node; p; p = p->parent)
        if (p == ancestor) return TRUE;
    return FALSE;
}

static const ns_node *
ns_fieldset_first_legend(const ns_node *fieldset)
{
    if (!fieldset) return NULL;
    for (const ns_node *c = fieldset->first_child; c; c = c->next_sibling)
        if (ns_node_is_element_named(c, "legend")) return c;
    return NULL;
}

/* True for elements that accept the disabled attribute. */
gboolean
ns_element_supports_disabled(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !el->name) return FALSE;
    return strcmp(el->name, "button") == 0 ||
           strcmp(el->name, "fieldset") == 0 ||
           strcmp(el->name, "input") == 0 ||
           strcmp(el->name, "optgroup") == 0 ||
           strcmp(el->name, "option") == 0 ||
           strcmp(el->name, "select") == 0 ||
           strcmp(el->name, "textarea") == 0;
}

/* HTML disabled state: own attribute, disabled optgroup, or disabled fieldset. */
gboolean
ns_element_effectively_disabled(const ns_node *el)
{
    if (!ns_element_supports_disabled(el)) return FALSE;
    if (ns_element_get_attr(el, "disabled")) return TRUE;
    for (const ns_node *p = el->parent; p; p = p->parent) {
        if (ns_node_is_element_named(el, "option") &&
            ns_node_is_element_named(p, "optgroup") &&
            ns_element_get_attr(p, "disabled"))
            return TRUE;
        if (!ns_node_is_element_named(p, "fieldset") ||
            !ns_element_get_attr(p, "disabled"))
            continue;
        const ns_node *legend = ns_fieldset_first_legend(p);
        if (legend && ns_node_contains(legend, el)) continue;
        return TRUE;
    }
    return FALSE;
}

static const ns_node *g_active_modal;

void
ns_dom_set_active_modal(const ns_node *modal)
{
    g_active_modal = modal;
}

const ns_node *
ns_dom_active_modal(void)
{
    return g_active_modal;
}

/* True if an ancestor is inert or the element lies outside the active modal dialog. */
gboolean
ns_element_effectively_inert(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    for (const ns_node *p = el; p; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT && ns_element_get_attr(p, "inert"))
            return TRUE;
    if (g_active_modal && el != g_active_modal &&
        !ns_node_contains(g_active_modal, el))
        return TRUE;
    return FALSE;
}

/* Appends text content, skipping style, script, noscript and template subtrees. */
static void
collect_text(const ns_node *n, GString *out, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        if (n->text) g_string_append(out, n->text);
        return;
    }
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        (strcmp(n->name, "style")    == 0 ||
         strcmp(n->name, "script")   == 0 ||
         strcmp(n->name, "noscript") == 0 ||
         strcmp(n->name, "template") == 0))
        return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_text(c, out, depth + 1);
}

/* Rendered-ish text of a subtree, excluding style/script/noscript/template; caller frees. */
char *
ns_node_collect_text(const ns_node *root)
{
    GString *out = g_string_new(NULL);
    collect_text(root, out, 0);
    return g_string_free(out, FALSE);
}

/* Appends all descendant text (textContent), skipping shadow roots. */
static void
collect_all_text(const ns_node *n, GString *out, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        if (n->text) g_string_append_len(out, n->text, (gssize)n->text_len);
        return;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        if (!ns_node_is_shadow_root(c) && !ns_node_is_embedded_document(c))
            collect_all_text(c, out, depth + 1);
}

/* textContent of a node with its length, fast-pathing a single text child; caller frees. */
char *
ns_node_collect_all_text_len(const ns_node *root, size_t *out_len)
{
    if (!root || !root->first_child) {
        if (out_len) *out_len = 0;
        return g_strdup("");
    }
    if (root->first_child == root->last_child && root->first_child->kind == NS_NODE_TEXT) {
        ns_node *fc = root->first_child;
        if (out_len) *out_len = (size_t)fc->text_len;
        return fc->text ? g_memdup2(fc->text, fc->text_len + 1) : g_strdup("");
    }
    GString *out = g_string_new(NULL);
    collect_all_text(root, out, 0);
    if (out_len) *out_len = (size_t)out->len;
    return g_string_free(out, FALSE);
}

#include "html.h"

#define is_void_tag ns_html_is_void

/* True for pre, textarea and listing, whose leading newline must be re-emitted. */
static gboolean
serialize_pre_like(const char *name)
{
    return name &&
           (g_ascii_strcasecmp(name, "pre") == 0 ||
            g_ascii_strcasecmp(name, "textarea") == 0 ||
            g_ascii_strcasecmp(name, "listing") == 0);
}

/* True if element children serialize unescaped (noscript only when scripting is on). */
static gboolean
serialize_raw_text(const ns_node *node)
{
    if (!node || node->kind != NS_NODE_ELEMENT || !node->name ||
        !ns_html_is_raw_text(node->name))
        return FALSE;
    if (g_ascii_strcasecmp(node->name, "noscript") != 0)
        return TRUE;
    const ns_node *root = ns_node_root(node);
    return !root || !(root->flags & NS_NODE_SCRIPTING_DISABLED);
}

/* Returns the host's shadow root child element, if any. */
static const ns_node *
ns_serialize_shadow_child(const ns_node *host)
{
    if (!host) return NULL;
    for (const ns_node *c = host->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT &&
            ns_element_get_attr(c, NS_SHADOW_ATTR))
            return c;
    return NULL;
}

/* True if getHTML options request this shadow root (listed or serializable). */
static gboolean
ns_shadow_root_included(const ns_node *sr, const ns_html_ser_opts *opts)
{
    if (!opts || !sr) return FALSE;
    for (int i = 0; i < opts->n_roots; i++)
        if (opts->roots[i] == sr) return TRUE;
    return opts->include_serializable &&
           ns_element_get_attr(sr, "data-nd-shadow-serializable") != NULL;
}

/* Appends a doctype with optional PUBLIC/SYSTEM identifiers. */
static void
serialize_doctype(const ns_node *n, GString *out)
{
    g_string_append(out, "<!DOCTYPE ");
    g_string_append(out, n->name ? n->name : "");
    if (n->public_id && *n->public_id) {
        g_string_append(out, " PUBLIC \"");
        g_string_append(out, n->public_id);
        g_string_append_c(out, '"');
        if (n->system_id && *n->system_id) {
            g_string_append(out, " \"");
            g_string_append(out, n->system_id);
            g_string_append_c(out, '"');
        }
    } else if (n->system_id && *n->system_id) {
        g_string_append(out, " SYSTEM \"");
        g_string_append(out, n->system_id);
        g_string_append_c(out, '"');
    }
    g_string_append_c(out, '>');
}

static void serialize_node_opts(const ns_node *n, GString *out,
                                gboolean include_self, int depth,
                                const ns_html_ser_opts *opts);

/* Serializes a shadow root as a declarative template with shadowroot* attributes. */
static void
serialize_shadow_template(const ns_node *sr, GString *out, int depth,
                          const ns_html_ser_opts *opts)
{
    const char *mode = ns_element_get_attr(sr, NS_SHADOW_ATTR);
    g_string_append(out, "<template shadowrootmode=\"");
    g_string_append(out, mode ? mode : "open");
    g_string_append_c(out, '"');
    if (ns_element_get_attr(sr, "data-nd-shadow-delegates"))
        g_string_append(out, " shadowrootdelegatesfocus=\"\"");
    if (ns_element_get_attr(sr, "data-nd-shadow-serializable"))
        g_string_append(out, " shadowrootserializable=\"\"");
    if (ns_element_get_attr(sr, "data-nd-shadow-clonable"))
        g_string_append(out, " shadowrootclonable=\"\"");
    g_string_append_c(out, '>');
    for (const ns_node *c = sr->first_child; c; c = c->next_sibling)
        serialize_node_opts(c, out, TRUE, depth + 1, opts);
    g_string_append(out, "</template>");
}

/* HTML fragment serialization of a node, optionally including shadow roots. */
static void
serialize_node_opts(const ns_node *n, GString *out, gboolean include_self,
                    int depth, const ns_html_ser_opts *opts)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        ns_html_escape_append(out, n->text, FALSE);
        return;
    }
    if (n->kind == NS_NODE_COMMENT) {
        g_string_append(out, "<!--");
        g_string_append(out, n->text ? n->text : "");
        g_string_append(out, "-->");
        return;
    }
    if (n->kind == NS_NODE_DOCTYPE) {
        serialize_doctype(n, out);
        return;
    }
    gboolean raw_text = serialize_raw_text(n);
    if (n->kind == NS_NODE_ELEMENT && include_self) {
        g_string_append_c(out, '<');
        g_string_append(out, n->name ? n->name : "");
        for (const ns_attr *a = n->attrs; a; a = a->next) {
            if (ns_attr_name_is_internal(a->name)) continue;
            g_string_append_c(out, ' ');
            g_string_append(out, a->name);
            g_string_append(out, "=\"");
            ns_html_escape_append(out, a->value, TRUE);
            g_string_append_c(out, '"');
        }
        g_string_append_c(out, '>');
        if (is_void_tag(n->name)) return;
        if (serialize_pre_like(n->name) && n->first_child &&
            n->first_child->kind == NS_NODE_TEXT && n->first_child->text &&
            n->first_child->text[0] == '\n')
            g_string_append_c(out, '\n');
    }
    if (n->tpl_content)
        for (const ns_node *c = n->tpl_content->first_child; c;
             c = c->next_sibling)
            serialize_node_opts(c, out, TRUE, depth + 1, opts);
    const ns_node *shadow = ns_serialize_shadow_child(n);
    if (shadow && ns_shadow_root_included(shadow, opts))
        serialize_shadow_template(shadow, out, depth, opts);
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        /* A frame's document hangs under its <iframe>; it is never part
         * of the host's serialization. */
        if (c == shadow || ns_node_is_embedded_document(c))
            continue;
        if (raw_text && c->kind == NS_NODE_TEXT)
            g_string_append(out, c->text ? c->text : "");
        else
            serialize_node_opts(c, out, TRUE, depth + 1, opts);
    }
    if (n->kind == NS_NODE_ELEMENT && include_self) {
        g_string_append(out, "</");
        g_string_append(out, n->name ? n->name : "");
        g_string_append_c(out, '>');
    }
}

static void
serialize_node(const ns_node *n, GString *out, gboolean include_self, int depth)
{
    serialize_node_opts(n, out, include_self, depth, NULL);
}

/* innerHTML serialization; uses template content and omits shadow roots; caller frees. */
char *
ns_node_inner_html(const ns_node *root)
{
    if (root && root->kind == NS_NODE_ELEMENT && is_void_tag(root->name))
        return g_strdup("");
    GString *out = g_string_new(NULL);
    gboolean raw_text = serialize_raw_text(root);
    if (root && root->tpl_content)
        root = root->tpl_content;
    const ns_node *shadow = ns_serialize_shadow_child(root);
    if (root)
        for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
            if (c == shadow || ns_node_is_embedded_document(c)) continue;
            if (raw_text && c->kind == NS_NODE_TEXT)
                g_string_append(out, c->text ? c->text : "");
            else
                serialize_node(c, out, TRUE, 0);
        }
    return g_string_free(out, FALSE);
}

/* getHTML(): inner serialization that may include shadow roots per opts; caller frees. */
char *
ns_node_get_html(const ns_node *root, const ns_html_ser_opts *opts)
{
    if (root && root->kind == NS_NODE_ELEMENT && is_void_tag(root->name))
        return g_strdup("");
    GString *out = g_string_new(NULL);
    gboolean raw_text = serialize_raw_text(root);
    if (root && root->tpl_content)
        root = root->tpl_content;
    const ns_node *shadow = ns_serialize_shadow_child(root);
    if (shadow && ns_shadow_root_included(shadow, opts))
        serialize_shadow_template(shadow, out, 0, opts);
    if (root)
        for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
            if (c == shadow) continue;
            if (raw_text && c->kind == NS_NODE_TEXT)
                g_string_append(out, c->text ? c->text : "");
            else
                serialize_node_opts(c, out, TRUE, 0, opts);
        }
    return g_string_free(out, FALSE);
}

/* outerHTML serialization of a node; caller frees. */
char *
ns_node_outer_html(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    if (node) serialize_node(node, out, TRUE, 0);
    return g_string_free(out, FALSE);
}

#define NS_HTML_NAMESPACE "http://www.w3.org/1999/xhtml"
#define NS_XML_NAMESPACE "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS_NAMESPACE "http://www.w3.org/2000/xmlns/"

/* Namespace URI of an element: stored URI, SVG, empty for foreign/XML, else XHTML. */
static const char *
ns_xml_element_ns(const ns_node *n)
{
    const char *stored = ns_element_get_attr(n, "data-nd-ns-uri");
    if (stored) return stored;
    if (n->flags & NS_NODE_SVG_NS) return "http://www.w3.org/2000/svg";
    if (n->flags & NS_NODE_FOREIGN_NS) {
        return "";
    }
    if (n->flags & NS_NODE_XML_DOC) return "";
    return NS_HTML_NAMESPACE;
}

/* Escapes text for XML; in attributes also quotes and tab/newline/CR. */
static void
ns_xml_escape_append(GString *out, const char *s, gboolean attr)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '&': g_string_append(out, "&amp;"); break;
        case '<': g_string_append(out, "&lt;"); break;
        case '>': g_string_append(out, "&gt;"); break;
        case '"':
            if (attr) g_string_append(out, "&quot;");
            else g_string_append_c(out, '"');
            break;
        case '\t':
            if (attr) g_string_append(out, "&#9;");
            else g_string_append_c(out, '\t');
            break;
        case '\n':
            if (attr) g_string_append(out, "&#10;");
            else g_string_append_c(out, '\n');
            break;
        case '\r':
            if (attr) g_string_append(out, "&#13;");
            else g_string_append_c(out, '\r');
            break;
        default:  g_string_append_c(out, *s);
        }
    }
}

static gboolean
xml_attr_is_namespace_decl(const ns_attr *a)
{
    return a && a->namespace_uri &&
           strcmp(a->namespace_uri, NS_XMLNS_NAMESPACE) == 0;
}

/* Namespace bound to prefix (NULL for default) by xmlns attributes on n itself. */
static const char *
xml_declared_namespace_on(const ns_node *n, const char *prefix)
{
    const char *value = NULL;
    for (const ns_attr *a = n ? n->attrs : NULL; a; a = a->next) {
        if (!xml_attr_is_namespace_decl(a)) continue;
        if (!prefix && !a->prefix && a->local_name &&
            strcmp(a->local_name, "xmlns") == 0)
            value = a->value;
        if (prefix && a->prefix && strcmp(a->prefix, "xmlns") == 0 &&
            a->local_name && strcmp(a->local_name, prefix) == 0)
            value = a->value;
    }
    return value;
}

/* Resolves a prefix through ancestor xmlns declarations; xml maps to its namespace. */
static const char *
xml_declared_namespace(const ns_node *n, const char *prefix)
{
    for (const ns_node *cur = n; cur; cur = cur->parent) {
        if (cur->kind != NS_NODE_ELEMENT) continue;
        const char *value = xml_declared_namespace_on(cur, prefix);
        if (value) return value;
    }
    return prefix && strcmp(prefix, "xml") == 0 ? NS_XML_NAMESPACE : NULL;
}

/* Finds the prefix bound to uri on the nearest declaring ancestor; caller frees. */
static char *
xml_prefix_for_namespace(const ns_node *n, const char *uri)
{
    for (const ns_node *cur = n; cur; cur = cur->parent) {
        if (cur->kind != NS_NODE_ELEMENT) continue;
        char *found = NULL;
        for (const ns_attr *a = cur->attrs; a; a = a->next) {
            if (!xml_attr_is_namespace_decl(a) || !a->prefix ||
                strcmp(a->prefix, "xmlns") != 0 || !a->local_name ||
                !a->value || strcmp(a->value, uri) != 0)
                continue;
            g_free(found);
            found = g_strdup(a->local_name);
        }
        if (found) return found;
    }
    return NULL;
}

/* True if n or any ancestor carries an xmlns declaration. */
static gboolean
xml_has_namespace_declaration(const ns_node *n)
{
    for (const ns_node *cur = n; cur; cur = cur->parent)
        if (cur->kind == NS_NODE_ELEMENT)
            for (const ns_attr *a = cur->attrs; a; a = a->next)
                if (xml_attr_is_namespace_decl(a)) return TRUE;
    return FALSE;
}

/* True if n declares an xmlns:prefix with an empty value. */
static gboolean
xml_has_empty_prefixed_declaration(const ns_node *n)
{
    for (const ns_attr *a = n ? n->attrs : NULL; a; a = a->next)
        if (xml_attr_is_namespace_decl(a) && a->prefix &&
            strcmp(a->prefix, "xmlns") == 0 && a->value && !*a->value)
            return TRUE;
    return FALSE;
}

/* Element prefix from stored data or the qualified name; caller frees, may be NULL. */
static char *
xml_element_prefix(const ns_node *n)
{
    const char *stored = ns_element_get_attr(n, "data-nd-ns-prefix");
    if (stored && *stored) return g_strdup(stored);
    if (n && n->name) {
        const char *colon = strchr(n->name, ':');
        if (colon) return g_strndup(n->name, (gsize)(colon - n->name));
    }
    return NULL;
}

static const char *
xml_local_name(const char *name)
{
    const char *colon = name ? strchr(name, ':') : NULL;
    return colon ? colon + 1 : (name ? name : "");
}

/* Generates the next nsN prefix for namespace fixup; caller frees. */
static char *
xml_generated_prefix(guint *index)
{
    return g_strdup_printf("ns%u", (*index)++);
}

/* XML serialization with namespace fixup, emitting xmlns declarations as needed. */
static void
xml_serialize_node(const ns_node *n, GString *out, const char *parent_ns,
                   guint *prefix_index, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        if (n->flags & NS_NODE_CDATA) {
            g_string_append(out, "<![CDATA[");
            g_string_append(out, n->text ? n->text : "");
            g_string_append(out, "]]>");
        } else {
            ns_xml_escape_append(out, n->text, FALSE);
        }
        return;
    }
    if (n->kind == NS_NODE_COMMENT) {
        if (n->flags & NS_NODE_PI) {
            g_string_append(out, "<?");
            g_string_append(out, n->name ? n->name : "");
            if (n->text && *n->text) {
                g_string_append_c(out, ' ');
                g_string_append(out, n->text);
            }
            g_string_append(out, "?>");
        } else {
            g_string_append(out, "<!--");
            g_string_append(out, n->text ? n->text : "");
            g_string_append(out, "-->");
        }
        return;
    }
    if (n->kind == NS_NODE_DOCTYPE) {
        serialize_doctype(n, out);
        return;
    }
    if (n->kind == NS_NODE_DOCUMENT) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (!ns_node_is_embedded_document(c))
                xml_serialize_node(c, out, NULL, prefix_index, depth + 1);
        return;
    }
    if (n->kind != NS_NODE_ELEMENT) return;

    const char *ns = ns_xml_element_ns(n);
    const char *inherited_default = parent_ns ? parent_ns : "";
    const char *own_default = xml_declared_namespace_on(n, NULL);
    const char *effective_default = own_default
        ? own_default : inherited_default;
    char *prefix = xml_element_prefix(n);
    gboolean generated_prefix = FALSE;
    if (strcmp(ns, NS_XML_NAMESPACE) == 0) {
        g_free(prefix);
        prefix = g_strdup("xml");
    } else if (prefix && strcmp(effective_default, ns) == 0) {
        g_free(prefix);
        prefix = NULL;
    } else if (prefix) {
        const char *own_binding = xml_declared_namespace_on(n, prefix);
        if (own_binding && strcmp(own_binding, ns) != 0) {
            g_free(prefix);
            prefix = xml_generated_prefix(prefix_index);
            generated_prefix = TRUE;
        }
    } else if (*ns) {
        prefix = xml_prefix_for_namespace(n, ns);
    }
    const char *local = xml_local_name(n->name);
    g_string_append_c(out, '<');
    if (prefix) {
        g_string_append(out, prefix);
        g_string_append_c(out, ':');
    }
    g_string_append(out, local);
    gboolean emit_element_namespace = FALSE;
    if (prefix && strcmp(prefix, "xml") != 0) {
        const char *binding = xml_declared_namespace(n, prefix);
        emit_element_namespace = generated_prefix || !binding ||
                                 strcmp(binding, ns) != 0;
    } else if (!prefix) {
        emit_element_namespace = strcmp(ns, inherited_default) != 0;
    }
    if (emit_element_namespace) {
        g_string_append(out, prefix ? " xmlns:" : " xmlns");
        if (prefix) g_string_append(out, prefix);
        g_string_append(out, "=\"");
        ns_xml_escape_append(out, ns, TRUE);
        g_string_append_c(out, '"');
    }
    for (const ns_attr *a = n->attrs; a; a = a->next) {
        if (ns_attr_name_is_internal(a->name)) continue;
        if (xml_attr_is_namespace_decl(a)) {
            if (!a->prefix &&
                (emit_element_namespace ||
                 (!prefix && !xml_has_empty_prefixed_declaration(n))))
                continue;
            g_string_append_c(out, ' ');
            g_string_append(out, a->name);
            g_string_append(out, "=\"");
            ns_xml_escape_append(out, a->value, TRUE);
            g_string_append_c(out, '"');
            continue;
        }
        if (strcmp(a->name, "xmlns") == 0 ||
            g_str_has_prefix(a->name, "xmlns:"))
            continue;
        char *attr_prefix = NULL;
        gboolean declare_attr_prefix = FALSE;
        if (a->namespace_uri && *a->namespace_uri) {
            if (strcmp(a->namespace_uri, NS_XML_NAMESPACE) == 0) {
                attr_prefix = g_strdup("xml");
            } else {
                attr_prefix = xml_prefix_for_namespace(n, a->namespace_uri);
                if (!attr_prefix && a->prefix && *a->prefix) {
                    const char *binding = xml_declared_namespace(n, a->prefix);
                    if (binding && strcmp(binding, a->namespace_uri) == 0) {
                        attr_prefix = g_strdup(a->prefix);
                    } else if (!binding &&
                               !xml_has_namespace_declaration(n)) {
                        attr_prefix = g_strdup(a->prefix);
                        declare_attr_prefix = TRUE;
                    }
                }
                if (!attr_prefix) {
                    attr_prefix = xml_generated_prefix(prefix_index);
                    declare_attr_prefix = TRUE;
                }
            }
        }
        if (declare_attr_prefix) {
            g_string_append(out, " xmlns:");
            g_string_append(out, attr_prefix);
            g_string_append(out, "=\"");
            ns_xml_escape_append(out, a->namespace_uri, TRUE);
            g_string_append_c(out, '"');
        }
        g_string_append_c(out, ' ');
        if (attr_prefix) {
            g_string_append(out, attr_prefix);
            g_string_append_c(out, ':');
        }
        g_string_append(out, a->local_name
            ? a->local_name : xml_local_name(a->name));
        g_string_append(out, "=\"");
        ns_xml_escape_append(out, a->value, TRUE);
        g_string_append_c(out, '"');
        g_free(attr_prefix);
    }
    const char *child_default = prefix
        ? (own_default ? own_default : inherited_default) : ns;
    gboolean html = strcmp(ns, NS_HTML_NAMESPACE) == 0;
    if (!n->first_child) {
        if (html && !is_void_tag(local)) {
            g_string_append(out, "></");
            if (prefix) {
                g_string_append(out, prefix);
                g_string_append_c(out, ':');
            }
            g_string_append(out, local);
            g_string_append_c(out, '>');
        } else {
            g_string_append(out, "/>");
        }
        g_free(prefix);
        return;
    }
    g_string_append_c(out, '>');
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        if (!ns_node_is_embedded_document(c))
            xml_serialize_node(c, out, child_default, prefix_index, depth + 1);
    g_string_append(out, "</");
    if (prefix) {
        g_string_append(out, prefix);
        g_string_append_c(out, ':');
    }
    g_string_append(out, local);
    g_string_append_c(out, '>');
    g_free(prefix);
}

/* XML serialization of a node and its subtree; caller frees. */
char *
ns_node_xml_outer_html(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    guint prefix_index = 1;
    if (node) xml_serialize_node(node, out, NULL, &prefix_index, 0);
    return g_string_free(out, FALSE);
}

/* XML serialization of a node's children in its default namespace; caller frees. */
char *
ns_node_xml_inner_html(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    guint prefix_index = 1;
    const char *default_ns = node && node->kind == NS_NODE_ELEMENT
        ? ns_xml_element_ns(node) : NULL;
    for (const ns_node *child = node ? node->first_child : NULL;
         child; child = child->next_sibling)
        if (!ns_node_is_embedded_document(child))
            xml_serialize_node(child, out, default_ns, &prefix_index, 0);
    return g_string_free(out, FALSE);
}

/* Appends text with C-style escapes, truncated to max bytes with an ellipsis. */
static void
ns_dump_text(GString *out, const char *s, gsize max)
{
    if (!s) return;
    gsize len = strlen(s);
    gboolean truncated = (max > 0 && len > max);
    if (truncated) len = max;
    for (gsize i = 0; i < len; i++) {
        guchar c = (guchar)s[i];
        if (c == '\\')      g_string_append(out, "\\\\");
        else if (c == '\n') g_string_append(out, "\\n");
        else if (c == '\r') g_string_append(out, "\\r");
        else if (c == '\t') g_string_append(out, "\\t");
        else if (c < 0x20)  g_string_append_printf(out, "\\x%02x", c);
        else                g_string_append_c(out, (char)c);
    }
    if (truncated)
        g_string_append(out, "…");
}

/* Appends an indented debug dump of a subtree, one node per line. */
static void
ns_dump_node(GString *out, const ns_node *n, int depth)
{
    if (depth >= NS_DOM_MAX_DEPTH) return;
    for (int i = 0; i < depth; i++)
        g_string_append(out, "  ");

    switch (n->kind) {
    case NS_NODE_DOCUMENT:
        g_string_append(out, "#document\n");
        break;
    case NS_NODE_DOCTYPE:
        g_string_append_printf(out, "<!DOCTYPE %s", n->name ? n->name : "");
        if (n->public_id && *n->public_id)
            g_string_append_printf(out, " PUBLIC \"%s\"", n->public_id);
        if (n->system_id && *n->system_id)
            g_string_append_printf(out, "%s\"%s\"",
                                   n->public_id && *n->public_id ? " " : " SYSTEM ",
                                   n->system_id);
        g_string_append(out, ">\n");
        break;
    case NS_NODE_ELEMENT:
        g_string_append_printf(out, "<%s", n->name ? n->name : "?");
        for (const ns_attr *a = n->attrs; a; a = a->next) {
            g_string_append_printf(out, " %s=\"", a->name);
            ns_dump_text(out, a->value, 0);
            g_string_append_c(out, '"');
        }
        g_string_append(out, ">\n");
        break;
    case NS_NODE_TEXT:
        g_string_append(out, "\"");
        ns_dump_text(out, n->text, 120);
        g_string_append(out, "\"\n");
        break;
    case NS_NODE_COMMENT:
        g_string_append(out, "<!--");
        ns_dump_text(out, n->text, 120);
        g_string_append(out, "-->\n");
        break;
    }

    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_dump_node(out, c, depth + 1);
}

/* Returns a debug tree dump of node; caller frees the GString. */
GString *
ns_node_dump(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    if (node)
        ns_dump_node(out, node, 0);
    return out;
}

/* Parses comma/space-separated area coords into out; returns count parsed. */
static int
ns_map_parse_coords(const char *s, double *out, int max)
{
    int n = 0;
    const char *p = s;
    while (p && *p && n < max) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p) break;
        char *e = NULL;
        double v = g_ascii_strtod(p, &e);
        if (e == p) break;
        out[n++] = v;
        p = e;
    }
    return n;
}

/* Even-odd ray casting test for a point inside a polygon of npairs vertices. */
static gboolean
ns_map_point_in_poly(const double *pts, int npairs, double x, double y)
{
    gboolean in = FALSE;
    for (int i = 0, j = npairs - 1; i < npairs; j = i++) {
        double xi = pts[2 * i], yi = pts[2 * i + 1];
        double xj = pts[2 * j], yj = pts[2 * j + 1];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

/* Hit-tests an area shape (rect, circle, poly, default) against image-local coords. */
static gboolean
ns_area_hit(const char *shape, const char *coords, double lx, double ly,
            double iw, double ih)
{
    gboolean is_circle = shape && g_ascii_strncasecmp(shape, "circ", 4) == 0;
    gboolean is_poly   = shape && g_ascii_strncasecmp(shape, "poly", 4) == 0;
    gboolean is_default = shape && g_ascii_strcasecmp(shape, "default") == 0;
    if (is_default)
        return lx >= 0 && ly >= 0 && lx <= iw && ly <= ih;
    double c[64];
    int n = coords ? ns_map_parse_coords(coords, c, 64) : 0;
    if (is_circle) {
        if (n < 3) return FALSE;
        double dx = lx - c[0], dy = ly - c[1];
        return dx * dx + dy * dy <= c[2] * c[2];
    }
    if (is_poly) {
        if (n < 6) return FALSE;
        return ns_map_point_in_poly(c, n / 2, lx, ly);
    }
    if (n < 4) return FALSE;
    double x1 = MIN(c[0], c[2]), x2 = MAX(c[0], c[2]);
    double y1 = MIN(c[1], c[3]), y2 = MAX(c[1], c[3]);
    return lx >= x1 && lx <= x2 && ly >= y1 && ly <= y2;
}

/* Finds a map element whose name or id matches. */
static const ns_node *
ns_find_map(const ns_node *n, const char *name, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (ns_node_is_element_named(n, "map")) {
        const char *mn = ns_element_get_attr(n, "name");
        const char *mid = ns_element_get_attr(n, "id");
        if ((mn && strcmp(mn, name) == 0) || (mid && strcmp(mid, name) == 0))
            return n;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *r = ns_find_map(c, name, depth + 1);
        if (r) return r;
    }
    return NULL;
}

/* First area descendant of a map whose shape contains the point. */
static const ns_node *
ns_map_first_area(const ns_node *n, double lx, double ly, double iw, double ih,
                  int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return NULL;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "area") &&
            ns_area_hit(ns_element_get_attr(c, "shape"),
                        ns_element_get_attr(c, "coords"), lx, ly, iw, ih))
            return c;
        const ns_node *r = ns_map_first_area(c, lx, ly, iw, ih, depth + 1);
        if (r) return r;
    }
    return NULL;
}

/* Resolves a usemap click to the hit area's href (and target); caller frees. */
char *
ns_image_map_resolve(const ns_node *doc, const char *usemap,
                     double lx, double ly, double iw, double ih,
                     const char **out_target)
{
    if (out_target) *out_target = NULL;
    if (!doc || !usemap || lx < 0 || ly < 0) return NULL;
    const char *name = usemap[0] == '#' ? usemap + 1 : usemap;
    if (!*name) return NULL;
    const ns_node *map = ns_find_map(doc, name, 0);
    if (!map) return NULL;
    const ns_node *area = ns_map_first_area(map, lx, ly, iw, ih, 0);
    if (!area) return NULL;
    const char *href = ns_element_get_attr(area, "href");
    if (!href || !*href) return NULL;
    if (out_target) *out_target = ns_element_get_attr(area, "target");
    return g_strdup(href);
}
