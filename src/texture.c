/* Northstar Web Browser — decoded-image texture abstraction.
 * Refcounted BGRA pixel buffer with user data, used for decoded images and video frames.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "texture.h"

#include <string.h>

struct ns_texture {
    gint           ref_count;
    int            width;
    int            height;
    gsize          stride;
    guchar        *bgra;
    void          *user_data;
    GDestroyNotify user_data_destroy;
};

/* Creates a texture holding a copy of height*stride bytes of BGRA pixels; NULL if short. */
ns_texture *
ns_texture_new(int width, int height, ns_texture_format format,
               GBytes *bytes, gsize stride)
{
    (void)format;
    if (width <= 0 || height <= 0 || !bytes) return NULL;

    gsize src_len = 0;
    const guchar *src = g_bytes_get_data(bytes, &src_len);
    if (stride > G_MAXSIZE / (gsize)height) return NULL;
    gsize needed = stride * (gsize)height;
    if (!src || src_len < needed) return NULL;

    ns_texture *t = g_new0(ns_texture, 1);
    t->ref_count = 1;
    t->width = width;
    t->height = height;
    t->stride = stride;
    t->bgra = g_malloc(needed);
    memcpy(t->bgra, src, needed);
    return t;
}

ns_texture *
ns_texture_ref(ns_texture *texture)
{
    if (texture) g_atomic_int_inc(&texture->ref_count);
    return texture;
}

/* Returns the texture's pixel data and stride without copying. */
const guchar *
ns_texture_peek(ns_texture *texture, gsize *stride)
{
    if (stride) *stride = texture ? texture->stride : 0;
    return texture ? texture->bgra : NULL;
}

/* Drops a reference; at zero runs the user-data destroy and frees the pixels. */
void
ns_texture_unref(ns_texture *texture)
{
    if (texture && g_atomic_int_dec_and_test(&texture->ref_count)) {
        if (texture->user_data_destroy && texture->user_data)
            texture->user_data_destroy(texture->user_data);
        g_free(texture->bgra);
        g_free(texture);
    }
}

void
ns_texture_clear(ns_texture **texture)
{
    if (texture && *texture) {
        ns_texture_unref(*texture);
        *texture = NULL;
    }
}

int
ns_texture_get_width(ns_texture *texture)
{
    return texture ? texture->width : 0;
}

int
ns_texture_get_height(ns_texture *texture)
{
    return texture ? texture->height : 0;
}

/* Copies the texture's rows into dst with the given stride, clipped to the narrower row. */
void
ns_texture_download(ns_texture *texture, guchar *dst, gsize dst_stride)
{
    if (!texture || !dst) return;
    gsize row = (gsize)texture->width * 4;
    if (row > dst_stride) row = dst_stride;
    if (row > texture->stride) row = texture->stride;
    for (int y = 0; y < texture->height; y++)
        memcpy(dst + (gsize)y * dst_stride,
               texture->bgra + (gsize)y * texture->stride, row);
}

void *
ns_texture_get_user_data(ns_texture *texture)
{
    return texture ? texture->user_data : NULL;
}

/* Attaches user data, destroying any previous different data; destroys data if no texture. */
void
ns_texture_set_user_data(ns_texture *texture, void *data,
                         GDestroyNotify destroy)
{
    if (!texture) {
        if (destroy && data) destroy(data);
        return;
    }
    if (texture->user_data_destroy && texture->user_data &&
        texture->user_data != data)
        texture->user_data_destroy(texture->user_data);
    texture->user_data = data;
    texture->user_data_destroy = destroy;
}
