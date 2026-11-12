# Orb! compiler architecture

```
source.orb ─► lexer ─► parser ─► AST ─► sema ─► typed AST ─► lowering ─► IR ─► optimiser
                                                                              │
                          executable ◄── ld ◄── as ◄── x86-64 asm ◄── regalloc/emit
```

The compiler is ~5,500 lines of dependency-free C11 (`src/`). It is a real compiler: it
generates machine code through GNU `as`/`ld`. It never translates to another high-level language.

| file | role |
|---|---|
| `lexer.c`, `parser.c`, `ast.h` | tokens, recursive-descent parser, uniform AST node (cheap to clone for generics) |
| `types.c` | interned types, layout (size/alignment), type classes |
| `sema.c` | modules, symbols, constant evaluation, struct layout, generic instantiation |
| `check.c` | expression/statement type checking, implicit conversions, error unions, method-call sugar |
| `lower.c`, `ir.h` | AST → three-address IR over virtual registers |
| `opt.c` | IR optimiser |
| `emit.c` | liveness, linear-scan register allocation, x86-64 emission, data sections |
| `main.c` | command line driver |

## Key decisions

**No libc, runtime in Orb!.** `std/core.orb` implements printing, allocation and panics on top of
raw `syscall`s. The compiler only emits `_start`, which calls `rt_init`, `main`, `rt_exit`.
This keeps binaries tiny and the runtime fully inspectable.

**Value classes.** Every type is *scalar* (one register: ints, floats, pointers, bools, enums),
*pair* (two registers: `str`, slices and error unions with a scalar payload) or *memory*
(structs, arrays, large error unions). Pairs are passed as two arguments and returned in
`rax:rdx`, so slices and `i64!` results never touch memory in hot paths. Memory values are
passed by pointer to a caller-made copy and returned through a hidden first (`sret`) pointer.

**Calling convention.** System V registers (`rdi rsi rdx rcx r8 r9`, then the stack),
return in `rax`/`rdx`; floats travel as bit patterns in integer registers; `rbx r12–r15` are
callee-saved. Because the whole program is compiled together, Orb! functions can use it
without ABI compatibility concerns; `extern` functions follow it for integer/pointer arguments.

**Error unions** are ordinary values `{err: i64, payload}`; `try` is a compare and a return.
There are no exceptions, no unwinding tables and no runtime cost on the success path beyond one branch.

**Generics** are monomorphised by cloning the function AST, binding the type parameters as
local type aliases, and type-checking the clone. Type arguments are inferred by unifying the
parameter type expressions with argument types. Zero-cost: no boxing, no dictionaries.

**Safety checks are explicit and removable.** Array/slice bounds, slice ranges and integer
division by zero compile to a compare + branch to a `never` panic call; `--unchecked` removes
them. The panic calls are flagged `noreturn` so they do not pessimise register allocation.

## IR and optimisation

The IR is a linear list of three-address instructions over virtual registers
(`add v3, v1, 5`, `load`, `store`, `jcc`, `call`…), with explicit stack slots for aggregates and
address-taken locals. Scalars that never have their address taken live purely in registers;
slices and strings are kept as two registers.

Passes (`opt.c`, iterated to a fixed point): global propagation of single-definition constants,
operation fusion (`op t; mov x,t` → `op x`), strength reduction (multiply/divide/modulo by
powers of two → shifts and masks; `x - c` → `lea`), dead-code elimination, jump threading,
removal of unreachable code. Lowering also folds constants and fuses compares into branches,
and rotates loops so each iteration has one branch.

## Register allocation

`emit.c` builds basic blocks, solves liveness with bit-set data flow, derives one interval per
virtual register, and runs linear scan. Intervals that live across a call are restricted to the
callee-saved registers, others prefer caller-saved ones. Register hints coalesce moves and make
call arguments land directly in their argument registers. When registers run out, the interval
that ends last is spilled to the frame. `rax`, `rcx`, `rdx` (and `xmm0/xmm1`) are reserved as
scratch registers for instruction sequences, which keeps the emitter simple and obviously
correct. Parallel moves (call arguments, parameters) are sequenced with cycle breaking.

## Testing strategy

* `tests/ok`: programs with expected output, each run default, `--no-opt` and `--unchecked`.
* `tests/err`: each program must fail to compile with a specific message.
* `tests/panic`: runtime panics must print the expected message and exit with status 101.
* `tests/fuzz.py`: generates random programs (arithmetic, narrow casts, calls with up to 8
  arguments, loops, branches), computes the expected result in Python with exact 64-bit
  semantics and compares it to compiled output, with and without the optimiser.

## Not implemented / future work

An integrated assembler and linker (to drop the binutils dependency), inlining, loop
optimisations, division by constant via multiplication, additional targets (aarch64), debug
information, a package manager.
