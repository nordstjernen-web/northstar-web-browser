/* Northstar Web Browser — WHATWG Encoding Standard API: labels, streaming decoders, encoders.
 * Also exposes MIME charset extraction and percent-encoding in a document's output encoding.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_ENCODING_H
#define NS_ENCODING_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_encoding ns_encoding;
typedef struct ns_decoder ns_decoder;

typedef enum {
    NS_PERCENT_SPECIAL_QUERY,
    NS_PERCENT_URLENCODED,
} ns_percent_set;

const ns_encoding *ns_encoding_for_label(const char *label);
const ns_encoding *ns_encoding_for_name(const char *name);
const ns_encoding *ns_encoding_utf8(void);
const char *ns_encoding_name(const ns_encoding *enc);
gboolean ns_encoding_is_utf8(const ns_encoding *enc);
gboolean ns_encoding_is_utf16(const ns_encoding *enc);
gboolean ns_encoding_is_replacement(const ns_encoding *enc);

ns_decoder *ns_decoder_new(const ns_encoding *enc, gboolean fatal);
void ns_decoder_reset(ns_decoder *dec);
gboolean ns_decoder_decode(ns_decoder *dec, const guint8 *data, gsize len,
                           gboolean flush, GString *out);
void ns_decoder_free(ns_decoder *dec);

char *ns_encoding_decode(const ns_encoding *enc, const char *data, gsize len,
                         gsize *out_len);
char *ns_encoding_decode_sniffed(const ns_encoding *enc, const char *data,
                                 gsize len, gsize *out_len);
char *ns_encoding_mime_charset(const char *mime);

const ns_encoding *ns_encoding_output(const ns_encoding *enc);
void ns_encoding_percent_encode(const ns_encoding *enc, const char *input,
                                gsize len, ns_percent_set set,
                                gboolean space_as_plus, GString *out);

G_END_DECLS

#endif
