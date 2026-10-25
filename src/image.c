/* Northstar Web Browser — image cache and the decode chain (Wuffs, ICO, AVIF, SVG, MPEG-1).
 * Async fetch and worker decode, retry, animation timelines, video streams, LRU memory budget.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "image.h"

/* True if a lowercased bare MIME type is decodable by the built-in decoders. */
static gboolean
ns_image_builtin_supports_mime(const char *bare)
{
    static const char *const types[] = {
        "image/png", "image/jpeg", "image/jpg", "image/gif",
        "image/bmp", "image/x-icon",
        "image/vnd.microsoft.icon",
        NULL
    };
    for (int i = 0; types[i]; i++)
        if (g_str_equal(bare, types[i])) return TRUE;
    if (g_str_equal(bare, "image/svg+xml")) return TRUE;
    if (g_str_equal(bare, "image/webp")) return TRUE;
#ifdef NS_HAVE_AVIF
    if (g_str_equal(bare, "image/avif")) return TRUE;
#endif
    return FALSE;
}
#include <math.h>
#include <string.h>
#include <cairo.h>

#include "config.h"
#include "net.h"
#include "svg.h"
#include "video.h"

#ifdef G_OS_WIN32
/* Windows: true for AVIF/HEIF/HEIC/JXL MIME types, which are not decoded there. */
static gboolean
ns_image_mime_blocked_on_platform(const char *bare)
{
    return g_str_equal(bare, "image/avif")  ||
           g_str_equal(bare, "image/heif")  ||
           g_str_equal(bare, "image/heic")  ||
           g_str_equal(bare, "image/heif-sequence") ||
           g_str_equal(bare, "image/heic-sequence") ||
           g_str_equal(bare, "image/jxl");
}

/* Windows: sniffs JPEG XL and ISO-BMFF AVIF/HEIF brands so they are refused. */
static gboolean
ns_image_bytes_blocked_on_platform(const guchar *data, gsize len)
{
    if (!data || len < 12) return FALSE;
    if (memcmp(data, "\xFF\x0A", 2) == 0) return TRUE;
    if (memcmp(data, "\x00\x00\x00", 3) == 0 &&
        memcmp(data + 4, "JXL ", 4) == 0) return TRUE;
    if (memcmp(data + 4, "ftyp", 4) != 0) return FALSE;
    static const char *const brands[] = {
        "avif", "avis", "heic", "heix", "hevc", "hevx",
        "mif1", "msf1", "heim", "heis", "hevm", "hevs",
        NULL
    };
    for (int i = 0; brands[i]; i++)
        if (memcmp(data + 8, brands[i], 4) == 0) return TRUE;
    return FALSE;
}
#endif

enum { NS_IMAGE_CACHE_BUDGET_BYTES = 256 * 1024 * 1024 };

struct ns_image_cache {
    GHashTable *by_url;
    GPtrArray  *pending;
    guint64     gen;
    gint64      total_bytes;
};

typedef struct ns_pending {
    ns_image          *img;
    ns_image_cache    *cache;
    ns_image_ready_cb  cb;
    gpointer           user_data;
    gboolean           dead;
} ns_pending;

static void
ns_image_anim_frame_clear(gpointer data)
{
    ns_image_anim_frame *f = data;
    if (f) ns_texture_unref(f->texture);
}

void
ns_image_pixel_frame_clear(gpointer data)
{
    ns_image_pixel_frame *f = data;
    if (f) g_free(f->pixels);
}

/* Frees a cache entry including its frames, video stream or texture. */
static void
ns_image_free(gpointer p)
{
    ns_image *img = p;
    if (!img) return;
    g_free(img->url);
    g_free(img->final_url);
    g_free(img->cors_allow_origin);
    g_free(img->error);
    if (img->render_surface) cairo_surface_destroy(img->render_surface);
    if (img->anim_frames) g_array_free(img->anim_frames, TRUE);
    else if (img->video) ns_video_stream_free(img->video);
    else ns_texture_unref(img->texture);
    g_free(img);
}

/* Estimates decoded memory: video stream size or 4 bytes/pixel per frame. */
static gint64
ns_image_decoded_bytes(const ns_image *img)
{
    if (!img) return 0;
    if (img->video) return (gint64)ns_video_stream_memory(img->video);
    if (img->anim_frames && img->anim_frames->len > 0) {
        gint64 total = 0;
        for (guint i = 0; i < img->anim_frames->len; i++) {
            const ns_image_anim_frame *f =
                &g_array_index(img->anim_frames, ns_image_anim_frame, i);
            if (!f->texture) continue;
            int w = ns_texture_get_width(f->texture);
            int h = ns_texture_get_height(f->texture);
            if (w > 0 && h > 0) total += (gint64)w * (gint64)h * 4;
        }
        return total;
    }
    if (img->texture) {
        int w = ns_texture_get_width(img->texture);
        int h = ns_texture_get_height(img->texture);
        if (w > 0 && h > 0) return (gint64)w * (gint64)h * 4;
    }
    return 0;
}

