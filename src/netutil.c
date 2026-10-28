/* Northstar Web Browser — curl-free network and URL helpers shared by the engine and GTK shell.
 * Accept-Language, search URLs, about: URL classes, file: URLs, proxy password masking.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "net.h"
#include "config.h"

#include <glib/gstdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

/* Directory containing the running executable (per-OS lookup); caller frees, may be NULL. */
char *
ns_executable_dir(void)
{
#ifdef G_OS_WIN32
    DWORD cap = MAX_PATH;
    wchar_t *buf = g_new(wchar_t, cap);
    DWORD n = GetModuleFileNameW(NULL, buf, cap);
    while (n >= cap && cap < 32768) {
        cap *= 2;
        buf = g_renew(wchar_t, buf, cap);
        n = GetModuleFileNameW(NULL, buf, cap);
    }
    char *utf8 = NULL;
    if (n > 0 && n < cap)
        utf8 = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
    g_free(buf);
    if (!utf8) return NULL;
    char *dir = g_path_get_dirname(utf8);
    g_free(utf8);
    return dir;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    if (size == 0 || size > 32768) return NULL;
    char *raw = g_malloc(size);
    if (_NSGetExecutablePath(raw, &size) != 0) { g_free(raw); return NULL; }
    char *real = realpath(raw, NULL);
    char *dir = g_path_get_dirname(real ? real : raw);
    free(real);
    g_free(raw);
    return dir;
#elif defined(__linux__)
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return NULL;
    char *dir = g_path_get_dirname(exe);
    g_free(exe);
    return dir;
#else
    return NULL;
#endif
}

/* Builds an Accept-Language value with q-weights from the user's locales; caller frees. */
static char *
build_accept_language_from_locales(void)
{
#ifdef G_OS_WIN32
    WCHAR locale_name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        char *locale = g_utf16_to_utf8((const gunichar2 *)locale_name,
                                       -1, NULL, NULL, NULL);
        if (locale && *locale) {
            char *dash = strchr(locale, '-');
            char *base = dash ? g_strndup(locale, dash - locale) : NULL;
            GString *windows_languages = g_string_new(locale);
            if (base && g_ascii_strcasecmp(base, locale) != 0)
                g_string_append_printf(windows_languages, ",%s;q=0.9", base);
            if (base && g_ascii_strcasecmp(base, "nb") == 0)
                g_string_append(windows_languages, ",no;q=0.8,nn;q=0.7");
            else if (base && g_ascii_strcasecmp(base, "nn") == 0)
                g_string_append(windows_languages, ",no;q=0.8,nb;q=0.7");
            else if (base && g_ascii_strcasecmp(base, "no") == 0)
                g_string_append(windows_languages, ",nb;q=0.8,nn;q=0.7");
            if (g_ascii_strncasecmp(locale, "en", 2) != 0)
                g_string_append(windows_languages,
                    base && (g_ascii_strcasecmp(base, "nb") == 0 ||
                             g_ascii_strcasecmp(base, "nn") == 0 ||
                             g_ascii_strcasecmp(base, "no") == 0)
                        ? ",en-US;q=0.6,en;q=0.5"
                        : ",en-US;q=0.8,en;q=0.7");
            g_free(base);
            g_free(locale);
            return g_string_free(windows_languages, FALSE);
        }
        g_free(locale);
    }
