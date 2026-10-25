/* Northstar Web Browser — local blocklist lookup and the warning interstitial for blocked hosts.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_SAFEBROWSING_H
#define NS_SAFEBROWSING_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_UNSAFE_CONTINUE_SCHEME "northstar-unsafe-continue:"

gboolean ns_safebrowsing_blocked(const char *host);
void     ns_safebrowsing_allow_host(const char *host);
char    *ns_safebrowsing_interstitial(const char *url, const char *host);

G_END_DECLS

#endif
