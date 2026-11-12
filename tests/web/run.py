#!/usr/bin/env python3
"""Orb! Web browser tests: compiles each tests/web/*.orb with --target wasm, loads it in a real Chromium
(Playwright), lets the Orb! program run its own checks (PASS/FAIL lines on the console) and, for some
tests, drives the page like a user (clicks, typing, keys) and asserts on the resulting DOM.

usage: python3 tests/web/run.py [test-name ...]      (needs: build/orb, python3 playwright + chromium)"""
import asyncio, http.server, json, os, subprocess, sys, threading, functools

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ORB = os.path.join(ROOT, "build", "orb")
OUT = os.path.join(ROOT, "build", "web-tests")
os.makedirs(OUT, exist_ok=True)

results = {"pass": 0, "fail": 0}
def ok(name): results["pass"] += 1
def bad(name, why=""):
    results["fail"] += 1
    print("FAIL:", name, ("-- " + why) if why else "")

class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

def serve():
    handler = functools.partial(Quiet, directory=ROOT)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]

def build(name, flags):
    out = os.path.join(OUT, name + ".wasm")
    r = subprocess.run([ORB, "build", os.path.join(ROOT, "tests", "web", name + ".orb"), "--target", "wasm", "-o", out] + flags,
                       capture_output=True, text=True)
    if r.returncode != 0:
        bad(name + " (compile " + " ".join(flags) + ")", r.stderr.strip()[:400])
        return False
    return True

async def open_page(browser, port, page_name, query="", route=None):
    ctx = await browser.new_context()
    page = await ctx.new_page()
    logs = []
    page.on("console", lambda m: logs.append((m.type, m.text)))
    page.on("pageerror", lambda e: logs.append(("pageerror", str(e))))
    if route: await route(page)
    await page.goto(f"http://127.0.0.1:{port}/tests/web/{page_name}.html{query}")
    await page.wait_for_function("window.ready === true", timeout=15000)
    return ctx, page, logs

async def wait_done(page, logs, timeout=20.0):
    for _ in range(int(timeout * 20)):
        if any(t.startswith("DONE") for _, t in logs): return True
        await asyncio.sleep(0.05)
    return False

def report_logs(name, logs, expect_min):
    passes = [t for _, t in logs if t.startswith("PASS ")]
    fails = [t for _, t in logs if t.startswith("FAIL ")]
    expected_noise = ("console-error", "Failed to load resource")      # triggered on purpose by the tests
    errs = [t for k, t in logs if (k == "pageerror" or (k == "error" and not t.startswith("FAIL")))
            and not t.startswith(expected_noise)]
    for f in fails: bad(name, f)
    for e in errs: bad(name, "page error: " + e[:200])
    if len(passes) < expect_min: bad(name, f"only {len(passes)} checks passed, expected at least {expect_min}")
    else: ok(name)
    return len(passes)

def build_example(name, flags):
    d = os.path.join(ROOT, "examples", "web", name)
    r = subprocess.run([ORB, "build", os.path.join(d, "main.orb"), "--target", "wasm", "-o", os.path.join(d, "app.wasm")] + flags,
                       capture_output=True, text=True)
    if r.returncode != 0:
        bad("example " + name + " (compile)", r.stderr.strip()[:400]); return False
    return True

async def open_example(browser, port, name, route=None, ctx=None):
    ctx = ctx or await browser.new_context()
    page = await ctx.new_page()
    logs = []
    page.on("console", lambda m: logs.append((m.type, m.text)))
    page.on("pageerror", lambda e: logs.append(("pageerror", str(e))))
    if route: await route(page)
    await page.goto(f"http://127.0.0.1:{port}/examples/web/{name}/index.html")
    await page.wait_for_function("window.ready === true", timeout=15000)
    return ctx, page, logs

