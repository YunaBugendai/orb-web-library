#include "sema.h"
#include "ir.h"

ISymVec g_isyms;
IRVec g_irs;

enum { L_NONE, L_REG, L_PAIR, L_SLOT, L_PTR, L_GLOB };
enum { A_SLOT, A_REG, A_GLOB };

typedef struct { int k; int64_t base; int idx; int scale; int64_t off; } Addr;
typedef struct { int brk, cont; } Loop;

static struct {
    FnIR *ir;
    Func *f;
    Type *ret;
    int sret;
    VEC(Loop) loops;
    int file;
} G;

static int *fn_isym, *glob_isym;
static int errtab_isym;
static Map str_map;

// ------------------------------------------------------------------ IR building blocks
static Opnd R(int r) { return (Opnd){ O_REG, r }; }
static Opnd I(int64_t v) { return (Opnd){ O_IMM, v }; }
static Opnd NONE(void) { return (Opnd){ O_NONE, 0 }; }

static int nreg(int cls) { vpush(G.ir->cls, (char)cls); return G.ir->cls.n - 1; }
static int cls_of(Type *t) { return t->kind == TY_F64 ? 1 : 0; }
static int nlabel(void) { return G.ir->nlabels++; }
static Ins *emit(int op) {
    Ins z; memset(&z, 0, sizeof z);
    z.op = op; z.dst = -1; z.dst2 = -1; z.scale = 1;
    vpush(G.ir->ins, z);
    return &G.ir->ins.d[G.ir->ins.n - 1];
}
static void label(int l) { emit(OP_LABEL)->label = l; }
static void jmp(int l) { emit(OP_JMP)->label = l; }
static void jcc(int cc, Opnd a, Opnd b, int l) { Ins *i = emit(OP_JCC); i->cc = cc; i->a = a; i->b = b; i->label = l; }
static int newslot(int size, int align) {
    Slot s = { size ? size : 1, align ? align : 1, 0 };
    vpush(G.ir->slots, s);
    return G.ir->slots.n - 1;
}

static int str_sym(const char *bytes, int len, bool nul) {
    char *key = xcalloc((size_t)len * 2 + 4);
    for (int i = 0; i < len; i++) sprintf(key + i * 2, "%02x", (unsigned char)bytes[i]);
    if (nul) strcat(key, "00");
    void *p = map_get(&str_map, key);
    if (p) return (int)(intptr_t)p - 1;
    ISym s = {0};
    s.kind = IS_STR;
    s.name = xsprintf(".Lstr%d", g_isyms.n);
    s.bytes = xcalloc((size_t)len + 2);
    memcpy(s.bytes, bytes, (size_t)len);
    s.len = len + (nul ? 1 : 0);
    vpush(g_isyms, s);
    map_put(&str_map, key, (void *)(intptr_t)(g_isyms.n));
    return g_isyms.n - 1;
}

static Opnd glob(int id) { return (Opnd){ O_GLOB, id }; }

// mov into a fresh register unless already a register
static int to_reg(Opnd o) {
    if (o.k == O_REG) return (int)o.v;
    int r = nreg(0);
    Ins *i = emit(OP_MOV); i->dst = r; i->a = o;
    return r;
}
static int fresh_copy(Opnd o, int cls) {
    int r = nreg(cls);
    Ins *i = emit(OP_MOV); i->dst = r; i->a = o;
    return r;
}
static void mov(int dst, Opnd v) { Ins *i = emit(OP_MOV); i->dst = dst; i->a = v; }

// ------------------------------------------------------------------ addresses
static Addr aslot(int s) { return (Addr){ A_SLOT, s, -1, 1, 0 }; }
static Addr areg(int r, int64_t off) { return (Addr){ A_REG, r, -1, 1, off }; }
static Addr aoff(Addr a, int64_t off) { a.off += off; return a; }

static Opnd lea(Addr a);
static void fix_addr(Addr *a) {
    if (a->k == A_GLOB && a->idx >= 0) {
        Ins *i = emit(OP_LEA);
        int r = nreg(0);
        i->dst = r; i->a = glob((int)a->base); i->off = a->off; i->scale = 1; i->b = NONE();
        a->k = A_REG; a->base = r; a->off = 0;
    }
}
static void set_addr(Ins *i, Addr a) {
    i->a = a.k == A_SLOT ? (Opnd){ O_SLOT, a.base } : a.k == A_GLOB ? glob((int)a.base) : R((int)a.base);
    i->b = a.idx >= 0 ? R(a.idx) : NONE();
    i->scale = a.scale; i->off = a.off;
}
static Opnd lea(Addr a) {
    fix_addr(&a);
    int r = nreg(0);
    Ins *i = emit(OP_LEA);
    i->dst = r; set_addr(i, a);
    return R(r);
}
static Addr flatten(Addr a) {
    if (a.idx < 0) return a;
    return areg((int)lea(a).v, 0);
}

static int mem_w(Type *t) {
    if (t->kind == TY_BOOL) return 1;
    int s = type_size(t);
    return s > 8 ? 8 : s;
}
static Opnd emit_load(Addr a, int w, bool sg, int cls) {
    fix_addr(&a);
    int r = nreg(cls);
    Ins *i = emit(OP_LOAD);
    i->dst = r; i->w = w; i->sg = sg; set_addr(i, a);
    return R(r);
}
static void emit_store(Addr a, Opnd v, int w) {
    fix_addr(&a);
    Ins *i = emit(OP_STORE);
    i->w = w; i->c = v; set_addr(i, a);
}
static Opnd load_scalar(Addr a, Type *t) { return emit_load(a, mem_w(t), ty_is_signed(t), cls_of(t)); }
static void store_scalar(Addr a, Opnd v, Type *t) { emit_store(a, v, mem_w(t)); }

static void emit_zero(Addr a, int size) {
    if (size > 128) {
        Opnd d = lea(a);
        Ins *i = emit(OP_MEMZERO);
        i->args = xcalloc(sizeof(Opnd) * 2); i->nargs = 2;
        i->args[0] = d; i->args[1] = I(size);
        return;
    }
    int off = 0;
    while (size - off >= 8) { emit_store(aoff(a, off), I(0), 8); off += 8; }
    if (size - off >= 4) { emit_store(aoff(a, off), I(0), 4); off += 4; }
    if (size - off >= 2) { emit_store(aoff(a, off), I(0), 2); off += 2; }
    if (size - off >= 1) { emit_store(aoff(a, off), I(0), 1); }
}
static void emit_copy(Addr dst, Addr src, int size) {
    if (size > 128) {
        Opnd d = lea(dst), s = lea(src);
        Ins *i = emit(OP_MEMCPY);
        i->args = xcalloc(sizeof(Opnd) * 3); i->nargs = 3;
        i->args[0] = d; i->args[1] = s; i->args[2] = I(size);
        return;
    }
    dst = flatten(dst); src = flatten(src);
    int off = 0;
    while (size - off >= 8) { emit_store(aoff(dst, off), emit_load(aoff(src, off), 8, false, 0), 8); off += 8; }
    if (size - off >= 4) { emit_store(aoff(dst, off), emit_load(aoff(src, off), 4, false, 0), 4); off += 4; }
    if (size - off >= 2) { emit_store(aoff(dst, off), emit_load(aoff(src, off), 2, false, 0), 2); off += 2; }
    if (size - off >= 1) { emit_store(aoff(dst, off), emit_load(aoff(src, off), 1, false, 0), 1); }
}

