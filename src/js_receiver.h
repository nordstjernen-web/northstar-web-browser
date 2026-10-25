/* Northstar Web Browser — installs WebIDL receiver checks on Node-derived interface members.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NS_JS_RECEIVER_H
#define NS_JS_RECEIVER_H

#include "quickjs_compat.h"

void ns_js_require_node_receivers(JSContext *ctx, JSValueConst global,
                                  JSClassID node_class, JSClassID attr_class);

#endif
