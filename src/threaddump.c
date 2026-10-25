/* Northstar Web Browser — SIGQUIT-triggered dump of this process's threads to stderr.
 * Reads state, CPU time and names from /proc/<pid>/task; a helper thread does the writing.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "threaddump.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} ns_thread_buf;

__attribute__((format(printf, 2, 3)))
/* printf-style append to a growable malloc'd buffer; silently drops on allocation failure. */
static void
ns_thread_buf_addf(ns_thread_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    if (b->len + (size_t)need + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 512;
        while (ncap < b->len + (size_t)need + 1) ncap *= 2;
        char *np = realloc(b->p, ncap);
        if (!np) {
            va_end(ap2);
            return;
        }
        b->p = np;
        b->cap = ncap;
    }
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
}

/* Formats per-thread state, CPU seconds and name from /proc/<pid>/task; caller frees. */
static char *
ns_thread_dump_text(int pid, const char *label)
{
    ns_thread_buf b = {0};
    ns_thread_buf_addf(&b, "===== thread dump: %s (pid %d) =====\n",
                       label ? label : "process", pid);
#if defined(__linux__)
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%d/task", pid);
    DIR *d = opendir(dir);
    if (!d) {
        ns_thread_buf_addf(&b, "  (no /proc/%d/task)\n", pid);
    } else {
        long tick = sysconf(_SC_CLK_TCK);
        if (tick <= 0) tick = 100;
        struct dirent *ent;
        int n = 0;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            char sp[320];
            snprintf(sp, sizeof sp, "/proc/%d/task/%s/stat", pid, ent->d_name);
            FILE *f = fopen(sp, "r");
            char st = '?', comm[64] = "";
            double cpu = -1.0;
            if (f) {
                char line[1024];
                if (fgets(line, sizeof line, f)) {
                    char *lp = strchr(line, '(');
                    char *rp = strrchr(line, ')');
                    if (lp && rp && rp > lp + 1) {
                        size_t clen = (size_t)(rp - lp - 1);
                        if (clen >= sizeof comm) clen = sizeof comm - 1;
                        memcpy(comm, lp + 1, clen);
                        comm[clen] = '\0';
                    }
                    if (rp && rp[1] && rp[2]) {
                        st = rp[2];
                        long utime = 0, stime = 0;
                        int idx = 0;
                        char *save = NULL;
                        for (char *tok = strtok_r(rp + 2, " ", &save); tok;
                             tok = strtok_r(NULL, " ", &save), idx++) {
                            if (idx == 11) utime = atol(tok);
                            else if (idx == 12) { stime = atol(tok); break; }
                        }
                        cpu = (double)(utime + stime) / (double)tick;
                    }
                }
                fclose(f);
            }
            ns_thread_buf_addf(&b, "  thread %-6s  state %c  cpu %8.3fs  %s\n",
                               ent->d_name, st, cpu, comm);
            n++;
        }
        closedir(d);
        ns_thread_buf_addf(&b, "  (%d threads)\n", n);
    }
#else
    ns_thread_buf_addf(&b, "  (thread dump not supported on this platform)\n");
#endif
    return b.p;
}

static void
ns_thread_dump_to_stderr(int pid, const char *label)
{
    char *text = ns_thread_dump_text(pid, label);
    if (text) {
        fputs(text, stderr);
        free(text);
    }
    fflush(stderr);
}

static int   g_sig_pipe[2] = { -1, -1 };
static char *g_sig_label;

/* Async-signal-safe SIGQUIT handler that wakes the dump thread through a pipe. */
static void
ns_thread_dump_signal_handler(int sig)
{
    (void)sig;
    int saved = errno;
    unsigned char b = 1;
    ssize_t r;
    do {
        r = write(g_sig_pipe[1], &b, 1);
    } while (r < 0 && errno == EINTR);
    (void)r;
    errno = saved;
}

/* Thread that writes a thread dump to stderr each time the signal pipe is poked. */
static void *
ns_thread_dump_dispatch(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned char b;
        ssize_t r = read(g_sig_pipe[0], &b, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0)
            break;
        ns_thread_dump_to_stderr((int)getpid(),
                                 g_sig_label ? g_sig_label : "process");
    }
    return NULL;
}
#endif

/* Installs a SIGQUIT handler that dumps this process's threads to stderr (POSIX only). */
void
ns_thread_dump_install_signal(const char *label)
{
#ifndef _WIN32
    if (g_sig_pipe[0] >= 0)
        return;
    int fds[2];
    if (pipe(fds) != 0)
        return;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    g_sig_pipe[0] = fds[0];
    g_sig_pipe[1] = fds[1];
    g_sig_label = label ? strdup(label) : NULL;

    pthread_t th;
    if (pthread_create(&th, NULL, ns_thread_dump_dispatch, NULL) == 0)
        pthread_detach(th);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ns_thread_dump_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGQUIT, &sa, NULL);
#else
    (void)label;
#endif
}