// ------------------------------------------------------------------ arithmetic helpers
static Opnd bin(int op, Opnd a, Opnd b, int cls) {
    if (a.k == O_IMM && b.k == O_REG && (op == OP_ADD || op == OP_MUL || op == OP_AND || op == OP_OR || op == OP_XOR)) { Opnd t = a; a = b; b = t; }
    int d = nreg(cls);
    Ins *i = emit(op);
    i->dst = d; i->a = a; i->b = b;
    return R(d);
}
static Opnd unop(int op, Opnd a, int cls) {
    int d = nreg(cls);
    Ins *i = emit(op);
    i->dst = d; i->a = a;
    return R(d);
}
static Opnd ext(Opnd v, int bits, bool sg) {
    if (v.k == O_IMM) {
        int64_t x = v.v;
        if (bits == 8) x = sg ? (int8_t)x : (uint8_t)x;
        else if (bits == 16) x = sg ? (int16_t)x : (uint16_t)x;
        else if (bits == 32) x = sg ? (int64_t)(int32_t)x : (int64_t)(uint32_t)x;
        return I(x);
    }
    int d = nreg(0);
    Ins *i = emit(sg ? OP_SEXT : OP_ZEXT);
    i->dst = d; i->a = v; i->w = bits / 8;
    return R(d);
}
static Opnd norm(Opnd v, Type *t) {
    if (ty_is_int(t) && ty_bits(t) < 64) return ext(v, ty_bits(t), ty_is_signed(t));
    return v;
}

static bool fold(int op, int64_t a, int64_t b, int64_t *out) {
    switch (op) {
    case OP_ADD: *out = (int64_t)((uint64_t)a + (uint64_t)b); return true;
    case OP_SUB: *out = (int64_t)((uint64_t)a - (uint64_t)b); return true;
    case OP_MUL: *out = (int64_t)((uint64_t)a * (uint64_t)b); return true;
    case OP_AND: *out = a & b; return true;
    case OP_OR: *out = a | b; return true;
    case OP_XOR: *out = a ^ b; return true;
    case OP_SHL: if (b < 0 || b > 63) return false; *out = (int64_t)((uint64_t)a << b); return true;
    case OP_SHR: if (b < 0 || b > 63) return false; *out = (int64_t)((uint64_t)a >> b); return true;
    case OP_SAR: if (b < 0 || b > 63) return false; *out = a >> b; return true;
    case OP_DIV: if (b == 0 || (a == INT64_MIN && b == -1)) return false; *out = a / b; return true;
    case OP_MOD: if (b == 0 || (a == INT64_MIN && b == -1)) return false; *out = a % b; return true;
    case OP_UDIV: if (b == 0) return false; *out = (int64_t)((uint64_t)a / (uint64_t)b); return true;
    case OP_UMOD: if (b == 0) return false; *out = (int64_t)((uint64_t)a % (uint64_t)b); return true;
    }
    return false;
}

static int cc_for(int tok, bool sg) {
    switch (tok) {
    case TK_EQ: return CC_EQ;
    case TK_NE: return CC_NE;
    case TK_LT: return sg ? CC_LT : CC_B;
    case TK_LE: return sg ? CC_LE : CC_BE;
    case TK_GT: return sg ? CC_GT : CC_A;
    default: return sg ? CC_GE : CC_AE;
    }
}
static int cc_invert(int cc) {
    static const int inv[] = { CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_AE, CC_A, CC_BE, CC_B };
    return inv[cc];
}
static bool cmp_signed(Type *t) { return ty_is_signed(t) || t->kind == TY_ENUM || t->kind == TY_ERRCODE; }
static bool fold_cc(int cc, int64_t a, int64_t b) {
    switch (cc) {
    case CC_EQ: return a == b; case CC_NE: return a != b;
    case CC_LT: return a < b; case CC_LE: return a <= b; case CC_GT: return a > b; case CC_GE: return a >= b;
    case CC_B: return (uint64_t)a < (uint64_t)b; case CC_BE: return (uint64_t)a <= (uint64_t)b;
    case CC_A: return (uint64_t)a > (uint64_t)b; default: return (uint64_t)a >= (uint64_t)b;
    }
}

// ------------------------------------------------------------------ runtime hooks
static Func *cf(const char *name) {
    Sym *s = core_sym(name);
    if (s->kind != S_FUNC) die("internal error: core.%s is not a function", name);
    func_sig(s->func);
    return s->func;
}
static Ins *emit_call(Opnd target, Opnd *args, int n, int dst, int dst2, bool noret) {
    Ins *i = emit(OP_CALL);
    i->a = target; i->dst = dst; i->dst2 = dst2; i->noret = noret;
    i->args = xcalloc(sizeof(Opnd) * (size_t)(n + 1)); i->nargs = n;
    for (int k = 0; k < n; k++) i->args[k] = args[k];
    G.ir->has_call = true;
    return i;
}
static Opnd call_core(const char *name, Opnd *args, int n, bool want_result) {
    Func *f = cf(name);
    Type *rt = f->type->ret;
    int d = (want_result && rt->kind != TY_VOID && rt->kind != TY_NEVER) ? nreg(cls_of(rt)) : -1;
    emit_call(glob(fn_isym[f->id]), args, n, d, -1, rt->kind == TY_NEVER);
    return d >= 0 ? R(d) : NONE();
}

static int file_str(void) {
    const char *p = source_path(G.file);
    return str_sym(p, (int)strlen(p), true);
}
static void check_bounds(Opnd idx, Opnd len, int line) {
    if (g_prog.unchecked) return;
    if (idx.k == O_IMM && len.k == O_IMM && (uint64_t)idx.v < (uint64_t)len.v) return;
    int ok = nlabel();
    jcc(CC_B, idx, len, ok);
    Opnd a[4] = { glob(file_str()), I(line), idx, len };
    call_core("panic_bounds", a, 4, false);
    label(ok);
}

// ------------------------------------------------------------------ variables
static void alloc_var(Sym *s) {
    Type *t = s->type;
    TypeClass tc = type_class(t);
    if (tc == TC_SCALAR && !s->addr_taken) { s->loc = L_REG; s->r0 = nreg(cls_of(t)); }
    else if (tc == TC_PAIR && !s->addr_taken) { s->loc = L_PAIR; s->r0 = nreg(0); s->r1 = nreg(0); }
    else { s->loc = L_SLOT; s->slot = newslot(type_size(t), type_align(t)); }
}
static Addr var_addr(Sym *s) {
    if (s->global) return (Addr){ A_GLOB, glob_isym[s->gid], -1, 1, 0 };
    if (s->loc == L_SLOT) return aslot(s->slot);
    return areg(s->r0, 0);   // L_PTR
}

// ------------------------------------------------------------------ forward decls
static Opnd lw_scalar(Node *n);
static void lw_pair(Node *n, Opnd *p0, Opnd *p1);
static Addr lw_addr(Node *n);
static void lw_into(Node *n, Addr dst);
static void lw_branch(Node *n, int lbl, bool jump_if);
static void lw_call(Node *n, Opnd *r0, Opnd *r1, Addr *sret);
static void lw_stmt(Node *s);
static void lw_block(Node *b);

static Opnd lw_arith(int tok, Opnd a, Opnd b, Type *lt, Type *rt, Type *res, int line) {
    (void)rt;
    if (lt->kind == TY_F64) {
        int op = tok == TK_PLUS ? OP_FADD : tok == TK_MINUS ? OP_FSUB : tok == TK_STAR ? OP_FMUL : OP_FDIV;
        return bin(op, a, b, 1);
    }
    if (lt->kind == TY_PTR) {
        if (res->kind == TY_I64) {   // ptr - ptr
            Opnd d = bin(OP_SUB, a, b, 0);
            int sz = type_size(lt->elem);
            if (sz > 1) d = bin(OP_DIV, d, I(sz), 0);
            return d;
        }
        int sz = type_size(lt->elem);
        Opnd off = b;
        if (sz != 1) off = off.k == O_IMM ? I(off.v * sz) : bin(OP_MUL, off, I(sz), 0);
        return bin(tok == TK_PLUS ? OP_ADD : OP_SUB, a, off, 0);
    }
    bool sg = ty_is_signed(lt);
    int op;
    switch (tok) {
    case TK_PLUS: op = OP_ADD; break;
    case TK_MINUS: op = OP_SUB; break;
    case TK_STAR: op = OP_MUL; break;
    case TK_SLASH: op = sg ? OP_DIV : OP_UDIV; break;
    case TK_PERCENT: op = sg ? OP_MOD : OP_UMOD; break;
    case TK_AMP: op = OP_AND; break;
    case TK_PIPE: op = OP_OR; break;
    case TK_CARET: op = OP_XOR; break;
    case TK_SHL: op = OP_SHL; break;
    default: op = sg ? OP_SAR : OP_SHR; break;
    }
    int64_t out;
    if (a.k == O_IMM && b.k == O_IMM && fold(op, a.v, b.v, &out)) return norm(I(out), lt);
    if ((op == OP_DIV || op == OP_UDIV || op == OP_MOD || op == OP_UMOD) && !g_prog.unchecked && b.k != O_IMM) {
        int ok = nlabel();
        jcc(CC_NE, b, I(0), ok);
        Opnd args[2] = { glob(file_str()), I(line) };
        call_core("panic_div", args, 2, false);
        label(ok);
    }
    Opnd r = bin(op, a, b, 0);
    if (op == OP_AND || op == OP_OR || op == OP_XOR || op == OP_SHR || op == OP_SAR) return r;
    return norm(r, lt);
}

