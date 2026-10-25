/* Northstar Web Browser — document-loading helpers around the HTML and XML parsers.
 * MIME dispatch, charset detection and decoding, serialization escapes, image and JSON viewers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "html.h"
#include "encoding.h"

#include <math.h>
#include <string.h>
#include <uchardet.h>

#include <lexbor/core/base.h>

static void
ns_html_append_digits(GString *num, const char **p)
{
    while (g_ascii_isdigit(**p)) g_string_append_c(num, *(*p)++);
}

/* HTML "rules for parsing floating-point number values"; FALSE if invalid or non-finite. */
gboolean
ns_html_parse_float(const char *s, double *out)
{
    if (!s) return FALSE;
    while (ns_is_ascii_ws(*s))
        s++;
    const char *p = s;
    GString *num = g_string_new(NULL);
    if (*p == '-') g_string_append_c(num, *p++);
    else if (*p == '+') p++;
    if (!g_ascii_isdigit(*p) && !(*p == '.' && g_ascii_isdigit(p[1]))) {
        g_string_free(num, TRUE);
        return FALSE;
    }
    ns_html_append_digits(num, &p);
    if (*p == '.') {
        if (g_ascii_isdigit(p[1])) {
            g_string_append_c(num, *p++);
            ns_html_append_digits(num, &p);
        } else if (p[1] == 'e' || p[1] == 'E') {
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        gboolean negative = *q == '-';
        if (*q == '-' || *q == '+') q++;
        if (g_ascii_isdigit(*q)) {
            g_string_append(num, negative ? "e-" : "e");
            ns_html_append_digits(num, &q);
        }
    }
    double v = g_ascii_strtod(num->str, NULL);
    g_string_free(num, TRUE);
    if (!isfinite(v)) return FALSE;
    *out = v == 0 ? 0.0 : v;
    return TRUE;
}

/* Returns the lowercased MIME type essence without parameters, default text/html. */
char *
ns_html_mime_essence(const char *content_type)
{
    if (!content_type || !*content_type)
        return g_strdup("text/html");
    const char *start = content_type;
    while (g_ascii_isspace(*start)) start++;
    const char *end = strchr(start, ';');
    if (!end) end = start + strlen(start);
    while (end > start && g_ascii_isspace(end[-1])) end--;
    if (end == start)
        return g_strdup("text/html");
    char *essence = g_strndup(start, (gsize)(end - start));
    for (char *p = essence; *p; p++)
        *p = g_ascii_tolower(*p);
    return essence;
}

/* Returns TRUE for text/xml, application/xml and any +xml MIME type. */
gboolean
ns_html_mime_is_xml(const char *content_type)
{
    g_autofree char *essence = ns_html_mime_essence(content_type);
    return strcmp(essence, "text/xml") == 0 ||
           strcmp(essence, "application/xml") == 0 ||
           g_str_has_suffix(essence, "+xml");
}

/* Parses as XML for XML MIME types (with an error page on failure), else as HTML. */
ns_node *
ns_html_parse_document(const char *input, gssize len,
                       const char *content_type)
{
    if (!ns_html_mime_is_xml(content_type))
        return ns_html_parse(input, len);
    ns_node *doc = ns_xml_parse(input, len);
    if (doc) return doc;
    static const char error_document[] =
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
        "<title>XML parsing error</title></head><body>"
        "<p>This XML document is not well-formed.</p></body></html>";
    return ns_xml_parse(error_document, sizeof error_document - 1);
}

/* Returns TRUE for HTML void elements, which have no end tag. */
gboolean
ns_html_is_void(const char *tag)
{
    if (!tag) return FALSE;
    static const char *const voids[] = {
        "area", "base", "basefont", "bgsound", "br", "col", "embed",
        "frame", "hr", "img", "input", "keygen", "link", "meta",
        "param", "source", "track", "wbr",
        NULL,
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(tag, voids[i]) == 0)
            return TRUE;
    return FALSE;
}

/* Returns TRUE for elements whose content is serialized without escaping. */
gboolean
ns_html_is_raw_text(const char *tag)
{
    if (!tag) return FALSE;
    static const char *const raws[] = {
        "script", "style", "xmp", "iframe", "noembed", "noframes",
        "noscript", "plaintext",
        NULL,
    };
    for (int i = 0; raws[i]; i++)
        if (g_ascii_strcasecmp(tag, raws[i]) == 0)
            return TRUE;
    return FALSE;
}

/* Appends s with &, <, > (optionally ") escaped and U+00A0 as &nbsp;. */
void
ns_html_escape_append(GString *out, const char *s, gboolean escape_quotes)
{
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '&': g_string_append(out, "&amp;"); break;
        case '<': g_string_append(out, "&lt;");  break;
        case '>': g_string_append(out, "&gt;");  break;
        case '"':
            if (escape_quotes) g_string_append(out, "&quot;");
            else               g_string_append_c(out, '"');
            break;
        case '\xc2':
            if ((unsigned char)p[1] == 0xa0) {
                g_string_append(out, "&nbsp;");
                p++;
            } else {
                g_string_append_c(out, *p);
            }
            break;
        default:  g_string_append_c(out, *p);    break;
        }
    }
}

