/* Northstar Web Browser — UI string translation from data/i18n .lang catalogues.
 * Loads a catalogue for every language in the OS language list, in priority order, and
 * falls back down that chain when a higher-priority catalogue lacks a translation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "i18n.h"

#include <string.h>

static GPtrArray *g_catalogues;   /* GHashTable *, highest priority first */
static char      *g_language;
static char      *g_html_lang;

/* Returns dir/<lang>.lang if it is a regular file; caller frees. */
static char *
catalogue_path_in(const char *dir, const char *lang)
{
    char *file = g_strdup_printf("%s.lang", lang);
    char *path = g_build_filename(dir, file, NULL);
    g_free(file);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) return path;
    g_free(path);
    return NULL;
}

/* Locates a language catalogue via NS_I18N_DIR, paths near the executable, or data/i18n. */
static char *
find_catalogue(const char *self_exe, const char *lang)
{
    const char *override = g_getenv("NS_I18N_DIR");
    if (override && *override) return catalogue_path_in(override, lang);

    static const char *const rel[] = {
        "../Resources/share/northstar/i18n",
        "../share/northstar/i18n",
        "share/northstar/i18n",
        "data/i18n",
        "../data/i18n",
        "../../data/i18n",
        "../../../data/i18n",
        "../../../../data/i18n",
    };
    if (self_exe) {
        char *exe_dir = g_path_get_dirname(self_exe);
        for (gsize i = 0; i < G_N_ELEMENTS(rel); i++) {
            char *dir = g_build_filename(exe_dir, rel[i], NULL);
            char *path = catalogue_path_in(dir, lang);
            g_free(dir);
            if (path) {
                g_free(exe_dir);
                return path;
            }
        }
        g_free(exe_dir);
    }
    return catalogue_path_in("data/i18n", lang);
}

/* Parses key=value lines into a translation table; NULL if empty or unreadable. */
static GHashTable *
catalogue_load(const char *path)
{
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return NULL;
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        char *line = lines[i];
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        char *key = g_strndup(line, (gsize)(eq - line));
        char *value = g_strdup(eq + 1);
        g_strchomp(value);
        if (*value)
            g_hash_table_replace(table, key, value);
        else {
            g_free(key);
            g_free(value);
        }
    }
    g_strfreev(lines);
    g_free(contents);
    if (g_hash_table_size(table) == 0) {
        g_hash_table_destroy(table);
        return NULL;
    }
    return table;
}

/* True for English tags: the UI source strings are English, so the chain ends there. */
static gboolean
is_source_language(const char *lang)
{
    return strcmp(lang, "en") == 0 || g_str_has_prefix(lang, "en_");
}

/* Loads a catalogue for each usable entry of the OS language list, skipping C/POSIX and
 * codeset/modifier variants; stops at English, the source language. */
void
ns_i18n_init(const char *self_exe)
{
    if (g_catalogues) return;
    g_catalogues = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_hash_table_destroy);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    const gchar *const *names = g_get_language_names();
    for (int i = 0; names && names[i]; i++) {
        const char *lang = names[i];
        if (strcmp(lang, "C") == 0 || strcmp(lang, "POSIX") == 0)
            continue;
        if (strchr(lang, '.') || strchr(lang, '@'))
            continue;
        if (!g_hash_table_add(seen, (gpointer)lang)) continue;
        if (is_source_language(lang)) break;
        char *path = find_catalogue(self_exe, lang);
        if (!path) continue;
        GHashTable *table = catalogue_load(path);
        g_free(path);
        if (!table) continue;
        g_ptr_array_add(g_catalogues, table);
        if (!g_language) g_language = g_strdup(lang);
    }
    g_hash_table_destroy(seen);
}

/* Returns the translation from the highest-priority catalogue that has one, or the string itself. */
const char *
ns_i18n(const char *text)
{
    if (!text || !g_catalogues) return text;
    for (guint i = 0; i < g_catalogues->len; i++) {
        const char *hit = g_hash_table_lookup(g_ptr_array_index(g_catalogues, i), text);
        if (hit) return hit;
    }
    return text;
}

const char *
ns_i18n_language(void)
{
    return g_language;
}

/* BCP 47 tag of the UI language for an HTML lang attribute ("en" when untranslated). */
const char *
ns_i18n_html_lang(void)
{
    if (!g_language) return "en";
    if (!g_html_lang) {
        g_html_lang = g_strdup(g_language);
        for (char *p = g_html_lang; *p; p++)
            if (*p == '_') *p = '-';
    }
    return g_html_lang;
}

/* "rtl" when the UI language is written right to left, else "ltr". */
const char *
ns_i18n_html_dir(void)
{
    static const char *const rtl[] = {
        "ar", "ckb", "dv", "fa", "he", "ps", "sd", "ug", "ur", "yi",
    };
    if (!g_language) return "ltr";
    gsize n = strcspn(g_language, "_-");
    for (gsize i = 0; i < G_N_ELEMENTS(rtl); i++)
        if (strlen(rtl[i]) == n && strncmp(g_language, rtl[i], n) == 0)
            return "rtl";
    return "ltr";
}

/* Appends s as the body of a JavaScript string literal, \u-escaping quotes, backslash,
 * HTML-significant characters and controls. */
static void
append_js_escaped(GString *out, const char *s)
{
    for (; *s; s++) {
        guchar c = (guchar)*s;
        if (c < 0x20 || c == '\'' || c == '"' || c == '\\' ||
            c == '<' || c == '>' || c == '&')
            g_string_append_printf(out, "\\u%04x", c);
        else
            g_string_append_c(out, (char)c);
    }
}

/* Expands translation markers in a built-in page template; caller frees.
 * {{Text}} becomes the HTML-escaped translation of Text, {{js:Text}} the same for use
 * inside a JavaScript string literal, and {{@lang}} / {{@dir}} the UI language's
 * lang and dir attribute values. */
char *
ns_i18n_expand(const char *tmpl)
{
    GString *out = g_string_sized_new(strlen(tmpl) + 256);
    const char *p = tmpl;
    for (;;) {
        const char *open = strstr(p, "{{");
        const char *close = open ? strstr(open + 2, "}}") : NULL;
        if (!close) {
            g_string_append(out, p);
            break;
        }
        g_string_append_len(out, p, (gssize)(open - p));
        char *key = g_strndup(open + 2, (gsize)(close - open - 2));
        if (strcmp(key, "@lang") == 0) {
            g_string_append(out, ns_i18n_html_lang());
        } else if (strcmp(key, "@dir") == 0) {
            g_string_append(out, ns_i18n_html_dir());
        } else if (g_str_has_prefix(key, "js:")) {
            append_js_escaped(out, ns_i18n(key + 3));
        } else {
            char *esc = g_markup_escape_text(ns_i18n(key), -1);
            g_string_append(out, esc);
            g_free(esc);
        }
        g_free(key);
        p = close + 2;
    }
    return g_string_free(out, FALSE);
}
