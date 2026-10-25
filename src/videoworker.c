/* Northstar Web Browser — a video decoder running on its own thread, a few frames ahead.
 * Seeks start a new epoch that drops queued work; tracks a global decode-rate estimate.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "videoworker.h"
#include "trace.h"

#include <math.h>

static const double NS_VIDEO_WORKER_EPSILON_S = 1e-6;
enum { NS_VIDEO_WORKER_DEFAULT_PICTURES = 4 };

typedef struct {
    GBytes  *sample;
    GBytes  *config;
    double   pts;
    gboolean want_texture;
    guint    epoch;
} worker_job;

typedef struct {
    double      pts;
    ns_texture *texture;
} worker_picture;

struct ns_video_worker {
    ns_video_decoder *decoder;
    GThread          *thread;
    GMutex            lock;
    GCond             wake;
    GQueue            jobs;
    GArray           *pictures;
    guint             epoch;
    guint             flushed_epoch;
    guint             max_pictures;
    gboolean          decoding;
    double            decoding_pts;
    gboolean          decoding_wanted;
    gboolean          quit;
    guint             presented;
    guint             dropped;
};

static GMutex g_rate_lock;
static double g_pixel_rate;
static guint  g_rate_samples;

/* Folds one decode's pixels per second into a smoothed global decode-rate estimate. */
static void
note_decode_rate(ns_texture *texture, gint64 us)
{
    if (!texture || us <= 0) return;
    double px = (double)ns_texture_get_width(texture) *
                ns_texture_get_height(texture);
    double rate = px / ((double)us / 1e6);
    g_mutex_lock(&g_rate_lock);
    g_pixel_rate = g_pixel_rate > 0 ? g_pixel_rate * 0.9 + rate * 0.1 : rate;
    g_rate_samples++;
    g_mutex_unlock(&g_rate_lock);
}

/* Returns the smoothed decode rate in pixels/s, or 0 until 30 samples exist. */
double
ns_video_decode_pixel_rate(void)
{
    g_mutex_lock(&g_rate_lock);
    double rate = g_rate_samples >= 30 ? g_pixel_rate : 0.0;
    g_mutex_unlock(&g_rate_lock);
    return rate;
}

static void
job_free(worker_job *job)
{
    if (job->sample) g_bytes_unref(job->sample);
    if (job->config) g_bytes_unref(job->config);
    g_free(job);
}

static void
picture_clear(gpointer data)
{
    worker_picture *picture = data;
    ns_texture_unref(picture->texture);
}

static void
drop_jobs(ns_video_worker *worker)
{
    worker_job *job;
    while ((job = g_queue_pop_head(&worker->jobs)) != NULL) job_free(job);
}

/* Decodes one sample (flushing first if asked) and queues the picture if still current. */
static void
decode_job(ns_video_worker *worker, worker_job *job, gboolean flush)
{
    gint64 trace_start = ns_trace_now();
    gint64 decode_start = g_get_monotonic_time();
    if (flush) ns_video_decoder_flush(worker->decoder);
    gint64 stamp = (gint64)llround(job->pts * 1e6);
    ns_texture *texture = ns_video_decoder_decode(worker->decoder, job->sample,
                                                  job->config, stamp,
                                                  job->want_texture);
    if (texture) note_decode_rate(texture, g_get_monotonic_time() - decode_start);
    int tex_w = texture ? ns_texture_get_width(texture) : 0;
    int tex_h = texture ? ns_texture_get_height(texture) : 0;
    g_mutex_lock(&worker->lock);
    worker->decoding = FALSE;
    if (job->want_texture && job->epoch == worker->epoch) {
        worker_picture picture = { job->pts, texture };
        g_array_append_val(worker->pictures, picture);
    } else {
        ns_texture_unref(texture);
    }
    g_mutex_unlock(&worker->lock);
    if (trace_start)
        ns_trace_completef("media", "video decode", trace_start,
                           "pts %.3f %dx%d%s%s", job->pts, tex_w, tex_h,
                           job->want_texture ? "" : " (skipped)",
                           flush ? " after flush" : "");
}

