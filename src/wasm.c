// Orb! compiler - WebAssembly (wasm32) backend.
//
// Consumes the same optimised three-address IR as the x86-64 backend and writes a .wasm binary
// directly (no external assembler, linker or LLVM).  Features used (all supported by every current
// browser and by Node >= 16): multi-value, sign-extension, non-trapping float->int, bulk memory.
//
// Design (see docs/WASM.md):
//   * Every Orb! integer / pointer value is an i64 local, every f64 is an f64 local.  Pointers stay
//     64-bit in memory (struct layout is identical to the native targets); addresses are wrapped to
//     i32 when they reach linear memory.
//   * Internal calling convention: all arguments are i64 (floats travel as bit patterns, exactly as
//     on x86-64), results are 0..2 i64 values (rax / rdx).
//   * Aggregates and address-taken locals live on a shadow stack in linear memory (global $sp).
//   * Control flow: the linear IR is turned into properly nested block / loop constructs when
//     possible; otherwise a `loop + br_table` dispatcher is used (always correct, slower).
//   * Function pointers are indices into one funcref table; address-taken functions get a thunk with a
//     uniform (i64 x n) -> (i64, i64) signature so that call_indirect is type-uniform.
//   * `extern fn` becomes an import (module "env", or "mod.name" in the symbol string).
//     `pub fn` of the entry file and `pub fn export_*` anywhere become exports with natural wasm types.
#include "sema.h"
#include "ir.h"
#include <math.h>
#include <errno.h>

int g_wasm_stack_kb = 1024;      // size of the shadow stack
bool g_wasm_force_dispatch = false;

// ------------------------------------------------------------------ byte buffer helpers
typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void bput(Buf *b, int c) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->d = xrealloc(b->d, b->cap); }
    b->d[b->n++] = (uint8_t)c;
}
static void bcat(Buf *b, const void *p, size_t n) { for (size_t i = 0; i < n; i++) bput(b, ((const uint8_t *)p)[i]); }
static void uleb(Buf *b, uint64_t v) { do { int c = v & 0x7f; v >>= 7; if (v) c |= 0x80; bput(b, c); } while (v); }
static void sleb(Buf *b, int64_t v) {
    for (;;) {
        int c = v & 0x7f; v >>= 7;
        bool done = (v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40));
        bput(b, done ? c : (c | 0x80));
        if (done) break;
    }
}
static void bname(Buf *b, const char *s) { uleb(b, strlen(s)); bcat(b, s, strlen(s)); }
static void section(Buf *out, int id, Buf *body) { bput(out, id); uleb(out, body->n); bcat(out, body->d, body->n); }

enum { VI32 = 0x7F, VI64 = 0x7E, VF64 = 0x7C };

// ------------------------------------------------------------------ signatures
typedef struct { uint8_t p[600]; int np; uint8_t r[4]; int nr; } Sig;
static VEC(Sig *) g_sigs;
static int sig_id(Sig *s) {
    for (int i = 0; i < g_sigs.n; i++) {
        Sig *t = g_sigs.d[i];
        if (t->np == s->np && t->nr == s->nr && !memcmp(t->p, s->p, (size_t)s->np) && !memcmp(t->r, s->r, (size_t)s->nr)) return i;
    }
    Sig *c = xcalloc(sizeof(Sig)); *c = *s;
    vpush(g_sigs, c);
    return g_sigs.n - 1;
}
static int sig_uniform(int nparams) {   // (i64 x n) -> (i64, i64)
    Sig s; memset(&s, 0, sizeof s);
    if (nparams > 590) die("wasm: function with too many parameters (%d)", nparams);
    for (int i = 0; i < nparams; i++) s.p[s.np++] = VI64;
    s.r[0] = s.r[1] = VI64; s.nr = 2;
    return sig_id(&s);
}
static int sig_plain(int nparams, int nres) {
    Sig s; memset(&s, 0, sizeof s);
    for (int i = 0; i < nparams; i++) s.p[s.np++] = VI64;
    for (int i = 0; i < nres; i++) s.r[s.nr++] = VI64;
    return sig_id(&s);
}

// ------------------------------------------------------------------ external (host) signatures
typedef struct { uint8_t wt; bool sg; } ESlot;
typedef struct { ESlot p[300]; int np; ESlot r[2]; int nr; bool ok; const char *why; } ESig;

static bool eslot_for(Type *t, ESlot *o) {
    o->sg = false;
    switch (t->kind) {
    case TY_F64: o->wt = VF64; return true;
    case TY_I64: case TY_U64: o->wt = VI64; return true;
    case TY_I8: case TY_I16: case TY_I32: o->wt = VI32; o->sg = true; return true;
    case TY_BOOL: case TY_U8: case TY_U16: case TY_U32: case TY_PTR: case TY_FUNC: case TY_ENUM: case TY_ERRCODE:
        o->wt = VI32; return true;
    default: return false;
    }
}
static void ext_sig(Func *f, ESig *s) {
    memset(s, 0, sizeof *s);
    s->ok = true;
    Type *ft = f->type;
    for (int i = 0; i < ft->nparams; i++) {
        Type *t = ft->params[i];
        TypeClass tc = type_class(t);
        if (tc == TC_SCALAR) {
            if (!eslot_for(t, &s->p[s->np])) { s->ok = false; s->why = "unsupported parameter type"; return; }
            s->np++;
        } else if (tc == TC_PAIR && (t->kind == TY_STR || t->kind == TY_SLICE)) {
            s->p[s->np].wt = VI32; s->p[s->np + 1].wt = VI32; s->np += 2;
        } else { s->ok = false; s->why = "parameters must be scalars, str or slices"; return; }
        if (s->np > 280) { s->ok = false; s->why = "too many parameters"; return; }
    }
    Type *rt = ft->ret;
    if (rt->kind == TY_VOID || rt->kind == TY_NEVER) return;
    TypeClass rc = type_class(rt);
    if (rc == TC_SCALAR) { if (!eslot_for(rt, &s->r[0])) { s->ok = false; s->why = "unsupported result type"; return; } s->nr = 1; }
    else if (rc == TC_PAIR && (rt->kind == TY_STR || rt->kind == TY_SLICE)) { s->r[0].wt = VI32; s->r[1].wt = VI32; s->nr = 2; }
    else { s->ok = false; s->why = "result must be a scalar, str or slice"; }
}

// ------------------------------------------------------------------ module state
typedef struct { Func *f; int sig, idx; ESig es; } Import;
typedef struct { FnIR *ir; int idx, sig, nres; } WFn;
typedef struct { int sig; Buf body; char *name; } Extra;     // thunks, export wrappers, _start