/* Charges a newly loaded image's decoded size to the cache budget once. */
static void
ns_image_cache_account(ns_image_cache *cache, ns_image *img)
{
    if (!cache || !img || !img->loaded || img->bytes != 0) return;
    img->bytes = ns_image_decoded_bytes(img);
    img->gen = cache->gen;
    cache->total_bytes += img->bytes;
}

/* True if a live pending request exists for img. */
static gboolean
ns_image_has_pending(const ns_image_cache *cache, const ns_image *img)
{
    for (guint i = 0; i < cache->pending->len; i++) {
        const ns_pending *p = g_ptr_array_index(cache->pending, i);
        if (!p->dead && p->img == img) return TRUE;
    }
    return FALSE;
}

/* Drops an image's decoded pixels to reclaim budget; it refetches on next get. */
static void
ns_image_cache_purge(ns_image_cache *cache, ns_image *img)
{
    cache->total_bytes -= img->bytes;
    img->bytes = 0;
    if (img->anim_frames) {
        g_array_free(img->anim_frames, TRUE);
        img->anim_frames = NULL;
        img->texture = NULL;
    } else if (img->video) {
        ns_video_stream_free(img->video);
        img->video = NULL;
        img->texture = NULL;
    } else {
        ns_texture_clear(&img->texture);
    }
    if (img->render_surface) {
        cairo_surface_destroy(img->render_surface);
        img->render_surface = NULL;
    }
    img->anim_current = 0;
    img->anim_total_ms = 0;
    img->anim_start_us = 0;
    img->loaded = FALSE;
    img->purged = TRUE;
}

ns_image_cache *
ns_image_cache_new(void)
{
    ns_image_cache *c = g_new0(ns_image_cache, 1);
    c->by_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ns_image_free);
    c->pending = g_ptr_array_new();
    return c;
}

/* Frees the cache, marking outstanding requests dead so callbacks are dropped. */
void
ns_image_cache_free(ns_image_cache *cache)
{
    if (!cache) return;
    for (guint i = 0; i < cache->pending->len; i++) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        p->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache);
}

/* Removes every pending request for the image and invokes their callbacks. */
static void
ns_image_fire_pending(ns_pending *pending)
{
    ns_image *img = pending->img;
    ns_image_cache *cache = pending->cache;
    GPtrArray *fire = g_ptr_array_new();
    for (guint i = 0; i < cache->pending->len; ) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        if (p->img == img) {
            g_ptr_array_remove_index_fast(cache->pending, i);
            g_ptr_array_add(fire, p);
        } else {
            i++;
        }
    }
    for (guint i = 0; i < fire->len; i++) {
        ns_pending *p = g_ptr_array_index(fire, i);
        if (p->cb) p->cb(img, p->user_data);
        g_free(p);
    }
    g_ptr_array_free(fire, TRUE);
}

/* True if a Content-Type (parameters ignored) is a decodable image type. */
gboolean
ns_image_supports_mime(const char *mime)
{
    if (!mime || !*mime) return FALSE;
    while (g_ascii_isspace(*mime)) mime++;
    const char *end = mime;
    while (*end && *end != ';' && !g_ascii_isspace(*end)) end++;
    if (end == mime) return FALSE;
    gchar *bare = g_ascii_strdown(mime, end - mime);
#ifdef G_OS_WIN32
    if (ns_image_mime_blocked_on_platform(bare)) {
        g_free(bare);
        return FALSE;
    }
#endif
    gboolean ok = ns_image_builtin_supports_mime(bare);
    g_free(bare);
    return ok;
}

/* Decodes a still image by sniffing: ICO, Wuffs formats, AVIF, then SVG; NULL if none. */
static ns_texture *
ns_image_decode_bytes(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len == 0) return NULL;

    if (len >= 4 && data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || data[2] == 2) && data[3] == 0) {
        ns_texture *tex = ns_image_decode_ico(data, len, out_w, out_h);
        if (tex) return tex;
    }

    if (ns_image_wuffs_supports_bytes(data, len)) {
        ns_texture *tex = ns_image_decode_wuffs(data, len, out_w, out_h);
        if (tex) return tex;
    }

#ifdef NS_HAVE_AVIF
    if (ns_image_avif_supports_bytes(data, len)) {
        ns_texture *tex = ns_image_decode_avif(data, len, out_w, out_h);
        if (tex) return tex;
    }
#endif

#ifdef G_OS_WIN32
    if (ns_image_bytes_blocked_on_platform(data, len)) return NULL;
