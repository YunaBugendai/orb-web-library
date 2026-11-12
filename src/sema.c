#include "sema.h"

Program g_prog;
const char *g_std_dir = NULL;
static Map loaded;
static Map universe;
static int next_errcode = 1;
static VEC(Type *) all_structs;

// ------------------------------------------------------------------ symbols & scopes
Sym *sym_new(SymKind k, const char *name, Pos p, Module *m) {
    Sym *s = xcalloc(sizeof(Sym));
    s->kind = k; s->name = xstrdup(name); s->pos = p; s->mod = m;
    return s;
}

void ctx_push(Ctx *c) { vpush(c->marks, c->scope.n); }
void ctx_pop(Ctx *c) { c->scope.n = c->marks.d[--c->marks.n]; }

Sym *ctx_declare(Ctx *c, Sym *s) {
    int start = c->marks.n ? c->marks.d[c->marks.n - 1] : 0;
    for (int i = c->scope.n - 1; i >= start; i--)
        if (!strcmp(c->scope.d[i]->name, s->name)) {
            diag_error(s->pos, "'%s' is already declared in this scope", s->name);
            diag_note(c->scope.d[i]->pos, "previous declaration is here");
            return NULL;
        }
    vpush(c->scope, s);
    return s;
}

static void init_universe(void) {
    struct { const char *n; Type *t; } b[] = {
        {"i8", ty_i8}, {"i16", ty_i16}, {"i32", ty_i32}, {"i64", ty_i64}, {"u8", ty_u8},
        {"u16", ty_u16}, {"u32", ty_u32}, {"u64", ty_u64}, {"f64", ty_f64}, {"bool", ty_bool},
        {"str", ty_str}, {"error", ty_errcode}, {"never", ty_never},
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++) {
        Sym *s = sym_new(S_TYPE, b[i].n, (Pos){0}, NULL);
        s->type = b[i].t; s->pub = true;
        map_put(&universe, s->name, s);
    }
}
Sym *universe_lookup(const char *name) { return map_get(&universe, name); }

Sym *lookup(Ctx *c, const char *name) {
    for (int i = c->scope.n - 1; i >= 0; i--)
        if (!strcmp(c->scope.d[i]->name, name)) return c->scope.d[i];
    if (c->mod) {
        Sym *s = map_get(&c->mod->syms, name);
        if (s) return s;
    }
    if (g_prog.core && c->mod != g_prog.core) {
        Sym *s = map_get(&g_prog.core->syms, name);
        if (s && s->pub) return s;
    }
    return map_get(&universe, name);
}

char *suggest_name(Ctx *c, const char *name) {
    const char *best = NULL; int bd = 3;
    for (int i = 0; i < c->scope.n; i++) {
        int d = edit_distance(name, c->scope.d[i]->name);
        if (d < bd && d < (int)strlen(name)) { bd = d; best = c->scope.d[i]->name; }
    }
    Map *maps[2] = { c->mod ? &c->mod->syms : NULL, g_prog.core ? &g_prog.core->syms : NULL };
    for (int k = 0; k < 2; k++) if (maps[k])
        for (int i = 0; i < maps[k]->cap; i++) if (maps[k]->keys[i]) {
            int d = edit_distance(name, maps[k]->keys[i]);
            if (d < bd && d < (int)strlen(name)) { bd = d; best = maps[k]->keys[i]; }
        }
    return best ? xstrdup(best) : NULL;
}

Sym *lookup_unknown_report(Ctx *c, const char *name, Pos p) {
    Sym *s = lookup(c, name);
    if (s) return s;
    char *sg = suggest_name(c, name);
    if (sg) diag_error(p, "unknown name '%s' (did you mean '%s'?)", name, sg);
    else diag_error(p, "unknown name '%s'", name);
    return NULL;
}

Sym *module_member(Module *m, const char *name, Pos p) {
    Sym *s = map_get(&m->syms, name);
    if (!s || s->kind == S_MODULE) {
        diag_error(p, "module '%s' has no member '%s'", m->name, name);
        return NULL;
    }
    if (!s->pub) {
        diag_error(p, "'%s' is private to module '%s' (declare it with `pub`)", name, m->name);
        return NULL;
    }
    return s;
}

Sym *core_sym(const char *name) {
    Sym *s = map_get(&g_prog.core->syms, name);
    if (!s) die("internal error: core library is missing '%s'", name);
    return s;
}

