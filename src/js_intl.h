/* Northstar Web Browser — installs the native Intl namespace into a QuickJS realm.
 * Also overrides toLocaleString, toLocale*String and localeCompare on builtins.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NS_JS_INTL_H
#define NS_JS_INTL_H

#include "quickjs_compat.h"

void ns_js_intl_install(JSContext *ctx, JSValueConst global);

#endif
