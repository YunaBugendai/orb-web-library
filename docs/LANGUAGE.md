# Orb! language reference

## Lexical structure

* Comments: `// line` and `/* block */`.
* Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
* Integers: `123`, `1_000_000`, `0xFF`, `0b1010`. Floats: `1.5`, `2e-3`. Characters: `'a'`, `'\n'`
  (a character is a `u8`). Strings: `"text\n"` with escapes `\n \t \r \0 \\ \" \xHH`.
* Keywords: `fn struct enum const use pub if else while for in loop break continue return true
  false null as try catch extern error fail`.
* Statements end with `;`.

## Types

| type | description |
|---|---|
| `i8 i16 i32 i64`, `u8 u16 u32 u64` | integers (two's complement, overflow wraps) |
| `f64` | IEEE double |
| `bool` | `true` / `false` |
| `str` | immutable byte string: pointer + length (`s.ptr`, `s.len`) |
| `*T` | pointer; `null` is the null pointer |
| `[N]T` | array of constant length (`.len`) |
| `[]T` | slice: pointer + length (`.ptr`, `.len`), bounds-checked |
| `struct` | named record, value semantics |
| `enum` | named integer constants (`Color.Red`), convertible with `as` |
| `fn(A, B) -> R` | function pointer |
| `T!` and `!` | error union: a `T` or an error (`!` = no value) |
| `never` | return type of functions that never return (`panic`, `exit`) |
| `error` | the type of error values |

There are no implicit conversions between different types. Integer and float *literals*
adapt to the type expected by context (`x: u8 = 200`, `f: f64 = 2`). Use `as` for
conversions: `n as f64`, `x as u8`, `p as *u8`, `bytes as str`.

## Declarations

```orb
x := 10;                  // inferred
y: u8 = 7;                // explicit
z: [16]i64;               // zero-initialised (all variables are initialised)
const N = 100;            // compile-time constant (untyped: adapts like a literal)
const SCALE: f64 = 0.5;

counter: i64 = 0;         // global variable (initialiser must be constant)

struct Point { x: f64, y: f64 }
enum Color { Red, Green = 5, Blue }
error NotFound, Invalid;

pub fn add(a: i64, b: i64) -> i64 { return a + b; }
extern fn my_add(a: i64, b: i64) -> i64;   // symbol provided by an object linked in:  orb build main.orb helpers.o
```

Struct literals: `Point{ x: 1.0, y: 2.0 }` (omitted fields are zero). In `if`/`while`/`for`
headers a struct literal must be parenthesised.

## Expressions

Precedence (high → low): postfix (`()` `[]` `.`), unary (`- ! ~ * & try`), `as`,
`* / %`, `+ -`, `<< >>`, `&`, `^`, `|`, comparisons, `&&`, `||`, `catch`.
Bitwise operators bind tighter than comparisons (`a & 1 == 1` works as expected).

* `a / b`, `a % b` truncate toward zero. Division by zero is a runtime panic (unless `--unchecked`).
* Pointer arithmetic is scaled by the element size: `p + 1` points to the next element.
* `&x` takes an address, `*p` dereferences, `p.f` auto-dereferences pointers to structs.
* `a[i]` indexes arrays, slices, strings (yielding `u8`) and pointers; arrays and slices are
  bounds-checked. `a[lo..hi]`, `a[lo..]`, `a[..hi]`, `a[..]`, `a[lo..=hi]` make slices.
* `==` / `!=` on `str` compares contents.
* `sizeof(T)` (or of a variable), `new(T)` (zeroed `*T`), `make(T, n)` (zeroed `[]T`), `free(p)`.

## Statements

```orb
if cond { } else if other { } else { }
while cond { }
loop { if done { break; } }
for i in 0..n { }              // 0 .. n-1;  0..=n includes n.  `i` is read-only
for x in slice_or_array { }    // x is a copy of each element (also works on `str`)
for i, x in values { }         // index and element
```

## Functions

Arguments are passed by value. Slices and strings are two machine words; structs are copied.
Any function can be called with method syntax: `v.push(1)` means `push(&v, 1)` — the receiver
is taken by address or by value as the first parameter requires. A function must return on
every path (checked), unless it returns nothing.

## Error handling

```orb
error NotFound;

fn find(xs: []i64, v: i64) -> i64! {
    for i, x in xs { if x == v { return i; } }
    fail NotFound;
}

fn caller(xs: []i64) -> ! {
    i := try find(xs, 3);                 // propagate the error to the caller
    println(i);
}

fn main() {
    nums := [1, 2, 3];
    a := find(nums[..], 9) catch -1;              // default value
    b := find(data, 9) catch |e| { println(e); return; };   // block must diverge
}
```

* `T!` is a value of type `T` or an error; ignoring one is a compile error.
* `fail E;` returns error `E` (any `error` value, including a caught one).
* `try expr` yields the value or returns the error from the enclosing function (which must
  return `U!` or `!`).
* `expr catch default` or `expr catch |e| { ... }` (the block must end in `return`, `break`,
  `continue` or a call to a `never` function, except for `!` results).
* `main` may return nothing, `i64` (the exit code) or `!` (a non-zero error prints
  `error: Name` to stderr and exits with status 1).

## Generics

```orb
fn largest<T>(xs: []T) -> T { ... }          // type arguments are inferred from the call
struct Pair<A, B> { first: A, second: B }    // instantiate with Pair<i64, str>
make_zero<f64>()                             // or given explicitly
```
Generics are monomorphised: one machine-code copy per distinct instantiation.

## Modules

`use name;` loads `name.orb` from the directory of the importing file, or the standard library.
Members are accessed as `name.member` and must be declared `pub` to be visible outside their
file. Module names must not collide with type names (`str` is a type, the string helpers are
the module `strings`).

## Built-in functions

`print(...)`, `println(...)` print any number of integers, floats, bools, strings, pointers
(hex) and errors (by name); `eprint`/`eprintln` write to stderr. `free(p)` accepts a pointer or
slice. The `__syscall`, `__sqrt`, `__memcpy`, `__memzero`, `__errname` intrinsics exist for the
standard library.
