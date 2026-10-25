/* Northstar Web Browser — runtime config: parses, saves and dumps the key = value settings file.
 * Table-driven defaults, NS_* environment overrides (not persisted on save), proxy masking.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"
#include "security.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

#include "net.h"

static ns_config g_cfg;
static char     *g_cfg_path;

/* The config file is line-based "key = value": a CR or LF inside a value
 * (e.g. a home_url submitted from the settings page) would start a new,
 * attacker-chosen line when saved.  Values never legitimately contain
 * them, so drop them on the way in. */
static void
set_string(char **slot, const char *value)
{
    g_free(*slot);
    char *v = g_strdup(value ? value : "");
    char *w = v;
    for (const char *r = v; *r; r++)
        if (*r != '\r' && *r != '\n') *w++ = *r;
    *w = '\0';
    *slot = v;
}

typedef struct { const char *name; int val; } ns_enum_choice;

/* Maps a value to an enum by ASCII case-insensitive name; dflt if empty or unknown. */
static int
parse_enum(const char *v, const ns_enum_choice *choices, gsize n, int dflt)
{
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < n; i++)
        if (g_ascii_strcasecmp(v, choices[i].name) == 0) return choices[i].val;
    return dflt;
}

/* Parses true/yes/on/1 or false/no/off/0; dflt otherwise. */
static gboolean
parse_bool(const char *v, gboolean dflt)
{
    static const ns_enum_choice map[] = {
        { "true", TRUE }, { "yes", TRUE }, { "on",  TRUE }, { "1", TRUE },
        { "false", FALSE }, { "no", FALSE }, { "off", FALSE }, { "0", FALSE },
    };
    return parse_enum(v, map, G_N_ELEMENTS(map), dflt) ? TRUE : FALSE;
}

/* Parses a decimal int, saturating to the int range; dflt if not a number. */
static int
parse_int(const char *v, int dflt)
{
    if (!v || !*v) return dflt;
    char *end = NULL;
    gint64 n = g_ascii_strtoll(v, &end, 10);
    if (end == v) return dflt;
    if (n < (gint64)G_MININT) return G_MININT;
    if (n > (gint64)G_MAXINT) return G_MAXINT;
    return (int)n;
}

