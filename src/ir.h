// Orb! compiler - intermediate representation
#ifndef ORB_IR_H
#define ORB_IR_H
#include "ast.h"

enum { O_NONE, O_REG, O_IMM, O_SLOT, O_GLOB };
typedef struct { int k; int64_t v; } Opnd;

enum { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE, CC_B, CC_BE, CC_A, CC_AE };

enum {
    OP_NOP, OP_LABEL, OP_JMP, OP_JCC,
    OP_MOV, OP_FCONST,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_UDIV, OP_MOD, OP_UMOD,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_SAR, OP_NEG, OP_NOT,
    OP_SEXT, OP_ZEXT,
    OP_SET,
    OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV, OP_FNEG, OP_FSQRT, OP_FSET,
    OP_I2F, OP_U2F, OP_F2I, OP_F2U,
    OP_LOAD, OP_STORE, OP_LEA,
    OP_CALL, OP_RET, OP_SYSCALL, OP_MEMCPY, OP_MEMZERO,
    OP_PARAM,
    OP_MEMGROW, OP_MEMSIZE,          // wasm only: memory.grow(pages) / memory.size
    OP__COUNT
};

typedef struct Ins {
    int op, cc, w;
    bool sg, noret;
    int dst, dst2;
    Opnd a, b, c;
    int scale;
    int64_t off;
    int label;
    Opnd *args; int nargs;
    int line;
} Ins;

typedef struct { int size, align, off; } Slot;

typedef struct FnIR {
    Func *f;
    char *name;
    VEC(char) cls;          // per vreg: 0 int, 1 float
    VEC(Slot) slots;
    VEC(Ins) ins;
    int nlabels;
    int nparams;
    bool has_call;
} FnIR;

typedef struct { char *name; int kind; char *bytes; int len; Func *func; Sym *gsym; bool used; } ISym;
enum { IS_FUNC, IS_DATA, IS_STR, IS_ERRTAB };

typedef VEC(ISym) ISymVec;
typedef VEC(FnIR *) IRVec;
extern ISymVec g_isyms;
extern IRVec g_irs;

void lower_program(void);
void ir_dump(FILE *f, FnIR *ir);
void opt_function(FnIR *ir);
void regalloc_and_emit(FILE *out);   // runs allocation + emission for all reachable functions
const char *cc_name(int cc);
void wasm_emit(const char *path);   // WebAssembly backend (wasm.c): writes a .wasm binary

#endif
