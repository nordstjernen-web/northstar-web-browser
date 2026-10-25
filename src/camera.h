/* Northstar Web Browser — webcam enumeration and per-origin camera permission API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_CAMERA_H
#define NS_CAMERA_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_camera_info {
    char *device;
    char *label;
} ns_camera_info;

GPtrArray  *ns_camera_enumerate(void);
void        ns_camera_info_free(ns_camera_info *info);

typedef struct ns_js ns_js;

int      ns_camera_permission(ns_js *js);
/* Whether the camera is granted, without asking the user. */
gboolean ns_camera_permission_granted(ns_js *js);
gboolean ns_camera_has_pending_origin(void);
char    *ns_camera_take_pending_origin(void);
void     ns_camera_set_decision(const char *origin, int allow);

G_END_DECLS

#endif