/* Parses a referer_policy setting, accepting aliases like none, default and full. */
static ns_referer_policy
parse_referer_policy(const char *v, ns_referer_policy dflt)
{
    static const ns_enum_choice map[] = {
        { "none",                            NS_REFERER_NO_REFERRER },
        { "no-referrer",                     NS_REFERER_NO_REFERRER },
        { "same-origin",                     NS_REFERER_SAME_ORIGIN },
        { "strict-origin-when-cross-origin", NS_REFERER_STRICT_ORIGIN_WHEN_CROSS },
        { "default",                         NS_REFERER_STRICT_ORIGIN_WHEN_CROSS },
        { "unsafe-url",                      NS_REFERER_UNSAFE_URL },
        { "full",                            NS_REFERER_UNSAFE_URL },
    };
    return (ns_referer_policy)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

/* Parses a cookie_policy setting (always, first-party, never and aliases). */
static ns_cookie_policy
parse_cookie_policy(const char *v, ns_cookie_policy dflt)
{
    static const ns_enum_choice map[] = {
        { "always",           NS_COOKIE_ALWAYS },
        { "first-party",      NS_COOKIE_FIRST_PARTY },
        { "first-party-only", NS_COOKIE_FIRST_PARTY },
        { "never",            NS_COOKIE_NEVER },
        { "off",              NS_COOKIE_NEVER },
    };
    return (ns_cookie_policy)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

/* Parses a color_scheme setting (auto/system, light, dark). */
static ns_color_scheme_pref
parse_color_scheme(const char *v, ns_color_scheme_pref dflt)
{
    static const ns_enum_choice map[] = {
        { "auto",   NS_COLOR_SCHEME_PREF_AUTO },
        { "system", NS_COLOR_SCHEME_PREF_AUTO },
        { "light",  NS_COLOR_SCHEME_PREF_LIGHT },
        { "dark",   NS_COLOR_SCHEME_PREF_DARK },
    };
    return (ns_color_scheme_pref)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

/* Parses a reduced_motion setting (auto/system, no-preference/off, reduce/on). */
static ns_reduced_motion_pref
parse_reduced_motion(const char *v, ns_reduced_motion_pref dflt)
{
    static const ns_enum_choice map[] = {
        { "auto",          NS_REDUCED_MOTION_PREF_AUTO },
        { "system",        NS_REDUCED_MOTION_PREF_AUTO },
        { "no-preference", NS_REDUCED_MOTION_PREF_NO_PREFERENCE },
        { "off",           NS_REDUCED_MOTION_PREF_NO_PREFERENCE },
        { "reduce",        NS_REDUCED_MOTION_PREF_REDUCE },
        { "on",            NS_REDUCED_MOTION_PREF_REDUCE },
    };
    return (ns_reduced_motion_pref)parse_enum(v, map, G_N_ELEMENTS(map), dflt);
}

typedef enum cfg_kind {
    CFG_STRING,
    CFG_BOOL,
    CFG_INT,
    CFG_REFERER,
    CFG_COOKIE,
    CFG_COLOR_SCHEME,
    CFG_REDUCED_MOTION,
} cfg_kind;

typedef struct cfg_field {
    const char *key;
    cfg_kind    kind;
    size_t      offset;
    const char *def_str;
    int         def_int;
} cfg_field;

#define FS(name, val)       { #name, CFG_STRING,       G_STRUCT_OFFSET(ns_config, name), val,   0 }
#define FB(name, val)       { #name, CFG_BOOL,         G_STRUCT_OFFSET(ns_config, name), NULL,  val }
#define FI(name, val)       { #name, CFG_INT,          G_STRUCT_OFFSET(ns_config, name), NULL,  val }
#define FE(name, kind, val) { #name, kind,             G_STRUCT_OFFSET(ns_config, name), NULL,  val }

static const cfg_field cfg_fields[] = {
    FS(home_url,              "about:start"),
    FS(user_agent,            ""),
    FS(compat_mode,           "chrome"),
    FS(accept_language,       ""),
    FS(search_engine,         "https://lite.duckduckgo.com/lite/?q=%s"),
    FS(http_proxy,            ""),
    FS(https_proxy,           ""),
    FS(no_proxy,              ""),
    FS(doh_url,               ""),
    FS(gsk_renderer,          "auto"),
    FE(referer_policy,        CFG_REFERER,      NS_REFERER_STRICT_ORIGIN_WHEN_CROSS),
    FE(cookie_policy,         CFG_COOKIE,       NS_COOKIE_FIRST_PARTY),
    FE(color_scheme,          CFG_COLOR_SCHEME,    NS_COLOR_SCHEME_PREF_AUTO),
    FE(reduced_motion,        CFG_REDUCED_MOTION,  NS_REDUCED_MOTION_PREF_AUTO),
    FB(do_not_track,          FALSE),
    FB(global_privacy_control, FALSE),
    FB(strip_tracking_params, TRUE),
    FB(https_first,           TRUE),
    FB(harden_allocator,      TRUE),
    FB(speculative_preload,   TRUE),
    FB(async_image_decode,    TRUE),
    FB(images_enabled,        TRUE),
    FB(autoplay_enabled,      FALSE),
    FB(javascript_enabled,    TRUE),
    FB(local_storage_enabled, TRUE),
    FB(cache_enabled,         TRUE),
    FB(tls_allow_insecure_override, FALSE),
    FB(watchdog_enabled,      TRUE),
    FI(cache_cap_mb,          256),
    FI(js_eval_budget_ms,     60000),
    FI(js_memory_cap_mb,      2048),
    FI(max_redirects,         NS_MAX_REDIRECTS),
};

#undef FS
#undef FB
#undef FI
#undef FE

/* Resets every table-driven setting to its compiled-in default. */
static void
apply_default(ns_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        void *slot = (char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING:      set_string((char **)slot, f->def_str); break;
        case CFG_BOOL:        *(gboolean *)slot = (gboolean)f->def_int; break;
        case CFG_INT:         *(int *)slot      = f->def_int; break;
        case CFG_REFERER:     *(ns_referer_policy *)slot     = (ns_referer_policy)f->def_int; break;
        case CFG_COOKIE:      *(ns_cookie_policy *)slot      = (ns_cookie_policy)f->def_int; break;
        case CFG_COLOR_SCHEME:    *(ns_color_scheme_pref *)slot   = (ns_color_scheme_pref)f->def_int; break;
        case CFG_REDUCED_MOTION:  *(ns_reduced_motion_pref *)slot = (ns_reduced_motion_pref)f->def_int; break;
        }
    }
}

/* Applies one key = value setting via the field table; unknown keys are ignored. */
static void
apply_pair(ns_config *c, const char *key, const char *value)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        if (strcmp(key, f->key) != 0) continue;
        void *slot = (char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING:       set_string((char **)slot, value); break;
        case CFG_BOOL:         *(gboolean *)slot = parse_bool(value, *(gboolean *)slot); break;
        case CFG_INT:          *(int *)slot      = parse_int(value, *(int *)slot); break;
        case CFG_REFERER:      *(ns_referer_policy *)slot     = parse_referer_policy(value, *(ns_referer_policy *)slot); break;
        case CFG_COOKIE:       *(ns_cookie_policy *)slot      = parse_cookie_policy(value, *(ns_cookie_policy *)slot); break;
        case CFG_COLOR_SCHEME:   *(ns_color_scheme_pref *)slot   = parse_color_scheme(value, *(ns_color_scheme_pref *)slot); break;
        case CFG_REDUCED_MOTION: *(ns_reduced_motion_pref *)slot = parse_reduced_motion(value, *(ns_reduced_motion_pref *)slot); break;
        }
        return;
    }
}