static Opnd lw_cast(Node *n) {
    Type *to = n->type, *from = n->a->type;
    Opnd v = lw_scalar(n->a);
    if (ty_is_float(to)) {
        if (ty_is_float(from)) return v;
        int d = nreg(1);
        Opnd sv = v.k == O_IMM ? R(to_reg(v)) : v;
        Ins *i = emit(from->kind == TY_U64 ? OP_U2F : OP_I2F);
        i->dst = d; i->a = sv;
        return R(d);
    }
    if (ty_is_float(from)) {   // f64 -> int
        int d = nreg(0);
        Ins *i = emit(to->kind == TY_U64 ? OP_F2U : OP_F2I);
        i->dst = d; i->a = v;
        return norm(R(d), to);
    }
    if (ty_is_int(to) && (ty_is_intlike(from) || from->kind == TY_BOOL || from->kind == TY_PTR || from->kind == TY_FUNC)) {
        int fb = ty_is_int(from) ? ty_bits(from) : 64;
        bool fs = ty_is_int(from) ? ty_is_signed(from) : false;
        int tb = ty_bits(to);
        bool ts = ty_is_signed(to);
        if (tb == 64) return v;
        if (tb > fb && (!fs || ts)) return v;
        if (tb == fb && fs == ts) return v;
        return ext(v, tb, ts);
    }
    return v;   // ptr/enum/error conversions are no-ops on 64-bit values
}

static Opnd lw_load_var(Sym *s) {
    if (s->loc == L_REG && !s->global) return R(s->r0);
    return load_scalar(var_addr(s), s->type);
}

static void lw_pair_load(Addr a, Opnd *p0, Opnd *p1) {
    a = flatten(a);
    *p0 = emit_load(a, 8, false, 0);
    *p1 = emit_load(aoff(a, 8), 8, false, 0);
}

static void lw_pair(Node *n, Opnd *p0, Opnd *p1) {
    switch (n->kind) {
    case N_STR: {
        int id = str_sym(n->str, n->slen, false);
        *p0 = lea((Addr){ A_GLOB, id, -1, 1, 0 });
        *p1 = I(n->slen);
        return;
    }
    case N_IDENT:
        if (n->sym->kind == S_VAR) {
            Sym *s = n->sym;
            if (!s->global && s->loc == L_PAIR) { *p0 = R(s->r0); *p1 = R(s->r1); return; }
            lw_pair_load(var_addr(s), p0, p1);
            return;
        }
        break;
    case N_FIELD: case N_INDEX:
        lw_pair_load(lw_addr(n), p0, p1);
        return;
    case N_UNARY:
        if (n->op == TK_STAR) { lw_pair_load(lw_addr(n), p0, p1); return; }
        break;
    case N_CAST:
        lw_pair(n->a, p0, p1);
        return;
    case N_CONV:
        if (n->aux == CV_ARR2SLICE) {
            Type *at = n->a->type;
            *p0 = lea(lw_addr(n->a));
            *p1 = I(at->len);
            return;
        }
        if (n->aux == CV_ERRWRAP) {
            *p0 = I(0);
            Type *pt = n->type->elem;
            *p1 = pt->kind == TY_VOID ? I(0) : lw_scalar(n->a);
            return;
        }
        break;
    case N_SLICE: {
        Node *base = n->a;
        Type *bt = base->type;
        Opnd p, len;
        int esz;
        if (bt->kind == TY_ARRAY) { p = lea(lw_addr(base)); len = I(bt->len); esz = type_size(bt->elem); }
        else { lw_pair(base, &p, &len); esz = bt->kind == TY_STR ? 1 : type_size(bt->elem); }
        Opnd lo = n->b ? lw_scalar(n->b) : I(0);
        Opnd hi = n->c ? lw_scalar(n->c) : len;
        if (n->c && n->aux) hi = bin(OP_ADD, hi, I(1), 0);
        if (!g_prog.unchecked && (n->b || n->c)) {
            int ok = nlabel();
            jcc(CC_BE, hi, len, ok);
            { Opnd a[5] = { glob(file_str()), I(n->pos.line), lo, hi, len }; call_core("panic_slice", a, 5, false); }
            label(ok);
            int ok2 = nlabel();
            jcc(CC_BE, lo, hi, ok2);
            { Opnd a[5] = { glob(file_str()), I(n->pos.line), lo, hi, len }; call_core("panic_slice", a, 5, false); }
            label(ok2);
        }
        Opnd np = p;
        if (!(lo.k == O_IMM && lo.v == 0)) {
            Opnd off = esz == 1 ? lo : (lo.k == O_IMM ? I(lo.v * esz) : bin(OP_MUL, lo, I(esz), 0));
            np = bin(OP_ADD, p, off, 0);
        }
        Opnd nl = (lo.k == O_IMM && lo.v == 0) ? hi : bin(OP_SUB, hi, lo, 0);
        *p0 = np; *p1 = nl;
        return;
    }
    case N_CALL: lw_call(n, p0, p1, NULL); return;
    case N_MAKE: {
        int esz = type_size(n->a->type ? n->a->type : n->type->elem);
        esz = type_size(n->type->elem);
        Opnd cnt = lw_scalar(n->b);
        if (!g_prog.unchecked) {
            int ok = nlabel();
            jcc(CC_GE, cnt, I(0), ok);
            Opnd a[2] = { glob(file_str()), I(n->pos.line) };
            call_core("panic_div", a, 2, false);   // negative size reported as a runtime panic
            label(ok);
        }
        Opnd bytes = cnt.k == O_IMM ? I(cnt.v * esz) : (esz == 1 ? cnt : bin(OP_MUL, cnt, I(esz), 0));
        Opnd a[1] = { bytes };
        *p0 = call_core("mem_zalloc", a, 1, true);
        *p1 = cnt;
        return;
    }
    case N_TRY: case N_CATCH: break;
    default: break;
    }
    // try / catch with pair payload, or anything else addressable
    if (n->kind == N_TRY || n->kind == N_CATCH) {
        extern void lw_trycatch(Node *n, Opnd *r0, Opnd *r1, Addr *dst);
        lw_trycatch(n, p0, p1, NULL);
        return;
    }
    {
        int tmp = newslot(16, 8);
        lw_into(n, aslot(tmp));
        lw_pair_load(aslot(tmp), p0, p1);
    }
}

// payload extraction shared by try / catch. Evaluates operand into (err, payload) form.
typedef struct { Opnd err; Addr mem; bool is_mem; Opnd v0, v1; } EU;
static EU lw_eu(Node *operand) {
    EU e = {0};
    Type *t = operand->type;
    if (type_class(t) == TC_PAIR) {
        lw_pair(operand, &e.err, &e.v0);
        e.v1 = I(0);
    } else {
        int tmp = newslot(type_size(t), type_align(t));
        lw_into(operand, aslot(tmp));
        e.is_mem = true; e.mem = aslot(tmp);
        e.err = emit_load(e.mem, 8, false, 0);
    }
    return e;
}
static int eu_payload_off(Type *eu) {
    int pa = type_align(eu->elem); if (pa < 8) pa = 8;
    return (8 + pa - 1) / pa * pa;
}
// read payload of the error-union described by e into r0/r1 (scalar/pair) -- MEM payloads handled by callers
static void eu_read(EU *e, Type *eut, Opnd *r0, Opnd *r1) {
    Type *pt = eut->elem;
    TypeClass pc = pt->kind == TY_VOID ? TC_VOID : type_class(pt);
    if (pc == TC_VOID) return;
    if (!e->is_mem) { *r0 = e->v0; return; }
    Addr pa = aoff(e->mem, eu_payload_off(eut));
    if (pc == TC_SCALAR) *r0 = load_scalar(pa, pt);
    else if (pc == TC_PAIR) lw_pair_load(pa, r0, r1);
}
static void emit_return_err(Opnd err) {
    Type *r = G.ret;
    if (type_class(r) == TC_PAIR) { Ins *i = emit(OP_RET); i->a = err; i->b = I(0); }
    else { emit_store(areg(G.sret, 0), err, 8); emit(OP_RET); }
}

