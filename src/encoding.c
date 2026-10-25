/* Northstar Web Browser — WHATWG Encoding Standard decoders and encoders on lexbor's tables.
 * Label lookup, streaming decode with BOM sniffing, CJK index encoders and URL percent-encoding.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "encoding.h"

#include <string.h>

#include <lexbor/encoding/encoding.h>
#include <lexbor/encoding/multi.h>
#include <lexbor/encoding/range.h>
#include <lexbor/encoding/single.h>

typedef enum {
    NS_KIND_UTF8,
    NS_KIND_SINGLE_BYTE,
    NS_KIND_GB18030,
    NS_KIND_BIG5,
    NS_KIND_EUC_JP,
    NS_KIND_ISO_2022_JP,
    NS_KIND_SHIFT_JIS,
    NS_KIND_EUC_KR,
    NS_KIND_REPLACEMENT,
    NS_KIND_UTF16BE,
    NS_KIND_UTF16LE,
    NS_KIND_X_USER_DEFINED,
} ns_encoding_kind;

struct ns_encoding {
    lxb_encoding_t id;
    ns_encoding_kind kind;
};

static const ns_encoding ns_encodings[] = {
    { LXB_ENCODING_UTF_8, NS_KIND_UTF8 },
    { LXB_ENCODING_IBM866, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_2, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_3, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_4, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_5, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_6, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_7, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_8, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_8_I, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_10, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_13, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_14, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_15, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_ISO_8859_16, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_KOI8_R, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_KOI8_U, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_MACINTOSH, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_874, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1250, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1251, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1252, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1253, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1254, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1255, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1256, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1257, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_WINDOWS_1258, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_X_MAC_CYRILLIC, NS_KIND_SINGLE_BYTE },
    { LXB_ENCODING_GBK, NS_KIND_GB18030 },
    { LXB_ENCODING_GB18030, NS_KIND_GB18030 },
    { LXB_ENCODING_BIG5, NS_KIND_BIG5 },
    { LXB_ENCODING_EUC_JP, NS_KIND_EUC_JP },
    { LXB_ENCODING_ISO_2022_JP, NS_KIND_ISO_2022_JP },
    { LXB_ENCODING_SHIFT_JIS, NS_KIND_SHIFT_JIS },
    { LXB_ENCODING_EUC_KR, NS_KIND_EUC_KR },
    { LXB_ENCODING_REPLACEMENT, NS_KIND_REPLACEMENT },
    { LXB_ENCODING_UTF_16BE, NS_KIND_UTF16BE },
    { LXB_ENCODING_UTF_16LE, NS_KIND_UTF16LE },
    { LXB_ENCODING_X_USER_DEFINED, NS_KIND_X_USER_DEFINED },
};

/* Lexbor 128-entry high-half table for a single-byte encoding; windows-1252 by default. */
static const lxb_encoding_single_index_t *
single_byte_index(lxb_encoding_t id)
{
    switch (id) {
    case LXB_ENCODING_IBM866: return lxb_encoding_single_index_ibm866;
    case LXB_ENCODING_ISO_8859_2: return lxb_encoding_single_index_iso_8859_2;
    case LXB_ENCODING_ISO_8859_3: return lxb_encoding_single_index_iso_8859_3;
    case LXB_ENCODING_ISO_8859_4: return lxb_encoding_single_index_iso_8859_4;
    case LXB_ENCODING_ISO_8859_5: return lxb_encoding_single_index_iso_8859_5;
    case LXB_ENCODING_ISO_8859_6: return lxb_encoding_single_index_iso_8859_6;
    case LXB_ENCODING_ISO_8859_7: return lxb_encoding_single_index_iso_8859_7;
    case LXB_ENCODING_ISO_8859_8: return lxb_encoding_single_index_iso_8859_8;
    case LXB_ENCODING_ISO_8859_8_I: return lxb_encoding_single_index_iso_8859_8;
    case LXB_ENCODING_ISO_8859_10: return lxb_encoding_single_index_iso_8859_10;
    case LXB_ENCODING_ISO_8859_13: return lxb_encoding_single_index_iso_8859_13;
    case LXB_ENCODING_ISO_8859_14: return lxb_encoding_single_index_iso_8859_14;
    case LXB_ENCODING_ISO_8859_15: return lxb_encoding_single_index_iso_8859_15;
    case LXB_ENCODING_ISO_8859_16: return lxb_encoding_single_index_iso_8859_16;
    case LXB_ENCODING_KOI8_R: return lxb_encoding_single_index_koi8_r;
    case LXB_ENCODING_KOI8_U: return lxb_encoding_single_index_koi8_u;
    case LXB_ENCODING_MACINTOSH: return lxb_encoding_single_index_macintosh;
    case LXB_ENCODING_WINDOWS_874: return lxb_encoding_single_index_windows_874;
    case LXB_ENCODING_WINDOWS_1250: return lxb_encoding_single_index_windows_1250;
    case LXB_ENCODING_WINDOWS_1251: return lxb_encoding_single_index_windows_1251;
    case LXB_ENCODING_WINDOWS_1252: return lxb_encoding_single_index_windows_1252;
    case LXB_ENCODING_WINDOWS_1253: return lxb_encoding_single_index_windows_1253;
    case LXB_ENCODING_WINDOWS_1254: return lxb_encoding_single_index_windows_1254;
    case LXB_ENCODING_WINDOWS_1255: return lxb_encoding_single_index_windows_1255;
    case LXB_ENCODING_WINDOWS_1256: return lxb_encoding_single_index_windows_1256;
    case LXB_ENCODING_WINDOWS_1257: return lxb_encoding_single_index_windows_1257;
    case LXB_ENCODING_WINDOWS_1258: return lxb_encoding_single_index_windows_1258;
    case LXB_ENCODING_X_MAC_CYRILLIC: return lxb_encoding_single_index_x_mac_cyrillic;
    default: return lxb_encoding_single_index_windows_1252;
    }
}

