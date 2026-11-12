// Liveness analysis, linear-scan register allocation and x86-64 (AT&T) emission.
#include "sema.h"
#include "ir.h"
#include <limits.h>
#include <math.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
static const char *R64[16] = { "%rax","%rcx","%rdx","%rbx","%rsp","%rbp","%rsi","%rdi","%r8","%r9","%r10","%r11","%r12","%r13","%r14","%r15" };
static const char *R32[16] = { "%eax","%ecx","%edx","%ebx","%esp","%ebp","%esi","%edi","%r8d","%r9d","%r10d","%r11d","%r12d","%r13d","%r14d","%r15d" };
static const char *R16[16] = { "%ax","%cx","%dx","%bx","%sp","%bp","%si","%di","%r8w","%r9w","%r10w","%r11w","%r12w","%r13w","%r14w","%r15w" };
static const char *RB8[16] = { "%al","%cl","%dl","%bl","%spl","%bpl","%sil","%dil","%r8b","%r9b","%r10b","%r11b","%r12b","%r13b","%r14b","%r15b" };
static const int ARGREG[6] = { RDI, RSI, RDX, RCX, R8, R9 };
static const char *CCN[] = { "e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae" };
static char XMMN[16][8];

enum { LK_NONE, LK_GPR, LK_XMM, LK_STACK };
typedef struct { int kind, r, off; } Loc;

static FILE *out;

// Symbol / label naming per object format (Mach-O prefixes globals with '_' and uses 'L' for local labels).
static const char *sy(const char *n) {
    if (g_target == TGT_MACOS) {
        if (n[0] == '.' && n[1] == 'L') return xsprintf("L%s", n + 2);
        return xsprintf("_%s", n);
    }
    return n;
}
static const char *lp(void) { return g_target == TGT_MACOS ? "L" : ".L"; }
static FnIR *ir;
static Loc *loc;
static int fid;
static int *slot_off;
static int saved_regs[5], nsaved;