#endif

    if (ns_svg_bytes_look_like_svg(data, len)) {
        ns_texture *tex = ns_svg_decode_bytes(data, len, out_w, out_h);
        if (tex) return tex;
    }

    return NULL;
}

/* Downloads a texture to a new tightly packed BGRA buffer and unrefs it; caller frees. */
static guint8 *
ns_image_texture_to_pixels(ns_texture *tex, int w, int h,
                           gsize *out_stride, gsize *out_buf_len)
{
    if (!tex || w <= 0 || h <= 0) {
        ns_texture_unref(tex);
        return NULL;
    }
    if ((gsize)w > G_MAXSIZE / 4 ||
        (gsize)h > G_MAXSIZE / ((gsize)w * 4)) {
        ns_texture_unref(tex);
        return NULL;
    }
    gsize stride = (gsize)w * 4;
    gsize buf_len = stride * (gsize)h;
    guint8 *pixels = g_try_malloc(buf_len);
    if (!pixels) {
        ns_texture_unref(tex);
        return NULL;
    }
    ns_texture_download(tex, pixels, stride);
    ns_texture_unref(tex);
    if (out_stride) *out_stride = stride;
    if (out_buf_len) *out_buf_len = buf_len;
    return pixels;
}

/* Decodes image bytes to a new pixel buffer, reporting size, stride and format. */
guint8 *
ns_image_decode_bytes_to_pixels(const guchar *data, gsize len,
                                int *out_w, int *out_h,
                                gsize *out_stride, gsize *out_buf_len,
                                ns_texture_format *out_format)
{
    if (!data || len == 0) return NULL;

    if (ns_image_wuffs_supports_bytes(data, len)) {
        guint8 *pix = ns_image_wuffs_decode_to_bgra(data, len, out_w, out_h,
                                                    out_stride, out_buf_len);
        if (pix) {
            if (out_format) *out_format = NS_TEXTURE_BGRA_PREMULTIPLIED;
            return pix;
        }
    }

#ifdef NS_HAVE_AVIF
    if (ns_image_avif_supports_bytes(data, len)) {
        int w = 0, h = 0;
        ns_texture *tex = ns_image_decode_avif(data, len, &w, &h);
        guint8 *pix = ns_image_texture_to_pixels(tex, w, h,
                                                 out_stride, out_buf_len);
        if (pix) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            if (out_format) *out_format = NS_TEXTURE_BGRA_PREMULTIPLIED;
            return pix;
        }
    }
#endif

#ifdef G_OS_WIN32
    if (ns_image_bytes_blocked_on_platform(data, len)) return NULL;
#endif

    int w = 0, h = 0;
    ns_texture *tex = ns_image_decode_bytes(data, len, &w, &h);
    guint8 *pixels = ns_image_texture_to_pixels(tex, w, h,
                                                out_stride, out_buf_len);
    if (!pixels) return NULL;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_format) *out_format = NS_TEXTURE_DEFAULT;
    return pixels;
}

/* Turns decoded pixel frames into texture frames (100 ms default delay); takes pixels. */
static GArray *
ns_image_anim_frames_from_pixels(GArray *pixel_frames,
                                 int *out_w, int *out_h,
                                 int *out_total_ms)
{
    if (!pixel_frames || pixel_frames->len == 0) return NULL;
    GArray *frames = g_array_new(FALSE, FALSE, sizeof(ns_image_anim_frame));
    g_array_set_clear_func(frames, ns_image_anim_frame_clear);
    gboolean ok = TRUE;
    gint64 total = 0;
    int w = 0, h = 0;
    for (guint i = 0; i < pixel_frames->len; i++) {
        ns_image_pixel_frame *pf =
            &g_array_index(pixel_frames, ns_image_pixel_frame, i);
        if (!pf->pixels || pf->pixels_len == 0 ||
            pf->width <= 0 || pf->height <= 0) {
            ok = FALSE;
            break;
        }
        GBytes *bytes = g_bytes_new_take(pf->pixels, pf->pixels_len);
        pf->pixels = NULL;
        ns_texture *tex = ns_texture_new(pf->width, pf->height, pf->format,
                                         bytes, pf->stride);
        g_bytes_unref(bytes);
        if (!tex) {
            ok = FALSE;
            break;
        }
        int delay = pf->delay_ms > 0 ? pf->delay_ms : 100;
        ns_image_anim_frame f = { tex, delay };
        g_array_append_val(frames, f);
        total += delay;
        if (i == 0) {
            w = pf->width;
            h = pf->height;
        }
    }
    if (!ok || frames->len == 0) {
        g_array_free(frames, TRUE);
        return NULL;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_total_ms) *out_total_ms = (int)CLAMP(total, 1, G_MAXINT / 2);
    return frames;
}

