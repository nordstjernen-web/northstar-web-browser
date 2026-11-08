/* Northstar Web Browser — phishing/malware checks: a local blocklist of SHA-256 host hashes
 * (host and parent domains) and Google Safe Browsing, session allow-lists, and the warning page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "safebrowsing.h"
#include "safebrowsing_google.h"
#include "config.h"
#include "net.h"

#include <string.h>

#include <openssl/evp.h>

static GHashTable *g_blocked;
static GHashTable *g_allow;
static gboolean    g_loaded;
/* Queried from the network and engine threads; guards the three above. */
static GMutex      g_sb_lock;
static gint        g_checks;
static gint        g_local_blocks;

/* Returns the lowercase hex SHA-256 of the lowercased host; caller frees. */
static char *
host_sha256_hex(const char *host)
{
    if (!host || !*host) return NULL;
    char *low = g_ascii_strdown(host, -1);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(low, strlen(low), digest, &len, EVP_sha256(), NULL) != 1) {
        g_free(low);
        return NULL;
    }
    g_free(low);
    GString *hex = g_string_sized_new(len * 2);
    for (unsigned int i = 0; i < len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);
    return g_string_free(hex, FALSE);
}

/* Adds host hashes (first token per line, 16+ chars, # comments) to the blocklist. */
static void
load_file(const char *path)
{
    char *contents = NULL;
    if (!path || !g_file_get_contents(path, &contents, NULL, NULL))
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#')
            continue;
        char *end = line;
        while (*end && !g_ascii_isspace(*end))
            end++;
        *end = '\0';
        if (strlen(line) >= 16)
            g_hash_table_add(g_blocked, g_ascii_strdown(line, -1));
    }
    g_strfreev(lines);
    g_free(contents);
}

/* Loads the first bundled safebrowsing.list found near the executable or in data/. */
static gboolean
load_bundled(void)
{
    static const char *const rel[] = {
        "../Resources/share/northstar/safebrowsing.list",
        "../share/northstar/safebrowsing.list",
        "share/northstar/safebrowsing.list",
        "data/safebrowsing.list",
        "../data/safebrowsing.list",
        "../../data/safebrowsing.list",
        "../../../data/safebrowsing.list",
    };
    char *dir = ns_executable_dir();
    if (dir) {
        for (gsize i = 0; i < G_N_ELEMENTS(rel); i++) {
            char *path = g_build_filename(dir, rel[i], NULL);
            gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
            if (exists) {
                load_file(path);
                g_free(path);
                g_free(dir);
                return TRUE;
            }
            g_free(path);
        }
        g_free(dir);
    }
    if (g_file_test("data/safebrowsing.list", G_FILE_TEST_EXISTS)) {
        load_file("data/safebrowsing.list");
        return TRUE;
    }
    return FALSE;
}

/* Lazily loads the env or user list plus the bundled list; caller holds the lock. */
static void
ensure_loaded(void)
{
    if (g_loaded)
        return;
    g_loaded = TRUE;
    g_blocked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_allow = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* An extra list (NS_SAFEBROWSING_LIST) adds to the bundled one; it
     * must not be a way to switch protection off. */
    const char *override = g_getenv("NS_SAFEBROWSING_LIST");
    if (override && *override) {
        load_file(override);
        load_bundled();
        return;
    }
    char *user = g_build_filename(g_get_user_config_dir(), "northstar",
                                  "safebrowsing.list", NULL);
    load_file(user);
    g_free(user);
    load_bundled();
}

/* Exempts a host from blocking for this session (user chose to continue). */
void
ns_safebrowsing_allow_host(const char *host)
{
    if (!host || !*host)
        return;
    g_mutex_lock(&g_sb_lock);
    ensure_loaded();
    g_hash_table_add(g_allow, g_ascii_strdown(host, -1));
    g_mutex_unlock(&g_sb_lock);
}