static const struct {
    const char *label;
    lxb_encoding_t id;
} ns_encoding_labels[] = {
    { "866", LXB_ENCODING_IBM866 },
    { "ansi_x3.4-1968", LXB_ENCODING_WINDOWS_1252 },
    { "arabic", LXB_ENCODING_ISO_8859_6 },
    { "ascii", LXB_ENCODING_WINDOWS_1252 },
    { "asmo-708", LXB_ENCODING_ISO_8859_6 },
    { "big5", LXB_ENCODING_BIG5 },
    { "big5-hkscs", LXB_ENCODING_BIG5 },
    { "chinese", LXB_ENCODING_GBK },
    { "cn-big5", LXB_ENCODING_BIG5 },
    { "cp1250", LXB_ENCODING_WINDOWS_1250 },
    { "cp1251", LXB_ENCODING_WINDOWS_1251 },
    { "cp1252", LXB_ENCODING_WINDOWS_1252 },
    { "cp1253", LXB_ENCODING_WINDOWS_1253 },
    { "cp1254", LXB_ENCODING_WINDOWS_1254 },
    { "cp1255", LXB_ENCODING_WINDOWS_1255 },
    { "cp1256", LXB_ENCODING_WINDOWS_1256 },
    { "cp1257", LXB_ENCODING_WINDOWS_1257 },
    { "cp1258", LXB_ENCODING_WINDOWS_1258 },
    { "cp819", LXB_ENCODING_WINDOWS_1252 },
    { "cp866", LXB_ENCODING_IBM866 },
    { "csbig5", LXB_ENCODING_BIG5 },
    { "cseuckr", LXB_ENCODING_EUC_KR },
    { "cseucpkdfmtjapanese", LXB_ENCODING_EUC_JP },
    { "csgb2312", LXB_ENCODING_GBK },
    { "csibm866", LXB_ENCODING_IBM866 },
    { "csiso2022jp", LXB_ENCODING_ISO_2022_JP },
    { "csiso2022kr", LXB_ENCODING_REPLACEMENT },
    { "csiso58gb231280", LXB_ENCODING_GBK },
    { "csiso88596e", LXB_ENCODING_ISO_8859_6 },
    { "csiso88596i", LXB_ENCODING_ISO_8859_6 },
    { "csiso88598e", LXB_ENCODING_ISO_8859_8 },
    { "csiso88598i", LXB_ENCODING_ISO_8859_8_I },
    { "csisolatin1", LXB_ENCODING_WINDOWS_1252 },
    { "csisolatin2", LXB_ENCODING_ISO_8859_2 },
    { "csisolatin3", LXB_ENCODING_ISO_8859_3 },
    { "csisolatin4", LXB_ENCODING_ISO_8859_4 },
    { "csisolatin5", LXB_ENCODING_WINDOWS_1254 },
    { "csisolatin6", LXB_ENCODING_ISO_8859_10 },
    { "csisolatin9", LXB_ENCODING_ISO_8859_15 },
    { "csisolatinarabic", LXB_ENCODING_ISO_8859_6 },
    { "csisolatincyrillic", LXB_ENCODING_ISO_8859_5 },
    { "csisolatingreek", LXB_ENCODING_ISO_8859_7 },
    { "csisolatinhebrew", LXB_ENCODING_ISO_8859_8 },
    { "cskoi8r", LXB_ENCODING_KOI8_R },
    { "csksc56011987", LXB_ENCODING_EUC_KR },
    { "csmacintosh", LXB_ENCODING_MACINTOSH },
    { "csshiftjis", LXB_ENCODING_SHIFT_JIS },
    { "csunicode", LXB_ENCODING_UTF_16LE },
    { "cyrillic", LXB_ENCODING_ISO_8859_5 },
    { "dos-874", LXB_ENCODING_WINDOWS_874 },
    { "ecma-114", LXB_ENCODING_ISO_8859_6 },
    { "ecma-118", LXB_ENCODING_ISO_8859_7 },
    { "elot_928", LXB_ENCODING_ISO_8859_7 },
    { "euc-jp", LXB_ENCODING_EUC_JP },
    { "euc-kr", LXB_ENCODING_EUC_KR },
    { "gb18030", LXB_ENCODING_GB18030 },
    { "gb2312", LXB_ENCODING_GBK },
    { "gb_2312", LXB_ENCODING_GBK },
    { "gb_2312-80", LXB_ENCODING_GBK },
    { "gbk", LXB_ENCODING_GBK },
    { "greek", LXB_ENCODING_ISO_8859_7 },
    { "greek8", LXB_ENCODING_ISO_8859_7 },
    { "hebrew", LXB_ENCODING_ISO_8859_8 },
    { "hz-gb-2312", LXB_ENCODING_REPLACEMENT },
    { "ibm819", LXB_ENCODING_WINDOWS_1252 },
    { "ibm866", LXB_ENCODING_IBM866 },
    { "iso-10646-ucs-2", LXB_ENCODING_UTF_16LE },
    { "iso-2022-cn", LXB_ENCODING_REPLACEMENT },
    { "iso-2022-cn-ext", LXB_ENCODING_REPLACEMENT },
    { "iso-2022-jp", LXB_ENCODING_ISO_2022_JP },
    { "iso-2022-kr", LXB_ENCODING_REPLACEMENT },
    { "iso-8859-1", LXB_ENCODING_WINDOWS_1252 },
    { "iso-8859-10", LXB_ENCODING_ISO_8859_10 },
    { "iso-8859-11", LXB_ENCODING_WINDOWS_874 },
    { "iso-8859-13", LXB_ENCODING_ISO_8859_13 },
    { "iso-8859-14", LXB_ENCODING_ISO_8859_14 },
    { "iso-8859-15", LXB_ENCODING_ISO_8859_15 },
    { "iso-8859-16", LXB_ENCODING_ISO_8859_16 },
    { "iso-8859-2", LXB_ENCODING_ISO_8859_2 },
    { "iso-8859-3", LXB_ENCODING_ISO_8859_3 },
    { "iso-8859-4", LXB_ENCODING_ISO_8859_4 },
    { "iso-8859-5", LXB_ENCODING_ISO_8859_5 },
    { "iso-8859-6", LXB_ENCODING_ISO_8859_6 },
    { "iso-8859-6-e", LXB_ENCODING_ISO_8859_6 },
    { "iso-8859-6-i", LXB_ENCODING_ISO_8859_6 },
    { "iso-8859-7", LXB_ENCODING_ISO_8859_7 },
    { "iso-8859-8", LXB_ENCODING_ISO_8859_8 },
    { "iso-8859-8-e", LXB_ENCODING_ISO_8859_8 },
    { "iso-8859-8-i", LXB_ENCODING_ISO_8859_8_I },
    { "iso-8859-9", LXB_ENCODING_WINDOWS_1254 },
    { "iso-ir-100", LXB_ENCODING_WINDOWS_1252 },
    { "iso-ir-101", LXB_ENCODING_ISO_8859_2 },
    { "iso-ir-109", LXB_ENCODING_ISO_8859_3 },
    { "iso-ir-110", LXB_ENCODING_ISO_8859_4 },
    { "iso-ir-126", LXB_ENCODING_ISO_8859_7 },
    { "iso-ir-127", LXB_ENCODING_ISO_8859_6 },
    { "iso-ir-138", LXB_ENCODING_ISO_8859_8 },
    { "iso-ir-144", LXB_ENCODING_ISO_8859_5 },
    { "iso-ir-148", LXB_ENCODING_WINDOWS_1254 },
    { "iso-ir-149", LXB_ENCODING_EUC_KR },
    { "iso-ir-157", LXB_ENCODING_ISO_8859_10 },
    { "iso-ir-58", LXB_ENCODING_GBK },
    { "iso8859-1", LXB_ENCODING_WINDOWS_1252 },
    { "iso8859-10", LXB_ENCODING_ISO_8859_10 },
    { "iso8859-11", LXB_ENCODING_WINDOWS_874 },
    { "iso8859-13", LXB_ENCODING_ISO_8859_13 },
    { "iso8859-14", LXB_ENCODING_ISO_8859_14 },
    { "iso8859-15", LXB_ENCODING_ISO_8859_15 },
    { "iso8859-2", LXB_ENCODING_ISO_8859_2 },
    { "iso8859-3", LXB_ENCODING_ISO_8859_3 },
    { "iso8859-4", LXB_ENCODING_ISO_8859_4 },
    { "iso8859-5", LXB_ENCODING_ISO_8859_5 },
    { "iso8859-6", LXB_ENCODING_ISO_8859_6 },
    { "iso8859-7", LXB_ENCODING_ISO_8859_7 },
    { "iso8859-8", LXB_ENCODING_ISO_8859_8 },
    { "iso8859-9", LXB_ENCODING_WINDOWS_1254 },
    { "iso88591", LXB_ENCODING_WINDOWS_1252 },
    { "iso885910", LXB_ENCODING_ISO_8859_10 },
    { "iso885911", LXB_ENCODING_WINDOWS_874 },
    { "iso885913", LXB_ENCODING_ISO_8859_13 },
    { "iso885914", LXB_ENCODING_ISO_8859_14 },
    { "iso885915", LXB_ENCODING_ISO_8859_15 },
    { "iso88592", LXB_ENCODING_ISO_8859_2 },
    { "iso88593", LXB_ENCODING_ISO_8859_3 },
    { "iso88594", LXB_ENCODING_ISO_8859_4 },
    { "iso88595", LXB_ENCODING_ISO_8859_5 },
    { "iso88596", LXB_ENCODING_ISO_8859_6 },
    { "iso88597", LXB_ENCODING_ISO_8859_7 },
    { "iso88598", LXB_ENCODING_ISO_8859_8 },
    { "iso88599", LXB_ENCODING_WINDOWS_1254 },
    { "iso_8859-1", LXB_ENCODING_WINDOWS_1252 },
    { "iso_8859-15", LXB_ENCODING_ISO_8859_15 },
    { "iso_8859-1:1987", LXB_ENCODING_WINDOWS_1252 },
    { "iso_8859-2", LXB_ENCODING_ISO_8859_2 },
    { "iso_8859-2:1987", LXB_ENCODING_ISO_8859_2 },
    { "iso_8859-3", LXB_ENCODING_ISO_8859_3 },
    { "iso_8859-3:1988", LXB_ENCODING_ISO_8859_3 },
    { "iso_8859-4", LXB_ENCODING_ISO_8859_4 },
    { "iso_8859-4:1988", LXB_ENCODING_ISO_8859_4 },
    { "iso_8859-5", LXB_ENCODING_ISO_8859_5 },
    { "iso_8859-5:1988", LXB_ENCODING_ISO_8859_5 },
    { "iso_8859-6", LXB_ENCODING_ISO_8859_6 },
    { "iso_8859-6:1987", LXB_ENCODING_ISO_8859_6 },
    { "iso_8859-7", LXB_ENCODING_ISO_8859_7 },
    { "iso_8859-7:1987", LXB_ENCODING_ISO_8859_7 },
    { "iso_8859-8", LXB_ENCODING_ISO_8859_8 },
    { "iso_8859-8:1988", LXB_ENCODING_ISO_8859_8 },
    { "iso_8859-9", LXB_ENCODING_WINDOWS_1254 },
    { "iso_8859-9:1989", LXB_ENCODING_WINDOWS_1254 },
    { "koi", LXB_ENCODING_KOI8_R },
    { "koi8", LXB_ENCODING_KOI8_R },
    { "koi8-r", LXB_ENCODING_KOI8_R },
    { "koi8-ru", LXB_ENCODING_KOI8_U },
    { "koi8-u", LXB_ENCODING_KOI8_U },
    { "koi8_r", LXB_ENCODING_KOI8_R },
    { "korean", LXB_ENCODING_EUC_KR },
    { "ks_c_5601-1987", LXB_ENCODING_EUC_KR },
    { "ks_c_5601-1989", LXB_ENCODING_EUC_KR },
    { "ksc5601", LXB_ENCODING_EUC_KR },
    { "ksc_5601", LXB_ENCODING_EUC_KR },
    { "l1", LXB_ENCODING_WINDOWS_1252 },
    { "l2", LXB_ENCODING_ISO_8859_2 },
    { "l3", LXB_ENCODING_ISO_8859_3 },
    { "l4", LXB_ENCODING_ISO_8859_4 },
    { "l5", LXB_ENCODING_WINDOWS_1254 },
    { "l6", LXB_ENCODING_ISO_8859_10 },
    { "l9", LXB_ENCODING_ISO_8859_15 },
    { "latin1", LXB_ENCODING_WINDOWS_1252 },
    { "latin2", LXB_ENCODING_ISO_8859_2 },
    { "latin3", LXB_ENCODING_ISO_8859_3 },
    { "latin4", LXB_ENCODING_ISO_8859_4 },
    { "latin5", LXB_ENCODING_WINDOWS_1254 },
    { "latin6", LXB_ENCODING_ISO_8859_10 },
    { "logical", LXB_ENCODING_ISO_8859_8_I },
    { "mac", LXB_ENCODING_MACINTOSH },
    { "macintosh", LXB_ENCODING_MACINTOSH },
    { "ms932", LXB_ENCODING_SHIFT_JIS },
    { "ms_kanji", LXB_ENCODING_SHIFT_JIS },
    { "replacement", LXB_ENCODING_REPLACEMENT },
    { "shift-jis", LXB_ENCODING_SHIFT_JIS },
    { "shift_jis", LXB_ENCODING_SHIFT_JIS },
    { "sjis", LXB_ENCODING_SHIFT_JIS },
    { "sun_eu_greek", LXB_ENCODING_ISO_8859_7 },
    { "tis-620", LXB_ENCODING_WINDOWS_874 },
    { "ucs-2", LXB_ENCODING_UTF_16LE },
    { "unicode", LXB_ENCODING_UTF_16LE },
    { "unicode-1-1-utf-8", LXB_ENCODING_UTF_8 },
    { "unicode11utf8", LXB_ENCODING_UTF_8 },
    { "unicode20utf8", LXB_ENCODING_UTF_8 },
    { "unicodefeff", LXB_ENCODING_UTF_16LE },
    { "unicodefffe", LXB_ENCODING_UTF_16BE },
    { "us-ascii", LXB_ENCODING_WINDOWS_1252 },
    { "utf-16", LXB_ENCODING_UTF_16LE },
    { "utf-16be", LXB_ENCODING_UTF_16BE },
    { "utf-16le", LXB_ENCODING_UTF_16LE },
    { "utf-8", LXB_ENCODING_UTF_8 },
    { "utf8", LXB_ENCODING_UTF_8 },
    { "visual", LXB_ENCODING_ISO_8859_8 },
    { "windows-1250", LXB_ENCODING_WINDOWS_1250 },
    { "windows-1251", LXB_ENCODING_WINDOWS_1251 },
    { "windows-1252", LXB_ENCODING_WINDOWS_1252 },
    { "windows-1253", LXB_ENCODING_WINDOWS_1253 },
    { "windows-1254", LXB_ENCODING_WINDOWS_1254 },
    { "windows-1255", LXB_ENCODING_WINDOWS_1255 },
    { "windows-1256", LXB_ENCODING_WINDOWS_1256 },
    { "windows-1257", LXB_ENCODING_WINDOWS_1257 },
    { "windows-1258", LXB_ENCODING_WINDOWS_1258 },
    { "windows-31j", LXB_ENCODING_SHIFT_JIS },
    { "windows-874", LXB_ENCODING_WINDOWS_874 },
    { "windows-949", LXB_ENCODING_EUC_KR },
    { "x-cp1250", LXB_ENCODING_WINDOWS_1250 },
    { "x-cp1251", LXB_ENCODING_WINDOWS_1251 },
    { "x-cp1252", LXB_ENCODING_WINDOWS_1252 },
    { "x-cp1253", LXB_ENCODING_WINDOWS_1253 },
    { "x-cp1254", LXB_ENCODING_WINDOWS_1254 },
    { "x-cp1255", LXB_ENCODING_WINDOWS_1255 },
    { "x-cp1256", LXB_ENCODING_WINDOWS_1256 },
    { "x-cp1257", LXB_ENCODING_WINDOWS_1257 },
    { "x-cp1258", LXB_ENCODING_WINDOWS_1258 },
    { "x-euc-jp", LXB_ENCODING_EUC_JP },
    { "x-gbk", LXB_ENCODING_GBK },
    { "x-mac-cyrillic", LXB_ENCODING_X_MAC_CYRILLIC },
    { "x-mac-roman", LXB_ENCODING_MACINTOSH },
    { "x-mac-ukrainian", LXB_ENCODING_X_MAC_CYRILLIC },
    { "x-sjis", LXB_ENCODING_SHIFT_JIS },
    { "x-unicode20utf8", LXB_ENCODING_UTF_8 },
    { "x-user-defined", LXB_ENCODING_X_USER_DEFINED },
    { "x-x-big5", LXB_ENCODING_BIG5 },
};

