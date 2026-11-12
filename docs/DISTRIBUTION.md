# Orb! installation and distribution

Orb! is designed so that a normal project does not install libraries one by one.

## Standard modules are built in

The compiler binary contains the standard-library sources. These modules can be used directly:

```orb
use math;
use strings;
use vec;
use io;
```

The WebAssembly target adds the `web` module automatically:

```orb
use web;

fn main() {
    web.log("Hello from Orb! Web");
}
```

There is no package-manager command for these modules. Rebuilding the compiler embeds the current
`std/*.orb` files into `build/orb`.

## WebAssembly: one build, two outputs

```sh
orb build main.orb --target wasm -o app.wasm
```

The compiler creates:

- `app.wasm`: compiled application.
- `app.wasm.js`: browser host runtime bundled with a launcher for that exact WASM file.

Load it from HTML:

```html
<script type="module" src="./app.wasm.js"></script>
```

The page does not import a package or manually call `start()`. Serve it over HTTP or HTTPS.

## Interactive installer

From a source checkout on Linux or macOS:

```sh
python3 tools/install.py
```

The terminal wizard lets users choose the compiler, VS Code extension, example projects,
offline documentation, editable standard-library sources and PATH integration.

The core compiler always includes the standard modules and WebAssembly runtime. Optional standard
library source installation is only for people who want to inspect or modify those sources.

Windows users can run the graphical installer built from `installer/Orb.iss`. It offers the same
optional components and can add Orb! to the current user's PATH. Windows native code generation
remains experimental; WSL2 is the recommended Windows execution environment.

## Build the examples

```sh
make
make web-examples
make serve
```

Open `http://localhost:8000/examples/web/starter/`.

## Why is there still a JavaScript runtime?

Browsers do not execute WebAssembly modules in isolation: browser APIs such as the DOM, events,
storage and Fetch are JavaScript host APIs. Orb! embeds the bridge into the compiler and emits it
automatically as `app.wasm.js` beside the compiled application. That is a compiler-generated output,
not a library the application developer must install or maintain separately.
