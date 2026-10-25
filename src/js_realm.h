/* Northstar Web Browser — installs a native ShadowRealm global when QuickJS lacks one.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NS_JS_REALM_H
#define NS_JS_REALM_H

#include "quickjs_compat.h"

void ns_js_realm_install(JSContext *ctx, JSValueConst global);

#endif
