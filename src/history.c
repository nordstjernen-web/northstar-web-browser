/* Northstar Web Browser — SQLite-backed browsing history.
 * Upserts http(s) visits, keeps the newest 10000, serves address-bar search and about:history.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "history.h"
#include "config.h"
#include "sqlite_db.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#define NS_HISTORY_MAX_ROWS  10000
#define NS_HISTORY_PAGE_ROWS 200

static sqlite3 *g_history_db;
static gboolean g_history_disabled;
static GMutex   g_history_mutex;

static gboolean
history_exec(const char *sql)
{
    return ns_sqlite_exec(g_history_db, "history", sql);
}

/* TRUE for http(s) URLs without control characters; others are not stored in history. */
static gboolean
history_is_recordable(const char *url)
{
    if (!url || !*url) return FALSE;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return FALSE;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return FALSE;
    return TRUE;
}

/* Sets WAL pragmas and creates the visits table and its last_visit index. */
static gboolean
history_schema(void)
{
    return history_exec("PRAGMA journal_mode=WAL") &&
           history_exec("PRAGMA synchronous=NORMAL") &&
           history_exec("CREATE TABLE IF NOT EXISTS visits("
                        "url TEXT PRIMARY KEY,"
                        "title TEXT,"
                        "visit_count INTEGER NOT NULL DEFAULT 1,"
                        "last_visit INTEGER NOT NULL)") &&
           history_exec("CREATE INDEX IF NOT EXISTS idx_visits_last "
                        "ON visits(last_visit)");
}

