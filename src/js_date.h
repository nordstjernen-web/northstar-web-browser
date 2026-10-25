/* Northstar Web Browser — installs the ISO/UTC-only Temporal global into a QuickJS context.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NS_JS_DATE_H
#define NS_JS_DATE_H

#include "quickjs_compat.h"

void ns_js_temporal_install(JSContext *ctx, JSValueConst global);

#endif