// Lowers `try x` and `x catch ...`. For scalar / pair / void payloads results go in r0/r1.
// For MEM payloads the value is copied into *dst.
void lw_trycatch(Node *n, Opnd *r0, Opnd *r1, Addr *dst) {
    Node *operand = n->a;
    Type *eut = operand->type;
    Type *pt = eut->elem;
    TypeClass pc = pt->kind == TY_VOID ? TC_VOID : type_class(pt);
    EU e = lw_eu(operand);
    int ok = nlabel();
    if (n->kind == N_TRY) {
        jcc(CC_EQ, e.err, I(0), ok);
        emit_return_err(e.err);
        label(ok);
        if (pc == TC_MEM) { if (dst) emit_copy(*dst, aoff(e.mem, eu_payload_off(eut)), type_size(pt)); else { /* caller wants address */ *r0 = lea(aoff(e.mem, eu_payload_off(eut))); } }
        else eu_read(&e, eut, r0, r1);
        return;
    }
    // catch
    int end = nlabel();
    int res0 = -1, res1 = -1;
    if (pc == TC_SCALAR) { res0 = nreg(cls_of(pt)); }
    else if (pc == TC_PAIR) { res0 = nreg(0); res1 = nreg(0); }
    jcc(CC_EQ, e.err, I(0), ok);
    // failure path
    if (n->b) {
        if (pc == TC_SCALAR) mov(res0, lw_scalar(n->b));
        else if (pc == TC_PAIR) { Opnd a, b; lw_pair(n->b, &a, &b); mov(res0, a); mov(res1, b); }
        else if (pc == TC_MEM) lw_into(n->b, *dst);
    } else {
        Sym *cs = n->sym;
        cs->loc = L_REG; cs->r0 = nreg(0);
        mov(cs->r0, e.err);
        lw_block(n->c);
        if (pc == TC_SCALAR) mov(res0, I(0));
    }
    jmp(end);
    label(ok);
    if (pc == TC_SCALAR) { Opnd a; eu_read(&e, eut, &a, NULL); mov(res0, a); }
    else if (pc == TC_PAIR) { Opnd a, b; eu_read(&e, eut, &a, &b); mov(res0, a); mov(res1, b); }
    else if (pc == TC_MEM) emit_copy(*dst, aoff(e.mem, eu_payload_off(eut)), type_size(pt));
    label(end);
    if (res0 >= 0) *r0 = R(res0);
    if (res1 >= 0) *r1 = R(res1);
}

static Addr lw_addr(Node *n) {
    switch (n->kind) {
    case N_IDENT:
        if (n->sym && n->sym->kind == S_VAR) {
            Sym *s = n->sym;
            if (!s->global && (s->loc == L_REG || s->loc == L_PAIR)) die("internal error: address of register variable '%s'", s->name);
            return var_addr(s);
        }
        break;
    case N_FIELD: {
        Type *bt = n->a->type;
        Type *st = n->aux == 1 ? bt->elem : bt;
        Addr base;
        if (n->aux == 1) base = areg(to_reg(lw_scalar(n->a)), 0);
        else base = lw_addr(n->a);
        if (st->kind == TY_STRUCT) return aoff(base, st->fields[n->ival].off);
        break;
    }
    case N_INDEX: {
        Type *bt = n->a->type;
        Addr base;
        int esz = type_size(n->type);
        Opnd idx;
        if (bt->kind == TY_ARRAY) {
            base = flatten(lw_addr(n->a));
            idx = lw_scalar(n->b);
            if (idx.k == O_IMM) { base.off += idx.v * esz; return base; }
            check_bounds(idx, I(bt->len), n->pos.line);
        } else if (bt->kind == TY_PTR) {
            base = areg(to_reg(lw_scalar(n->a)), 0);
            idx = lw_scalar(n->b);
            if (idx.k == O_IMM) { base.off += idx.v * esz; return base; }
        } else {   // slice / str
            Opnd p, len;
            lw_pair(n->a, &p, &len);
            idx = lw_scalar(n->b);
            check_bounds(idx, len, n->pos.line);
            base = areg(to_reg(p), 0);
            if (idx.k == O_IMM) { base.off += idx.v * esz; return base; }
        }
        int ir = to_reg(idx);
        if (esz == 1 || esz == 2 || esz == 4 || esz == 8) { base.idx = ir; base.scale = esz; }
        else { base.idx = (int)bin(OP_MUL, R(ir), I(esz), 0).v; base.scale = 1; }
        return base;
    }
    case N_UNARY:
        if (n->op == TK_STAR) return areg(to_reg(lw_scalar(n->a)), 0);
        break;
    default: break;
    }
    // rvalue of memory class: build it in a temporary
    int tmp = newslot(type_size(n->type), type_align(n->type));
    lw_into(n, aslot(tmp));
    return aslot(tmp);
}