/* Decodes animation frames for GIF or animated PNG data; NULL otherwise. */
static GArray *
ns_image_pixel_frames_for(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if ((len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') ||
        ns_image_png_is_animated(data, len))
        return ns_image_decode_wuffs_anim_to_pixels(data, len, out_w, out_h);
    return NULL;
}

typedef struct {
    ns_texture      *tex;
    GArray          *frames;
    ns_video_stream *video;
    int              w;
    int              h;
} ns_img_decoded;

/* Releases whatever a decode result holds (frames, video or texture). */
static void
ns_img_decoded_clear(ns_img_decoded *d)
{
    if (d->frames) {
        g_array_set_clear_func(d->frames, ns_image_anim_frame_clear);
        g_array_free(d->frames, TRUE);
    }
    ns_video_stream_free(d->video);
    if (d->tex) ns_texture_unref(d->tex);
}

/* Decodes a response body as MPEG-1 video, animated GIF/APNG, or a still image. */
static ns_img_decoded
ns_image_decode_body(const guchar *data, gsize len)
{
    ns_img_decoded d = { NULL, NULL, NULL, 0, 0 };
    int w = 0, h = 0;
    GArray *frames = NULL;
    if (ns_video_bytes_are_mpeg1(data, len)) {
        d.video = ns_video_stream_new(data, len);
        d.w = ns_video_stream_width(d.video);
        d.h = ns_video_stream_height(d.video);
        return d;
    }
    GArray *pixel_frames = ns_image_pixel_frames_for(data, len, &w, &h);
    if (pixel_frames) {
        frames = ns_image_anim_frames_from_pixels(pixel_frames, &w, &h, NULL);
        g_array_free(pixel_frames, TRUE);
    }
    if (frames && frames->len > 0) {
        d.frames = frames;
        d.w = w;
        d.h = h;
        return d;
    }
    if (frames) {
        g_array_set_clear_func(frames, ns_image_anim_frame_clear);
        g_array_free(frames, TRUE);
    }
    d.tex = ns_image_decode_bytes(data, len, &w, &h);
    d.w = w;
    d.h = h;
    return d;
}

/* Moves a decode result into the image and sets loaded/animation state or failure. */
static void
ns_image_apply_decoded_state(ns_image *img, ns_img_decoded *d,
                             const char *content_type, gsize body_len)
{
    if (d->video) {
        img->video = d->video;
        img->texture = ns_video_stream_texture(d->video);
        img->natural_width  = d->w;
        img->natural_height = d->h;
        img->anim_total_ms = ns_video_stream_duration_ms(d->video);
        img->anim_start_us = g_get_monotonic_time();
        img->anim_loop = FALSE;
        img->anim_paused = TRUE;
        img->anim_paused_phase_ms = 0;
        img->loaded = TRUE;
    } else if (d->frames) {
        g_array_set_clear_func(d->frames, ns_image_anim_frame_clear);
        img->anim_frames = d->frames;
        ns_image_anim_frame *f0 =
            &g_array_index(d->frames, ns_image_anim_frame, 0);
        img->texture = f0->texture;
        img->natural_width  = d->w;
        img->natural_height = d->h;
        gint64 total = 0;
        for (guint i = 0; i < d->frames->len; i++)
            total += g_array_index(d->frames, ns_image_anim_frame, i).delay_ms;
        img->anim_total_ms = (int)CLAMP(total, 1, G_MAXINT / 2);
        img->anim_start_us = g_get_monotonic_time();
        img->anim_loop = TRUE;
        img->anim_paused = FALSE;
        img->anim_paused_phase_ms = 0;
        img->loaded = TRUE;
    } else if (d->tex) {
        img->texture = d->tex;
        img->natural_width  = d->w;
        img->natural_height = d->h;
        img->loaded = TRUE;
    } else {
        img->failed = TRUE;
        img->failed_at_us = g_get_monotonic_time();
        img->error = g_strdup_printf("could not decode image (%s, %u bytes)",
            content_type && *content_type ? content_type : "unknown type",
            (unsigned)body_len);
    }
}

typedef struct {
    ns_pending *pending;
    GBytes     *body;
    char       *content_type;
    gsize       body_len;
} ns_decode_job;

static void
ns_decode_job_free(ns_decode_job *job)
{
    if (!job) return;
    if (job->body) g_bytes_unref(job->body);
    g_free(job->content_type);
    g_free(job);
}

/* GTask worker decoding an image body off the main thread. */
static void
image_decode_worker(GTask *task, gpointer source, gpointer task_data,
                    GCancellable *cancellable)
{
    (void)source;
    (void)cancellable;
    ns_decode_job *job = task_data;
    gsize len = 0;
    const guchar *data = g_bytes_get_data(job->body, &len);
    ns_img_decoded *d = g_new(ns_img_decoded, 1);
    *d = ns_image_decode_body(data, len);
    g_task_return_pointer(task, d, g_free);
}

/* Applies a worker decode result to its image and fires callbacks unless cancelled. */
static void
on_image_decoded(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    ns_decode_job *job = user_data;
    ns_img_decoded *d = g_task_propagate_pointer(G_TASK(res), NULL);
    ns_pending *pending = job->pending;
    if (pending->dead) {
        if (d) {
            ns_img_decoded_clear(d);
            g_free(d);
        }
        g_free(pending);
        ns_decode_job_free(job);
        return;
    }
    ns_img_decoded result = { NULL, NULL, NULL, 0, 0 };
    if (d) {
        result = *d;
        g_free(d);
    }
    ns_image_apply_decoded_state(pending->img, &result,
                                 job->content_type, job->body_len);
    ns_image_cache_account(pending->cache, pending->img);
    ns_image_fire_pending(pending);
    ns_decode_job_free(job);
}

/* Image fetch completion: records status/CORS, then decodes inline or in a worker. */
static void
on_image_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    if (!resp) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = err && err->message
            ? g_strdup(err->message) : g_strdup("fetch failed");
        g_clear_error(&err);
        if (pending->cb) pending->cb(pending->img, pending->user_data);
        g_ptr_array_remove_fast(pending->cache->pending, pending);
        g_free(pending);
        return;
    }
    pending->img->http_status = resp->status;
    g_free(pending->img->final_url);
    pending->img->final_url = g_strdup(resp->final_url);
    g_free(pending->img->cors_allow_origin);
    pending->img->cors_allow_origin = g_strdup(resp->cors_allow_origin);
    if (resp->error) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = g_strdup(resp->error);
    } else if (resp->status >= 400) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = g_strdup_printf("HTTP %ld", resp->status);
    } else if (!resp->body || resp->body->len == 0) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = g_strdup("empty response");
    } else if (ns_config_get()->async_image_decode) {
        ns_decode_job *job = g_new0(ns_decode_job, 1);
        job->pending = pending;
        job->body = g_bytes_new(resp->body->data, resp->body->len);
        job->content_type =
            g_strdup(resp->content_type ? resp->content_type : "");
        job->body_len = resp->body->len;
        ns_response_free(resp);
        GTask *task = g_task_new(NULL, NULL, on_image_decoded, job);
        g_task_set_task_data(task, job, NULL);
        g_task_run_in_thread(task, image_decode_worker);
        g_object_unref(task);
        return;
    } else {
        ns_img_decoded d = ns_image_decode_body(resp->body->data,
                                                resp->body->len);
        ns_image_apply_decoded_state(pending->img, &d,
                                     resp->content_type, resp->body->len);
    }
    ns_response_free(resp);
    ns_image_cache_account(pending->cache, pending->img);
    ns_image_fire_pending(pending);
}

