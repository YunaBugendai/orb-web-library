# Orb! Web: HTML + CSS + Orb!

Orb! Web lets you write the logic of a browser application in Orb! instead of JavaScript. HTML and CSS
stay exactly what they are; the Orb! program is compiled to WebAssembly and talks to the page through the
standard module `web`.

```
 page.html + style.css          main.orb
        |                           |  orb build --target wasm
        |                           v
        |                       app.wasm  (your logic, Orb! compiled to WebAssembly)
        |                           |
        +---- app.wasm.js (generated automatically; contains the embedded browser host runtime)
                                    |
                              browser: DOM, events, timers, storage, location, fetch, JSON
```

There is no HTML parser or template language in Orb!, and no JavaScript transpilation: `main.orb` is
compiled by the real Orb! compiler's WebAssembly backend (see [WASM.md](WASM.md)). The browser bridge source
is maintained in `web/orb.js` and embedded into the compiler; developers do not copy or install it.

## Installation: built in, not an extra package

The standard library is embedded in the Orb! compiler itself. `use math;`, `use strings;`, and
`use web;` are resolved from the compiler's built-in modules. There is no npm package to install,
no `--std` path to configure and no separate browser runtime download.

Build your program:

```sh
orb build main.orb --target wasm -o app.wasm
```

Orb! writes **two files**:

- `app.wasm`: your program, compiled from Orb! to WebAssembly.
- `app.wasm.js`: the embedded browser runtime plus an automatic launcher for `app.wasm`.

Add one script tag to your HTML:

```html
<script type="module" src="./app.wasm.js"></script>
```

That's it. No JavaScript import, npm install, copied runtime, Emscripten, LLVM, assembler or linker.
Serve the page over HTTP or HTTPS; browsers do not let a `file://` page fetch a `.wasm` file.

The generated launcher exposes `window.orb` after startup and sets `window.ready = true`. Advanced
users can import the lower-level runtime source from the Orb! repository and call `start(url, options)`
themselves when custom host imports or lifecycle callbacks are needed.

## Getting started

`index.html`
```html
<!doctype html>
<html><body>
  <button id="go">Say hello</button>
  <p id="out"></p>
  <script type="module" src="./app.wasm.js"></script>
</body></html>
```

`main.orb`
```orb
use web;

fn on_go() {
    web.set_text_by_id("out", "Hello from Orb!") catch |e| { web.log_error("no #out element"); };
}

fn main() {
    button := web.get_element("go") catch |e| { return; };
    web.on_click(button, on_go) catch |e| { return; };
}
```

```sh
orb build main.orb --target wasm -o app.wasm
python3 -m http.server          # then open http://localhost:8000/
```

`main` runs once when the page loads. When it returns the program **stays alive**: your registered
callbacks are called by the browser's event loop.

`println`, `print` and `eprintln` work: output goes to the browser console (`console.log` per line,
`console.error` for standard error).

## Concepts

**Errors are explicit.** Everything that can fail returns an error union: `Element!`, `str!`, `!`.
You handle it with `try` (pass it on), `catch default`, or `catch |e| { ... }`, as in the rest of Orb!.
The errors are `web.BadHandle`, `web.NotFound`, `web.Invalid`, `web.Security`, `web.WrongType`,
`web.Network` and `web.Unsupported`; each function documents which ones it can return.

**Handles.** DOM nodes, events, responses and JSON values live in JavaScript; Orb! holds small typed
handles (`web.Element`, `web.Event`, `web.Response`, `web.Json`). A handle contains a generation number,
so a released or made-up handle is rejected with `BadHandle` instead of touching the wrong object.
Element handles are reference counted: asking twice for the same element gives the same handle, and each
`web.release(el)` gives one reference back. Singletons (the button you listen to for the whole session)
can simply be kept. `Event` and `Response` handles are valid only inside the callback that receives them.

**Strings.** `str` crosses the boundary as UTF-8 bytes. Text you pass in is decoded directly from the
program's memory (no intermediate copy). Text you get back is staged once on the JavaScript side and
copied once into a fresh heap block; release it with `web.free_str(s)`. Large payloads (a fetch response)
can instead be read into your own buffer with `web.body_into`, which copies the bytes once and creates no
string.

**Numbers and booleans.** Integers, `f64` and `bool` are passed by value. `f64` properties and JSON
numbers keep full double precision. (JSON and JavaScript numbers are doubles: integers above 2^53 lose
precision.)

**Structured data.** Use the JSON functions: parse into a `web.Json` handle and read members, or build a
value with `json_object` / `json_array` / `json_set` / `json_push` and `json_stringify` it.

**Callbacks.** Orb! has function pointers but no closures, so a callback is a plain `fn` and keeps its state
in global variables (see the examples). Callbacks run one at a time on the browser's event loop and never
re-enter each other, except when your own code triggers an event synchronously (`web.dispatch`).