typedef enum {
    NS_INDEX_JIS0208,
    NS_INDEX_JIS0212,
    NS_INDEX_EUC_KR,
    NS_INDEX_GB18030,
    NS_INDEX_BIG5,
} ns_index_id;

static const gsize ns_index_sizes[] = {
    [NS_INDEX_JIS0208] = G_N_ELEMENTS(lxb_encoding_multi_jis0208_map),
    [NS_INDEX_JIS0212] = G_N_ELEMENTS(lxb_encoding_multi_jis0212_map),
    [NS_INDEX_EUC_KR] = G_N_ELEMENTS(lxb_encoding_multi_euc_kr_map),
    [NS_INDEX_GB18030] = G_N_ELEMENTS(lxb_encoding_multi_gb18030_map),
    [NS_INDEX_BIG5] = G_N_ELEMENTS(lxb_encoding_multi_big5_map),
};

/* Lexbor code point table backing a WHATWG multi-byte index. */
static const lxb_codepoint_t *
index_map(ns_index_id id)
{
    switch (id) {
    case NS_INDEX_JIS0208: return lxb_encoding_multi_jis0208_map;
    case NS_INDEX_JIS0212: return lxb_encoding_multi_jis0212_map;
    case NS_INDEX_EUC_KR: return lxb_encoding_multi_euc_kr_map;
    case NS_INDEX_GB18030: return lxb_encoding_multi_gb18030_map;
    case NS_INDEX_BIG5: return lxb_encoding_multi_big5_map;
    }
    return lxb_encoding_multi_jis0208_map;
}

#define NS_EOS (-1)

enum {
    NS_STEP_CONTINUE = 0,
    NS_STEP_ERROR = -1,
    NS_STEP_FINISHED = -2,
};

enum {
    NS_ISO_ASCII,
    NS_ISO_ROMAN,
    NS_ISO_KATAKANA,
    NS_ISO_LEAD,
    NS_ISO_TRAIL,
    NS_ISO_ESCAPE_START,
    NS_ISO_ESCAPE,
};

struct ns_decoder {
    const ns_encoding *enc;
    gboolean fatal;
    guint8 pending[8];
    guint npending;
    guint32 code_point;
    guint needed;
    guint seen;
    guint8 lower;
    guint8 upper;
    guint8 lead;
    guint8 second;
    guint8 third;
    int lead_byte;
    guint32 lead_surrogate;
    gboolean jis0212;
    gboolean output;
    gboolean replaced;
    guint8 state;
    guint8 output_state;
};