/* Loads key = value lines from the config file, skipping blanks and # comments. */
static void
load_file(ns_config *c, const char *path)
{
    char *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("config: failed to read %s: %s", path, err->message);
        g_clear_error(&err);
        return;
    }
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key   = g_strstrip(line);
        char *value = g_strstrip(eq + 1);
        if (*key) apply_pair(c, key, value);
    }
    g_strfreev(lines);
    g_free(contents);
}

static const struct { const char *env; const char *key; } env_disable[] = {
    { "NS_NO_CACHE",         "cache_enabled"         },
    { "NS_NO_LOCAL_STORAGE", "local_storage_enabled" },
    { "NS_NO_IMAGES",        "images_enabled"        },
    { "NS_NO_WATCHDOG",      "watchdog_enabled"      },
    { "NS_NO_HTTPS_FIRST",   "https_first"           },
    { "NS_NO_HARDEN_ALLOC",  "harden_allocator"      },
    { "NS_NO_PRELOAD_SCAN",  "speculative_preload"   },
    { "NS_NO_ASYNC_IMG_DECODE", "async_image_decode" },
};

static const struct { const char *env; const char *key; } env_value[] = {
    { "NS_HOME_URL",    "home_url"    },
    { "NS_USER_AGENT",  "user_agent"  },
    { "NS_COMPAT_MODE", "compat_mode" },
    { "NS_HTTP_PROXY",  "http_proxy"  },
    { "NS_HTTPS_PROXY", "https_proxy" },
    { "NS_NO_PROXY",    "no_proxy"    },
    { "NS_DOH_URL",     "doh_url"     },
    { "NS_GSK_RENDERER","gsk_renderer"},
};

