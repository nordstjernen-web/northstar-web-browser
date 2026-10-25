/* Northstar Web Browser — build-time version macros (NS_VERSION, NS_BUILD_DATE).
 * Defined by meson.build from the project version and build date; fallbacks otherwise.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_VERSION_H
#define NS_VERSION_H

#ifndef NS_VERSION
#define NS_VERSION "unknown"
#endif

#ifndef NS_BUILD_DATE
#define NS_BUILD_DATE ""
#endif

#endif