/* WHATWG "index code point" lookup for pointer; 0 when out of range or unmapped. */
static guint32
index_code_point(ns_index_id id, guint pointer)
{
    if (pointer >= ns_index_sizes[id]) return 0;
    lxb_codepoint_t cp = index_map(id)[pointer];
    return cp == LXB_ENCODING_ERROR_CODEPOINT ? 0 : cp;
}

/* WHATWG "index gb18030 ranges code point" via binary search; 0 for invalid pointers. */
static guint32
gb18030_ranges_code_point(guint32 pointer)
{
    const lxb_encoding_range_index_t *ranges = lxb_encoding_range_index_gb18030;
    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) return 0;
    if (pointer == 7457) return 0xE7C7;
    gsize lo = 0, hi = LXB_ENCODING_RANGE_INDEX_GB18030_SIZE;
    while (hi - lo > 1) {
        gsize mid = lo + (hi - lo) / 2;
        if (ranges[mid].index <= pointer) lo = mid;
        else hi = mid;
    }
    return ranges[lo].codepoint + pointer - ranges[lo].index;
}

/* Maps a lexbor encoding id to its static ns_encoding entry. */
static const ns_encoding *
ns_encoding_by_id(lxb_encoding_t id)
{
    for (gsize i = 0; i < G_N_ELEMENTS(ns_encodings); i++)
        if (ns_encodings[i].id == id) return &ns_encodings[i];
    return NULL;
}

/* WHATWG "get an encoding": trims ASCII whitespace and matches labels case-insensitively. */
const ns_encoding *
ns_encoding_for_label(const char *label)
{
    if (!label) return NULL;
    while (*label == ' ' || *label == '\t' || *label == '\n' ||
           *label == '\f' || *label == '\r')
        label++;
    gsize len = strlen(label);
    while (len > 0 && (label[len - 1] == ' ' || label[len - 1] == '\t' ||
                       label[len - 1] == '\n' || label[len - 1] == '\f' ||
                       label[len - 1] == '\r'))
        len--;
    for (gsize i = 0; i < G_N_ELEMENTS(ns_encoding_labels); i++) {
        if (strlen(ns_encoding_labels[i].label) == len &&
            g_ascii_strncasecmp(ns_encoding_labels[i].label, label, len) == 0)
            return ns_encoding_by_id(ns_encoding_labels[i].id);
    }
    return NULL;
}

const char *
ns_encoding_name(const ns_encoding *enc)
{
    return enc ? (const char *)lxb_encoding_data(enc->id)->name : "UTF-8";
}

/* Finds an encoding by canonical name, falling back to label lookup. */
const ns_encoding *
ns_encoding_for_name(const char *name)
{
    if (!name) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(ns_encodings); i++)
        if (g_ascii_strcasecmp(ns_encoding_name(&ns_encodings[i]), name) == 0)
            return &ns_encodings[i];
    return ns_encoding_for_label(name);
}

const ns_encoding *
ns_encoding_utf8(void)
{
    return &ns_encodings[0];
}

gboolean
ns_encoding_is_utf8(const ns_encoding *enc)
{
    return enc && enc->kind == NS_KIND_UTF8;
}

gboolean
ns_encoding_is_utf16(const ns_encoding *enc)
{
    return enc && (enc->kind == NS_KIND_UTF16BE ||
                   enc->kind == NS_KIND_UTF16LE);
}

gboolean
ns_encoding_is_replacement(const ns_encoding *enc)
{
    return enc && enc->kind == NS_KIND_REPLACEMENT;
}

/* Pushes bytes back onto the decoder's input queue ("prepend to stream"); 8-byte cap. */
static void
prepend(ns_decoder *d, const guint8 *bytes, guint n)
{
    if (d->npending + n > sizeof d->pending) return;
    memmove(d->pending + n, d->pending, d->npending);
    memcpy(d->pending, bytes, n);
    d->npending += n;
}

static void
prepend_byte(ns_decoder *d, int byte)
{
    if (byte == NS_EOS) return;
    guint8 b = (guint8)byte;
    prepend(d, &b, 1);
}

/* WHATWG UTF-8 decoder handler for one byte (or EOS); returns code points or a step code. */
static int
utf8_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) {
        if (d->needed == 0) return NS_STEP_FINISHED;
        ns_decoder_reset(d);
        return NS_STEP_ERROR;
    }
    if (d->needed == 0) {
        if (byte <= 0x7F) {
            out[0] = (guint32)byte;
            return 1;
        }
        if (byte >= 0xC2 && byte <= 0xDF) {
            d->needed = 1;
            d->code_point = byte & 0x1F;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            if (byte == 0xE0) d->lower = 0xA0;
            if (byte == 0xED) d->upper = 0x9F;
            d->needed = 2;
            d->code_point = byte & 0xF;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            if (byte == 0xF0) d->lower = 0x90;
            if (byte == 0xF4) d->upper = 0x8F;
            d->needed = 3;
            d->code_point = byte & 0x7;
        } else {
            return NS_STEP_ERROR;
        }
        return NS_STEP_CONTINUE;
    }
    if (byte < d->lower || byte > d->upper) {
        d->code_point = 0;
        d->needed = d->seen = 0;
        d->lower = 0x80;
        d->upper = 0xBF;
        prepend_byte(d, byte);
        return NS_STEP_ERROR;
    }
    d->lower = 0x80;
    d->upper = 0xBF;
    d->code_point = (d->code_point << 6) | (guint32)(byte & 0x3F);
    if (++d->seen != d->needed) return NS_STEP_CONTINUE;
    out[0] = d->code_point;
    d->code_point = 0;
    d->needed = d->seen = 0;
    return 1;
}

/* WHATWG shared UTF-16 decoder handler, combining surrogate pairs; be selects byte order. */
static int
utf16_step(ns_decoder *d, int byte, gboolean be, guint32 *out)
{
    if (byte == NS_EOS) {
        if (d->lead_byte < 0 && !d->lead_surrogate) return NS_STEP_FINISHED;
        d->lead_byte = -1;
        d->lead_surrogate = 0;
        return NS_STEP_ERROR;
    }
    if (d->lead_byte < 0) {
        d->lead_byte = byte;
        return NS_STEP_CONTINUE;
    }
    guint32 unit = be ? ((guint32)d->lead_byte << 8) + (guint32)byte
                      : ((guint32)byte << 8) + (guint32)d->lead_byte;
    d->lead_byte = -1;
    if (d->lead_surrogate) {
        guint32 lead = d->lead_surrogate;
        d->lead_surrogate = 0;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            out[0] = 0x10000 + ((lead - 0xD800) << 10) + (unit - 0xDC00);
            return 1;
        }
        guint8 bytes[2] = { (guint8)(be ? unit >> 8 : unit & 0xFF),
                            (guint8)(be ? unit & 0xFF : unit >> 8) };
        prepend(d, bytes, 2);
        return NS_STEP_ERROR;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        d->lead_surrogate = unit;
        return NS_STEP_CONTINUE;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) return NS_STEP_ERROR;
    out[0] = unit;
    return 1;
}

/* WHATWG single-byte decoder: ASCII passthrough, high bytes via the encoding's index. */
static int
single_byte_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) return NS_STEP_FINISHED;
    if (byte < 0x80) {
        out[0] = (guint32)byte;
        return 1;
    }
    lxb_codepoint_t cp = single_byte_index(d->enc->id)[byte - 0x80].codepoint;
    if (cp == LXB_ENCODING_ERROR_CODEPOINT) return NS_STEP_ERROR;
    out[0] = cp;
    return 1;
}

