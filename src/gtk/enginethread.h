/* Northstar Web Browser — posts jobs to the thread the page engine runs on.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NORTHSTAR_GTK_ENGINETHREAD_H
#define NORTHSTAR_GTK_ENGINETHREAD_H

#include <glib.h>

typedef void (*NsEngineJob)(gpointer data);

void ns_engine_thread_post(NsEngineJob fn, gpointer data);

#endif
