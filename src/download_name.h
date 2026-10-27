/* Northstar Web Browser — download file-name sanitising.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_DOWNLOAD_NAME_H
#define NS_DOWNLOAD_NAME_H

#include <glib.h>

G_BEGIN_DECLS

/* Turns a page-supplied file name (the download attribute, a
 * Content-Disposition filename, the last URL path segment) into one that
 * names a plain regular file in the downloads directory on every
 * platform, or returns NULL when nothing usable is left.  Path
 * separators and the characters Windows refuses (: * ? " < > |) become
 * '_', so a name can never address an NTFS alternate data stream;
 * control and bidi-override characters are dropped; trailing dots and
 * spaces, which Win32 strips silently, are removed; a Windows device
 * stem (CON, NUL, COM1, ...) is prefixed with '_'; hidden names (leading
 * '.') are refused; the result is valid UTF-8 and at most
 * NS_DOWNLOAD_NAME_MAX bytes, keeping a short extension.  Caller frees. */
#define NS_DOWNLOAD_NAME_MAX 128
char *ns_download_file_name(const char *name);

G_END_DECLS

#endif