// ------------------------------------------------------------------ functions
Func *new_func(Sym *s, Node *decl, Module *m) {
    Func *f = xcalloc(sizeof(Func));
    f->sym = s; f->decl = decl; f->mod = m;
    f->is_extern = decl->ext;
    f->winabi = decl->ext && decl->op == 1;
    f->id = g_prog.funcs.n;
    f->asm_name = f->is_extern ? xstrdup(decl->str ? decl->str : decl->name) : xsprintf("orb_%s_%s_%d", m->name, decl->name, f->id);
    s->func = f;
    vpush(g_prog.funcs, f);
    return f;
}
void queue_func(Func *f) { f->queued = true; }

Func *func_sig(Func *f) {
    if (f->sig_done) return f;
    f->sig_done = true;
    Ctx c = {0};
    c.mod = f->mod;
    ctx_push(&c);
    for (int i = 0; i < f->tparams.n; i++) vpush(c.scope, f->tparams.d[i]);
    Node *d = f->decl;
    Type **ps = xcalloc(sizeof(Type *) * (size_t)(d->list.n + 1));
    for (int i = 0; i < d->list.n; i++) {
        ps[i] = resolve_type(&c, d->list.d[i]->a);
        if (ps[i]->kind == TY_VOID || ps[i]->kind == TY_NEVER) { diag_error(d->list.d[i]->pos, "parameter cannot have type '%s'", type_str(ps[i])); ps[i] = ty_err; }
    }
    Type *ret = d->a ? resolve_type(&c, d->a) : ty_void;
    f->type = ty_func(ps, d->list.n, ret);
    return f;
}

static char *type_key(Type **t, int n) {
    char *k = xstrdup("");
    for (int i = 0; i < n; i++) k = xsprintf("%s%d,", k, t[i]->id);
    return k;
}

Func *instantiate_func(Ctx *c, Sym *g, Type **targs, int n, Pos p) {
    (void)c;
    Node *d = g->decl;
    if (n != d->gen.n) { diag_error(p, "'%s' expects %d type argument(s), got %d", g->name, d->gen.n, n); return NULL; }
    char *key = type_key(targs, n);
    Func *f = map_get(&g->insts, key);
    if (f) return f;
    Node *cl = node_clone(d);
    cl->gen.n = 0;
    char *nm = xstrdup(g->name);
    nm = xsprintf("%s<", nm);
    for (int i = 0; i < n; i++) nm = xsprintf("%s%s%s", nm, i ? "," : "", type_str(targs[i]));
    nm = xsprintf("%s>", nm);
    Sym *s = sym_new(S_FUNC, nm, d->pos, g->mod);
    s->pub = g->pub; s->decl = cl;
    f = new_func(s, cl, g->mod);
    f->asm_name = xsprintf("orb_%s_%s_%d", g->mod->name, g->name, f->id);
    for (int i = 0; i < n; i++) {
        Sym *ts = sym_new(S_TYPE, d->gen.d[i], p, g->mod);
        ts->type = targs[i];
        vpush(f->tparams, ts);
    }
    map_put(&g->insts, key, f);
    func_sig(f);
    queue_func(f);
    return f;
}

// ------------------------------------------------------------------ types
Field *find_field(Type *t, const char *name) {
    if (t->kind != TY_STRUCT) return NULL;
    complete_type(t, (Pos){0});
    for (int i = 0; i < t->nfields; i++) if (!strcmp(t->fields[i].name, name)) return &t->fields[i];
    return NULL;
}

static Ctx *type_ctx(Type *t) {
    Ctx *c = xcalloc(sizeof(Ctx));
    c->mod = t->mod;
    ctx_push(c);
    if (t->gsym) {
        Node *d = t->gsym->decl;
        for (int i = 0; i < d->gen.n; i++) {
            Sym *ts = sym_new(S_TYPE, d->gen.d[i], d->pos, t->mod);
            ts->type = t->targs[i];
            vpush(c->scope, ts);
        }
    }
    return c;
}