/* WHATWG gb18030 decoder handler covering two- and four-byte sequences. */
static int
gb18030_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) {
        if (!d->lead && !d->second && !d->third) return NS_STEP_FINISHED;
        d->lead = d->second = d->third = 0;
        return NS_STEP_ERROR;
    }
    if (d->third) {
        if (byte < 0x30 || byte > 0x39) {
            guint8 bytes[3] = { d->second, d->third, (guint8)byte };
            prepend(d, bytes, 3);
            d->lead = d->second = d->third = 0;
            return NS_STEP_ERROR;
        }
        guint32 pointer = (((guint32)(d->lead - 0x81) * 10 + d->second - 0x30)
                           * 126 + d->third - 0x81) * 10 + (guint32)byte - 0x30;
        d->lead = d->second = d->third = 0;
        guint32 cp = gb18030_ranges_code_point(pointer);
        if (!cp) return NS_STEP_ERROR;
        out[0] = cp;
        return 1;
    }
    if (d->second) {
        if (byte >= 0x81 && byte <= 0xFE) {
            d->third = (guint8)byte;
            return NS_STEP_CONTINUE;
        }
        guint8 bytes[2] = { d->second, (guint8)byte };
        prepend(d, bytes, 2);
        d->lead = d->second = 0;
        return NS_STEP_ERROR;
    }
    if (d->lead) {
        if (byte >= 0x30 && byte <= 0x39) {
            d->second = (guint8)byte;
            return NS_STEP_CONTINUE;
        }
        guint lead = d->lead;
        d->lead = 0;
        guint offset = byte < 0x7F ? 0x40 : 0x41;
        guint32 cp = 0;
        if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFE))
            cp = index_code_point(NS_INDEX_GB18030,
                                  (lead - 0x81) * 190 + (guint)byte - offset);
        if (cp) {
            out[0] = cp;
            return 1;
        }
        if (byte < 0x80) prepend_byte(d, byte);
        return NS_STEP_ERROR;
    }
    if (byte < 0x80) {
        out[0] = (guint32)byte;
        return 1;
    }
    if (byte == 0x80) {
        out[0] = 0x20AC;
        return 1;
    }
    if (byte <= 0xFE) {
        d->lead = (guint8)byte;
        return NS_STEP_CONTINUE;
    }
    return NS_STEP_ERROR;
}

/* WHATWG Big5 decoder handler, emitting two code points for the four special pointers. */
static int
big5_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) {
        if (!d->lead) return NS_STEP_FINISHED;
        d->lead = 0;
        return NS_STEP_ERROR;
    }
    if (d->lead) {
        guint lead = d->lead;
        d->lead = 0;
        if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0xA1 && byte <= 0xFE)) {
            guint offset = byte < 0x7F ? 0x40 : 0x62;
            guint pointer = (lead - 0x81) * 157 + (guint)byte - offset;
            static const guint32 pairs[][3] = {
                { 1133, 0x00CA, 0x0304 }, { 1135, 0x00CA, 0x030C },
                { 1164, 0x00EA, 0x0304 }, { 1166, 0x00EA, 0x030C },
            };
            for (gsize i = 0; i < G_N_ELEMENTS(pairs); i++) {
                if (pairs[i][0] == pointer) {
                    out[0] = pairs[i][1];
                    out[1] = pairs[i][2];
                    return 2;
                }
            }
            guint32 cp = index_code_point(NS_INDEX_BIG5, pointer);
            if (cp) {
                out[0] = cp;
                return 1;
            }
        }
        if (byte < 0x80) prepend_byte(d, byte);
        return NS_STEP_ERROR;
    }
    if (byte < 0x80) {
        out[0] = (guint32)byte;
        return 1;
    }
    if (byte >= 0x81 && byte <= 0xFE) {
        d->lead = (guint8)byte;
        return NS_STEP_CONTINUE;
    }
    return NS_STEP_ERROR;
}

/* WHATWG EUC-JP decoder handler with half-width katakana and JIS X 0212 support. */
static int
euc_jp_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) {
        if (!d->lead) return NS_STEP_FINISHED;
        d->lead = 0;
        d->jis0212 = FALSE;
        return NS_STEP_ERROR;
    }
    if (d->lead == 0x8E && byte >= 0xA1 && byte <= 0xDF) {
        d->lead = 0;
        out[0] = 0xFF61 - 0xA1 + (guint32)byte;
        return 1;
    }
    if (d->lead == 0x8F && byte >= 0xA1 && byte <= 0xFE) {
        d->jis0212 = TRUE;
        d->lead = (guint8)byte;
        return NS_STEP_CONTINUE;
    }
    if (d->lead) {
        guint lead = d->lead;
        d->lead = 0;
        guint32 cp = 0;
        if (lead >= 0xA1 && lead <= 0xFE && byte >= 0xA1 && byte <= 0xFE)
            cp = index_code_point(d->jis0212 ? NS_INDEX_JIS0212
                                             : NS_INDEX_JIS0208,
                                  (lead - 0xA1) * 94 + (guint)byte - 0xA1);
        d->jis0212 = FALSE;
        if (cp) {
            out[0] = cp;
            return 1;
        }
        if (byte < 0x80) prepend_byte(d, byte);
        return NS_STEP_ERROR;
    }
    if (byte < 0x80) {
        out[0] = (guint32)byte;
        return 1;
    }
    if (byte == 0x8E || byte == 0x8F || (byte >= 0xA1 && byte <= 0xFE)) {
        d->lead = (guint8)byte;
        return NS_STEP_CONTINUE;
    }
    return NS_STEP_ERROR;
}

/* ISO-2022-JP escape state: switches charset on a valid escape, else reprocesses bytes. */
static int
iso_2022_jp_escape(ns_decoder *d, int byte)
{
    guint8 lead = d->lead;
    int state = -1;
    d->lead = 0;
    if (lead == 0x28 && byte == 0x42) state = NS_ISO_ASCII;
    else if (lead == 0x28 && byte == 0x4A) state = NS_ISO_ROMAN;
    else if (lead == 0x28 && byte == 0x49) state = NS_ISO_KATAKANA;
    else if (lead == 0x24 && (byte == 0x40 || byte == 0x42)) state = NS_ISO_LEAD;
    if (state >= 0) {
        gboolean was_output = d->output;
        d->state = d->output_state = (guint8)state;
        d->output = TRUE;
        return was_output ? NS_STEP_ERROR : NS_STEP_CONTINUE;
    }
    guint8 bytes[2] = { lead, (guint8)byte };
    prepend(d, bytes, byte == NS_EOS ? 1 : 2);
    d->output = FALSE;
    d->state = d->output_state;
    return NS_STEP_ERROR;
}

/* WHATWG ISO-2022-JP decoder state machine for one byte. */
static int
iso_2022_jp_step(ns_decoder *d, int byte, guint32 *out)
{
    if (d->state == NS_ISO_ESCAPE_START) {
        if (byte == 0x24 || byte == 0x28) {
            d->lead = (guint8)byte;
            d->state = NS_ISO_ESCAPE;
            return NS_STEP_CONTINUE;
        }
        prepend_byte(d, byte);
        d->output = FALSE;
        d->state = d->output_state;
        return NS_STEP_ERROR;
    }
    if (d->state == NS_ISO_ESCAPE) return iso_2022_jp_escape(d, byte);
    if (d->state == NS_ISO_TRAIL) {
        if (byte == 0x1B) {
            d->state = NS_ISO_ESCAPE_START;
            return NS_STEP_ERROR;
        }
        d->state = NS_ISO_LEAD;
        if (byte < 0x21 || byte > 0x7E) return NS_STEP_ERROR;
        guint32 cp = index_code_point(NS_INDEX_JIS0208,
                                      (d->lead - 0x21) * 94u + (guint)byte - 0x21);
        if (!cp) return NS_STEP_ERROR;
        out[0] = cp;
        return 1;
    }
    if (byte == 0x1B) {
        d->state = NS_ISO_ESCAPE_START;
        return NS_STEP_CONTINUE;
    }
    if (byte == NS_EOS) return NS_STEP_FINISHED;
    d->output = FALSE;
    switch (d->state) {
    case NS_ISO_ROMAN:
        if (byte == 0x5C) out[0] = 0xA5;
        else if (byte == 0x7E) out[0] = 0x203E;
        else if (byte <= 0x7F && byte != 0x0E && byte != 0x0F) out[0] = (guint32)byte;
        else return NS_STEP_ERROR;
        return 1;
    case NS_ISO_KATAKANA:
        if (byte < 0x21 || byte > 0x5F) return NS_STEP_ERROR;
        out[0] = 0xFF61 - 0x21 + (guint32)byte;
        return 1;
    case NS_ISO_LEAD:
        if (byte < 0x21 || byte > 0x7E) return NS_STEP_ERROR;
        d->lead = (guint8)byte;
        d->state = NS_ISO_TRAIL;
        return NS_STEP_CONTINUE;
    default:
        if (byte > 0x7F || byte == 0x0E || byte == 0x0F) return NS_STEP_ERROR;
        out[0] = (guint32)byte;
        return 1;
    }
}