static FnIR **by_func;
static int *w_index;            // func id -> wasm function index (defined or import), -1 if none
static int *w_nres;             // func id -> number of i64 results (defined functions)
static int *w_tab;              // func id -> table index (0 = not address-taken)
static bool *w_addr, *w_reach;
static VEC(WFn) g_wfns;
static VEC(Import) g_imps;
static VEC(Extra) g_extras;
static int n_imports;
static int64_t *isym_addr;      // memory address of data / string / errtab symbols
static bool *isym_used;
static int stack_lo, stack_top;
static int panic_fn = -1, str_stackov;   // function index of core `panic` and address of its message
static int g_export_sp_global = 0;

typedef struct { int addr; Buf bytes; } DataSeg;
static VEC(DataSeg) g_segs;

static int align_up(int v, int a) { return (v + a - 1) / a * a; }

// ------------------------------------------------------------------ per-function emission
static Buf *cb;                 // current code buffer
static FnIR *cir;
static int c_np, c_nv, L_pc, L_fp, L_t64, L_tf, c_frame, c_nres;
static int *c_slot_off;
static bool c_has_frame;

static void op(int o) { bput(cb, o); }
static void opu(int o, uint64_t v) { bput(cb, o); uleb(cb, v); }
static void i64c(int64_t v) { bput(cb, 0x42); sleb(cb, v); }
static void i32c(int32_t v) { bput(cb, 0x41); sleb(cb, v); }
static void f64c(int64_t bits) { bput(cb, 0x44); for (int i = 0; i < 8; i++) bput(cb, (int)((uint64_t)bits >> (8 * i)) & 0xff); }
static void lget(int i) { opu(0x20, (uint64_t)i); }
static void lset(int i) { opu(0x21, (uint64_t)i); }
static int vloc(int v) { return c_np + v; }
static char vcls(int v) { return cir->cls.d[v] == 1 ? 'f' : 'i'; }

static int64_t sym_value(int isym) {
    ISym *s = &g_isyms.d[isym];
    if (s->kind == IS_FUNC) {
        if (s->func->is_extern) die("wasm: cannot take the address of the extern function '%s'", s->func->decl->name);
        return w_tab[s->func->id];
    }
    return isym_addr[isym];
}

static void get(Opnd o, char want) {
    switch (o.k) {
    case O_NONE: if (want == 'f') f64c(0); else i64c(0); return;
    case O_IMM: if (want == 'f') f64c(o.v); else i64c(o.v); return;
    case O_REG: {
        lget(vloc((int)o.v));
        char have = vcls((int)o.v);
        if (have == 'f' && want == 'i') op(0xBD);
        else if (have == 'i' && want == 'f') op(0xBF);
        return;
    }
    case O_SLOT:
        lget(L_fp); op(0xAD); i64c(c_slot_off[o.v]); op(0x7C);
        if (want == 'f') op(0xBF);
        return;
    case O_GLOB:
        i64c(sym_value((int)o.v));
        if (want == 'f') op(0xBF);
        return;
    }
}
static void set(int v, char have) {
    char want = vcls(v);
    if (have == 'f' && want == 'i') op(0xBD);
    else if (have == 'i' && want == 'f') op(0xBF);
    lset(vloc(v));
}
static void drop(void) { op(0x1A); }

// pushes an i32 address; *memoff receives the constant offset for the memarg
static void addr32(Ins *i, uint32_t *memoff) {
    int64_t off = i->off;
    Opnd base = i->a, idx = i->b;
    bool have_base = true;
    if (base.k == O_SLOT) { off += c_slot_off[base.v]; lget(L_fp); }
    else if (base.k == O_GLOB) {
        off += sym_value((int)base.v);
        if (idx.k == O_REG) have_base = false; else i32c(0);
    } else { get(base, 'i'); op(0xA7); }
    if (idx.k == O_REG) {
        get(idx, 'i'); op(0xA7);
        if (i->scale != 1) { i32c(i->scale); op(0x6C); }
        if (have_base) op(0x6A);
    }
    if (off < 0 || off > 0x7fffffffLL) { i32c((int32_t)off); op(0x6A); *memoff = 0; }
    else *memoff = (uint32_t)off;
}
static void memarg(int log2a, uint32_t off) { uleb(cb, (uint64_t)log2a); uleb(cb, off); }

static void cmp_op(int cc) {   // i64 compare
    static const int tab[10] = { 0x51, 0x52, 0x53, 0x57, 0x55, 0x59, 0x54, 0x58, 0x56, 0x5A };
    op(tab[cc]);
}

static void emit_load(Ins *i) {
    uint32_t mo;
    addr32(i, &mo);
    if (cir->cls.d[i->dst] == 1 && i->w == 8) { op(0x2B); memarg(3, mo); set(i->dst, 'f'); return; }
    switch (i->w) {
    case 1: op(i->sg ? 0x30 : 0x31); memarg(0, mo); break;
    case 2: op(i->sg ? 0x32 : 0x33); memarg(1, mo); break;
    case 4: op(i->sg ? 0x34 : 0x35); memarg(2, mo); break;
    default: op(0x29); memarg(3, mo); break;
    }
    set(i->dst, 'i');
}
static void emit_store(Ins *i) {
    uint32_t mo;
    addr32(i, &mo);
    Opnd v = i->c;
    if (v.k == O_REG && vcls((int)v.v) == 'f' && i->w == 8) { get(v, 'f'); op(0x39); memarg(3, mo); return; }
    get(v, 'i');
    switch (i->w) {
    case 1: op(0x3C); memarg(0, mo); break;
    case 2: op(0x3D); memarg(1, mo); break;
    case 4: op(0x3E); memarg(2, mo); break;
    default: op(0x37); memarg(3, mo); break;
    }
}
static void emit_lea(Ins *i) {
    int64_t off = i->off;
    Opnd base = i->a, idx = i->b;
    bool have = false;
    if (base.k == O_GLOB) off += sym_value((int)base.v);
    else if (base.k == O_SLOT) { lget(L_fp); op(0xAD); off += c_slot_off[base.v]; have = true; }
    else { get(base, 'i'); have = true; }
    if (idx.k == O_REG) {
        get(idx, 'i');
        if (i->scale != 1) { i64c(i->scale); op(0x7E); }
        if (have) op(0x7C);
        have = true;
    }
    if (!have) i64c(off);
    else if (off != 0) { i64c(off); op(0x7C); }
    set(i->dst, 'i');
}

// ---- calls
static void restore_sp(void) {
    if (!c_has_frame) return;
    lget(L_fp); i32c(c_frame); op(0x6A); opu(0x24, (uint64_t)g_export_sp_global);
}

