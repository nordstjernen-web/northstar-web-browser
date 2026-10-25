/* Northstar Web Browser — HTML forms: submit/reset triggers, form data set, validation.
 * Builds the urlencoded data set; checks required, pattern, type, min/max and maxlength.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "forms.h"
#include "dom.h"
#include "net.h"

#define NS_FORM_MAX_DEPTH 512
#define NS_PATTERN_MAX_LEN 2048
#define NS_PATTERN_VALUE_MAX_LEN 10000

/* True for elements that submit their form when activated (submit buttons, image inputs). */
gboolean
ns_form_is_submit_trigger(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "button") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        return !type || g_ascii_strcasecmp(type, "submit") == 0;
    }
    if (strcmp(n->name, "input") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        return type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                        g_ascii_strcasecmp(type, "image") == 0);
    }
    return FALSE;
}

/* True for button or input elements of type reset. */
gboolean
ns_form_is_reset_trigger(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    if (!type || g_ascii_strcasecmp(type, "reset") != 0) return FALSE;
    return strcmp(n->name, "button") == 0 ||
           strcmp(n->name, "input") == 0;
}

/* True if control's form owner is form. */
static gboolean
form_control_belongs_to(const ns_node *form, const ns_node *control,
                        const ns_node *doc)
{
    return form && control && ns_form_owner(control, doc) == form;
}

/* True if an option is disabled itself or by an ancestor optgroup within its select. */
static gboolean
form_option_disabled(const ns_node *option)
{
    if (ns_element_effectively_disabled(option)) return TRUE;
    int depth = 0;
    for (const ns_node *p = option ? option->parent : NULL; p && depth++ < NS_FORM_MAX_DEPTH; p = p->parent) {
        if (ns_node_is_element_named(p, "select")) return FALSE;
        if (ns_node_is_element_named(p, "optgroup") &&
            ns_element_get_attr(p, "disabled"))
            return TRUE;
    }
    return FALSE;
}

/* Enabled selected options of a select that contribute to the form data set. */
static GPtrArray *
form_selected_options(const ns_node *select)
{
    GPtrArray *out = g_ptr_array_new();
    if (!select) return out;
    if (!ns_element_get_attr(select, "multiple")) {
        const ns_node *opt = ns_select_chosen_option(select);
        if (opt && !form_option_disabled(opt))
            g_ptr_array_add(out, (gpointer)opt);
        return out;
    }
    GPtrArray *opts = g_ptr_array_new();
    ns_select_collect_options(select, opts);
    for (guint i = 0; i < opts->len; i++) {
        const ns_node *o = g_ptr_array_index(opts, i);
        if (ns_option_is_selected(o) && !form_option_disabled(o))
            g_ptr_array_add(out, (gpointer)o);
    }
    g_ptr_array_free(opts, TRUE);
    return out;
}

