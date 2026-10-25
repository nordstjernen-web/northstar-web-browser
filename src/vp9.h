/* Northstar Web Browser — VP9 video decoder API over libvpx.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_VP9_H
#define NS_VP9_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_vp9_decoder ns_vp9_decoder;

gboolean        ns_vp9_available(void);
ns_vp9_decoder *ns_vp9_decoder_new(void);
void            ns_vp9_decoder_free(ns_vp9_decoder *decoder);
void            ns_vp9_decoder_flush(ns_vp9_decoder *decoder);
ns_texture     *ns_vp9_decoder_decode(ns_vp9_decoder *decoder, GBytes *sample,
                                      gboolean want_texture);

G_END_DECLS

#endif
