/* Northstar Web Browser — shell icon API: bundled SVG icons for images, entries and the window.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NORTHSTAR_GTK_ICONS_H
#define NORTHSTAR_GTK_ICONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ns_icon_image_new(const char *name);

void ns_icon_image_set(GtkImage *image, const char *name);

void ns_icon_entry_set(GtkEntry *entry, GtkEntryIconPosition pos,
                       const char *name);

void ns_icon_install_window_icon(const char *name);

G_END_DECLS

#endif
