# Orb!

**Native performance. Minimal complexity.** — a small statically typed systems language by Orb Technologies.

Orb! compiles `.orb` source files straight to native x86-64 Linux executables. There is no
virtual machine, no garbage collector and no libc: the runtime and standard library are
written in Orb! itself and talk to the kernel through system calls. A hello-world binary is
about 10 KB.

```orb
fn main() {
    x := 10;
    y := 20;

    print(x + y);
}
```

## Quick start

Requirements: Linux x86-64, a C compiler (to build the compiler), and GNU binutils
(`as` and `ld`, needed when *using* the compiler).

```sh
make            # builds build/orb
make test       # 78 checks (every program is run with and without optimisation)
sudo make install        # optional: installs `orb` to /usr/local/bin

orb run examples/hello.orb
orb build examples/primes.orb -o primes && ./primes
orb check examples/bank.orb
orb new myproject        # creates myproject/main.orb
orb version
```

| command | meaning |
|---|---|
| `orb build f.orb [-o out]` | compile to an executable (default name: the file name without `.orb`) |
| `orb run f.orb [args...]` | compile to a temporary executable, run it, propagate its exit code |
| `orb check f.orb` | parse and type-check only |
| `orb new name` | create a new project directory |
| `orb version` | print the version |

Options: `--unchecked` (remove bounds / division checks), `--no-opt` (disable IR optimiser),
`-S` (print assembly), `--emit-ir` (print the intermediate representation),
`--std <dir>` (use an external standard library directory; useful when hacking on `std/`).
Extra `.o` / `.a` files after the source file are passed to the linker (for `extern fn`; there is no libc).

## The language in one page

```orb
use math;                       // modules: other .orb files or the standard library

struct Point { x: f64, y: f64 } // structs, value semantics
error OutOfRange;               // error values

fn dist(a: Point, b: Point) -> f64 {
    dx := a.x - b.x;            // `:=` declares with inferred type
    dy: f64 = a.y - b.y;        // or with an explicit type
    return math.sqrt(dx * dx + dy * dy);
}

fn checked_div(a: i64, b: i64) -> i64! {   // `T!` = value or error
    if b == 0 { fail OutOfRange; }
    return a / b;
}

fn main() {
    nums := [4, 8, 15, 16, 23, 42];        // arrays: [6]i64
    total := 0;
    for n in nums { total += n; }          // loops over arrays, slices, strings and ranges
    println("total = ", total);

    mid := nums[1..4];                     // slices: []i64 (pointer + length, bounds-checked)
    println(mid.len, " ", mid[0]);

    p := new(Point);                       // explicit heap allocation ...
    p.x = 3.0; p.y = 4.0;
    println(dist(*p, Point{ x: 0.0, y: 0.0 }));
    free(p);                               // ... and explicit release

    q := checked_div(10, 0) catch |e| {    // error handling: try / catch / fail
        println("failed: ", e);
        return;
    };
    println(q);
}
```

Full reference: [docs/LANGUAGE.md](docs/LANGUAGE.md). Standard library: [docs/STDLIB.md](docs/STDLIB.md).
Compiler internals and design decisions: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## What is implemented

* Static typing with inference for locals; `i8…i64`, `u8…u64`, `f64`, `bool`, `str`, pointers,
  arrays, slices, structs, enums, function pointers, error unions.
* Functions, modules (`use`, `pub`), constants (compile-time evaluated), globals.
* `if / else`, `while`, `loop`, `for` over ranges / arrays / slices / strings, `break`, `continue`.
* Pointers with arithmetic, explicit `new` / `make` / `free`, no garbage collector.
* Error handling: `T!`, `fail`, `try`, `catch`.
* Generic functions and generic structs (monomorphised, inferred from arguments).
* Method-call syntax for any function: `list.push(3)` is `push(&list, 3)`.
* Compiler: lexer → parser → type checker → IR → optimiser → register allocator → x86-64 assembly.
* Diagnostics with source line, caret and "did you mean" suggestions.
* Standard library: `core` (I/O, memory, panics), `math`, `strings`, `io`, `vec`.
* Tests: 78 checks + a differential fuzzer (`python3 tests/fuzz.py 500`).

## Performance (measured, reproducible)

`make bench` builds each program in `bench/` with Orb! and with `gcc -O0` / `gcc -O2` and
prints the best of three runs. Results on the development machine (1 core, x86-64):

| benchmark | orb | orb `--unchecked` | gcc -O0 | gcc -O2 |
|---|---:|---:|---:|---:|
| fib(40) (recursion) | 0.617 s | 0.623 s | 0.695 s | 0.133 s |
| sieve of 20 M | 0.135 s | 0.138 s | 0.237 s | 0.134 s |
| matmul 300×300 f64 | 0.030 s | 0.035 s | 0.057 s | 0.009 s |
| collatz < 3 M | 1.015 s | 1.000 s | 1.181 s | 0.587 s |
| mandelbrot 600×400 | 0.033 s | 0.033 s | 0.060 s | 0.027 s |

All outputs are checked against the C versions. **Orb! does not beat `gcc -O2`**: it is roughly
at the level of (or a little faster than) `gcc -O0`, and within 1× – 5× of `gcc -O2` on these
programs. The optimiser is deliberately small (see limitations); the numbers are reported as
they are.

## Known limitations (honest list)

* **Linux x86-64 only.** The compiler writes assembly text and relies on GNU `as` and `ld`.
* No debug information, no incremental compilation.
* The optimiser is basic: constant propagation, strength reduction, dead-code elimination,
  jump threading, linear-scan register allocation. No inlining, no loop optimisations, no
  vectorisation, no division by non-power-of-two constants via multiplication. This is why
  `fib` is ~4.6× slower than `gcc -O2` (which inlines recursion).
* Floating point is `f64` only. Printing uses six fractional digits (scientific notation for
  values ≥ 1e18).
* Integer overflow wraps (two's complement). Shifting by ≥ the bit width is undefined.
  Null-pointer dereference crashes with SIGSEGV; double `free` is undefined behaviour.
* Large local arrays live on the 8 MB stack; use globals or `make` for big buffers.
* Generic functions are type-checked per instantiation (like C++ templates); an uninstantiated
  generic is not checked.
* `new`, `make`, `sizeof` are reserved words (not usable as function names).
* No closures, interfaces, operator overloading, `switch`/`match`, `defer`, threads, or
  multiple return values (return a struct instead). Errors carry a code, not a payload.
* Strings are immutable byte slices (no Unicode-aware operations).
* Global initialisers must be compile-time constants.

## Repository layout

```
src/        the compiler (C11, no dependencies)
std/        standard library written in Orb! (embedded into the compiler at build time)
examples/   example programs        tests/    regression tests, fuzzer
bench/      benchmarks vs C         docs/     language reference, architecture
tools/      build helper (embeds std/)
```

© Orb Technologies
