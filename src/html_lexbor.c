/* Northstar Web Browser — HTML parser built on lexbor, converting its tree into ns_node.
 * Declarative shadow DOM, streaming document.write fragments, script lines, video metadata.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "html.h"

#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <lexbor/html/interfaces/template_element.h>

static void ns_html_parser_free(ns_html_parser *parser);

struct ns_html_parser {
    lxb_html_document_t *document;
    GString *source;
    gboolean scripting;
    gboolean failed;
    gboolean finished;
};

struct ns_html_fragment_parser {
    lxb_html_parser_t *parser;
    lxb_html_document_t *document;
    ns_node *output;
    gboolean failed;
    gboolean finished;
    gboolean in_markup;
    gboolean in_char_ref;
    char quote;
};

static void
lxb_doc_destroy_void(void *p)
{
    if (p) lxb_html_document_destroy((lxb_html_document_t *)p);
}

/* Copies lexbor attributes onto out, borrowing strings; xlink/xml/xmlns get namespaces. */
static void
lxb_borrow_attributes(lxb_dom_element_t *el, ns_node *out)
{
    lxb_dom_attr_t *attr = lxb_dom_element_first_attribute(el);
    while (attr) {
        size_t klen = 0, vlen = 0;
        const lxb_char_t *k = lxb_dom_attr_qualified_name(attr, &klen);
        const lxb_char_t *v = lxb_dom_attr_value(attr, &vlen);
        if (k && klen > 0 && !ns_attr_name_is_internal((const char *)k)) {
            (void)klen;
            (void)vlen;
            const char *uri = NULL;
            switch (attr->node.ns) {
            case LXB_NS_XLINK: uri = "http://www.w3.org/1999/xlink"; break;
            case LXB_NS_XML:   uri = "http://www.w3.org/XML/1998/namespace"; break;
            case LXB_NS_XMLNS: uri = "http://www.w3.org/2000/xmlns/"; break;
            default: break;
            }
            if (uri) {
                size_t llen = 0;
                const lxb_char_t *ln = lxb_dom_attr_local_name(attr, &llen);
                const char *qn = (const char *)k;
                const char *colon = strchr(qn, ':');
                char *prefix = colon ? g_strndup(qn, (gsize)(colon - qn)) : NULL;
                char *local = (ln && llen) ? g_strndup((const char *)ln, llen)
                                           : g_strdup(qn);
                if (!ns_attr_name_is_internal(local))
                    ns_element_set_attr_ns(out, uri, prefix, local, qn,
                                           v ? (const char *)v : "");
                g_free(prefix);
                g_free(local);
            } else {
                ns_element_append_attr_borrow(out,
                    (const char *)k,
                    v ? (const char *)v : "");
            }
        }
        attr = lxb_dom_element_next_attribute(attr);
    }
}

/* Converts one lexbor node to an ns_node (children not included); NULL if unsupported. */
static ns_node *
lxb_node_convert(lxb_dom_node_t *src)
{
    switch (src->type) {
    case LXB_DOM_NODE_TYPE_DOCUMENT:
    case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
        return ns_node_new_document();
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        lxb_dom_element_t *el = lxb_dom_interface_element(src);
        size_t nlen = 0;
        const lxb_char_t *name = lxb_dom_element_qualified_name(el, &nlen);
        (void)nlen;
        ns_node *out = ns_node_new_element(NULL);
        ns_node_set_name_borrow(out, name ? (const char *)name : "unknown");
        lxb_borrow_attributes(el, out);
        if (src->ns == LXB_NS_SVG) {
            out->flags |= NS_NODE_SVG_NS;
            ns_element_set_attr(out, "data-nd-ns-uri",
                                "http://www.w3.org/2000/svg");
        } else if (src->ns == LXB_NS_MATH) {
            out->flags |= NS_NODE_FOREIGN_NS;
            ns_element_set_attr(out, "data-nd-ns-uri",
                                "http://www.w3.org/1998/Math/MathML");
        }
        return out;
    }
    case LXB_DOM_NODE_TYPE_TEXT:
    case LXB_DOM_NODE_TYPE_CDATA_SECTION: {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(src);
        ns_node *out = ns_node_new_text(NULL);
        ns_node_set_text_borrow(out, cd->data.data ? (const char *)cd->data.data : "");
        return out;
    }
    case LXB_DOM_NODE_TYPE_COMMENT: {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(src);
        const char *text = cd->data.data ? (const char *)cd->data.data : "";
        ns_node *out = ns_node_new_comment(NULL);
        ns_node_set_text_borrow(out, text);
        return out;
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
        lxb_dom_document_type_t *dt = lxb_dom_interface_document_type(src);
        size_t nlen = 0, plen = 0, slen = 0;
        const lxb_char_t *name = lxb_dom_document_type_name(dt, &nlen);
        const lxb_char_t *pub = lxb_dom_document_type_public_id(dt, &plen);
        const lxb_char_t *sys = lxb_dom_document_type_system_id(dt, &slen);
        return ns_node_new_doctype(
            g_strndup(name && nlen ? (const char *)name : "html",
                      name && nlen ? nlen : 4),
            g_strndup(pub && plen ? (const char *)pub : "", plen),
            g_strndup(sys && slen ? (const char *)sys : "", slen));
    }
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_ATTRIBUTE:
    case LXB_DOM_NODE_TYPE_ENTITY_REFERENCE:
    case LXB_DOM_NODE_TYPE_ENTITY:
    case LXB_DOM_NODE_TYPE_NOTATION:
    default:
        return NULL;
    }
}