bool complete_type(Type *t, Pos p) {
    if (t->kind == TY_ARRAY || t->kind == TY_ERRUNION) { if (t->elem) return complete_type(t->elem, p); return true; }
    if (t->kind == TY_ENUM) {
        if (t->state == 2) return true;
        t->state = 2;
        Node *d = t->decl;
        Ctx c = {0}; c.mod = t->mod;
        t->nfields = d->list.n;
        t->fields = xcalloc(sizeof(Field) * (size_t)(d->list.n + 1));
        int64_t next = 0;
        for (int i = 0; i < d->list.n; i++) {
            Node *it = d->list.d[i];
            t->fields[i].name = it->name; t->fields[i].type = t; t->fields[i].pos = it->pos;
            if (it->a) {
                ConstVal v;
                if (eval_const(&c, it->a, &v) && v.kind == 1) next = (int64_t)v.i;
                else diag_error(it->a->pos, "enum value must be a constant integer");
            }
            for (int j = 0; j < i; j++) if (!strcmp(t->fields[j].name, it->name)) diag_error(it->pos, "duplicate enum variant '%s'", it->name);
            t->fields[i].val = next++;
        }
        return true;
    }
    if (t->kind != TY_STRUCT || t->state == 2) return true;
    if (t->state == 1) {
        diag_error(t->decl ? t->decl->pos : p, "struct '%s' contains itself by value (use a pointer)", t->name);
        t->state = 2; t->size = 8; t->align = 8;
        return false;
    }
    t->state = 1;
    Ctx *c = type_ctx(t);
    Node *d = t->decl;
    t->nfields = d->list.n;
    t->fields = xcalloc(sizeof(Field) * (size_t)(d->list.n + 1));
    for (int i = 0; i < d->list.n; i++) {
        Node *f = d->list.d[i];
        t->fields[i].name = f->name; t->fields[i].pos = f->pos;
        for (int j = 0; j < i; j++) if (!strcmp(t->fields[j].name, f->name)) diag_error(f->pos, "duplicate field '%s'", f->name);
        Type *ft = resolve_type(c, f->a);
        if (ft->kind == TY_VOID || ft->kind == TY_NEVER) { diag_error(f->pos, "field cannot have type '%s'", type_str(ft)); ft = ty_err; }
        t->fields[i].type = ft;
    }
    for (int i = 0; i < t->nfields; i++) complete_type(t->fields[i].type, t->fields[i].pos);
    struct_layout(t);
    return true;
}

Type *instantiate_gtype(Ctx *c, Sym *g, Type **targs, int n, Pos p) {
    (void)c;
    Node *d = g->decl;
    if (n != d->gen.n) { diag_error(p, "'%s' expects %d type argument(s), got %d", g->name, d->gen.n, n); return ty_err; }
    char *key = type_key(targs, n);
    Type *t = map_get(&g->insts, key);
    if (t) return t;
    char *nm = xsprintf("%s<", g->name);
    for (int i = 0; i < n; i++) nm = xsprintf("%s%s%s", nm, i ? "," : "", type_str(targs[i]));
    nm = xsprintf("%s>", nm);
    t = ty_new_struct(nm, g->mod, d);
    t->gsym = g; t->targs = xcalloc(sizeof(Type *) * (size_t)n); memcpy(t->targs, targs, sizeof(Type *) * (size_t)n);
    t->ntargs = n;
    map_put(&g->insts, key, t);
    vpush(all_structs, t);
    return t;
}

Type *resolve_type(Ctx *c, Node *tn) {
    switch (tn->kind) {
    case N_TYPE_NAME: {
        Sym *s;
        if (tn->name2) {
            Sym *m = lookup_unknown_report(c, tn->name2, tn->pos);
            if (!m) return ty_err;
            if (m->kind != S_MODULE) { diag_error(tn->pos, "'%s' is not a module", tn->name2); return ty_err; }
            s = module_member(m->target, tn->name, tn->pos);
        } else s = lookup_unknown_report(c, tn->name, tn->pos);
        if (!s) return ty_err;
        if (s->kind == S_TYPE) {
            if (tn->list.n) { diag_error(tn->pos, "type '%s' is not generic", tn->name); return ty_err; }
            return s->type;
        }
        if (s->kind == S_GTYPE) {
            if (!tn->list.n) { diag_error(tn->pos, "generic type '%s' needs type arguments, e.g. %s<i64>", tn->name, tn->name); return ty_err; }
            Type **args = xcalloc(sizeof(Type *) * (size_t)tn->list.n);
            for (int i = 0; i < tn->list.n; i++) {
                args[i] = resolve_type(c, tn->list.d[i]);
                if (args[i]->kind == TY_ERR) return ty_err;
            }
            return instantiate_gtype(c, s, args, tn->list.n, tn->pos);
        }
        diag_error(tn->pos, "'%s' is not a type", tn->name);
        return ty_err;
    }
    case N_TYPE_PTR: {
        Type *e = resolve_type(c, tn->a);
        return e->kind == TY_ERR ? ty_err : ty_ptr(e);
    }
    case N_TYPE_SLICE: {
        Type *e = resolve_type(c, tn->a);
        if (e->kind == TY_ERR) return ty_err;
        if (e->kind == TY_VOID || e->kind == TY_NEVER) { diag_error(tn->pos, "invalid element type"); return ty_err; }
        return ty_slice(e);
    }
    case N_TYPE_ARRAY: {
        Type *e = resolve_type(c, tn->b);
        ConstVal v;
        if (!eval_const(c, tn->a, &v) || v.kind != 1) { diag_error(tn->a->pos, "array length must be a constant integer"); return ty_err; }
        if (v.i < 0 || v.i > (1 << 30)) { diag_error(tn->a->pos, "invalid array length"); return ty_err; }
        if (e->kind == TY_ERR) return ty_err;
        if (e->kind == TY_VOID || e->kind == TY_NEVER) { diag_error(tn->pos, "invalid element type"); return ty_err; }
        complete_type(e, tn->pos);
        return ty_array(e, (int64_t)v.i);
    }
    case N_TYPE_FUNC: {
        Type **ps = xcalloc(sizeof(Type *) * (size_t)(tn->list.n + 1));
        for (int i = 0; i < tn->list.n; i++) { ps[i] = resolve_type(c, tn->list.d[i]); if (ps[i]->kind == TY_ERR) return ty_err; }
        Type *r = tn->a ? resolve_type(c, tn->a) : ty_void;
        if (r->kind == TY_ERR) return ty_err;
        return ty_func(ps, tn->list.n, r);
    }
    case N_TYPE_ERR: {
        Type *p = tn->a ? resolve_type(c, tn->a) : ty_void;
        if (p->kind == TY_ERR) return ty_err;
        if (p->kind == TY_ERRUNION || p->kind == TY_NEVER) { diag_error(tn->pos, "invalid error-union payload type '%s'", type_str(p)); return ty_err; }
        complete_type(p, tn->pos);
        return ty_errunion(p);
    }
    default:
        diag_error(tn->pos, "expected a type");
        return ty_err;
    }
}

