/* Northstar Web Browser — installs a SIGQUIT handler that dumps the process's threads to stderr.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_THREADDUMP_H
#define NS_THREADDUMP_H

void ns_thread_dump_install_signal(const char *label);

#endif