**Synchronous and asynchronous.** Everything is synchronous (it returns the result) except:
`web.fetch*` (the response arrives later in a callback), `web.set_timeout` / `web.set_interval`
(the callback runs later), and event listeners. Those return immediately.

**Security.** All browser restrictions apply and are reported, not hidden:
`fetch` obeys same-origin / CORS / mixed-content rules (a blocked request arrives as a `Response` with
`failed(r) == true`); `localStorage` can be unavailable (blocked cookies, sandboxed frames) and then
returns `web.Security`, as does exceeding the quota. `set_text` escapes markup; only `set_html` parses
HTML, so never pass it untrusted text.

**Crashes.** A WebAssembly trap or runtime error inside a callback is logged to the console, the
program's stack is restored, and later events keep working (tested). `panic(...)` or `exit(...)` end the
program: the message goes to the console, `start(...)`'s `onexit` option is called, and further events are
ignored.

## `start` options (web/orb.js)

```js
const app = await start("app.wasm", {
  stdout: (text) => ...,        // receive println output instead of console.log
  stderr: (text) => ...,
  onexit: (code) => ...,        // the program called exit() / panicked
  onerror: (error) => ...,      // a callback trapped
  imports: { env: { my_host_function(a, b) { ... } } },   // your own JavaScript functions (see WASM.md)
});
app.exports.my_pub_fn(1, 2);     // `pub fn`s of your main file are exported (see WASM.md)
app.stats();                     // { handles, listeners, timers, fetches, memoryBytes, exited }
```

## API reference

The reference below is generated from the doc comments of `std/web.orb`. `T!` is "a `T` or an error",
`!` is "nothing or an error".

### DOM: lookup and handles

- `web.get_element(id: str) -> Element!` - Returns the element with that id attribute.
- `web.query(selector: str) -> Element!` - Returns the first element matching a CSS selector (Invalid if the selector is malformed).
- `web.query_all(selector: str) -> []Element!` - Returns every element matching a CSS selector. Release the slice with free() and each element with release().
- `web.body() -> Element!`
- `web.document() -> Element!` - The document (for listening to keyboard events anywhere on the page).
- `web.window() -> Element!`
- `web.release(el: Element)`

### DOM: structure

- `web.create(tag: str) -> Element!`
- `web.create_text(text: str) -> Element!`
- `web.append(parent: Element, child: Element) -> !`
- `web.remove(el: Element) -> !` - Removes the element from the document (the handle stays valid until released).
- `web.clear(el: Element) -> !` - Removes all children.
- `web.focus(el: Element) -> !`

### DOM: text, html, attributes

- `web.get_text(el: Element) -> str!`
- `web.set_text(el: Element, text: str) -> !`
- `web.set_text_by_id(id: str, text: str) -> !` - Convenience: set_text on the element with this id.
- `web.get_html(el: Element) -> str!` - innerHTML. Never pass untrusted text here (it is parsed as HTML); use set_text instead.
- `web.set_html(el: Element, html: str) -> !`
- `web.get_attr(el: Element, name: str) -> str!` - NotFound if the attribute is absent.
- `web.set_attr(el: Element, name: str, value: str) -> !`
- `web.remove_attr(el: Element, name: str) -> !`
- `web.has_attr(el: Element, name: str) -> bool!`

### DOM: CSS classes and style

- `web.add_class(el: Element, name: str) -> !`
- `web.remove_class(el: Element, name: str) -> !`
- `web.toggle_class(el: Element, name: str) -> bool!` - Adds the class if it is missing, removes it otherwise; returns whether it is present afterwards.
- `web.has_class(el: Element, name: str) -> bool!`
- `web.set_style(el: Element, property: str, value: str) -> !`

### DOM: properties (el.value, el.checked, ...)

- `web.get_prop_str(el: Element, name: str) -> str!` - WrongType if the property has another type, NotFound if it does not exist.
- `web.set_prop_str(el: Element, name: str, value: str) -> !`
- `web.get_prop_num(el: Element, name: str) -> f64!`
- `web.set_prop_num(el: Element, name: str, value: f64) -> !`
- `web.get_prop_bool(el: Element, name: str) -> bool!`
- `web.set_prop_bool(el: Element, name: str, value: bool) -> !`
- `web.get_value(el: Element) -> str!` - Shortcuts for form controls.
- `web.set_value(el: Element, value: str) -> !`
- `web.is_checked(el: Element) -> bool!`
- `web.set_checked(el: Element, value: bool) -> !`

### Events

