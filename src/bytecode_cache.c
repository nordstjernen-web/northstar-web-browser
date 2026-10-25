/* Northstar Web Browser — QuickJS bytecode cache in memory and in an on-disk LRU directory.
 * SHA-256 keys per site, script and source; background precompiles large classic scripts.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bytecode_cache.h"

#include "config.h"
#include "net.h"
#include "quickjs_compat.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <io.h>
#include <sys/utime.h>
#else
#include <unistd.h>
#include <utime.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define NS_BYTECODE_CACHE_MEM_CAP_BYTES   (64u * 1024u * 1024u)
#define NS_BYTECODE_CACHE_VALUE_CAP_BYTES (48u * 1024u * 1024u)
#define NS_BYTECODE_CACHE_DISK_CAP_BYTES  ((guint64)256u * 1024u * 1024u)
#define NS_BYTECODE_CACHE_PRUNE_MIN_BYTES (1024u * 1024u)
#define NS_BYTECODE_PRECOMPILE_MIN_BYTES  (64u * 1024u)
#define NS_BYTECODE_PRECOMPILE_WAIT_US    (20 * G_USEC_PER_SEC)
#define NS_BYTECODE_PRECOMPILE_STACK      (512u * 1024u)
#define NS_BYTECODE_PRECOMPILE_MEMORY     ((size_t)1024 * 1024 * 1024)
#define NS_BYTECODE_CACHE_FORMAT_VERSION  2026101701u

#ifdef NS_QUICKJS_ORIGINAL
#define NS_BYTECODE_CACHE_DIR "jsbc" G_DIR_SEPARATOR_S "quickjs-" NS_QUICKJS_VERSION
#else
#define NS_BYTECODE_CACHE_DIR "jsbc"
#endif

typedef struct ns_bytecode_cache_entry {
    guint8 *bytes;
    gsize   len;
    gint64  used_us;
} ns_bytecode_cache_entry;

static GHashTable *g_mem;
static GMutex      g_lock;
static GCond       g_pending_cond;
static GHashTable *g_pending;
static GThreadPool *g_precompile_pool;
static guint64     g_mem_bytes;
static char       *g_dir;

static void
ns_bytecode_cache_entry_free(gpointer data)
{
    ns_bytecode_cache_entry *e = data;
    if (!e) return;
    g_free(e->bytes);
    g_free(e);
}

/* Creates the in-memory cache and, if caching is enabled, the on-disk jsbc directory. */
void
ns_bytecode_cache_init(void)
{
    g_mutex_lock(&g_lock);
    if (!g_mem)
        g_mem = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free, ns_bytecode_cache_entry_free);
    if (!g_dir) {
        const ns_config *c = ns_config_get();
        if (!c || c->cache_enabled) {
            const char *base = g_get_user_cache_dir();
            g_dir = g_build_filename(base, NS_APP_DIR_NAME, NS_BYTECODE_CACHE_DIR, NULL);
            g_mkdir_with_parents(g_dir, 0700);
        }
    }
    g_mutex_unlock(&g_lock);
}

/* Joins precompile workers and frees the memory cache and pending set. */
void
ns_bytecode_cache_shutdown(void)
{
    if (g_precompile_pool) {
        g_thread_pool_free(g_precompile_pool, TRUE, TRUE);
        g_precompile_pool = NULL;
    }
    g_mutex_lock(&g_lock);
    g_clear_pointer(&g_pending, g_hash_table_destroy);
    if (g_mem) {
        g_hash_table_destroy(g_mem);
        g_mem = NULL;
    }
    g_mem_bytes = 0;
    g_clear_pointer(&g_dir, g_free);
    g_mutex_unlock(&g_lock);
}

/* SHA-256 key over top-level site partition, script name and source text. */
static void
hash_source(const char *top_url, const char *name,
            const char *src, gsize len, char out[65])
{
    g_autofree char *site = top_url ? ns_url_site_from(top_url) : NULL;
    if (!site && top_url) site = ns_url_origin_from(top_url);
    const char *partition = site ? site : (top_url ? top_url : "");
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)partition, (gssize)strlen(partition) + 1);
    g_checksum_update(c, (const guchar *)(name ? name : ""),
                      (gssize)(name ? strlen(name) : 0) + 1);
    g_checksum_update(c, (const guchar *)(src ? src : ""), (gssize)len);
    g_strlcpy(out, g_checksum_get_string(c), 65);
    g_checksum_free(c);
}