/* True if the host or any parent domain's SHA-256 is blocklisted and not allowed. */
gboolean
ns_safebrowsing_blocked(const char *host)
{
    const ns_config *cfg = ns_config_get();
    if (!host || !*host || (cfg && !cfg->safe_browsing))
        return FALSE;
    g_mutex_lock(&g_sb_lock);
    ensure_loaded();
    if (g_hash_table_size(g_blocked) == 0) {
        g_mutex_unlock(&g_sb_lock);
        return FALSE;
    }
    char *low = g_ascii_strdown(host, -1);
    for (gsize ln = strlen(low); ln > 0 && low[ln - 1] == '.'; ln--)
        low[ln - 1] = '\0';
    if (g_hash_table_contains(g_allow, low)) {
        g_mutex_unlock(&g_sb_lock);
        g_free(low);
        return FALSE;
    }
    gboolean hit = FALSE;
    for (const char *h = low; h && strchr(h, '.'); h = strchr(h, '.') + 1) {
        char *hex = host_sha256_hex(h);
        if (hex && g_hash_table_contains(g_blocked, hex))
            hit = TRUE;
        g_free(hex);
        if (hit)
            break;
    }
    g_mutex_unlock(&g_sb_lock);
    g_free(low);
    return hit;
}

/* True if the user chose to continue to this host earlier in the session. */
static gboolean
ns_safebrowsing_host_allowed(const char *host)
{
    char *low = g_ascii_strdown(host, -1);
    for (gsize ln = strlen(low); ln > 0 && low[ln - 1] == '.'; ln--)
        low[ln - 1] = '\0';
    g_mutex_lock(&g_sb_lock);
    ensure_loaded();
    gboolean allowed = g_hash_table_contains(g_allow, low);
    g_mutex_unlock(&g_sb_lock);
    g_free(low);
    return allowed;
}

/* Checks a URL against the local list, then Google Safe Browsing. */
ns_sb_verdict
ns_safebrowsing_check_url(const char *url)
{
    const ns_config *cfg = ns_config_get();
    if (!url || (cfg && !cfg->safe_browsing))
        return NS_SB_SAFE;
    g_autofree char *host = ns_url_host_from(url);
    if (!host || !*host || ns_safebrowsing_host_allowed(host))
        return NS_SB_SAFE;
    g_atomic_int_inc(&g_checks);
    if (ns_safebrowsing_blocked(host)) {
        g_atomic_int_inc(&g_local_blocks);
        return NS_SB_LOCAL_LIST;
    }
    return ns_sbg_check_url(url);
}

/* Fills in the status, list sizes and session counters for about:northstar. */
void
ns_safebrowsing_get_stats(ns_sb_stats *out)
{
    memset(out, 0, sizeof *out);
    const ns_config *cfg = ns_config_get();
    out->enabled = !cfg || cfg->safe_browsing;
    g_mutex_lock(&g_sb_lock);
    ensure_loaded();
    out->local_hosts = g_hash_table_size(g_blocked);
    g_mutex_unlock(&g_sb_lock);
    out->checks = (guint)g_atomic_int_get(&g_checks);
    out->local_blocks = (guint)g_atomic_int_get(&g_local_blocks);
    ns_sbg_get_stats(out);
}

static const char *
interstitial_heading(ns_sb_verdict verdict)
{
    switch (verdict) {
    case NS_SB_MALWARE:
        return "Dangerous site ahead";
    case NS_SB_UNWANTED_SOFTWARE:
        return "Site ahead offers harmful programs";
    default:
        return "Deceptive site ahead";
    }
}

static const char *
interstitial_summary(ns_sb_verdict verdict)
{
    switch (verdict) {
    case NS_SB_SOCIAL_ENGINEERING:
        return "This site is known for phishing. It may pretend to be a site "
               "you trust to get your passwords, payment details or other "
               "personal information.";
    case NS_SB_MALWARE:
        return "This site is known to spread malware. Visiting it may install "
               "programs that steal or damage the information on your device.";
    case NS_SB_UNWANTED_SOFTWARE:
        return "This site is known to push unwanted software. It may try to "
               "get you to install programs that change your browser or slow "
               "down your device.";
    default:
        return "Northstar Web Browser blocked this page because the site "
               "below is on your local list of known phishing or malware hosts. "
               "Attackers there may try to trick you into revealing passwords or "
               "payment details, or to install harmful software.";
    }
}