static void push_results_to_dsts(Ins *i, int nres) {
    // results are on the stack (i64 each); the last result is on top
    if (nres >= 2) { if (i->dst2 >= 0) set(i->dst2, 'i'); else drop(); }
    if (nres >= 1) { if (i->dst >= 0) set(i->dst, 'i'); else drop(); }
}

static void emit_call(Ins *i) {
    bool indirect = i->a.k == O_REG;
    if (!indirect) {
        ISym *s = &g_isyms.d[i->a.v];
        Func *f = s->func;
        if (f->is_extern) {
            Import *im = NULL;
            for (int k = 0; k < g_imps.n; k++) if (g_imps.d[k].f == f || !strcmp(g_imps.d[k].f->asm_name, f->asm_name)) { im = &g_imps.d[k]; break; }
            ESig *es = &im->es;
            if (i->nargs != es->np) die("wasm: internal error: argument count mismatch calling '%s'", f->asm_name);
            for (int k = 0; k < es->np; k++) {
                if (es->p[k].wt == VF64) get(i->args[k], 'f');
                else { get(i->args[k], 'i'); if (es->p[k].wt == VI32) op(0xA7); }
            }
            opu(0x10, (uint64_t)im->idx);
            if (i->noret) { op(0x00); return; }
            // results
            for (int k = es->nr - 1; k >= 0; k--) {
                int dstv = k == 0 ? i->dst : i->dst2;
                if (dstv < 0) { drop(); continue; }
                if (es->r[k].wt == VF64) set(dstv, 'f');
                else { if (es->r[k].wt == VI32) op(es->r[k].sg ? 0xAC : 0xAD); set(dstv, 'i'); }
            }
            return;
        }
        for (int k = 0; k < i->nargs; k++) get(i->args[k], 'i');
        opu(0x10, (uint64_t)w_index[f->id]);
        if (i->noret) { op(0x00); return; }
        push_results_to_dsts(i, w_nres[f->id]);
        return;
    }
    for (int k = 0; k < i->nargs; k++) get(i->args[k], 'i');
    get(i->a, 'i'); op(0xA7);
    op(0x11); uleb(cb, (uint64_t)sig_uniform(i->nargs)); uleb(cb, 0);
    if (i->noret) { op(0x00); return; }
    push_results_to_dsts(i, 2);
}

static void emit_ret(Ins *i) {
    if (c_nres >= 1) get(i->a, 'i');
    if (c_nres >= 2) get(i->b, 'i');
    restore_sp();
    op(0x0F);
}

// one non-control instruction
static void emit_plain(Ins *i) {
    switch (i->op) {
    case OP_NOP: case OP_PARAM: case OP_LABEL: break;
    case OP_MOV: { char w = vcls(i->dst); get(i->a, w); set(i->dst, w); break; }
    case OP_FCONST: f64c(i->a.v); set(i->dst, 'f'); break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
    case OP_DIV: case OP_UDIV: case OP_MOD: case OP_UMOD: case OP_SHL: case OP_SHR: case OP_SAR: {
        static const int tab[OP__COUNT] = { [OP_ADD] = 0x7C, [OP_SUB] = 0x7D, [OP_MUL] = 0x7E, [OP_DIV] = 0x7F, [OP_UDIV] = 0x80,
            [OP_MOD] = 0x81, [OP_UMOD] = 0x82, [OP_AND] = 0x83, [OP_OR] = 0x84, [OP_XOR] = 0x85, [OP_SHL] = 0x86, [OP_SAR] = 0x87, [OP_SHR] = 0x88 };
        get(i->a, 'i'); get(i->b, 'i'); op(tab[i->op]); set(i->dst, 'i');
        break;
    }
    case OP_NEG: i64c(0); get(i->a, 'i'); op(0x7D); set(i->dst, 'i'); break;
    case OP_NOT: get(i->a, 'i'); i64c(-1); op(0x85); set(i->dst, 'i'); break;
    case OP_SEXT:
        get(i->a, 'i');
        if (i->w == 1) op(0xC2); else if (i->w == 2) op(0xC3); else if (i->w == 4) op(0xC4);
        set(i->dst, 'i');
        break;
    case OP_ZEXT:
        get(i->a, 'i');
        if (i->w == 1) { i64c(0xff); op(0x83); } else if (i->w == 2) { i64c(0xffff); op(0x83); } else if (i->w == 4) { i64c(0xffffffffLL); op(0x83); }
        set(i->dst, 'i');
        break;
    case OP_SET: get(i->a, 'i'); get(i->b, 'i'); cmp_op(i->cc); op(0xAD); set(i->dst, 'i'); break;
    case OP_FSET: {
        static const int tab[10] = { 0x61, 0x62, 0x63, 0x65, 0x64, 0x66, 0, 0, 0, 0 };
        get(i->a, 'f'); get(i->b, 'f'); op(tab[i->cc]); op(0xAD); set(i->dst, 'i');
        break;
    }
    case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
        static const int tab[OP__COUNT] = { [OP_FADD] = 0xA0, [OP_FSUB] = 0xA1, [OP_FMUL] = 0xA2, [OP_FDIV] = 0xA3 };
        get(i->a, 'f'); get(i->b, 'f'); op(tab[i->op]); set(i->dst, 'f');
        break;
    }
    case OP_FNEG: get(i->a, 'f'); op(0x9A); set(i->dst, 'f'); break;
    case OP_FSQRT: get(i->a, 'f'); op(0x9F); set(i->dst, 'f'); break;
    case OP_I2F: get(i->a, 'i'); op(0xB9); set(i->dst, 'f'); break;
    case OP_U2F: get(i->a, 'i'); op(0xBA); set(i->dst, 'f'); break;
    case OP_F2I: get(i->a, 'f'); op(0xFC); uleb(cb, 6); set(i->dst, 'i'); break;
    case OP_F2U:
        get(i->a, 'f'); lset(L_tf);
        lget(L_tf); f64c(0x43E0000000000000LL); op(0x66);     // x >= 2^63 ?
        op(0x04); op(VI64);
        lget(L_tf); f64c(0x43E0000000000000LL); op(0xA1); op(0xFC); uleb(cb, 6); i64c((int64_t)0x8000000000000000ULL); op(0x85);
        op(0x05);
        lget(L_tf); op(0xFC); uleb(cb, 6);
        op(0x0B);
        set(i->dst, 'i');
        break;
    case OP_LOAD: emit_load(i); break;
    case OP_STORE: emit_store(i); break;
    case OP_LEA: emit_lea(i); break;
    case OP_CALL: emit_call(i); break;
    case OP_RET: emit_ret(i); break;
    case OP_SYSCALL: die("wasm: __syscall is not available on this target (use `extern fn` host imports; see docs/WASM.md)");
        break;
    case OP_MEMCPY:
        get(i->args[0], 'i'); op(0xA7); get(i->args[1], 'i'); op(0xA7); get(i->args[2], 'i'); op(0xA7);
        op(0xFC); uleb(cb, 10); op(0); op(0);
        break;
    case OP_MEMZERO:
        get(i->args[0], 'i'); op(0xA7); i32c(0); get(i->args[1], 'i'); op(0xA7);
        op(0xFC); uleb(cb, 11); op(0);
        break;
    case OP_MEMGROW: get(i->a, 'i'); op(0xA7); op(0x40); op(0); op(0xAC); set(i->dst, 'i'); break;
    case OP_MEMSIZE: op(0x3F); op(0); op(0xAD); set(i->dst, 'i'); break;
    default: die("wasm: internal error: unhandled IR op %d", i->op);
    }
}