/* Builds the urlencoded form data set from controls owned by form, including the submitter. */
static void
ns_form_collect_inputs_depth(const ns_node *form, const ns_node *n,
                    const ns_node *doc, GString *query, gboolean *first,
                    const ns_node *submitter, int depth)
{
    if (!n || depth >= NS_FORM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT && n->name) {
        gboolean is_input    = strcmp(n->name, "input") == 0;
        gboolean is_textarea = strcmp(n->name, "textarea") == 0;
        gboolean is_select   = strcmp(n->name, "select") == 0;
        gboolean is_button   = strcmp(n->name, "button") == 0;
        if (is_input || is_textarea || is_select || is_button) {
            if (!form_control_belongs_to(form, n, doc)) goto recurse;
            const char *name = ns_element_get_attr(n, "name");
            if (!name || !*name) goto recurse;
            if (ns_element_effectively_disabled(n)) goto recurse;
            if (is_input) {
                const char *type = ns_element_get_attr(n, "type");
                if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                             g_ascii_strcasecmp(type, "radio") == 0)) {
                    if (!ns_input_is_checked(n)) goto recurse;
                }
                if (type && g_ascii_strcasecmp(type, "submit") == 0) {
                    if (n == submitter) {
                        const char *v = ns_element_get_attr(n, "value");
                        ns_form_urlencoded_append_pair(query, first, name, v ? v : "");
                    }
                    goto recurse;
                }
                if (type && g_ascii_strcasecmp(type, "image") == 0) {
                    if (n == submitter) {
                        char *xname = g_strconcat(name, ".x", NULL);
                        char *yname = g_strconcat(name, ".y", NULL);
                        ns_form_urlencoded_append_pair(query, first, xname, "0");
                        ns_form_urlencoded_append_pair(query, first, yname, "0");
                        g_free(xname);
                        g_free(yname);
                    }
                    goto recurse;
                }
                if (type && (g_ascii_strcasecmp(type, "button") == 0 ||
                             g_ascii_strcasecmp(type, "reset")  == 0 ||
                             g_ascii_strcasecmp(type, "file")   == 0))
                    goto recurse;
                const char *value = ns_input_used_value(n);
                if (!value && type &&
                    (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                     g_ascii_strcasecmp(type, "radio") == 0))
                    value = "on";
                ns_form_urlencoded_append_pair(query, first, name, value);
            } else if (is_textarea) {
                char *text = ns_textarea_value_dup(n);
                ns_form_urlencoded_append_pair(query, first, name, text ? text : "");
                g_free(text);
            } else if (is_select) {
                GPtrArray *opts = form_selected_options(n);
                for (guint i = 0; i < opts->len; i++) {
                    char *v = ns_option_value_dup(g_ptr_array_index(opts, i));
                    ns_form_urlencoded_append_pair(query, first, name, v ? v : "");
                    g_free(v);
                }
                g_ptr_array_free(opts, TRUE);
                goto recurse;
            } else if (is_button) {
                const char *type = ns_element_get_attr(n, "type");
                gboolean acts_as_submit = !type || g_ascii_strcasecmp(type, "submit") == 0;
                if (acts_as_submit && n == submitter) {
                    const char *v = ns_element_get_attr(n, "value");
                    ns_form_urlencoded_append_pair(query, first, name, v ? v : "");
                }
                goto recurse;
            }
        }
    }
recurse:
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_form_collect_inputs_depth(form, c, doc, query, first, submitter,
                                     depth + 1);
}

void
ns_form_collect_inputs(const ns_node *form, const ns_node *n, const ns_node *doc,
                    GString *query, gboolean *first, const ns_node *submitter)
{
    ns_form_collect_inputs_depth(form, n, doc, query, first, submitter, 0);
}

/* Tests a value against an anchored pattern attribute; invalid regexes match. */
static gboolean
ns_value_matches_pattern(const char *value, const char *pattern)
{
    if (!pattern || !*pattern) return TRUE;
    if (strlen(pattern) > NS_PATTERN_MAX_LEN) return FALSE;
    if (value && strlen(value) > NS_PATTERN_VALUE_MAX_LEN) return FALSE;
    char *anchored = g_strdup_printf("^(?:%s)$", pattern);
    GError *err = NULL;
    GRegex *re = g_regex_new(anchored, 0, 0, &err);
    g_free(anchored);
    if (!re) { g_clear_error(&err); return TRUE; }
    gboolean ok = g_regex_match(re, value ? value : "", 0, NULL);
    g_regex_unref(re);
    return ok;
}

/* typeMismatch/badInput check for email, url and numeric input types. */
static gboolean
ns_value_matches_type(const ns_node *n, const char *value, const char *type)
{
    if (!value || !*value || !type) return TRUE;
    if (g_ascii_strcasecmp(type, "email") == 0)
        return ns_input_email_value_valid(n, value);
    if (g_ascii_strcasecmp(type, "url") == 0)
        return ns_url_is_valid_absolute(value);
    if (ns_input_type_has_number_value(type))
        return ns_input_value_to_number(type, value, NULL);
    return TRUE;
}

/* True unless the value underflows min or overflows max. */
static gboolean
ns_value_matches_range(const ns_node *n, const char *value)
{
    gboolean under = FALSE, over = FALSE;
    if (!ns_input_value_range_state(n, value, &under, &over)) return TRUE;
    return !under && !over;
}

