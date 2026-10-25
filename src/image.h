/* Northstar Web Browser — image cache API: decode, frames and animation timelines.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_IMAGE_H
#define NS_IMAGE_H

#include <glib.h>

#include "fetch_policy.h"
#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_image_cache  ns_image_cache;
typedef struct ns_image        ns_image;
typedef struct ns_video_stream ns_video_stream;

typedef struct ns_image_anim_frame {
    ns_texture *texture;
    int         delay_ms;
} ns_image_anim_frame;

typedef struct ns_image_pixel_frame {
    guint8            *pixels;
    gsize              pixels_len;
    gsize              stride;
    ns_texture_format  format;
    int                width;
    int                height;
    int                delay_ms;
} ns_image_pixel_frame;

struct ns_image {
    char        *url;
    char        *final_url;
    char        *cors_allow_origin;
    ns_texture  *texture;
    void        *render_surface;
    int          natural_width;
    int          natural_height;
    long         http_status;
    char        *error;
    gint64       failed_at_us;
    int          attempts;
    gboolean     loaded;
    gboolean     failed;
    gboolean     purged;
    gint64       bytes;
    guint64      gen;
    GArray      *anim_frames;
    gint64       anim_start_us;
    int          anim_current;
    int          anim_total_ms;
    gboolean     anim_paused;
    int          anim_paused_phase_ms;
    gboolean     anim_loop;
    guint        anim_seek_gen;
    ns_video_stream *video;
};

typedef void (*ns_image_ready_cb)(ns_image *img, gpointer user_data);

ns_image_cache *ns_image_cache_new(void);
void            ns_image_cache_free(ns_image_cache *cache);

ns_image       *ns_image_cache_get(ns_image_cache *cache,
                                   const char     *url,
                                   const char     *top_url,
                                   ns_fetch_policy *policy,
                                   ns_image_ready_cb cb,
                                   gpointer        user_data);

void            ns_image_cache_cancel_cb(ns_image_cache *cache,
                                         gpointer user_data);

ns_image       *ns_image_cache_peek(ns_image_cache *cache, const char *url);

ns_image       *ns_image_cache_insert_stream(ns_image_cache  *cache,
                                             const char      *url,
                                             ns_video_stream *stream);

ns_image       *ns_image_cache_insert_encoded(ns_image_cache *cache,
                                              const char     *url,
                                              const guchar   *data,
                                              gsize           len);

typedef struct ns_image_decoding ns_image_decoding;

ns_image_decoding *ns_image_decode_encoded(const guchar *data, gsize len);
ns_image       *ns_image_cache_insert_decoding(ns_image_cache    *cache,
                                               const char        *url,
                                               ns_image_decoding *decoding);
void            ns_image_decoding_free(ns_image_decoding *decoding);

ns_texture *ns_image_decode_wuffs(const guchar *data, gsize len,
                                  int *out_w, int *out_h);

gboolean ns_image_png_is_animated(const guchar *data, gsize len);

GArray *ns_image_decode_wuffs_anim_to_pixels(const guchar *data, gsize len,
                                             int *out_w, int *out_h);

guint8 *ns_image_wuffs_decode_to_bgra(const guchar *data, gsize len,
                                      int *out_w, int *out_h,
                                      gsize *out_stride, gsize *out_buf_len);

gboolean ns_image_wuffs_supports_bytes(const guchar *data, gsize len);

void ns_image_pixel_frame_clear(gpointer data);

guint8 *ns_image_decode_bytes_to_pixels(const guchar *data, gsize len,
                                        int *out_w, int *out_h,
                                        gsize *out_stride,
                                        gsize *out_buf_len,
                                        ns_texture_format *out_format);

ns_texture *ns_image_decode_ico(const guchar *data, gsize len,
                                int *out_w, int *out_h);

gboolean ns_image_cache_tick(ns_image_cache *cache, gint64 now_us);
gboolean ns_image_cache_tick_collect(ns_image_cache *cache, gint64 now_us,
                                     GPtrArray *changed);

gboolean ns_image_is_animation(const ns_image *img);
GBytes  *ns_image_video_bytes(const ns_image *img);
gboolean ns_image_video_has_audio(const ns_image *img);
double   ns_image_anim_duration(const ns_image *img);
double   ns_image_anim_position(const ns_image *img, gint64 now_us);
void     ns_image_anim_set_paused(ns_image *img, gboolean paused,
                                  gint64 now_us);
void     ns_image_anim_seek(ns_image *img, double seconds, gint64 now_us);
void     ns_image_anim_shift(ns_image *img, gint64 delta_us);
gboolean ns_image_anim_ended(const ns_image *img, gint64 now_us);
void     ns_image_anim_set_loop(ns_image *img, gboolean loop, gint64 now_us);
gboolean ns_image_cache_animating(const ns_image_cache *cache);
gboolean ns_image_cache_has_pending(const ns_image_cache *cache);

void     ns_image_cache_begin_generation(ns_image_cache *cache);
void     ns_image_cache_collect(ns_image_cache *cache);

#ifdef NS_HAVE_AVIF
ns_texture *ns_image_decode_avif(const guchar *data, gsize len,
                                 int *out_w, int *out_h);

gboolean ns_image_avif_supports_bytes(const guchar *data, gsize len);
#endif

gboolean ns_image_supports_mime(const char *mime);

G_END_DECLS

#endif
