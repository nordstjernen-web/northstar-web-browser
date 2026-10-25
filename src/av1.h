/* Northstar Web Browser — AV1 decoder interface over libdav1d: create, decode, flush, free.
 * Decoding turns one demuxed sample into an ns_texture; ns_av1_available reports build support.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_AV1_H
#define NS_AV1_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_av1_decoder ns_av1_decoder;

gboolean        ns_av1_available(void);
ns_av1_decoder *ns_av1_decoder_new(void);
void            ns_av1_decoder_free(ns_av1_decoder *decoder);
void            ns_av1_decoder_flush(ns_av1_decoder *decoder);
ns_texture     *ns_av1_decoder_decode(ns_av1_decoder *decoder, GBytes *sample,
                                      gint64 timestamp, gboolean want_texture);

G_END_DECLS

#endif