/* The file's own value of each setting an environment variable
 * overrides (key -> value as saved), so saving the settings writes those
 * back rather than persisting the override. */
static GHashTable *g_env_overridden;

static char *cfg_field_format(const ns_config *c, const char *key);

/* Remembers a setting's file value before an environment override replaces it. */
static void
note_env_override(const ns_config *c, const char *key)
{
    if (!g_env_overridden)
        g_env_overridden = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, g_free);
    if (g_hash_table_contains(g_env_overridden, key)) return;
    char *v = cfg_field_format(c, key);
    if (v) g_hash_table_insert(g_env_overridden, g_strdup(key), v);
}

/* Applies NS_* environment overrides on top of the file settings. */
static void
apply_env(ns_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(env_disable); i++)
        if (ns_env_flag(env_disable[i].env)) {
            note_env_override(c, env_disable[i].key);
            apply_pair(c, env_disable[i].key, "false");
        }
    for (gsize i = 0; i < G_N_ELEMENTS(env_value); i++) {
        const char *v = g_getenv(env_value[i].env);
        if (v && *v) {
            note_env_override(c, env_value[i].key);
            apply_pair(c, env_value[i].key, v);
        }
    }
    if (ns_env_flag("NS_PRIVATE")) {
        note_env_override(c, "private_mode");
        c->private_mode = TRUE;
    }
}

/* Loads defaults, the user config file and environment overrides once. */
void
ns_config_init(void)
{
    if (g_cfg_path)
        return;
    apply_default(&g_cfg);
    g_cfg_path = g_build_filename(g_get_user_config_dir(),
                                  NS_APP_DIR_NAME, "northstar.conf",
                                  NULL);
    load_file(&g_cfg, g_cfg_path);
    apply_env(&g_cfg);
}

/* Frees config strings and resets the global config. */
void
ns_config_shutdown(void)
{
    g_free(g_cfg.home_url);
    g_free(g_cfg.user_agent);
    g_free(g_cfg.accept_language);
    g_free(g_cfg.search_engine);
    g_free(g_cfg.http_proxy);
    g_free(g_cfg.https_proxy);
    g_free(g_cfg.no_proxy);
    g_free(g_cfg.doh_url);
    g_free(g_cfg.gsk_renderer);
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_clear_pointer(&g_cfg_path, g_free);
}

const ns_config *
ns_config_get(void)
{
    return &g_cfg;
}

ns_config *
ns_config_mut(void)
{
    return &g_cfg;
}

static GMutex g_cfg_mutex;

void
ns_config_lock(void)
{
    g_mutex_lock(&g_cfg_mutex);
}

void
ns_config_unlock(void)
{
    g_mutex_unlock(&g_cfg_mutex);
}

static const char *const referer_policy_names[] = {
    [NS_REFERER_NO_REFERRER]              = "none",
    [NS_REFERER_SAME_ORIGIN]              = "same-origin",
    [NS_REFERER_STRICT_ORIGIN_WHEN_CROSS] = "strict-origin-when-cross-origin",
    [NS_REFERER_UNSAFE_URL]               = "unsafe-url",
};

static const char *const cookie_policy_names[] = {
    [NS_COOKIE_ALWAYS]      = "always",
    [NS_COOKIE_FIRST_PARTY] = "first-party",
    [NS_COOKIE_NEVER]       = "never",
};

static const char *const color_scheme_names[] = {
    [NS_COLOR_SCHEME_PREF_AUTO]  = "auto",
    [NS_COLOR_SCHEME_PREF_LIGHT] = "light",
    [NS_COLOR_SCHEME_PREF_DARK]  = "dark",
};

static const char *const reduced_motion_names[] = {
    [NS_REDUCED_MOTION_PREF_AUTO]          = "auto",
    [NS_REDUCED_MOTION_PREF_NO_PREFERENCE] = "no-preference",
    [NS_REDUCED_MOTION_PREF_REDUCE]        = "reduce",
};