bool unify(Ctx *c, Node *tn, Type *actual, char **names, Type **bound, int ntp) {
    switch (tn->kind) {
    case N_TYPE_NAME:
        if (!tn->name2 && !tn->list.n)
            for (int i = 0; i < ntp; i++) if (!strcmp(names[i], tn->name)) {
                if (!bound[i]) { bound[i] = actual; return true; }
                return bound[i] == actual;
            }
        if (tn->list.n) {
            Sym *s = tn->name2 ? NULL : lookup(c, tn->name);
            if (tn->name2) { Sym *m = lookup(c, tn->name2); if (m && m->kind == S_MODULE) s = map_get(&m->target->syms, tn->name); }
            if (!s || s->kind != S_GTYPE) return false;
            if (actual->kind != TY_STRUCT || actual->gsym != s || actual->ntargs != tn->list.n) return false;
            for (int i = 0; i < tn->list.n; i++) if (!unify(c, tn->list.d[i], actual->targs[i], names, bound, ntp)) return false;
            return true;
        }
        {
            int save = g_errors;
            Type *t = resolve_type(c, tn);
            if (g_errors != save) return false;
            return t == actual;
        }
    case N_TYPE_PTR: return actual->kind == TY_PTR && unify(c, tn->a, actual->elem, names, bound, ntp);
    case N_TYPE_SLICE: return actual->kind == TY_SLICE && unify(c, tn->a, actual->elem, names, bound, ntp);
    case N_TYPE_ARRAY: return actual->kind == TY_ARRAY && unify(c, tn->b, actual->elem, names, bound, ntp);
    case N_TYPE_ERR:
        if (actual->kind != TY_ERRUNION) return false;
        return tn->a ? unify(c, tn->a, actual->elem, names, bound, ntp) : actual->elem->kind == TY_VOID;
    case N_TYPE_FUNC:
        if (actual->kind != TY_FUNC || actual->nparams != tn->list.n) return false;
        for (int i = 0; i < tn->list.n; i++) if (!unify(c, tn->list.d[i], actual->params[i], names, bound, ntp)) return false;
        return tn->a ? unify(c, tn->a, actual->ret, names, bound, ntp) : actual->ret->kind == TY_VOID;
    default: return false;
    }
}

// ------------------------------------------------------------------ constants
static bool int_fits_bits(__int128 v, int bits, bool sgn) {
    if (sgn) { __int128 lo = -((__int128)1 << (bits - 1)), hi = ((__int128)1 << (bits - 1)) - 1; return v >= lo && v <= hi; }
    return v >= 0 && v <= (((__int128)1 << bits) - 1);
}
bool int_fits_type(Type *t, __int128 v) { return int_fits_bits(v, ty_bits(t), ty_is_signed(t)); }

static __int128 wrap_to(Type *t, __int128 v) {
    int bits = ty_bits(t);
    if (bits >= 64) return ty_is_signed(t) ? (__int128)(int64_t)v : (__int128)(uint64_t)v;
    uint64_t m = ((uint64_t)1 << bits) - 1;
    uint64_t u = (uint64_t)v & m;
    if (ty_is_signed(t) && (u >> (bits - 1))) return (__int128)(int64_t)(u | ~m);
    return (__int128)u;
}

void const_sym_resolve(Sym *s) {
    Ctx c = {0}; c.mod = s->mod;
    const_sym_resolve_in(&c, s);
}