/* True if a failed load may retry: under 3 attempts, transient status, backoff elapsed. */
static gboolean
ns_image_should_retry(const ns_image *img, gint64 now_us)
{
    if (!img || !img->failed || img->attempts >= 3) return FALSE;
    if (img->http_status > 0 &&
        img->http_status != 408 &&
        img->http_status != 425 &&
        img->http_status != 429 &&
        img->http_status != 500 &&
        img->http_status != 502 &&
        img->http_status != 503 &&
        img->http_status != 504)
        return FALSE;
    gint64 wait_us = img->attempts <= 1 ? 2 * G_USEC_PER_SEC
                                        : 10 * G_USEC_PER_SEC;
    if (img->http_status == 429)
        wait_us = img->attempts <= 1 ? 15 * G_USEC_PER_SEC
                                     : 45 * G_USEC_PER_SEC;
    return img->failed_at_us == 0 || now_us - img->failed_at_us >= wait_us;
}

/* Registers a pending callback and starts the async image fetch. */
static void
ns_image_cache_start_request(ns_image_cache *cache,
                             ns_image *img,
                             const char *url,
                             const char *top_url,
                             ns_fetch_policy *policy,
                             ns_image_ready_cb cb,
                             gpointer user_data)
{
    ns_pending *pending = g_new0(ns_pending, 1);
    pending->img = img;
    pending->cache = cache;
    pending->cb = cb;
    pending->user_data = user_data;
    g_ptr_array_add(cache->pending, pending);
    img->attempts++;
    ns_net_request_async(
        url, top_url, "GET", NULL, 0, NULL,
        ns_net_accept_headers_for(NS_FETCH_DEST_IMAGE), NS_FETCH_DEST_IMAGE,
        policy, NULL, on_image_fetched, pending);
}

