/* Northstar Web Browser — security API: refuse-root, OS sandboxes, CSPRNG, SRI, download origin.
 * Also env flags, the sandbox-writable downloads dir and the tool-mode switch for test hooks.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_SECURITY_H
#define NS_SECURITY_H

#include <glib.h>

G_BEGIN_DECLS

const char *ns_security_downloads_dir(void);

/* A boolean environment switch: set to anything other than "", "0",
 * "false", "no" or "off" (so NS_NO_SANDBOX=0 keeps the sandbox on). */
gboolean ns_env_flag(const char *name);

/* Headless/tooling runs (WPT, --dump, ...) may honour test-only hooks such
 * as NS_CAMERA_ALLOW and NS_EARLY_JS_FILE; the interactive browser never
 * does. */
void     ns_security_set_tool_mode(gboolean on);
gboolean ns_security_tool_mode(void);

gboolean ns_security_refuse_root(void);

void ns_security_sandbox_init(const char *self_exe);

/* TRUE when the sandbox also refuses reads outside the folders it grants
 * (Landlock on Linux), so an unreadable local file may be its doing. */
gboolean    ns_security_sandbox_limits_reads(void);
/* The download directory, when that sandbox lets the browser read it. */
const char *ns_security_sandbox_readable_downloads(void);

void ns_security_add_writable_dir(const char *dir);

void ns_security_allow_gpu_devices(void);

void ns_security_seccomp_init(void);
void ns_security_deny_exec_memory(gboolean whole_process);

void ns_security_win32_mitigations_init(gboolean interactive);

gboolean ns_security_csprng_fill(void *buf, gsize len);

gboolean ns_security_sri_check(const char *integrity_attr,
                               const void *body,
                               gsize       body_len);

void ns_security_mark_download_origin(const char *path, const char *url);

void ns_security_harden_allocator(void);

G_END_DECLS

#endif