async def t_examples(browser, port, flags):
    for name in ["counter", "todo", "form", "fetch", "stopwatch"]:
        if not build_example(name, flags): continue
        n = "example " + name + str(flags)
        def eq(what, got, want, n=n):
            if got == want: ok(n + ": " + what)
            else: bad(n + ": " + what, f"got {got!r}, want {want!r}")
        if name == "counter":
            ctx, page, logs = await open_example(browser, port, name)
            for _ in range(3): await page.click("#inc")
            eq("+1 x3", await page.inner_text("#count"), "3")
            for _ in range(5): await page.click("#dec")
            eq("-1 x5", await page.inner_text("#count"), "-2")
            eq("negative numbers get the neg class", await page.get_attribute("#count", "class"), "big neg")
            await page.reload(); await page.wait_for_function("window.ready === true")
            eq("the count survives a reload (localStorage)", await page.inner_text("#count"), "-2")
            await page.click("#reset")
            eq("reset", await page.inner_text("#count"), "0")
            eq("neg class removed", await page.get_attribute("#count", "class"), "big")
            eq("no errors", [t for k, t in logs if k in ("pageerror", "error")], [])
            await ctx.close()
        elif name == "todo":
            ctx, page, logs = await open_example(browser, port, name)
            await page.fill("#new", "buy milk"); await page.press("#new", "Enter")
            await page.fill("#new", "write Orb!"); await page.press("#new", "Enter")
            eq("input cleared after add", await page.input_value("#new"), "")
            await page.fill("#new", "   "); await page.press("#new", "Enter")
            eq("two items added (blank ignored)", await page.locator("#list li").count(), 2)
            eq("remaining count", await page.inner_text("#left"), "2 item(s) left")
            await page.click("#list li:first-child span")
            eq("click toggles done", await page.get_attribute("#list li:first-child span", "class"), "done")
            eq("remaining count updates", await page.inner_text("#left"), "1 item(s) left")
            await page.reload(); await page.wait_for_function("window.ready === true")
            eq("list restored from JSON in localStorage", await page.locator("#list li").count(), 2)
            eq("done state restored", await page.get_attribute("#list li:first-child span", "class"), "done")
            await page.click("#list li:first-child button")
            eq("delete", await page.locator("#list li").count(), 1)
            eq("the right item remains", await page.inner_text("#list li span"), "write Orb!")
            eq("no errors", [t for k, t in logs if k in ("pageerror", "error")], [])
            await ctx.close()
        elif name == "form":
            ctx, page, logs = await open_example(browser, port, name)
            eq("submit starts disabled", await page.is_disabled("#send"), True)
            await page.fill("#name", "A")
            eq("short name is reported", await page.inner_text("#name-msg"), "at least 2 characters")
            await page.fill("#name", "Ada Lovelace"); await page.fill("#email", "ada@")
            eq("bad e-mail is reported", await page.inner_text("#email-msg"), "not a valid e-mail address")
            await page.fill("#email", "ada@example.org"); await page.check("#terms")
            eq("valid form enables submit", await page.is_disabled("#send"), False)
            await page.click("#send")
            eq("submit handled without navigation", await page.inner_text("#result"), "Welcome, Ada Lovelace")
            await page.uncheck("#terms")
            eq("unchecking terms disables submit again", await page.is_disabled("#send"), True)
            eq("no errors", [t for k, t in logs if k in ("pageerror", "error")], [])
            await ctx.close()
        elif name == "fetch":
            ctx, page, logs = await open_example(browser, port, name)
            await page.click("#load")
            await page.wait_for_function("document.getElementById('status').textContent === 'Loaded.'", timeout=5000)
            eq("three people listed", await page.locator("#people li").count(), 3)
            eq("rendered from JSON", await page.inner_text("#people li:nth-child(2)"), "Grace Hopper - compiler pioneer")
            await page.click("#load")
            await page.wait_for_function("document.getElementById('status').textContent === 'Loaded.'", timeout=5000)
            eq("loading again replaces the list", await page.locator("#people li").count(), 3)
            await ctx.close()
            ctx2 = await browser.new_context()
            async def abort(page): await page.route("**/people.json", lambda r: r.abort())
            ctx2, page2, logs2 = await open_example(browser, port, name, abort, ctx2)
            await page2.click("#load")
            await page2.wait_for_function("document.getElementById('status').textContent.startsWith('Network error')", timeout=5000)
            eq("network failure is shown to the user", await page2.locator("#people li").count(), 0)
            await ctx2.close()
        elif name == "stopwatch":
            ctx, page, logs = await open_example(browser, port, name)
            await page.click("#startstop")
            await asyncio.sleep(0.5)
            await page.click("#startstop")
            t = await page.inner_text("#time")
            eq("time advanced", t != "00:00.0" and t.startswith("00:00."), True)
            tenths = int(t.split(".")[1])
            eq("about half a second elapsed", 3 <= tenths <= 9, True)
            await asyncio.sleep(0.3)
            eq("stopped: the display no longer changes", await page.inner_text("#time"), t)
            eq("interval cleared", (await page.evaluate("window.orb.stats()"))["timers"], 0)
            await page.click("#reset")
            eq("reset", await page.inner_text("#time"), "00:00.0")
            eq("no errors", [t for k, t in logs if k in ("pageerror", "error")], [])
            await ctx.close()

# ---------------------------------------------------------------------------------------------------
async def t_dom(browser, port, flags):
    ctx, page, logs = await open_page(browser, port, "dom")
    if not await wait_done(page, logs): bad("dom" + str(flags), "timeout waiting for DONE")
    n = report_logs("dom" + str(flags), logs, 42)
    st = await page.evaluate("window.orb.stats()")
    if st["handles"] > 30: bad("dom handles", f"{st}")
    await ctx.close()