/* Config-file name for a referer policy, defaulting to strict-origin-when-cross-origin. */
static const char *
referer_policy_name(ns_referer_policy p)
{
    if ((unsigned)p >= G_N_ELEMENTS(referer_policy_names) || !referer_policy_names[p])
        return "strict-origin-when-cross-origin";
    return referer_policy_names[p];
}

/* Config-file name for a cookie policy, defaulting to first-party. */
static const char *
cookie_policy_name(ns_cookie_policy p)
{
    if ((unsigned)p >= G_N_ELEMENTS(cookie_policy_names) || !cookie_policy_names[p])
        return "first-party";
    return cookie_policy_names[p];
}

/* Config-file name for a color scheme preference, defaulting to auto. */
static const char *
color_scheme_name(ns_color_scheme_pref p)
{
    if ((unsigned)p >= G_N_ELEMENTS(color_scheme_names) || !color_scheme_names[p])
        return "auto";
    return color_scheme_names[p];
}

/* Config-file name for a reduced motion preference, defaulting to auto. */
static const char *
reduced_motion_name(ns_reduced_motion_pref p)
{
    if ((unsigned)p >= G_N_ELEMENTS(reduced_motion_names) || !reduced_motion_names[p])
        return "auto";
    return reduced_motion_names[p];
}

/* The value of setting key as the config file spells it. */
static char *
cfg_field_format(const ns_config *c, const char *key)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        if (strcmp(f->key, key) != 0) continue;
        const void *slot = (const char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING: {
            const char *v = *(const char *const *)slot;
            GString *out = g_string_new(NULL);
            for (const char *r = v ? v : ""; *r; r++)
                if (*r != '\r' && *r != '\n') g_string_append_c(out, *r);
            return g_string_free(out, FALSE);
        }
        case CFG_BOOL:
            return g_strdup(*(const gboolean *)slot ? "true" : "false");
        case CFG_INT:
            return g_strdup_printf("%d", *(const int *)slot);
        case CFG_REFERER:
            return g_strdup(referer_policy_name(*(const ns_referer_policy *)slot));
        case CFG_COOKIE:
            return g_strdup(cookie_policy_name(*(const ns_cookie_policy *)slot));
        case CFG_COLOR_SCHEME:
            return g_strdup(color_scheme_name(*(const ns_color_scheme_pref *)slot));
        case CFG_REDUCED_MOTION:
            return g_strdup(reduced_motion_name(*(const ns_reduced_motion_pref *)slot));
        }
    }
    return NULL;
}

/* Writes all settings to the config file (mode 0600), keeping file values for env overrides. */
gboolean
ns_config_save(GError **error)
{
    if (!g_cfg_path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "config path not initialized");
        return FALSE;
    }
    char *dir = g_path_get_dirname(g_cfg_path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "could not create config directory %s: %s",
                    dir, g_strerror(errno));
        g_free(dir);
        return FALSE;
    }
    g_free(dir);

    const ns_config *c = &g_cfg;
    GString *s = g_string_new(NULL);
    g_string_append(s, "# northstar configuration\n");
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const char *key = cfg_fields[i].key;
        const char *file_value = g_env_overridden
            ? g_hash_table_lookup(g_env_overridden, key) : NULL;
        g_autofree char *v = file_value ? NULL : cfg_field_format(c, key);
        g_string_append_printf(s, "%s = %s\n", key,
                               file_value ? file_value : (v ? v : ""));
    }
    gboolean ok = g_file_set_contents(g_cfg_path, s->str, (gssize)s->len, error);
    if (ok) g_chmod(g_cfg_path, 0600);
    g_string_free(s, TRUE);
    return ok;
}