/* WHATWG Shift_JIS decoder handler, including the EUDC private-use range. */
static int
shift_jis_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) {
        if (!d->lead) return NS_STEP_FINISHED;
        d->lead = 0;
        return NS_STEP_ERROR;
    }
    if (d->lead) {
        guint lead = d->lead;
        d->lead = 0;
        if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFC)) {
            guint offset = byte < 0x7F ? 0x40 : 0x41;
            guint lead_offset = lead < 0xA0 ? 0x81 : 0xC1;
            guint pointer = (lead - lead_offset) * 188 + (guint)byte - offset;
            if (pointer >= 8836 && pointer <= 10715) {
                out[0] = 0xE000 - 8836 + pointer;
                return 1;
            }
            guint32 cp = index_code_point(NS_INDEX_JIS0208, pointer);
            if (cp) {
                out[0] = cp;
                return 1;
            }
        }
        if (byte < 0x80) prepend_byte(d, byte);
        return NS_STEP_ERROR;
    }
    if (byte <= 0x80) {
        out[0] = (guint32)byte;
        return 1;
    }
    if (byte >= 0xA1 && byte <= 0xDF) {
        out[0] = 0xFF61 - 0xA1 + (guint32)byte;
        return 1;
    }
    if ((byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC)) {
        d->lead = (guint8)byte;
        return NS_STEP_CONTINUE;
    }
    return NS_STEP_ERROR;
}

/* WHATWG EUC-KR decoder handler. */
static int
euc_kr_step(ns_decoder *d, int byte, guint32 *out)
{
    if (byte == NS_EOS) {
        if (!d->lead) return NS_STEP_FINISHED;
        d->lead = 0;
        return NS_STEP_ERROR;
    }
    if (d->lead) {
        guint lead = d->lead;
        d->lead = 0;
        if (byte >= 0x41 && byte <= 0xFE) {
            guint32 cp = index_code_point(NS_INDEX_EUC_KR,
                                          (lead - 0x81) * 190 + (guint)byte - 0x41);
            if (cp) {
                out[0] = cp;
                return 1;
            }
        }
        if (byte < 0x80) prepend_byte(d, byte);
        return NS_STEP_ERROR;
    }
    if (byte < 0x80) {
        out[0] = (guint32)byte;
        return 1;
    }
    if (byte >= 0x81 && byte <= 0xFE) {
        d->lead = (guint8)byte;
        return NS_STEP_CONTINUE;
    }
    return NS_STEP_ERROR;
}

/* Dispatches one byte to the decoder handler for the encoding kind. */
static int
decoder_step(ns_decoder *d, int byte, guint32 *out)
{
    switch (d->enc->kind) {
    case NS_KIND_UTF8:        return utf8_step(d, byte, out);
    case NS_KIND_SINGLE_BYTE: return single_byte_step(d, byte, out);
    case NS_KIND_GB18030:     return gb18030_step(d, byte, out);
    case NS_KIND_BIG5:        return big5_step(d, byte, out);
    case NS_KIND_EUC_JP:      return euc_jp_step(d, byte, out);
    case NS_KIND_ISO_2022_JP: return iso_2022_jp_step(d, byte, out);
    case NS_KIND_SHIFT_JIS:   return shift_jis_step(d, byte, out);
    case NS_KIND_EUC_KR:      return euc_kr_step(d, byte, out);
    case NS_KIND_UTF16BE:     return utf16_step(d, byte, TRUE, out);
    case NS_KIND_UTF16LE:     return utf16_step(d, byte, FALSE, out);
    case NS_KIND_REPLACEMENT:
        if (byte == NS_EOS || d->replaced) return NS_STEP_FINISHED;
        d->replaced = TRUE;
        return NS_STEP_ERROR;
    case NS_KIND_X_USER_DEFINED:
        if (byte == NS_EOS) return NS_STEP_FINISHED;
        out[0] = byte < 0x80 ? (guint32)byte : 0xF780 + (guint32)byte - 0x80;
        return 1;
    }
    return NS_STEP_ERROR;
}

ns_decoder *
ns_decoder_new(const ns_encoding *enc, gboolean fatal)
{
    ns_decoder *d = g_new0(ns_decoder, 1);
    d->enc = enc ? enc : ns_encoding_utf8();
    d->fatal = fatal;
    ns_decoder_reset(d);
    return d;
}

/* Resets decoder state to initial while keeping its encoding and fatal flag. */
void
ns_decoder_reset(ns_decoder *d)
{
    const ns_encoding *enc = d->enc;
    gboolean fatal = d->fatal;
    memset(d, 0, sizeof *d);
    d->enc = enc;
    d->fatal = fatal;
    d->lower = 0x80;
    d->upper = 0xBF;
    d->lead_byte = -1;
    d->state = d->output_state = NS_ISO_ASCII;
}

void
ns_decoder_free(ns_decoder *d)
{
    g_free(d);
}

/* Length of the leading valid UTF-8 run, used as a decode fast path. */
static gsize
utf8_valid_run(const guint8 *data, gsize len)
{
    const gchar *end = NULL;
    g_utf8_validate_len((const gchar *)data, len, &end);
    return (gsize)((const guint8 *)end - data);
}

/* Streams bytes through the decoder into UTF-8; FALSE on error in fatal mode. */
gboolean
ns_decoder_decode(ns_decoder *d, const guint8 *data, gsize len,
                  gboolean flush, GString *out)
{
    gsize i = 0;
    for (;;) {
        if (d->enc->kind == NS_KIND_UTF8 && d->needed == 0 &&
            d->npending == 0 && i < len) {
            gsize run = utf8_valid_run(data + i, len - i);
            g_string_append_len(out, (const gchar *)data + i, (gssize)run);
            i += run;
        }
        int byte;
        if (d->npending) {
            byte = d->pending[0];
            d->npending--;
            memmove(d->pending, d->pending + 1, d->npending);
        } else if (i < len) {
            byte = data[i++];
        } else if (flush) {
            byte = NS_EOS;
        } else {
            return TRUE;
        }
        guint32 cps[2];
        int r = decoder_step(d, byte, cps);
        if (r > 0) {
            for (int k = 0; k < r; k++) g_string_append_unichar(out, cps[k]);
        } else if (r == NS_STEP_ERROR) {
            if (d->fatal) return FALSE;
            g_string_append_unichar(out, 0xFFFD);
        } else if (r == NS_STEP_FINISHED) {
            return TRUE;
        }
    }
}

/* One-shot replacement-mode decode to a new UTF-8 string; caller frees. */
char *
ns_encoding_decode(const ns_encoding *enc, const char *data, gsize len,
                   gsize *out_len)
{
    ns_decoder *d = ns_decoder_new(enc, FALSE);
    GString *out = g_string_sized_new(len + 1);
    ns_decoder_decode(d, (const guint8 *)data, len, TRUE, out);
    ns_decoder_free(d);
    if (out_len) *out_len = out->len;
    return g_string_free(out, FALSE);
}

/* Decodes with BOM sniffing (UTF-8/UTF-16BE/LE) overriding enc; caller frees. */
char *
ns_encoding_decode_sniffed(const ns_encoding *enc, const char *data, gsize len,
                           gsize *out_len)
{
    if (len >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0)
        return ns_encoding_decode(ns_encoding_by_id(LXB_ENCODING_UTF_8),
                                  data + 3, len - 3, out_len);
    if (len >= 2 && memcmp(data, "\xFE\xFF", 2) == 0)
        return ns_encoding_decode(ns_encoding_by_id(LXB_ENCODING_UTF_16BE),
                                  data + 2, len - 2, out_len);
    if (len >= 2 && memcmp(data, "\xFF\xFE", 2) == 0)
        return ns_encoding_decode(ns_encoding_by_id(LXB_ENCODING_UTF_16LE),
                                  data + 2, len - 2, out_len);
    return ns_encoding_decode(enc, data, len, out_len);
}

static gboolean
mime_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Extracts the charset parameter from a MIME type string; caller frees, NULL if absent. */
char *
ns_encoding_mime_charset(const char *mime)
{
    const char *p = mime ? strchr(mime, ';') : NULL;
    while (p) {
        p++;
        while (mime_is_space(*p)) p++;
        const char *name = p;
        while (*p && *p != '=' && *p != ';') p++;
        gsize name_len = (gsize)(p - name);
        if (*p != '=') {
            p = *p ? p : NULL;
            continue;
        }
        p++;
        GString *value = g_string_new(NULL);
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                g_string_append_c(value, *p++);
            }
            p = strchr(p, ';');
        } else {
            const char *start = p;
            while (*p && *p != ';') p++;
            const char *end = p;
            while (end > start && mime_is_space(end[-1])) end--;
            g_string_append_len(value, start, end - start);
            p = *p ? p : NULL;
        }
        if (name_len == 7 && g_ascii_strncasecmp(name, "charset", 7) == 0 &&
            value->len > 0)
            return g_string_free(value, FALSE);
        g_string_free(value, TRUE);
    }
    return NULL;
}