- `web.on(el: Element, name: str, f: fn(Event)) -> Listener!` - Calls f every time the event `name` ("click", "input", "keydown", ...) fires on el. The Event handle passed to f is valid only during the call.
- `web.on_click(el: Element, f: fn()) -> Listener!` - Click handler without event details.
- `web.on_input(el: Element, f: fn(Event)) -> Listener!`
- `web.on_change(el: Element, f: fn(Event)) -> Listener!`
- `web.on_submit(el: Element, f: fn(Event)) -> Listener!` - Call prevent_default(ev) inside f to stop the browser from submitting the form.
- `web.on_keydown(el: Element, f: fn(Event)) -> Listener!`
- `web.on_keyup(el: Element, f: fn(Event)) -> Listener!`
- `web.off(l: Listener) -> !` - Removes a listener registered with on / on_click / ...
- `web.dispatch(el: Element, name: str) -> !` - Fires an event on an element, as if the user had caused it (runs the listeners synchronously).
- `web.event_type(ev: Event) -> str!` - ---- event details
- `web.key(ev: Event) -> str!` - KeyboardEvent.key, e.g. "Enter" or "a".
- `web.key_code(ev: Event) -> str!` - KeyboardEvent.code, e.g. "KeyA".
- `web.input_value(ev: Event) -> str!` - event.target.value (for input / change events).
- `web.ctrl_key(ev: Event) -> bool`
- `web.shift_key(ev: Event) -> bool`
- `web.alt_key(ev: Event) -> bool`
- `web.meta_key(ev: Event) -> bool`
- `web.is_repeat(ev: Event) -> bool`
- `web.mouse_button(ev: Event) -> i64`
- `web.mouse_x(ev: Event) -> i64`
- `web.mouse_y(ev: Event) -> i64`
- `web.target(ev: Event) -> Element!` - The element the event was dispatched on. Release it when done.
- `web.prevent_default(ev: Event) -> !`
- `web.stop_propagation(ev: Event) -> !`

### Timers (asynchronous)

- `web.set_timeout(ms: i64, f: fn()) -> Timer!` - Calls f once after ms milliseconds.
- `web.set_interval(ms: i64, f: fn()) -> Timer!` - Calls f every ms milliseconds until clear_timer.
- `web.clear_timer(t: Timer)` - Cancels a timer. Cancelling a timeout that already fired is harmless.
- `web.now() -> f64` - Milliseconds since the page started (performance.now()).
- `web.random() -> f64` - Math.random(): a float in [0, 1).

### Console

- `web.log(msg: str)`
- `web.info(msg: str)`
- `web.warn(msg: str)`
- `web.log_error(msg: str)`
- `web.debug(msg: str)`

### Local / session storage

- `web.local_get(key: str) -> str!` - Security is returned when the browser denies access (blocked cookies, sandboxed frames) or the quota is exceeded. NotFound means "no such key".
- `web.session_get(key: str) -> str!`
- `web.local_set(key: str, value: str) -> !`
- `web.session_set(key: str, value: str) -> !`
- `web.local_remove(key: str) -> !`
- `web.session_remove(key: str) -> !`
- `web.local_clear() -> !`
- `web.session_clear() -> !`
- `web.local_len() -> i64!`
- `web.session_len() -> i64!`
- `web.local_key(i: i64) -> str!` - The i-th key of the storage area.
- `web.session_key(i: i64) -> str!`

### URL and location

- `web.href() -> str` - The functions below return "" if the information is unavailable.
- `web.origin() -> str`
- `web.protocol() -> str`
- `web.host() -> str`
- `web.hostname() -> str`
- `web.pathname() -> str`
- `web.search() -> str`
- `web.hash() -> str`
- `web.query_param(name: str) -> str!` - Value of a query-string parameter (?name=value); NotFound if absent.
- `web.set_hash(value: str) -> !`
- `web.navigate(url: str) -> !` - Navigates to another page (leaves the application).
- `web.reload() -> !`

### Fetch (asynchronous HTTP)

- `web.fetch(url: str, method: str, body: str, content_type: str, f: fn(Response)) -> !` - Starts an HTTP request and returns immediately; f is called later with the Response. `body` and `content_type` may be "". The browser's rules apply: same-origin or CORS, no mixed content, no requests from file:// pages. A failed request (offline, blocked by CORS, ...) is reported to f as a Response with failed(r) == true and status 0. The Response handle is valid only inside f.
- `web.fetch_get(url: str, f: fn(Response)) -> !`
- `web.fetch_post_json(url: str, json: str, f: fn(Response)) -> !` - POSTs a JSON text.
- `web.status(r: Response) -> i64` - HTTP status code (0 if the request failed).
- `web.response_ok(r: Response) -> bool` - True for status 200-299.
- `web.failed(r: Response) -> bool` - True if the request itself failed (network error, CORS, ...): there is no HTTP status.
- `web.body_len(r: Response) -> i64`
- `web.body_into(r: Response, buf: []u8) -> i64!` - Copies the body into your own buffer (one copy, no intermediate string); returns the byte count.
- `web.body_text(r: Response) -> str!` - The body as a newly allocated str (the bytes are used as they are). Release with free_str().
- `web.header(r: Response, name: str) -> str!`