void const_sym_resolve_in(Ctx *cp, Sym *s) {
    if (s->cstate == 2) return;
    if (s->cstate == 1) { diag_error(s->pos, "constant '%s' depends on itself", s->name); s->cstate = 2; s->type = ty_err; return; }
    s->cstate = 1;
    Ctx *c_ = cp;
    Node *d = s->decl;
    ConstVal v = {0};
    if (!eval_const(c_, d->b, &v)) {
        if (!g_errors) diag_error(d->b->pos, "constant '%s' must be initialized with a constant expression", s->name);
        else diag_error(d->b->pos, "initializer of constant '%s' is not a constant expression", s->name);
        s->type = ty_err; s->cstate = 2; return;
    }
    if (d->a) {
        Type *t = resolve_type(c_, d->a);
        s->type = t;
        if (ty_is_int(t) && v.kind == 1) {
            if (!int_fits_type(t, v.i)) diag_error(d->b->pos, "constant value does not fit in %s", type_str(t));
        } else if (t->kind == TY_F64 && v.kind == 1) { v.f = (double)v.i; v.kind = 2; }
        else if (t->kind == TY_F64 && v.kind == 2) {}
        else if (t->kind == TY_BOOL && v.kind == 3) {}
        else if (t->kind == TY_STR && v.kind == 4) {}
        else if (t->kind != TY_ERR) diag_error(d->b->pos, "cannot initialize constant of type %s with this value", type_str(t));
        v.untyped = false;
    } else {
        s->type = v.kind == 1 ? ty_i64 : v.kind == 2 ? ty_f64 : v.kind == 3 ? ty_bool : ty_str;
    }
    s->cv = v;
    s->cstate = 2;
}