/* Returns the cached image for url, starting, retrying or refetching a load as needed. */
ns_image *
ns_image_cache_get(ns_image_cache *cache,
                   const char *url,
                   const char *top_url,
                   ns_fetch_policy *policy,
                   ns_image_ready_cb cb,
                   gpointer user_data)
{
    if (!cache || !url) return NULL;
    const ns_config *cfg = ns_config_get();
    ns_image *cached = g_hash_table_lookup(cache->by_url, url);
    if (cached) {
        cached->gen = cache->gen;
        if (cfg && !cfg->images_enabled) {
            if (cb && (cached->loaded || cached->failed))
                cb(cached, user_data);
            return cached;
        }
        if (cached->purged && !ns_image_has_pending(cache, cached)) {
            cached->purged = FALSE;
            ns_image_cache_start_request(cache, cached, url, top_url,
                                         policy, cb, user_data);
            return cached;
        }
        if (ns_image_should_retry(cached, g_get_monotonic_time())) {
            cached->failed = FALSE;
            cached->http_status = 0;
            cached->failed_at_us = 0;
            g_clear_pointer(&cached->error, g_free);
            ns_image_cache_start_request(cache, cached, url, top_url,
                                         policy, cb, user_data);
            return cached;
        }
        if (cached->loaded || cached->failed) {
            if (cb) cb(cached, user_data);
        } else if (cb) {
            gboolean dup = FALSE;
            for (guint i = 0; i < cache->pending->len; i++) {
                ns_pending *p = g_ptr_array_index(cache->pending, i);
                if (!p->dead && p->img == cached &&
                    p->cb == cb && p->user_data == user_data) {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup) {
                ns_pending *pending = g_new0(ns_pending, 1);
                pending->img = cached;
                pending->cache = cache;
                pending->cb = cb;
                pending->user_data = user_data;
                g_ptr_array_add(cache->pending, pending);
            }
        }
        return cached;
    }

    ns_image *img = g_new0(ns_image, 1);
    img->url = g_strdup(url);
    g_hash_table_insert(cache->by_url, g_strdup(url), img);

    if (cfg && !cfg->images_enabled) {
        img->failed = TRUE;
        if (cb) cb(img, user_data);
        return img;
    }

    ns_image_cache_start_request(cache, img, url, top_url, policy, cb,
                                 user_data);
    return img;
}

/* Detaches callbacks registered with user_data so they never fire. */
void
ns_image_cache_cancel_cb(ns_image_cache *cache, gpointer user_data)
{
    if (!cache || !cache->pending) return;
    for (guint i = 0; i < cache->pending->len; i++) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        if (p->user_data == user_data) {
            p->cb = NULL;
            p->user_data = NULL;
        }
    }
}

/* Looks up a cached image without fetching, marking it used this generation. */
ns_image *
ns_image_cache_peek(ns_image_cache *cache, const char *url)
{
    if (!cache || !url) return NULL;
    ns_image *img = g_hash_table_lookup(cache->by_url, url);
    if (img) img->gen = cache->gen;
    return img;
}

/* True for loaded videos or multi-frame animated images. */
gboolean
ns_image_is_animation(const ns_image *img)
{
    return img && img->loaded && img->anim_total_ms > 0 &&
           (img->video ||
            (img->anim_frames && img->anim_frames->len > 1));
}

GBytes *
ns_image_video_bytes(const ns_image *img)
{
    return img && img->loaded ? ns_video_stream_bytes(img->video) : NULL;
}

gboolean
ns_image_video_has_audio(const ns_image *img)
{
    return img && img->loaded && ns_video_stream_has_audio(img->video);
}

double
ns_image_anim_duration(const ns_image *img)
{
    return ns_image_is_animation(img) ? img->anim_total_ms / 1000.0 : 0.0;
}

/* Pushes the image's playback clock state into its MSE video stream. */
static void
image_sync_video_clock(const ns_image *img)
{
    if (!img || !img->video || !ns_video_stream_is_mse(img->video)) return;
    ns_video_clock clock = {
        .paused = img->anim_paused,
        .loop = img->anim_loop,
        .start_us = img->anim_start_us,
        .paused_ms = img->anim_paused_phase_ms,
        .total_ms = img->anim_total_ms,
        .seek_gen = img->anim_seek_gen,
    };
    ns_video_stream_set_clock(img->video, &clock);
}

/* Current playback position in ms, frozen when paused and wrapped when looping. */
static int
ns_image_anim_phase_ms(const ns_image *img, gint64 now_us)
{
    if (!ns_image_is_animation(img)) return 0;
    if (img->anim_paused) return img->anim_paused_phase_ms;
    gint64 elapsed_ms = (now_us - img->anim_start_us) / 1000;
    if (elapsed_ms < 0) elapsed_ms = 0;
    if (!img->anim_loop)
        return (int)MIN(elapsed_ms, (gint64)img->anim_total_ms);
    return (int)(elapsed_ms % img->anim_total_ms);
}

