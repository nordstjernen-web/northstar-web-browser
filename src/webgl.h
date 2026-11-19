/* Northstar Web Browser — minimalist WebGL: canvas.getContext mapped onto GLES.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_WEBGL_H
#define NS_WEBGL_H

#include <glib.h>
#include <cairo.h>
#include "quickjs_compat.h"

#include "dom.h"

G_BEGIN_DECLS

typedef struct ns_js ns_js;

JSValue ns_webgl_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                             const ns_node *canvas, int version,
                             JSValueConst attrs);

/* Decides, once at startup and before the sandbox is installed, whether
 * this run offers WebGL: built in, enabled in Settings (webgl_enabled) and
 * not switched off with NS_NO_WEBGL. Returns the decision. */
gboolean ns_webgl_startup(void);
/* The decision ns_webgl_startup() made; FALSE before it ran. */
gboolean ns_webgl_available(void);

void ns_webgl_install(JSContext *ctx, JSValueConst global);

cairo_surface_t *ns_webgl_canvas_surface(const ns_node *canvas);

/* Called when a canvas node is freed, so its context stops referring to it. */
void ns_webgl_node_freed(const ns_node *canvas);

cairo_surface_t *ns_js_drawimage_source_surface(JSContext *ctx,
                                                JSValueConst src,
                                                int *out_w, int *out_h,
                                                gboolean *threw);

G_END_DECLS

#endif