bool eval_const(Ctx *c, Node *n, ConstVal *out) {
    memset(out, 0, sizeof *out);
    switch (n->kind) {
    case N_INT: {
        out->kind = 1;
        if (n->flag) out->i = -(__int128)(uint64_t)((uint64_t)0 - n->ival);
        else out->i = (__int128)n->ival;
        out->untyped = n->aux != 2;
        return true;
    }
    case N_FLOAT: out->kind = 2; out->f = n->fval; out->untyped = true; return true;
    case N_BOOL: out->kind = 3; out->i = (__int128)n->ival; return true;
    case N_STR: out->kind = 4; out->s = n->str; out->slen = n->slen; return true;
    case N_IDENT: {
        Sym *s = lookup(c, n->name);
        if (!s || s->kind != S_CONST) return false;
        const_sym_resolve(s);
        if (s->type == ty_err) return false;
        *out = s->cv;
        return true;
    }
    case N_FIELD: {
        if (n->a->kind != N_IDENT) return false;
        Sym *s = lookup(c, n->a->name);
        if (!s) return false;
        if (s->kind == S_MODULE) {
            Sym *m = map_get(&s->target->syms, n->name);
            if (!m || m->kind != S_CONST || !m->pub) return false;
            const_sym_resolve(m);
            *out = m->cv;
            return m->type != ty_err;
        }
        if (s->kind == S_TYPE && s->type->kind == TY_ENUM) {
            complete_type(s->type, n->pos);
            for (int i = 0; i < s->type->nfields; i++) if (!strcmp(s->type->fields[i].name, n->name)) {
                out->kind = 1; out->i = s->type->fields[i].val; out->untyped = false; return true;
            }
        }
        return false;
    }
    case N_SIZEOF: {
        Type *t = resolve_type(c, n->a);
        if (t->kind == TY_ERR) return false;
        complete_type(t, n->pos);
        out->kind = 1; out->i = type_size(t); out->untyped = true;
        return true;
    }
    case N_UNARY: {
        ConstVal a;
        if (!eval_const(c, n->a, &a)) return false;
        *out = a;
        if (n->op == TK_MINUS && a.kind == 1) { out->i = -a.i; return true; }
        if (n->op == TK_MINUS && a.kind == 2) { out->f = -a.f; return true; }
        if (n->op == TK_BANG && a.kind == 3) { out->i = !a.i; return true; }
        if (n->op == TK_TILDE && a.kind == 1) { out->i = ~a.i; return true; }
        return false;
    }
    case N_CAST: {
        ConstVal a;
        if (!eval_const(c, n->a, &a)) return false;
        Type *t = resolve_type(c, n->b);
        *out = a; out->untyped = false;
        if (ty_is_int(t) && a.kind == 1) { out->i = wrap_to(t, a.i); return true; }
        if (ty_is_int(t) && a.kind == 2) { out->kind = 1; out->i = wrap_to(t, (__int128)a.f); return true; }
        if (ty_is_int(t) && a.kind == 3) { out->kind = 1; return true; }
        if (t->kind == TY_F64 && a.kind == 1) { out->kind = 2; out->f = (double)a.i; return true; }
        if (t->kind == TY_F64 && a.kind == 2) return true;
        return false;
    }
    case N_BINARY: {
        ConstVal a, b;
        if (!eval_const(c, n->a, &a) || !eval_const(c, n->b, &b)) return false;
        bool un = a.untyped && b.untyped;
        if (a.kind == 1 && b.kind == 2 && a.untyped) { a.f = (double)a.i; a.kind = 2; }
        if (b.kind == 1 && a.kind == 2 && b.untyped) { b.f = (double)b.i; b.kind = 2; }
        if (a.kind != b.kind) return false;
        out->untyped = un;
        if (a.kind == 1) {
            __int128 x = a.i, y = b.i;
            out->kind = 1;
            switch (n->op) {
            case TK_PLUS: out->i = x + y; return true;
            case TK_MINUS: out->i = x - y; return true;
            case TK_STAR: out->i = x * y; return true;
            case TK_SLASH: case TK_PERCENT:
                if (y == 0) { diag_error(n->pos, "division by zero in constant expression"); return false; }
                out->i = n->op == TK_SLASH ? x / y : x % y; return true;
            case TK_AMP: out->i = x & y; return true;
            case TK_PIPE: out->i = x | y; return true;
            case TK_CARET: out->i = x ^ y; return true;
            case TK_SHL: if (y < 0 || y > 63) return false; out->i = (__int128)((uint64_t)x << (int)y); if (x < 0) out->i = x << (int)y; return true;
            case TK_SHR: if (y < 0 || y > 63) return false; out->i = x >> (int)y; return true;
            case TK_EQ: out->kind = 3; out->i = x == y; return true;
            case TK_NE: out->kind = 3; out->i = x != y; return true;
            case TK_LT: out->kind = 3; out->i = x < y; return true;
            case TK_LE: out->kind = 3; out->i = x <= y; return true;
            case TK_GT: out->kind = 3; out->i = x > y; return true;
            case TK_GE: out->kind = 3; out->i = x >= y; return true;
            }
            return false;
        }
        if (a.kind == 2) {
            out->kind = 2;
            switch (n->op) {
            case TK_PLUS: out->f = a.f + b.f; return true;
            case TK_MINUS: out->f = a.f - b.f; return true;
            case TK_STAR: out->f = a.f * b.f; return true;
            case TK_SLASH: out->f = a.f / b.f; return true;
            case TK_EQ: out->kind = 3; out->i = a.f == b.f; return true;
            case TK_NE: out->kind = 3; out->i = a.f != b.f; return true;
            case TK_LT: out->kind = 3; out->i = a.f < b.f; return true;
            case TK_LE: out->kind = 3; out->i = a.f <= b.f; return true;
            case TK_GT: out->kind = 3; out->i = a.f > b.f; return true;
            case TK_GE: out->kind = 3; out->i = a.f >= b.f; return true;
            }
            return false;
        }
        if (a.kind == 3) {
            out->kind = 3;
            switch (n->op) {
            case TK_ANDAND: out->i = a.i && b.i; return true;
            case TK_OROR: out->i = a.i || b.i; return true;
            case TK_EQ: out->i = a.i == b.i; return true;
            case TK_NE: out->i = a.i != b.i; return true;
            }
            return false;
        }
        if (a.kind == 4 && (n->op == TK_EQ || n->op == TK_NE)) {
            bool eq = a.slen == b.slen && !memcmp(a.s, b.s, (size_t)a.slen);
            out->kind = 3; out->i = n->op == TK_EQ ? eq : !eq; return true;
        }
        return false;
    }
    default: return false;
    }
}