static void E(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputc('\t', out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    va_end(ap);
}

// ================================================================= liveness & allocation
typedef struct { int start, end; bool crosses; int hint; int phys; } Interval;

static bool is_callish(Ins *i) {
    return (i->op == OP_CALL && !i->noret) || i->op == OP_SYSCALL || i->op == OP_MEMCPY || i->op == OP_MEMZERO;
}

typedef struct { int start, end, succ[2], nsucc; } Block;

typedef struct { uint64_t *use, *def, *in, *out; int words; } Bits;

static Interval *ivs;
static int nvregs;

static void touch(int v, int pos) {
    if (pos < ivs[v].start) ivs[v].start = pos;
    if (pos > ivs[v].end) ivs[v].end = pos;
}

static void compute_intervals(void) {
    int n = ir->ins.n;
    nvregs = ir->cls.n;
    ivs = xcalloc(sizeof(Interval) * (size_t)(nvregs + 1));
    for (int v = 0; v < nvregs; v++) { ivs[v].start = INT_MAX; ivs[v].end = -1; ivs[v].hint = -1; ivs[v].phys = -1; }
    if (n == 0) return;
    // blocks
    bool *leader = xcalloc((size_t)n + 1);
    leader[0] = true;
    int maxlabel = ir->nlabels + 1;
    int *label_ins = xcalloc(sizeof(int) * (size_t)maxlabel);
    for (int k = 0; k < maxlabel; k++) label_ins[k] = -1;
    for (int k = 0; k < n; k++) {
        Ins *i = &ir->ins.d[k];
        if (i->op == OP_LABEL) { leader[k] = true; label_ins[i->label] = k; }
        if ((i->op == OP_JMP || i->op == OP_JCC || i->op == OP_RET || (i->op == OP_CALL && i->noret)) && k + 1 < n) leader[k + 1] = true;
    }
    int nb = 0;
    int *blk_of = xcalloc(sizeof(int) * (size_t)n);
    for (int k = 0; k < n; k++) { if (leader[k]) nb++; blk_of[k] = nb - 1; }
    Block *bl = xcalloc(sizeof(Block) * (size_t)nb);
    for (int k = 0, b = -1; k < n; k++) {
        if (leader[k]) { b++; bl[b].start = k; }
        bl[b].end = k;
    }
    for (int b = 0; b < nb; b++) {
        Ins *last = &ir->ins.d[bl[b].end];
        bl[b].nsucc = 0;
        if (last->op == OP_JMP) { bl[b].succ[bl[b].nsucc++] = blk_of[label_ins[last->label]]; }
        else if (last->op == OP_JCC) {
            bl[b].succ[bl[b].nsucc++] = blk_of[label_ins[last->label]];
            if (b + 1 < nb) bl[b].succ[bl[b].nsucc++] = b + 1;
        } else if (last->op == OP_RET || (last->op == OP_CALL && last->noret)) {}
        else if (b + 1 < nb) bl[b].succ[bl[b].nsucc++] = b + 1;
    }
    int words = (nvregs + 63) / 64;
    uint64_t *pool = xcalloc(sizeof(uint64_t) * (size_t)words * (size_t)nb * 4 + 8);
    Bits *bs = xcalloc(sizeof(Bits) * (size_t)nb);
    for (int b = 0; b < nb; b++) {
        bs[b].words = words;
        bs[b].use = pool + (size_t)(b * 4 + 0) * (size_t)words;
        bs[b].def = pool + (size_t)(b * 4 + 1) * (size_t)words;
        bs[b].in = pool + (size_t)(b * 4 + 2) * (size_t)words;
        bs[b].out = pool + (size_t)(b * 4 + 3) * (size_t)words;
    }
    // use/def per block; also interval touches at instruction positions
    for (int b = 0; b < nb; b++) {
        for (int k = bl[b].start; k <= bl[b].end; k++) {
            Ins *i = &ir->ins.d[k];
            int pos = k;
            // uses
            Opnd *ops[3] = { &i->a, &i->b, &i->c };
            for (int q = 0; q < 3; q++) if (ops[q]->k == O_REG) {
                int v = (int)ops[q]->v;
                if (!(bs[b].def[v / 64] >> (v % 64) & 1)) bs[b].use[v / 64] |= 1ull << (v % 64);
                touch(v, pos);
            }
            for (int q = 0; q < i->nargs; q++) if (i->args[q].k == O_REG) {
                int v = (int)i->args[q].v;
                if (!(bs[b].def[v / 64] >> (v % 64) & 1)) bs[b].use[v / 64] |= 1ull << (v % 64);
                touch(v, pos);
            }
            int ds[2] = { i->dst, i->dst2 };
            for (int q = 0; q < 2; q++) if (ds[q] >= 0) {
                int v = ds[q];
                bs[b].def[v / 64] |= 1ull << (v % 64);
                touch(v, i->op == OP_PARAM ? 0 : pos);
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = nb - 1; b >= 0; b--) {
            for (int w = 0; w < words; w++) {
                uint64_t o = 0;
                for (int s = 0; s < bl[b].nsucc; s++) o |= bs[bl[b].succ[s]].in[w];
                uint64_t in = bs[b].use[w] | (o & ~bs[b].def[w]);
                if (o != bs[b].out[w] || in != bs[b].in[w]) changed = true;
                bs[b].out[w] = o; bs[b].in[w] = in;
            }
        }
    }
    for (int b = 0; b < nb; b++)
        for (int w = 0; w < words; w++) {
            uint64_t x = bs[b].in[w];
            while (x) { int bit = __builtin_ctzll(x); x &= x - 1; touch(w * 64 + bit, bl[b].start); }
            x = bs[b].out[w];
            while (x) { int bit = __builtin_ctzll(x); x &= x - 1; touch(w * 64 + bit, bl[b].end); }
        }
    // parameters must stay alive together
    int np = 0;
    for (int k = 0; k < n && ir->ins.d[k].op == OP_PARAM; k++) np++;
    for (int k = 0; k < np; k++) touch(ir->ins.d[k].dst, np);
    // call crossing
    int *cum = xcalloc(sizeof(int) * (size_t)(n + 1));
    for (int k = 0; k < n; k++) cum[k] = (k ? cum[k - 1] : 0) + (is_callish(&ir->ins.d[k]) ? 1 : 0);
    for (int v = 0; v < nvregs; v++) {
        if (ivs[v].end < 0) continue;
        int s = ivs[v].start, e = ivs[v].end;
        ivs[v].crosses = (e - 1 >= s + 1) && (cum[e - 1] - cum[s] > 0);
    }
    // hints: dst of mov/alu prefers the register of its first source
    for (int k = 0; k < n; k++) {
        Ins *i = &ir->ins.d[k];
        if (i->dst >= 0 && i->a.k == O_REG && (i->op == OP_MOV || (i->op >= OP_ADD && i->op <= OP_NOT) || (i->op >= OP_FADD && i->op <= OP_FSQRT) || i->op == OP_SEXT || i->op == OP_ZEXT))
            if (ivs[i->dst].hint < 0) ivs[i->dst].hint = (int)i->a.v;
    }
    // call arguments that die at the call prefer their argument register
    for (int k = 0; k < n; k++) {
        Ins *i = &ir->ins.d[k];
        if (i->op != OP_CALL) continue;
        for (int q = 0; q < i->nargs && q < 6; q++) {
            if (i->args[q].k != O_REG) continue;
            int v = (int)i->args[q].v;
            if (ivs[v].end == k && ir->cls.d[v] == 0 && !ivs[v].crosses && ivs[v].phys < 0) ivs[v].phys = ARGREG[q];
        }
    }
    free(leader); free(label_ins); free(blk_of); free(bl); free(pool); free(bs); free(cum);
}

static int cmp_start(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (ivs[x].start != ivs[y].start) return ivs[x].start - ivs[y].start;
    return x - y;
}

static int nspill;
static void spill(int v) { loc[v].kind = LK_STACK; loc[v].off = nspill++; }

static void allocate(void) {
    loc = xcalloc(sizeof(Loc) * (size_t)(nvregs + 1));
    int *order = xcalloc(sizeof(int) * (size_t)(nvregs + 1));
    int cnt = 0;
    for (int v = 0; v < nvregs; v++) if (ivs[v].end >= 0) order[cnt++] = v;
    qsort(order, (size_t)cnt, sizeof(int), cmp_start);
    static const int caller[] = { RSI, RDI, R8, R9, R10, R11 };
    static const int callee[] = { RBX, R12, R13, R14, R15 };
    int owner_g[16], owner_x[16];
    for (int r = 0; r < 16; r++) { owner_g[r] = -1; owner_x[r] = -1; }
    nspill = 0;
    for (int oi = 0; oi < cnt; oi++) {
        int v = order[oi];
        int cls = ir->cls.d[v];
        // expire
        for (int r = 0; r < 16; r++) {
            if (owner_g[r] >= 0 && ivs[owner_g[r]].end <= ivs[v].start) owner_g[r] = -1;
            if (owner_x[r] >= 0 && ivs[owner_x[r]].end <= ivs[v].start) owner_x[r] = -1;
        }
        int *owner = cls ? owner_x : owner_g;
        int pick = -1;
        // candidate registers
        int cand[16], nc = 0;
        if (cls) { if (!ivs[v].crosses) for (int r = 2; r < 16; r++) cand[nc++] = r; }
        else {
            if (!ivs[v].crosses) for (int k = 0; k < 6; k++) cand[nc++] = caller[k];
            for (int k = 0; k < 5; k++) cand[nc++] = callee[k];
        }
        if (ivs[v].phys >= 0 && !cls) {
            for (int k = 0; k < nc; k++) if (cand[k] == ivs[v].phys && owner[cand[k]] < 0) { pick = cand[k]; break; }
        }
        int h = ivs[v].hint;
        if (pick < 0 && h >= 0 && loc[h].kind == (cls ? LK_XMM : LK_GPR) && ir->cls.d[h] == cls) {
            for (int k = 0; k < nc; k++) if (cand[k] == loc[h].r && owner[cand[k]] < 0) { pick = cand[k]; break; }
        }
        if (pick < 0) for (int k = 0; k < nc; k++) if (owner[cand[k]] < 0) { pick = cand[k]; break; }
        if (pick < 0) {
            // steal from the active interval that ends last
            int best = -1;
            for (int k = 0; k < nc; k++) {
                int o = owner[cand[k]];
                if (o >= 0 && (best < 0 || ivs[o].end > ivs[owner[best]].end)) best = cand[k];
            }
            if (best >= 0 && ivs[owner[best]].end > ivs[v].end) {
                int o = owner[best];
                spill(o);
                owner[best] = -1;
                pick = best;
            }
        }
        if (pick < 0) { spill(v); continue; }
        owner[pick] = v;
        loc[v].kind = cls ? LK_XMM : LK_GPR;
        loc[v].r = pick;
    }
    free(order);
}

// ================================================================= emission helpers
static Loc L(int v) { return loc[v]; }
static bool fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }
static char *stack_str(int v) { return xsprintf("%d(%%rbp)", slot_off[ir->slots.n + loc[v].off]); }
static int sym_used_id(Opnd o) { return (int)o.v; }

static void lea_glob(Opnd o, int reg) { E("leaq %s(%%rip), %s", sy(g_isyms.d[sym_used_id(o)].name), R64[reg]); }

static void mov_to_gpr(Opnd o, int reg) {
    switch (o.k) {
    case O_IMM:
        if (o.v == 0) E("xorl %s, %s", R32[reg], R32[reg]);
        else if (o.v > 0 && o.v <= UINT32_MAX) E("movl $%lld, %s", (long long)o.v, R32[reg]);
        else if (fits32(o.v)) E("movq $%lld, %s", (long long)o.v, R64[reg]);
        else E("movabsq $%lld, %s", (long long)o.v, R64[reg]);
        break;
    case O_GLOB: lea_glob(o, reg); break;
    case O_REG: {
        Loc l = L((int)o.v);
        if (l.kind == LK_GPR) { if (l.r != reg) E("movq %s, %s", R64[l.r], R64[reg]); }
        else if (l.kind == LK_STACK) E("movq %s, %s", stack_str((int)o.v), R64[reg]);
        else if (l.kind == LK_XMM) E("movq %s, %s", XMMN[l.r], R64[reg]);
        else E("# BUG: unallocated vreg v%d", (int)o.v);
        break;
    }
    default: E("# BUG: bad operand kind %d", o.k);
    }
}
// returns a string usable as a source operand (register, memory or imm32). May load into scratch.
static const char *src(Opnd o, int scratch) {
    if (o.k == O_IMM && fits32(o.v)) return xsprintf("$%lld", (long long)o.v);
    if (o.k == O_REG) {
        Loc l = L((int)o.v);
        if (l.kind == LK_GPR) return R64[l.r];
        if (l.kind == LK_STACK) return stack_str((int)o.v);
    }
    mov_to_gpr(o, scratch);
    return R64[scratch];
}
static void store_gpr(int reg, int v) {
    Loc l = L(v);
    if (l.kind == LK_GPR) { if (l.r != reg) E("movq %s, %s", R64[reg], R64[l.r]); }
    else if (l.kind == LK_STACK) E("movq %s, %s", R64[reg], stack_str(v));
    else if (l.kind == LK_XMM) E("movq %s, %s", R64[reg], XMMN[l.r]);
}
static int dst_gpr(int v, int scratch) { Loc l = L(v); return l.kind == LK_GPR ? l.r : scratch; }

static bool same_loc_opnd(Opnd a, int v) {
    if (a.k != O_REG) return false;
    Loc x = L((int)a.v), y = L(v);
    return x.kind == y.kind && x.r == y.r && (x.kind != LK_STACK || x.off == y.off);
}

// ---- parallel moves into gprs
typedef struct { int dst; Opnd src; int srcgpr; bool done; } PM;
static void parallel_move(PM *m, int n, int tmp) {
    for (int i = 0; i < n; i++) {
        m[i].srcgpr = -1;
        if (m[i].src.k == O_REG && L((int)m[i].src.v).kind == LK_GPR) m[i].srcgpr = L((int)m[i].src.v).r;
        if (m[i].srcgpr == m[i].dst) m[i].done = true;
    }
    for (;;) {
        bool progress = false, pending = false;
        for (int i = 0; i < n; i++) {
            if (m[i].done) continue;
            pending = true;
            bool blocked = false;
            for (int j = 0; j < n; j++) if (j != i && !m[j].done && m[j].srcgpr == m[i].dst) { blocked = true; break; }
            if (blocked) continue;
            if (m[i].srcgpr >= 0) E("movq %s, %s", R64[m[i].srcgpr], R64[m[i].dst]);
            else mov_to_gpr(m[i].src, m[i].dst);
            m[i].done = true; progress = true;
        }
        if (!pending) break;
        if (!progress) {
            int pick = -1;
            for (int i = 0; i < n; i++) if (!m[i].done && m[i].srcgpr >= 0) { pick = i; break; }
            if (pick < 0) { E("# BUG: parallel move deadlock"); break; }
            int s = m[pick].srcgpr;
            E("movq %s, %s", R64[s], R64[tmp]);
            for (int j = 0; j < n; j++) if (!m[j].done && m[j].srcgpr == s) m[j].srcgpr = tmp;
        }
    }
}

// ---- memory operands
static const char *mem_str(Ins *i) {
    Opnd base = i->a, idx = i->b;
    int64_t off = i->off;
    char *s;
    if (base.k == O_GLOB) {
        if (idx.k == O_REG) { lea_glob(base, RCX); s = xsprintf("%lld(%s", (long long)off, R64[RCX]); }
        else return xsprintf("%s%+lld(%%rip)", sy(g_isyms.d[base.v].name), (long long)off);
    } else if (base.k == O_SLOT) {
        s = xsprintf("%lld(%%rbp", (long long)(off + slot_off[base.v]));
    } else {
        Loc l = L((int)base.v);
        int r;
        if (l.kind == LK_GPR) r = l.r;
        else { mov_to_gpr(base, RCX); r = RCX; }
        s = xsprintf("%lld(%s", (long long)off, R64[r]);
    }
    if (idx.k == O_REG) {
        Loc l = L((int)idx.v);
        int r;
        if (l.kind == LK_GPR) r = l.r;
        else { mov_to_gpr(idx, RDX); r = RDX; }
        s = xsprintf("%s,%s,%d)", s, R64[r], i->scale);
    } else s = xsprintf("%s)", s);
    return s;
}

// ---- xmm helpers
static int xdst(int v) { Loc l = L(v); return l.kind == LK_XMM ? l.r : 0; }
static void xstore(int x, int v) {
    Loc l = L(v);
    if (l.kind == LK_XMM) { if (l.r != x) E("movaps %s, %s", XMMN[x], XMMN[l.r]); }
    else if (l.kind == LK_STACK) E("movsd %s, %s", XMMN[x], stack_str(v));
    else if (l.kind == LK_GPR) E("movq %s, %s", XMMN[x], R64[l.r]);
}
// load float operand into xmm register (returns register index)
static int xsrc_reg(Opnd o, int scratch) {
    if (o.k == O_REG) {
        Loc l = L((int)o.v);
        if (l.kind == LK_XMM) return l.r;
        if (l.kind == LK_STACK) { E("movsd %s, %s", stack_str((int)o.v), XMMN[scratch]); return scratch; }
        if (l.kind == LK_GPR) { E("movq %s, %s", R64[l.r], XMMN[scratch]); return scratch; }
    }
    if (o.k == O_IMM) { mov_to_gpr(o, RAX); E("movq %%rax, %s", XMMN[scratch]); return scratch; }
    return scratch;
}
static const char *xsrc_str(Opnd o, int scratch) {
    if (o.k == O_REG) {
        Loc l = L((int)o.v);
        if (l.kind == LK_XMM) return XMMN[l.r];
        if (l.kind == LK_STACK) return stack_str((int)o.v);
    }
    return XMMN[xsrc_reg(o, scratch)];
}

// ================================================================= instruction emission
static void emit_cmp(Opnd a, Opnd b) {
    const char *as, *bs;
    if (a.k == O_IMM || (a.k == O_REG && L((int)a.v).kind != LK_GPR && L((int)a.v).kind != LK_STACK)) { mov_to_gpr(a, RAX); as = R64[RAX]; }
    else as = src(a, RAX);
    bool a_mem = a.k == O_REG && L((int)a.v).kind == LK_STACK;
    if (b.k == O_IMM && b.v == 0 && !a_mem && a.k == O_REG && L((int)a.v).kind == LK_GPR) { E("testq %s, %s", as, as); return; }
    if (b.k == O_REG && L((int)b.v).kind == LK_STACK && a_mem) { mov_to_gpr(a, RAX); as = R64[RAX]; }
    bs = src(b, RCX);
    E("cmpq %s, %s", bs, as);
}

static void emit_alu(Ins *i, const char *m, bool commut) {
    Opnd a = i->a, b = i->b;
    Loc ld = L(i->dst);
    int t = dst_gpr(i->dst, RAX);
    if (b.k == O_REG && L((int)b.v).kind == LK_GPR && L((int)b.v).r == t && !same_loc_opnd(a, i->dst)) {
        if (commut) { Opnd x = a; a = b; b = x; }
        else t = RAX;
    }
    // `add $imm, reg` -> lea when destination differs from source
    if (!strcmp(m, "addq") && ld.kind == LK_GPR && a.k == O_REG && L((int)a.v).kind == LK_GPR && L((int)a.v).r != t && b.k == O_IMM && fits32(b.v)) {
        E("leaq %lld(%s), %s", (long long)b.v, R64[L((int)a.v).r], R64[t]);
        return;
    }
    mov_to_gpr(a, t);
    if (!strcmp(m, "imulq") && b.k == O_IMM && fits32(b.v)) E("imulq $%lld, %s, %s", (long long)b.v, R64[t], R64[t]);
    else E("%s %s, %s", m, src(b, RCX), R64[t]);
    if (!(ld.kind == LK_GPR && ld.r == t)) store_gpr(t, i->dst);
}

static void emit_div(Ins *i, bool sg, bool want_rem) {
    mov_to_gpr(i->a, RAX);
    const char *bs;
    if (i->b.k == O_IMM) { mov_to_gpr(i->b, RCX); bs = R64[RCX]; }
    else bs = src(i->b, RCX);
    if (sg) { E("cqto"); E("idivq %s", bs); }
    else { E("xorl %%edx, %%edx"); E("divq %s", bs); }
    store_gpr(want_rem ? RDX : RAX, i->dst);
}

static void emit_shift(Ins *i, const char *m) {
    Loc ld = L(i->dst);
    int t = dst_gpr(i->dst, RAX);
    if (i->b.k == O_IMM) {
        mov_to_gpr(i->a, t);
        E("%s $%lld, %s", m, (long long)(i->b.v & 63), R64[t]);
    } else {
        mov_to_gpr(i->b, RCX);
        mov_to_gpr(i->a, t);
        E("%s %%cl, %s", m, R64[t]);
    }
    if (!(ld.kind == LK_GPR && ld.r == t)) store_gpr(t, i->dst);
}

static void emit_ext(Ins *i) {
    int t = dst_gpr(i->dst, RAX);
    Opnd a = i->a;
    bool sg = i->op == OP_SEXT;
    int w = i->w;
    const char *from;
    if (a.k == O_REG && L((int)a.v).kind == LK_GPR) {
        int r = L((int)a.v).r;
        from = w == 1 ? RB8[r] : w == 2 ? R16[r] : R32[r];
    } else if (a.k == O_REG && L((int)a.v).kind == LK_STACK) {
        from = stack_str((int)a.v);
    } else { mov_to_gpr(a, t); int r = t; from = w == 1 ? RB8[r] : w == 2 ? R16[r] : R32[r]; }
    if (sg) E("%s %s, %s", w == 1 ? "movsbq" : w == 2 ? "movswq" : "movslq", from, R64[t]);
    else if (w == 4) E("movl %s, %s", from, R32[t]);
    else E("%s %s, %s", w == 1 ? "movzbl" : "movzwl", from, R32[t]);
    store_gpr(t, i->dst);
}

static void emit_setcc(Ins *i) {
    emit_cmp(i->a, i->b);
    int t = dst_gpr(i->dst, RAX);
    E("set%s %s", CCN[i->cc], RB8[t]);
    E("movzbl %s, %s", RB8[t], R32[t]);
    store_gpr(t, i->dst);
}

static void emit_fset(Ins *i) {
    int cc = i->cc;
    Opnd a = i->a, b = i->b;
    bool swap = cc == CC_LT || cc == CC_LE;
    if (swap) { Opnd x = a; a = b; b = x; cc = cc == CC_LT ? CC_GT : CC_GE; }
    int ra = xsrc_reg(a, 0);
    const char *bs = xsrc_str(b, 1);
    E("ucomisd %s, %s", bs, XMMN[ra]);
    int t = dst_gpr(i->dst, RAX);
    if (cc == CC_EQ) { E("sete %%al"); E("setnp %%cl"); E("andb %%cl, %%al"); }
    else if (cc == CC_NE) { E("setne %%al"); E("setp %%cl"); E("orb %%cl, %%al"); }
    else E("set%s %%al", cc == CC_GT ? "a" : "ae");
    E("movzbl %%al, %s", R32[t]);
    store_gpr(t, i->dst);
}

static void emit_fbin(Ins *i, const char *m, bool commut) {
    Opnd a = i->a, b = i->b;
    int t = xdst(i->dst);
    Loc ld = L(i->dst);
    if (b.k == O_REG && L((int)b.v).kind == LK_XMM && L((int)b.v).r == t && !same_loc_opnd(a, i->dst)) {
        if (commut) { Opnd x = a; a = b; b = x; } else t = 0;
    }
    int ra = xsrc_reg(a, t);
    if (ra != t) E("movaps %s, %s", XMMN[ra], XMMN[t]);
    E("%s %s, %s", m, xsrc_str(b, 1), XMMN[t]);
    if (!(ld.kind == LK_XMM && ld.r == t)) xstore(t, i->dst);
}

static void emit_load(Ins *i) {
    const char *m = mem_str(i);
    if (ir->cls.d[i->dst] == 1) {
        int t = xdst(i->dst);
        E("movsd %s, %s", m, XMMN[t]);
        if (L(i->dst).kind != LK_XMM) xstore(t, i->dst);
        return;
    }
    int t = dst_gpr(i->dst, RAX);
    switch (i->w) {
    case 1: E(i->sg ? "movsbq %s, %s" : "movzbl %s, %s", m, i->sg ? R64[t] : R32[t]); break;
    case 2: E(i->sg ? "movswq %s, %s" : "movzwl %s, %s", m, i->sg ? R64[t] : R32[t]); break;
    case 4: if (i->sg) E("movslq %s, %s", m, R64[t]); else E("movl %s, %s", m, R32[t]); break;
    default: E("movq %s, %s", m, R64[t]); break;
    }
    if (L(i->dst).kind != LK_GPR) store_gpr(t, i->dst);
}

static void emit_store_ins(Ins *i) {
    Opnd v = i->c;
    const char *m;
    int w = i->w;
    if (v.k == O_REG && ir->cls.d[v.v] == 1 && L((int)v.v).kind == LK_XMM) {
        m = mem_str(i);
        E("movsd %s, %s", XMMN[L((int)v.v).r], m);
        return;
    }
    if (v.k == O_IMM && (w < 8 || fits32(v.v))) {
        m = mem_str(i);
        int64_t x = v.v;
        if (w == 1) E("movb $%lld, %s", (long long)(int8_t)x, m);
        else if (w == 2) E("movw $%lld, %s", (long long)(int16_t)x, m);
        else if (w == 4) E("movl $%lld, %s", (long long)(int32_t)x, m);
        else E("movq $%lld, %s", (long long)x, m);
        return;
    }
    int r;
    if (v.k == O_REG && L((int)v.v).kind == LK_GPR) r = L((int)v.v).r;
    else { mov_to_gpr(v, RAX); r = RAX; }
    m = mem_str(i);
    if (w == 1) E("movb %s, %s", RB8[r], m);
    else if (w == 2) E("movw %s, %s", R16[r], m);
    else if (w == 4) E("movl %s, %s", R32[r], m);
    else E("movq %s, %s", R64[r], m);
}

static void emit_mov(Ins *i) {
    Opnd a = i->a;
    Loc ld = L(i->dst);
    int cls = ir->cls.d[i->dst];
    if (cls == 1) {
        if (same_loc_opnd(a, i->dst)) return;
        if (a.k == O_REG && L((int)a.v).kind == LK_XMM) { xstore(L((int)a.v).r, i->dst); return; }
        if (ld.kind == LK_XMM) { int r = xsrc_reg(a, ld.r); if (r != ld.r) E("movaps %s, %s", XMMN[r], XMMN[ld.r]); return; }
        mov_to_gpr(a, RAX); store_gpr(RAX, i->dst);
        return;
    }
    if (same_loc_opnd(a, i->dst)) return;
    if (ld.kind == LK_GPR) { mov_to_gpr(a, ld.r); return; }
    if (a.k == O_IMM && fits32(a.v)) { E("movq $%lld, %s", (long long)a.v, stack_str(i->dst)); return; }
    if (a.k == O_REG && L((int)a.v).kind == LK_GPR) { E("movq %s, %s", R64[L((int)a.v).r], stack_str(i->dst)); return; }
    mov_to_gpr(a, RAX);
    store_gpr(RAX, i->dst);
}

static void push_opnd(Opnd o) {
    if (o.k == O_IMM && fits32(o.v)) { E("pushq $%lld", (long long)o.v); return; }
    if (o.k == O_REG) {
        Loc l = L((int)o.v);
        if (l.kind == LK_GPR) { E("pushq %s", R64[l.r]); return; }
        if (l.kind == LK_STACK) { E("pushq %s", stack_str((int)o.v)); return; }
    }
    mov_to_gpr(o, RAX);
    E("pushq %%rax");
}

static void emit_call_win64(Ins *i) {
    // Microsoft x64: rcx, rdx, r8, r9, then the stack; 32 bytes of shadow space; rsp 16-aligned at the call.
    int n = i->nargs;
    int nstack = n > 4 ? n - 4 : 0;
    int total = (32 + 8 * nstack + 15) / 16 * 16;
    E("subq $%d, %%rsp", total);
    for (int k = 4; k < n; k++) {
        mov_to_gpr(i->args[k], RAX);
        E("movq %%rax, %d(%%rsp)", 32 + 8 * (k - 4));
    }
    static const int wregs[4] = { RCX, RDX, R8, R9 };
    PM pm[4]; int np = 0;
    for (int k = 0; k < n && k < 4; k++) { pm[np].dst = wregs[k]; pm[np].src = i->args[k]; pm[np].done = false; np++; }
    parallel_move(pm, np, RAX);
    E("call %s", sy(g_isyms.d[i->a.v].name));
    E("addq $%d, %%rsp", total);
    if (i->dst >= 0) store_gpr(RAX, i->dst);
    if (i->dst2 >= 0) store_gpr(RDX, i->dst2);
}

static void emit_call(Ins *i) {
    if (i->a.k == O_GLOB && g_isyms.d[i->a.v].kind == IS_FUNC && g_isyms.d[i->a.v].func->winabi) { emit_call_win64(i); return; }
    int n = i->nargs;
    int nstack = n > 6 ? n - 6 : 0;
    int pad = nstack & 1;
    if (pad) E("subq $8, %%rsp");
    for (int k = n - 1; k >= 6; k--) push_opnd(i->args[k]);
    PM pm[8];
    int np = 0;
    for (int k = 0; k < n && k < 6; k++) { pm[np].dst = ARGREG[k]; pm[np].src = i->args[k]; pm[np].done = false; np++; }
    bool indirect = i->a.k == O_REG;
    if (indirect) { pm[np].dst = R11; pm[np].src = i->a; pm[np].done = false; np++; }
    parallel_move(pm, np, RAX);
    if (indirect) E("call *%%r11");
    else E("call %s", sy(g_isyms.d[i->a.v].name));
    if (nstack + pad) E("addq $%d, %%rsp", (nstack + pad) * 8);
    if (i->dst >= 0) store_gpr(RAX, i->dst);
    if (i->dst2 >= 0) store_gpr(RDX, i->dst2);
}

static void emit_special(Ins *i) {
    PM pm[8];
    int np = 0;
    if (i->op == OP_SYSCALL) {
        static const int regs[7] = { RAX, RDI, RSI, RDX, R10, R8, R9 };
        for (int k = 0; k < i->nargs; k++) { pm[np].dst = regs[k]; pm[np].src = i->args[k]; pm[np].done = false; np++; }
        parallel_move(pm, np, RCX);
        E("syscall");
        if (i->dst >= 0) store_gpr(RAX, i->dst);
    } else if (i->op == OP_MEMCPY) {
        static const int regs[3] = { RDI, RSI, RCX };
        for (int k = 0; k < 3; k++) { pm[np].dst = regs[k]; pm[np].src = i->args[k]; pm[np].done = false; np++; }
        parallel_move(pm, np, RAX);
        E("rep movsb");
    } else {
        static const int regs[2] = { RDI, RCX };
        for (int k = 0; k < 2; k++) { pm[np].dst = regs[k]; pm[np].src = i->args[k]; pm[np].done = false; np++; }
        parallel_move(pm, np, RAX);
        E("xorl %%eax, %%eax");
        E("rep stosb");
    }
}

static void emit_params(int np) {
    // register-passed params: xmm / stack destinations first (they only read arg registers)
    PM pm[8]; int n = 0;
    for (int k = 0; k < np; k++) {
        Ins *i = &ir->ins.d[k];
        int idx = (int)i->a.v;
        int v = i->dst;
        if (idx >= 6) continue;
        Loc l = L(v);
        if (l.kind == LK_XMM) E("movq %s, %s", R64[ARGREG[idx]], XMMN[l.r]);
        else if (l.kind == LK_STACK) E("movq %s, %s", R64[ARGREG[idx]], stack_str(v));
    }
    for (int k = 0; k < np; k++) {
        Ins *i = &ir->ins.d[k];
        int idx = (int)i->a.v;
        if (idx >= 6) continue;
        Loc l = L(i->dst);
        if (l.kind == LK_GPR) { pm[n].dst = l.r; pm[n].src = (Opnd){ O_REG, -1 }; pm[n].srcgpr = ARGREG[idx]; pm[n].done = false; n++; }
    }
    // custom parallel move with explicit gpr sources
    for (;;) {
        bool pending = false, progress = false;
        for (int a = 0; a < n; a++) {
            if (pm[a].done) continue;
            if (pm[a].srcgpr == pm[a].dst) { pm[a].done = true; continue; }
            pending = true;
            bool blocked = false;
            for (int b = 0; b < n; b++) if (b != a && !pm[b].done && pm[b].srcgpr == pm[a].dst) { blocked = true; break; }
            if (blocked) continue;
            E("movq %s, %s", R64[pm[a].srcgpr], R64[pm[a].dst]);
            pm[a].done = true; progress = true;
        }
        if (!pending) break;
        if (!progress) {
            int pick = -1;
            for (int a = 0; a < n; a++) if (!pm[a].done) { pick = a; break; }
            int s = pm[pick].srcgpr;
            E("movq %s, %%rax", R64[s]);
            for (int b = 0; b < n; b++) if (!pm[b].done && pm[b].srcgpr == s) pm[b].srcgpr = RAX;
        }
    }
    for (int k = 0; k < np; k++) {
        Ins *i = &ir->ins.d[k];
        int idx = (int)i->a.v;
        if (idx < 6) continue;
        int off = 16 + 8 * (idx - 6);
        Loc l = L(i->dst);
        if (l.kind == LK_GPR) E("movq %d(%%rbp), %s", off, R64[l.r]);
        else if (l.kind == LK_XMM) E("movsd %d(%%rbp), %s", off, XMMN[l.r]);
        else if (l.kind == LK_STACK) { E("movq %d(%%rbp), %%rax", off); E("movq %%rax, %s", stack_str(i->dst)); }
    }
}

static void emit_ret(Ins *i, bool last) {
    if (i->a.k != O_NONE) mov_to_gpr(i->a, RAX);
    if (i->b.k != O_NONE) mov_to_gpr(i->b, RDX);
    if (!last) E("jmp %sret%d", lp(), fid);
}

static void emit_ins(Ins *i, int idx, int total) {
    switch (i->op) {
    case OP_NOP: case OP_PARAM: break;
    case OP_LABEL: fprintf(out, "%s%d_%d:\n", lp(), fid, i->label); break;
    case OP_JMP:
        // skip jumps to the immediately following label
        if (idx + 1 < total && ir->ins.d[idx + 1].op == OP_LABEL && ir->ins.d[idx + 1].label == i->label) break;
        E("jmp %s%d_%d", lp(), fid, i->label);
        break;
    case OP_JCC:
        emit_cmp(i->a, i->b);
        E("j%s %s%d_%d", CCN[i->cc], lp(), fid, i->label);
        break;
    case OP_MOV: emit_mov(i); break;
    case OP_FCONST: {
        Loc l = L(i->dst);
        if (i->a.v == 0 && l.kind == LK_XMM) E("xorps %s, %s", XMMN[l.r], XMMN[l.r]);
        else { mov_to_gpr(i->a, RAX); store_gpr(RAX, i->dst); }
        break;
    }
    case OP_ADD: emit_alu(i, "addq", true); break;
    case OP_SUB: emit_alu(i, "subq", false); break;
    case OP_MUL: emit_alu(i, "imulq", true); break;
    case OP_AND: emit_alu(i, "andq", true); break;
    case OP_OR: emit_alu(i, "orq", true); break;
    case OP_XOR: emit_alu(i, "xorq", true); break;
    case OP_DIV: emit_div(i, true, false); break;
    case OP_UDIV: emit_div(i, false, false); break;
    case OP_MOD: emit_div(i, true, true); break;
    case OP_UMOD: emit_div(i, false, true); break;
    case OP_SHL: emit_shift(i, "shlq"); break;
    case OP_SHR: emit_shift(i, "shrq"); break;
    case OP_SAR: emit_shift(i, "sarq"); break;
    case OP_NEG: case OP_NOT: {
        Loc ld = L(i->dst);
        int t = dst_gpr(i->dst, RAX);
        mov_to_gpr(i->a, t);
        E("%s %s", i->op == OP_NEG ? "negq" : "notq", R64[t]);
        if (!(ld.kind == LK_GPR && ld.r == t)) store_gpr(t, i->dst);
        break;
    }
    case OP_SEXT: case OP_ZEXT: emit_ext(i); break;
    case OP_SET: emit_setcc(i); break;
    case OP_FADD: emit_fbin(i, "addsd", true); break;
    case OP_FSUB: emit_fbin(i, "subsd", false); break;
    case OP_FMUL: emit_fbin(i, "mulsd", true); break;
    case OP_FDIV: emit_fbin(i, "divsd", false); break;
    case OP_FNEG: {
        int t = xdst(i->dst);
        int ra = xsrc_reg(i->a, t);
        if (ra != t) E("movaps %s, %s", XMMN[ra], XMMN[t]);
        E("movabsq $-9223372036854775808, %%rax");
        E("movq %%rax, %%xmm1");
        E("xorpd %%xmm1, %s", XMMN[t]);
        if (L(i->dst).kind != LK_XMM) xstore(t, i->dst);
        break;
    }
    case OP_FSQRT: {
        int t = xdst(i->dst);
        E("sqrtsd %s, %s", xsrc_str(i->a, 1), XMMN[t]);
        if (L(i->dst).kind != LK_XMM) xstore(t, i->dst);
        break;
    }
    case OP_FSET: emit_fset(i); break;
    case OP_I2F: {
        int t = xdst(i->dst);
        const char *s = src(i->a, RAX);
        E("pxor %s, %s", XMMN[t], XMMN[t]);
        E("cvtsi2sdq %s, %s", s, XMMN[t]);
        if (L(i->dst).kind != LK_XMM) xstore(t, i->dst);
        break;
    }
    case OP_U2F: {
        int t = xdst(i->dst);
        mov_to_gpr(i->a, RAX);
        E("pxor %s, %s", XMMN[t], XMMN[t]);
        E("testq %%rax, %%rax");
        E("js 1f");
        E("cvtsi2sdq %%rax, %s", XMMN[t]);
        E("jmp 2f");
        E("1:");
        E("movq %%rax, %%rcx");
        E("shrq $1, %%rcx");
        E("andl $1, %%eax");
        E("orq %%rax, %%rcx");
        E("cvtsi2sdq %%rcx, %s", XMMN[t]);
        E("addsd %s, %s", XMMN[t], XMMN[t]);
        E("2:");
        if (L(i->dst).kind != LK_XMM) xstore(t, i->dst);
        break;
    }
    case OP_F2I: {
        int t = dst_gpr(i->dst, RAX);
        E("cvttsd2siq %s, %s", xsrc_str(i->a, 0), R64[t]);
        if (L(i->dst).kind != LK_GPR) store_gpr(t, i->dst);
        break;
    }
    case OP_F2U: {
        int ra = xsrc_reg(i->a, 0);
        if (ra != 0) E("movaps %s, %%xmm0", XMMN[ra]);
        E("movabsq $4890909195324358656, %%rax");   // 2^63 as double
        E("movq %%rax, %%xmm1");
        E("ucomisd %%xmm1, %%xmm0");
        E("jae 1f");
        E("cvttsd2siq %%xmm0, %%rax");
        E("jmp 2f");
        E("1:");
        E("subsd %%xmm1, %%xmm0");
        E("cvttsd2siq %%xmm0, %%rax");
        E("movabsq $-9223372036854775808, %%rcx");
        E("xorq %%rcx, %%rax");
        E("2:");
        store_gpr(RAX, i->dst);
        break;
    }
    case OP_LOAD: emit_load(i); break;
    case OP_STORE: emit_store_ins(i); break;
    case OP_LEA: {
        const char *m = mem_str(i);
        int t = dst_gpr(i->dst, RAX);
        E("leaq %s, %s", m, R64[t]);
        if (L(i->dst).kind != LK_GPR) store_gpr(t, i->dst);
        break;
    }
    case OP_CALL: emit_call(i); break;
    case OP_RET: emit_ret(i, idx == total - 1); break;
    case OP_SYSCALL: case OP_MEMCPY: case OP_MEMZERO: emit_special(i); break;
    }
}

// ================================================================= function driver
static void emit_function(FnIR *f, int id) {
    ir = f; fid = id;
    compute_intervals();
    allocate();
    // frame layout
    bool used_callee[16] = {0};
    for (int v = 0; v < nvregs; v++) if (ivs[v].end >= 0 && loc[v].kind == LK_GPR) {
        int r = loc[v].r;
        if (r == RBX || r >= R12) used_callee[r] = true;
    }
    nsaved = 0;
    static const int order[5] = { RBX, R12, R13, R14, R15 };
    for (int k = 0; k < 5; k++) if (used_callee[order[k]]) saved_regs[nsaved++] = order[k];
    int nslots = f->slots.n;
    slot_off = xcalloc(sizeof(int) * (size_t)(nslots + nspill + 1));
    int cur = 8 * nsaved;
    for (int s = 0; s < nslots; s++) {
        int al = f->slots.d[s].align < 8 ? f->slots.d[s].align : (f->slots.d[s].align > 16 ? 16 : f->slots.d[s].align);
        cur += f->slots.d[s].size;
        cur = (cur + al - 1) / al * al;
        slot_off[s] = -cur;
    }
    for (int s = 0; s < nspill; s++) { cur += 8; slot_off[nslots + s] = -cur; }
    int total = (cur + 15) / 16 * 16;
    int frame = total - 8 * nsaved;

    fprintf(out, "\n.globl %s\n%s:\n", sy(f->name), sy(f->name));
    E("pushq %%rbp");
    E("movq %%rsp, %%rbp");
    for (int k = 0; k < nsaved; k++) E("pushq %s", R64[saved_regs[k]]);
    if (frame) E("subq $%d, %%rsp", frame);
    int np = 0;
    while (np < f->ins.n && f->ins.d[np].op == OP_PARAM) np++;
    emit_params(np);
    for (int k = np; k < f->ins.n; k++) emit_ins(&f->ins.d[k], k, f->ins.n);
    fprintf(out, "%sret%d:\n", lp(), id);
    if (nsaved) E("leaq -%d(%%rbp), %%rsp", 8 * nsaved);
    for (int k = nsaved - 1; k >= 0; k--) E("popq %s", R64[saved_regs[k]]);
    if (!nsaved) E("movq %%rbp, %%rsp");
    E("popq %%rbp");
    E("ret");
    free(ivs); free(loc); free(slot_off);
}

// ================================================================= program emission
static void data_bytes(const char *b, int n) {
    for (int i = 0; i < n; i += 16) {
        fprintf(out, "\t.byte ");
        for (int k = i; k < n && k < i + 16; k++) fprintf(out, "%s%d", k > i ? "," : "", (unsigned char)b[k]);
        fprintf(out, "\n");
    }
}

static void emit_global(ISym *s) {
    Sym *g = s->gsym;
    Type *t = g->type;
    int size = type_size(t), al = type_align(t);
    if (size == 0) size = 1;
    ConstVal *cv = &g->cv;
    bool zero = cv->kind == 0 || cv->kind == 5 || (cv->kind == 1 && cv->i == 0) || (cv->kind == 3 && cv->i == 0) || (cv->kind == 2 && cv->f == 0.0 && !signbit(cv->f));
    if (zero) {
        if (g_target == TGT_MACOS) {
            int lg = 0; while ((1 << lg) < (al < 16 ? al : 16)) lg++;
            fprintf(out, ".zerofill __DATA,__bss,%s,%d,%d\n", sy(s->name), size, lg);
        } else fprintf(out, ".bss\n.balign %d\n%s:\n\t.zero %d\n", al < 16 ? al : 16, s->name, size);
        return;
    }
    fprintf(out, ".data\n.balign %d\n%s:\n", al < 16 ? al : 16, sy(s->name));
    if (cv->kind == 4) {
        int id = -1;
        for (int k = 0; k < g_isyms.n; k++)
            if (g_isyms.d[k].kind == IS_STR && g_isyms.d[k].len == cv->slen && !memcmp(g_isyms.d[k].bytes, cv->s, (size_t)cv->slen)) { id = k; break; }
        if (id < 0) die("internal error: string constant for global '%s' was not interned", g->name);
        fprintf(out, "\t.quad %s\n\t.quad %d\n", sy(g_isyms.d[id].name), cv->slen);
    } else if (cv->kind == 2) {
        double d = cv->f; int64_t bits; memcpy(&bits, &d, 8);
        fprintf(out, "\t.quad %lld\n", (long long)bits);
    } else {
        long long v = (long long)cv->i;
        if (size == 1) fprintf(out, "\t.byte %lld\n", v & 0xff);
        else if (size == 2) fprintf(out, "\t.value %lld\n", v & 0xffff);
        else if (size == 4) fprintf(out, "\t.long %lld\n", v & 0xffffffffLL);
        else fprintf(out, "\t.quad %lld\n", v);
    }
}

void regalloc_and_emit(FILE *o) {
    out = o;
    for (int i = 0; i < 16; i++) snprintf(XMMN[i], sizeof XMMN[i], "%%xmm%d", i);
    // map funcs -> IR and compute reachability
    FnIR **by_func = xcalloc(sizeof(FnIR *) * (size_t)(g_prog.funcs.n + 1));
    for (int i = 0; i < g_irs.n; i++) by_func[g_irs.d[i]->f->id] = g_irs.d[i];
    int *func_isym = xcalloc(sizeof(int) * (size_t)(g_prog.funcs.n + 1));
    for (int i = 0; i < g_isyms.n; i++) if (g_isyms.d[i].kind == IS_FUNC) func_isym[g_isyms.d[i].func->id] = i;
    VEC(FnIR *) work = {0};
    bool *queued = xcalloc((size_t)g_prog.funcs.n + 1);
    Func *roots[4] = { g_prog.main_fn, core_sym("rt_init")->func, core_sym("rt_exit")->func, core_sym("rt_main_err")->func };
    for (int k = 0; k < 4; k++) if (roots[k] && by_func[roots[k]->id] && !queued[roots[k]->id]) { queued[roots[k]->id] = true; vpush(work, by_func[roots[k]->id]); }
    for (int w = 0; w < work.n; w++) {
        FnIR *f = work.d[w];
        for (int k = 0; k < f->ins.n; k++) {
            Ins *i = &f->ins.d[k];
            Opnd *ops[3] = { &i->a, &i->b, &i->c };
            for (int q = 0; q < 3 + i->nargs; q++) {
                Opnd *o = q < 3 ? ops[q] : &i->args[q - 3];
                if (o->k != O_GLOB) continue;
                ISym *s = &g_isyms.d[o->v];
                s->used = true;
                if (s->kind == IS_FUNC && by_func[s->func->id] && !queued[s->func->id]) { queued[s->func->id] = true; vpush(work, by_func[s->func->id]); }
            }
        }
    }
    fprintf(out, "\t.text\n");
    // entry point
    Type *mr = g_prog.main_fn->type->ret;
    const char *f_init = core_sym("rt_init")->func->asm_name, *f_exit = core_sym("rt_exit")->func->asm_name,
               *f_err = core_sym("rt_main_err")->func->asm_name;
    if (g_target == TGT_LINUX) {
        fprintf(out, ".globl _start\n_start:\n");
        E("xorl %%ebp, %%ebp");
        E("movq (%%rsp), %%rdi");
        E("leaq 8(%%rsp), %%rsi");
        E("andq $-16, %%rsp");
    } else if (g_target == TGT_MACOS) {
        fprintf(out, ".globl _main\n_main:\n");
        E("pushq %%rbp");
        E("movq %%rsp, %%rbp");
    } else {
        fprintf(out, ".globl main\nmain:\n");
        E("pushq %%rbp");
        E("movq %%rsp, %%rbp");
        E("movq %%rcx, %%rdi");      // Win64: argc in ecx, argv in rdx
        E("movq %%rdx, %%rsi");
    }
    E("call %s", sy(f_init));
    E("call %s", sy(g_prog.main_fn->asm_name));
    if (mr->kind == TY_I64) { E("movq %%rax, %%rdi"); E("call %s", sy(f_exit)); }
    else if (mr->kind == TY_ERRUNION) { E("movq %%rax, %%rdi"); E("call %s", sy(f_err)); }
    else { E("xorl %%edi, %%edi"); E("call %s", sy(f_exit)); }
    E("ud2");
    for (int w = 0; w < work.n; w++) emit_function(work.d[w], w);
    // read-only data
    if (g_target == TGT_MACOS) fprintf(out, "\n\t.section __TEXT,__const\n");
    else if (g_target == TGT_WINDOWS) fprintf(out, "\n\t.section .rdata,\"dr\"\n");
    else fprintf(out, "\n\t.section .rodata\n");
    for (int i = 0; i < g_isyms.n; i++) {
        ISym *s = &g_isyms.d[i];
        if (s->kind == IS_STR) { fprintf(out, "%s:\n", sy(s->name)); data_bytes(s->bytes, s->len); }
    }
    // error name table (contains pointers, so it lives in writable data on Mach-O)
    if (g_target == TGT_MACOS) fprintf(out, "\t.data\n");
    const char *L = lp();
    fprintf(out, "\t.balign 8\n%s:\n\t.quad %serrunk\n\t.quad 1\n", sy("orb_errtab"), L);
    for (int i = 0; i < g_prog.errors.n; i++) fprintf(out, "\t.quad %serr%d\n\t.quad %d\n", L, i, (int)strlen(g_prog.errors.d[i]->name));
    fprintf(out, "%serrunk:\n\t.ascii \"?\"\n", L);
    for (int i = 0; i < g_prog.errors.n; i++) fprintf(out, "%serr%d:\n\t.ascii \"%s\"\n", L, i, g_prog.errors.d[i]->name);
    for (int i = 0; i < g_isyms.n; i++) if (g_isyms.d[i].kind == IS_DATA) emit_global(&g_isyms.d[i]);
    if (g_target == TGT_LINUX) fprintf(out, "\n\t.section .note.GNU-stack,\"\",@progbits\n");
}
