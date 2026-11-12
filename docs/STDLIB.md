# Orb! standard library

The library is written in Orb! (`std/*.orb`) and embedded into the compiler binary, so an
installed `orb` needs no support files. Every program implicitly sees `core`; the other
modules are imported with `use`.

## core (implicit)
| function | description |
|---|---|
| `print(...)`, `println(...)` | built-ins: print values, `println` adds a newline. Output is buffered (64 KB) and flushed at exit, on panic, before reading stdin, and by `flush()` |
| `eprint(...)`, `eprintln(...)` | same, to standard error (unbuffered) |
| `flush()` | write buffered standard output now |
| `exit(code) -> never` | flush and terminate |
| `panic(msg: str) -> never` | print `panic: msg` to stderr, exit status 101 |
| `assert(cond: bool, msg: str)` | panic when `cond` is false |
| `args() -> []str` | command-line arguments (index 0 is the program) |
| `str_from(p: *u8, n: i64) -> str` | view raw memory as a string (no copy) |
| `mem_alloc(n) -> *u8`, `mem_zalloc(n) -> *u8` | raw / zeroed allocation |
| `new(T)`, `make(T, n)`, `free(p)` | typed allocation built-ins (zeroed memory) |

The allocator is a size-class free-list allocator (16-byte header, 16-byte alignment, 4 MB
chunks from `mmap`; blocks above 1 MB are mapped individually and returned to the OS on `free`).

## math
`PI`, `E`, `sqrt`, `abs<T>`, `min<T>`, `max<T>`, `clamp<T>`, `floor`, `ceil`, `round`,
`powi(x, n)`, `ipow(base, exp)`, `sin`, `cos`, `exp`, `ln`.
`sqrt` compiles to the `sqrtsd` instruction. `sin/cos/exp/ln` are series approximations
(about 1e-12 relative accuracy in normal ranges), not correctly-rounded libm functions.

## strings
`concat(a, b) -> str` (allocates), `starts_with`, `ends_with`, `find(s, needle) -> i64` (-1 if
absent), `trim`, `is_space`, `parse_i64(s) -> i64!` (error `ParseError`),
`from_i64(n) -> str` (allocates). Slicing (`s[a..b]`) and `==` are built into the language.

## io
`read_file(path) -> str!`, `write_file(path, data) -> !`, `read_stdin() -> str!`,
`read_line() -> str!`. Errors: `IoError`, `NotFound`. Results of the read functions are
heap-allocated; release them with `free`.

## vec
Generic growable array:
```orb
use vec;
v: vec.Vec<i64>;          // a zeroed Vec is a valid empty vector
v.push(3);                // push, get, set, pop, count, items, free_vec
println(v.get(0), v.len);
v.free_vec();
```
