/* Northstar Web Browser — installs the WebAssembly JS namespace into a QuickJS global.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_WASM_H
#define NS_WASM_H

#include "quickjs_compat.h"

void ns_wasm_install(JSContext *ctx, JSValueConst global);

#endif