/* Returns TRUE if the next job can run without exceeding the picture queue limit. */
static gboolean
worker_may_decode(ns_video_worker *worker)
{
    const worker_job *job = g_queue_peek_head(&worker->jobs);
    if (!job) return FALSE;
    return !job->want_texture || job->epoch != worker->epoch ||
           worker->pictures->len < worker->max_pictures;
}

/* Decode thread loop: runs jobs of the current epoch, flushing after each restart. */
static gpointer
worker_main(gpointer data)
{
    ns_video_worker *worker = data;
    ns_trace_thread_name_once("ns-video-decode");
    g_mutex_lock(&worker->lock);
    while (!worker->quit) {
        if (!worker_may_decode(worker)) {
            g_cond_wait(&worker->wake, &worker->lock);
            continue;
        }
        worker_job *job = g_queue_pop_head(&worker->jobs);
        if (job->epoch != worker->epoch) {
            job_free(job);
            continue;
        }
        gboolean flush = worker->flushed_epoch != worker->epoch;
        worker->flushed_epoch = worker->epoch;
        worker->decoding = TRUE;
        worker->decoding_pts = job->pts;
        worker->decoding_wanted = job->want_texture;
        g_mutex_unlock(&worker->lock);
        decode_job(worker, job, flush);
        job_free(job);
        g_mutex_lock(&worker->lock);
    }
    g_mutex_unlock(&worker->lock);
    return NULL;
}

/* Creates a decode worker thread that takes ownership of decoder. */
ns_video_worker *
ns_video_worker_new(ns_video_decoder *decoder)
{
    if (!decoder) return NULL;
    ns_video_worker *worker = g_new0(ns_video_worker, 1);
    worker->decoder = decoder;
    g_mutex_init(&worker->lock);
    g_cond_init(&worker->wake);
    g_queue_init(&worker->jobs);
    worker->max_pictures = NS_VIDEO_WORKER_DEFAULT_PICTURES;
    worker->pictures = g_array_new(FALSE, FALSE, sizeof(worker_picture));
    g_array_set_clear_func(worker->pictures, picture_clear);
    worker->thread = g_thread_new("ns-video-decode", worker_main, worker);
    return worker;
}

/* Stops and joins the worker thread, then frees the decoder and queued pictures. */
void
ns_video_worker_free(ns_video_worker *worker)
{
    if (!worker) return;
    g_mutex_lock(&worker->lock);
    worker->quit = TRUE;
    drop_jobs(worker);
    g_cond_signal(&worker->wake);
    g_mutex_unlock(&worker->lock);
    g_thread_join(worker->thread);
    ns_video_decoder_free(worker->decoder);
    g_array_free(worker->pictures, TRUE);
    g_cond_clear(&worker->wake);
    g_mutex_clear(&worker->lock);
    g_free(worker);
}

/* Starts a new epoch (e.g. after a seek), dropping queued jobs and decoded pictures. */
void
ns_video_worker_restart(ns_video_worker *worker)
{
    if (!worker) return;
    g_mutex_lock(&worker->lock);
    worker->epoch++;
    drop_jobs(worker);
    g_array_set_size(worker->pictures, 0);
    g_cond_signal(&worker->wake);
    g_mutex_unlock(&worker->lock);
}

/* Queues a sample for decoding; want_texture FALSE decodes only to advance state. */
void
ns_video_worker_push(ns_video_worker *worker, GBytes *sample, GBytes *config,
                     double pts, gboolean want_texture)
{
    if (!worker || !sample) return;
    worker_job *job = g_new0(worker_job, 1);
    job->sample = g_bytes_ref(sample);
    job->config = config ? g_bytes_ref(config) : NULL;
    job->pts = pts;
    job->want_texture = want_texture;
    g_mutex_lock(&worker->lock);
    job->epoch = worker->epoch;
    g_queue_push_tail(&worker->jobs, job);
    g_cond_signal(&worker->wake);
    g_mutex_unlock(&worker->lock);
}