static Opnd lw_scalar(Node *n) {
    switch (n->kind) {
    case N_INT: return I((int64_t)n->ival);
    case N_BOOL: return I((int64_t)n->ival);
    case N_NULL: return I(0);
    case N_FLOAT: {
        int d = nreg(1);
        Ins *i = emit(OP_FCONST);
        double f = n->fval; int64_t bits; memcpy(&bits, &f, 8);
        i->dst = d; i->a = I(bits);
        return R(d);
    }
    case N_IDENT: {
        Sym *s = n->sym;
        if (s->kind == S_VAR) return lw_load_var(s);
        if (s->kind == S_FUNC) { Addr a = { A_GLOB, fn_isym[s->func->id], -1, 1, 0 }; return lea(a); }
        break;
    }
    case N_FIELD: {
        Type *bt = n->a->type;
        Type *st = n->aux == 1 ? bt->elem : bt;
        if (st->kind == TY_SLICE || st->kind == TY_STR) {
            int which = !strcmp(n->name, "len");
            if (n->aux != 1 && n->a->kind == N_IDENT && n->a->sym->kind == S_VAR && !n->a->sym->global && n->a->sym->loc == L_PAIR)
                return R(which ? n->a->sym->r1 : n->a->sym->r0);
            if (n->aux != 1 && !(n->a->kind == N_IDENT || n->a->kind == N_FIELD || n->a->kind == N_INDEX || (n->a->kind == N_UNARY && n->a->op == TK_STAR))) {
                Opnd p, l; lw_pair(n->a, &p, &l); return which ? l : p;
            }
            Addr base;
            if (n->aux == 1) base = areg(to_reg(lw_scalar(n->a)), 0);
            else base = lw_addr(n->a);
            return emit_load(aoff(base, which ? 8 : 0), 8, false, 0);
        }
        return load_scalar(lw_addr(n), n->type);
    }
    case N_INDEX: return load_scalar(lw_addr(n), n->type);
    case N_UNARY:
        switch (n->op) {
        case TK_AMP: return lea(lw_addr(n->a));
        case TK_STAR: return load_scalar(lw_addr(n), n->type);
        case TK_MINUS: {
            Opnd v = lw_scalar(n->a);
            if (n->type->kind == TY_F64) return unop(OP_FNEG, v, 1);
            if (v.k == O_IMM) return norm(I((int64_t)(0 - (uint64_t)v.v)), n->type);
            return norm(unop(OP_NEG, v, 0), n->type);
        }
        case TK_TILDE: {
            Opnd v = lw_scalar(n->a);
            if (v.k == O_IMM) return norm(I(~v.v), n->type);
            return norm(unop(OP_NOT, v, 0), n->type);
        }
        case TK_BANG: {
            Opnd v = lw_scalar(n->a);
            if (v.k == O_IMM) return I(!v.v);
            return bin(OP_XOR, v, I(1), 0);
        }
        }
        break;
    case N_BINARY: {
        int op = n->op;
        if (op == TK_ANDAND || op == TK_OROR) {
            int res = nreg(0), lf = nlabel(), le = nlabel();
            lw_branch(n, lf, false);
            mov(res, I(1)); jmp(le);
            label(lf); mov(res, I(0));
            label(le);
            return R(res);
        }
        Type *lt = n->a->type;
        bool cmp = op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE;
        if (cmp) {
            if (lt->kind == TY_STR) {
                Opnd a0, a1, b0, b1;
                lw_pair(n->a, &a0, &a1); lw_pair(n->b, &b0, &b1);
                Opnd args[4] = { a0, a1, b0, b1 };
                Opnd r = call_core("str_eq", args, 4, true);
                if (op == TK_NE) return bin(OP_XOR, r, I(1), 0);
                return r;
            }
            Opnd a = lw_scalar(n->a), b = lw_scalar(n->b);
            int d = nreg(0);
            if (lt->kind == TY_F64) {
                Ins *i = emit(OP_FSET);
                i->dst = d; i->a = a; i->b = b; i->cc = cc_for(op, true);
                return R(d);
            }
            int cc = cc_for(op, cmp_signed(lt));
            if (a.k == O_IMM && b.k == O_IMM) return I(fold_cc(cc, a.v, b.v));
            Ins *i = emit(OP_SET);
            i->dst = d; i->a = a; i->b = b; i->cc = cc;
            return R(d);
        }
        Opnd a = lw_scalar(n->a), b = lw_scalar(n->b);
        return lw_arith(op, a, b, lt, n->b->type, n->type, n->pos.line);
    }
    case N_CAST: return lw_cast(n);
    case N_CALL: { Opnd r0 = NONE(), r1 = NONE(); lw_call(n, &r0, &r1, NULL); return r0.k == O_NONE ? I(0) : r0; }
    case N_TRY: case N_CATCH: {
        Opnd r0 = NONE(), r1 = NONE();
        lw_trycatch(n, &r0, &r1, NULL);
        return r0.k == O_NONE ? I(0) : r0;
    }
    case N_NEW: {
        Opnd a[1] = { I(type_size(n->type->elem)) };
        return call_core("mem_zalloc", a, 1, true);
    }
    case N_CONV: break;
    default: break;
    }
    die("internal error: cannot lower expression kind %d as scalar (line %d)", n->kind, n->pos.line);
    return I(0);
}

static void lw_branch(Node *n, int lbl, bool jump_if) {
    switch (n->kind) {
    case N_BOOL:
        if ((n->ival != 0) == jump_if) jmp(lbl);
        return;
    case N_UNARY:
        if (n->op == TK_BANG) { lw_branch(n->a, lbl, !jump_if); return; }
        break;
    case N_BINARY: {
        int op = n->op;
        if (op == TK_ANDAND) {
            if (!jump_if) { lw_branch(n->a, lbl, false); lw_branch(n->b, lbl, false); }
            else { int skip = nlabel(); lw_branch(n->a, skip, false); lw_branch(n->b, lbl, true); label(skip); }
            return;
        }
        if (op == TK_OROR) {
            if (jump_if) { lw_branch(n->a, lbl, true); lw_branch(n->b, lbl, true); }
            else { int skip = nlabel(); lw_branch(n->a, skip, true); lw_branch(n->b, lbl, false); label(skip); }
            return;
        }
        bool cmp = op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE;
        Type *lt = n->a->type;
        if (cmp && lt->kind != TY_F64 && lt->kind != TY_STR) {
            Opnd a = lw_scalar(n->a), b = lw_scalar(n->b);
            int cc = cc_for(op, cmp_signed(lt));
            if (!jump_if) cc = cc_invert(cc);
            if (a.k == O_IMM && b.k == O_IMM) { if (fold_cc(cc, a.v, b.v)) jmp(lbl); return; }
            jcc(cc, a, b, lbl);
            return;
        }
        break;
    }
    default: break;
    }
    Opnd v = lw_scalar(n);
    if (v.k == O_IMM) { if ((v.v != 0) == jump_if) jmp(lbl); return; }
    jcc(jump_if ? CC_NE : CC_EQ, v, I(0), lbl);
}

// ------------------------------------------------------------------ calls
static void print_value(Node *a) {
    Type *t = a->type;
    if (t->kind == TY_STR) { Opnd p, l; lw_pair(a, &p, &l); Opnd ar[2] = { p, l }; call_core("print_str", ar, 2, false); return; }
    Opnd v = lw_scalar(a);
    Opnd ar[1] = { v };
    if (t->kind == TY_F64) call_core("print_f64", ar, 1, false);
    else if (t->kind == TY_BOOL) call_core("print_bool", ar, 1, false);
    else if (t->kind == TY_PTR) call_core("print_ptr", ar, 1, false);
    else if (t->kind == TY_ERRCODE) call_core("print_err", ar, 1, false);
    else if (t->kind == TY_U64 || t->kind == TY_U32 || t->kind == TY_U16 || t->kind == TY_U8) call_core("print_u64", ar, 1, false);
    else call_core("print_i64", ar, 1, false);
}

static void lw_builtin(Node *n, Opnd *r0, Opnd *r1) {
    (void)r1;
    switch (n->aux) {
    case B_PRINT: case B_PRINTLN: case B_EPRINT: case B_EPRINTLN: {
        bool err = n->aux == B_EPRINT || n->aux == B_EPRINTLN;
        if (err) call_core("err_begin", NULL, 0, false);
        for (int i = 0; i < n->list.n; i++) print_value(n->list.d[i]);
        if (n->aux == B_PRINTLN || n->aux == B_EPRINTLN) call_core("print_nl", NULL, 0, false);
        if (err) call_core("err_end", NULL, 0, false);
        return;
    }
    case B_FREE: {
        Node *a = n->list.d[0];
        Opnd p;
        if (a->type->kind == TY_PTR) p = lw_scalar(a);
        else { Opnd l; lw_pair(a, &p, &l); }
        Opnd ar[1] = { p };
        call_core("mem_free", ar, 1, false);
        return;
    }
    case B_SYSCALL: {
        int d = nreg(0);
        Opnd *args = xcalloc(sizeof(Opnd) * 8);
        int na = n->list.n;
        for (int k = 0; k < na; k++) args[k] = lw_scalar(n->list.d[k]);
        Ins *i = emit(OP_SYSCALL);
        i->dst = d; i->args = args; i->nargs = na;
        G.ir->has_call = true;
        *r0 = R(d);
        return;
    }
    case B_SQRT: {
        Opnd v = lw_scalar(n->list.d[0]);
        *r0 = unop(OP_FSQRT, v, 1);
        return;
    }
    case B_MEMCPY: {
        Opnd a = lw_scalar(n->list.d[0]), b = lw_scalar(n->list.d[1]), c = lw_scalar(n->list.d[2]);
        Ins *i = emit(OP_MEMCPY);
        i->args = xcalloc(sizeof(Opnd) * 3); i->nargs = 3;
        i->args[0] = a; i->args[1] = b; i->args[2] = c;
        return;
    }
    case B_MEMZERO: {
        Opnd a = lw_scalar(n->list.d[0]), b = lw_scalar(n->list.d[1]);
        Ins *i = emit(OP_MEMZERO);
        i->args = xcalloc(sizeof(Opnd) * 2); i->nargs = 2;
        i->args[0] = a; i->args[1] = b;
        return;
    }
    case B_MEMGROW: {
        Opnd a = lw_scalar(n->list.d[0]);
        int d = nreg(0);
        Ins *i = emit(OP_MEMGROW); i->dst = d; i->a = a;
        *r0 = R(d);
        return;
    }
    case B_MEMSIZE: {
        int d = nreg(0);
        Ins *i = emit(OP_MEMSIZE); i->dst = d;
        *r0 = R(d);
        return;
    }
    case B_ERRNAME: {
        Opnd code = lw_scalar(n->list.d[0]);
        int c = nreg(0);
        mov(c, code);
        int ok = nlabel();
        jcc(CC_B, R(c), I(g_prog.errors.n + 1), ok);
        mov(c, I(0));
        label(ok);
        Opnd idx = bin(OP_MUL, R(c), I(16), 0);
        Opnd tab = lea((Addr){ A_GLOB, errtab_isym, -1, 1, 0 });
        Opnd addr = bin(OP_ADD, tab, idx, 0);
        Addr ad = areg((int)addr.v, 0);
        *r0 = emit_load(ad, 8, false, 0);
        *r1 = emit_load(aoff(ad, 8), 8, false, 0);
        return;
    }
    }
}