typedef enum {
    NS_REV_JIS0208,
    NS_REV_SHIFT_JIS,
    NS_REV_EUC_KR,
    NS_REV_GB18030,
    NS_REV_BIG5,
    NS_REV_COUNT
} ns_rev_id;

typedef struct {
    guint32 code_point;
    guint32 pointer;
} ns_rev_entry;

typedef struct {
    ns_rev_entry *entries;
    gsize len;
} ns_rev_map;

static ns_rev_map ns_rev_maps[NS_REV_COUNT];
static gsize ns_rev_ready[NS_REV_COUNT];

/* Orders reverse-index entries by code point, then pointer. */
static int
rev_entry_cmp(const void *a, const void *b)
{
    const ns_rev_entry *x = a, *y = b;
    if (x->code_point != y->code_point)
        return x->code_point < y->code_point ? -1 : 1;
    return x->pointer < y->pointer ? -1 : x->pointer > y->pointer;
}

/* Code points whose Big5 encoder uses the last index pointer rather than the first. */
static gboolean
big5_prefers_last(guint32 cp)
{
    return cp == 0x2550 || cp == 0x255E || cp == 0x2561 || cp == 0x256A ||
           cp == 0x5341 || cp == 0x5345;
}

/* Pointers skipped when building encoder maps (Shift_JIS 8272..8835, Big5 below 0xA1 leads). */
static gboolean
rev_pointer_excluded(ns_rev_id id, guint32 pointer)
{
    if (id == NS_REV_SHIFT_JIS) return pointer >= 8272 && pointer <= 8835;
    if (id == NS_REV_BIG5) return pointer < (0xA1 - 0x81) * 157;
    return FALSE;
}

/* Builds a sorted code point to pointer map from an index, keeping the spec-chosen pointer. */
static void
rev_map_build(ns_rev_id id, ns_rev_map *map)
{
    static const ns_index_id sources[NS_REV_COUNT] = {
        [NS_REV_JIS0208] = NS_INDEX_JIS0208,
        [NS_REV_SHIFT_JIS] = NS_INDEX_JIS0208,
        [NS_REV_EUC_KR] = NS_INDEX_EUC_KR,
        [NS_REV_GB18030] = NS_INDEX_GB18030,
        [NS_REV_BIG5] = NS_INDEX_BIG5,
    };
    ns_index_id src = sources[id];
    gsize size = ns_index_sizes[src];
    ns_rev_entry *all = g_new(ns_rev_entry, size);
    gsize n = 0;
    for (guint32 p = 0; p < size; p++) {
        guint32 cp = index_code_point(src, p);
        if (!cp || rev_pointer_excluded(id, p)) continue;
        all[n].code_point = cp;
        all[n].pointer = p;
        n++;
    }
    qsort(all, n, sizeof *all, rev_entry_cmp);
    gsize kept = 0;
    for (gsize i = 0; i < n; i++) {
        gboolean same = kept > 0 &&
                        all[kept - 1].code_point == all[i].code_point;
        if (!same) all[kept++] = all[i];
        else if (id == NS_REV_BIG5 && big5_prefers_last(all[i].code_point))
            all[kept - 1] = all[i];
    }
    map->entries = g_renew(ns_rev_entry, all, kept ? kept : 1);
    map->len = kept;
}