char *
ns_html_escape_text(const char *s)
{
    GString *g = g_string_new(NULL);
    ns_html_escape_append(g, s, TRUE);
    return g_string_free(g, FALSE);
}

/* Builds the synthetic HTML page that displays a top-level image URL; caller frees. */
char *
ns_html_image_document(const char *url)
{
    const char *u = url ? url : "";
    char *esc = ns_html_escape_text(u);
    char *name = g_path_get_basename(u);
    char *query = strchr(name, '?');
    if (query) *query = '\0';
    char *esc_name = ns_html_escape_text(*name ? name : "image");
    char *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><title>%s</title><style>"
        "html,body{margin:0;min-height:100vh}"
        "body{background:#1c1d1e;text-align:center}"
        "img{max-width:100vw;max-height:100vh}"
        "</style></head><body><img src=\"%s\" alt=\"\"></body></html>",
        esc_name, esc);
    g_free(esc);
    g_free(esc_name);
    g_free(name);
    return html;
}

static const char NS_DOC_VIEWER_STYLE[] =
    "<style>"
    "body{margin:0;background:#fbfbfd;color:#1a1a1a;"
    "font:13px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "pre{margin:0;padding:14px;white-space:pre;tab-size:2}"
    ".k{color:#9b2393}.s{color:#1a7f37}.n{color:#0b69c7}.b{color:#b35900}"
    ".p{color:#6e7781}.tag{color:#116329}.at{color:#6f42c1}.av{color:#1a7f37}"
    ".cm{color:#6a737d;font-style:italic}.pi{color:#6e7781}"
    "</style>";

static void
doc_append_escaped(GString *o, const char *s, const char *e)
{
    for (const char *p = s; p < e; p++) {
        switch (*p) {
        case '<': g_string_append(o, "&lt;"); break;
        case '>': g_string_append(o, "&gt;"); break;
        case '&': g_string_append(o, "&amp;"); break;
        default:  g_string_append_c(o, *p);
        }
    }
}

static void
doc_indent(GString *o, int depth)
{
    for (int i = 0; i < depth && i < 64; i++) g_string_append(o, "  ");
}

typedef struct {
    const char *p;
    const char *end;
    GString    *o;
    gboolean    ok;
} json_ctx;

static void
json_ws(json_ctx *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

/* Emits a JSON string token as a highlighted span; clears ok if unterminated. */
static void
json_string(json_ctx *c, const char *cls)
{
    const char *s = c->p;
    c->p++;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->end) c->p++;
        c->p++;
    }
    if (c->p >= c->end) { c->ok = FALSE; return; }
    c->p++;
    g_string_append_printf(c->o, "<span class=%s>", cls);
    doc_append_escaped(c->o, s, c->p);
    g_string_append(c->o, "</span>");
}