async def t_events(browser, port, flags):
    n = "events" + str(flags)
    ctx, page, logs = await open_page(browser, port, "events")
    txt = lambda sel: page.inner_text(sel)
    def eq(what, got, want):
        if got == want: ok(n + ": " + what)
        else: bad(n + ": " + what, f"got {got!r}, want {want!r}")
    cb0 = await page.evaluate("Number(window.orb.exports.callbacks())")
    eq("registered callbacks", cb0, 9)
    for _ in range(3): await page.click("#btn")
    eq("click x3", await txt("#clicks"), "3")
    await page.fill("#txt", "hello wörld")
    eq("input event carries the value", await txt("#inval"), "hello wörld")
    await page.press("#txt", "Tab")
    eq("change event on blur", await txt("#changes"), "1")
    url0 = page.url
    await page.click("#submit")
    await asyncio.sleep(0.2)
    eq("submit handled", await txt("#submits"), "1")
    eq("preventDefault stopped the navigation", page.url, url0)
    await page.focus("#txt")
    await page.keyboard.press("a")
    eq("keydown key/code", await txt("#lastkey"), "a|KeyA")
    await page.keyboard.press("Shift+B")
    eq("keydown with modifier", await txt("#lastkey"), "B|KeyB|shift")
    await page.keyboard.press("c")
    eq("keyup", await txt("#lastup"), "c")
    await page.keyboard.press("Enter")
    eq("Enter key", (await txt("#lastkey")).split("|")[0], "Enter")
    box = await page.locator("#area").bounding_box()
    await page.mouse.click(box["x"] + 10, box["y"] + 20, button="left")
    mouse = await txt("#mouse")
    want_x, want_y = int(box["x"] + 10), int(box["y"] + 20)
    eq("mouse position and button", mouse, f"{want_x},{want_y},b0")
    eq("event target element id", await txt("#evtype"), "area")
    await page.click("#rm")
    await page.click("#btn")
    eq("off() removes the listener", await txt("#clicks"), "3")
    cb1 = await page.evaluate("Number(window.orb.exports.callbacks())")
    eq("off() frees the callback slot", cb1, cb0 - 1)
    st = await page.evaluate("window.orb.stats()")
    eq("runtime listener count after off()", st["listeners"], cb0 - 1)
    await page.click("#rearm")
    await page.click("#btn")
    eq("listener can be registered again", await txt("#clicks"), "4")
    await page.evaluate("document.getElementById('btn').dispatchEvent(new Event('click'))")
    eq("synthetic events reach the handler", await txt("#clicks"), "5")
    errs = [t for k, t in logs if k in ("pageerror",) or t.startswith("FAIL")]
    eq("no errors on the page", errs, [])
    st = await page.evaluate("window.orb.stats()")
    eq("no leaked handles after many events (only singletons)", st["handles"] <= 10, True)
    await ctx.close()

async def t_browser(browser, port, flags):
    n = "browser" + str(flags)
    async def route(page):
        async def echo(r):
            body = r.request.post_data or ""
            await r.fulfill(status=201, content_type="application/json", body=body)
        await page.route("**/echo", echo)
        await page.route("**/blocked", lambda r: r.abort())
    ctx, page, logs = await open_page(browser, port, "browser", "?q=hello%20world&n=5", route)
    if not await wait_done(page, logs): bad(n, "timeout waiting for DONE")
    report_logs(n, logs, 44)
    have = {(k, t) for k, t in logs}
    for kind, text in [("log", "console-log"), ("info", "console-info"), ("warning", "console-warn"),
                       ("error", "console-error"), ("debug", "console-debug"), ("log", "console-println")]:
        if (kind, text) in have: ok(n + ": console " + text)
        else: bad(n + ": console", f"missing {kind}:{text}")
    st = await page.evaluate("window.orb.stats()")
    if st["timers"] != 0 or st["fetches"] != 0: bad(n + ": leftovers", str(st))
    else: ok(n + ": no timers or fetches left over")
    await ctx.close()

async def t_data(browser, port, flags):
    n = "data" + str(flags)
    ctx, page, logs = await open_page(browser, port, "data")
    if not await wait_done(page, logs, 40): bad(n, "timeout waiting for DONE")
    report_logs(n, logs, 28)
    await ctx.close()