#endif
    const char *const *langs = g_get_language_names();
    if (!langs || !langs[0]) return NULL;
    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    int n = 0;
    for (int i = 0; langs[i] && n < 6; i++) {
        char *tag = g_strdup(langs[i]);
        char *dot = strchr(tag, '.');
        if (dot) *dot = '\0';
        char *at = strchr(tag, '@');
        if (at) *at = '\0';
        for (char *p = tag; *p; p++) if (*p == '_') *p = '-';
        if (!*tag ||
            g_ascii_strcasecmp(tag, "C") == 0 ||
            g_ascii_strcasecmp(tag, "POSIX") == 0) {
            g_free(tag); continue;
        }
        char *lower = g_ascii_strdown(tag, -1);
        if (g_hash_table_contains(seen, lower)) {
            g_free(tag); g_free(lower); continue;
        }
        g_hash_table_insert(seen, lower, NULL);
        if (n == 0) {
            g_string_append(out, tag);
        } else {
            double q = 1.0 - (double)n * 0.1;
            if (q < 0.1) q = 0.1;
            g_string_append_printf(out, ",%s;q=%.1f", tag, q);
        }
        n++;
        g_free(tag);
    }
    g_hash_table_destroy(seen);
    if (n == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

/* Locale-derived Accept-Language, computed once; falls back to en-US. */
const char *
ns_net_default_accept_language(void)
{
    static char *cached;
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
        cached = build_accept_language_from_locales();
        if (!cached) cached = g_strdup("en-US,en;q=0.9");
    }
    return cached;
}

/* Configured Accept-Language, or the locale-derived default. */
const char *
ns_net_effective_accept_language(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg && cfg->accept_language && *cfg->accept_language
        ? cfg->accept_language : ns_net_default_accept_language();
}

/* navigator.languages list parsed from Accept-Language, deduplicated; caller frees. */
char **
ns_net_navigator_languages(void)
{
    char **parts = g_strsplit(ns_net_effective_accept_language(), ",", -1);
    GPtrArray *langs = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    for (guint i = 0; parts && parts[i] && langs->len < 20; i++) {
        char *part = g_strstrip(parts[i]);
        char *semi = strchr(part, ';');
        if (semi) *semi = '\0';
        part = g_strstrip(part);
        if (!*part || strcmp(part, "*") == 0) continue;
        gboolean valid = TRUE;
        for (const char *p = part; *p; p++) {
            if (!g_ascii_isalnum(*p) && *p != '-') {
                valid = FALSE;
                break;
            }
        }
        if (!valid) continue;
        char *lower = g_ascii_strdown(part, -1);
        if (g_hash_table_contains(seen, lower)) {
            g_free(lower);
            continue;
        }
        g_hash_table_add(seen, lower);
        g_ptr_array_add(langs, g_strdup(part));
    }
    g_strfreev(parts);
    g_hash_table_destroy(seen);
    if (langs->len == 0)
        g_ptr_array_add(langs, g_strdup("en-US"));
    g_ptr_array_add(langs, NULL);
    return (char **)g_ptr_array_free(langs, FALSE);
}

/* True if address-bar input should be treated as a search query rather than a URL. */
gboolean
ns_address_is_search(const char *s)
{
    if (!s || !*s) return FALSE;
    if (g_str_has_prefix(s, "about:") || g_str_has_prefix(s, "file:") ||
        g_str_has_prefix(s, "data:") || strstr(s, "://"))
        return FALSE;
    for (const char *p = s; *p; p++)
        if (*p == ' ' || *p == '\t')
            return TRUE;
    if (strstr(s, "\xe3\x80\x80"))
        return TRUE;
    if (g_str_has_prefix(s, "localhost") &&
        (s[9] == '\0' || s[9] == ':' || s[9] == '/'))
        return FALSE;
    g_autofree char *local = ns_url_from_local_path(s);
    if (local)
        return FALSE;
    if (strchr(s, '.') || strchr(s, ':'))
        return FALSE;
    return TRUE;
}

/* True if url, ignoring its query and fragment, is one of the NULL-terminated pages. */
static gboolean
about_url_is_one_of(const char *url, const char *const *pages)
{
    if (!url) return FALSE;
    gsize len = strcspn(url, "?#");
    for (gsize i = 0; pages[i]; i++)
        if (len == strlen(pages[i]) &&
            g_ascii_strncasecmp(url, pages[i], len) == 0)
            return TRUE;
    return FALSE;
}

/* True for about: pages web content may open (blank, srcdoc, start/home/newtab). */
gboolean
ns_about_url_is_public(const char *url)
{
    static const char *const public_pages[] = {
        "about:blank", "about:srcdoc", "about:start", "about:home",
        "about:newtab", NULL,
    };
    return about_url_is_one_of(url, public_pages);
}