/* WHATWG "index pointer" lookup via a lazily built, thread-safe reverse map. */
static gboolean
rev_pointer(ns_rev_id id, guint32 cp, guint32 *pointer)
{
    if (g_once_init_enter(&ns_rev_ready[id])) {
        rev_map_build(id, &ns_rev_maps[id]);
        g_once_init_leave(&ns_rev_ready[id], 1);
    }
    const ns_rev_map *map = &ns_rev_maps[id];
    gsize lo = 0, hi = map->len;
    while (lo < hi) {
        gsize mid = lo + (hi - lo) / 2;
        if (map->entries[mid].code_point < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= map->len || map->entries[lo].code_point != cp) return FALSE;
    *pointer = map->entries[lo].pointer;
    return TRUE;
}

typedef struct {
    const ns_encoding *enc;
    guint8 iso_state;
} ns_encoder;

enum {
    NS_ENCODE_ERROR = -1,
    NS_ENCODE_ERROR_FFFD = -2,
};

static int
encode_byte(guint32 value, guint8 *out)
{
    out[0] = (guint8)value;
    return 1;
}

static int
encode_two(guint32 lead, guint32 trail, guint8 *out)
{
    out[0] = (guint8)lead;
    out[1] = (guint8)trail;
    return 2;
}

/* Single-byte encoder by linear search of the high-half table. */
static int
encode_single_byte(const ns_encoder *e, guint32 cp, guint8 *out)
{
    const lxb_encoding_single_index_t *index = single_byte_index(e->enc->id);
    for (guint b = 0; b < 128; b++)
        if (index[b].codepoint == cp) return encode_byte(b + 0x80, out);
    return NS_ENCODE_ERROR;
}

/* WHATWG "index gb18030 ranges pointer" for a code point. */
static guint32
gb18030_ranges_pointer(guint32 cp)
{
    if (cp == 0xE7C7) return 7457;
    const lxb_encoding_range_index_t *ranges = lxb_encoding_range_index_gb18030;
    gsize lo = 0, hi = LXB_ENCODING_RANGE_INDEX_GB18030_SIZE;
    while (hi - lo > 1) {
        gsize mid = lo + (hi - lo) / 2;
        if (ranges[mid].codepoint <= cp) lo = mid;
        else hi = mid;
    }
    return ranges[lo].index + cp - ranges[lo].codepoint;
}

/* WHATWG gb18030/GBK encoder handler; GBK forbids four-byte sequences. */
static int
encode_gb18030(const ns_encoder *e, guint32 cp, guint8 *out)
{
    static const struct { guint16 code_point; guint8 lead, trail; } fixed[] = {
        { 0xE78D, 0xA6, 0xD9 }, { 0xE78E, 0xA6, 0xDA }, { 0xE78F, 0xA6, 0xDB },
        { 0xE790, 0xA6, 0xDC }, { 0xE791, 0xA6, 0xDD }, { 0xE792, 0xA6, 0xDE },
        { 0xE793, 0xA6, 0xDF }, { 0xE794, 0xA6, 0xEC }, { 0xE795, 0xA6, 0xED },
        { 0xE796, 0xA6, 0xF3 }, { 0xE81E, 0xFE, 0x59 }, { 0xE826, 0xFE, 0x61 },
        { 0xE82B, 0xFE, 0x66 }, { 0xE82C, 0xFE, 0x67 }, { 0xE832, 0xFE, 0x6D },
        { 0xE843, 0xFE, 0x7E }, { 0xE854, 0xFE, 0x90 }, { 0xE864, 0xFE, 0xA0 },
    };
    gboolean gbk = e->enc->id == LXB_ENCODING_GBK;
    if (cp == 0xE5E5) return NS_ENCODE_ERROR;
    if (gbk && cp == 0x20AC) return encode_byte(0x80, out);
    for (gsize i = 0; i < G_N_ELEMENTS(fixed); i++)
        if (fixed[i].code_point == cp)
            return encode_two(fixed[i].lead, fixed[i].trail, out);
    guint32 pointer;
    if (rev_pointer(NS_REV_GB18030, cp, &pointer)) {
        guint32 trail = pointer % 190;
        return encode_two(pointer / 190 + 0x81,
                          trail + (trail < 0x3F ? 0x40 : 0x41), out);
    }
    if (gbk) return NS_ENCODE_ERROR;
    pointer = gb18030_ranges_pointer(cp);
    out[0] = (guint8)(pointer / (10 * 126 * 10) + 0x81);
    pointer %= 10 * 126 * 10;
    out[1] = (guint8)(pointer / (10 * 126) + 0x30);
    pointer %= 10 * 126;
    out[2] = (guint8)(pointer / 10 + 0x81);
    out[3] = (guint8)(pointer % 10 + 0x30);
    return 4;
}

/* WHATWG Big5 encoder handler. */
static int
encode_big5(guint32 cp, guint8 *out)
{
    guint32 pointer;
    if (!rev_pointer(NS_REV_BIG5, cp, &pointer)) return NS_ENCODE_ERROR;
    guint32 trail = pointer % 157;
    return encode_two(pointer / 157 + 0x81,
                      trail + (trail < 0x3F ? 0x40 : 0x62), out);
}

/* WHATWG EUC-JP encoder handler. */
static int
encode_euc_jp(guint32 cp, guint8 *out)
{
    if (cp == 0xA5) return encode_byte(0x5C, out);
    if (cp == 0x203E) return encode_byte(0x7E, out);
    if (cp >= 0xFF61 && cp <= 0xFF9F)
        return encode_two(0x8E, cp - 0xFF61 + 0xA1, out);
    if (cp == 0x2212) cp = 0xFF0D;
    guint32 pointer;
    if (!rev_pointer(NS_REV_JIS0208, cp, &pointer)) return NS_ENCODE_ERROR;
    return encode_two(pointer / 94 + 0xA1, pointer % 94 + 0xA1, out);
}

/* Emits the ISO-2022-JP escape for a new state and asks to re-encode the code point. */
static int
iso_2022_jp_switch(ns_encoder *e, guint8 state, guint8 *out, gboolean *again)
{
    *again = TRUE;
    e->iso_state = state;
    out[0] = 0x1B;
    out[1] = state == NS_ISO_LEAD ? 0x24 : 0x28;
    out[2] = state == NS_ISO_ROMAN ? 0x4A : 0x42;
    return 3;
}

/* WHATWG ISO-2022-JP encoder handler; may emit an escape and request a retry. */
static int
encode_iso_2022_jp(ns_encoder *e, guint32 cp, guint8 *out, gboolean *again)
{
    guint8 state = e->iso_state;
    gboolean ascii = cp < 0x80;
    if ((state == NS_ISO_ASCII || state == NS_ISO_ROMAN) &&
        (cp == 0x0E || cp == 0x0F || cp == 0x1B))
        return NS_ENCODE_ERROR_FFFD;
    if (state == NS_ISO_ASCII && ascii) return encode_byte(cp, out);
    if (state == NS_ISO_ROMAN &&
        ((ascii && cp != 0x5C && cp != 0x7E) || cp == 0xA5 || cp == 0x203E))
        return encode_byte(cp == 0xA5 ? 0x5C : cp == 0x203E ? 0x7E : cp, out);
    if (ascii) return iso_2022_jp_switch(e, NS_ISO_ASCII, out, again);
    if (cp == 0xA5 || cp == 0x203E)
        return iso_2022_jp_switch(e, NS_ISO_ROMAN, out, again);
    guint32 mapped = cp == 0x2212 ? 0xFF0D : cp;
    if (mapped >= 0xFF61 && mapped <= 0xFF9F)
        mapped = lxb_encoding_multi_iso_2022_jp_katakana_map[mapped - 0xFF61];
    guint32 pointer;
    if (!rev_pointer(NS_REV_JIS0208, mapped, &pointer)) {
        if (state == NS_ISO_LEAD)
            return iso_2022_jp_switch(e, NS_ISO_ASCII, out, again);
        return NS_ENCODE_ERROR;
    }
    if (state != NS_ISO_LEAD)
        return iso_2022_jp_switch(e, NS_ISO_LEAD, out, again);
    return encode_two(pointer / 94 + 0x21, pointer % 94 + 0x21, out);
}

/* WHATWG Shift_JIS encoder handler. */
static int
encode_shift_jis(guint32 cp, guint8 *out)
{
    if (cp == 0x80) return encode_byte(0x80, out);
    if (cp == 0xA5) return encode_byte(0x5C, out);
    if (cp == 0x203E) return encode_byte(0x7E, out);
    if (cp >= 0xFF61 && cp <= 0xFF9F) return encode_byte(cp - 0xFF61 + 0xA1, out);
    if (cp == 0x2212) cp = 0xFF0D;
    guint32 pointer;
    if (!rev_pointer(NS_REV_SHIFT_JIS, cp, &pointer)) return NS_ENCODE_ERROR;
    guint32 lead = pointer / 188, trail = pointer % 188;
    return encode_two(lead + (lead < 0x1F ? 0x81 : 0xC1),
                      trail + (trail < 0x3F ? 0x40 : 0x41), out);
}

/* WHATWG EUC-KR encoder handler. */
static int
encode_euc_kr(guint32 cp, guint8 *out)
{
    guint32 pointer;
    if (!rev_pointer(NS_REV_EUC_KR, cp, &pointer)) return NS_ENCODE_ERROR;
    return encode_two(pointer / 190 + 0x81, pointer % 190 + 0x41, out);
}

/* Encodes one code point in the output encoding; negative on unmappable. */
static int
encode_step(ns_encoder *e, guint32 cp, guint8 *out, gboolean *again)
{
    *again = FALSE;
    ns_encoding_kind kind = e->enc->kind;
    if (kind == NS_KIND_ISO_2022_JP)
        return encode_iso_2022_jp(e, cp, out, again);
    if (cp < 0x80) return encode_byte(cp, out);
    switch (kind) {
    case NS_KIND_SINGLE_BYTE: return encode_single_byte(e, cp, out);
    case NS_KIND_GB18030:     return encode_gb18030(e, cp, out);
    case NS_KIND_BIG5:        return encode_big5(cp, out);
    case NS_KIND_EUC_JP:      return encode_euc_jp(cp, out);
    case NS_KIND_SHIFT_JIS:   return encode_shift_jis(cp, out);
    case NS_KIND_EUC_KR:      return encode_euc_kr(cp, out);
    case NS_KIND_X_USER_DEFINED:
        if (cp < 0xF780 || cp > 0xF7FF) return NS_ENCODE_ERROR;
        return encode_byte(cp - 0xF780 + 0x80, out);
    default:
        return g_unichar_to_utf8(cp, (gchar *)out);
    }
}

/* Emits the final ISO-2022-JP return-to-ASCII escape if needed. */
static int
encode_end(ns_encoder *e, guint8 *out)
{
    gboolean again;
    if (e->enc->kind != NS_KIND_ISO_2022_JP || e->iso_state == NS_ISO_ASCII)
        return 0;
    return iso_2022_jp_switch(e, NS_ISO_ASCII, out, &again);
}

/* WHATWG "get an output encoding": UTF-16 and replacement map to UTF-8. */
const ns_encoding *
ns_encoding_output(const ns_encoding *enc)
{
    if (!enc || ns_encoding_is_utf16(enc) || ns_encoding_is_replacement(enc))
        return ns_encoding_utf8();
    return enc;
}

/* TRUE if a byte must be percent-encoded in the special-query or form-urlencoded set. */
static gboolean
percent_encode_byte(guint8 b, ns_percent_set set)
{
    if (b < 0x20 || b > 0x7E) return TRUE;
    if (set == NS_PERCENT_SPECIAL_QUERY)
        return b == ' ' || b == '"' || b == '#' || b == '<' || b == '>' ||
               b == '\'';
    return !g_ascii_isalnum(b) && b != '*' && b != '-' && b != '.' &&
           b != '_';
}

/* Appends bytes percent-encoded per set, optionally writing space as '+'. */
static void
percent_append(GString *out, const guint8 *bytes, int n, ns_percent_set set,
               gboolean space_as_plus)
{
    for (int i = 0; i < n; i++) {
        guint8 b = bytes[i];
        if (space_as_plus && b == ' ')
            g_string_append_c(out, '+');
        else if (percent_encode_byte(b, set))
            g_string_append_printf(out, "%%%02X", b);
        else
            g_string_append_c(out, (char)b);
    }
}

/* URL "percent-encode after encoding"; unmappables become %26%23NNN%3B. */
void
ns_encoding_percent_encode(const ns_encoding *enc, const char *input,
                           gsize len, ns_percent_set set,
                           gboolean space_as_plus, GString *out)
{
    ns_encoder e = { ns_encoding_output(enc), NS_ISO_ASCII };
    const char *p = input, *end = input + len;
    guint8 bytes[8];
    while (p < end) {
        gunichar cp = g_utf8_get_char_validated(p, end - p);
        if (cp == (gunichar)-1 || cp == (gunichar)-2) {
            cp = 0xFFFD;
            p++;
            for (int k = 0; k < 2 && p < end && ((guint8)*p & 0xC0) == 0x80; k++)
                p++;
        } else {
            p = g_utf8_next_char(p);
        }
        gboolean again;
        do {
            int n = encode_step(&e, cp, bytes, &again);
            if (n < 0) {
                g_string_append_printf(out, "%%26%%23%u%%3B",
                    n == NS_ENCODE_ERROR_FFFD ? 0xFFFDu : (unsigned)cp);
                break;
            }
            percent_append(out, bytes, n, set, space_as_plus);
        } while (again);
    }
    int n = encode_end(&e, bytes);
    percent_append(out, bytes, n, set, space_as_plus);
}