/* First child of an HTML template element's content fragment, or NULL. */
static lxb_dom_node_t *
lxb_template_content_first_child(lxb_dom_node_t *src)
{
    if (src->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    if (src->ns != LXB_NS_HTML) return NULL;
    if (src->local_name != LXB_TAG_TEMPLATE) return NULL;
    lxb_html_template_element_t *tpl = lxb_html_interface_template(src);
    if (!tpl || !tpl->content) return NULL;
    return tpl->content->node.first_child;
}

typedef struct lxb_walk_frame {
    lxb_dom_node_t *src_child;
    ns_node        *ns_parent;
} lxb_walk_frame;

static void
lxb_walk_push(GArray *stack, lxb_dom_node_t *child, ns_node *parent)
{
    if (!child || !parent) return;
    lxb_walk_frame fr = { .src_child = child, .ns_parent = parent };
    g_array_append_val(stack, fr);
}

/* Iteratively converts a lexbor subtree (including template contents) under ns_root. */
static void
lxb_walk_into(lxb_dom_node_t *src_root, ns_node *ns_root)
{
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(lxb_walk_frame));
    lxb_walk_push(stack, src_root->first_child, ns_root);
    lxb_walk_push(stack, lxb_template_content_first_child(src_root), ns_root);
    while (stack->len > 0) {
        lxb_walk_frame fr = g_array_index(stack, lxb_walk_frame, stack->len - 1);
        g_array_set_size(stack, stack->len - 1);
        lxb_dom_node_t *src = fr.src_child;
        ns_node *parent = fr.ns_parent;
        while (src) {
            lxb_dom_node_t *next = src->next;
            ns_node *converted = lxb_node_convert(src);
            if (converted) {
                ns_node_append_child(parent, converted);
                lxb_dom_node_t *kids = src->first_child;
                lxb_dom_node_t *tpl_kids = lxb_template_content_first_child(src);
                if (next) lxb_walk_push(stack, next, parent);
                if (tpl_kids)
                    lxb_walk_push(stack, tpl_kids,
                                  ns_template_content_get(converted));
                if (kids) {
                    src = kids;
                    parent = converted;
                    continue;
                }
            } else if (next) {
                src = next;
                continue;
            }
            src = NULL;
        }
    }
    g_array_free(stack, TRUE);
}

/* Converts a whole lexbor tree into a new ns_node tree. */
static ns_node *
lxb_to_nd_root(lxb_dom_node_t *root)
{
    if (!root) return NULL;
    ns_node *out = lxb_node_convert(root);
    if (!out) return NULL;
    lxb_walk_into(root, out);
    return out;
}

/* True for element names allowed to host a shadow root (attachShadow list or custom). */
static gboolean
ns_valid_shadow_host(const char *name)
{
    if (!name) return FALSE;
    static const char *const hosts[] = {
        "article", "aside", "blockquote", "body", "div", "footer",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav",
        "p", "section", "span",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(hosts); i++)
        if (g_ascii_strcasecmp(name, hosts[i]) == 0) return TRUE;
    return strchr(name, '-') != NULL;
}

/* Converts declarative shadow DOM templates into internal shadow root elements. */
static void
ns_dsd_convert(ns_node *n, int depth)
{
    if (!n || depth >= 512) return;
    gboolean host_ok = n->kind == NS_NODE_ELEMENT &&
                       ns_valid_shadow_host(n->name);
    gboolean shadow_done = FALSE;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (host_ok && !shadow_done && c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "template") == 0) {
            const char *mode = ns_element_get_attr(c, "shadowrootmode");
            if (!mode) mode = ns_element_get_attr(c, "shadowroot");
            if (mode && (g_ascii_strcasecmp(mode, "open") == 0 ||
                         g_ascii_strcasecmp(mode, "closed") == 0)) {
                shadow_done = TRUE;
                gboolean delegates =
                    ns_element_get_attr(c, "shadowrootdelegatesfocus") != NULL;
                gboolean serializable =
                    ns_element_get_attr(c, "shadowrootserializable") != NULL;
                gboolean clonable =
                    ns_element_get_attr(c, "shadowrootclonable") != NULL;
                ns_node_set_name_borrow(c, "div");
                ns_element_remove_attr(c, "shadowrootmode");
                ns_element_remove_attr(c, "shadowroot");
                ns_element_remove_attr(c, "shadowrootdelegatesfocus");
                ns_element_remove_attr(c, "shadowrootserializable");
                ns_element_remove_attr(c, "shadowrootclonable");
                if (c->tpl_content) {
                    ns_node *frag = c->tpl_content;
                    c->tpl_content = NULL;
                    ns_node *ch = frag->first_child;
                    while (ch) {
                        ns_node *next = ch->next_sibling;
                        ns_node_remove(ch);
                        ns_node_append_child(c, ch);
                        ch = next;
                    }
                    ns_node_free(frag);
                }
                ns_element_set_attr(c, NS_SHADOW_ATTR,
                    g_ascii_strcasecmp(mode, "closed") == 0 ? "closed" : "open");
                ns_element_set_attr(c, "data-nd-shadow-declarative", "1");
                if (delegates)
                    ns_element_set_attr(c, "data-nd-shadow-delegates", "1");
                if (serializable)
                    ns_element_set_attr(c, "data-nd-shadow-serializable", "1");
                if (clonable)
                    ns_element_set_attr(c, "data-nd-shadow-clonable", "1");
            }
        }
        ns_dsd_convert(c, depth + 1);
    }
    if (n->tpl_content)
        ns_dsd_convert(n->tpl_content, depth + 1);
}