static const char *
interstitial_details(ns_sb_verdict verdict)
{
    if (verdict == NS_SB_LOCAL_LIST)
        return "This check runs entirely on your device against a local list "
               "\xe2\x80\x94 nothing about the page you visited was sent anywhere.";
    return "Google Safe Browsing lists this site as unsafe. Northstar checks "
           "addresses against a list kept on your device and only asks Google "
           "about a short, anonymous fragment of a hash of the address, never "
           "the address itself. Advisory provided by Google: <a href=\""
           "https://developers.google.com/safe-browsing/v4/advisory\">"
           "About Safe Browsing advisories</a>.";
}

/* Builds the HTML warning page for a blocked URL; caller frees. */
char *
ns_safebrowsing_interstitial(const char *url, const char *host,
                             ns_sb_verdict verdict)
{
    char *esc_url = g_markup_escape_text(url && *url ? url : "", -1);
    char *esc_host = g_markup_escape_text(host && *host ? host : "", -1);
    char *continue_href = g_strconcat(NS_UNSAFE_CONTINUE_SCHEME,
                                      url ? url : "", NULL);
    char *esc_continue = g_markup_escape_text(continue_href, -1);
    g_free(continue_href);

    char *html = g_strdup_printf(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Security warning — Northstar Web Browser</title><style>"
        "body{font-family:system-ui,-apple-system,\"Segoe UI\",Helvetica,"
        "Arial,sans-serif;background:#7a1212;color:#1b1b22;margin:0;padding:0;"
        "min-height:100vh;display:flex;align-items:center;justify-content:center}"
        ".card{background:#fff;border:1px solid #e3e3e8;border-top:6px solid "
        "#c0271c;border-radius:10px;box-shadow:0 6px 30px rgba(0,0,0,0.25);"
        "padding:36px 40px;max-width:640px;margin:32px 16px;line-height:1.5}"
        ".icon{font-size:52px;line-height:1;margin-bottom:14px}"
        "h1{font-size:23px;margin:0 0 12px 0;color:#a01b12}"
        "p.summary{font-size:16px;color:#33333d;margin:0 0 16px 0}"
        ".host{font-family:ui-monospace,\"SF Mono\",Menlo,Consolas,monospace;"
        "background:#fbeeec;border:1px solid #f0cfcb;border-radius:6px;"
        "padding:8px 12px;font-size:14px;color:#a01b12;font-weight:600;"
        "overflow-wrap:anywhere;margin:0 0 18px 0}"
        ".actions{display:flex;gap:10px;flex-wrap:wrap}"
        ".btn{display:inline-block;padding:10px 18px;border-radius:6px;"
        "text-decoration:none;font-size:14px;font-weight:600;"
        "border:1px solid transparent;font-family:inherit}"
        ".btn.primary{background:#2f7d36;color:#fff;border-color:#2f7d36}"
        ".btn.primary:hover{background:#286b2e}"
        ".btn.danger{background:#fff;color:#a01b12;border-color:#e0b6b2}"
        ".btn.danger:hover{background:#fbeeec}"
        ".tips{margin-top:24px;padding-top:18px;border-top:1px solid #ececf0;"
        "color:#555;font-size:13px}"
        "</style></head><body><div class=\"card\">"
        "<div class=\"icon\">\xe2\x9a\xa0\xef\xb8\x8f</div>"
        "<h1>%s</h1>"
        "<p class=\"summary\">%s</p>"
        "<p class=\"host\">%s</p>"
        "<div class=\"actions\">"
        "<a class=\"btn primary\" href=\"about:start\">Back to safety</a>"
        "<a class=\"btn danger\" href=\"%s\">Continue anyway "
        "(not recommended)</a>"
        "</div>"
        "<div class=\"tips\">%s "
        "The blocked address was <span style=\"font-family:ui-monospace,"
        "monospace;overflow-wrap:anywhere\">%s</span>.</div>"
        "</div></body></html>",
        interstitial_heading(verdict), interstitial_summary(verdict),
        esc_host, esc_continue, interstitial_details(verdict), esc_url);

    g_free(esc_url);
    g_free(esc_host);
    g_free(esc_continue);
    return html;
}
