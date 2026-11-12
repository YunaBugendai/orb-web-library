// Orb! compiler - IR optimizations (per function)
#include "ir.h"

static void count_defs_uses(FnIR *f, int *defs, int *uses) {
    for (int k = 0; k < f->ins.n; k++) {
        Ins *i = &f->ins.d[k];
        if (i->dst >= 0) defs[i->dst]++;
        if (i->dst2 >= 0) defs[i->dst2]++;
        Opnd *ops[3] = { &i->a, &i->b, &i->c };
        for (int q = 0; q < 3; q++) if (ops[q]->k == O_REG) uses[ops[q]->v]++;
        for (int q = 0; q < i->nargs; q++) if (i->args[q].k == O_REG) uses[i->args[q].v]++;
    }
}

static bool is_pure(Ins *i) {
    switch (i->op) {
    case OP_MOV: case OP_FCONST: case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
    case OP_SHL: case OP_SHR: case OP_SAR: case OP_NEG: case OP_NOT: case OP_SEXT: case OP_ZEXT: case OP_SET:
    case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG: case OP_FSQRT: case OP_FSET:
    case OP_I2F: case OP_U2F: case OP_F2I: case OP_F2U: case OP_LEA: case OP_LOAD:
        return true;
    default: return false;
    }
}

// Global constant propagation of single-definition vregs defined by `mov v, imm`,
// and rewriting `op t, ...; mov x, t` (t used once) into `op x, ...`.
static bool pass_const_and_fuse(FnIR *f) {
    int nv = f->cls.n;
    int *defs = xcalloc(sizeof(int) * (size_t)(nv + 1)), *uses = xcalloc(sizeof(int) * (size_t)(nv + 1));
    count_defs_uses(f, defs, uses);
    Opnd *cval = xcalloc(sizeof(Opnd) * (size_t)(nv + 1));
    for (int k = 0; k < f->ins.n; k++) {
        Ins *i = &f->ins.d[k];
        if (i->op == OP_MOV && i->dst >= 0 && defs[i->dst] == 1 && i->a.k == O_IMM && f->cls.d[i->dst] == 0) cval[i->dst] = i->a;
    }
    bool changed = false;
    for (int k = 0; k < f->ins.n; k++) {
        Ins *i = &f->ins.d[k];
        Opnd *ops[3] = { &i->a, &i->b, &i->c };
        for (int q = 0; q < 3; q++) {
            if (ops[q]->k != O_REG || cval[ops[q]->v].k != O_IMM) continue;
            // memory base / index operands must stay registers
            if ((i->op == OP_LOAD || i->op == OP_STORE || i->op == OP_LEA) && q < 2) continue;
            if (i->op == OP_JCC && q == 0) continue;
            if (i->op == OP_SET && q == 0) continue;
            if ((i->op == OP_CALL && q == 0) || i->op == OP_RET) { if (q == 0 && i->op == OP_CALL) continue; }
            if (i->op == OP_I2F || i->op == OP_U2F || i->op == OP_F2I || i->op == OP_F2U) continue;
            if (i->op == OP_FADD || i->op == OP_FSUB || i->op == OP_FMUL || i->op == OP_FDIV || i->op == OP_FSET || i->op == OP_FNEG || i->op == OP_FSQRT) continue;
            if (i->op == OP_MOV && f->cls.d[i->dst] == 1) continue;
            if (i->op == OP_STORE && q == 2 && false) continue;
            if ((i->op == OP_SUB || i->op == OP_SHL || i->op == OP_SHR || i->op == OP_SAR || i->op == OP_DIV || i->op == OP_UDIV || i->op == OP_MOD || i->op == OP_UMOD) && q == 0) continue;
            if ((i->op == OP_NEG || i->op == OP_NOT || i->op == OP_SEXT || i->op == OP_ZEXT) && q == 0) continue;
            *ops[q] = cval[ops[q]->v];
            changed = true;
        }
        for (int q = 0; q < i->nargs; q++)
            if (i->args[q].k == O_REG && cval[i->args[q].v].k == O_IMM) { i->args[q] = cval[i->args[q].v]; changed = true; }
        // commutative ops with immediate first operand: swap
        if ((i->op == OP_ADD || i->op == OP_MUL || i->op == OP_AND || i->op == OP_OR || i->op == OP_XOR) && i->a.k == O_IMM && i->b.k == O_REG) { Opnd t = i->a; i->a = i->b; i->b = t; }
    }
    // fuse: `X t, ... ; mov x, t` with t single-def single-use
    for (int k = 0; k + 1 < f->ins.n; k++) {
        Ins *i = &f->ins.d[k], *m = &f->ins.d[k + 1];
        if (m->op != OP_MOV || m->a.k != O_REG || i->dst < 0 || (int)m->a.v != i->dst) continue;
        if (defs[i->dst] != 1 || uses[i->dst] != 1 || i->dst2 >= 0) continue;
        if (!is_pure(i) && i->op != OP_CALL) continue;
        if (i->op == OP_CALL || i->op == OP_LOAD) continue;
        if (f->cls.d[i->dst] != f->cls.d[m->dst]) continue;
        // the destination must not be read by instruction i itself in a way that conflicts: fine for 2-address emission
        i->dst = m->dst;
        m->op = OP_NOP; m->dst = -1; m->a.k = O_NONE;
        changed = true;
    }
    free(defs); free(uses); free(cval);
    return changed;
}