static void lw_call(Node *n, Opnd *r0, Opnd *r1, Addr *sret) {
    if (n->op == CALL_BUILTIN) { lw_builtin(n, r0, r1); return; }
    Type *ft;
    Opnd target;
    Func *callee = NULL;
    if (n->op == CALL_DIRECT) { callee = n->sym->func; ft = callee->type; target = glob(fn_isym[callee->id]); }
    else { ft = n->a->type; target = NONE(); }
    Type *rt = ft->ret;
    TypeClass rc = (rt->kind == TY_VOID || rt->kind == TY_NEVER) ? TC_VOID : type_class(rt);
    Opnd fnp = NONE();
    if (n->op == CALL_INDIRECT) fnp = R(to_reg(lw_scalar(n->a)));
    Opnd *args = xcalloc(sizeof(Opnd) * (size_t)(n->list.n * 2 + 2));
    int na = 0;
    if (rc == TC_MEM) {
        Addr dst;
        if (sret) dst = *sret;
        else dst = aslot(newslot(type_size(rt), type_align(rt)));
        args[na++] = lea(dst);
        if (!sret) { Addr *p = xcalloc(sizeof(Addr)); *p = dst; sret = p; }
    }
    for (int i = 0; i < n->list.n; i++) {
        Node *a = n->list.d[i];
        TypeClass ac = type_class(a->type);
        if (ac == TC_SCALAR) args[na++] = lw_scalar(a);
        else if (ac == TC_PAIR) { Opnd p, l; lw_pair(a, &p, &l); args[na++] = p; args[na++] = l; }
        else if (ac == TC_MEM) {
            int tmp = newslot(type_size(a->type), type_align(a->type));
            lw_into(a, aslot(tmp));
            args[na++] = lea(aslot(tmp));
        }
    }
    int d0 = -1, d1 = -1;
    if (rc == TC_SCALAR) d0 = nreg(cls_of(rt));
    else if (rc == TC_PAIR) { d0 = nreg(0); d1 = nreg(0); }
    Ins *i = emit_call(target.k == O_NONE ? fnp : target, args, na, d0, d1, rt->kind == TY_NEVER);
    (void)i;
    if (d0 >= 0) *r0 = R(d0);
    if (d1 >= 0) *r1 = R(d1);
    if (callee && callee->is_extern && rc == TC_SCALAR && ty_is_int(rt) && ty_bits(rt) < 64) *r0 = norm(*r0, rt);
    if (rc == TC_MEM) *r0 = lea(*sret);
}

// ------------------------------------------------------------------ memory-class initialisation
static void lw_into(Node *n, Addr dst) {
    Type *t = n->type;
    if (t->kind == TY_VOID || t->kind == TY_NEVER) {
        if (n->kind == N_CALL) { Opnd a = NONE(), b = NONE(); lw_call(n, &a, &b, NULL); }
        return;
    }
    TypeClass tc = type_class(t);
    if (tc == TC_SCALAR) { store_scalar(dst, lw_scalar(n), t); return; }
    if (tc == TC_PAIR) {
        Opnd a, b;
        lw_pair(n, &a, &b);
        dst = flatten(dst);
        emit_store(dst, a, 8);
        emit_store(aoff(dst, 8), b, 8);
        return;
    }
    switch (n->kind) {
    case N_STRUCTLIT: {
        if (n->list.n < t->nfields) emit_zero(dst, type_size(t));
        for (int i = 0; i < n->list.n; i++) {
            Node *fi = n->list.d[i];
            lw_into(fi->a, aoff(dst, t->fields[fi->ival].off));
        }
        return;
    }
    case N_ARRAYLIT: {
        int esz = type_size(t->elem);
        for (int i = 0; i < n->list.n; i++) lw_into(n->list.d[i], aoff(dst, i * esz));
        return;
    }
    case N_ARRAYREP: {
        int esz = type_size(t->elem);
        int64_t cnt = t->len;
        if (cnt == 0) return;
        Node *v = n->a;
        if (v->kind == N_INT && v->ival == 0 && type_class(t->elem) != TC_MEM) { emit_zero(dst, type_size(t)); return; }
        dst = flatten(dst);
        lw_into(v, dst);
        if (cnt == 1) return;
        if (cnt * esz <= 256) {
            for (int64_t i = 1; i < cnt; i++) emit_copy(aoff(dst, i * esz), dst, esz);
            return;
        }
        Opnd base = lea(dst);
        int i = nreg(0), top = nlabel(), cond = nlabel();
        mov(i, I(1));
        jmp(cond);
        label(top);
        Opnd off = bin(OP_MUL, R(i), I(esz), 0);
        Opnd p = bin(OP_ADD, base, off, 0);
        emit_copy(areg((int)p.v, 0), dst, esz);
        Ins *inc = emit(OP_ADD); inc->dst = i; inc->a = R(i); inc->b = I(1);
        label(cond);
        jcc(CC_LT, R(i), I(cnt), top);
        return;
    }
    case N_CALL: { Opnd a = NONE(), b = NONE(); lw_call(n, &a, &b, &dst); return; }
    case N_TRY: case N_CATCH: { Opnd a = NONE(), b = NONE(); lw_trycatch(n, &a, &b, &dst); return; }
    case N_CONV:
        if (n->aux == CV_ERRWRAP) {
            emit_store(dst, I(0), 8);
            lw_into(n->a, aoff(dst, eu_payload_off(t)));
            return;
        }
        break;
    default: break;
    }
    emit_copy(dst, lw_addr(n), type_size(t));
}

// ------------------------------------------------------------------ statements
static void init_zero_var(Sym *s) {
    Type *t = s->type;
    TypeClass tc = type_class(t);
    if (tc == TC_SCALAR && s->loc == L_REG) {
        if (t->kind == TY_F64) { Ins *i = emit(OP_FCONST); i->dst = s->r0; i->a = I(0); }
        else mov(s->r0, I(0));
    } else if (tc == TC_PAIR && s->loc == L_PAIR) { mov(s->r0, I(0)); mov(s->r1, I(0)); }
    else emit_zero(aslot(s->slot), type_size(t));
}

static void lw_vardecl(Node *n) {
    Sym *s = n->sym;
    alloc_var(s);
    Type *t = s->type;
    TypeClass tc = type_class(t);
    if (!n->b) { init_zero_var(s); return; }
    if (tc == TC_SCALAR) {
        Opnd v = lw_scalar(n->b);
        if (s->loc == L_REG) mov(s->r0, v); else store_scalar(aslot(s->slot), v, t);
    } else if (tc == TC_PAIR) {
        Opnd a, b;
        lw_pair(n->b, &a, &b);
        if (s->loc == L_PAIR) { mov(s->r0, a); mov(s->r1, b); }
        else { emit_store(aslot(s->slot), a, 8); emit_store(aoff(aslot(s->slot), 8), b, 8); }
    } else lw_into(n->b, aslot(s->slot));
}

static bool is_temp_kind(Node *n) {
    return n->kind == N_STRUCTLIT || n->kind == N_ARRAYLIT || n->kind == N_ARRAYREP || n->kind == N_CALL ||
           n->kind == N_TRY || n->kind == N_CATCH || n->kind == N_CONV;
}