// ------------------------------------------------------------------ control flow
typedef struct { int start, end; } BB;
typedef struct { int kind, s, e; } Cons;      // kind: 0 block (target e), 1 loop (header s)

static BB *bbs; static int nbb;
static int *lab_bb;

static bool is_term(Ins *i) { return i->op == OP_JMP || i->op == OP_JCC || i->op == OP_RET || (i->op == OP_CALL && i->noret); }

static void build_blocks(int first) {
    int n = cir->ins.n;
    bool *lead = xcalloc((size_t)n + 2);
    if (first < n) lead[first] = true;
    for (int k = first; k < n; k++) {
        Ins *i = &cir->ins.d[k];
        if (i->op == OP_LABEL) lead[k] = true;
        if (is_term(i) && k + 1 < n) lead[k + 1] = true;
    }
    bbs = xcalloc(sizeof(BB) * (size_t)(n + 1)); nbb = 0;
    lab_bb = xcalloc(sizeof(int) * (size_t)(cir->nlabels + 2));
    for (int k = first; k < n; k++) {
        if (lead[k]) { bbs[nbb].start = k; nbb++; }
        bbs[nbb - 1].end = k;
    }
    for (int b = 0; b < nbb; b++)
        for (int k = bbs[b].start; k <= bbs[b].end; k++)
            if (cir->ins.d[k].op == OP_LABEL) lab_bb[cir->ins.d[k].label] = b;
    free(lead);
}

static Ins *bb_term(int b) {
    Ins *l = &cir->ins.d[bbs[b].end];
    return is_term(l) ? l : NULL;
}

static void emit_block_body(int b) {
    int end = bbs[b].end;
    Ins *t = bb_term(b);
    if (t && (t->op == OP_JMP || t->op == OP_JCC)) end--;
    for (int k = bbs[b].start; k <= end; k++) emit_plain(&cir->ins.d[k]);
}

static VEC(Cons) open_stack;
static int find_depth(int kind, int key) {
    for (int k = open_stack.n - 1; k >= 0; k--) {
        Cons *c = &open_stack.d[k];
        if (c->kind == kind && (kind == 0 ? c->e : c->s) == key) return open_stack.n - 1 - k;
    }
    return -1;
}

static bool emit_structured(void) {
    int n = nbb;
    int *minsrc = xcalloc(sizeof(int) * (size_t)(n + 1)), *maxback = xcalloc(sizeof(int) * (size_t)(n + 1));
    for (int b = 0; b <= n; b++) { minsrc[b] = -1; maxback[b] = -1; }
    for (int b = 0; b < n; b++) {
        Ins *t = bb_term(b);
        if (!t || (t->op != OP_JMP && t->op != OP_JCC)) continue;
        int tg = lab_bb[t->label];
        if (tg == b + 1) continue;                              // falls through anyway
        if (tg > b) { if (minsrc[tg] < 0 || b < minsrc[tg]) minsrc[tg] = b; }
        else { if (b > maxback[tg]) maxback[tg] = b; }
    }
    VEC(Cons) cs = {0};
    for (int b = 0; b < n; b++) {
        if (minsrc[b] >= 0) { Cons c = { 0, minsrc[b], b }; vpush(cs, c); }
        if (maxback[b] >= 0) { Cons c = { 1, b, maxback[b] + 1 }; vpush(cs, c); }
    }
    // make the family properly nested by moving the start of forward blocks outwards
    for (int iter = 0; iter < 64; iter++) {
        bool changed = false;
        for (int a = 0; a < cs.n; a++) for (int b = 0; b < cs.n; b++) {
            Cons *A = &cs.d[a], *B = &cs.d[b];
            if (A->s < B->s && B->s < A->e && A->e < B->e) {
                if (B->kind == 0) { B->s = A->s; changed = true; }
                else { free(minsrc); free(maxback); free(cs.d); return false; }
            }
        }
        if (!changed) goto nested;
    }
    free(minsrc); free(maxback); free(cs.d);
    return false;
nested:;
    open_stack.n = 0;
    for (int b = 0; b <= n; b++) {
        // close constructs ending here
        while (open_stack.n > 0 && open_stack.d[open_stack.n - 1].e == b) { op(0x0B); open_stack.n--; }
        for (int k = 0; k < open_stack.n; k++) if (open_stack.d[k].e == b) { free(minsrc); free(maxback); free(cs.d); return false; }
        if (b == n) break;
        // open constructs starting here: outermost (largest end) first; block outside loop on ties
        for (;;) {
            int best = -1;
            for (int k = 0; k < cs.n; k++) {
                if (cs.d[k].s != b || cs.d[k].kind < 0) continue;
                if (best < 0 || cs.d[k].e > cs.d[best].e || (cs.d[k].e == cs.d[best].e && cs.d[k].kind < cs.d[best].kind)) best = k;
            }
            if (best < 0) break;
            Cons c = cs.d[best];
            cs.d[best].kind = -1;
            op(c.kind == 0 ? 0x02 : 0x03); op(0x40);
            vpush(open_stack, c);
        }
        emit_block_body(b);
        Ins *t = bb_term(b);
        if (t && (t->op == OP_JMP || t->op == OP_JCC)) {
            int tg = lab_bb[t->label];
            if (tg != b + 1) {
                int d = tg > b ? find_depth(0, tg) : find_depth(1, tg);
                if (d < 0) { free(minsrc); free(maxback); free(cs.d); return false; }
                if (t->op == OP_JCC) { get(t->a, 'i'); get(t->b, 'i'); cmp_op(t->cc); opu(0x0D, (uint64_t)d); }
                else opu(0x0C, (uint64_t)d);
            }
        } else if (t && t->op == OP_RET) {
            emit_plain(t);
        }
        // RET terminators were emitted by emit_plain above; noret calls inside the body already emitted `unreachable`
    }
    free(minsrc); free(maxback); free(cs.d);
    return true;
}