static gboolean
json_literal(json_ctx *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) >= n && strncmp(c->p, lit, n) == 0) {
        g_string_append_printf(c->o, "<span class=b>%s</span>", lit);
        c->p += n;
        return TRUE;
    }
    return FALSE;
}

static void json_value(json_ctx *c, int depth);

/* Recursively pretty-prints and syntax-highlights one JSON value as HTML. */
static void
json_value(json_ctx *c, int depth)
{
    if (depth > 256) { c->ok = FALSE; return; }
    json_ws(c);
    if (c->p >= c->end) { c->ok = FALSE; return; }
    char ch = *c->p;
    if (ch == '{' || ch == '[') {
        char close = ch == '{' ? '}' : ']';
        gboolean obj = ch == '{';
        g_string_append_printf(c->o, "<span class=p>%c</span>", ch);
        c->p++;
        json_ws(c);
        if (c->p < c->end && *c->p == close) {
            c->p++;
            g_string_append_printf(c->o, "<span class=p>%c</span>", close);
            return;
        }
        for (;;) {
            g_string_append_c(c->o, '\n');
            doc_indent(c->o, depth + 1);
            json_ws(c);
            if (obj) {
                if (c->p >= c->end || *c->p != '"') { c->ok = FALSE; return; }
                json_string(c, "k");
                json_ws(c);
                if (c->p >= c->end || *c->p != ':') { c->ok = FALSE; return; }
                c->p++;
                g_string_append(c->o, "<span class=p>: </span>");
            }
            json_value(c, depth + 1);
            if (!c->ok) return;
            json_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                g_string_append(c->o, "<span class=p>,</span>");
                continue;
            }
            break;
        }
        g_string_append_c(c->o, '\n');
        doc_indent(c->o, depth);
        if (c->p >= c->end || *c->p != close) { c->ok = FALSE; return; }
        c->p++;
        g_string_append_printf(c->o, "<span class=p>%c</span>", close);
        return;
    }
    if (ch == '"') { json_string(c, "s"); return; }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        const char *s = c->p;
        if (*c->p == '-') c->p++;
        while (c->p < c->end &&
               ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
                *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-'))
            c->p++;
        if (c->p == s) { c->ok = FALSE; return; }
        g_string_append(c->o, "<span class=n>");
        doc_append_escaped(c->o, s, c->p);
        g_string_append(c->o, "</span>");
        return;
    }
    if (json_literal(c, "true") || json_literal(c, "false") ||
        json_literal(c, "null"))
        return;
    c->ok = FALSE;
}

/* Renders JSON as a highlighted, indented HTML viewer page; NULL if it is not valid JSON. */
char *
ns_html_json_document(const char *url, const char *json, gsize len)
{
    if (!json) return NULL;
    GString *o = g_string_new(NULL);
    json_ctx c = { json, json + len, o, TRUE };
    json_ws(&c);
    json_value(&c, 0);
    if (!c.ok) { g_string_free(o, TRUE); return NULL; }
    char *esc_url = ns_html_escape_text(url && *url ? url : "");
    char *html = g_strconcat(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>",
        esc_url, "</title>", NS_DOC_VIEWER_STYLE,
        "</head><body><pre>", o->str, "</pre></body></html>", NULL);
    g_free(esc_url);
    g_string_free(o, TRUE);
    return html;
}

