/* Northstar Web Browser — download file-name sanitising.
 * A page chooses the name a download is saved under; this makes it name a plain file.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "download_name.h"

#include <string.h>

/* Characters that are path separators somewhere, or that Windows refuses
 * in a file name; ':' also introduces an NTFS alternate data stream. */
static gboolean
name_char_is_unsafe(gunichar c)
{
    return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
           c == '"' || c == '<' || c == '>' || c == '|';
}

/* Invisible characters that reorder or hide the text around them. */
static gboolean
name_char_is_format_control(gunichar c)
{
    return c == 0x200B || c == 0x200C || c == 0x200D ||   /* ZW space/joiners */
           c == 0x200E || c == 0x200F ||                  /* LRM, RLM */
           (c >= 0x202A && c <= 0x202E) ||                /* LRE..RLO, PDF */
           (c >= 0x2066 && c <= 0x2069) ||                /* LRI..PDI */
           c == 0xFEFF ||                                 /* BOM */
           g_unichar_type(c) == G_UNICODE_FORMAT;
}

/* TRUE for a Windows device name stem: CON, PRN, AUX, NUL, COM0-9, LPT0-9. */
static gboolean
stem_is_windows_device(const char *name, gsize stem_len)
{
    static const char *const fixed[] = { "CON", "PRN", "AUX", "NUL", NULL };
    for (gsize i = 0; fixed[i]; i++)
        if (stem_len == 3 && g_ascii_strncasecmp(name, fixed[i], 3) == 0)
            return TRUE;
    if (stem_len == 4 && g_ascii_isdigit(name[3]) &&
        (g_ascii_strncasecmp(name, "COM", 3) == 0 ||
         g_ascii_strncasecmp(name, "LPT", 3) == 0))
        return TRUE;
    return FALSE;
}

char *
ns_download_file_name(const char *name)
{
    if (!name || !*name) return NULL;
    g_autofree char *valid = g_utf8_make_valid(name, -1);
    GString *out = g_string_new(NULL);
    for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c < 0x20 || c == 0x7F || name_char_is_format_control(c))
            continue;
        if (name_char_is_unsafe(c))
            g_string_append_c(out, '_');
        else
            g_string_append_unichar(out, c);
    }

    /* Win32 drops trailing dots and spaces, so "evil.exe." would be saved
     * as evil.exe while the UI shows the dotted name; drop them first. */
    while (out->len > 0 &&
           (out->str[out->len - 1] == '.' || out->str[out->len - 1] == ' '))
        g_string_truncate(out, out->len - 1);
    /* Leading spaces are confusing and leading dots hide the file. */
    gsize lead = 0;
    while (out->str[lead] == ' ') lead++;
    if (lead) g_string_erase(out, 0, (gssize)lead);
    if (out->len == 0 || out->str[0] == '.') {
        g_string_free(out, TRUE);
        return NULL;
    }

    /* Keep a short extension when shortening a long name, cutting the
     * base at a UTF-8 character boundary. */
    if (out->len > NS_DOWNLOAD_NAME_MAX) {
        const char *dot = strrchr(out->str, '.');
        gsize ext_len = dot && out->len - (gsize)(dot - out->str) <= 16
                        ? out->len - (gsize)(dot - out->str) : 0;
        g_autofree char *ext = ext_len ? g_strdup(dot) : NULL;
        gsize keep = NS_DOWNLOAD_NAME_MAX - ext_len;
        gsize cut_len = 0;
        for (const char *p = out->str; *p; p = g_utf8_next_char(p)) {
            gsize next_len = (gsize)(g_utf8_next_char(p) - out->str);
            if (next_len > keep) break;
            cut_len = next_len;
        }
        g_string_truncate(out, cut_len);
        while (out->len > 0 &&
               (out->str[out->len - 1] == '.' || out->str[out->len - 1] == ' '))
            g_string_truncate(out, out->len - 1);
        if (ext) g_string_append(out, ext);
        if (out->len == 0 || out->str[0] == '.') {
            g_string_free(out, TRUE);
            return NULL;
        }
    }

    /* "CON", "nul.txt", "COM1.pdf" open devices on Windows. */
    const char *first_dot = strchr(out->str, '.');
    gsize stem_len = first_dot ? (gsize)(first_dot - out->str) : out->len;
    if (stem_is_windows_device(out->str, stem_len))
        g_string_prepend_c(out, '_');

    return g_string_free(out, FALSE);
}