static void emit_dispatch(void) {
    int n = nbb;
    op(0x03); op(0x40);                      // loop $dispatch
    for (int k = 0; k < n; k++) { op(0x02); op(0x40); }
    lget(L_pc);
    op(0x0E); uleb(cb, (uint64_t)n);
    for (int k = 0; k < n; k++) uleb(cb, (uint64_t)k);
    uleb(cb, (uint64_t)(n - 1));
    for (int b = 0; b < n; b++) {
        op(0x0B);                            // end of block b: block b's code follows
        emit_block_body(b);
        int depth_loop = n - 1 - b;           // blocks still open around this code
        Ins *t = bb_term(b);
        if (t && (t->op == OP_JMP || t->op == OP_JCC)) {
            int tg = lab_bb[t->label];
            if (t->op == OP_JCC) {
                get(t->a, 'i'); get(t->b, 'i'); cmp_op(t->cc);
                op(0x04); op(0x40);
                i32c(tg); lset(L_pc); opu(0x0C, (uint64_t)depth_loop + 1);
                op(0x0B);
            } else if (tg != b + 1) {
                i32c(tg); lset(L_pc); opu(0x0C, (uint64_t)depth_loop);
            }
        } else if (t && t->op == OP_RET) emit_plain(t);
    }
    op(0x0B);                                // end loop
}

// ------------------------------------------------------------------ one IR function
static void emit_function(FnIR *f, int nres, Buf *out) {
    cir = f; c_nres = nres;
    Buf code = {0};
    cb = &code;
    c_np = f->nparams;
    c_nv = f->cls.n;
    L_pc = c_np + c_nv; L_fp = L_pc + 1; L_t64 = L_fp + 1; L_tf = L_t64 + 1;
    // slot layout
    int ns = f->slots.n;
    c_slot_off = xcalloc(sizeof(int) * (size_t)(ns + 1));
    int cur = 0;
    for (int s = 0; s < ns; s++) {
        int al = f->slots.d[s].align; if (al < 1) al = 1; if (al > 16) al = 16;
        cur = align_up(cur, al);
        c_slot_off[s] = cur;
        cur += f->slots.d[s].size;
    }
    c_frame = align_up(cur, 16);
    c_has_frame = false;
    for (int k = 0; k < f->ins.n && !c_has_frame; k++) {
        Ins *i = &f->ins.d[k];
        if (i->a.k == O_SLOT || i->b.k == O_SLOT || i->c.k == O_SLOT) c_has_frame = true;
        for (int q = 0; q < i->nargs; q++) if (i->args[q].k == O_SLOT) c_has_frame = true;
    }
    if (c_has_frame && c_frame == 0) c_frame = 16;
    // prologue
    if (c_has_frame) {
        opu(0x23, (uint64_t)g_export_sp_global); i32c(c_frame); op(0x6B); lset(L_fp);
        lget(L_fp); i32c(stack_lo + 4096); op(0x49);
        op(0x04); op(0x40);
        if (panic_fn >= 0) { i64c(str_stackov); i64c(14); opu(0x10, (uint64_t)panic_fn); }
        op(0x00);
        op(0x0B);
        lget(L_fp); opu(0x24, (uint64_t)g_export_sp_global);
    }
    int np = 0;
    while (np < f->ins.n && f->ins.d[np].op == OP_PARAM) np++;
    for (int k = 0; k < np; k++) {
        Ins *i = &f->ins.d[k];
        lget((int)i->a.v);
        if (vcls(i->dst) == 'f') op(0xBF);
        lset(vloc(i->dst));
    }
    build_blocks(np);
    bool done = false;
    if (nbb == 0) done = true;
    else if (nbb == 1) { emit_block_body(0); Ins *t = bb_term(0); if (t && t->op == OP_RET) emit_plain(t); done = true; }
    if (!done && !g_wasm_force_dispatch) {
        size_t mark = code.n;
        if (emit_structured()) done = true; else code.n = mark;
    }
    if (!done) emit_dispatch();
    op(0x00);                                // end of function body is unreachable
    op(0x0B);
    // assemble: locals header + code
    Buf body = {0};
    // local groups: vregs in order, then pc/fp (i32), t64 (i64), tf (f64)
    VEC(uint8_t) types = {0};
    for (int v = 0; v < c_nv; v++) vpush(types, f->cls.d[v] == 1 ? VF64 : VI64);
    vpush(types, VI32); vpush(types, VI32); vpush(types, VI64); vpush(types, VF64);
    int ngroups = 0;
    for (int k = 0; k < types.n; k++) if (k == 0 || types.d[k] != types.d[k - 1]) ngroups++;
    uleb(&body, (uint64_t)ngroups);
    for (int k = 0; k < types.n;) {
        int j = k; while (j < types.n && types.d[j] == types.d[k]) j++;
        uleb(&body, (uint64_t)(j - k)); bput(&body, types.d[k]);
        k = j;
    }
    bcat(&body, code.d, code.n);
    uleb(out, body.n); bcat(out, body.d, body.n);
    free(code.d); free(body.d); free(types.d); free(c_slot_off); free(bbs); free(lab_bb);
}

// ------------------------------------------------------------------ helper functions (thunks, exports, _start)
static void new_extra(int sig, const char *name, Buf *body_with_locals) {
    Extra e; e.sig = sig; e.name = xstrdup(name); e.body = *body_with_locals;
    vpush(g_extras, e);
}
static int extra_index(int k) { return n_imports + g_wfns.n + k; }

// results of a defined function, in terms of how many i64 results it has
static int count_results(FnIR *f) {
    int r = 0;
    for (int k = 0; k < f->ins.n; k++) {
        Ins *i = &f->ins.d[k];
        if (i->op != OP_RET) continue;
        int c = (i->a.k != O_NONE) + (i->b.k != O_NONE);
        if (i->b.k != O_NONE) c = 2;
        if (c > r) r = c;
    }
    return r;
}

static void conv_i32_to_i64(ESlot s) { if (s.wt == VI32) { bput(cb, s.sg ? 0xAC : 0xAD); } }

