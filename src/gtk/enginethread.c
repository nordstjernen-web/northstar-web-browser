/* Northstar Web Browser — the thread the page engine runs on, with its own main context.
 * Started on first post with an 8 MiB stack; runs queued jobs in order outside nested iterations.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "enginethread.h"
#include "mainctx.h"
#include "security.h"
#include "trace.h"

#ifndef G_OS_WIN32
#include <pthread.h>
#endif

#define NS_ENGINE_STACK_BYTES ((size_t)8 * 1024 * 1024)

typedef struct {
    NsEngineJob fn;
    gpointer    data;
} Job;

static GMainContext *g_ctx;
static GAsyncQueue  *g_jobs;

/* True when jobs are queued and the loop is not inside a nested main iteration. */
static gboolean
jobs_ready(void)
{
    return g_main_depth() == 0 && g_async_queue_length(g_jobs) > 0;
}

static gboolean
jobs_prepare(GSource *source, gint *timeout)
{
    (void)source;
    *timeout = -1;
    return jobs_ready();
}

static gboolean
jobs_check(GSource *source)
{
    (void)source;
    return jobs_ready();
}

/* GSource dispatch that runs every queued engine job in order. */
static gboolean
jobs_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    (void)source;
    (void)callback;
    (void)user_data;
    Job *job;
    while ((job = g_async_queue_try_pop(g_jobs))) {
        job->fn(job->data);
        g_free(job);
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs jobs_funcs = {
    .prepare = jobs_prepare,
    .check = jobs_check,
    .dispatch = jobs_dispatch,
};

static void
engine_set_os_thread_name(void)
{
#if defined(__APPLE__)
    pthread_setname_np("ns-engine");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "ns-engine");
#endif
}

/* Engine thread body: W^X setup, thread naming, job source and main loop on its context. */
static gpointer
engine_main(gpointer data)
{
    (void)data;
    ns_security_deny_exec_memory(FALSE);
    engine_set_os_thread_name();
    g_main_context_push_thread_default(g_ctx);
    ns_engine_context_set(g_ctx);
    ns_trace_thread_name("ns-engine");
    GSource *src = g_source_new(&jobs_funcs, sizeof(GSource));
    g_source_attach(src, g_ctx);
    g_source_unref(src);
    GMainLoop *loop = g_main_loop_new(g_ctx, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(g_ctx);
    return NULL;
}

/* Starts the detached engine thread with an 8 MiB stack, falling back to GThread. */
static void
engine_spawn(void)
{
#ifndef G_OS_WIN32
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, NS_ENGINE_STACK_BYTES);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&thread, &attr, engine_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc == 0) return;
#endif
    g_thread_unref(g_thread_new("ns-engine", engine_main, NULL));
}

/* Creates the engine context and job queue and spawns the thread exactly once. */
static void
engine_start(void)
{
    static gsize started;
    if (g_once_init_enter(&started)) {
        g_ctx = g_main_context_new();
        g_jobs = g_async_queue_new();
        engine_spawn();
        g_once_init_leave(&started, 1);
    }
}

/* Queues fn(data) to run on the engine thread, starting it on first use. */
void
ns_engine_thread_post(NsEngineJob fn, gpointer data)
{
    engine_start();
    Job *job = g_new0(Job, 1);
    job->fn = fn;
    job->data = data;
    g_async_queue_push(g_jobs, job);
    g_main_context_wakeup(g_ctx);
}