/* True for the browser's start page (about:start, about:home, about:newtab). */
gboolean
ns_about_url_is_start(const char *url)
{
    static const char *const start_pages[] = {
        "about:start", "about:home", "about:newtab", NULL,
    };
    return about_url_is_one_of(url, start_pages);
}

/* A browser-chrome about: page (settings, history, ...), as opposed to
 * the public ones that web content can open or script, like about:blank
 * and about:srcdoc. */
gboolean
ns_about_url_is_chrome(const char *url)
{
    /* An explicit list: an about: URL nobody serves (about:blank/x, ...)
     * must not count as privileged just for not being public. */
    static const char *const chrome_pages[] = {
        "about:settings", "about:config", "about:history", NULL,
    };
    if (!url || g_ascii_strncasecmp(url, "about:", 6) != 0) return FALSE;
    if (about_url_is_one_of(url, chrome_pages)) return TRUE;
    return strcspn(url, "?#") > 15 &&
           g_ascii_strncasecmp(url, "about:settings-", 15) == 0;
}

/* Builds the search URL by substituting the escaped query for %s; caller frees. */
char *
ns_search_url_for(const char *query)
{
    const ns_config *cfg = ns_config_get();
    const char *engine = (cfg && cfg->search_engine && *cfg->search_engine)
        ? cfg->search_engine : "https://lite.duckduckgo.com/lite/?q=%s";
    char *enc = g_uri_escape_string(query ? query : "", NULL, TRUE);
    const char *pct = strstr(engine, "%s");
    char *out;
    if (pct) {
        char *prefix = g_strndup(engine, (gsize)(pct - engine));
        out = g_strconcat(prefix, enc, pct + 2, NULL);
        g_free(prefix);
    } else {
        out = g_strconcat(engine, enc, NULL);
    }
    g_free(enc);
    return out;
}

/* file: URL for an existing local path (directories get a trailing slash); caller frees. */
char *
ns_url_from_local_path(const char *path)
{
    if (!path || !*path) return NULL;
    if (g_str_has_prefix(path, "about:") || g_str_has_prefix(path, "file:") ||
        g_str_has_prefix(path, "data:") || strstr(path, "://"))
        return NULL;

    char *candidate = NULL;
#ifdef G_OS_WIN32
    if (g_ascii_isalpha((guchar)path[0]) && path[1] == ':' && path[2] == '\0') {
        char root[4] = { path[0], ':', G_DIR_SEPARATOR, '\0' };
        candidate = g_strdup(root);
    }
#endif

    if (!candidate) {
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            return NULL;
        candidate = g_canonicalize_filename(path, NULL);
    }
    if (!candidate)
        return NULL;

    if (g_file_test(candidate, G_FILE_TEST_IS_DIR) &&
        !g_str_has_suffix(candidate, G_DIR_SEPARATOR_S)) {
        char *with_sep = g_strconcat(candidate, G_DIR_SEPARATOR_S, NULL);
        g_free(candidate);
        candidate = with_sep;
    }

    char *uri = g_filename_to_uri(candidate, NULL, NULL);
    g_free(candidate);
    return uri;
}

/* Copy of a proxy URL with any password replaced by ***; caller frees. */
char *
ns_net_proxy_mask(const char *proxy_url)
{
    if (!proxy_url || !*proxy_url) return g_strdup("");
    const char *scheme_sep = strstr(proxy_url, "://");
    const char *cursor = scheme_sep ? scheme_sep + 3 : proxy_url;
    const char *at = strchr(cursor, '@');
    if (!at) return g_strdup(proxy_url);
    const char *colon = memchr(cursor, ':', (gsize)(at - cursor));
    if (!colon) return g_strdup(proxy_url);
    GString *s = g_string_new(NULL);
    g_string_append_len(s, proxy_url, (gssize)(colon - proxy_url));
    g_string_append(s, ":***");
    g_string_append(s, at);
    return g_string_free(s, FALSE);
}