/* Private browsing must neither read nor leave a trace in the cache. */
static gboolean
cache_bypassed(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg && cfg->private_mode;
}

/* On-disk path for a key, sharded by its first two hex chars; caller frees. */
static char *
disk_path_for(const char *key)
{
    g_mutex_lock(&g_lock);
    char *base = g_strdup(g_dir);
    g_mutex_unlock(&g_lock);
    if (!base || !key || !*key) { g_free(base); return NULL; }
    char sub[3] = { key[0], key[1], '\0' };
    char *dir = g_build_filename(base, sub, NULL);
    g_mkdir_with_parents(dir, 0700);
    char *file = g_build_filename(dir, key + 2, NULL);
    g_free(dir);
    g_free(base);
    return file;
}

/* Reads a cached bytecode file, checking magic and format version; caller frees. */
static guint8 *
read_disk(const char *key, gsize *out_len)
{
    char *path = disk_path_for(key);
    if (!path) return NULL;
    GStatBuf stbuf;
    if (g_stat(path, &stbuf) != 0 || stbuf.st_size < 8 ||
        (guint64)stbuf.st_size > (guint64)NS_BYTECODE_CACHE_VALUE_CAP_BYTES + 8) {
        g_free(path);
        return NULL;
    }
    gchar *contents = NULL;
    gsize length = 0;
    gboolean ok = g_file_get_contents(path, &contents, &length, NULL);
    if (ok) g_utime(path, NULL);
    g_free(path);
    if (!ok) return NULL;
    if (length < 4 + 4) { g_free(contents); return NULL; }
    guint32 magic = 0, fmt = 0;
    memcpy(&magic, contents,     4);
    memcpy(&fmt,   contents + 4, 4);
    if (magic != GUINT32_FROM_LE(0x4E4A4243u) ||
        fmt   != GUINT32_FROM_LE(NS_BYTECODE_CACHE_FORMAT_VERSION)) {
        g_free(contents);
        return NULL;
    }
    gsize bc_len = length - 8;
    if (bc_len == 0 || bc_len > NS_BYTECODE_CACHE_VALUE_CAP_BYTES) {
        g_free(contents);
        return NULL;
    }
    guint8 *out = g_malloc(bc_len);
    memcpy(out, contents + 8, bc_len);
    g_free(contents);
    if (out_len) *out_len = bc_len;
    return out;
}

typedef struct {
    char   *path;
    guint64 size;
    gint64  mtime;
} disk_file;

/* Sorts disk cache files by modification time, oldest first. */
static gint
disk_file_older_first(gconstpointer a, gconstpointer b)
{
    const disk_file *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0;
}

/* Deletes least recently used cache files until the disk cache fits its cap. */
static void
prune_disk(void)
{
    g_mutex_lock(&g_lock);
    char *base = g_strdup(g_dir);
    g_mutex_unlock(&g_lock);
    if (!base) return;
    GArray *files = g_array_new(FALSE, FALSE, sizeof(disk_file));
    guint64 total = 0;
    GDir *top = g_dir_open(base, 0, NULL);
    const char *sub;
    while (top && (sub = g_dir_read_name(top))) {
        char *subdir = g_build_filename(base, sub, NULL);
        GDir *d = g_dir_open(subdir, 0, NULL);
        const char *name;
        while (d && (name = g_dir_read_name(d))) {
            disk_file f = { g_build_filename(subdir, name, NULL), 0, 0 };
            GStatBuf st;
            if (g_file_test(f.path, G_FILE_TEST_IS_REGULAR) &&
                g_stat(f.path, &st) == 0) {
                f.size = (guint64)st.st_size;
                f.mtime = (gint64)st.st_mtime;
                total += f.size;
                g_array_append_val(files, f);
            } else {
                g_free(f.path);
            }
        }
        if (d) g_dir_close(d);
        g_free(subdir);
    }
    if (top) g_dir_close(top);
    g_free(base);
    if (total > NS_BYTECODE_CACHE_DISK_CAP_BYTES) {
        g_array_sort(files, disk_file_older_first);
        for (guint i = 0; i < files->len &&
                          total > NS_BYTECODE_CACHE_DISK_CAP_BYTES; i++) {
            disk_file *f = &g_array_index(files, disk_file, i);
            if (g_unlink(f->path) == 0) total -= f->size;
        }
    }
    for (guint i = 0; i < files->len; i++)
        g_free(g_array_index(files, disk_file, i).path);
    g_array_free(files, TRUE);
}