// ------------------------------------------------------------------ globals
Sym *global_resolve(Sym *s) {
    if (s->state == 2) return s;
    if (s->state == 1) { diag_error(s->pos, "global '%s' depends on itself", s->name); s->type = ty_err; s->state = 2; return s; }
    s->state = 1;
    Ctx c = {0}; c.mod = s->mod;
    Node *d = s->decl;
    ConstVal v = {0};
    bool have = false;
    if (d->b) {
        if (d->b->kind == N_NULL) { v.kind = 5; have = true; }
        else if (eval_const(&c, d->b, &v)) have = true;
        else { diag_error(d->b->pos, "global initializer must be a constant expression"); }
    }
    if (d->a) {
        s->type = resolve_type(&c, d->a);
        complete_type(s->type, d->pos);
        Type *t = s->type;
        if (have && t->kind != TY_ERR) {
            if (ty_is_int(t) && v.kind == 1) { if (!int_fits_type(t, v.i)) diag_error(d->b->pos, "value does not fit in %s", type_str(t)); }
            else if (t->kind == TY_F64 && v.kind == 1) { v.f = (double)v.i; v.kind = 2; }
            else if (t->kind == TY_F64 && v.kind == 2) {}
            else if (t->kind == TY_BOOL && v.kind == 3) {}
            else if (t->kind == TY_STR && v.kind == 4) {}
            else if ((t->kind == TY_PTR || t->kind == TY_FUNC) && v.kind == 5) {}
            else if (ty_is_intlike(t) && v.kind == 1 && t->kind != TY_ERRCODE) {}
            else { diag_error(d->b->pos, "cannot initialize global of type %s with this value", type_str(t)); have = false; }
        }
    } else if (have) {
        s->type = v.kind == 1 ? ty_i64 : v.kind == 2 ? ty_f64 : v.kind == 3 ? ty_bool : v.kind == 4 ? ty_str : ty_err;
        if (v.kind == 5) diag_error(d->b->pos, "cannot infer the type of a null global; add a type annotation");
    } else s->type = ty_err;
    s->cv = have ? v : (ConstVal){0};
    s->state = 2;
    return s;
}

// ------------------------------------------------------------------ module loading & collection
static Module *load_module_text(const char *path, const char *name, char *text, bool is_user);

static char *dir_of(const char *path) {
    const char *sl = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (!sl) return xstrdup(".");
    return xstrndup(path, (size_t)(sl - path));
}

// The standard module `os` is a different file for every target (os_linux, os_macos, os_windows).
static const char *std_file_for(const char *name) {
    if (!strcmp(name, "os")) return xsprintf("os_%s", target_name(g_target));
    return name;
}

static Module *find_module(Module *from, const char *name, Pos p) {
    Module *m = map_get(&loaded, name);
    if (m) return m;
    bool std_only = !from->is_user && !strcmp(name, "os");
    char *cand = xsprintf("%s/%s.orb", dir_of(from->path), name);
    char *text = std_only ? NULL : read_file_text(cand);
    if (text) return load_module_text(cand, name, text, true);
    if (!strcmp(name, "web") && g_target != TGT_WASM) {
        diag_error(p, "the standard module `web` talks to a browser: build with --target wasm (see docs/WEB.md)");
        return NULL;
    }
    const char *real = std_file_for(name);
    if (g_std_dir) {
        cand = xsprintf("%s/%s.orb", g_std_dir, real);
        text = read_file_text(cand);
        if (text) return load_module_text(cand, name, text, false);
    }
    const char *src = std_source(real);
    if (src) return load_module_text(xsprintf("<std>/%s.orb", real), name, xstrdup(src), false);
    diag_error(p, "cannot find module '%s' (looked for %s/%s.orb and the standard library)", name, dir_of(from->path), name);
    return NULL;
}

static void declare_top(Module *m, Sym *s) {
    Sym *old = map_get(&m->syms, s->name);
    if (old) {
        diag_error(s->pos, "'%s' is already defined in module '%s'", s->name, m->name);
        diag_note(old->pos, "previous definition is here");
        return;
    }
    map_put(&m->syms, s->name, s);
}

static void collect(Module *m) {
    for (int i = 0; i < m->ast->list.n; i++) {
        Node *d = m->ast->list.d[i];
        switch (d->kind) {
        case N_FN: {
            if (d->gen.n) {
                if (d->ext) { diag_error(d->pos, "extern functions cannot be generic"); break; }
                Sym *s = sym_new(S_GFUNC, d->name, d->pos, m);
                s->decl = d; s->pub = d->pub;
                declare_top(m, s);
            } else {
                Sym *s = sym_new(S_FUNC, d->name, d->pos, m);
                s->decl = d; s->pub = d->pub;
                new_func(s, d, m);
                declare_top(m, s);
                if (d->ext) s->func->checked = true;
            }
            break;
        }
        case N_STRUCT: {
            Sym *s = sym_new(d->gen.n ? S_GTYPE : S_TYPE, d->name, d->pos, m);
            s->decl = d; s->pub = d->pub;
            if (!d->gen.n) { s->type = ty_new_struct(d->name, m, d); vpush(all_structs, s->type); }
            declare_top(m, s);
            break;
        }
        case N_ENUM: {
            Sym *s = sym_new(S_TYPE, d->name, d->pos, m);
            s->decl = d; s->pub = d->pub;
            s->type = ty_new_enum(d->name, m, d);
            declare_top(m, s);
            break;
        }
        case N_CONSTDECL: {
            Sym *s = sym_new(S_CONST, d->name, d->pos, m);
            s->decl = d; s->pub = d->pub;
            declare_top(m, s);
            break;
        }
        case N_VARDECL: {
            Sym *s = sym_new(S_VAR, d->name, d->pos, m);
            s->decl = d; s->pub = d->pub; s->global = true;
            s->gid = g_prog.globals.n;
            vpush(g_prog.globals, s);
            declare_top(m, s);
            break;
        }
        case N_ERRDECL: {
            Sym *s = sym_new(S_ERROR, d->name, d->pos, m);
            s->decl = d; s->pub = d->pub; s->type = ty_errcode;
            s->errcode = next_errcode++;
            vpush(g_prog.errors, s);
            declare_top(m, s);
            break;
        }
        case N_USE: {
            Module *t = find_module(m, d->name, d->pos);
            if (!t) break;
            if (t == m) { diag_error(d->pos, "a module cannot use itself"); break; }
            Sym *s = sym_new(S_MODULE, d->name, d->pos, m);
            s->target = t;
            declare_top(m, s);
            vpush(m->imports, t);
            break;
        }
        default: break;
        }
    }
}