// export wrapper: natural wasm types -> i64 ABI
static void make_export_wrapper(Func *f, int target_idx, int target_nres, ESig *es, const char *name) {
    Buf code = {0};
    cb = &code;
    for (int k = 0; k < es->np; k++) {
        lget(k);
        if (es->p[k].wt == VF64) op(0xBD);
        else conv_i32_to_i64(es->p[k]);
    }
    // callee parameters: pairs are already two entries, matching the flattened ABI
    opu(0x10, (uint64_t)target_idx);
    int tmp = es->np;                        // i64 temp local
    if (es->nr == 0) { for (int k = 0; k < target_nres; k++) drop(); }
    else if (es->nr == 1) {
        if (es->r[0].wt == VF64) op(0xBF);
        else if (es->r[0].wt == VI32) op(0xA7);
    } else {
        lset(tmp); op(0xA7); lget(tmp); op(0xA7);
    }
    op(0x0B);
    Buf body = {0};
    uleb(&body, 1); uleb(&body, 1); bput(&body, VI64);
    bcat(&body, code.d, code.n);
    Buf withsize = {0};
    uleb(&withsize, body.n); bcat(&withsize, body.d, body.n);
    Sig s; memset(&s, 0, sizeof s);
    for (int k = 0; k < es->np; k++) s.p[s.np++] = es->p[k].wt;
    for (int k = 0; k < es->nr; k++) s.r[s.nr++] = es->r[k].wt;
    new_extra(sig_id(&s), name, &withsize);
    free(code.d); free(body.d);
}

// ------------------------------------------------------------------ driver
static void mark_ops(Ins *i, void (*fn)(Opnd *, Ins *, bool)) {
    Opnd *ops[3] = { &i->a, &i->b, &i->c };
    for (int q = 0; q < 3; q++) fn(ops[q], i, q == 0 && i->op == OP_CALL);
    for (int q = 0; q < i->nargs; q++) fn(&i->args[q], i, false);
}

static VEC(FnIR *) work;
static void reach_func(Func *f) {
    if (w_reach[f->id]) return;
    w_reach[f->id] = true;
    if (f->is_extern) return;
    if (by_func[f->id]) vpush(work, by_func[f->id]);
}
static void reach_opnd(Opnd *o, Ins *i, bool is_call_target) {
    if (o->k != O_GLOB) return;
    ISym *s = &g_isyms.d[o->v];
    if (s->kind == IS_FUNC) { reach_func(s->func); if (!is_call_target) w_addr[s->func->id] = true; }
    else isym_used[o->v] = true;
}

static int find_str_isym(const char *b, int len) {
    for (int k = 0; k < g_isyms.n; k++)
        if (g_isyms.d[k].kind == IS_STR && g_isyms.d[k].len == len && !memcmp(g_isyms.d[k].bytes, b, (size_t)len)) return k;
    return -1;
}

static bool is_exported_user_fn(Func *f) {
    if (f->is_extern || !f->decl || !f->decl->pub || f == g_prog.main_fn || f->decl->gen.n > 0) return false;
    if (f->mod == g_prog.main_fn->mod) return true;
    return strncmp(f->decl->name, "export_", 7) == 0;
}

