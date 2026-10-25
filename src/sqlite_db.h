/* Northstar Web Browser — helpers for opening, hardening and running SQL on SQLite databases.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_SQLITE_DB_H
#define NS_SQLITE_DB_H

#include <glib.h>
#include <sqlite3.h>

G_BEGIN_DECLS

sqlite3  *ns_sqlite_open(const char *tag, const char *path, int flags);
void      ns_sqlite_harden(sqlite3 *db);
gboolean  ns_sqlite_exec(sqlite3 *db, const char *tag, const char *sql);

G_END_DECLS

#endif