static gboolean
ns_value_matches_step(const ns_node *n, const char *value)
{
    return !ns_input_value_step_mismatch(n, value);
}

/* Finds the first control owned by form that fails constraint validation. */
static const ns_node *
ns_form_first_invalid_depth(const ns_node *form, const ns_node *n,
                            const ns_node *doc, int depth)
{
    if (!n || depth >= NS_FORM_MAX_DEPTH) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name) {
        gboolean is_input    = strcmp(n->name, "input") == 0;
        gboolean is_textarea = strcmp(n->name, "textarea") == 0;
        gboolean is_select   = strcmp(n->name, "select") == 0;
        if (is_input || is_textarea || is_select) {
            if (form_control_belongs_to(form, n, doc) &&
                !ns_element_effectively_disabled(n) &&
                !ns_form_control_readonly_bars_validation(n)) {
                const char *type = is_input ? ns_element_get_attr(n, "type") : NULL;
                gboolean skip = type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                                         g_ascii_strcasecmp(type, "button") == 0 ||
                                         g_ascii_strcasecmp(type, "reset")  == 0 ||
                                         g_ascii_strcasecmp(type, "image")  == 0 ||
                                         g_ascii_strcasecmp(type, "hidden") == 0);
                if (!skip) {
                    const char *custom = ns_element_get_attr(n, NS_CUSTOM_VALIDITY_ATTR);
                    if (custom && *custom) return n;
                    const char *value;
                    char *collected = NULL;
                    if (is_textarea) {
                        collected = ns_textarea_value_dup(n);
                        value = collected ? collected : "";
                    } else if (is_select) {
                        GPtrArray *opts = form_selected_options(n);
                        if (opts->len > 0)
                            collected = ns_option_value_dup(g_ptr_array_index(opts, 0));
                        g_ptr_array_free(opts, TRUE);
                        value = collected ? collected : "";
                    } else {
                        value = ns_input_used_value(n);
                        if (!value) value = "";
                    }
                    gboolean required = ns_form_control_supports_required(n) &&
                                        ns_element_get_attr(n, "required") != NULL;
                    if (required && ns_form_control_value_missing(n, value, doc)) {
                        g_free(collected);
                        return n;
                    }
                    if (*value) {
                        const char *pattern = ns_element_get_attr(n, "pattern");
                        if (is_input &&
                            ns_input_type_supports_text_constraints(type) &&
                            !ns_value_matches_pattern(value, pattern)) {
                            g_free(collected);
                            return n;
                        }
                        if (is_input && !ns_value_matches_type(n, value, type)) {
                            g_free(collected);
                            return n;
                        }
                        if (is_input && !ns_value_matches_range(n, value)) {
                            g_free(collected);
                            return n;
                        }
                        if (is_input && !ns_value_matches_step(n, value)) {
                            g_free(collected);
                            return n;
                        }
                        if (ns_form_control_length_limits_apply(n) &&
                            ns_element_get_attr(n, "data-nd-user-edited")) {
                            const char *minlen = ns_element_get_attr(n, "minlength");
                            const char *maxlen = ns_element_get_attr(n, "maxlength");
                            glong vlen = (glong)g_utf8_strlen(value, -1);
                            if (minlen) {
                                glong mn = (glong)ns_parse_int(minlen, 0, 0, 1000000);
                                if (vlen < mn) { g_free(collected); return n; }
                            }
                            if (maxlen) {
                                glong mx = (glong)ns_parse_int(maxlen, 0, 0, 1000000);
                                if (vlen > mx) { g_free(collected); return n; }
                            }
                        }
                    }
                    g_free(collected);
                }
            }
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *m = ns_form_first_invalid_depth(form, c, doc, depth + 1);
        if (m) return m;
    }
    return NULL;
}

/* First invalid control owned by form in tree order, or NULL. */
const ns_node *
ns_form_first_invalid(const ns_node *form, const ns_node *n, const ns_node *doc)
{
    return ns_form_first_invalid_depth(form, n, doc, 0);
}