/* Appends all descendant text of n to out (depth-limited). */
static void
text_descendants_append(const ns_node *n, GString *out, int depth)
{
    if (!n || !out || depth >= 64) return;
    if (n->kind == NS_NODE_TEXT && n->text)
        g_string_append(out, n->text);
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        text_descendants_append(c, out, depth + 1);
}

/* Returns the text content of a script element; caller frees. */
static char *
script_text(const ns_node *n)
{
    GString *out = g_string_new(NULL);
    text_descendants_append(n, out, 0);
    return g_string_free(out, FALSE);
}

/* Unescapes a JSON string body up to its closing quote; caller frees. */
static char *
json_string_unescape(const char *p)
{
    if (!p) return NULL;
    GString *out = g_string_new(NULL);
    for (; *p && *p != '"'; p++) {
        if (*p != '\\') {
            g_string_append_c(out, *p);
            continue;
        }
        p++;
        if (!*p) break;
        if (*p == '/' || *p == '"' || *p == '\\') {
            g_string_append_c(out, *p);
        } else if (*p == 'n') {
            g_string_append_c(out, '\n');
        } else if (*p == 't') {
            g_string_append_c(out, '\t');
        } else if (*p == 'u' &&
                   g_ascii_isxdigit(p[1]) && g_ascii_isxdigit(p[2]) &&
                   g_ascii_isxdigit(p[3]) && g_ascii_isxdigit(p[4])) {
            char hex[5] = { p[1], p[2], p[3], p[4], 0 };
            gunichar ch = (gunichar)g_ascii_strtoull(hex, NULL, 16);
            if (ch) {
                char buf[8] = {0};
                gint len = g_unichar_to_utf8(ch, buf);
                g_string_append_len(out, buf, len);
            }
            p += 4;
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

/* Finds "key": "..." in raw JSON text and returns a pointer after the opening quote. */
static const char *
json_string_value_for_key(const char *text, const char *key, const char **out_key)
{
    const char *p = text;
    gsize klen = key ? strlen(key) : 0;
    if (!text || !key || klen == 0) return NULL;
    while ((p = strstr(p, key))) {
        const char *q = p + klen;
        while (g_ascii_isspace(*q)) q++;
        if (*q == ':') {
            q++;
            while (g_ascii_isspace(*q)) q++;
            if (*q == '"') {
                if (out_key) *out_key = p;
                return q + 1;
            }
        }
        p += klen;
    }
    return NULL;
}

/* Unescaped string value of the first occurrence of key in JSON text; caller frees. */
static char *
json_first_url_for_key(const char *text, const char *key)
{
    const char *value = json_string_value_for_key(text, key, NULL);
    return value ? json_string_unescape(value) : NULL;
}

static gboolean
attr_contains_word(const ns_node *n, const char *attr, const char *word)
{
    const char *v = ns_element_get_attr(n, attr);
    return v && word && g_strstr_len(v, -1, word) != NULL;
}

/* First element whose attribute value contains word as a substring. */
static ns_node *
find_element_with_attr_contains(ns_node *n, const char *attr, const char *word,
                                int depth)
{
    if (!n || depth >= 512) return NULL;
    if (n->kind == NS_NODE_ELEMENT && attr_contains_word(n, attr, word))
        return n;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        ns_node *hit = find_element_with_attr_contains(c, attr, word, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

/* Element to annotate with extracted media: first video, #player, or a .player element. */
static ns_node *
document_media_target(const ns_node *root)
{
    ns_node *video = ns_node_find_first_element(root, "video");
    if (video) return video;
    ns_node *target = ns_node_find_by_id(root, "player");
    if (target) return target;
    return find_element_with_attr_contains((ns_node *)root, "class", "player",
                                           0);
}

/* Finds a meta element with this property or name and non-empty content. */
static ns_node *
find_meta_property(ns_node *n, const char *prop, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name && strcmp(n->name, "meta") == 0) {
        const char *p = ns_element_get_attr(n, "property");
        if (!p) p = ns_element_get_attr(n, "name");
        const char *c = ns_element_get_attr(n, "content");
        if (p && c && *c && g_ascii_strcasecmp(p, prop) == 0)
            return n;
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        ns_node *hit = find_meta_property(c, prop, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

/* Content of the matching meta property; caller frees, NULL if absent. */
static char *
meta_property_content(const ns_node *root, const char *prop)
{
    ns_node *m = find_meta_property((ns_node *)root, prop, 0);
    const char *c = m ? ns_element_get_attr(m, "content") : NULL;
    return (c && *c) ? g_strdup(c) : NULL;
}

/* String value for key in the first JSON-LD script mentioning VideoObject; caller frees. */
static char *
jsonld_videoobject_url(const ns_node *n, const char *key, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        strcmp(n->name, "script") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        if (type && g_ascii_strcasecmp(type, "application/ld+json") == 0) {
            g_autofree char *text = script_text(n);
            if (text && strstr(text, "VideoObject")) {
                char *v = json_first_url_for_key(text, key);
                if (v && *v) return v;
                g_free(v);
            }
        }
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        char *hit = jsonld_videoobject_url(c, key, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

/* True for http(s) URLs whose path ends in a natively playable container extension. */
static gboolean
media_url_is_direct_playable(const char *url)
{
    if (!url || !g_str_has_prefix(url, "http")) return FALSE;
    const char *cut = strpbrk(url, "?#");
    gsize len = cut ? (gsize)(cut - url) : strlen(url);
    const char *dot = NULL;
    for (const char *p = url; p < url + len; p++)
        if (*p == '.') dot = p;
    if (!dot) return FALSE;
    g_autofree char *ext = g_ascii_strdown(dot, (gssize)(url + len - dot));
    return strcmp(ext, ".webm") == 0 || strcmp(ext, ".mpg") == 0 ||
           strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".m1v") == 0 ||
           strcmp(ext, ".ogv") == 0 || strcmp(ext, ".ogg") == 0;
}

/* Returns a if non-NULL (freeing b), else b. */
static char *
first_nonempty(char *a, char *b)
{
    if (a) { g_free(b); return a; }
    return b;
}

/* Marks the media target with a video src or stream flag and poster from page metadata. */
static void
ns_media_extract_standard(const ns_node *root)
{
    ns_node *target = document_media_target(root);
    if (!target || ns_element_get_attr(target, NS_MEDIA_SRC_ATTR) ||
        ns_element_get_attr(target, NS_MEDIA_STREAM_ATTR))
        return;

    char *direct = jsonld_videoobject_url(root, "\"contentUrl\"", 0);
    if (direct && !media_url_is_direct_playable(direct)) {
        g_free(direct);
        direct = NULL;
    }
    char *og_video = meta_property_content(root, "og:video:secure_url");
    og_video = first_nonempty(og_video,
                              meta_property_content(root, "og:video:url"));
    og_video = first_nonempty(og_video,
                              meta_property_content(root, "og:video"));
    char *tw_player = meta_property_content(root, "twitter:player");

    char *poster = meta_property_content(root, "og:image:secure_url");
    poster = first_nonempty(poster, meta_property_content(root, "og:image"));
    poster = first_nonempty(poster,
                            jsonld_videoobject_url(root, "\"thumbnailUrl\"", 0));
    poster = first_nonempty(poster,
                            meta_property_content(root, "twitter:image"));

    char *embed = jsonld_videoobject_url(root, "\"embedUrl\"", 0);
    gboolean has_video = direct || og_video || tw_player || embed;
    if (has_video) {
        if (direct)
            ns_element_set_attr(target, NS_MEDIA_SRC_ATTR, direct);
        else if (og_video && media_url_is_direct_playable(og_video))
            ns_element_set_attr(target, NS_MEDIA_SRC_ATTR, og_video);
        else
            ns_element_set_attr(target, NS_MEDIA_STREAM_ATTR, "1");
        if (poster && *poster)
            ns_element_set_attr(target, NS_MEDIA_POSTER_ATTR, poster);
    }
    g_free(direct);
    g_free(og_video);
    g_free(tw_player);
    g_free(poster);
    g_free(embed);
}

/* Returns the position just after delim, or NULL if not found before end. */
static const char *
xml_skip_until(const char *p, const char *end, const char *delim)
{
    size_t dl = strlen(delim);
    while (p + dl <= end) {
        if (memcmp(p, delim, dl) == 0) return p + dl;
        p++;
    }
    return NULL;
}

/* Extracts the xmlns default namespace value from a root start tag; caller frees. */
static char *
xml_root_default_namespace(const char *s, const char *end)
{
    for (const char *p = s; p < end; p++) {
        if (p != s && !g_ascii_isspace(p[-1])) continue;
        if (end - p < 5 || strncmp(p, "xmlns", 5) != 0) continue;
        const char *a = p + 5;
        while (a < end && g_ascii_isspace(*a)) a++;
        if (a >= end || *a != '=') continue;
        a++;
        while (a < end && g_ascii_isspace(*a)) a++;
        if (a >= end || (*a != '"' && *a != '\'')) continue;
        char q = *a++;
        const char *v = a;
        while (a < end && *a != q) a++;
        return g_strndup(v, (gsize)(a - v));
    }
    return NULL;
}

/* Lightweight tag-balance well-formedness check; optionally returns the root namespace. */
gboolean
ns_xml_well_formed(const char *input, gssize len, char **out_root_ns)
{
    if (out_root_ns) *out_root_ns = NULL;
    if (!input) return FALSE;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    const char *p = input;
    const char *end = input + n;
    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
    gboolean ok = TRUE;
    gboolean root_seen = FALSE;
    gboolean root_closed = FALSE;

    while (p < end && ok) {
        if (*p != '<') { p++; continue; }
        const char *q = p + 1;
        if (q < end && *q == '!') {
            if (end - q >= 3 && strncmp(q, "!--", 3) == 0) {
                const char *e = xml_skip_until(q + 3, end, "-->");
                if (!e) { ok = FALSE; break; }
                p = e; continue;
            }
            if (end - q >= 8 && strncmp(q, "![CDATA[", 8) == 0) {
                const char *e = xml_skip_until(q + 8, end, "]]>");
                if (!e) { ok = FALSE; break; }
                p = e; continue;
            }
            const char *e;
            if (end - q >= 8 && g_ascii_strncasecmp(q, "!DOCTYPE", 8) == 0) {
                /* Same scanner as the parser, so both agree on where the
                 * DOCTYPE (and its internal subset) ends. */
                e = ns_xml_doctype_end(q + 8, end);
                if (!e) { ok = FALSE; break; }
            } else {
                e = q;
                while (e < end && *e != '>') e++;
            }
            if (e >= end) { ok = FALSE; break; }
            p = e + 1; continue;
        }
        if (q < end && *q == '?') {
            const char *e = xml_skip_until(q + 1, end, "?>");
            if (!e) { ok = FALSE; break; }
            p = e; continue;
        }
        gboolean is_end = (q < end && *q == '/');
        if (is_end) q++;
        const char *te = q;
        char quote = 0;
        while (te < end) {
            char c = *te;
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') break;
            te++;
        }
        if (te >= end) { ok = FALSE; break; }
        gboolean self_close = FALSE;
        const char *inner_end = te;
        if (!is_end) {
            const char *t = te - 1;
            while (t > q && g_ascii_isspace(*t)) t--;
            if (*t == '/') { self_close = TRUE; inner_end = t; }
        }
        const char *ne = q;
        while (ne < inner_end && !g_ascii_isspace(*ne) && *ne != '/') ne++;
        if (ne == q) { ok = FALSE; break; }
        char *name = g_strndup(q, (gsize)(ne - q));

        if (is_end) {
            if (stack->len == 0 ||
                strcmp(g_ptr_array_index(stack, stack->len - 1), name) != 0) {
                ok = FALSE; g_free(name); break;
            }
            g_ptr_array_remove_index(stack, stack->len - 1);
            if (stack->len == 0) root_closed = TRUE;
            g_free(name);
        } else {
            if (root_closed) { ok = FALSE; g_free(name); break; }
            if (!root_seen) {
                root_seen = TRUE;
                if (out_root_ns)
                    *out_root_ns = xml_root_default_namespace(ne, inner_end);
            }
            if (self_close) {
                if (stack->len == 0) root_closed = TRUE;
                g_free(name);
            } else {
                g_ptr_array_add(stack, name);
            }
        }
        p = te + 1;
    }
    if (ok && (stack->len != 0 || !root_seen)) ok = FALSE;
    g_ptr_array_free(stack, TRUE);
    if (!ok && out_root_ns) { g_free(*out_root_ns); *out_root_ns = NULL; }
    return ok;
}

/* Collects script elements in document order. */
static void
ns_collect_script_elems(ns_node *n, GPtrArray *out, int depth)
{
    if (!n || depth >= 512) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "script") == 0)
            g_ptr_array_add(out, c);
        ns_collect_script_elems(c, out, depth + 1);
    }
}

/* Sets each script element's source line by scanning the input for script start tags. */
static void
ns_html_assign_script_positions(ns_node *root, const char *input, size_t len)
{
    if (!root || !input) return;
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(int));
    int line = 1;
    size_t i = 0;
    while (i < len) {
        if (input[i] == '<' && i + 7 <= len &&
            g_ascii_strncasecmp(input + i, "<script", 7) == 0 &&
            (i + 7 == len || !g_ascii_isalnum(input[i + 7]))) {
            size_t j = i;
            int l = line;
            while (j < len && input[j] != '>') {
                if (input[j] == '\n') l++;
                j++;
            }
            if (j < len) {
                g_array_append_val(lines, l);
                line = l;
                i = j + 1;
                continue;
            }
        }
        if (input[i] == '\n') line++;
        i++;
    }
    GPtrArray *elems = g_ptr_array_new();
    ns_collect_script_elems(root, elems, 0);
    guint m = MIN(elems->len, lines->len);
    for (guint k = 0; k < m; k++) {
        ns_node *e = g_ptr_array_index(elems, k);
        e->src_line = g_array_index(lines, int, k);
    }
    g_ptr_array_free(elems, TRUE);
    g_array_free(lines, TRUE);
}

/* Ends parsing and converts the document, applying quirks, DSD and media extraction. */
static ns_node *
ns_html_parser_finish(ns_html_parser *parser)
{
    if (!parser || parser->finished || parser->failed) return NULL;
    parser->finished = TRUE;
    if (lxb_html_document_parse_chunk_end(parser->document) != LXB_STATUS_OK) {
        parser->failed = TRUE;
        return NULL;
    }
    ns_node *root = lxb_to_nd_root(lxb_dom_interface_node(parser->document));
    if (!root) return NULL;
    if (parser->document->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS)
        root->flags |= NS_NODE_QUIRKS;
    else if (parser->document->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_LIMITED_QUIRKS)
        root->flags |= NS_NODE_LIMITED_QUIRKS;
    if (!parser->scripting) root->flags |= NS_NODE_SCRIPTING_DISABLED;
    ns_html_assign_script_positions(root, parser->source->str,
                                    parser->source->len);
    ns_dsd_convert(root, 0);
    ns_media_extract_standard(root);
    ns_node_attach_backing(root, parser->document, lxb_doc_destroy_void);
    parser->document = NULL;
    return root;
}

/* Creates a chunked lexbor document parser with scripting flag set. */
static ns_html_parser *
ns_html_parser_new(gboolean scripting)
{
    ns_html_parser *parser = g_new0(ns_html_parser, 1);
    parser->document = lxb_html_document_create();
    parser->source = g_string_new(NULL);
    parser->scripting = scripting;
    if (!parser->document) {
        ns_html_parser_free(parser);
        return NULL;
    }
    lxb_html_document_dom_opt_set(parser->document,
                                  LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    lxb_html_document_scripting_set(parser->document, scripting);
    if (lxb_html_document_parse_chunk_begin(parser->document) !=
        LXB_STATUS_OK) {
        ns_html_parser_free(parser);
        return NULL;
    }
    return parser;
}

/* Feeds a chunk to the parser, keeping a copy of the source for script positions. */
static gboolean
ns_html_parser_write(ns_html_parser *parser, const char *input, gsize len)
{
    if (!parser || parser->finished || parser->failed || (!input && len))
        return FALSE;
    if (len == 0) return TRUE;
    g_string_append_len(parser->source, input, len);
    if (lxb_html_document_parse_chunk(parser->document,
                                      (const lxb_char_t *)input,
                                      len) != LXB_STATUS_OK) {
        parser->failed = TRUE;
        return FALSE;
    }
    return TRUE;
}

static void
ns_html_parser_free(ns_html_parser *parser)
{
    if (!parser) return;
    if (parser->document) lxb_html_document_destroy(parser->document);
    if (parser->source) g_string_free(parser->source, TRUE);
    g_free(parser);
}

/* Parses a full HTML document in 16 KiB chunks; caller frees the tree. */
ns_node *
ns_html_parse_with_scripting(const char *input, gssize len,
                             gboolean scripting)
{
    if (!input) return NULL;
    gsize n = (len < 0) ? strlen(input) : (gsize)len;
    ns_html_parser *parser = ns_html_parser_new(scripting);
    if (!parser) return NULL;
    gsize offset = 0;
    while (offset < n) {
        gsize chunk = MIN((gsize)16384, n - offset);
        if (!ns_html_parser_write(parser, input + offset, chunk)) {
            ns_html_parser_free(parser);
            return NULL;
        }
        offset += chunk;
    }
    ns_node *root = ns_html_parser_finish(parser);
    ns_html_parser_free(parser);
    return root;
}

ns_node *
ns_html_parse(const char *input, gssize len)
{
    return ns_html_parse_with_scripting(input, len, TRUE);
}

/* Lexbor tag ID for a context element name; defaults to body. */
static lxb_tag_id_t
lxb_tag_id_from_name(lxb_html_document_t *doc, const char *name)
{
    if (!name || !*name) return LXB_TAG_BODY;
    lexbor_hash_t *hash = doc->dom_document.tags;
    const lxb_tag_data_t *data = lxb_tag_data_by_name(hash,
        (const lxb_char_t *)name, strlen(name));
    if (!data) return LXB_TAG_BODY;
    return data->tag_id;
}

/* Mirrors a lexbor node into the output tree incrementally, updating changed text. */
static ns_node *
lxb_fragment_sync_node(lxb_dom_node_t *src, ns_node *parent)
{
    ns_node *out = src->user;
    if (!out) {
        out = lxb_node_convert(src);
        if (!out) return NULL;
        if (ns_node_is_element_named(out, "nd-write-flush")) {
            ns_node_free(out);
            return NULL;
        }
        ns_node_own_strings_deep(out);
        src->user = out;
        ns_node_append_child(parent, out);
    } else if ((src->type == LXB_DOM_NODE_TYPE_TEXT ||
                src->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
                src->type == LXB_DOM_NODE_TYPE_COMMENT)) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(src);
        const char *text = cd->data.data ? (const char *)cd->data.data : "";
        if (g_strcmp0(out->text, text) != 0)
            ns_node_replace_text_owned(out, g_strdup(text));
    }
    ns_node *child_parent = out;
    lxb_dom_node_t *child = src->first_child;
    if (src->type == LXB_DOM_NODE_TYPE_ELEMENT &&
        src->ns == LXB_NS_HTML && src->local_name == LXB_TAG_TEMPLATE) {
        child = lxb_template_content_first_child(src);
        child_parent = ns_template_content_get(out);
    }
    while (child) {
        lxb_fragment_sync_node(child, child_parent);
        child = child->next;
    }
    return out;
}

/* Tracks whether streamed input ends inside a tag or character reference. */
static void
lxb_fragment_track_input(ns_html_fragment_parser *stream,
                         const char *input, gsize len)
{
    for (gsize i = 0; i < len; i++) {
        char c = input[i];
        if (stream->in_markup) {
            if (stream->quote) {
                if (c == stream->quote) stream->quote = 0;
            } else if (c == '\'' || c == '"') {
                stream->quote = c;
            } else if (c == '>') {
                stream->in_markup = FALSE;
            }
            continue;
        }
        if (stream->in_char_ref) {
            if (c == ';') stream->in_char_ref = FALSE;
            else if (!(g_ascii_isalnum((guchar)c) || c == '#'))
                stream->in_char_ref = FALSE;
            continue;
        }
        if (c == '<') stream->in_markup = TRUE;
        else if (c == '&') stream->in_char_ref = TRUE;
    }
}

/* True if pending text can be flushed (not mid-tag or inside raw-text elements). */
static gboolean
lxb_fragment_can_flush_text(ns_html_fragment_parser *stream)
{
    if (!stream || stream->in_markup || stream->in_char_ref ||
        !stream->parser || !stream->parser->tree ||
        !stream->parser->tree->open_elements)
        return FALSE;
    lexbor_array_t *open = stream->parser->tree->open_elements;
    for (size_t i = 0; i < open->length; i++) {
        lxb_dom_node_t *node = open->list[i];
        if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (node->local_name == LXB_TAG_SCRIPT ||
            node->local_name == LXB_TAG_STYLE ||
            node->local_name == LXB_TAG_TITLE ||
            node->local_name == LXB_TAG_TEXTAREA ||
            node->local_name == LXB_TAG_PLAINTEXT)
            return FALSE;
    }
    return TRUE;
}

/* Clears the parser-open flag on all mirrored nodes under src. */
static void
lxb_fragment_clear_open(lxb_dom_node_t *src)
{
    for (lxb_dom_node_t *child = src ? src->first_child : NULL;
         child; child = child->next) {
        ns_node *out = child->user;
        if (out) out->flags &= ~NS_NODE_PARSER_OPEN;
        lxb_fragment_clear_open(child);
        lxb_dom_node_t *tpl = lxb_template_content_first_child(child);
        while (tpl) {
            lxb_fragment_clear_open(tpl);
            if (tpl->user)
                ((ns_node *)tpl->user)->flags &= ~NS_NODE_PARSER_OPEN;
            tpl = tpl->next;
        }
    }
}

/* Syncs the lexbor fragment into output, marking still-open script elements. */
static void
lxb_fragment_sync(ns_html_fragment_parser *stream, lxb_dom_node_t *root)
{
    if (!stream || !root) return;
    for (lxb_dom_node_t *child = root->first_child;
         child; child = child->next) {
        if (!child->user)
            lxb_fragment_sync_node(child, stream->output);
        else
            lxb_fragment_sync_node(child, (ns_node *)child->user);
    }
    lxb_fragment_clear_open(root);
    if (!stream->finished && stream->parser && stream->parser->tree &&
        stream->parser->tree->open_elements) {
        lexbor_array_t *open = stream->parser->tree->open_elements;
        for (size_t i = 0; i < open->length; i++) {
            lxb_dom_node_t *node = open->list[i];
            ns_node *out = node ? node->user : NULL;
            if (out && ns_node_is_element_named(out, "script"))
                out->flags |= NS_NODE_PARSER_OPEN;
        }
    }
}

/* Creates a streaming fragment parser (used for document.write) in a context element. */
ns_html_fragment_parser *
ns_html_fragment_parser_new(const char *context_tag, gboolean scripting)
{
    ns_html_fragment_parser *stream = g_new0(ns_html_fragment_parser, 1);
    stream->parser = lxb_html_parser_create();
    stream->document = lxb_html_document_create();
    stream->output = ns_node_new_document();
    if (!scripting) stream->output->flags |= NS_NODE_SCRIPTING_DISABLED;
    if (!stream->parser || !stream->document ||
        lxb_html_parser_init(stream->parser) != LXB_STATUS_OK) {
        ns_html_fragment_parser_free(stream);
        return NULL;
    }
    lxb_html_parser_dom_opt_set(stream->parser,
                               LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    lxb_html_parser_scripting_set(stream->parser, scripting);
    lxb_html_document_scripting_set(stream->document, scripting);
    lxb_tag_id_t tag_id = lxb_tag_id_from_name(stream->document, context_tag);
    if (lxb_html_parse_fragment_chunk_begin(stream->parser, stream->document,
                                            tag_id, LXB_NS_HTML) !=
        LXB_STATUS_OK) {
        ns_html_fragment_parser_free(stream);
        return NULL;
    }
    return stream;
}

/* Feeds streamed markup, forcing a text flush with a sentinel element, then syncs output. */
gboolean
ns_html_fragment_parser_write(ns_html_fragment_parser *stream,
                              const char *input, gsize len)
{
    if (!stream || stream->finished || stream->failed || (!input && len))
        return FALSE;
    if (len) {
        lxb_fragment_track_input(stream, input, len);
        if (lxb_html_parse_fragment_chunk_process(
                stream->parser, (const lxb_char_t *)input, len) !=
            LXB_STATUS_OK) {
            stream->failed = TRUE;
            return FALSE;
        }
        if (lxb_fragment_can_flush_text(stream)) {
            static const char flush[] =
                "<nd-write-flush></nd-write-flush>";
            if (lxb_html_parse_fragment_chunk_process(
                    stream->parser, (const lxb_char_t *)flush,
                    sizeof(flush) - 1) != LXB_STATUS_OK) {
                stream->failed = TRUE;
                return FALSE;
            }
        }
    }
    lxb_fragment_sync(stream, stream->parser->root);
    return TRUE;
}

/* Finishes the streaming fragment parse and syncs the final tree. */
gboolean
ns_html_fragment_parser_finish(ns_html_fragment_parser *stream)
{
    if (!stream || stream->finished || stream->failed) return FALSE;
    lxb_dom_node_t *root =
        lxb_html_parse_fragment_chunk_end(stream->parser);
    stream->finished = TRUE;
    if (!root) {
        stream->failed = TRUE;
        return FALSE;
    }
    lxb_fragment_sync(stream, root);
    return TRUE;
}

/* True if the stream is mid-token or has elements still open beyond the root. */
gboolean
ns_html_fragment_parser_needs_source(const ns_html_fragment_parser *stream)
{
    if (!stream || stream->finished || stream->failed) return FALSE;
    if (stream->in_markup || stream->in_char_ref) return TRUE;
    if (!stream->parser || !stream->parser->tree ||
        !stream->parser->tree->open_elements)
        return FALSE;
    return stream->parser->tree->open_elements->length > 1;
}

ns_node *
ns_html_fragment_parser_output(ns_html_fragment_parser *stream)
{
    return stream ? stream->output : NULL;
}

void
ns_html_fragment_parser_free(ns_html_fragment_parser *stream)
{
    if (!stream) return;
    if (stream->parser) lxb_html_parser_destroy(stream->parser);
    if (stream->document) lxb_html_document_destroy(stream->document);
    if (stream->output) ns_node_free(stream->output);
    g_free(stream);
}

/* HTML fragment parsing algorithm with the given context element and namespace. */
static ns_node *
parse_fragment_in_namespace(const char *context_tag, lxb_ns_id_t context_ns,
                            const char *input, gssize len, gboolean scripting)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (!parser || lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        if (parser) lxb_html_parser_destroy(parser);
        return NULL;
    }
    lxb_html_parser_dom_opt_set(parser, LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    lxb_html_parser_scripting_set(parser, scripting);
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) {
        lxb_html_parser_destroy(parser);
        return NULL;
    }
    lxb_html_document_scripting_set(doc, scripting);
    char *lower = context_tag ? g_ascii_strdown(context_tag, -1) : NULL;
    lxb_tag_id_t tag_id = lxb_tag_id_from_name(doc, lower);
    g_free(lower);
    lxb_dom_node_t *frag = lxb_html_parse_fragment_by_tag_id(
        parser, doc, tag_id, context_ns,
        (const lxb_char_t *)input, n);
    lxb_html_parser_destroy(parser);
    if (!frag) {
        lxb_html_document_destroy(doc);
        return NULL;
    }
    ns_node *out = ns_node_new_document();
    if (!scripting) out->flags |= NS_NODE_SCRIPTING_DISABLED;
    lxb_walk_into(frag, out);
    ns_node_attach_backing(out, doc, lxb_doc_destroy_void);
    return out;
}

ns_node *
ns_html_parse_fragment_with_scripting(const char *context_tag,
                                      const char *input, gssize len,
                                      gboolean scripting)
{
    return parse_fragment_in_namespace(context_tag, LXB_NS_HTML, input, len,
                                       scripting);
}

/* Parses a fragment using a context node, honoring SVG/MathML context namespaces. */
ns_node *
ns_html_parse_fragment_in(const ns_node *context, const char *input,
                          gssize len, gboolean scripting)
{
    const char *tag = context && context->kind == NS_NODE_ELEMENT
        ? context->name : NULL;
    lxb_ns_id_t ns = LXB_NS_HTML;
    if (tag && (context->flags & NS_NODE_SVG_NS)) ns = LXB_NS_SVG;
    else if (tag && (context->flags & NS_NODE_FOREIGN_NS)) ns = LXB_NS_MATH;
    return parse_fragment_in_namespace(tag, ns, input, len, scripting);
}

void
ns_html_convert_declarative_shadow(ns_node *root)
{
    ns_dsd_convert(root, 0);
}
