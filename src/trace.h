/* Northstar Web Browser — Chrome trace-event output of frame phases, fetches and decodes.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_TRACE_H
#define NS_TRACE_H

#include <glib.h>

G_BEGIN_DECLS

gboolean ns_trace_open(const char *path);
void     ns_trace_close(void);
gboolean ns_trace_enabled(void);
gint64   ns_trace_now(void);
void     ns_trace_complete(const char *category, const char *name,
                           gint64 start_us, const char *detail);
void     ns_trace_completef(const char *category, const char *name,
                            gint64 start_us, const char *fmt, ...)
                            G_GNUC_PRINTF(4, 5);
void     ns_trace_thread_name(const char *name);
void     ns_trace_thread_name_once(const char *name);

G_END_DECLS

#endif