static Module *load_module_text(const char *path, const char *name, char *text, bool is_user) {
    Module *m = xcalloc(sizeof(Module));
    m->name = xstrdup(name); m->path = xstrdup(path); m->is_user = is_user;
    m->is_core = !strcmp(name, "core") && !is_user;
    m->file = add_source(path, text);
    map_put(&loaded, m->name, m);
    vpush(g_prog.mods, m);
    if (m->is_core) g_prog.core = m;
    m->ast = parse_file(m->file, text);
    collect(m);
    return m;
}

int g_target = TGT_LINUX;
int g_host = TGT_LINUX;
const char *target_name(int t) { return t == TGT_MACOS ? "macos" : t == TGT_WINDOWS ? "windows" : t == TGT_WASM ? "wasm" : "linux"; }

Module *load_std_module(const char *name) {
    if (g_std_dir) {
        char *cand = xsprintf("%s/%s.orb", g_std_dir, name);
        char *text = read_file_text(cand);
        if (text) return load_module_text(cand, name, text, false);
    }
    const char *src = std_source(name);
    if (!src) return NULL;
    return load_module_text(xsprintf("<std>/%s.orb", name), name, xstrdup(src), false);
}

Module *load_module_file(const char *path, const char *modname, bool is_user) {
    char *text = read_file_text(path);
    if (!text) return NULL;
    return load_module_text(path, modname, text, is_user);
}

void sema_init(void) {
    types_init();
    init_universe();
}

// ------------------------------------------------------------------ driver
void sema_run(Module *main_mod) {
    // complete every struct / enum, resolve constants and globals, then check functions
    for (int i = 0; i < all_structs.n; i++) complete_type(all_structs.d[i], all_structs.d[i]->decl ? all_structs.d[i]->decl->pos : (Pos){0});
    for (int i = 0; i < g_prog.mods.n; i++) {
        Module *m = g_prog.mods.d[i];
        for (int k = 0; k < m->syms.cap; k++) {
            Sym *s = m->syms.vals[k];
            if (!m->syms.keys[k]) continue;
            if (s->kind == S_CONST) const_sym_resolve(s);
            else if (s->kind == S_TYPE && s->type->kind == TY_ENUM) complete_type(s->type, s->pos);
        }
        for (int j = 0; j < m->ast->list.n; j++) {   // deterministic order for globals
            Node *d = m->ast->list.d[j];
            if (d->kind == N_VARDECL) { Sym *s = map_get(&m->syms, d->name); if (s && s->kind == S_VAR) global_resolve(s); }
        }
    }
    for (int i = 0; i < g_prog.funcs.n; i++) func_sig(g_prog.funcs.d[i]);
    // check bodies; instantiations append to g_prog.funcs while we iterate
    for (int i = 0; i < g_prog.funcs.n; i++) {
        Func *f = g_prog.funcs.d[i];
        if (f->checked) continue;
        check_func(f);
        f->checked = true;
    }
    for (int i = 0; i < all_structs.n; i++) complete_type(all_structs.d[i], (Pos){0});
    // entry point
    Sym *ms = map_get(&main_mod->syms, "main");
    if (!ms || ms->kind != S_FUNC) {
        if (!g_errors) diag_error((Pos){ main_mod->file, 1, 1 }, "no `main` function found in '%s'", main_mod->name);
    } else {
        Func *f = ms->func;
        g_prog.main_fn = f;
        if (f->type && f->type->nparams) diag_error(ms->pos, "`main` must not take parameters");
        else if (f->type) {
            Type *r = f->type->ret;
            if (!(r->kind == TY_VOID || r->kind == TY_I64 || (r->kind == TY_ERRUNION && r->elem->kind == TY_VOID)))
                diag_error(ms->pos, "`main` must return nothing, i64 (exit code) or `!`");
        }
    }
}