/* Atomically writes headered bytecode via temp file and rename; prunes after big writes. */
static void
write_disk(const char *key, const guint8 *bc, gsize bc_len)
{
    char *path = disk_path_for(key);
    if (!path) return;
    guint32 magic = GUINT32_TO_LE(0x4E4A4243u);
    guint32 fmt   = GUINT32_TO_LE(NS_BYTECODE_CACHE_FORMAT_VERSION);
    char *tmp = g_strdup_printf("%s.tmp", path);
    int fd = g_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        if (fd >= 0) close(fd);
        g_free(tmp); g_free(path);
        return;
    }
    gboolean wrote = fwrite(&magic, 1, 4, f) == 4 &&
                     fwrite(&fmt,   1, 4, f) == 4 &&
                     fwrite(bc,     1, bc_len, f) == bc_len &&
                     ferror(f) == 0;
    if (fclose(f) != 0 || !wrote) {
        g_unlink(tmp);
        g_free(tmp); g_free(path);
        return;
    }
    if (g_rename(tmp, path) != 0)
        g_unlink(tmp);
    g_free(tmp);
    g_free(path);
    if (bc_len >= NS_BYTECODE_CACHE_PRUNE_MIN_BYTES) prune_disk();
}

/* Evicts least recently used memory entries until want_bytes fits the cap. */
static void
evict_until_fits(gsize want_bytes)
{
    if (!g_mem) return;
    while (g_mem_bytes + want_bytes > NS_BYTECODE_CACHE_MEM_CAP_BYTES) {
        gint64   oldest_us = G_MAXINT64;
        gpointer oldest_key = NULL;
        ns_bytecode_cache_entry *oldest_entry = NULL;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_mem);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ns_bytecode_cache_entry *e = v;
            if (e->used_us < oldest_us) {
                oldest_us = e->used_us;
                oldest_key = k;
                oldest_entry = e;
            }
        }
        if (!oldest_entry) break;
        g_mem_bytes -= oldest_entry->len;
        g_hash_table_remove(g_mem, oldest_key);
    }
}

/* Looks up bytecode in memory then disk, waiting for a pending precompile; caller frees. */
guint8 *
ns_bytecode_cache_get(const char *top_url, const char *name,
                      const char *src, gsize src_len, gsize *out_len)
{
    if (!src || !src_len || cache_bypassed()) return NULL;
    char key[65];
    hash_source(top_url, name, src, src_len, key);

    g_mutex_lock(&g_lock);
    if (!g_mem) {
        g_mutex_unlock(&g_lock);
        return NULL;
    }
    gint64 wait_until = g_get_monotonic_time() + NS_BYTECODE_PRECOMPILE_WAIT_US;
    while (g_pending && g_hash_table_contains(g_pending, key))
        if (!g_cond_wait_until(&g_pending_cond, &g_lock, wait_until))
            break;
    ns_bytecode_cache_entry *e = g_hash_table_lookup(g_mem, key);
    if (e) {
        e->used_us = g_get_monotonic_time();
        guint8 *copy = g_memdup2(e->bytes, e->len);
        if (out_len) *out_len = e->len;
        g_mutex_unlock(&g_lock);
        return copy;
    }
    g_mutex_unlock(&g_lock);

    gsize disk_len = 0;
    guint8 *disk = read_disk(key, &disk_len);
    if (!disk) return NULL;

    g_mutex_lock(&g_lock);
    if (g_mem && disk_len <= NS_BYTECODE_CACHE_VALUE_CAP_BYTES) {
        if (!g_hash_table_lookup(g_mem, key)) {
            evict_until_fits(disk_len);
            ns_bytecode_cache_entry *ne = g_new0(ns_bytecode_cache_entry, 1);
            ne->bytes = g_memdup2(disk, disk_len);
            ne->len = disk_len;
            ne->used_us = g_get_monotonic_time();
            g_hash_table_insert(g_mem, g_strdup(key), ne);
            g_mem_bytes += disk_len;
        }
    }
    g_mutex_unlock(&g_lock);
    if (out_len) *out_len = disk_len;
    return disk;
}