/* True if a non-looping animation has reached its end. */
gboolean
ns_image_anim_ended(const ns_image *img, gint64 now_us)
{
    return ns_image_is_animation(img) && !img->anim_loop &&
           ns_image_anim_phase_ms(img, now_us) >= img->anim_total_ms;
}

/* Changes looping while preserving the current playback position. */
void
ns_image_anim_set_loop(ns_image *img, gboolean loop, gint64 now_us)
{
    if (!ns_image_is_animation(img) || img->anim_loop == loop) return;
    int phase = ns_image_anim_phase_ms(img, now_us);
    img->anim_loop = loop;
    if (loop) phase %= img->anim_total_ms;
    if (img->anim_paused) img->anim_paused_phase_ms = phase;
    else img->anim_start_us = now_us - (gint64)phase * 1000;
    image_sync_video_clock(img);
}

double
ns_image_anim_position(const ns_image *img, gint64 now_us)
{
    return ns_image_anim_phase_ms(img, now_us) / 1000.0;
}

/* Pauses or resumes playback, preserving the current position. */
void
ns_image_anim_set_paused(ns_image *img, gboolean paused, gint64 now_us)
{
    if (!ns_image_is_animation(img) || img->anim_paused == paused) return;
    if (paused) {
        img->anim_paused_phase_ms = ns_image_anim_phase_ms(img, now_us);
        img->anim_paused = TRUE;
        image_sync_video_clock(img);
        return;
    }
    img->anim_paused = FALSE;
    img->anim_start_us = now_us - (gint64)img->anim_paused_phase_ms * 1000;
    image_sync_video_clock(img);
}

/* Seeks playback to seconds, clamped or wrapped to the duration; bumps seek generation. */
void
ns_image_anim_seek(ns_image *img, double seconds, gint64 now_us)
{
    if (!ns_image_is_animation(img)) return;
    if (!(seconds >= 0.0)) seconds = 0.0;
    double total_s = img->anim_total_ms / 1000.0;
    if (seconds > total_s)
        seconds = img->anim_loop ? fmod(seconds, total_s) : total_s;
    int phase = (int)(seconds * 1000.0);
    if (img->anim_loop) phase %= img->anim_total_ms;
    if (img->anim_paused) img->anim_paused_phase_ms = phase;
    else img->anim_start_us = now_us - (gint64)phase * 1000;
    img->anim_seek_gen++;
    image_sync_video_clock(img);
}

/* Shifts the start time of a playing animation by delta_us. */
void
ns_image_anim_shift(ns_image *img, gint64 delta_us)
{
    if (!ns_image_is_animation(img) || img->anim_paused) return;
    img->anim_start_us += delta_us;
    image_sync_video_clock(img);
}

/* Shows the video frame or animation frame for phase; TRUE if the texture changed. */
static gboolean
ns_image_apply_phase(ns_image *img, int phase)
{
    if (img->video) {
        if (ns_video_stream_is_mse(img->video))
            img->anim_total_ms = ns_video_stream_duration_ms(img->video);
        image_sync_video_clock(img);
        gboolean changed = ns_video_stream_show(img->video, phase);
        img->texture = ns_video_stream_texture(img->video);
        img->natural_width = ns_video_stream_width(img->video);
        img->natural_height = ns_video_stream_height(img->video);
        return changed;
    }
    int idx = 0;
    gint64 acc = 0;
    for (guint i = 0; i < img->anim_frames->len; i++) {
        ns_image_anim_frame *f =
            &g_array_index(img->anim_frames, ns_image_anim_frame, i);
        acc += f->delay_ms;
        if (phase < acc) { idx = (int)i; break; }
        idx = (int)i;
    }
    if (idx == img->anim_current) return FALSE;
    img->anim_current = idx;
    img->texture =
        g_array_index(img->anim_frames, ns_image_anim_frame, idx).texture;
    return TRUE;
}

gboolean
ns_image_cache_tick(ns_image_cache *cache, gint64 now_us)
{
    return ns_image_cache_tick_collect(cache, now_us, NULL);
}

/* Advances all animations to now, collecting images whose frame changed. */
gboolean
ns_image_cache_tick_collect(ns_image_cache *cache, gint64 now_us,
                            GPtrArray *changed)
{
    if (!cache) return FALSE;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        ns_image *img = value;
        if (!ns_image_is_animation(img)) continue;
        if (ns_image_apply_phase(img, ns_image_anim_phase_ms(img, now_us))) {
            any = TRUE;
            if (changed) g_ptr_array_add(changed, img);
        }
    }
    return any;
}