async def t_memory(browser, port, flags):
    n = "memory" + str(flags)
    ctx, page, logs = await open_page(browser, port, "memory")
    ev = lambda js: page.evaluate(js)
    def eq(what, got, want):
        if got == want: ok(n + ": " + what)
        else: bad(n + ": " + what, f"got {got!r}, want {want!r}")
    base = await ev("window.orb.stats()")
    eq("churn(20000) succeeds", await ev("window.orb.exports.churn(20000)"), 0)
    after = await ev("window.orb.stats()")
    eq("no element handles leaked", after["handles"], base["handles"])
    eq("no children left in the list", await ev("document.getElementById('list').children.length"), 0)
    mem1 = after["memoryBytes"]
    eq("churn again", await ev("window.orb.exports.churn(20000)"), 0)
    mem2 = (await ev("window.orb.stats()"))["memoryBytes"]
    eq("heap does not grow on the second round", mem2, mem1)
    eq("listeners(5000) succeeds", await ev("window.orb.exports.listeners(5000)"), 0)
    st = await ev("window.orb.stats()")
    eq("no DOM listeners left", st["listeners"], 0)
    eq("no callback slots left", await ev("Number(window.orb.exports.live())"), 0)
    eq("json(5000) succeeds", await ev("window.orb.exports.json(5000)"), 0)
    eq("no JSON handles leaked", (await ev("window.orb.stats()"))["handles"], base["handles"])
    eq("small_blocks(50000) succeeds", await ev("window.orb.exports.small_blocks(50000)"), 0)
    m3 = (await ev("window.orb.stats()"))["memoryBytes"]
    await ev("window.orb.exports.big_blocks(3)")
    m4 = (await ev("window.orb.stats()"))["memoryBytes"]
    eq("big_blocks(300) succeeds", await ev("window.orb.exports.big_blocks(300)"), 0)
    m5 = (await ev("window.orb.stats()"))["memoryBytes"]
    eq("large blocks are reused (memory is flat over 300 alloc/free cycles)", m5, m4)
    if m5 > 40 * 1024 * 1024: bad(n + ": memory", f"{m5} bytes is more than expected")
    else: ok(n + ": total memory stays small (" + str(m5 // 1024) + " KiB)")
    # a stale handle must be rejected, and a trap must not corrupt later calls (shadow stack is restored)
    r = await ev("(() => { const e = window.orb.exports; return e.churn(10); })()")
    eq("still works after everything", r, 0)
    errs = [t for k, t in logs if k == "pageerror" or t.startswith("FAIL")]
    eq("no page errors", errs, [])
    await ctx.close()

EXTRA_FLAGS = {"trap": ["--unchecked"]}

async def t_trap(browser, port, flags):
    n = "trap" + str(flags)
    ctx, page, logs = await open_page(browser, port, "trap")
    ev = lambda js: page.evaluate(js)
    def eq(what, got, want):
        if got == want: ok(n + ": " + what)
        else: bad(n + ": " + what, f"got {got!r}, want {want!r}")
    sp0 = await ev("window.orb.exports.__orb_sp.value")
    await page.click("#bump")
    for _ in range(50): await page.click("#boom")
    eq("a trap in a handler is reported, not fatal", any("divide by zero" in t.lower() for k, t in logs if k == "error"), True)
    eq("the shadow stack pointer is restored after 50 traps", await ev("window.orb.exports.__orb_sp.value"), sp0)
    await page.click("#bump")
    eq("handlers still work after traps", await page.inner_text("#n"), "2")
    eq("the program is still running", (await ev("window.orb.stats()"))["exited"], None)
    await page.click("#die")
    await asyncio.sleep(0.1)
    eq("panic: message on the console", any("deliberate panic" in t for k, t in logs if k == "error"), True)
    eq("panic ends the program with exit code 101", await ev("window.exitCode"), 101)
    await page.click("#bump")
    eq("events are ignored after the program exited", await page.inner_text("#n"), "2")
    await ctx.close()

TESTS = {"examples": t_examples, "trap": t_trap, "dom": t_dom, "events": t_events, "browser": t_browser, "data": t_data, "memory": t_memory}

async def main():
    from playwright.async_api import async_playwright
    wanted = sys.argv[1:] or list(TESTS)
    srv, port = serve()
    async with async_playwright() as p:
        browser = await p.chromium.launch(args=["--no-sandbox"])
        for name in wanted:
            for flags in ([], ["--wasm-dispatch"]):
                if name == "examples" or build(name, flags + EXTRA_FLAGS.get(name, [])):
                    try: await TESTS[name](browser, port, flags)
                    except Exception as e: bad(name + str(flags), f"{type(e).__name__}: {e}")
        await browser.close()
    print(f"web tests: passed {results['pass']}, failed {results['fail']}")
    sys.exit(1 if results["fail"] else 0)

asyncio.run(main())