/* Stores compiled bytecode in memory and on disk unless already cached. */
void
ns_bytecode_cache_put(const char *top_url, const char *name,
                      const char *src, gsize src_len,
                      const guint8 *bc, gsize bc_len)
{
    if (!src || !src_len || !bc || !bc_len || cache_bypassed()) return;
    if (bc_len > NS_BYTECODE_CACHE_VALUE_CAP_BYTES) return;
    char key[65];
    hash_source(top_url, name, src, src_len, key);

    g_mutex_lock(&g_lock);
    if (!g_mem) { g_mutex_unlock(&g_lock); return; }
    ns_bytecode_cache_entry *existing = g_hash_table_lookup(g_mem, key);
    if (existing) {
        existing->used_us = g_get_monotonic_time();
        g_mutex_unlock(&g_lock);
        return;
    }
    evict_until_fits(bc_len);
    ns_bytecode_cache_entry *ne = g_new0(ns_bytecode_cache_entry, 1);
    ne->bytes = g_memdup2(bc, bc_len);
    ne->len = bc_len;
    ne->used_us = g_get_monotonic_time();
    g_hash_table_insert(g_mem, g_strdup(key), ne);
    g_mem_bytes += bc_len;
    g_mutex_unlock(&g_lock);

    write_disk(key, bc, bc_len);
}

typedef struct {
    char    key[65];
    char   *top_url;
    char   *url;
    GBytes *source;
} precompile_job;

/* Clears a job's pending mark, wakes waiters and frees the job. */
static void
precompile_job_finish(precompile_job *job)
{
    g_mutex_lock(&g_lock);
    if (g_pending) g_hash_table_remove(g_pending, job->key);
    g_cond_broadcast(&g_pending_cond);
    g_mutex_unlock(&g_lock);
    g_bytes_unref(job->source);
    g_free(job->top_url);
    g_free(job->url);
    g_free(job);
}

/* Worker: compiles a script in a throwaway runtime and caches its bytecode. */
static void
precompile_run(gpointer data, gpointer user_data)
{
    (void)user_data;
    precompile_job *job = data;
    gsize len = 0;
    const char *src = g_bytes_get_data(job->source, &len);
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    if (ctx) {
        JS_SetMaxStackSize(rt, NS_BYTECODE_PRECOMPILE_STACK);
        JS_SetMemoryLimit(rt, NS_BYTECODE_PRECOMPILE_MEMORY);
        JSValue fn = JS_Eval(ctx, src, len, job->url,
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(fn)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            size_t bc_size = 0;
            uint8_t *bc = JS_WriteObject(ctx, &bc_size, fn,
                                         JS_WRITE_OBJ_BYTECODE);
            if (bc && bc_size > 0)
                ns_bytecode_cache_put(job->top_url, job->url, src, len,
                                      bc, bc_size);
            if (bc) js_free(ctx, bc);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeContext(ctx);
    }
    if (rt) JS_FreeRuntime(rt);
    precompile_job_finish(job);
}

/* Queues background compilation of a large classic script not yet cached. */
void
ns_bytecode_cache_precompile(const char *top_url, const char *url,
                             const guint8 *src, gsize len)
{
    if (!src || len < NS_BYTECODE_PRECOMPILE_MIN_BYTES || cache_bypassed())
        return;
    precompile_job *job = g_new0(precompile_job, 1);
    hash_source(top_url, url ? url : "", (const char *)src, len, job->key);
    g_mutex_lock(&g_lock);
    gboolean skip = !g_mem || g_hash_table_contains(g_mem, job->key) ||
                    (g_pending && g_hash_table_contains(g_pending, job->key));
    g_mutex_unlock(&g_lock);
    char *path = skip ? NULL : disk_path_for(job->key);
    if (path && g_file_test(path, G_FILE_TEST_EXISTS)) skip = TRUE;
    g_free(path);
    if (skip) {
        g_free(job);
        return;
    }
    g_mutex_lock(&g_lock);
    if (!g_pending)
        g_pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          NULL);
    g_hash_table_add(g_pending, g_strdup(job->key));
    if (!g_precompile_pool)
        g_precompile_pool = g_thread_pool_new(precompile_run, NULL, 2, FALSE,
                                              NULL);
    g_mutex_unlock(&g_lock);
    job->url = g_strdup(url ? url : "");
    job->top_url = g_strdup(top_url);
    char *terminated = g_malloc(len + 1);
    memcpy(terminated, src, len);
    terminated[len] = '\0';
    job->source = g_bytes_new_take(terminated, len);
    g_thread_pool_push(g_precompile_pool, job, NULL);
}