### JSON

- `web.json_parse(text: str) -> Json!` - Parses a JSON text (Invalid on a syntax error). Release the result with json_release().
- `web.json_release(j: Json)`
- `web.json_kind(j: Json) -> i64!` - One of the JSON_* constants.
- `web.json_len(j: Json) -> i64!` - Number of elements of an array / members of an object (WrongType otherwise).
- `web.json_get(j: Json, key: str) -> Json!`
- `web.json_at(j: Json, i: i64) -> Json!`
- `web.json_key_at(j: Json, i: i64) -> str!` - The i-th member name of an object.
- `web.json_str(j: Json) -> str!`
- `web.json_num(j: Json) -> f64!`
- `web.json_bool(j: Json) -> bool!`
- `web.json_stringify(j: Json) -> str!` - Serialises a value back to JSON text. Release with free_str().
- `web.json_object() -> Json!`
- `web.json_array() -> Json!`
- `web.json_null() -> Json!`
- `web.json_from_num(x: f64) -> Json!`
- `web.json_from_str(s: str) -> Json!`
- `web.json_from_bool(b: bool) -> Json!`
- `web.json_set(obj: Json, key: str, child: Json) -> !` - obj[key] = child (the value is copied into the object; release child afterwards).
- `web.json_push(arr: Json, child: Json) -> !`

Constants: `web.JSON_NULL`, `JSON_BOOL`, `JSON_NUMBER`, `JSON_STRING`, `JSON_ARRAY`, `JSON_OBJECT`.
Types: `Element`, `Event`, `Response`, `Json`, `Listener`, `Timer`.

## Examples

All in `examples/web/<name>/` (build with `make web-examples`, serve with `make serve`):

| example | shows |
|---|---|
| `counter` | click handlers, classes, `localStorage` persistence |
| `todo` | building DOM nodes, event delegation, JSON + `localStorage` |
| `form` | `input` / `change` / `submit` events, live validation, `prevent_default` |
| `fetch` | asynchronous `fetch`, JSON parsing, error display |
| `stopwatch` | `set_interval`, `now()`, `clear_timer` |

## Browser compatibility

Output uses WebAssembly features that are standard in all current browsers: multi-value, sign-extension
operators, non-trapping float-to-int conversions and bulk memory. By the feature tables of the engines
that means Chrome/Edge 85+, Firefox 79+ and Safari 15+ (versions from the engines' published feature
status, **not verified by me**).

What has actually been tested: **Chromium (Playwright, headless)** for the browser tests and
**Node.js 22** for the compiler tests. Firefox and Safari have *not* been run. `web/orb.js` is an ES module
and uses only `TextDecoder/TextEncoder`, `WebAssembly`, `fetch` and the DOM.

## Tests

```sh
make test-wasm     # compiler: all tests/ok + tests/panic programs under Node, in 3 modes, ABI, stack overflow
make test-web      # browser: DOM, events, browser APIs, data exchange, memory, traps, the 5 examples (Chromium)
```
`make test-web` needs `python3`, the `playwright` package and a Chromium it can launch; each test is run
with both control-flow strategies of the backend. Last result on the development machine: 51 compiler
checks and 172 browser checks, all passing.

## Known limitations

* No closures: callbacks are plain functions; per-callback state lives in globals (or an index you keep).
* No `async`/`await`: asynchronous operations take a callback.
* `Event` and `Response` handles are invalid after their callback returns; copy what you need.
* Strings are UTF-8 bytes; the host decodes invalid UTF-8 with replacement characters.
* The browser has no file system or standard input: `read_file`, `write_file` report an error,
  `read_stdin` returns an empty string, `args()` is empty.
* A program's memory never shrinks (WebAssembly cannot return pages). Large blocks that are freed are
  reused; the heap otherwise stays at its high-water mark.
* Pointers are 8 bytes inside the program (so data layouts match the native targets) and the address
  space is 4 GiB (wasm32).
* Reading through a null pointer does not trap (it reads zeros from low memory); bounds checks of
  arrays and slices are unchanged and do panic.
* Exported functions (see WASM.md) accept ints, `bool`, `f64`, pointers, `str` and slices; `i64` appears as
  a `BigInt` in JavaScript.
* Only the DOM/browser APIs listed above are wrapped. Anything else needs a few lines of JavaScript
  registered through `start(..., { imports })` and declared with `extern fn` (see WASM.md).
* Not tested: Firefox, Safari, and building the `.wasm` on Windows or macOS hosts.
