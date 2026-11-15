/* Northstar Web Browser — PDF documents shown in a sandboxed PDF.js viewer page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_PDF_H
#define NS_PDF_H

#include <glib.h>

G_BEGIN_DECLS

char *ns_pdf_document_html(const guint8 *data, gsize len, const char *url,
                           char **csp_out);

G_END_DECLS

#endif