static void lw_assign(Node *s) {
    Node *lhs = s->a;
    Type *t = lhs->type;
    TypeClass tc = type_class(t);
    if (lhs->kind == N_IDENT && lhs->sym->kind == S_VAR && !lhs->sym->global && (lhs->sym->loc == L_REG || lhs->sym->loc == L_PAIR)) {
        Sym *v = lhs->sym;
        if (v->loc == L_REG) {
            Opnd val;
            if (s->op) { Opnd r = lw_scalar(s->b); val = lw_arith(s->op, R(v->r0), r, t, s->b->type, t, s->pos.line); }
            else val = lw_scalar(s->b);
            mov(v->r0, val);
        } else {
            Opnd a, b;
            lw_pair(s->b, &a, &b);
            mov(v->r0, a); mov(v->r1, b);
        }
        return;
    }
    Addr a = lw_addr(lhs);
    if (s->op) {
        Opnd old = load_scalar(a, t);
        Opnd r = lw_scalar(s->b);
        Opnd nv = lw_arith(s->op, old, r, t, s->b->type, t, s->pos.line);
        store_scalar(a, nv, t);
        return;
    }
    if (tc == TC_SCALAR) { Opnd v = lw_scalar(s->b); store_scalar(a, v, t); }
    else if (tc == TC_PAIR) {
        Opnd x, y;
        lw_pair(s->b, &x, &y);
        a = flatten(a);
        emit_store(a, x, 8); emit_store(aoff(a, 8), y, 8);
    } else {
        if (is_temp_kind(s->b)) {
            int tmp = newslot(type_size(t), type_align(t));
            lw_into(s->b, aslot(tmp));
            emit_copy(a, aslot(tmp), type_size(t));
        } else emit_copy(a, lw_addr(s->b), type_size(t));
    }
}

static void bind_loop_var(Sym *s, Opnd *src_for_store, int counter_reg) {
    // read-only loop variable: share the counter register unless its address is taken
    (void)src_for_store;
    if (!s->addr_taken && type_class(s->type) == TC_SCALAR) { s->loc = L_REG; s->r0 = counter_reg; }
    else { s->loc = L_SLOT; s->slot = newslot(type_size(s->type), type_align(s->type)); }
}

static void lw_for(Node *n) {
    int lbody = nlabel(), lcont = nlabel(), lcond = nlabel(), lend = nlabel();
    Loop lp = { lend, lcont };
    if (n->b) {   // range
        Sym *s = n->sym;
        Type *t = s->type;
        Opnd lo = lw_scalar(n->a);
        Opnd hi = lw_scalar(n->b);
        int hr = hi.k == O_IMM ? -1 : fresh_copy(hi, 0);
        Opnd hiop = hr < 0 ? hi : R(hr);
        int ctr = nreg(0);
        mov(ctr, lo);
        bind_loop_var(s, NULL, ctr);
        jmp(lcond);
        label(lbody);
        if (s->loc == L_SLOT) store_scalar(aslot(s->slot), R(ctr), t);
        vpush(G.loops, lp);
        lw_block(n->c);
        G.loops.n--;
        label(lcont);
        Ins *inc = emit(OP_ADD); inc->dst = ctr; inc->a = R(ctr); inc->b = I(1);
        if (ty_bits(t) < 64) { Opnd e = ext(R(ctr), ty_bits(t), ty_is_signed(t)); mov(ctr, e); }
        label(lcond);
        bool sg = ty_is_signed(t);
        jcc(n->flag ? (sg ? CC_LE : CC_BE) : (sg ? CC_LT : CC_B), R(ctr), hiop, lbody);
        label(lend);
        return;
    }
    Type *it = n->a->type;
    Opnd p, len;
    Type *et;
    if (it->kind == TY_ARRAY) { p = lea(lw_addr(n->a)); len = I(it->len); et = it->elem; }
    else { lw_pair(n->a, &p, &len); et = it->kind == TY_STR ? ty_u8 : it->elem; }
    int pr = fresh_copy(p, 0);
    int lr = len.k == O_IMM ? -1 : fresh_copy(len, 0);
    Opnd lenop = lr < 0 ? len : R(lr);
    int ctr = nreg(0);
    mov(ctr, I(0));
    Sym *iv = n->name2 ? n->sym : NULL;
    Sym *ev = n->sym2;
    if (iv) bind_loop_var(iv, NULL, ctr);
    int esz = type_size(et);
    jmp(lcond);
    label(lbody);
    if (iv && iv->loc == L_SLOT) store_scalar(aslot(iv->slot), R(ctr), ty_i64);
    {
        Addr ea = areg(pr, 0);
        if (esz == 1 || esz == 2 || esz == 4 || esz == 8) { ea.idx = ctr; ea.scale = esz; }
        else { ea.idx = (int)bin(OP_MUL, R(ctr), I(esz), 0).v; ea.scale = 1; }
        alloc_var(ev);
        TypeClass ec = type_class(et);
        if (ec == TC_SCALAR) {
            Opnd v = load_scalar(ea, et);
            if (ev->loc == L_REG) mov(ev->r0, v); else store_scalar(aslot(ev->slot), v, et);
        } else if (ec == TC_PAIR) {
            Opnd a, b;
            lw_pair_load(ea, &a, &b);
            if (ev->loc == L_PAIR) { mov(ev->r0, a); mov(ev->r1, b); }
            else { emit_store(aslot(ev->slot), a, 8); emit_store(aoff(aslot(ev->slot), 8), b, 8); }
        } else emit_copy(aslot(ev->slot), ea, esz);
    }
    vpush(G.loops, lp);
    lw_block(n->c);
    G.loops.n--;
    label(lcont);
    { Ins *inc = emit(OP_ADD); inc->dst = ctr; inc->a = R(ctr); inc->b = I(1); }
    label(lcond);
    jcc(CC_LT, R(ctr), lenop, lbody);
    label(lend);
}

static void lw_return(Node *s) {
    Type *r = G.ret;
    TypeClass rc = (r->kind == TY_VOID || r->kind == TY_NEVER) ? TC_VOID : type_class(r);
    if (!s->a) {
        Ins *i = emit(OP_RET);
        if (rc == TC_PAIR) { i->a = I(0); i->b = I(0); }
        return;
    }
    if (rc == TC_SCALAR) { Opnd v = lw_scalar(s->a); Ins *i = emit(OP_RET); i->a = v; }
    else if (rc == TC_PAIR) { Opnd a, b; lw_pair(s->a, &a, &b); Ins *i = emit(OP_RET); i->a = a; i->b = b; }
    else if (rc == TC_MEM) { lw_into(s->a, areg(G.sret, 0)); emit(OP_RET); }
    else emit(OP_RET);
}

static void lw_stmt(Node *s) {
    switch (s->kind) {
    case N_BLOCK: lw_block(s); break;
    case N_VARDECL: lw_vardecl(s); break;
    case N_CONSTDECL: break;
    case N_ASSIGN: lw_assign(s); break;
    case N_EXPRSTMT: {
        Node *e = s->a;
        Type *t = e->type;
        if (t->kind == TY_VOID || t->kind == TY_NEVER) {
            if (e->kind == N_CALL) { Opnd a = NONE(), b = NONE(); lw_call(e, &a, &b, NULL); }
            else if (e->kind == N_TRY || e->kind == N_CATCH) { Opnd a = NONE(), b = NONE(); lw_trycatch(e, &a, &b, NULL); }
            break;
        }
        TypeClass tc = type_class(t);
        if (tc == TC_SCALAR) lw_scalar(e);
        else if (tc == TC_PAIR) { Opnd a, b; lw_pair(e, &a, &b); }
        else { int tmp = newslot(type_size(t), type_align(t)); lw_into(e, aslot(tmp)); }
        break;
    }
    case N_IF: {
        int lend = nlabel();
        if (!s->c) {
            lw_branch(s->a, lend, false);
            lw_block(s->b);
            label(lend);
        } else {
            int lelse = nlabel();
            lw_branch(s->a, lelse, false);
            lw_block(s->b);
            jmp(lend);
            label(lelse);
            if (s->c->kind == N_IF) lw_stmt(s->c); else lw_block(s->c);
            label(lend);
        }
        break;
    }
    case N_WHILE: {
        int lcond = nlabel(), lbody = nlabel(), lend = nlabel();
        Loop lp = { lend, lcond };
        jmp(lcond);
        label(lbody);
        vpush(G.loops, lp);
        lw_block(s->b);
        G.loops.n--;
        label(lcond);
        lw_branch(s->a, lbody, true);
        label(lend);
        break;
    }
    case N_LOOP: {
        int ltop = nlabel(), lend = nlabel();
        Loop lp = { lend, ltop };
        label(ltop);
        vpush(G.loops, lp);
        lw_block(s->b);
        G.loops.n--;
        jmp(ltop);
        label(lend);
        break;
    }
    case N_FOR: lw_for(s); break;
    case N_BREAK: jmp(G.loops.d[G.loops.n - 1].brk); break;
    case N_CONTINUE: jmp(G.loops.d[G.loops.n - 1].cont); break;
    case N_RETURN: lw_return(s); break;
    case N_FAIL: { Opnd e = lw_scalar(s->a); emit_return_err(e); break; }
    default: die("internal error: cannot lower statement kind %d", s->kind);
    }
}

