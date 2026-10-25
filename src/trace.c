/* Northstar Web Browser — writes a Chrome trace-event JSON array that Perfetto can open.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <glib/gstdio.h>

static FILE   *g_trace_file;
static GMutex  g_trace_lock;
static gint    g_trace_next_tid;
static GPrivate g_trace_tid;
static gint64  g_trace_origin_us;

gboolean
ns_trace_enabled(void)
{
    return g_atomic_pointer_get(&g_trace_file) != NULL;
}

/* Returns the monotonic time to start a span, or 0 when tracing is off. */
gint64
ns_trace_now(void)
{
    return ns_trace_enabled() ? g_get_monotonic_time() : 0;
}

/* Returns a small per-thread trace id, assigned on first use. */
static int
trace_tid(void)
{
    int tid = GPOINTER_TO_INT(g_private_get(&g_trace_tid));
    if (tid == 0) {
        tid = g_atomic_int_add(&g_trace_next_tid, 1) + 1;
        g_private_set(&g_trace_tid, GINT_TO_POINTER(tid));
    }
    return tid;
}

/* Appends s as a JSON string literal, escaping quotes, backslashes and controls. */
static void
trace_append_json_string(GString *out, const char *s)
{
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20)
            g_string_append_printf(out, "\\u%04x", *p);
        else
            g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
}

/* Writes one trace event and a trailing comma to the open trace file. */
static void
trace_write(GString *event)
{
    g_mutex_lock(&g_trace_lock);
    if (g_trace_file) {
        fputs(event->str, g_trace_file);
        fputs(",\n", g_trace_file);
        fflush(g_trace_file);
    }
    g_mutex_unlock(&g_trace_lock);
}

/* Starts a Chrome trace-event JSON file at path and names the main thread. */
gboolean
ns_trace_open(const char *path)
{
    if (!path || !*path) return FALSE;
    FILE *f = g_fopen(path, "w");
    if (!f) return FALSE;
    g_trace_origin_us = g_get_monotonic_time();
    fputs("[\n", f);
    g_mutex_lock(&g_trace_lock);
    g_atomic_pointer_set(&g_trace_file, f);
    g_mutex_unlock(&g_trace_lock);
    ns_trace_thread_name("main");
    return TRUE;
}

/* Terminates the JSON array and closes the trace file. */
void
ns_trace_close(void)
{
    g_mutex_lock(&g_trace_lock);
    FILE *f = g_trace_file;
    g_atomic_pointer_set(&g_trace_file, NULL);
    g_mutex_unlock(&g_trace_lock);
    if (!f) return;
    fputs("{}]\n", f);
    fclose(f);
}

/* Emits a thread_name record for this thread unless it already has a trace id. */
void
ns_trace_thread_name_once(const char *name)
{
    if (!ns_trace_enabled() || g_private_get(&g_trace_tid)) return;
    ns_trace_thread_name(name);
}

/* Emits a thread_name metadata event for the calling thread. */
void
ns_trace_thread_name(const char *name)
{
    if (!ns_trace_enabled() || !name) return;
    GString *e = g_string_new("{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,");
    g_string_append_printf(e, "\"tid\":%d,\"args\":{\"name\":", trace_tid());
    trace_append_json_string(e, name);
    g_string_append(e, "}}");
    trace_write(e);
    g_string_free(e, TRUE);
}

void
ns_trace_completef(const char *category, const char *name, gint64 start_us,
                   const char *fmt, ...)
{
    if (!ns_trace_enabled() || start_us <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    char *detail = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    ns_trace_complete(category, name, start_us, detail);
    g_free(detail);
}

/* Emits a complete ("X") event from start_us to now with optional detail. */
void
ns_trace_complete(const char *category, const char *name, gint64 start_us,
                  const char *detail)
{
    if (!ns_trace_enabled() || !name || start_us <= 0) return;
    gint64 end_us = g_get_monotonic_time();
    GString *e = g_string_new("{\"name\":");
    trace_append_json_string(e, name);
    g_string_append(e, ",\"cat\":");
    trace_append_json_string(e, category ? category : "engine");
    g_string_append_printf(e,
        ",\"ph\":\"X\",\"ts\":%" G_GINT64_FORMAT ",\"dur\":%" G_GINT64_FORMAT
        ",\"pid\":1,\"tid\":%d",
        start_us - g_trace_origin_us, MAX(end_us - start_us, (gint64)0),
        trace_tid());
    if (detail && *detail) {
        g_string_append(e, ",\"args\":{\"detail\":");
        trace_append_json_string(e, detail);
        g_string_append_c(e, '}');
    }
    g_string_append_c(e, '}');
    trace_write(e);
    g_string_free(e, TRUE);
}
