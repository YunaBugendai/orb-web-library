# Orb! Web SDK distribution

Orb! Web has two separate pieces:

1. **The Orb! compiler**, which compiles .orb source into .wasm.
2. **The browser runtime**, which loads that .wasm file and provides browser host functions.

The compiler is built from this repository with make. The browser runtime is distributed as the
small ESM package orb-web-runtime.

## Install the runtime from GitHub

~~~
npm install github:YunaBugendai/orb-web-library
~~~

Then import it from an ESM application:

~~~
import { start } from "orb-web-runtime";

const app = await start("/app.wasm");
~~~

This GitHub installation uses the repository's default branch. For reproducible application builds,
pin the dependency to a reviewed commit SHA in your package.json:

~~~
{
  "dependencies": {
    "orb-web-runtime": "github:YunaBugendai/orb-web-library#COMMIT_SHA"
  }
}
~~~

Replace COMMIT_SHA with the full commit hash you have tested. The package has no runtime
dependencies and does not run installation scripts.

## Compile an application

Install/build the Orb! compiler separately, then compile the entry file:

~~~
make
./build/orb build src/main.orb --target wasm -o public/app.wasm
~~~

The compiler needs a C compiler to build. The WebAssembly backend does not need Emscripten,
LLVM, an assembler or a linker.

For a Vite-style application, put app.wasm in the public assets directory and load it by URL:

~~~
import { start } from "orb-web-runtime";

await start("/app.wasm");
~~~

For a plain static site without a bundler, copy the runtime module next to your HTML:

~~~
cp node_modules/orb-web-runtime/web/orb.js public/orb.js
~~~

~~~
<script type="module">
  import { start } from "./orb.js";
  await start("./app.wasm");
</script>
~~~

Serve the site over HTTP or HTTPS. Loading a WebAssembly file from a file:// URL is blocked by
normal browser security rules.

## Starter application

A plain HTML/CSS/Orb! starter project is in examples/web/starter/. From the repository root:

~~~
make
make web-examples
make serve
~~~

Open http://localhost:8000/examples/web/starter/.

## Build the distributable package locally

Node.js 18 or newer and npm are required for packaging:

~~~
npm test
npm pack
~~~

npm pack creates an archive containing only the browser runtime, TypeScript declarations and
relevant WebAssembly standard-library sources/documentation. It does not include the native compiler
or the full repository.

GitHub Actions runs the same package smoke test and uploads the generated archive as a workflow
artifact on successful CI runs.

## Compatibility and scope

- Runtime format: native ECMAScript module (ESM).
- Browser APIs: DOM, events, timers, storage, location, Fetch and JSON, as documented in WEB.md.
- Compiler and browser runtime must come from compatible Orb! revisions. Keep them on the same
  repository revision when using unreleased changes.
- The runtime does not compile .orb source in the browser; compilation is a separate build step.
- The current repository does not contain an explicit open-source license. Do not redistribute the
  package or present it as an OSI-licensed library until Orb Technologies adds a license.

## Versioning

The first packaged runtime follows the repository's Orb! 0.2.0 baseline. The package version,
runtime API and compiler source are kept together in this repository. Breaking runtime API changes
must increment the package's major version; additive backwards-compatible API changes increment the
minor version; compatible fixes increment the patch version.
