#!/usr/bin/env python3
"""End-to-end test of Northstar's W3C WebDriver server (northstar --webdriver).

Serves a few fixture pages from a local HTTP server, starts the browser on a
free port and drives it over the WebDriver protocol with nothing but the
standard library, so it runs in CI without Selenium.

Usage: scripts/webdriver-test.py [path/to/northstar]
"""

import base64
import http.server
import json
import os
import re
import socketserver
import subprocess
import sys
import threading
import time
import traceback
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "builddir", "src", "gtk", "northstar")
ELEMENT = "element-6066-11e4-a52e-4f735466cecf"

PAGES = {
    "/index.html": """<!doctype html>
<html><head><title>WebDriver fixture</title></head>
<body>
<h1 id="heading">Hello WebDriver</h1>
<p id="para" class="intro" data-x="42">Some <b>bold</b> text.</p>
<p id="hidden" style="display:none">secret</p>
<a id="link" href="/second.html">Go to second</a>
<button id="btn" onclick="document.getElementById('heading').textContent='Clicked'">Click me</button>
<input id="name" type="text" value="">
<input id="check" type="checkbox">
<input id="dis" type="text" disabled>
<select id="sel"><option value="a">Alpha</option><option value="b">Beta</option></select>
<form id="form" action="/second.html" method="get"><input id="q" name="q"></form>
<div id="log"></div>
<ul><li class="item">one</li><li class="item">two</li><li class="item">three</li></ul>
<div id="host"></div>
<script>
  var log = document.getElementById('log');
  var name = document.getElementById('name');
  name.addEventListener('keydown', function (e) { log.textContent += 'd' + e.key; });
  name.addEventListener('input', function () { log.setAttribute('data-input', name.value); });
  document.getElementById('host').attachShadow({mode: 'open'}).innerHTML =
    '<span id="inner">shadow text</span>';
  document.cookie = 'fromPage=1; path=/';
</script>
</body></html>
""",
    "/second.html": """<!doctype html>
<html><head><title>Second page</title></head>
<body><p id="where">second</p><a id="back" href="/index.html">back</a></body></html>
""",
    "/frames.html": """<!doctype html>
<html><head><title>Frames</title></head>
<body><p id="outer">outer</p>
<iframe id="frm" name="frm" src="/inner.html"></iframe></body></html>
""",
    "/inner.html": """<!doctype html>
<html><head><title>Inner</title></head>
<body><p id="innerp">inside the frame</p>
<script>window.frameMarker = 'frame-realm';</script></body></html>
""",
    "/tall.html": """<!doctype html>
<html><head><title>Tall</title></head>
<body style="margin:0"><div style="height:3000px">top</div>
<button id="far" onclick="this.textContent='far clicked'">far</button>
<div id="clicks"></div>
<script>
document.addEventListener('mousedown', function (e) {
  document.getElementById('clicks').textContent += 'down@' + e.clientX + ',' + e.clientY + ';';
});
</script></body></html>
""",
}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        body = PAGES.get(path)
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class WebDriverError(Exception):
    def __init__(self, status, error, message):
        super().__init__("%s (%d): %s" % (error, status, message))
        self.status = status
        self.error = error


class Client:
    def __init__(self, base):
        self.base = base
        self.sid = None

    def call(self, method, path, body=None):
        url = self.base + path
        data = None
        headers = {}
        if method == "POST":
            data = json.dumps(body if body is not None else {}).encode("utf-8")
            headers["Content-Type"] = "application/json; charset=utf-8"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
                return payload.get("value")
        except urllib.error.HTTPError as e:
            payload = json.loads(e.read().decode("utf-8"))
            v = payload.get("value", {})
            raise WebDriverError(e.code, v.get("error"), v.get("message"))

    def s(self, method, path, body=None):
        return self.call(method, "/session/%s%s" % (self.sid, path), body)

    def find(self, using, value, many=False, root=None):
        base = "/element/%s" % root if root else ""
        return self.s("POST", base + ("/elements" if many else "/element"),
                      {"using": using, "value": value})

    def css(self, value):
        return self.find("css selector", value)[ELEMENT]

    def run(self, script, *args):
        return self.s("POST", "/execute/sync", {"script": script, "args": list(args)})


