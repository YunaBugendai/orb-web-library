// Orb! compiler - semantic analysis internals
#ifndef ORB_SEMA_H
#define ORB_SEMA_H
#include "ast.h"

typedef struct Ctx {
    Module *mod;
    Func *fn;
    Type *ret;                  // return type of current function
    VEC(Sym *) scope;
    VEC(int) marks;
    int loops;
    int catch_depth;
} Ctx;

void ctx_push(Ctx *c);
void ctx_pop(Ctx *c);
Sym *ctx_declare(Ctx *c, Sym *s);               // returns NULL (after reporting) on redeclaration
Sym *sym_new(SymKind k, const char *name, Pos p, Module *m);
Sym *lookup(Ctx *c, const char *name);
Sym *lookup_unknown_report(Ctx *c, const char *name, Pos p);
Sym *module_member(Module *m, const char *name, Pos p);
Type *resolve_type(Ctx *c, Node *t);
bool complete_type(Type *t, Pos p);
bool eval_const(Ctx *c, Node *n, ConstVal *out);
void const_sym_resolve(Sym *s);
void const_sym_resolve_in(Ctx *c, Sym *s);
bool int_fits_type(Type *t, __int128 v);
Func *func_sig(Func *f);
Func *instantiate_func(Ctx *c, Sym *g, Type **targs, int n, Pos p);
Type *instantiate_gtype(Ctx *c, Sym *g, Type **targs, int n, Pos p);
bool unify(Ctx *c, Node *tn, Type *actual, char **names, Type **bound, int ntp);
Field *find_field(Type *t, const char *name);
char *suggest_name(Ctx *c, const char *name);
Sym *universe_lookup(const char *name);
void queue_func(Func *f);
Func *new_func(Sym *s, Node *decl, Module *m);
Sym *global_resolve(Sym *s);

// check.c
void check_func(Func *f);
Type *check_expr(Ctx *c, Node *n, Type *hint);
bool is_untyped_lit(Ctx *c, Node *n);
extern void (*sema_check_global_hook)(Sym *);
#endif
