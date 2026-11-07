/* Northstar Web Browser — MP4 avcC config and length-prefixed H.264 samples to Annex B.
 * Copyright 2026 Gabriel Ferreira
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_AVC_H
#define NS_AVC_H

#include <glib.h>

G_BEGIN_DECLS

/* Appends avcC SPS/PPS as Annex B NAL units and reads the NAL length size. */
gboolean ns_avc_append_parameter_sets(GByteArray *out, GBytes *config,
                                      int *length_size);
/* Converts a length-prefixed MP4 sample into Annex B start-code NAL units. */
gboolean ns_avc_append_nal_units(GByteArray *out, const guint8 *data, gsize len,
                                 int length_size);

G_END_DECLS

#endif
