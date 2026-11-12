# The WebAssembly target

`orb build prog.orb --target wasm [-o prog.wasm]` compiles an Orb! program to a WebAssembly module
(wasm32). `src/wasm.c` is a second code generator that consumes the **same optimised IR** as the x86-64
backend; the frontend, type checker, lowering and optimiser are shared. The compiler writes the binary
directly: no assembler, linker, LLVM or Emscripten is involved, and `-S` is not available for this target
(use `--emit-ir` to look at the IR).

Orb! Web ([WEB.md](WEB.md)) is the browser library built on this target. A wasm module also runs outside
a browser: `node tools/orb-node.js prog.wasm` runs `main` under Node.js.

## Result of the compiler audit

The task started with the questions below; these are the answers found by reading the source of v0.2.0.

| question | answer |
|---|---|
| Can the architecture support a WASM backend? | **Yes.** The pipeline is parser, sema/check, lowering to a three-address IR with virtual registers (`ir.h`), optimiser (`opt.c`), then `emit.c` (register allocation + x86-64 text). The IR is machine independent apart from the 8-byte pointer size, so a second backend after `opt.c` is a clean fit. |
| Was WASM support already there? | No. |
| Can the runtime and standard library be reused? | **Almost entirely.** v0.2.0 already isolates the system interface in the module `os`, with one file per target. The standard library (`core`, `strings`, `vec`, `math`, `io`) is reused unchanged; the new target adds `std/os_wasm.orb`. |
| Which language features were missing? | None that blocked the work. Orb! has `extern fn name(...) = "symbol"` (used for host imports) and function pointers (callbacks). It has no closures, so the web library keeps callbacks in a registry and the host calls `export_dispatch(id, arg)`. The type `error` cannot be written in a parameter list; the tests cast with `e as i64`. |
| What had to change in the compiler? | `ast.h` (target `TGT_WASM`, builtins `B_MEMGROW`/`B_MEMSIZE`), `check.c` and `lower.c` (the two builtins, `OP_MEMGROW` / `OP_MEMSIZE`), `sema.c` (`os_wasm`, `use web` only for wasm), `main.c` (`--target wasm`, `--stack-size`, `--wasm-dispatch`), `Makefile`, new `src/wasm.c`, `std/os_wasm.orb`, `std/web.orb`. The x86 backend is untouched. |

## How the backend maps Orb! to WebAssembly

* **Values.** Every integer / pointer value is an `i64` local and every `f64` an `f64` local, exactly like
  the IR's register classes. Because pointers stay 8 bytes in memory, struct layouts equal the native
  targets. Addresses are wrapped to `i32` when they reach linear memory (4 GiB limit).
* **Calls.** Internally every argument is an `i64` (floats as bit patterns, as on x86-64) and a function
  returns 0, 1 or 2 `i64` values (multi-value). Direct calls use `call`.
* **Function pointers** are indices into one `funcref` table (index 0 is reserved, so calling `null` traps).
  Address-taken functions get a thunk with the uniform type `(i64 x n) -> (i64, i64)` so `call_indirect` is
  type-uniform.
* **Aggregates and address-taken locals** live on a shadow stack in linear memory (global `$sp`, exported as
  `__orb_sp`). Frames are 16-byte aligned. A function that needs a frame checks the stack limit and calls the
  runtime's `panic("stack overflow")` before it would overflow (default 1 MiB, `--stack-size <KiB>`).
  Functions without a frame use only the engine's native stack; extremely deep recursion there raises the
  engine's own `RangeError`.
* **Control flow.** The IR is a linear list of labels and jumps. The backend first tries to rebuild properly
  nested `block` / `loop` constructs (forward jumps become `br` out of a block, backward jumps `br` to a
  loop); if the jumps cannot be nested it falls back to a `loop` + `br_table` dispatcher, which is always
  correct. `--wasm-dispatch` forces the fallback (the test suite runs both).
* **Memory layout.** `[0,1024)` unused; then string literals, the error-name table and globals; then the
  shadow stack; the heap follows and grows with `memory.grow` in 64 KiB pages (the `core` allocator asks
  `os.alloc` for 4 MiB chunks). Zero-initialised globals take no space in the `.wasm` file.
* **Imports.** An `extern fn` becomes an import of module `env`, or of module `m` when the symbol string is
  `"m.name"`. The signature is derived from the Orb! types: `f64` -> `f64`, `i64`/`u64` -> `i64`, other
  integers, `bool`, pointers, function pointers, enums -> `i32` (sign-extended on return if the Orb! type is
  signed), `str` and slices -> two `i32` (pointer, length). Results may be a scalar, `str` or a slice.
  Aggregates and error unions are rejected with a message at compile time.
* **Exports.** `_start` (runs `rt_init`, `main`, then flushes), `memory`, `__indirect_function_table`,
  `__orb_sp`, every `pub fn` of the entry file, and every `pub fn export_*` of any module, with the same
  type mapping as imports.
* **Program lifetime.** After `main` returns the instance stays usable (a reactor): a non-zero `i64` result
  or an error from `main`, `exit` and `panic` go through the normal runtime path and end the program.
* **Differences from native.** `argc` is 0; no files, no stdin; `__syscall` is rejected at compile time;
  float-to-integer casts saturate (native x86 yields the minimum integer for out-of-range values);
  null-pointer reads do not trap. Everything else prints the same as the native build: the 14 programs of
  `tests/ok` that do not read stdin produce byte-identical output (two lines of `features_c` differ because
  of `argc` and files; `wordcount` needs stdin and is not run).

## Calling your own JavaScript

```orb
extern fn alert_user(p: *u8, n: i32) = "env.alert_user";     // `= "name"` or `= "module.name"`

fn main() {
    msg := "hello";
    alert_user(msg.ptr, msg.len as i32);
}
```
```js
start("app.wasm", { imports: { env: { alert_user(p, n) {
  alert(new TextDecoder().decode(new Uint8Array(app.memory.buffer, p, n)));   // memory may grow: re-create views
} } } });
```

## Flags

| flag | meaning |
|---|---|
| `--target wasm` | produce a WebAssembly module |
| `--stack-size <KiB>` | shadow stack size (default 1024) |
| `--wasm-dispatch` | force the `br_table` control-flow strategy (testing) |
| `--unchecked` | as on native: no bounds / division checks (a zero division then traps in the engine) |
| `--no-opt`, `--emit-ir` | as on native |

## Measurements

Collatz-steps benchmark (1.5 million starting values, integer loops), wall time including the start-up of
`node` 22 on the development machine:

| build | time |
|---|---|
| native Orb! (x86-64) | 0.46 s |
| WebAssembly, structured control flow (Node/V8) | 0.78 s |
| WebAssembly, `br_table` dispatcher (Node/V8) | 0.82 s |

One benchmark on one machine: read it as "within about 2x of native Orb! under V8", not as a general result.
Module sizes (not minified, no `wasm-opt`): hello world 2.1 KB, counter example 10 KB, todo example 20 KB.

## Known limitations of the backend

* The structured-control-flow pass nests intervals greedily and falls back to the dispatcher when that
  fails; it is not a full relooper. Both strategies are tested for equivalence.
* No register-level optimisation of the generated code (every IR operation goes through locals); the
  engine's optimising tier does the rest.
* Source maps are not produced; function names are present in the `name` section for DevTools stack traces.
* wasm32 only (no memory64, no threads, no SIMD).