static bool pass_dce(FnIR *f) {
    int nv = f->cls.n;
    int *defs = xcalloc(sizeof(int) * (size_t)(nv + 1)), *uses = xcalloc(sizeof(int) * (size_t)(nv + 1));
    count_defs_uses(f, defs, uses);
    bool changed = false;
    for (int k = 0; k < f->ins.n; k++) {
        Ins *i = &f->ins.d[k];
        if (i->op == OP_NOP || i->dst < 0 || i->dst2 >= 0) continue;
        if (!is_pure(i)) continue;
        if (uses[i->dst] == 0) { i->op = OP_NOP; i->dst = -1; i->a.k = i->b.k = i->c.k = O_NONE; changed = true; }
    }
    free(defs); free(uses);
    return changed;
}

// remove jumps to the next label, unreachable code after unconditional transfers
static bool pass_cfg(FnIR *f) {
    bool changed = false;
    int n = f->ins.n;
    // unreachable instructions between a jmp/ret and the next label
    for (int k = 0; k < n; k++) {
        Ins *i = &f->ins.d[k];
        if (i->op == OP_JMP || i->op == OP_RET || (i->op == OP_CALL && i->noret)) {
            for (int j = k + 1; j < n && f->ins.d[j].op != OP_LABEL; j++)
                if (f->ins.d[j].op != OP_NOP) { f->ins.d[j].op = OP_NOP; f->ins.d[j].dst = -1; f->ins.d[j].dst2 = -1; changed = true; }
        }
    }
    // jump threading: jmp L1 where L1: jmp L2
    int *target = xcalloc(sizeof(int) * (size_t)(f->nlabels + 1));
    for (int l = 0; l < f->nlabels; l++) target[l] = l;
    for (int k = 0; k < n; k++) {
        if (f->ins.d[k].op != OP_LABEL) continue;
        int j = k + 1;
        while (j < n && (f->ins.d[j].op == OP_NOP || f->ins.d[j].op == OP_LABEL)) j++;
        if (j < n && f->ins.d[j].op == OP_JMP) target[f->ins.d[k].label] = f->ins.d[j].label;
        // labels that directly follow each other are equivalent
        int m = k + 1;
        while (m < n && f->ins.d[m].op == OP_NOP) m++;
    }
    for (int k = 0; k < n; k++) {
        Ins *i = &f->ins.d[k];
        if (i->op != OP_JMP && i->op != OP_JCC) continue;
        int t = i->label, guard = 0;
        while (target[t] != t && guard++ < 16) t = target[t];
        if (t != i->label) { i->label = t; changed = true; }
    }
    free(target);
    return changed;
}