void
ns_video_worker_set_max_pictures(ns_video_worker *worker, guint max_pictures)
{
    if (!worker || max_pictures < 1) return;
    g_mutex_lock(&worker->lock);
    if (worker->max_pictures != max_pictures) {
        worker->max_pictures = max_pictures;
        g_cond_signal(&worker->wake);
    }
    g_mutex_unlock(&worker->lock);
}

/* Returns TRUE if a picture for pts is decoding, decoded or queued. */
gboolean
ns_video_worker_expects(ns_video_worker *worker, double pts)
{
    if (!worker) return FALSE;
    g_mutex_lock(&worker->lock);
    gboolean found = worker->decoding && worker->decoding_wanted &&
                     fabs(worker->decoding_pts - pts) < NS_VIDEO_WORKER_EPSILON_S;
    for (guint i = 0; !found && i < worker->pictures->len; i++)
        found = fabs(g_array_index(worker->pictures, worker_picture, i).pts - pts) <
                NS_VIDEO_WORKER_EPSILON_S;
    for (GList *l = worker->jobs.head; !found && l; l = l->next) {
        const worker_job *job = l->data;
        found = job->want_texture &&
                fabs(job->pts - pts) < NS_VIDEO_WORKER_EPSILON_S;
    }
    g_mutex_unlock(&worker->lock);
    return found;
}

void
ns_video_worker_counts(ns_video_worker *worker, guint *presented,
                       guint *dropped)
{
    *presented = *dropped = 0;
    if (!worker) return;
    g_mutex_lock(&worker->lock);
    *presented = worker->presented;
    *dropped = worker->dropped;
    g_mutex_unlock(&worker->lock);
}

/* Returns the number of decoded pictures and the newest pts among them. */
guint
ns_video_worker_ready(ns_video_worker *worker, double *newest_pts)
{
    if (newest_pts) *newest_pts = -1.0;
    if (!worker) return 0;
    g_mutex_lock(&worker->lock);
    guint n = 0;
    for (guint i = 0; i < worker->pictures->len; i++) {
        const worker_picture *p =
            &g_array_index(worker->pictures, worker_picture, i);
        if (!p->texture) continue;
        n++;
        if (newest_pts && p->pts > *newest_pts) *newest_pts = p->pts;
    }
    g_mutex_unlock(&worker->lock);
    return n;
}

/* Takes the newest picture at or before pts, dropping older ones; caller owns texture. */
ns_texture *
ns_video_worker_take(ns_video_worker *worker, double pts, double *taken_pts)
{
    if (!worker) return NULL;
    g_mutex_lock(&worker->lock);
    gint best = -1;
    for (guint i = 0; i < worker->pictures->len; i++) {
        const worker_picture *picture =
            &g_array_index(worker->pictures, worker_picture, i);
        if (picture->texture && picture->pts <= pts + NS_VIDEO_WORKER_EPSILON_S &&
            (best < 0 ||
             picture->pts > g_array_index(worker->pictures, worker_picture, best).pts))
            best = (gint)i;
    }
    ns_texture *texture = NULL;
    if (best >= 0) {
        worker_picture *picture = &g_array_index(worker->pictures, worker_picture, best);
        texture = picture->texture;
        picture->texture = NULL;
        if (taken_pts) *taken_pts = picture->pts;
        worker->presented++;
    }
    for (guint i = 0; i < worker->pictures->len;) {
        const worker_picture *p = &g_array_index(worker->pictures, worker_picture, i);
        if (p->pts <= pts + NS_VIDEO_WORKER_EPSILON_S) {
            if (p->texture) worker->dropped++;
            g_array_remove_index(worker->pictures, i);
        } else {
            i++;
        }
    }
    g_cond_signal(&worker->wake);
    g_mutex_unlock(&worker->lock);
    return texture;
}
