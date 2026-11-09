/* Northstar Web Browser — W3C WebDriver server over a headless page session.
 * Run options for the --webdriver command line mode.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_WEBDRIVER_H
#define NS_WEBDRIVER_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_WEBDRIVER_DEFAULT_PORT 4444

typedef struct ns_webdriver_opts {
    int      port;
    int      viewport_width;
    int      viewport_height;
    int      settle_ms;
    gboolean log;
} ns_webdriver_opts;

int ns_webdriver_run(const ns_webdriver_opts *opts);

G_END_DECLS

#endif