static int newv(FnIR *f, int cls) { vpush(f->cls, (char)cls); return f->cls.n - 1; }
static int log2_exact(int64_t v) {
    if (v <= 1 || (v & (v - 1))) return -1;
    return __builtin_ctzll((uint64_t)v);
}

// Strength reduction: multiply/divide/modulo by constants, sub-immediate -> add-immediate.
static bool pass_strength(FnIR *f) {
    bool changed = false;
    VEC(Ins) out = {0};
    for (int k = 0; k < f->ins.n; k++) {
        Ins i = f->ins.d[k];
        if (i.dst >= 0 && f->cls.d[i.dst] == 0 && i.a.k == O_REG && i.b.k == O_IMM) {
            int sh = log2_exact(i.b.v);
            if (i.op == OP_SUB && i.b.v > INT32_MIN && i.b.v <= INT32_MAX) {
                i.op = OP_ADD; i.b.v = -i.b.v; vpush(out, i); changed = true; continue;
            }
            if (i.op == OP_MUL && sh > 0) {
                i.op = OP_SHL; i.b.v = sh; vpush(out, i); changed = true; continue;
            }
            if (i.op == OP_UDIV && sh > 0) {
                i.op = OP_SHR; i.b.v = sh; vpush(out, i); changed = true; continue;
            }
            if (i.op == OP_UMOD && sh > 0 && i.b.v - 1 <= INT32_MAX) {
                i.op = OP_AND; i.b.v = i.b.v - 1; vpush(out, i); changed = true; continue;
            }
            if ((i.op == OP_DIV || i.op == OP_MOD) && sh > 0) {
                int t1 = newv(f, 0), t2 = newv(f, 0), t3 = newv(f, 0);
                Ins x;
                memset(&x, 0, sizeof x); x.dst = -1; x.dst2 = -1; x.scale = 1;
                x.op = OP_SAR; x.dst = t1; x.a = i.a; x.b = (Opnd){ O_IMM, 63 }; vpush(out, x);
                x.op = OP_SHR; x.dst = t2; x.a = (Opnd){ O_REG, t1 }; x.b = (Opnd){ O_IMM, 64 - sh }; vpush(out, x);
                x.op = OP_ADD; x.dst = t3; x.a = i.a; x.b = (Opnd){ O_REG, t2 }; vpush(out, x);
                if (i.op == OP_DIV) {
                    x.op = OP_SAR; x.dst = i.dst; x.a = (Opnd){ O_REG, t3 }; x.b = (Opnd){ O_IMM, sh }; vpush(out, x);
                } else {
                    int t4 = newv(f, 0);
                    x.op = OP_AND; x.dst = t4; x.a = (Opnd){ O_REG, t3 }; x.b = (Opnd){ O_IMM, -i.b.v }; vpush(out, x);
                    x.op = OP_SUB; x.dst = i.dst; x.a = i.a; x.b = (Opnd){ O_REG, t4 }; vpush(out, x);
                }
                changed = true;
                continue;
            }
        }
        vpush(out, i);
    }
    if (changed) { free(f->ins.d); f->ins.d = out.d; f->ins.n = out.n; f->ins.cap = out.cap; }
    else free(out.d);
    return changed;
}

static void compact(FnIR *f) {
    int w = 0;
    for (int k = 0; k < f->ins.n; k++) if (f->ins.d[k].op != OP_NOP) f->ins.d[w++] = f->ins.d[k];
    f->ins.n = w;
}

void opt_function(FnIR *f) {
    for (int round = 0; round < 6; round++) {
        bool c = false;
        c |= pass_const_and_fuse(f);
        c |= pass_strength(f);
        c |= pass_cfg(f);
        c |= pass_dce(f);
        compact(f);
        if (!c) break;
    }
}
