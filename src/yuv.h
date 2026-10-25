/* Northstar Web Browser — YUV picture to BGRA texture conversion for the video decoders.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_YUV_H
#define NS_YUV_H

#include <stddef.h>

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct {
    const void *planes[3];
    ptrdiff_t   strides[3];
    int         width;
    int         height;
    int         bits;
    int         sub_x;
    int         sub_y;
    gboolean    monochrome;
    gboolean    bt601;
    gboolean    full_range;
} ns_yuv_picture;

ns_texture *ns_yuv_to_texture(const ns_yuv_picture *picture);

G_END_DECLS

#endif