static void lw_block(Node *b) {
    for (int i = 0; i < b->list.n; i++) lw_stmt(b->list.d[i]);
}

// ------------------------------------------------------------------ functions & program
static void lower_func(Func *f) {
    FnIR *ir = xcalloc(sizeof(FnIR));
    ir->f = f; ir->name = f->asm_name;
    memset(&G, 0, sizeof G);
    G.ir = ir; G.f = f; G.ret = f->type->ret; G.sret = -1; G.file = f->decl->pos.file;
    Node *d = f->decl;
    int pidx = 0;
    Type *r = G.ret;
    if (r->kind != TY_VOID && r->kind != TY_NEVER && type_class(r) == TC_MEM) {
        G.sret = nreg(0);
        Ins *i = emit(OP_PARAM); i->dst = G.sret; i->a = I(pidx++);
    }
    for (int k = 0; k < d->list.n; k++) {
        Sym *s = d->list.d[k]->sym;
        Type *t = s->type;
        TypeClass tc = type_class(t);
        if (tc == TC_SCALAR) {
            int rg = nreg(cls_of(t));
            Ins *i = emit(OP_PARAM); i->dst = rg; i->a = I(pidx++);
            if (!s->addr_taken) { s->loc = L_REG; s->r0 = rg; }
            else { s->loc = L_SLOT; s->slot = newslot(type_size(t), type_align(t)); store_scalar(aslot(s->slot), R(rg), t); }
        } else if (tc == TC_PAIR) {
            int r0 = nreg(0), r1 = nreg(0);
            Ins *i = emit(OP_PARAM); i->dst = r0; i->a = I(pidx++);
            i = emit(OP_PARAM); i->dst = r1; i->a = I(pidx++);
            if (!s->addr_taken) { s->loc = L_PAIR; s->r0 = r0; s->r1 = r1; }
            else { s->loc = L_SLOT; s->slot = newslot(16, 8); emit_store(aslot(s->slot), R(r0), 8); emit_store(aoff(aslot(s->slot), 8), R(r1), 8); }
        } else {
            int rg = nreg(0);
            Ins *i = emit(OP_PARAM); i->dst = rg; i->a = I(pidx++);
            s->loc = L_PTR; s->r0 = rg;
        }
    }
    ir->nparams = pidx;
    lw_block(d->b);
    {
        Ins *i = emit(OP_RET);
        if (r->kind != TY_VOID && r->kind != TY_NEVER && type_class(r) == TC_PAIR) { i->a = I(0); i->b = I(0); }
    }
    vpush(g_irs, ir);
}

void lower_program(void) {
    fn_isym = xcalloc(sizeof(int) * (size_t)(g_prog.funcs.n + 1));
    glob_isym = xcalloc(sizeof(int) * (size_t)(g_prog.globals.n + 1));
    errtab_isym = 0;
    for (int i = 0; i < g_prog.funcs.n; i++) {
        Func *f = g_prog.funcs.d[i];
        ISym s = {0};
        s.kind = IS_FUNC; s.name = f->asm_name; s.func = f;
        vpush(g_isyms, s);
        fn_isym[f->id] = g_isyms.n - 1;
    }
    for (int i = 0; i < g_prog.globals.n; i++) {
        Sym *g = g_prog.globals.d[i];
        ISym s = {0};
        s.kind = IS_DATA; s.name = xsprintf("orb_g_%s_%s_%d", g->mod->name, g->name, i); s.gsym = g;
        if (g->cv.kind == 4) str_sym(g->cv.s, g->cv.slen, false);
        vpush(g_isyms, s);
        glob_isym[i] = g_isyms.n - 1;
    }
    {
        ISym s = {0};
        s.kind = IS_ERRTAB; s.name = "orb_errtab";
        vpush(g_isyms, s);
        errtab_isym = g_isyms.n - 1;
    }
    for (int i = 0; i < g_prog.funcs.n; i++) {
        Func *f = g_prog.funcs.d[i];
        if (f->is_extern) continue;
        lower_func(f);
    }
}

// ------------------------------------------------------------------ IR dump
static const char *opnames[OP__COUNT] = {
    "nop", "label", "jmp", "jcc", "mov", "fconst", "add", "sub", "mul", "div", "udiv", "mod", "umod",
    "and", "or", "xor", "shl", "shr", "sar", "neg", "not", "sext", "zext", "set",
    "fadd", "fsub", "fmul", "fdiv", "fneg", "fsqrt", "fset", "i2f", "u2f", "f2i", "f2u",
    "load", "store", "lea", "call", "ret", "syscall", "memcpy", "memzero", "param", "memgrow", "memsize",
};
const char *cc_name(int cc) {
    static const char *n[] = { "e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae" };
    return n[cc];
}
static void dump_opnd(FILE *f, Opnd o) {
    switch (o.k) {
    case O_REG: fprintf(f, "v%lld", (long long)o.v); break;
    case O_IMM: fprintf(f, "%lld", (long long)o.v); break;
    case O_SLOT: fprintf(f, "slot%lld", (long long)o.v); break;
    case O_GLOB: fprintf(f, "@%s", g_isyms.d[o.v].name); break;
    default: fprintf(f, "_");
    }
}
void ir_dump(FILE *f, FnIR *ir) {
    fprintf(f, "func %s  (vregs=%d slots=%d)\n", ir->name, ir->cls.n, ir->slots.n);
    for (int k = 0; k < ir->ins.n; k++) {
        Ins *i = &ir->ins.d[k];
        if (i->op == OP_NOP) continue;
        if (i->op == OP_LABEL) { fprintf(f, " L%d:\n", i->label); continue; }
        fprintf(f, "    %-7s", opnames[i->op]);
        if (i->dst >= 0) fprintf(f, " v%d", i->dst);
        if (i->dst2 >= 0) fprintf(f, ", v%d", i->dst2);
        if (i->op == OP_JCC || i->op == OP_SET || i->op == OP_FSET) fprintf(f, " [%s]", cc_name(i->cc));
        if (i->a.k) { fprintf(f, " "); dump_opnd(f, i->a); }
        if (i->b.k) { fprintf(f, ", "); dump_opnd(f, i->b); }
        if (i->c.k) { fprintf(f, ", "); dump_opnd(f, i->c); }
        if (i->op == OP_LOAD || i->op == OP_STORE || i->op == OP_LEA) fprintf(f, "  *%d+%lld w%d", i->scale, (long long)i->off, i->w);
        if (i->op == OP_JMP || i->op == OP_JCC) fprintf(f, " -> L%d", i->label);
        if (i->nargs) { fprintf(f, " ("); for (int a = 0; a < i->nargs; a++) { if (a) fprintf(f, ", "); dump_opnd(f, i->args[a]); } fprintf(f, ")"); }
        fprintf(f, "\n");
    }
}
