/* Northstar Web Browser — UI string translation API: ns_i18n() over the OS-language catalogue.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_I18N_H
#define NS_I18N_H

#include <glib.h>

G_BEGIN_DECLS

void        ns_i18n_init(const char *self_exe);
const char *ns_i18n(const char *text);
const char *ns_i18n_language(void);

G_END_DECLS

#endif