/* Finds a "charset=" value in a header or markup prefix; caller frees. */
static char *
charset_value_in(const char *s, gsize len)
{
    for (gsize i = 0; i + 7 <= len; i++) {
        if (g_ascii_strncasecmp(s + i, "charset", 7) != 0) continue;
        gsize p = i + 7;
        while (p < len && g_ascii_isspace(s[p])) p++;
        if (p >= len || s[p] != '=') continue;
        p++;
        while (p < len && g_ascii_isspace(s[p])) p++;
        if (p < len && (s[p] == '"' || s[p] == '\'')) p++;
        gsize start = p;
        while (p < len && (g_ascii_isalnum(s[p]) || s[p] == '-' ||
                           s[p] == '_' || s[p] == ':' || s[p] == '.'))
            p++;
        if (p > start && p - start < 40)
            return g_strndup(s + start, p - start);
    }
    return NULL;
}

static void
charset_report(char **charset_out, const char *name)
{
    if (charset_out && !*charset_out) *charset_out = g_strdup(name);
}

/* Resolves the charset from Content-Type or the first 1024 bytes; UTF-16 maps to UTF-8. */
char *
ns_html_declared_charset(const char *body, gsize len, const char *content_type)
{
    char *label = content_type
        ? charset_value_in(content_type, strlen(content_type)) : NULL;
    if (!label && body)
        label = charset_value_in(body, len < 1024 ? len : 1024);
    if (!label) return NULL;
    const ns_encoding *enc = ns_encoding_for_label(label);
    g_free(label);
    if (!enc) return NULL;
    const char *name = ns_encoding_name(enc);
    if (ns_encoding_is_utf16(enc))
        name = "UTF-8";
    else if (strcmp(name, "x-user-defined") == 0)
        name = "windows-1252";
    return g_strdup(name);
}

static char *
decode_with(const char *encoding, const char *body, gsize len,
            char **charset_out, const char *report)
{
    char *out = ns_encoding_decode(ns_encoding_for_name(encoding), body, len,
                                   NULL);
    charset_report(charset_out, report);
    return out;
}

/* Decodes a document body via BOM, declared charset, UTF-8 check or uchardet sniffing. */
char *
ns_html_decode_body_full(const char *body, gsize len,
                         const char *content_type, char **charset_out)
{
    if (charset_out) *charset_out = NULL;
    if (!body || len == 0) return g_strdup("");

    if (len >= 3 && memcmp(body, "\xef\xbb\xbf", 3) == 0)
        return decode_with("UTF-8", body + 3, len - 3, charset_out, "UTF-8");
    if (len >= 2 && memcmp(body, "\xff\xfe", 2) == 0)
        return decode_with("UTF-16LE", body + 2, len - 2, charset_out,
                           "UTF-16LE");
    if (len >= 2 && memcmp(body, "\xfe\xff", 2) == 0)
        return decode_with("UTF-16BE", body + 2, len - 2, charset_out,
                           "UTF-16BE");

    char *declared = ns_html_declared_charset(body, len, content_type);
    const ns_encoding *enc = declared ? ns_encoding_for_name(declared) : NULL;
    if (enc && !ns_encoding_is_utf8(enc)) {
        char *out = ns_encoding_decode(enc, body, len, NULL);
        charset_report(charset_out, declared);
        g_free(declared);
        return out;
    }
    g_free(declared);

    if (enc || g_utf8_validate(body, (gssize)len, NULL))
        return decode_with("UTF-8", body, len, charset_out, "UTF-8");

    const ns_encoding *detected = NULL;
    uchardet_t det = uchardet_new();
    if (det) {
        gsize scan = len < (gsize)1024 * 1024 ? len : (gsize)1024 * 1024;
        if (uchardet_handle_data(det, body, scan) == 0) {
            uchardet_data_end(det);
            detected = ns_encoding_for_label(uchardet_get_charset(det));
        }
        uchardet_delete(det);
    }
    if (!detected || ns_encoding_is_utf16(detected) ||
        ns_encoding_is_replacement(detected))
        detected = ns_encoding_for_name("windows-1252");
    return decode_with(ns_encoding_name(detected), body, len, charset_out,
                       ns_encoding_name(detected));
}
