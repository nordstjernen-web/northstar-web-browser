/* Northstar Web Browser — page view limits for the GTK shell: viewport, zoom, redirects.
 * Also the page settle time (overridable by NS_SETTLE_MS) and console poll interval.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_PROC_LIMITS_H
#define NS_PROC_LIMITS_H

#define NS_PROC_MAX_WIDTH        2560
#define NS_PROC_MAX_HEIGHT       1600
#define NS_PROC_MAX_JS_REDIRECTS 20
#define NS_PROC_SETTLE_MS        400
#define NS_PROC_CONSOLE_POLL_MS  250

#define NS_PROC_ZOOM_MIN  0.25
#define NS_PROC_ZOOM_MAX  5.0

#define NS_PROC_SETTLE_ENV "NS_SETTLE_MS"

#endif
