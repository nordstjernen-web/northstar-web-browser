/* Northstar Web Browser — unaligned little/big-endian byte readers and writers for the decoders.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_BYTES_H
#define NS_BYTES_H

#include <glib.h>

static inline guint16
ns_get_le16(const guint8 *p)
{
    return (guint16)(p[0] | (p[1] << 8));
}

static inline guint32
ns_get_le32(const guint8 *p)
{
    return (guint32)p[0] | ((guint32)p[1] << 8) |
           ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
}

static inline guint32
ns_get_be32(const guint8 *p)
{
    return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
           ((guint32)p[2] << 8) | (guint32)p[3];
}

static inline void
ns_put_le32(guint8 *p, guint32 v)
{
    p[0] = (guint8)v; p[1] = (guint8)(v >> 8);
    p[2] = (guint8)(v >> 16); p[3] = (guint8)(v >> 24);
}

static inline void
ns_put_be16(guint8 *p, guint16 v)
{
    p[0] = (guint8)(v >> 8); p[1] = (guint8)v;
}

static inline void
ns_put_be32(guint8 *p, guint32 v)
{
    p[0] = (guint8)(v >> 24); p[1] = (guint8)(v >> 16);
    p[2] = (guint8)(v >> 8); p[3] = (guint8)v;
}

#endif