/* Formats the effective configuration as text with proxies masked; caller frees. */
char *
ns_config_dump(void)
{
    const ns_config *c = &g_cfg;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "# northstar effective config\n");
    g_string_append_printf(s, "# file: %s\n", g_cfg_path ? g_cfg_path : "(none)");
    g_string_append_printf(s, "home_url              = %s\n", c->home_url);
    g_string_append_printf(s, "user_agent            = %s\n", c->user_agent);
    if (c->accept_language && *c->accept_language)
        g_string_append_printf(s, "accept_language       = %s\n",
                               c->accept_language);
    else
        g_string_append_printf(s, "accept_language       = (auto: %s)\n",
                               ns_net_default_accept_language());
    g_string_append_printf(s, "search_engine         = %s\n", c->search_engine);
    g_string_append_printf(s, "gsk_renderer          = %s\n",
                           c->gsk_renderer && *c->gsk_renderer
                               ? c->gsk_renderer : "auto");
    {
        char *hp  = ns_net_proxy_mask(c->http_proxy);
        char *hsp = ns_net_proxy_mask(c->https_proxy);
        g_string_append_printf(s, "http_proxy            = %s\n",
                               hp  && *hp  ? hp  : "(none)");
        g_string_append_printf(s, "https_proxy           = %s\n",
                               hsp && *hsp ? hsp : "(none)");
        g_string_append_printf(s, "no_proxy              = %s\n",
                               c->no_proxy && *c->no_proxy ? c->no_proxy : "(none)");
        g_free(hp);
        g_free(hsp);
    }
    g_string_append_printf(s, "doh_url               = %s\n",
                           c->doh_url && *c->doh_url ? c->doh_url : "(system resolver)");
    g_string_append_printf(s, "referer_policy        = %s\n", referer_policy_name(c->referer_policy));
    g_string_append_printf(s, "cookie_policy         = %s\n", cookie_policy_name(c->cookie_policy));
    g_string_append_printf(s, "color_scheme          = %s\n", color_scheme_name(c->color_scheme));
    g_string_append_printf(s, "reduced_motion        = %s\n", reduced_motion_name(c->reduced_motion));
    g_string_append_printf(s, "do_not_track          = %s\n", c->do_not_track ? "true" : "false");
    g_string_append_printf(s, "global_privacy_control = %s\n", c->global_privacy_control ? "true" : "false");
    g_string_append_printf(s, "strip_tracking_params = %s\n", c->strip_tracking_params ? "true" : "false");
    g_string_append_printf(s, "https_first           = %s\n", c->https_first ? "true" : "false");
    g_string_append_printf(s, "harden_allocator      = %s\n", c->harden_allocator ? "true" : "false");
    g_string_append_printf(s, "speculative_preload   = %s\n", c->speculative_preload ? "true" : "false");
    g_string_append_printf(s, "async_image_decode    = %s\n", c->async_image_decode ? "true" : "false");
    g_string_append_printf(s, "images_enabled        = %s\n", c->images_enabled ? "true" : "false");
    g_string_append_printf(s, "autoplay_enabled      = %s\n", c->autoplay_enabled ? "true" : "false");
    g_string_append_printf(s, "local_storage_enabled = %s\n", c->local_storage_enabled ? "true" : "false");
    g_string_append_printf(s, "cache_enabled         = %s\n", c->cache_enabled ? "true" : "false");
    g_string_append_printf(s, "tls_allow_insecure_override = %s\n", c->tls_allow_insecure_override ? "true" : "false");
    g_string_append_printf(s, "watchdog_enabled      = %s\n", c->watchdog_enabled ? "true" : "false");
    g_string_append_printf(s, "private_mode          = %s\n", c->private_mode ? "true" : "false");
    g_string_append_printf(s, "cache_cap_mb          = %d\n", c->cache_cap_mb);
    g_string_append_printf(s, "js_eval_budget_ms     = %d\n", c->js_eval_budget_ms);
    g_string_append_printf(s, "js_memory_cap_mb      = %d\n", c->js_memory_cap_mb);
    g_string_append_printf(s, "max_redirects         = %d\n", c->max_redirects);
    return g_string_free(s, FALSE);
}
