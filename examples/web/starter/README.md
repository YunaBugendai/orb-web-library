# Orb! Web starter

A minimal HTML + CSS + Orb! application. The button handler and counter are written in Orb! and
compiled to WebAssembly.

From the repository root:

~~~
make
make web-examples
make serve
~~~

Open http://localhost:8000/examples/web/starter/.

To change the application, edit main.orb and rebuild the examples with make web-examples.
The generated app.wasm file is intentionally ignored by Git.