def expect_error(code, fn, *args):
    try:
        fn(*args)
    except WebDriverError as e:
        if e.error != code:
            raise AssertionError("expected %s, got %s" % (code, e))
        return
    raise AssertionError("expected %s, got success" % code)


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("NS_BIN", DEFAULT_BIN)
    httpd = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    site = "http://127.0.0.1:%d" % httpd.server_address[1]

    env = dict(os.environ)
    env.setdefault("NS_ALLOW_ROOT", "1")
    proc = subprocess.Popen([binary, "--webdriver=0", "--viewport=1000x700"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env, text=True)
    port = None
    deadline = time.time() + 30
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        m = re.search(r"listening on http://127\.0\.0\.1:(\d+)", line)
        if m:
            port = int(m.group(1))
            break
    if not port:
        proc.kill()
        print("webdriver-test: the server did not start")
        print(proc.stderr.read())
        return 1

    wd = Client("http://127.0.0.1:%d" % port)
    results = []

    def test(name):
        def deco(fn):
            try:
                fn()
                results.append((name, None))
                print("ok   %s" % name)
            except Exception as e:  # noqa: BLE001 - report every failure
                results.append((name, e))
                print("FAIL %s: %s" % (name, e))
                traceback.print_exc()
            return fn
        return deco

    try:
        @test("status before a session")
        def _():
            st = wd.call("GET", "/status")
            check(st["ready"] is True, "not ready: %r" % st)

        @test("capability mismatch is refused")
        def _():
            expect_error("session not created", wd.call, "POST", "/session",
                         {"capabilities": {"alwaysMatch": {"browserName": "firefox"}}})

        @test("new session")
        def _():
            v = wd.call("POST", "/session", {"capabilities": {
                "alwaysMatch": {"browserName": "northstar"},
                "firstMatch": [{}]}})
            wd.sid = v["sessionId"]
            check(v["capabilities"]["browserName"] == "northstar", "browserName")
            check(v["capabilities"]["setWindowRect"] is True, "setWindowRect")

        @test("unknown session id")
        def _():
            expect_error("invalid session id", wd.call, "GET", "/session/nope/url")

        @test("unknown command")
        def _():
            expect_error("unknown command", wd.call, "GET", "/session/%s/nonsense" % wd.sid)

        @test("timeouts round trip")
        def _():
            wd.s("POST", "/timeouts", {"implicit": 0, "script": 5000})
            t = wd.s("GET", "/timeouts")
            check(t == {"script": 5000, "pageLoad": 300000, "implicit": 0}, repr(t))
            expect_error("invalid argument", wd.s, "POST", "/timeouts", {"implicit": -1})

        @test("navigate, url and title")
        def _():
            wd.s("POST", "/url", {"url": site + "/index.html"})
            check(wd.s("GET", "/url") == site + "/index.html", "url")
            check(wd.s("GET", "/title") == "WebDriver fixture", "title")
            expect_error("invalid argument", wd.s, "POST", "/url", {"url": "not a url"})

        @test("find elements by every strategy")
        def _():
            check(ELEMENT in wd.find("css selector", "#heading"), "css")
            check(ELEMENT in wd.find("xpath", "//h1"), "xpath")
            check(ELEMENT in wd.find("link text", "Go to second"), "link text")
            check(ELEMENT in wd.find("partial link text", "second"), "partial link text")
            check(ELEMENT in wd.find("tag name", "h1"), "tag name")
            items = wd.find("css selector", "li.item", many=True)
            check(len(items) == 3, "3 items, got %d" % len(items))
            expect_error("no such element", wd.find, "css selector", "#missing")
            expect_error("invalid selector", wd.find, "css selector", "[[[")
            expect_error("invalid selector", wd.find, "xpath", "//*[")
            check(wd.find("css selector", "#missing", many=True) == [], "empty list")

        @test("find from an element")
        def _():
            ul = wd.find("tag name", "ul")[ELEMENT]
            lis = wd.find("tag name", "li", many=True, root=ul)
            check(len(lis) == 3, "children")

        @test("element state and properties")
        def _():
            h = wd.css("#heading")
            check(wd.s("GET", "/element/%s/text" % h) == "Hello WebDriver", "text")
            check(wd.s("GET", "/element/%s/name" % h) == "h1", "tag name")
            p = wd.css("#para")
            check(wd.s("GET", "/element/%s/text" % p) == "Some bold text.", "nested text")
            check(wd.s("GET", "/element/%s/attribute/data-x" % p) == "42", "attribute")
            check(wd.s("GET", "/element/%s/attribute/nope" % p) is None, "missing attribute")
            check(wd.s("GET", "/element/%s/property/className" % p) == "intro", "property")
            hid = wd.css("#hidden")
            check(wd.s("GET", "/element/%s/displayed" % hid) is False, "hidden not displayed")
            check(wd.s("GET", "/element/%s/text" % hid) == "", "hidden text")
            check(wd.s("GET", "/element/%s/displayed" % h) is True, "heading displayed")
            check(wd.s("GET", "/element/%s/enabled" % wd.css("#dis")) is False, "disabled")
            check(wd.s("GET", "/element/%s/enabled" % wd.css("#name")) is True, "enabled")
            r = wd.s("GET", "/element/%s/rect" % h)
            check(r["width"] > 0 and r["height"] > 0, "rect %r" % r)
            disp = wd.s("GET", "/element/%s/css/display" % h)
            check(disp == "block", "css display %r" % disp)
            check(wd.s("GET", "/element/%s/computedrole" % h) == "heading", "role")

        @test("click a button")
        def _():
            wd.s("POST", "/element/%s/click" % wd.css("#btn"), {})
            check(wd.s("GET", "/element/%s/text" % wd.css("#heading")) == "Clicked", "clicked")

        @test("click a checkbox and an option")
        def _():
            cb = wd.css("#check")
            wd.s("POST", "/element/%s/click" % cb, {})
            check(wd.s("GET", "/element/%s/selected" % cb) is True, "checkbox selected")
            opt = wd.find("css selector", "#sel option[value=b]")[ELEMENT]
            wd.s("POST", "/element/%s/click" % opt, {})
            check(wd.s("GET", "/element/%s/selected" % opt) is True, "option selected")
            check(wd.run("return document.getElementById('sel').value") == "b", "select value")

        @test("send keys and clear")
        def _():
            el = wd.css("#name")
            wd.s("POST", "/element/%s/value" % el, {"text": "abc"})
            check(wd.s("GET", "/element/%s/property/value" % el) == "abc", "typed value")
            wd.s("POST", "/element/%s/value" % el, {"text": "D"})
            check(wd.s("GET", "/element/%s/property/value" % el) == "abc", "backspace")
            log = wd.run("return document.getElementById('log').textContent")
            check(log.startswith("dadbdc"), "keydown events %r" % log)
            wd.s("POST", "/element/%s/clear" % el, {})
            check(wd.s("GET", "/element/%s/property/value" % el) == "", "cleared")
            expect_error("invalid element state", wd.s, "POST",
                         "/element/%s/clear" % wd.css("#heading"), {})

        @test("execute script values and arguments")
        def _():
            check(wd.run("return 1 + 2") == 3, "number")
            check(wd.run("return [1, 'a', null, true]") == [1, "a", None, True], "array")
            check(wd.run("return {a: {b: 2}}") == {"a": {"b": 2}}, "object")
            check(wd.run("return arguments[0] + arguments[1]", 2, 3) == 5, "arguments")
            el = wd.run("return document.getElementById('para')")
            check(ELEMENT in el, "element result %r" % el)
            check(wd.run("return arguments[0].id", el) == "para", "element argument")
            check(len(wd.run("return document.querySelectorAll('li')")) == 3, "NodeList")
            check(wd.run("return Promise.resolve(7)") == 7, "promise")
            check(wd.run("return undefined") is None, "undefined")
            expect_error("javascript error", wd.run, "throw new Error('boom')")
            expect_error("javascript error", wd.run, "var o = {}; o.o = o; return o")

        @test("execute async script")
        def _():
            v = wd.s("POST", "/execute/async", {
                "script": "var cb = arguments[arguments.length - 1];"
                          "setTimeout(function () { cb(arguments.length + 40); }, 50);",
                "args": []})
            check(v == 40, "async value %r" % v)
            wd.s("POST", "/timeouts", {"script": 300})
            expect_error("script timeout", wd.s, "POST", "/execute/async",
                         {"script": "/* never calls back */", "args": []})
            wd.s("POST", "/timeouts", {"script": 30000})

        @test("shadow roots")
        def _():
            host = wd.css("#host")
            sr = wd.s("GET", "/element/%s/shadow" % host)
            sid = sr["shadow-6066-11e4-a52e-4f735466cecf"]
            inner = wd.s("POST", "/shadow/%s/element" % sid,
                         {"using": "css selector", "value": "#inner"})
            check(wd.s("GET", "/element/%s/text" % inner[ELEMENT]) == "shadow text", "shadow text")
            expect_error("no such shadow root", wd.s, "GET", "/element/%s/shadow" % wd.css("#para"))

        @test("cookies")
        def _():
            names = [c["name"] for c in wd.s("GET", "/cookie")]
            check("fromPage" in names, "page cookie in %r" % names)
            wd.s("POST", "/cookie", {"cookie": {"name": "wd", "value": "yes", "path": "/"}})
            c = wd.s("GET", "/cookie/wd")
            check(c["value"] == "yes" and c["path"] == "/", "cookie %r" % c)
            check("wd=yes" in wd.run("return document.cookie"), "document.cookie")
            wd.s("POST", "/cookie", {"cookie": {"name": "ho", "value": "1", "httpOnly": True}})
            check(wd.s("GET", "/cookie/ho")["httpOnly"] is True, "httpOnly")
            check("ho=" not in wd.run("return document.cookie"), "httpOnly hidden")
            wd.s("DELETE", "/cookie/wd")
            expect_error("no such cookie", wd.s, "GET", "/cookie/wd")
            expect_error("invalid cookie domain", wd.s, "POST", "/cookie",
                         {"cookie": {"name": "x", "value": "y", "domain": "example.com"}})
            wd.s("DELETE", "/cookie")
            check(wd.s("GET", "/cookie") == [], "all cookies deleted")

        @test("screenshots")
        def _():
            png = base64.b64decode(wd.s("GET", "/screenshot"))
            check(png[:8] == b"\x89PNG\r\n\x1a\n", "png signature")
            w = int.from_bytes(png[16:20], "big")
            h = int.from_bytes(png[20:24], "big")
            check((w, h) == (1000, 700), "screenshot size %dx%d" % (w, h))
            epng = base64.b64decode(wd.s("GET", "/element/%s/screenshot" % wd.css("#btn")))
            check(epng[:8] == b"\x89PNG\r\n\x1a\n", "element png")
            ew = int.from_bytes(epng[16:20], "big")
            check(0 < ew < 400, "element screenshot width %d" % ew)

        @test("print to PDF")
        def _():
            pdf = base64.b64decode(wd.s("POST", "/print", {"orientation": "landscape"}))
            check(pdf[:5] == b"%PDF-", "pdf header")
            expect_error("invalid argument", wd.s, "POST", "/print", {"scale": 5})

        @test("page source")
        def _():
            src = wd.s("GET", "/source")
            check("<h1" in src and "Hello" in src or "Clicked" in src, "source")

        @test("form submit with Enter navigates")
        def _():
            wd.s("POST", "/element/%s/value" % wd.css("#q"), {"text": "hi there"})
            url = wd.s("GET", "/url")
            check(url.startswith(site + "/second.html") and "q=hi" in url, "submitted url %r" % url)

        @test("link navigation, back, forward, refresh")
        def _():
            wd.s("POST", "/url", {"url": site + "/index.html"})
            link = wd.css("#link")
            wd.s("POST", "/element/%s/click" % link, {})
            check(wd.s("GET", "/title") == "Second page", "after link click")
            expect_error("stale element reference", wd.s, "GET", "/element/%s/text" % link)
            wd.s("POST", "/back", {})
            check(wd.s("GET", "/title") == "WebDriver fixture", "after back")
            wd.s("POST", "/forward", {})
            check(wd.s("GET", "/title") == "Second page", "after forward")
            wd.s("POST", "/refresh", {})
            check(wd.s("GET", "/url") == site + "/second.html", "after refresh")

        @test("script navigation lands")
        def _():
            wd.run("location.href = arguments[0]", site + "/index.html")
            check(wd.s("GET", "/title") == "WebDriver fixture", "script navigated")

        @test("scroll into view and click far below")
        def _():
            wd.s("POST", "/url", {"url": site + "/tall.html"})
            far = wd.css("#far")
            wd.s("POST", "/element/%s/click" % far, {})
            check(wd.s("GET", "/element/%s/text" % far) == "far clicked", "far button clicked")
            check(wd.run("return window.scrollY") > 0, "page scrolled")

        @test("actions: pointer and keys")
        def _():
            wd.s("POST", "/url", {"url": site + "/index.html"})
            btn = wd.css("#btn")
            wd.s("POST", "/actions", {"actions": [{
                "type": "pointer", "id": "mouse", "parameters": {"pointerType": "mouse"},
                "actions": [
                    {"type": "pointerMove", "origin": {ELEMENT: btn}, "x": 0, "y": 0},
                    {"type": "pointerDown", "button": 0},
                    {"type": "pointerUp", "button": 0}]}]})
            check(wd.s("GET", "/element/%s/text" % wd.css("#heading")) == "Clicked", "pointer click")
            name = wd.css("#name")
            wd.s("POST", "/element/%s/click" % name, {})
            wd.s("POST", "/actions", {"actions": [{
                "type": "key", "id": "kbd", "actions": [
                    {"type": "keyDown", "value": ""},
                    {"type": "keyDown", "value": "x"},
                    {"type": "keyUp", "value": "x"},
                    {"type": "keyUp", "value": ""},
                    {"type": "keyDown", "value": "y"},
                    {"type": "keyUp", "value": "y"}]}]})
            check(wd.s("GET", "/element/%s/property/value" % name) == "Xy", "key actions")
            wd.s("DELETE", "/actions")
            expect_error("move target out of bounds", wd.s, "POST", "/actions", {"actions": [{
                "type": "pointer", "id": "mouse", "actions": [
                    {"type": "pointerMove", "x": 50000, "y": 10}]}]})
            expect_error("invalid argument", wd.s, "POST", "/actions", {"actions": [{
                "type": "pointer", "id": "mouse", "actions": [{"type": "bogus"}]}]})

        @test("wheel scrolls the page")
        def _():
            wd.s("POST", "/url", {"url": site + "/tall.html"})
            wd.s("POST", "/actions", {"actions": [{
                "type": "wheel", "id": "wheel", "actions": [
                    {"type": "scroll", "x": 100, "y": 100, "deltaX": 0, "deltaY": 500}]}]})
            y = wd.run("return window.scrollY")
            check(y >= 400, "scrollY after wheel %r" % y)

        @test("frames")
        def _():
            wd.s("POST", "/url", {"url": site + "/frames.html"})
            wd.s("POST", "/frame", {"id": 0})
            p = wd.css("#innerp")
            check(wd.s("GET", "/element/%s/text" % p) == "inside the frame", "frame text")
            expect_error("no such element", wd.find, "css selector", "#outer")
            check(wd.run("return document.getElementById('innerp').id") == "innerp",
                  "script runs against the frame's document")
            wd.s("POST", "/frame/parent", {})
            check(ELEMENT in wd.find("css selector", "#outer"), "back in parent")
            frm = wd.css("#frm")
            wd.s("POST", "/frame", {"id": {ELEMENT: frm}})
            check(ELEMENT in wd.find("css selector", "#innerp"), "frame by element")
            wd.s("POST", "/frame", {"id": None})
            check(ELEMENT in wd.find("css selector", "#outer"), "top again")
            expect_error("no such frame", wd.s, "POST", "/frame", {"id": 7})

        @test("window rect and windows")
        def _():
            r = wd.s("POST", "/window/rect", {"width": 800, "height": 600})
            check(r["width"] == 800 and r["height"] == 600, "rect %r" % r)
            check(wd.run("return innerWidth") == 800, "innerWidth follows")
            first = wd.s("GET", "/window")
            new = wd.s("POST", "/window/new", {"type": "tab"})
            check(len(wd.s("GET", "/window/handles")) == 2, "two windows")
            wd.s("POST", "/window", {"handle": new["handle"]})
            check(wd.s("GET", "/url") == "about:blank", "new window is blank")
            handles = wd.s("DELETE", "/window")
            check(handles == [first], "remaining %r" % handles)
            expect_error("no such window", wd.s, "GET", "/url")
            wd.s("POST", "/window", {"handle": first})
            check(wd.s("GET", "/title") == "Frames", "back to first window")

        @test("no user prompts")
        def _():
            expect_error("no such alert", wd.s, "GET", "/alert/text")

        @test("delete session")
        def _():
            wd.s("DELETE", "")
            check(wd.call("GET", "/status")["ready"] is True, "ready again")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        httpd.shutdown()

    failed = [n for n, e in results if e]
    print("\n%d passed, %d failed" % (len(results) - len(failed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
