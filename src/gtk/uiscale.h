/* Northstar Web Browser — UI scale for displays GTK does not scale itself.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_GTK_UISCALE_H
#define NS_GTK_UISCALE_H

#include <glib.h>

double ns_ui_scale(void);
int    ns_ui_px(int px);
char  *ns_ui_scale_css(const char *css);

#endif