/* True if any animation is playing or a video is waiting for data. */
gboolean
ns_image_cache_animating(const ns_image_cache *cache)
{
    if (!cache) return FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, cache->by_url);
    gint64 now = g_get_monotonic_time();
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const ns_image *img = value;
        if (ns_image_is_animation(img) && !img->anim_paused &&
            !ns_image_anim_ended(img, now))
            return TRUE;
        if (img->video && ns_video_stream_waiting(img->video)) return TRUE;
    }
    return FALSE;
}

gboolean
ns_image_cache_has_pending(const ns_image_cache *cache)
{
    return cache && cache->pending->len > 0;
}

struct ns_image_decoding {
    ns_img_decoded decoded;
};

/* Decodes encoded bytes into a standalone decoding result; caller frees or inserts. */
ns_image_decoding *
ns_image_decode_encoded(const guchar *data, gsize len)
{
    ns_image_decoding *decoding = g_new(ns_image_decoding, 1);
    decoding->decoded = ns_image_decode_body(data, len);
    return decoding;
}

void
ns_image_decoding_free(ns_image_decoding *decoding)
{
    if (!decoding) return;
    ns_img_decoded_clear(&decoding->decoded);
    g_free(decoding);
}

/* Inserts a decode result under url unless present or empty; consumes decoding. */
ns_image *
ns_image_cache_insert_decoding(ns_image_cache *cache, const char *url,
                               ns_image_decoding *decoding)
{
    if (!cache || !url || !decoding) {
        ns_image_decoding_free(decoding);
        return NULL;
    }
    ns_image *existing = g_hash_table_lookup(cache->by_url, url);
    if (existing || (!decoding->decoded.frames && !decoding->decoded.tex &&
                     !decoding->decoded.video)) {
        ns_image_decoding_free(decoding);
        return existing;
    }
    ns_image *img = g_new0(ns_image, 1);
    img->url = g_strdup(url);
    ns_image_apply_decoded_state(img, &decoding->decoded, NULL, 0);
    g_free(decoding);
    g_hash_table_insert(cache->by_url, g_strdup(url), img);
    ns_image_cache_account(cache, img);
    return img;
}

/* Installs a video stream as url's image, replacing any prior content; takes stream. */
ns_image *
ns_image_cache_insert_stream(ns_image_cache *cache, const char *url,
                             ns_video_stream *stream)
{
    if (!cache || !url || !stream) {
        ns_video_stream_free(stream);
        return NULL;
    }
    ns_image *img = g_hash_table_lookup(cache->by_url, url);
    if (img) {
        if (img->anim_frames) g_array_free(img->anim_frames, TRUE);
        else if (img->video) ns_video_stream_free(img->video);
        else ns_texture_unref(img->texture);
        img->anim_frames = NULL;
        img->failed = FALSE;
        cache->total_bytes -= img->bytes;
        img->bytes = 0;
    } else {
        img = g_new0(ns_image, 1);
        img->url = g_strdup(url);
        g_hash_table_insert(cache->by_url, g_strdup(url), img);
    }
    img->video = stream;
    img->texture = ns_video_stream_texture(stream);
    img->anim_total_ms = ns_video_stream_duration_ms(stream);
    img->anim_start_us = g_get_monotonic_time();
    img->anim_paused = TRUE;
    img->anim_paused_phase_ms = 0;
    img->anim_loop = FALSE;
    img->loaded = TRUE;
    return img;
}

/* Decodes and inserts bytes under url, or returns the existing entry. */
ns_image *
ns_image_cache_insert_encoded(ns_image_cache *cache, const char *url,
                              const guchar *data, gsize len)
{
    if (!cache || !url || !data || len == 0) return NULL;
    ns_image *existing = g_hash_table_lookup(cache->by_url, url);
    if (existing) return existing;
    return ns_image_cache_insert_decoding(cache, url,
                                          ns_image_decode_encoded(data, len));
}

void
ns_image_cache_begin_generation(ns_image_cache *cache)
{
    if (cache) cache->gen++;
}

/* Purges least-recently-used images from older generations until under the 256 MiB budget. */
void
ns_image_cache_collect(ns_image_cache *cache)
{
    if (!cache) return;
    while (cache->total_bytes > NS_IMAGE_CACHE_BUDGET_BYTES) {
        ns_image *victim = NULL;
        guint64 oldest = G_MAXUINT64;
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, cache->by_url);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            ns_image *img = value;
            if (!img->loaded || img->bytes == 0) continue;
            if (img->gen == cache->gen) continue;
            if (ns_image_has_pending(cache, img)) continue;
            if (img->gen < oldest) {
                oldest = img->gen;
                victim = img;
            }
        }
        if (!victim) break;
        ns_image_cache_purge(cache, victim);
    }
}
