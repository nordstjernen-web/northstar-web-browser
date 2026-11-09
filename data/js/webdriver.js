/* Northstar Web Browser — WebDriver atoms: the page-side half of the W3C
 * WebDriver commands (src/webdriver.c is the other half).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file evaluates to a factory.  ns_js_webdriver_call() (src/js.c)
 * calls it once per page with the bridge natives and keeps the object it
 * returns in C, so neither the natives nor the element registry is ever
 * reachable from page script.  Every command goes through run(), which
 * takes a plain request object and always returns a JSON string:
 *   {"value": ...}                      success
 *   {"pending": "token"}                a script still waiting on a promise
 *   {"error": "code", "message": ...}   a WebDriver error
 */
(function (natives) {
    'use strict';

    var ELEMENT = 'element-6066-11e4-a52e-4f735466cecf';
    var SHADOW = 'shadow-6066-11e4-a52e-4f735466cecf';
    var WINDOW = 'window-fcc6-11e5-b4f8-330a88ab9d7f';
    var FRAME = 'frame-075b-4da1-b6ba-e579c2d3230a';

    /* Intrinsics captured before any WebDriver command runs, so a page
     * that later patches the prototypes does not reach into the atoms. */
    var G = window;
    var NativeMap = Map, NativeWeakMap = WeakMap;
    var mapGet = Map.prototype.get, mapSet = Map.prototype.set,
        mapHas = Map.prototype.has, mapDelete = Map.prototype['delete'];
    var wmGet = WeakMap.prototype.get, wmSet = WeakMap.prototype.set;
    var objKeys = Object.keys, objCreate = Object.create;
    var isArray = Array.isArray;
    var objToString = Object.prototype.toString;
    var hasOwn = Object.prototype.hasOwnProperty;
    var fnApply = Function.prototype.apply;
    var mathFloor = Math.floor, mathMax = Math.max, mathMin = Math.min;
    var mathRandom = Math.random;
    var NodeCtor = G.Node, ElementCtor = G.Element;
    var ShadowRootCtor = G.ShadowRoot;
    var getComputed = G.getComputedStyle;
    var cryptoObj = G.crypto;

    function call(fn, self, args) { return fnApply.call(fn, self, args || []); }

    var ids = new NativeWeakMap();
    var nodes = new NativeMap();
    var pending = new NativeMap();
    var handle = '';

    function WDError(code, message, extra) {
        this.code = code;
        this.message = message;
        this.extra = extra;
    }

    function fail(code, message, extra) { throw new WDError(code, message, extra); }

    function uuid() {
        if (cryptoObj && typeof cryptoObj.randomUUID === 'function') {
            try { return String(cryptoObj.randomUUID()); } catch (e) { /* fall through */ }
        }
        var hex = '0123456789abcdef', out = '';
        for (var i = 0; i < 36; i++) {
            if (i === 8 || i === 13 || i === 18 || i === 23) { out += '-'; continue; }
            var r = mathFloor(mathRandom() * 16);
            if (i === 14) r = 4;
            else if (i === 19) r = (r & 3) | 8;
            out += hex.charAt(r);
        }
        return out;
    }

    /* ---- small helpers ---- */

    function isNode(v) {
        try { return v !== null && typeof v === 'object' && v instanceof NodeCtor; }
        catch (e) { return false; }
    }

    /* Shadow roots here are element-backed (nodeType 1, instanceof
     * Element), so the ShadowRoot test comes first everywhere. */
    function isShadowRoot(v) {
        if (!isNode(v)) return false;
        if (ShadowRootCtor) {
            try { if (v instanceof ShadowRootCtor) return true; } catch (e) { /* ignore */ }
        }
        return v.nodeType === 11 && !!v.host;
    }

    function isElement(v) {
        try {
            return v !== null && typeof v === 'object' && v instanceof ElementCtor &&
                   !isShadowRoot(v);
        } catch (e) { return false; }
    }

    function lower(s) { return String(s).toLowerCase(); }

    function localName(el) { return el && el.localName ? lower(el.localName) : ''; }

    function px(v) {
        var n = parseFloat(v);
        return isFinite(n) ? n : 0;
    }

    function style(el) {
        try { return getComputed(el); } catch (e) { return null; }
    }

    function idFor(node) {
        var id = call(wmGet, ids, [node]);
        if (!id) {
            id = uuid();
            call(wmSet, ids, [node, id]);
            call(mapSet, nodes, [id, node]);
        }
        return id;
    }

    /* The element or shadow root's composed parent: across shadow
     * boundaries to the host, and never out of a frame's document. */
    function composedParent(n) {
        if (!n) return null;
        if (isShadowRoot(n)) return n.host || null;
        var p = n.parentNode;
        if (p && isShadowRoot(p)) return p.host || null;
        return p && p.nodeType === 1 ? p : null;
    }

    function composedContains(ancestor, n) {
        for (var cur = n; cur; cur = cur.parentNode || cur.host || null) {
            if (cur === ancestor) return true;
            if (cur.nodeType === 9) return false;
        }
        return false;
    }

    /* ---- browsing contexts ---- */

    /* Resolves the frame path (element references, outermost first) to
     * the current browsing context's document, window and frame chain. */
    function context(path) {
        var doc = G.document, win = G, frames = [];
        for (var i = 0; path && i < path.length; i++) {
            var fe = call(mapGet, nodes, [path[i]]);
            if (!fe || !fe.isConnected || fe.ownerDocument !== doc ||
                localName(fe) !== 'iframe')
                fail('no such window', 'The current frame is no longer available');
            var d = natives.frameDocument(fe);
            if (!d) fail('no such window', 'The current frame has no document');
            frames.push(fe);
            doc = d;
            win = natives.frameWindow(fe) || null;
        }
        return { doc: doc, win: win, frames: frames };
    }

    function viewportSize(c) {
        var w = c.win && c.win.innerWidth, h = c.win && c.win.innerHeight;
        var de = c.doc.documentElement;
        if (!(w > 0) && de) w = de.clientWidth;
        if (!(h > 0) && de) h = de.clientHeight;
        return { w: w || 0, h: h || 0 };
    }

    /* Top-level viewport position of the current frame's viewport origin. */
    function frameOffset(c) {
        var x = 0, y = 0;
        for (var i = 0; i < c.frames.length; i++) {
            var fe = c.frames[i], r = fe.getBoundingClientRect(), cs = style(fe);
            x += r.left + (cs ? px(cs.borderLeftWidth) + px(cs.paddingLeft) : 0);
            y += r.top + (cs ? px(cs.borderTopWidth) + px(cs.paddingTop) : 0);
        }
        return { x: x, y: y };
    }

    function scrollPos(c) {
        var w = c.win;
        return {
            x: w ? (w.scrollX || w.pageXOffset || 0) : 0,
            y: w ? (w.scrollY || w.pageYOffset || 0) : 0
        };
    }

    /* ---- element references ---- */

    function known(id, c) {
        if (typeof id !== 'string') fail('invalid argument', 'Element reference is not a string');
        var n = call(mapGet, nodes, [id]);
        if (!n || !isElement(n))
            fail('no such element', 'No element is known with reference ' + id, { unknownRef: id });
        if (!n.isConnected)
            fail('stale element reference',
                 'The element with reference ' + id + ' is no longer attached to the DOM');
        if (n.ownerDocument !== c.doc)
            fail('no such element',
                 'The element with reference ' + id + ' is not in the current browsing context');
        return n;
    }

    function knownShadow(id, c) {
        if (typeof id !== 'string') fail('invalid argument', 'Shadow root reference is not a string');
        var n = call(mapGet, nodes, [id]);
        if (!n || !isShadowRoot(n))
            fail('no such shadow root', 'No shadow root is known with reference ' + id, { unknownRef: id });
        if (!n.host || !n.host.isConnected)
            fail('detached shadow root', 'The shadow root with reference ' + id + ' is detached');
        if (n.host.ownerDocument !== c.doc)
            fail('no such shadow root', 'The shadow root is not in the current browsing context');
        return n;
    }

    function frameWindowRef(win, c) {
        var list = c.doc.querySelectorAll('iframe');
        for (var i = 0; i < list.length; i++) {
            try {
                if (natives.frameWindow(list[i]) === win) {
                    var r = objCreate(null);
                    r[FRAME] = idFor(list[i]);
                    return r;
                }
            } catch (e) { /* ignore */ }
        }
        return null;
    }

    /* ---- JSON clone (serialize) and deserialize ---- */

    var COLLECTION_TAGS = {
        '[object Arguments]': 1, '[object NodeList]': 1, '[object HTMLCollection]': 1,
        '[object FileList]': 1, '[object HTMLAllCollection]': 1,
        '[object HTMLFormControlsCollection]': 1, '[object HTMLOptionsCollection]': 1,
        '[object RadioNodeList]': 1
    };

    function isCollection(v) {
        if (isArray(v)) return true;
        var tag;
        try { tag = call(objToString, v); } catch (e) { return false; }
        if (COLLECTION_TAGS[tag]) return true;
        try {
            if (G.NodeList && v instanceof G.NodeList) return true;
            if (G.HTMLCollection && v instanceof G.HTMLCollection) return true;
        } catch (e) { /* ignore */ }
        return false;
    }

    function isWindowLike(v) {
        try { return v === G || (v !== null && typeof v === 'object' && v.window === v && !!v.document); }
        catch (e) { return false; }
    }

    function serialize(v, c, seen) {
        if (v === undefined || v === null) return null;
        var t = typeof v;
        if (t === 'boolean' || t === 'string') return v;
        if (t === 'number') return isFinite(v) ? v : null;
        if (t === 'bigint') fail('javascript error', 'A BigInt value cannot be serialized');
        if (t === 'symbol') return null;
        if (isShadowRoot(v)) {
            if (!v.host || !v.host.isConnected)
                fail('detached shadow root', 'The returned shadow root is detached');
            var s = objCreate(null);
            s[SHADOW] = idFor(v);
            return s;
        }
        if (isElement(v)) {
            if (!v.isConnected)
                fail('stale element reference', 'The returned element is no longer attached to the DOM');
            var e = objCreate(null);
            e[ELEMENT] = idFor(v);
            return e;
        }
        if (isWindowLike(v)) {
            if (v === G) {
                var w = objCreate(null);
                w[WINDOW] = handle;
                return w;
            }
            var f = frameWindowRef(v, c);
            if (f) return f;
        }
        for (var i = 0; i < seen.length; i++)
            if (seen[i] === v) fail('javascript error', 'Cyclic object value');
        if (seen.length > 512) fail('javascript error', 'Object nesting is too deep to serialize');
        seen.push(v);
        try {
            if (isCollection(v)) {
                var arr = [], n = v.length >>> 0;
                for (var k = 0; k < n; k++) arr.push(serialize(v[k], c, seen));
                return arr;
            }
            if (typeof v.toJSON === 'function') return serialize(v.toJSON(), c, seen);
            var out = objCreate(null), keys = objKeys(v);
            for (var j = 0; j < keys.length; j++) out[keys[j]] = serialize(v[keys[j]], c, seen);
            return out;
        } finally {
            seen.pop();
        }
    }

    function deserialize(v, c, depth) {
        if (v === null || typeof v !== 'object') return v;
        if ((depth || 0) > 512) fail('javascript error', 'Argument nesting is too deep');
        if (isArray(v)) {
            var arr = [];
            for (var i = 0; i < v.length; i++) arr.push(deserialize(v[i], c, (depth || 0) + 1));
            return arr;
        }
        if (call(hasOwn, v, [ELEMENT])) return known(v[ELEMENT], c);
        if (call(hasOwn, v, [SHADOW])) return knownShadow(v[SHADOW], c);
        if (call(hasOwn, v, [WINDOW])) {
            if (v[WINDOW] === handle) return G;
            fail('no such window', 'Unknown window reference ' + v[WINDOW]);
        }
        if (call(hasOwn, v, [FRAME])) {
            var fe = known(v[FRAME], c);
            var fw = localName(fe) === 'iframe' ? natives.frameWindow(fe) : null;
            if (!fw) fail('no such frame', 'The frame reference does not name a frame');
            return fw;
        }
        var out = {}, keys = objKeys(v);
        for (var j = 0; j < keys.length; j++) out[keys[j]] = deserialize(v[keys[j]], c, (depth || 0) + 1);
        return out;
    }

    /* Our own JSON writer, so a page's patched JSON or toJSON is never consulted. */
    function quote(s) {
        var out = '"';
        for (var i = 0; i < s.length; i++) {
            var ch = s.charAt(i), code = s.charCodeAt(i);
            if (ch === '"') out += '\\"';
            else if (ch === '\\') out += '\\\\';
            else if (code < 0x20 || code === 0x2028 || code === 0x2029 ||
                     (code >= 0xD800 && code <= 0xDFFF &&
                      !(code <= 0xDBFF && i + 1 < s.length &&
                        s.charCodeAt(i + 1) >= 0xDC00 && s.charCodeAt(i + 1) <= 0xDFFF) &&
                      !(code >= 0xDC00 && i > 0 && s.charCodeAt(i - 1) >= 0xD800 &&
                        s.charCodeAt(i - 1) <= 0xDBFF))) {
                var hex = code.toString(16);
                out += '\\u' + '0000'.substring(hex.length) + hex;
            } else {
                out += ch;
            }
        }
        return out + '"';
    }

    function stringify(v) {
        if (v === null || v === undefined) return 'null';
        var t = typeof v;
        if (t === 'boolean') return v ? 'true' : 'false';
        if (t === 'number') return isFinite(v) ? String(v) : 'null';
        if (t === 'string') return quote(v);
        if (isArray(v)) {
            var parts = [];
            for (var i = 0; i < v.length; i++) parts.push(stringify(v[i]));
            return '[' + parts.join(',') + ']';
        }
        var keys = objKeys(v), items = [];
        for (var j = 0; j < keys.length; j++) items.push(quote(keys[j]) + ':' + stringify(v[keys[j]]));
        return '{' + items.join(',') + '}';
    }

    function result(fields) {
        var o = objCreate(null);
        for (var k in fields) if (call(hasOwn, fields, [k])) o[k] = fields[k];
        return { __raw: o };
    }

    function errorOf(e) {
        var o = objCreate(null);
        if (e instanceof WDError) {
            o.error = e.code;
            o.message = e.message;
            o.stacktrace = '';
            if (e.extra) for (var k in e.extra) o[k] = e.extra[k];
            return o;
        }
        o.error = 'javascript error';
        var msg = '';
        try {
            msg = e && typeof e === 'object' && 'message' in e
                ? (e.name ? e.name + ': ' : '') + String(e.message) : String(e);
        } catch (x) { msg = 'Script error'; }
        o.message = msg;
        try { o.stacktrace = e && e.stack ? String(e.stack) : ''; } catch (x) { o.stacktrace = ''; }
        return o;
    }

    /* ---- element state ---- */

    var BOOLEAN_ATTRIBUTES = {};
    ('allowfullscreen allowpaymentrequest allowusermedia async autofocus autoplay checked ' +
     'compact complete controls declare default defaultchecked defaultselected defer disabled ' +
     'ended formnovalidate hidden indeterminate iscontenteditable ismap itemscope loop multiple ' +
     'muted nohref nomodule noresize noshade novalidate nowrap open paused playsinline pubdate ' +
     'readonly required reversed scoped seamless seeking selected truespeed typemustmatch ' +
     'willvalidate').split(' ').forEach(function (n) { BOOLEAN_ATTRIBUTES[n] = 1; });

    function isXml(doc) {
        return doc && doc.documentElement && doc.documentElement.namespaceURI !== null &&
               doc.contentType && doc.contentType !== 'text/html' &&
               /xml/.test(doc.contentType);
    }

    function getAttribute(el, name) {
        name = String(name);
        if (BOOLEAN_ATTRIBUTES[lower(name)] && !isXml(el.ownerDocument))
            return el.hasAttribute(name) ? 'true' : null;
        var v = el.getAttribute(name);
        return v === null || v === undefined ? null : String(v);
    }

    var FORM_CONTROLS = { button: 1, input: 1, select: 1, textarea: 1, optgroup: 1, option: 1, fieldset: 1 };

    function isEnabled(el) {
        if (isXml(el.ownerDocument)) return false;
        var name = localName(el);
        if (!FORM_CONTROLS[name]) return true;
        try { if (typeof el.matches === 'function') return !el.matches(':disabled'); }
        catch (e) { /* fall back below */ }
        for (var n = el; n && n.nodeType === 1; n = n.parentNode) {
            var nn = localName(n);
            if (FORM_CONTROLS[nn] && n.hasAttribute('disabled')) return false;
        }
        return true;
    }

    function isSelected(el) {
        var name = localName(el);
        if (name === 'input') {
            var type = lower(el.type || el.getAttribute('type') || 'text');
            if (type === 'checkbox' || type === 'radio') return !!el.checked;
            return false;
        }
        if (name === 'option') return !!el.selected;
        return false;
    }

    function hasPositiveSize(el, depth) {
        if (depth > 64) return false;
        var rects;
        try { rects = el.getClientRects(); } catch (e) { rects = null; }
        if (rects) {
            for (var i = 0; i < rects.length; i++)
                if (rects[i].width > 0 && rects[i].height > 0) return true;
        }
        var cs = style(el);
        if (cs && cs.overflow === 'hidden' && rects && rects.length) return false;
        for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
            var ccs = style(c);
            if (ccs && (ccs.display === 'none' || ccs.visibility === 'hidden')) continue;
            if (hasPositiveSize(c, depth + 1)) return true;
        }
        if (el.shadowRoot) {
            for (var s = el.shadowRoot.firstElementChild; s; s = s.nextElementSibling)
                if (hasPositiveSize(s, depth + 1)) return true;
        }
        return false;
    }

    var NEVER_SHOWN = { head: 1, title: 1, meta: 1, link: 1, script: 1, style: 1,
                        template: 1, noscript: 1, base: 1 };

    /* An approximation of Selenium's isShown atom (bot.dom.isShown). */
    function isDisplayed(el) {
        if (!el || !el.isConnected) return false;
        var name = localName(el);
        if (name === 'html' || name === 'body') return true;
        if (NEVER_SHOWN[name]) return false;
        if (name === 'option' || name === 'optgroup') {
            for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode) {
                var pn = localName(p);
                if (pn === 'select') return isDisplayed(p);
                if (pn === 'datalist') return false;
            }
        }
        if (name === 'input' && lower(el.type || '') === 'hidden') return false;
        var prev = null;
        for (var n = el; n; prev = n, n = composedParent(n)) {
            var cs = style(n);
            if (cs && cs.display === 'none') return false;
            if (cs && parseFloat(cs.opacity) === 0) return false;
            if (localName(n) === 'details' && !n.open && prev &&
                !(localName(prev) === 'summary' && prev.parentNode === n))
                return false;
        }
        var own = style(el);
        if (own && (own.visibility === 'hidden' || own.visibility === 'collapse')) return false;
        /* The engine parses frame documents but does not lay them out, so
         * nothing inside an unrendered frame has a size to measure. */
        if (inUnrenderedFrame(el)) return true;
        return hasPositiveSize(el, 0);
    }

    var curCtx = null;

    function inUnrenderedFrame(el) {
        if (!curCtx || !curCtx.frames.length || el.ownerDocument !== curCtx.doc) return false;
        for (var i = 0; i < curCtx.frames.length; i++) {
            var r = curCtx.frames[i].getBoundingClientRect();
            if (!(r.width > 0 && r.height > 0)) return true;
        }
        return false;
    }

    function normalizeText(t) {
        t = String(t).replace(/\r\n?/g, '\n').replace(/ /g, ' ');
        var lines = t.split('\n');
        for (var i = 0; i < lines.length; i++) lines[i] = lines[i].replace(/[ \t\f\v]+$/, '').replace(/^[ \t\f\v]+/, '');
        return lines.join('\n').replace(/^\n+|\n+$/g, '');
    }

    function elementText(el) {
        if (!isDisplayed(el)) return '';
        var t = el.innerText;
        if (typeof t !== 'string') t = el.textContent || '';
        return normalizeText(t);
    }

    var INPUT_ROLES = {
        button: 'button', submit: 'button', reset: 'button', image: 'button',
        checkbox: 'checkbox', radio: 'radio', range: 'slider', number: 'spinbutton',
        search: 'searchbox', email: 'textbox', tel: 'textbox', text: 'textbox',
        url: 'textbox', password: 'textbox'
    };
    var TAG_ROLES = {
        article: 'article', aside: 'complementary', blockquote: 'blockquote', button: 'button',
        caption: 'caption', code: 'code', dd: 'definition', del: 'deletion', details: 'group',
        dfn: 'term', dialog: 'dialog', dt: 'term', em: 'emphasis', fieldset: 'group',
        figure: 'figure', form: 'form', h1: 'heading', h2: 'heading', h3: 'heading',
        h4: 'heading', h5: 'heading', h6: 'heading', hr: 'separator', ins: 'insertion',
        li: 'listitem', main: 'main', math: 'math', menu: 'list', meter: 'meter',
        nav: 'navigation', ol: 'list', optgroup: 'group', option: 'option', output: 'status',
        p: 'paragraph', progress: 'progressbar', search: 'search', strong: 'strong',
        sub: 'subscript', sup: 'superscript', table: 'table', tbody: 'rowgroup',
        td: 'cell', textarea: 'textbox', tfoot: 'rowgroup', th: 'columnheader',
        thead: 'rowgroup', time: 'time', tr: 'row', ul: 'list', div: 'generic',
        span: 'generic', b: 'generic', i: 'generic', u: 'generic', small: 'generic',
        address: 'group', hgroup: 'group', html: 'document', body: 'generic'
    };

    function computedRole(el) {
        var explicit = el.getAttribute('role');
        if (explicit && explicit.trim()) return explicit.trim().split(/\s+/)[0];
        var name = localName(el);
        if (name === 'a' || name === 'area') return el.hasAttribute('href') ? 'link' : 'generic';
        if (name === 'img') return el.getAttribute('alt') === '' ? 'presentation' : 'img';
        if (name === 'select')
            return el.multiple || (el.size || 0) > 1 ? 'listbox' : 'combobox';
        if (name === 'input') {
            var type = lower(el.type || 'text');
            if (el.hasAttribute('list') && /^(text|search|email|tel|url)$/.test(type)) return 'combobox';
            return INPUT_ROLES[type] || '';
        }
        if (name === 'header' || name === 'footer') {
            for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode)
                if (/^(article|aside|main|nav|section)$/.test(localName(p))) return 'generic';
            return name === 'header' ? 'banner' : 'contentinfo';
        }
        if (name === 'section')
            return el.hasAttribute('aria-label') || el.hasAttribute('aria-labelledby') ? 'region' : 'generic';
        return TAG_ROLES[name] || '';
    }

    function textOfIds(el, list) {
        var doc = el.ownerDocument, parts = [];
        String(list).trim().split(/\s+/).forEach(function (id) {
            var t = id ? doc.getElementById(id) : null;
            if (t) parts.push(normalizeText(t.textContent || ''));
        });
        return parts.join(' ').trim();
    }

    function computedLabel(el) {
        var v = el.getAttribute('aria-labelledby');
        if (v) { var t = textOfIds(el, v); if (t) return t; }
        v = el.getAttribute('aria-label');
        if (v && v.trim()) return v.trim();
        var name = localName(el);
        if (name === 'input' || name === 'select' || name === 'textarea' || name === 'meter' ||
            name === 'progress' || name === 'output') {
            var labels = el.labels, texts = [];
            if (labels) for (var i = 0; i < labels.length; i++) texts.push(normalizeText(labels[i].textContent || ''));
            if (texts.length) return texts.join(' ').trim();
            if (name === 'input') {
                var type = lower(el.type || 'text');
                if (type === 'button' || type === 'submit' || type === 'reset')
                    return el.value || (type === 'submit' ? 'Submit' : type === 'reset' ? 'Reset' : '');
                if (type === 'image') return el.getAttribute('alt') || el.getAttribute('title') || '';
            }
        }
        if (name === 'img' || name === 'area') {
            v = el.getAttribute('alt');
            if (v) return v.trim();
        }
        if (name === 'fieldset') {
            var lg = el.querySelector('legend');
            if (lg) return normalizeText(lg.textContent || '');
        }
        if (name === 'table') {
            var cap = el.querySelector('caption');
            if (cap) return normalizeText(cap.textContent || '');
        }
        var role = computedRole(el);
        if (/^(button|link|heading|option|cell|columnheader|row|listitem|checkbox|radio|tab|menuitem|treeitem|switch)$/.test(role)) {
            var txt = normalizeText(el.textContent || '').replace(/\s+/g, ' ').trim();
            if (txt) return txt;
        }
        v = el.getAttribute('title');
        if (v && v.trim()) return v.trim();
        v = el.getAttribute('placeholder');
        if (v && v.trim()) return v.trim();
        return '';
    }

    /* ---- geometry and interactability ---- */

    function firstRect(el) {
        var rects;
        try { rects = el.getClientRects(); } catch (e) { rects = null; }
        if (rects && rects.length) {
            for (var i = 0; i < rects.length; i++)
                if (rects[i].width > 0 || rects[i].height > 0) return rects[i];
            return rects[0];
        }
        return null;
    }

    function inViewCenter(el, c) {
        var r = firstRect(el);
        if (!r) return null;
        var vp = viewportSize(c);
        var left = mathMax(0, mathMin(r.left, r.right));
        var right = mathMin(vp.w, mathMax(r.left, r.right));
        var top = mathMax(0, mathMin(r.top, r.bottom));
        var bottom = mathMin(vp.h, mathMax(r.top, r.bottom));
        if (!(right > left) || !(bottom > top)) return null;
        return { x: mathFloor((left + right) / 2), y: mathFloor((top + bottom) / 2) };
    }

    function fullyInView(el, c) {
        var r = firstRect(el);
        if (!r) return false;
        var vp = viewportSize(c);
        return r.left >= 0 && r.top >= 0 && r.right <= vp.w && r.bottom <= vp.h;
    }

    function isScrollContainer(n) {
        var cs = style(n);
        if (!cs || !/(auto|scroll|hidden)/.test(String(cs.overflowX) + String(cs.overflowY) + String(cs.overflow)))
            return false;
        return n.scrollHeight > n.clientHeight || n.scrollWidth > n.clientWidth;
    }

    /* Scrolls the element into view, block "end" and inline "nearest" as
     * WebDriver asks: first its scrolling ancestors, then the viewport.
     * Done here with scrollTop and scrollTo, which apply at once, since
     * the engine's scrollIntoView() lands only at the next frame. */
    function scrollIntoView(el, c) {
        if (fullyInView(el, c)) return;
        var r, de = c.doc.documentElement, body = c.doc.body;
        for (var p = composedParent(el); p && p !== de && p !== body; p = composedParent(p)) {
            if (!isScrollContainer(p)) continue;
            r = firstRect(el);
            if (!r) return;
            var pr = p.getBoundingClientRect();
            if (r.bottom > pr.bottom || r.top < pr.top)
                p.scrollTop += r.height > pr.height ? r.top - pr.top : r.bottom - pr.bottom;
            if (r.right > pr.right) p.scrollLeft += r.right - pr.right;
            else if (r.left < pr.left) p.scrollLeft -= pr.left - r.left;
        }
        r = firstRect(el);
        if (!r || !c.win || typeof c.win.scrollTo !== 'function') return;
        var vp = viewportSize(c), pos = scrollPos(c), x = pos.x, y = pos.y;
        if (r.bottom > vp.h || r.top < 0)
            y = pos.y + (r.height > vp.h ? r.top : r.bottom - vp.h);
        if (r.right > vp.w) x = pos.x + (r.width > vp.w ? r.left : r.right - vp.w);
        else if (r.left < 0) x = pos.x + r.left;
        if (x !== pos.x || y !== pos.y) c.win.scrollTo(mathMax(0, x), mathMax(0, y));
    }

    function optionContainer(el) {
        var name = localName(el);
        if (name !== 'option' && name !== 'optgroup') return null;
        for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode) {
            var pn = localName(p);
            if (pn === 'select' || pn === 'datalist') return p;
        }
        return null;
    }

    function describe(el) {
        if (!el || el.nodeType !== 1) return String(el);
        var s = '<' + localName(el);
        if (el.id) s += ' id="' + el.id + '"';
        var cls = el.getAttribute('class');
        if (cls) s += ' class="' + cls + '"';
        return s + '>';
    }

    function hitAt(doc, x, y) {
        var hit = null;
        try { hit = doc.elementFromPoint(x, y); } catch (e) { hit = null; }
        for (var guard = 0; hit && hit.shadowRoot && guard < 32; guard++) {
            var inner = null;
            try { inner = hit.shadowRoot.elementFromPoint ? hit.shadowRoot.elementFromPoint(x, y) : null; }
            catch (e) { inner = null; }
            if (!inner || inner === hit) break;
            hit = inner;
        }
        return hit;
    }

    function fire(target, type, init) {
        var ev, opts = { bubbles: true, cancelable: true, composed: true, view: target.ownerDocument.defaultView || null };
        for (var k in init || {}) opts[k] = init[k];
        try {
            ev = /^(mouse|click|dblclick|contextmenu)/.test(type) && G.MouseEvent
                ? new G.MouseEvent(type, opts) : new G.Event(type, opts);
        } catch (e) {
            ev = new G.Event(type, { bubbles: true, cancelable: true });
        }
        return target.dispatchEvent(ev);
    }

    /* Element Click on an <option>: the WebDriver steps, run without a pointer. */
    function clickOption(opt, container) {
        fire(container, 'mouseover');
        fire(container, 'mousemove');
        fire(container, 'mousedown');
        try { container.focus(); } catch (e) { /* ignore */ }
        if (isEnabled(opt)) {
            var before = !!opt.selected;
            if (localName(container) === 'select' && container.multiple) opt.selected = !before;
            else opt.selected = true;
            fire(container, 'input', { cancelable: false });
            if (!before || container.multiple) fire(container, 'change', { cancelable: false });
        }
        fire(container, 'mouseup');
        fire(container, 'click');
    }

    /* Scrolls an element into view and returns the top-level viewport point
     * to press, after the WebDriver interactability and obscuring checks. */
    function clickPoint(el, c) {
        if (localName(el) === 'input' && lower(el.type || '') === 'file')
            fail('invalid argument', 'Cannot click a file input; send keys to it instead');
        var container = optionContainer(el) || el;
        scrollIntoView(container, c);
        var center = inViewCenter(container, c);
        if (!center) fail('element not interactable', describe(el) + ' has no size or is out of the viewport');
        var hit = hitAt(c.doc, center.x, center.y);
        if (!hit)
            fail('element not interactable', describe(el) + ' could not be hit at (' + center.x + ', ' + center.y + ')');
        if (hit !== container && !composedContains(container, hit) &&
            !(hit.shadowRoot && composedContains(hit, container))) {
            var lbl = localName(hit) === 'label' && hit.control === container;
            if (!lbl)
                fail('element click intercepted', describe(el) + ' is not clickable at point (' +
                     center.x + ', ' + center.y + '). Other element would receive the click: ' + describe(hit));
        }
        if (container !== el) {
            clickOption(el, container);
            return { done: true };
        }
        var off = frameOffset(c);
        return { x: center.x + off.x, y: center.y + off.y };
    }

    var TEXT_INPUT_TYPES = {
        text: 1, search: 1, url: 1, tel: 1, email: 1, password: 1, date: 1, month: 1,
        week: 1, time: 1, 'datetime-local': 1, number: 1, range: 1, color: 1, file: 1
    };

    function isContentEditable(el) {
        try { if (el.isContentEditable) return true; } catch (e) { /* ignore */ }
        var v = el.getAttribute('contenteditable');
        return v !== null && lower(v) !== 'false';
    }

    function isEditable(el) {
        var name = localName(el);
        if (name === 'input') {
            var type = lower(el.type || el.getAttribute('type') || 'text');
            if (!TEXT_INPUT_TYPES[type]) return false;
            return !el.disabled && !el.readOnly && isEnabled(el);
        }
        if (name === 'textarea') return !el.disabled && !el.readOnly && isEnabled(el);
        return isContentEditable(el);
    }

    function clearElement(el, c) {
        if (!isEditable(el)) fail('invalid element state', describe(el) + ' is not editable');
        scrollIntoView(el, c);
        if (!inViewCenter(el, c)) fail('element not interactable', describe(el) + ' is not in view');
        var name = localName(el);
        if (name !== 'input' && name !== 'textarea') {
            if (el.innerHTML === '') return null;
            try { el.focus(); } catch (e) { /* ignore */ }
            el.innerHTML = '';
            try { el.blur(); } catch (e) { /* ignore */ }
            return null;
        }
        var hasFiles = name === 'input' && el.files && el.files.length;
        if (el.value === '' && !hasFiles) return null;
        try { el.focus(); } catch (e) { /* ignore */ }
        el.value = '';
        fire(el, 'input', { cancelable: false });
        fire(el, 'change', { cancelable: false });
        try { el.blur(); } catch (e) { /* ignore */ }
        return null;
    }

    function isFocusable(el) {
        var name = localName(el);
        if (name === 'body') return true;
        if (el.hasAttribute('tabindex')) return true;
        if (isContentEditable(el)) return true;
        if (/^(input|select|textarea|button|iframe|summary)$/.test(name)) return !el.disabled;
        if ((name === 'a' || name === 'area') && el.hasAttribute('href')) return true;
        return typeof el.tabIndex === 'number' && el.tabIndex >= 0;
    }

    /* Prepares Element Send Keys: file inputs report their point for the
     * file drop; everything else is scrolled to and focused. */
    function keysPrepare(el, c, strictFile) {
        var name = localName(el);
        if (name === 'input' && lower(el.type || '') === 'file') {
            if (strictFile) {
                scrollIntoView(el, c);
                if (!inViewCenter(el, c)) fail('element not interactable', describe(el) + ' is not in view');
            }
            var r = el.getBoundingClientRect(), off = frameOffset(c);
            return { file: true, multiple: !!el.multiple,
                     x: r.left + r.width / 2 + off.x, y: r.top + r.height / 2 + off.y };
        }
        scrollIntoView(el, c);
        if (!isFocusable(el)) fail('element not interactable', describe(el) + ' is not reachable by keyboard');
        if (c.doc.activeElement !== el) {
            try { el.focus(); } catch (e) { /* ignore */ }
        }
        if (c.doc.activeElement !== el && name !== 'body')
            fail('element not interactable', describe(el) + ' could not be focused');
        return { file: false };
    }

    /* ---- locators ---- */

    function onlyOwn(list, doc) {
        var out = [];
        for (var i = 0; list && i < list.length; i++) {
            var n = list[i];
            if (isElement(n) && n.ownerDocument === doc) out.push(n);
        }
        return out;
    }

    function locate(root, doc, using, value, c) {
        var list;
        switch (using) {
        case 'css selector':
            try { list = root.querySelectorAll(value); }
            catch (e) { fail('invalid selector', 'Invalid CSS selector: ' + value); }
            return onlyOwn(list, doc);
        case 'tag name':
            try {
                list = typeof root.getElementsByTagName === 'function'
                    ? root.getElementsByTagName(value) : root.querySelectorAll(value);
            } catch (e) { fail('invalid selector', 'Invalid tag name: ' + value); }
            return onlyOwn(list, doc);
        case 'link text':
        case 'partial link text': {
            var anchors = onlyOwn(root.querySelectorAll('a'), doc), out = [];
            for (var i = 0; i < anchors.length; i++) {
                var text = elementText(anchors[i]);
                if (using === 'link text' ? text.trim() === value : text.indexOf(value) >= 0)
                    out.push(anchors[i]);
            }
            return out;
        }
        case 'xpath': {
            var res, found = [];
            try {
                res = doc.evaluate(value, root, null, 7, null);
            } catch (e) {
                fail('invalid selector', 'Invalid XPath expression: ' + value);
            }
            for (var k = 0; k < res.snapshotLength; k++) {
                var n = res.snapshotItem(k);
                if (!isElement(n))
                    fail('invalid selector', 'The XPath expression ' + value + ' selects a non-element node');
                found.push(n);
            }
            return onlyOwn(found, doc);
        }
        }
        fail('invalid argument', 'Unknown location strategy: ' + using);
    }

    function find(req, c) {
        var root = c.doc;
        var from = req.from;
        if (from && call(hasOwn, from, [ELEMENT])) root = known(from[ELEMENT], c);
        else if (from && call(hasOwn, from, [SHADOW])) root = knownShadow(from[SHADOW], c);
        var found = locate(root, c.doc, req.using, String(req.value), c);
        var refs = [];
        for (var i = 0; i < found.length; i++) {
            var r = objCreate(null);
            r[ELEMENT] = idFor(found[i]);
            refs.push(r);
            if (!req.multiple) break;
        }
        return refs;
    }

    /* ---- script execution ---- */

    function isThenable(v) {
        try { return v !== null && (typeof v === 'object' || typeof v === 'function') && typeof v.then === 'function'; }
        catch (e) { return false; }
    }

    /* Calls the script body as a function in the current browsing
     * context's realm: the frame's own realm when it has one, else the
     * page's realm with window and document bound to the frame's. */
    function invoke(script, args, c) {
        var body = String(script);
        if (!c.frames.length) {
            var fn = natives.compile('(function(){\n' + body + '\n})');
            return call(fn, G, args);
        }
        var fe = c.frames[c.frames.length - 1];
        var boxed = natives.frameCall(fe,
            '(function(a){ return { v: (function(){\n' + body + '\n}).apply(null, a) }; })', args);
        if (boxed && typeof boxed === 'object') return boxed.v;
        var parentWin = c.frames.length > 1
            ? natives.frameWindow(c.frames[c.frames.length - 2]) : G;
        var maker = natives.compile(
            '(function(window, document, self, frames, parent, top){ return function(){\n' +
            body + '\n}; })');
        var inner = maker(c.win, c.doc, c.win, c.win, parentWin, G);
        return call(inner, c.win || G, args);
    }

    function settle(state, c) {
        if (!state.ok) return errorOf(state.error);
        var o = objCreate(null);
        o.value = serialize(state.value, c, []);
        return o;
    }

    function execute(req, c) {
        var args = deserialize(req.args || [], c, 0);
        if (!isArray(args)) args = [args];
        var state = { done: false, ok: true, value: undefined, error: null, ctx: req.ctx || [] };
        function resolve(v) {
            if (state.done) return;
            if (isThenable(v)) { adopt(v); return; }
            state.done = true;
            state.value = v;
        }
        function reject(e) {
            if (state.done) return;
            state.done = true;
            state.ok = false;
            state.error = e;
        }
        function adopt(p) {
            try { p.then(resolve, reject); } catch (e) { reject(e); }
        }
        if (req.async) args.push(function (v) { resolve(v); });
        var ret;
        try {
            ret = invoke(req.script, args, c);
        } catch (e) {
            return { __raw: errorOf(e) };
        }
        if (isThenable(ret)) adopt(ret);
        else if (!req.async) resolve(ret);
        if (state.done) return { __raw: settle(state, c) };
        var token = uuid();
        call(mapSet, pending, [token, state]);
        return result({ pending: token });
    }

    function poll(req) {
        var state = call(mapGet, pending, [req.token]);
        if (!state)
            fail('javascript error', 'The document unloaded while waiting for the script result');
        if (!state.done) return result({ pending: req.token });
        call(mapDelete, pending, [req.token]);
        return { __raw: settle(state, context(state.ctx)) };
    }

    /* ---- commands ---- */

    var commands = {
        find: function (req, c) {
            var refs = find(req, c);
            if (!req.multiple) {
                if (!refs.length)
                    fail('no such element', 'Unable to locate element: {"method":"' + req.using +
                         '","selector":' + stringify(String(req.value)) + '}');
                return refs[0];
            }
            return refs;
        },
        active: function (req, c) {
            var a = c.doc.activeElement || c.doc.body;
            if (!a) fail('no such element', 'There is no active element');
            return serialize(a, c, []);
        },
        shadow: function (req, c) {
            var el = known(req.id, c), sr = el.shadowRoot;
            if (!sr) fail('no such shadow root', describe(el) + ' has no shadow root');
            var r = objCreate(null);
            r[SHADOW] = idFor(sr);
            return r;
        },
        selected: function (req, c) { return isSelected(known(req.id, c)); },
        attribute: function (req, c) { return getAttribute(known(req.id, c), req.name); },
        property: function (req, c) {
            var el = known(req.id, c), v;
            try { v = el[req.name]; } catch (e) { v = undefined; }
            return v === undefined ? null : serialize(v, c, []);
        },
        css: function (req, c) {
            var el = known(req.id, c);
            if (isXml(el.ownerDocument)) return '';
            var cs = style(el);
            return cs ? String(cs.getPropertyValue(String(req.name)) || '') : '';
        },
        text: function (req, c) { return elementText(known(req.id, c)); },
        name: function (req, c) { return localName(known(req.id, c)); },
        rect: function (req, c) {
            var el = known(req.id, c), r = el.getBoundingClientRect(), s = scrollPos(c);
            return { x: r.left + s.x, y: r.top + s.y, width: r.width, height: r.height };
        },
        enabled: function (req, c) { return isEnabled(known(req.id, c)); },
        displayed: function (req, c) { return isDisplayed(known(req.id, c)); },
        role: function (req, c) { return computedRole(known(req.id, c)); },
        label: function (req, c) { return computedLabel(known(req.id, c)); },
        clickPoint: function (req, c) { return clickPoint(known(req.id, c), c); },
        clear: function (req, c) { return clearElement(known(req.id, c), c); },
        keysPrepare: function (req, c) { return keysPrepare(known(req.id, c), c, !!req.strictFile); },
        /* In-view center point of an element in top-level viewport
         * coordinates, for pointer actions; not scrolled. */
        center: function (req, c) {
            var el = known(req.id, c), p = inViewCenter(el, c), off = frameOffset(c);
            if (!p) {
                var r = firstRect(el);
                if (!r) fail('move target out of bounds', describe(el) + ' has no box');
                p = { x: mathFloor(r.left + r.width / 2), y: mathFloor(r.top + r.height / 2) };
            }
            return { x: p.x + off.x, y: p.y + off.y };
        },
        /* Scrolls an element into view and returns its box in top-level
         * viewport coordinates, for an element screenshot. */
        box: function (req, c) {
            var el = known(req.id, c);
            scrollIntoView(el, c);
            var r = el.getBoundingClientRect(), off = frameOffset(c);
            return { x: r.left + off.x, y: r.top + off.y, width: r.width, height: r.height };
        },
        execute: function (req, c) { return execute(req, c); },
        poll: function (req) { return poll(req); },
        cancel: function (req) { call(mapDelete, pending, [req.token]); return null; },
        source: function (req, c) {
            var de = c.doc.documentElement;
            if (!de) return '';
            if (typeof de.outerHTML === 'string') return de.outerHTML;
            try { return new G.XMLSerializer().serializeToString(c.doc); } catch (e) { return ''; }
        },
        frame: function (req, c) {
            var fe;
            if (typeof req.id === 'number') {
                var list = onlyOwn(c.doc.querySelectorAll('iframe'), c.doc);
                fe = list[req.id];
                if (!fe) fail('no such frame', 'There is no frame at index ' + req.id);
            } else {
                fe = known(req.id && req.id[ELEMENT], c);
                if (localName(fe) !== 'iframe') fail('no such frame', describe(fe) + ' is not a frame');
            }
            if (!natives.frameDocument(fe)) fail('no such frame', 'The frame has no document');
            return idFor(fe);
        },
        /* Readiness, address and title of the current browsing context. */
        info: function (req, c) {
            var d = c.doc;
            return { readyState: String(d.readyState || 'complete'),
                     url: String((d.location && d.location.href) || d.URL || ''),
                     title: String(d.title || '') };
        },
        /* Fires dblclick at a top-level viewport point after a second
         * click there: the engine's own pointer path has no double click. */
        dblclick: function (req) {
            var target = hitAt(G.document, req.x, req.y);
            if (!target) return false;
            fire(target, 'dblclick', { clientX: req.x, clientY: req.y, detail: 2, button: 0,
                                      shiftKey: !!(req.mods & 1), ctrlKey: !!(req.mods & 2),
                                      altKey: !!(req.mods & 4), metaKey: !!(req.mods & 8) });
            return true;
        },
        /* Dispatches a wheel event at a top-level viewport point; reports
         * whether the page cancelled it. */
        wheel: function (req) {
            var doc = G.document, target = hitAt(doc, req.x, req.y) || doc.body || doc.documentElement;
            if (!target) return { prevented: false };
            var init = { bubbles: true, cancelable: true, composed: true, view: G,
                         clientX: req.x, clientY: req.y, deltaX: req.dx, deltaY: req.dy,
                         deltaZ: 0, deltaMode: 0, shiftKey: !!(req.mods & 1),
                         ctrlKey: !!(req.mods & 2), altKey: !!(req.mods & 4),
                         metaKey: !!(req.mods & 8) };
            var ev;
            try { ev = G.WheelEvent ? new G.WheelEvent('wheel', init) : new G.Event('wheel', init); }
            catch (e) { ev = new G.Event('wheel', { bubbles: true, cancelable: true }); }
            return { prevented: !target.dispatchEvent(ev) };
        }
    };

    function run(req) {
        try {
            if (typeof req.handle === 'string') handle = req.handle;
            var cmd = commands[req.cmd];
            if (!cmd) fail('unknown command', 'Unknown WebDriver atom ' + req.cmd);
            var c = req.cmd === 'poll' || req.cmd === 'cancel' ? null : context(req.ctx || []);
            curCtx = c;
            var v = cmd(req, c);
            if (v && typeof v === 'object' && v.__raw) return stringify(v.__raw);
            var o = objCreate(null);
            o.value = v === undefined ? null : v;
            return stringify(o);
        } catch (e) {
            return stringify(errorOf(e));
        }
    }

    return { run: run };
})