/* Deletes all but the 10000 most recently visited history rows. */
static void
history_prune(void)
{
    if (!g_history_db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_history_db,
            "DELETE FROM visits WHERE url NOT IN ("
            "SELECT url FROM visits ORDER BY last_visit DESC LIMIT ?)",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, NS_HISTORY_MAX_ROWS);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Opens or creates the 0600 history database; disables history if it cannot open. */
void
ns_history_init(void)
{
    g_mutex_lock(&g_history_mutex);
    if (g_history_db || g_history_disabled) {
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    char *dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    char *path = g_build_filename(dir, "history.sqlite", NULL);
    g_free(dir);

    g_history_db = ns_sqlite_open("history", path,
                                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!g_history_db) {
        g_history_disabled = TRUE;
        g_free(path);
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    if (!history_schema()) {
        sqlite3_close(g_history_db);
        g_history_db = NULL;
        g_history_disabled = TRUE;
        g_free(path);
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    g_chmod(path, 0600);
    g_free(path);
    history_prune();
    g_mutex_unlock(&g_history_mutex);
}

void
ns_history_shutdown(void)
{
    g_mutex_lock(&g_history_mutex);
    if (g_history_db) {
        sqlite3_close(g_history_db);
        g_history_db = NULL;
    }
    g_mutex_unlock(&g_history_mutex);
}

/* Upserts a visit, bumping its count and time and keeping the old title if new is empty. */
void
ns_history_record(const char *url, const char *title)
{
    if (!history_is_recordable(url)) return;
    g_mutex_lock(&g_history_mutex);
    if (!g_history_db) {
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_history_db,
            "INSERT INTO visits(url,title,visit_count,last_visit) "
            "VALUES(?,?,1,?) "
            "ON CONFLICT(url) DO UPDATE SET "
            "visit_count=visit_count+1,last_visit=excluded.last_visit,"
            "title=COALESCE(NULLIF(excluded.title,''),title)",
            -1, &st, NULL) != SQLITE_OK) {
        g_mutex_unlock(&g_history_mutex);
        return;
    }
    sqlite3_bind_text (st, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, (title && *title) ? title : NULL, -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_step(st);
    sqlite3_finalize(st);
    g_mutex_unlock(&g_history_mutex);
}

void
ns_history_clear(void)
{
    g_mutex_lock(&g_history_mutex);
    history_exec("DELETE FROM visits");
    g_mutex_unlock(&g_history_mutex);
}

static const char k_history_style[] =
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:820px;margin:2.4em auto;"
    "padding:0 1.2em;color:#222;background:#fff;}"
    "h1{font-size:1.6em;font-weight:600;margin:0 0 0.2em 0;}"
    ".sub{color:#777;margin:0 0 1.8em 0;font-size:0.92em;}"
    "ul{list-style:none;padding:0;margin:0;}"
    "li{padding:0.5em 0;border-bottom:1px solid #eee;}"
    "a{color:#3a63d0;text-decoration:none;font-size:1.02em;}"
    "a:hover{text-decoration:underline;}"
    ".u{display:block;color:#999;font-size:0.82em;word-break:break-all;}"
    ".d{color:#bbb;font-size:0.8em;float:right;}"
    ".empty{color:#999;font-style:italic;margin-top:2em;}"
    "@media (prefers-color-scheme: dark){"
    "body{color:#e6e8ec;background:#16181d;}"
    ".sub{color:#9aa3b2;}"
    "li{border-bottom-color:#26292f;}"
    "a{color:#7fa4ff;}"
    ".u{color:#7d8593;}"
    ".d{color:#666d78;}"
    ".empty{color:#8b93a1;}"
    "}"
    "</style>";

static void
history_match_free(gpointer data)
{
    ns_history_match *m = data;
    g_free(m->url);
    g_free(m->title);
    g_free(m);
}

/* Builds a %substring% LIKE pattern with %, _ and \ escaped; caller frees. */
static char *
history_like_pattern(const char *query)
{
    GString *s = g_string_new("%");
    for (const char *p = query; *p; p++) {
        if (*p == '%' || *p == '_' || *p == '\\')
            g_string_append_c(s, '\\');
        g_string_append_c(s, *p);
    }
    g_string_append_c(s, '%');
    return g_string_free(s, FALSE);
}

/* Substring search over URL and title, ranked by visit count then recency. */
GPtrArray *
ns_history_search(const char *query, guint limit)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(history_match_free);
    if (!query || !*query || !limit)
        return out;
    char *pattern = history_like_pattern(query);
    g_mutex_lock(&g_history_mutex);
    sqlite3_stmt *st = NULL;
    if (g_history_db &&
        sqlite3_prepare_v2(g_history_db,
            "SELECT url,title FROM visits "
            "WHERE url LIKE ?1 ESCAPE '\\' OR title LIKE ?1 ESCAPE '\\' "
            "ORDER BY visit_count DESC, last_visit DESC LIMIT ?2",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, pattern, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, (int)limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *url   = (const char *)sqlite3_column_text(st, 0);
            const char *title = (const char *)sqlite3_column_text(st, 1);
            if (!url) continue;
            ns_history_match *m = g_new0(ns_history_match, 1);
            m->url = g_strdup(url);
            m->title = g_strdup(title ? title : "");
            g_ptr_array_add(out, m);
        }
        sqlite3_finalize(st);
    }
    g_mutex_unlock(&g_history_mutex);
    g_free(pattern);
    return out;
}

/* Renders the about:history page of the 200 most recent visits; caller frees. */
char *
ns_history_html_page(void)
{
    GString *s = g_string_new(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>History</title>");
    g_string_append(s, k_history_style);
    g_string_append(s, "</head><body><h1>History</h1>"
                       "<p class=\"sub\">Recently visited pages.</p>");

    g_mutex_lock(&g_history_mutex);
    sqlite3_stmt *st = NULL;
    int have = 0;
    if (g_history_db &&
        sqlite3_prepare_v2(g_history_db,
            "SELECT url,title,last_visit FROM visits "
            "ORDER BY last_visit DESC LIMIT ?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, NS_HISTORY_PAGE_ROWS);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *url   = (const char *)sqlite3_column_text(st, 0);
            const char *title = (const char *)sqlite3_column_text(st, 1);
            gint64 when       = sqlite3_column_int64(st, 2);
            if (!url) continue;
            if (!have) g_string_append(s, "<ul>");
            have = 1;
            char *e_url   = g_markup_escape_text(url, -1);
            char *e_title = g_markup_escape_text(
                (title && *title) ? title : url, -1);
            GDateTime *dt = g_date_time_new_from_unix_local(when);
            char *date = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : NULL;
            if (dt) g_date_time_unref(dt);
            g_string_append_printf(s,
                "<li><span class=\"d\">%s</span>"
                "<a href=\"%s\">%s</a>"
                "<span class=\"u\">%s</span></li>",
                date ? date : "", e_url, e_title, e_url);
            g_free(date);
            g_free(e_url);
            g_free(e_title);
        }
        sqlite3_finalize(st);
    }
    g_mutex_unlock(&g_history_mutex);

    if (have) g_string_append(s, "</ul>");
    else      g_string_append(s, "<p class=\"empty\">No history yet.</p>");
    g_string_append(s, "</body></html>");
    return g_string_free(s, FALSE);
}