void wasm_emit(const char *path) {
    int nf = g_prog.funcs.n + 1;
    by_func = xcalloc(sizeof(FnIR *) * (size_t)nf);
    for (int i = 0; i < g_irs.n; i++) by_func[g_irs.d[i]->f->id] = g_irs.d[i];
    w_index = xcalloc(sizeof(int) * (size_t)nf); for (int i = 0; i < nf; i++) w_index[i] = -1;
    w_nres = xcalloc(sizeof(int) * (size_t)nf);
    w_tab = xcalloc(sizeof(int) * (size_t)nf);
    w_addr = xcalloc((size_t)nf); w_reach = xcalloc((size_t)nf);
    isym_used = xcalloc((size_t)g_isyms.n + 1);
    isym_addr = xcalloc(sizeof(int64_t) * (size_t)(g_isyms.n + 1));

    // ---- roots
    Func *fn_init = core_sym("rt_init")->func, *fn_exit = core_sym("rt_exit")->func, *fn_err = core_sym("rt_main_err")->func;
    Func *fn_flush = core_sym("flush") ? core_sym("flush")->func : NULL;
    Func *fn_panic = core_sym("panic") ? core_sym("panic")->func : NULL;
    if (!fn_flush) die("wasm: the runtime (core) does not define `flush`");
    reach_func(g_prog.main_fn); reach_func(fn_init); reach_func(fn_exit); reach_func(fn_err); reach_func(fn_flush);
    if (fn_panic) reach_func(fn_panic);
    VEC(Func *) exports = {0};
    for (int i = 0; i < g_prog.funcs.n; i++) {
        Func *f = g_prog.funcs.d[i];
        if (by_func[f->id] && is_exported_user_fn(f)) { reach_func(f); vpush(exports, f); }
    }
    for (int w = 0; w < work.n; w++) {
        FnIR *f = work.d[w];
        for (int k = 0; k < f->ins.n; k++) mark_ops(&f->ins.d[k], reach_opnd);
    }
    // global initialisers may reference strings
    for (int k = 0; k < g_isyms.n; k++) {
        ISym *s = &g_isyms.d[k];
        if (s->kind == IS_DATA && isym_used[k] && s->gsym->cv.kind == 4) {
            int id = find_str_isym(s->gsym->cv.s, s->gsym->cv.slen);
            if (id >= 0) isym_used[id] = true;
        }
    }
    for (int k = 0; k < g_isyms.n; k++) if (g_isyms.d[k].kind == IS_ERRTAB) isym_used[k] = true;

    // ---- imports
    for (int i = 0; i < g_prog.funcs.n; i++) {
        Func *f = g_prog.funcs.d[i];
        if (!f->is_extern || !w_reach[f->id]) continue;
        bool dup = false;
        for (int k = 0; k < g_imps.n; k++) if (!strcmp(g_imps.d[k].f->asm_name, f->asm_name)) { w_index[f->id] = g_imps.d[k].idx; dup = true; }
        if (dup) continue;
        Import im; memset(&im, 0, sizeof im);
        im.f = f;
        ext_sig(f, &im.es);
        if (!im.es.ok) die("wasm: extern fn '%s': %s (allowed: ints, bool, pointers, f64, str, slices; scalar / str / slice results)", f->decl->name, im.es.why);
        Sig s; memset(&s, 0, sizeof s);
        for (int k = 0; k < im.es.np; k++) s.p[s.np++] = im.es.p[k].wt;
        for (int k = 0; k < im.es.nr; k++) s.r[s.nr++] = im.es.r[k].wt;
        im.sig = sig_id(&s);
        im.idx = g_imps.n;
        w_index[f->id] = im.idx;
        vpush(g_imps, im);
    }
    n_imports = g_imps.n;
    // ---- defined functions
    for (int w = 0; w < work.n; w++) {
        FnIR *f = work.d[w];
        WFn wf; wf.ir = f; wf.idx = n_imports + w; wf.nres = count_results(f);
        wf.sig = sig_plain(f->nparams, wf.nres);
        w_index[f->f->id] = wf.idx; w_nres[f->f->id] = wf.nres;
        vpush(g_wfns, wf);
    }
    // ---- table indices
    int ntab = 0;
    VEC(Func *) taken = {0};
    for (int i = 0; i < g_prog.funcs.n; i++) {
        Func *f = g_prog.funcs.d[i];
        if (!w_addr[f->id] || f->is_extern || !by_func[f->id]) continue;
        w_tab[f->id] = ++ntab;
        vpush(taken, f);
    }
    // ---- memory layout
    int cur = 1024;
    for (int k = 0; k < g_isyms.n; k++) {
        ISym *s = &g_isyms.d[k];
        if (!isym_used[k]) continue;
        if (s->kind == IS_STR) {
            isym_addr[k] = cur;
            DataSeg d = { cur, {0} }; bcat(&d.bytes, s->bytes, (size_t)s->len);
            vpush(g_segs, d);
            cur = align_up(cur + (s->len ? s->len : 1), 4);
        } else if (s->kind == IS_DATA) {
            Type *t = s->gsym->type;
            int size = type_size(t), al = type_align(t);
            if (size == 0) size = 1;
            if (al > 16) al = 16;
            cur = align_up(cur, al < 8 ? 8 : al);
            isym_addr[k] = cur;
            cur += size;
        }
    }
    // stack-overflow message
    {
        const char *msg = "stack overflow";
        cur = align_up(cur, 4); str_stackov = cur;
        DataSeg d = { cur, {0} }; bcat(&d.bytes, msg, 14);
        vpush(g_segs, d);
        cur += 14;
    }
    // error name table: (ptr,len) pairs then the names
    for (int k = 0; k < g_isyms.n; k++) {
        if (g_isyms.d[k].kind != IS_ERRTAB) continue;
        int ne = g_prog.errors.n;
        cur = align_up(cur, 8);
        isym_addr[k] = cur;
        int tab = cur, strs = tab + 16 * (ne + 1);
        DataSeg d = { tab, {0} };
        int sp = strs;
        for (int e = 0; e <= ne; e++) {
            const char *nm = e == 0 ? "?" : g_prog.errors.d[e - 1]->name;
            int64_t p = sp, l = (int64_t)strlen(nm);
            bcat(&d.bytes, &p, 8); bcat(&d.bytes, &l, 8);
            sp += (int)l;
        }
        for (int e = 0; e <= ne; e++) { const char *nm = e == 0 ? "?" : g_prog.errors.d[e - 1]->name; bcat(&d.bytes, nm, strlen(nm)); }
        vpush(g_segs, d);
        cur = sp;
    }
    // initialised globals (after strings so their addresses are known)
    for (int k = 0; k < g_isyms.n; k++) {
        ISym *s = &g_isyms.d[k];
        if (s->kind != IS_DATA || !isym_used[k]) continue;
        Sym *g = s->gsym; ConstVal *cv = &g->cv;
        int size = type_size(g->type); if (size == 0) size = 1;
        bool zero = cv->kind == 0 || (cv->kind == 1 && cv->i == 0) || (cv->kind == 3 && cv->i == 0) || (cv->kind == 2 && cv->f == 0.0 && !signbit(cv->f));
        if (zero) continue;
        DataSeg d = { (int)isym_addr[k], {0} };
        if (cv->kind == 4) {
            int id = find_str_isym(cv->s, cv->slen);
            if (id < 0) die("wasm: internal error: string constant for global '%s' was not interned", g->name);
            int64_t p = isym_addr[id], l = cv->slen;
            bcat(&d.bytes, &p, 8); bcat(&d.bytes, &l, 8);
        } else if (cv->kind == 2) {
            double x = cv->f; bcat(&d.bytes, &x, 8);
        } else {
            long long v = (long long)cv->i;
            for (int b = 0; b < size && b < 8; b++) bput(&d.bytes, (int)((v >> (8 * b)) & 0xff));
        }
        vpush(g_segs, d);
    }
    stack_lo = align_up(cur, 16);
    stack_top = stack_lo + g_wasm_stack_kb * 1024;
    int init_pages = (stack_top + 65535) / 65536;
    // ---- global $sp is the only global
    g_export_sp_global = 0;
    panic_fn = fn_panic && w_index[fn_panic->id] >= 0 ? w_index[fn_panic->id] : -1;

    // ---- function bodies
    Buf codesec = {0};
    uleb(&codesec, 0);   // placeholder, replaced below (count written later)
    Buf bodies = {0};
    for (int w = 0; w < g_wfns.n; w++) emit_function(g_wfns.d[w].ir, g_wfns.d[w].nres, &bodies);
    // thunks
    for (int k = 0; k < taken.n; k++) {
        Func *f = taken.d[k];
        FnIR *ir = by_func[f->id];
        Buf code = {0}; cb = &code;
        for (int p = 0; p < ir->nparams; p++) lget(p);
        opu(0x10, (uint64_t)w_index[f->id]);
        for (int z = w_nres[f->id]; z < 2; z++) i64c(0);
        op(0x0B);
        Buf body = {0}; uleb(&body, 0); bcat(&body, code.d, code.n);
        Buf ws = {0}; uleb(&ws, body.n); bcat(&ws, body.d, body.n);
        new_extra(sig_uniform(ir->nparams), "thunk", &ws);
        free(code.d); free(body.d);
    }
    int first_thunk_extra = 0;
    // exports: wrappers
    typedef struct { char *name; int kind, idx; } Exp;
    VEC(Exp) exps = {0};
    for (int k = 0; k < exports.n; k++) {
        Func *f = exports.d[k];
        ESig es; ext_sig(f, &es);
        if (!es.ok) { fprintf(stderr, "orb: warning: pub fn '%s' is not exported to WebAssembly: %s\n", f->decl->name, es.why); continue; }
        int before = g_extras.n;
        make_export_wrapper(f, w_index[f->id], w_nres[f->id], &es, f->decl->name);
        Exp e = { xstrdup(f->decl->name), 0, extra_index(before) };
        vpush(exps, e);
    }
    // _start
    {
        Buf code = {0}; cb = &code;
        i64c(0); i64c(0); opu(0x10, (uint64_t)w_index[fn_init->id]);
        Type *mr = g_prog.main_fn->type->ret;
        opu(0x10, (uint64_t)w_index[g_prog.main_fn->id]);
        int mres = w_nres[g_prog.main_fn->id];
        if (mr->kind == TY_I64 || mr->kind == TY_ERRUNION) {
            if (mr->kind == TY_ERRUNION) { for (int z = 1; z < mres; z++) drop(); }
            // stack: value / error code
            lset(0);                  // local 0 = i64 temp (declared in the body header)
            lget(0); i64c(0); op(0x52);
            op(0x04); op(0x40);
            lget(0);
            opu(0x10, (uint64_t)w_index[mr->kind == TY_I64 ? fn_exit->id : fn_err->id]);
            op(0x00);
            op(0x05);
            opu(0x10, (uint64_t)w_index[fn_flush->id]);
            op(0x0B);
        } else {
            for (int z = 0; z < mres; z++) drop();
            opu(0x10, (uint64_t)w_index[fn_flush->id]);
        }
        op(0x0B);
        Buf body = {0}; uleb(&body, 1); uleb(&body, 1); bput(&body, VI64); bcat(&body, code.d, code.n);
        Buf ws = {0}; uleb(&ws, body.n); bcat(&ws, body.d, body.n);
        Sig s; memset(&s, 0, sizeof s);
        int before = g_extras.n;
        new_extra(sig_id(&s), "_start", &ws);
        Exp e = { xstrdup("_start"), 0, extra_index(before) };
        vpush(exps, e);
        free(code.d); free(body.d);
    }
    (void)first_thunk_extra;

    // ================================================================= assemble the module
    Buf out = {0};
    bcat(&out, "\0asm\1\0\0\0", 8);
    Buf s = {0};
    // type section (all signatures are known now)
    uleb(&s, (uint64_t)g_sigs.n);
    for (int k = 0; k < g_sigs.n; k++) {
        Sig *g = g_sigs.d[k];
        bput(&s, 0x60); uleb(&s, (uint64_t)g->np); bcat(&s, g->p, (size_t)g->np); uleb(&s, (uint64_t)g->nr); bcat(&s, g->r, (size_t)g->nr);
    }
    section(&out, 1, &s); s.n = 0;
    // imports
    if (n_imports) {
        uleb(&s, (uint64_t)n_imports);
        for (int k = 0; k < n_imports; k++) {
            const char *sym = g_imps.d[k].f->asm_name;
            const char *dot = strchr(sym, '.');
            char *mod = dot ? xstrndup(sym, (size_t)(dot - sym)) : xstrdup("env");
            bname(&s, mod); bname(&s, dot ? dot + 1 : sym);
            bput(&s, 0); uleb(&s, (uint64_t)g_imps.d[k].sig);
        }
        section(&out, 2, &s); s.n = 0;
    }
    // functions
    uleb(&s, (uint64_t)(g_wfns.n + g_extras.n));
    for (int k = 0; k < g_wfns.n; k++) uleb(&s, (uint64_t)g_wfns.d[k].sig);
    for (int k = 0; k < g_extras.n; k++) uleb(&s, (uint64_t)g_extras.d[k].sig);
    section(&out, 3, &s); s.n = 0;
    // table
    uleb(&s, 1); bput(&s, 0x70); bput(&s, 0x00); uleb(&s, (uint64_t)(ntab + 1));
    section(&out, 4, &s); s.n = 0;
    // memory
    uleb(&s, 1); bput(&s, 0x00); uleb(&s, (uint64_t)init_pages);
    section(&out, 5, &s); s.n = 0;
    // globals: $sp
    uleb(&s, 1); bput(&s, VI32); bput(&s, 1); bput(&s, 0x41); sleb(&s, stack_top); bput(&s, 0x0B);
    section(&out, 6, &s); s.n = 0;
    // exports
    {
        int ne = exps.n + 3;
        uleb(&s, (uint64_t)ne);
        for (int k = 0; k < exps.n; k++) { bname(&s, exps.d[k].name); bput(&s, 0); uleb(&s, (uint64_t)exps.d[k].idx); }
        bname(&s, "memory"); bput(&s, 2); uleb(&s, 0);
        bname(&s, "__indirect_function_table"); bput(&s, 1); uleb(&s, 0);
        bname(&s, "__orb_sp"); bput(&s, 3); uleb(&s, 0);
        section(&out, 7, &s); s.n = 0;
    }
    // element segment: table[1..] = thunks
    if (ntab) {
        uleb(&s, 1); bput(&s, 0); bput(&s, 0x41); sleb(&s, 1); bput(&s, 0x0B);
        uleb(&s, (uint64_t)ntab);
        for (int k = 0; k < ntab; k++) uleb(&s, (uint64_t)extra_index(k));   // thunks were created first, in table order
        section(&out, 9, &s); s.n = 0;
    }
    // code
    {
        uleb(&s, (uint64_t)(g_wfns.n + g_extras.n));
        bcat(&s, bodies.d, bodies.n);
        for (int k = 0; k < g_extras.n; k++) bcat(&s, g_extras.d[k].body.d, g_extras.d[k].body.n);
        section(&out, 10, &s); s.n = 0;
    }
    // data
    if (g_segs.n) {
        uleb(&s, (uint64_t)g_segs.n);
        for (int k = 0; k < g_segs.n; k++) {
            DataSeg *d = &g_segs.d[k];
            bput(&s, 0); bput(&s, 0x41); sleb(&s, d->addr); bput(&s, 0x0B);
            uleb(&s, d->bytes.n); bcat(&s, d->bytes.d, d->bytes.n);
        }
        section(&out, 11, &s); s.n = 0;
    }
    // name section (function names, for readable stack traces and DevTools)
    {
        Buf names = {0};
        int total = n_imports + g_wfns.n + g_extras.n;
        uleb(&names, (uint64_t)total);
        for (int k = 0; k < n_imports; k++) { uleb(&names, (uint64_t)k); bname(&names, g_imps.d[k].f->decl->name); }
        for (int k = 0; k < g_wfns.n; k++) { uleb(&names, (uint64_t)g_wfns.d[k].idx); bname(&names, g_wfns.d[k].ir->f->decl->name); }
        for (int k = 0; k < g_extras.n; k++) {
            uleb(&names, (uint64_t)extra_index(k));
            char *nm = k < taken.n ? xsprintf("%s@table", taken.d[k]->decl->name) : g_extras.d[k].name;
            bname(&names, nm);
        }
        Buf sub = {0}; bput(&sub, 1); uleb(&sub, names.n); bcat(&sub, names.d, names.n);
        bname(&s, "name"); bcat(&s, sub.d, sub.n);
        section(&out, 0, &s); s.n = 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write '%s': %s", path, strerror(errno));
    fwrite(out.d, 1, out.n, f);
    fclose(f);
}
