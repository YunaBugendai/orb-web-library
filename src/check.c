#include "sema.h"

static Node *mk_conv(Node *n, int kind, Type *t) {
    Node *c = node_new(N_CONV, n->pos);
    c->a = n; c->aux = kind; c->type = t;
    return c;
}

static const char *i128str(__int128 v) {
    if (v < 0) return xsprintf("%lld", (long long)v);
    return xsprintf("%llu", (unsigned long long)v);
}

static Type *lit_int(Node *n, Type *hint) {
    __int128 v = n->flag ? -(__int128)(uint64_t)((uint64_t)0 - n->ival) : (__int128)n->ival;
    Type *t = n->aux == 1 ? ty_u8 : ty_i64;
    if (hint) {
        if (ty_is_int(hint)) t = hint;
        else if (hint->kind == TY_F64) { n->kind = N_FLOAT; n->fval = (double)v; n->type = ty_f64; return ty_f64; }
    }
    if (!int_fits_type(t, v)) { diag_error(n->pos, "integer literal %s does not fit in %s", i128str(v), type_str(t)); }
    n->ival = (uint64_t)v; n->flag = v < 0;
    n->type = t;
    return t;
}

bool is_untyped_lit(Ctx *c, Node *n) {
    switch (n->kind) {
    case N_INT: case N_FLOAT: return n->aux != 2 && !n->type;
    case N_SIZEOF: return true;
    case N_IDENT: {
        if (n->type) return false;
        Sym *s = lookup(c, n->name);
        if (s && s->kind == S_CONST) { const_sym_resolve(s); return s->cv.untyped && s->type != ty_err; }
        return false;
    }
    case N_UNARY: return n->op == TK_MINUS && is_untyped_lit(c, n->a);
    case N_BINARY:
        switch (n->op) {
        case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT:
        case TK_AMP: case TK_PIPE: case TK_CARET: case TK_SHL: case TK_SHR:
            return is_untyped_lit(c, n->a) && is_untyped_lit(c, n->b);
        }
        return false;
    default: return false;
    }
}

static bool is_lvalue(Node *n) {
    switch (n->kind) {
    case N_IDENT: return n->sym && n->sym->kind == S_VAR;
    case N_FIELD:
        if (n->a->type && (n->a->type->kind == TY_SLICE || n->a->type->kind == TY_STR || n->a->type->kind == TY_ARRAY)) return false;
        if (n->a->type && n->a->type->kind == TY_PTR && n->aux == 1 && n->a->type->elem->kind != TY_STRUCT) return false;
        return n->aux == 1 || is_lvalue(n->a);
    case N_INDEX:
        if (!n->a->type) return false;
        if (n->a->type->kind == TY_STR) return false;
        if (n->a->type->kind == TY_ARRAY) return is_lvalue(n->a);
        return true;
    case N_UNARY: return n->op == TK_STAR;
    default: return false;
    }
}

static void mark_addr(Node *n) {
    for (;;) {
        if (n->kind == N_IDENT) { if (n->sym && n->sym->kind == S_VAR) n->sym->addr_taken = true; return; }
        if (n->kind == N_FIELD && n->aux != 1) { n = n->a; continue; }
        if (n->kind == N_INDEX && n->a->type && n->a->type->kind == TY_ARRAY) { n = n->a; continue; }
        return;
    }
}

static Sym *root_var(Node *n) {
    for (;;) {
        if (n->kind == N_IDENT) return n->sym && n->sym->kind == S_VAR ? n->sym : NULL;
        if (n->kind == N_FIELD && n->aux != 1) { n = n->a; continue; }
        if (n->kind == N_INDEX && n->a->type && n->a->type->kind == TY_ARRAY) { n = n->a; continue; }
        return NULL;
    }
}

static Node *convert(Ctx *c, Node *n, Type *want) {
    Type *have = n->type;
    if (!have || have == want || have->kind == TY_ERR || want->kind == TY_ERR) return n;
    if (have->kind == TY_NEVER) return n;
    if (n->kind == N_INT && n->aux != 2 && (ty_is_int(want) || want->kind == TY_F64)) { lit_int(n, want); return n; }
    if (n->kind == N_FLOAT && n->aux != 2 && want->kind == TY_F64) { n->type = want; return n; }
    if (have->kind == TY_NULL && (want->kind == TY_PTR || want->kind == TY_FUNC)) { n->type = want; return n; }
    if (have->kind == TY_ARRAY && want->kind == TY_SLICE && have->elem == want->elem) {
        if (!is_lvalue(n)) { diag_error(n->pos, "cannot convert a temporary array to a slice; store it in a variable first"); return n; }
        mark_addr(n);
        return mk_conv(n, CV_ARR2SLICE, want);
    }
    if (want->kind == TY_ERRUNION && have->kind != TY_ERRUNION) {
        Node *in = convert(c, n, want->elem);
        if (in->type == want->elem) return mk_conv(in, CV_ERRWRAP, want);
        return in;
    }
    diag_error(n->pos, "type mismatch: expected %s, found %s", type_str(want), type_str(have));
    return n;
}

static bool need_bool(Ctx *c, Node **pn, const char *what) {
    Type *t = check_expr(c, *pn, ty_bool);
    if (t->kind == TY_ERR || t->kind == TY_NEVER) return false;
    if (t->kind != TY_BOOL) { diag_error((*pn)->pos, "%s must be bool, found %s", what, type_str(t)); return false; }
    return true;
}

static bool diverges(Node *s);
static bool has_break(Node *s) {
    if (!s) return false;
    switch (s->kind) {
    case N_BREAK: return true;
    case N_BLOCK: for (int i = 0; i < s->list.n; i++) if (has_break(s->list.d[i])) return true; return false;
    case N_IF: return has_break(s->b) || has_break(s->c);
    default: return false;
    }
}
static bool diverges(Node *s) {
    if (!s) return false;
    switch (s->kind) {
    case N_RETURN: case N_FAIL: return true;
    case N_BLOCK: for (int i = 0; i < s->list.n; i++) if (diverges(s->list.d[i])) return true; return false;
    case N_IF: return s->c && diverges(s->b) && diverges(s->c);
    case N_LOOP: return !has_break(s->b);
    case N_WHILE: return s->a->kind == N_BOOL && s->a->ival && !has_break(s->b);
    case N_EXPRSTMT: return s->a->type && s->a->type->kind == TY_NEVER;
    default: return false;
    }
}

static int builtin_id(const char *nm) {
    if (!strcmp(nm, "print")) return B_PRINT;
    if (!strcmp(nm, "println")) return B_PRINTLN;
    if (!strcmp(nm, "eprint")) return B_EPRINT;
    if (!strcmp(nm, "eprintln")) return B_EPRINTLN;
    if (!strcmp(nm, "free")) return B_FREE;
    if (!strcmp(nm, "__syscall")) return B_SYSCALL;
    if (!strcmp(nm, "__sqrt")) return B_SQRT;
    if (!strcmp(nm, "__memcpy")) return B_MEMCPY;
    if (!strcmp(nm, "__memzero")) return B_MEMZERO;
    if (!strcmp(nm, "__errname")) return B_ERRNAME;
    if (!strcmp(nm, "__memgrow")) return B_MEMGROW;
    if (!strcmp(nm, "__memsize")) return B_MEMSIZE;
    return 0;
}

static Type *check_builtin(Ctx *c, Node *n, int b) {
    n->op = CALL_BUILTIN; n->aux = b;
    int na = n->list.n;
    switch (b) {
    case B_PRINT: case B_PRINTLN: case B_EPRINT: case B_EPRINTLN:
        for (int i = 0; i < na; i++) {
            Type *t = check_expr(c, n->list.d[i], NULL);
            if (t->kind == TY_ERR) continue;
            if (!(ty_is_intlike(t) || t->kind == TY_BOOL || t->kind == TY_F64 || t->kind == TY_STR || t->kind == TY_PTR)) {
                diag_error(n->list.d[i]->pos, "cannot print a value of type %s (print fields individually, or cast []u8 to str)", type_str(t));
            }
        }
        return n->type = ty_void;
    case B_FREE: {
        if (na != 1) { diag_error(n->pos, "free expects 1 argument"); return n->type = ty_void; }
        Type *t = check_expr(c, n->list.d[0], NULL);
        if (t->kind != TY_ERR && t->kind != TY_PTR && t->kind != TY_SLICE && t->kind != TY_STR)
            diag_error(n->list.d[0]->pos, "free expects a pointer or slice, found %s", type_str(t));
        return n->type = ty_void;
    }
    case B_SYSCALL: {
        if (na < 1 || na > 7) { diag_error(n->pos, "__syscall takes 1 to 7 arguments"); return n->type = ty_i64; }
        for (int i = 0; i < na; i++) {
            Type *t = check_expr(c, n->list.d[i], ty_i64);
            if (t->kind != TY_ERR && !(ty_is_intlike(t) || t->kind == TY_PTR)) diag_error(n->list.d[i]->pos, "syscall arguments must be integers or pointers");
        }
        return n->type = ty_i64;
    }
    case B_MEMGROW: case B_MEMSIZE: {
        int want = b == B_MEMGROW ? 1 : 0;
        if (g_target != TGT_WASM) { diag_error(n->pos, "__memgrow / __memsize exist only on the wasm target"); return n->type = ty_i64; }
        if (na != want) { diag_error(n->pos, "builtin expects %d argument%s", want, want == 1 ? "" : "s"); return n->type = ty_i64; }
        if (want) check_expr(c, n->list.d[0], ty_i64);
        return n->type = ty_i64;
    }
    case B_ERRNAME:
        if (na != 1) { diag_error(n->pos, "__errname expects 1 argument"); return n->type = ty_str; }
        check_expr(c, n->list.d[0], ty_i64);
        return n->type = ty_str;
    case B_SQRT:
        if (na != 1) { diag_error(n->pos, "__sqrt expects 1 argument"); return n->type = ty_f64; }
        check_expr(c, n->list.d[0], ty_f64);
        n->list.d[0] = convert(c, n->list.d[0], ty_f64);
        return n->type = ty_f64;
    case B_MEMCPY: case B_MEMZERO: {
        int want = b == B_MEMCPY ? 3 : 2;
        if (na != want) { diag_error(n->pos, "builtin expects %d arguments", want); return n->type = ty_void; }
        for (int i = 0; i < na; i++) {
            Type *t = check_expr(c, n->list.d[i], ty_i64);
            bool isptr = i < want - 1;
            if (t->kind != TY_ERR && ((isptr && t->kind != TY_PTR) || (!isptr && !ty_is_int(t))))
                diag_error(n->list.d[i]->pos, "bad argument type %s", type_str(t));
        }
        return n->type = ty_void;
    }
    }
    return n->type = ty_err;
}

// --------------------------------------------------------------------- binary
static bool is_cmp(int op) { return op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE; }

static Type *check_binary(Ctx *c, Node *n, Type *hint) {
    int op = n->op;
    if (op == TK_ANDAND || op == TK_OROR) {
        bool a = need_bool(c, &n->a, "operand of logical operator");
        bool b = need_bool(c, &n->b, "operand of logical operator");
        return n->type = (a && b) ? ty_bool : ty_err;
    }
    Type *h = is_cmp(op) ? NULL : hint;
    if (op == TK_SHL || op == TK_SHR) {
        Type *ta = check_expr(c, n->a, h);
        Type *tb = check_expr(c, n->b, ty_i64);
        if (ta->kind == TY_ERR || tb->kind == TY_ERR) return n->type = ty_err;
        if (!ty_is_int(ta) || !ty_is_int(tb)) { diag_error(n->pos, "shift operands must be integers, found %s and %s", type_str(ta), type_str(tb)); return n->type = ty_err; }
        return n->type = ta;
    }
    bool la = is_untyped_lit(c, n->a), lb = is_untyped_lit(c, n->b);
    Type *ta, *tb;
    if (la && !lb) { tb = check_expr(c, n->b, h); ta = check_expr(c, n->a, tb->kind == TY_ERR ? NULL : tb); }
    else { ta = check_expr(c, n->a, h); tb = check_expr(c, n->b, ta->kind == TY_ERR ? h : ta); }
    if (ta->kind == TY_ERR || tb->kind == TY_ERR) return n->type = ty_err;
    // pointer arithmetic
    if (ta->kind == TY_PTR && (op == TK_PLUS || op == TK_MINUS) && ty_is_int(tb)) return n->type = ta;
    if (ta->kind == TY_PTR && tb->kind == TY_PTR && op == TK_MINUS && ta == tb) return n->type = ty_i64;
    // null comparisons / literal adaptation
    if (ta != tb) {
        if (ta->kind == TY_NULL && (tb->kind == TY_PTR || tb->kind == TY_FUNC)) n->a = convert(c, n->a, tb), ta = tb;
        else if (tb->kind == TY_NULL && (ta->kind == TY_PTR || ta->kind == TY_FUNC)) n->b = convert(c, n->b, ta), tb = ta;
        else if (n->a->kind == N_INT && n->a->aux != 2 && (ty_is_int(tb) || tb->kind == TY_F64)) { n->a = convert(c, n->a, tb); ta = n->a->type; }
        else if (n->b->kind == N_INT && n->b->aux != 2 && (ty_is_int(ta) || ta->kind == TY_F64)) { n->b = convert(c, n->b, ta); tb = n->b->type; }
    }
    if (ta != tb) {
        diag_error(n->pos, "operator %s needs operands of the same type, found %s and %s (use `as` to convert)", tok_name((TokKind)op), type_str(ta), type_str(tb));
        return n->type = ty_err;
    }
    Type *t = ta;
    if (is_cmp(op)) {
        bool eq = op == TK_EQ || op == TK_NE;
        bool ok = ty_is_numeric(t) || t->kind == TY_ENUM || t->kind == TY_PTR ||
                  (eq && (t->kind == TY_BOOL || t->kind == TY_ERRCODE || t->kind == TY_FUNC || t->kind == TY_STR));
        if (!ok) { diag_error(n->pos, "cannot compare values of type %s with %s", type_str(t), tok_name((TokKind)op)); return n->type = ty_err; }
        return n->type = ty_bool;
    }
    switch (op) {
    case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH:
        if (!ty_is_numeric(t)) break;
        if ((op == TK_SLASH) && ty_is_int(t) && n->b->kind == N_INT && n->b->ival == 0) diag_error(n->b->pos, "division by zero");
        return n->type = t;
    case TK_PERCENT:
        if (!ty_is_int(t)) break;
        if (n->b->kind == N_INT && n->b->ival == 0) diag_error(n->b->pos, "division by zero");
        return n->type = t;
    case TK_AMP: case TK_PIPE: case TK_CARET:
        if (!ty_is_int(t)) break;
        return n->type = t;
    }
    diag_error(n->pos, "operator %s cannot be applied to type %s", tok_name((TokKind)op), type_str(t));
    return n->type = ty_err;
}

// --------------------------------------------------------------------- casts
static bool cast_ok(Type *from, Type *to) {
    if (from == to) return true;
    bool fi = ty_is_intlike(from) || from->kind == TY_BOOL, ti = ty_is_intlike(to);
    if (fi && ti) return true;
    if ((ty_is_int(from) || from->kind == TY_ENUM) && to->kind == TY_F64) return true;
    if (from->kind == TY_F64 && (ty_is_int(to) || to->kind == TY_F64)) return true;
    if ((from->kind == TY_PTR || from->kind == TY_FUNC) && (to->kind == TY_PTR || to->kind == TY_FUNC)) return true;
    if ((from->kind == TY_PTR || from->kind == TY_FUNC) && (to->kind == TY_I64 || to->kind == TY_U64)) return true;
    if (ty_is_int(from) && to->kind == TY_PTR) return true;
    if (from->kind == TY_NULL && (to->kind == TY_PTR || to->kind == TY_FUNC)) return true;
    if (from->kind == TY_SLICE && from->elem == ty_u8 && to->kind == TY_STR) return true;
    if (from->kind == TY_STR && to->kind == TY_SLICE && to->elem == ty_u8) return true;
    return false;
}

// --------------------------------------------------------------------- calls
static Type *finish_args(Ctx *c, Node *n, Type *ft, int nrecv, const char *name) {
    int np = ft->nparams;
    if (n->list.n != np) {
        diag_error(n->pos, "'%s' expects %d argument%s, found %d", name, np, np == 1 ? "" : "s", n->list.n);
        for (int i = 0; i < n->list.n; i++) if (i >= nrecv) check_expr(c, n->list.d[i], NULL);
        return n->type = ft->ret;
    }
    for (int i = 0; i < np; i++) {
        check_expr(c, n->list.d[i], ft->params[i]);
        n->list.d[i] = convert(c, n->list.d[i], ft->params[i]);
    }
    return n->type = ft->ret;
}

// adapt a receiver expression (already checked) to parameter type; returns NULL if not possible
static Node *adapt_receiver(Node *recv, Type *want) {
    Type *rt = recv->type;
    if (rt == want) return recv;
    if (want->kind == TY_PTR && want->elem == rt && is_lvalue(recv)) {
        mark_addr(recv);
        Node *u = node_new(N_UNARY, recv->pos);
        u->op = TK_AMP; u->a = recv; u->type = want;
        return u;
    }
    if (rt->kind == TY_PTR && rt->elem == want) {
        Node *u = node_new(N_UNARY, recv->pos);
        u->op = TK_STAR; u->a = recv; u->type = want;
        return u;
    }
    if (rt->kind == TY_ARRAY && want->kind == TY_SLICE && rt->elem == want->elem && is_lvalue(recv)) { mark_addr(recv); return mk_conv(recv, CV_ARR2SLICE, want); }
    return NULL;
}

static Type *call_generic(Ctx *c, Node *n, Sym *g, Node *recv_checked, Node *explicit_from) {
    Node *d = g->decl;
    int ng = d->gen.n;
    Type **bound = xcalloc(sizeof(Type *) * (size_t)ng);
    Ctx tc = {0}; tc.mod = g->mod;
    if (explicit_from) {
        if (explicit_from->list.n > ng) { diag_error(n->pos, "too many type arguments for '%s'", g->name); return n->type = ty_err; }
        for (int i = 0; i < explicit_from->list.n; i++) bound[i] = resolve_type(c, explicit_from->list.d[i]);
    }
    int nargs = n->list.n + (recv_checked ? 1 : 0);
    if (nargs != d->list.n) {
        diag_error(n->pos, "'%s' expects %d argument%s, found %d", g->name, d->list.n, d->list.n == 1 ? "" : "s", nargs);
        return n->type = ty_err;
    }
    Node **args = xcalloc(sizeof(Node *) * (size_t)(nargs + 1));
    int k = 0;
    if (recv_checked) args[k++] = recv_checked;
    for (int i = 0; i < n->list.n; i++) args[k++] = n->list.d[i];
    int start = recv_checked ? 1 : 0;
    bool *done = xcalloc((size_t)nargs + 1);
    if (recv_checked) {
        // receiver: try as-is, then by address, then by value through pointer
        Type *rt = recv_checked->type;
        Type *cands[2] = { rt, rt->kind == TY_PTR ? rt->elem : ty_ptr(rt) };
        bool ok = false;
        for (int ci = 0; ci < 2 && !ok; ci++) {
            Type **save = xcalloc(sizeof(Type *) * (size_t)ng);
            memcpy(save, bound, sizeof(Type *) * (size_t)ng);
            if (unify(&tc, d->list.d[0]->a, cands[ci], d->gen.d, bound, ng)) ok = true;
            else memcpy(bound, save, sizeof(Type *) * (size_t)ng);
        }
        if (!ok && rt->kind == TY_ARRAY) {
            Type *sl = ty_slice(rt->elem);
            ok = unify(&tc, d->list.d[0]->a, sl, d->gen.d, bound, ng);
        }
        if (!ok) { diag_error(n->pos, "cannot call '%s' on a value of type %s", g->name, type_str(rt)); return n->type = ty_err; }
        done[0] = true;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = start; i < nargs; i++) {
            if (done[i]) continue;
            bool lit = is_untyped_lit(c, args[i]);
            if ((pass == 0) == lit) continue;
            Node *pt = d->list.d[i]->a;
            Type *hint = NULL;
            if (pt->kind == N_TYPE_NAME && !pt->name2 && !pt->list.n)
                for (int q = 0; q < ng; q++) if (!strcmp(d->gen.d[q], pt->name)) hint = bound[q];
            Type *at = check_expr(c, args[i], hint);
            done[i] = true;
            if (at->kind == TY_ERR) return n->type = ty_err;
            Type *ua = at;
            if (pt->kind == N_TYPE_SLICE && at->kind == TY_ARRAY) ua = ty_slice(at->elem);
            if (!unify(&tc, pt, ua, d->gen.d, bound, ng)) {
                diag_error(args[i]->pos, "argument %d of '%s': type %s does not match the parameter", i + 1 - (recv_checked ? 1 : 0) + (recv_checked ? 1 : 0), g->name, type_str(at));
                return n->type = ty_err;
            }
        }
    for (int q = 0; q < ng; q++) if (!bound[q]) {
        diag_error(n->pos, "cannot infer type parameter '%s' of '%s'; write %s<Type>(...)", d->gen.d[q], g->name, g->name);
        return n->type = ty_err;
    }
    Func *f = instantiate_func(c, g, bound, ng, n->pos);
    if (!f) return n->type = ty_err;
    Type *ft = f->type;
    // rebuild argument list with the receiver included
    n->list.n = 0;
    for (int i = 0; i < nargs; i++) vpush(n->list, args[i]);
    for (int i = 0; i < nargs; i++) {
        if (i == 0 && recv_checked) {
            Node *r = adapt_receiver(args[0], ft->params[0]);
            if (!r) { diag_error(args[0]->pos, "cannot pass receiver of type %s as %s", type_str(args[0]->type), type_str(ft->params[0])); return n->type = ty_err; }
            n->list.d[0] = r;
        } else n->list.d[i] = convert(c, args[i], ft->params[i]);
    }
    n->op = CALL_DIRECT; n->sym = f->sym;
    return n->type = ft->ret;
}

static Sym *find_method(Ctx *c, const char *name) {
    Sym *s = lookup(c, name);
    if (s && (s->kind == S_FUNC || s->kind == S_GFUNC)) return s;
    if (c->mod) for (int i = 0; i < c->mod->imports.n; i++) {
        Sym *m = map_get(&c->mod->imports.d[i]->syms, name);
        if (m && m->pub && (m->kind == S_FUNC || m->kind == S_GFUNC)) return m;
    }
    return NULL;
}

static Type *check_call(Ctx *c, Node *n, Type *hint) {
    (void)hint;
    Node *callee = n->a;
    Sym *fs = NULL;
    Node *recv = NULL;
    Node *explicit_from = NULL;

    if (callee->kind == N_IDENT) {
        Sym *s = lookup(c, callee->name);
        if (!s) {
            int b = builtin_id(callee->name);
            if (b && !callee->flag) return check_builtin(c, n, b);
            lookup_unknown_report(c, callee->name, callee->pos);
            for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i], NULL);
            return n->type = ty_err;
        }
        if (s->kind == S_FUNC || s->kind == S_GFUNC) { fs = s; if (callee->flag) explicit_from = callee; }
        else if (s->kind == S_TYPE || s->kind == S_GTYPE) {
            diag_error(callee->pos, "'%s' is a type; use `value as %s` to convert", callee->name, callee->name);
            return n->type = ty_err;
        }
    } else if (callee->kind == N_FIELD) {
        if (callee->a->kind == N_IDENT) {
            Sym *m = lookup(c, callee->a->name);
            if (m && m->kind == S_MODULE) {
                fs = module_member(m->target, callee->name, callee->pos);
                if (!fs) { for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i], NULL); return n->type = ty_err; }
                if (fs->kind != S_FUNC && fs->kind != S_GFUNC) fs = NULL;
                else {
                    Node *id = node_new(N_IDENT, callee->pos);
                    id->name = callee->name;
                    callee = n->a = id;
                }
            }
        }
        if (!fs && callee->kind == N_FIELD) {
            Type *bt = check_expr(c, callee->a, NULL);
            if (bt->kind == TY_ERR) { for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i], NULL); return n->type = ty_err; }
            Type *st = bt->kind == TY_PTR ? bt->elem : bt;
            Field *fld = st->kind == TY_STRUCT ? find_field(st, callee->name) : NULL;
            if (!fld) {
                Sym *m = find_method(c, callee->name);
                if (!m) {
                    diag_error(callee->pos, "type %s has no field or method '%s'", type_str(bt), callee->name);
                    for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i], NULL);
                    return n->type = ty_err;
                }
                fs = m; recv = callee->a;
                Node *id = node_new(N_IDENT, callee->pos);
                id->name = callee->name;
                callee = n->a = id;
            }
        }
    }

    if (fs) {
        if (fs->kind == S_GFUNC) return call_generic(c, n, fs, recv, explicit_from);
        Func *f = fs->func;
        func_sig(f);
        f->sym->used = true;
        Type *ft = f->type;
        if (recv) {
            if (ft->nparams < 1) { diag_error(n->pos, "'%s' takes no arguments and cannot be used as a method", fs->name); return n->type = ty_err; }
            Node *r = adapt_receiver(recv, ft->params[0]);
            if (!r) { diag_error(recv->pos, "cannot call '%s' on a value of type %s (expects %s)", fs->name, type_str(recv->type), type_str(ft->params[0])); return n->type = ty_err; }
            vpush(n->list, NULL);
            for (int i = n->list.n - 1; i > 0; i--) n->list.d[i] = n->list.d[i - 1];
            n->list.d[0] = r;
        }
        n->op = CALL_DIRECT; n->sym = fs;
        callee->sym = fs; callee->type = ft;
        if (recv) {
            // receiver already checked; check remaining args
            if (n->list.n != ft->nparams) { diag_error(n->pos, "'%s' expects %d argument%s, found %d", fs->name, ft->nparams - 1, ft->nparams == 2 ? "" : "s", n->list.n - 1); return n->type = ft->ret; }
            for (int i = 1; i < ft->nparams; i++) { check_expr(c, n->list.d[i], ft->params[i]); n->list.d[i] = convert(c, n->list.d[i], ft->params[i]); }
            return n->type = ft->ret;
        }
        return finish_args(c, n, ft, 0, fs->name);
    }

    // indirect call through a function value
    Type *ct = check_expr(c, callee, NULL);
    if (ct->kind == TY_ERR) { for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i], NULL); return n->type = ty_err; }
    if (ct->kind != TY_FUNC) { diag_error(callee->pos, "cannot call a value of type %s", type_str(ct)); for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i], NULL); return n->type = ty_err; }
    n->op = CALL_INDIRECT;
    return finish_args(c, n, ct, 0, "function value");
}

// --------------------------------------------------------------------- expressions
static Type *use_sym(Ctx *c, Node *n, Sym *s, Type *hint) {
    n->sym = s;
    switch (s->kind) {
    case S_VAR:
        if (s->global) global_resolve(s);
        s->used = true;
        return n->type = s->type;
    case S_CONST: {
        const_sym_resolve(s);
        if (s->type == ty_err) return n->type = ty_err;
        ConstVal *v = &s->cv;
        if (v->kind == 1) {
            n->kind = N_INT; n->ival = (uint64_t)v->i; n->flag = v->i < 0; n->aux = v->untyped ? 0 : 2; n->type = NULL;
            if (v->untyped) return lit_int(n, hint);
            if (ty_is_int(s->type)) n->type = s->type; else n->type = s->type;
            return n->type;
        }
        if (v->kind == 2) { n->kind = N_FLOAT; n->fval = v->f; n->aux = v->untyped ? 0 : 2; return n->type = ty_f64; }
        if (v->kind == 3) { n->kind = N_BOOL; n->ival = (uint64_t)v->i; return n->type = ty_bool; }
        n->kind = N_STR; n->str = v->s; n->slen = v->slen; return n->type = ty_str;
    }
    case S_FUNC:
        func_sig(s->func);
        s->used = true;
        return n->type = s->func->type;
    case S_GFUNC: {
        if (!n->flag) { diag_error(n->pos, "generic function '%s' must be called, or instantiated like %s<Type>", s->name, s->name); return n->type = ty_err; }
        Type **ta = xcalloc(sizeof(Type *) * (size_t)(n->list.n + 1));
        for (int i = 0; i < n->list.n; i++) ta[i] = resolve_type(c, n->list.d[i]);
        Func *f = instantiate_func(c, s, ta, n->list.n, n->pos);
        if (!f) return n->type = ty_err;
        n->sym = f->sym;
        return n->type = f->type;
    }
    case S_ERROR:
        n->kind = N_INT; n->ival = (uint64_t)s->errcode; n->flag = false; n->aux = 2;
        return n->type = ty_errcode;
    case S_TYPE: case S_GTYPE:
        diag_error(n->pos, "'%s' is a type, not a value", n->name);
        return n->type = ty_err;
    case S_MODULE:
        diag_error(n->pos, "'%s' is a module, not a value", n->name);
        return n->type = ty_err;
    }
    return n->type = ty_err;
}

static Type *check_field(Ctx *c, Node *n, Type *hint) {
    Node *a = n->a;
    if (a->kind == N_IDENT && !a->type) {
        Sym *s = lookup(c, a->name);
        if (s && s->kind == S_MODULE) {
            Sym *m = module_member(s->target, n->name, n->pos);
            if (!m) return n->type = ty_err;
            n->kind = N_IDENT;
            return use_sym(c, n, m, hint);
        }
        if (s && s->kind == S_TYPE && s->type->kind == TY_ENUM) {
            complete_type(s->type, n->pos);
            for (int i = 0; i < s->type->nfields; i++) if (!strcmp(s->type->fields[i].name, n->name)) {
                n->kind = N_INT; n->ival = (uint64_t)s->type->fields[i].val; n->flag = s->type->fields[i].val < 0; n->aux = 2;
                return n->type = s->type;
            }
            diag_error(n->pos, "enum '%s' has no variant '%s'", s->name, n->name);
            return n->type = ty_err;
        }
        if (s && (s->kind == S_TYPE || s->kind == S_GTYPE)) { diag_error(n->pos, "type '%s' has no static member '%s'", a->name, n->name); return n->type = ty_err; }
    }
    Type *bt = check_expr(c, a, NULL);
    if (bt->kind == TY_ERR) return n->type = ty_err;
    Type *st = bt;
    if (bt->kind == TY_PTR && (bt->elem->kind == TY_STRUCT || bt->elem->kind == TY_SLICE || bt->elem->kind == TY_STR || bt->elem->kind == TY_ARRAY)) { st = bt->elem; n->aux = 1; }
    switch (st->kind) {
    case TY_STRUCT: {
        Field *f = find_field(st, n->name);
        if (!f) {
            char *best = NULL; int bd = 3;
            for (int i = 0; i < st->nfields; i++) { int d = edit_distance(n->name, st->fields[i].name); if (d < bd) { bd = d; best = st->fields[i].name; } }
            if (best) diag_error(n->pos, "type %s has no field '%s' (did you mean '%s'?)", type_str(st), n->name, best);
            else diag_error(n->pos, "type %s has no field '%s'", type_str(st), n->name);
            return n->type = ty_err;
        }
        n->ival = (uint64_t)(f - st->fields);
        return n->type = f->type;
    }
    case TY_SLICE: case TY_STR:
        if (!strcmp(n->name, "len")) return n->type = ty_i64;
        if (!strcmp(n->name, "ptr")) return n->type = ty_ptr(st->kind == TY_STR ? ty_u8 : st->elem);
        break;
    case TY_ARRAY:
        if (!strcmp(n->name, "len")) {
            n->kind = N_INT; n->ival = (uint64_t)st->len; n->flag = false; n->aux = 2; n->a = NULL;
            return n->type = ty_i64;
        }
        break;
    default: break;
    }
    diag_error(n->pos, "type %s has no field '%s'", type_str(bt), n->name);
    return n->type = ty_err;
}

static Type *check_index_expr(Ctx *c, Node *n) {
    Type *bt = check_expr(c, n->a, NULL);
    Type *it = check_expr(c, n->b, ty_i64);
    if (bt->kind == TY_ERR || it->kind == TY_ERR) return n->type = ty_err;
    if (!ty_is_int(it)) { diag_error(n->b->pos, "index must be an integer, found %s", type_str(it)); return n->type = ty_err; }
    switch (bt->kind) {
    case TY_ARRAY:
        if (n->b->kind == N_INT) {
            __int128 v = n->b->flag ? -(__int128)(uint64_t)((uint64_t)0 - n->b->ival) : (__int128)n->b->ival;
            if (v < 0 || v >= bt->len) diag_error(n->b->pos, "index %s is out of bounds for array of length %lld", i128str(v), (long long)bt->len);
        }
        return n->type = bt->elem;
    case TY_SLICE: return n->type = bt->elem;
    case TY_STR: return n->type = ty_u8;
    case TY_PTR: n->aux = 1; return n->type = bt->elem;
    default:
        diag_error(n->pos, "cannot index a value of type %s", type_str(bt));
        return n->type = ty_err;
    }
}

static Type *check_unary(Ctx *c, Node *n, Type *hint) {
    switch (n->op) {
    case TK_AMP: {
        Type *h = hint && hint->kind == TY_PTR ? hint->elem : NULL;
        Type *t = check_expr(c, n->a, h);
        if (t->kind == TY_ERR) return n->type = ty_err;
        if (n->a->kind == N_IDENT && n->a->sym && n->a->sym->kind == S_FUNC) { diag_error(n->pos, "functions are already pointers; drop the `&`"); return n->type = ty_err; }
        if (!is_lvalue(n->a)) { diag_error(n->pos, "cannot take the address of this expression"); return n->type = ty_err; }
        mark_addr(n->a);
        return n->type = ty_ptr(t);
    }
    case TK_STAR: {
        Type *t = check_expr(c, n->a, NULL);
        if (t->kind == TY_ERR) return n->type = ty_err;
        if (t->kind != TY_PTR) { diag_error(n->pos, "cannot dereference a value of type %s", type_str(t)); return n->type = ty_err; }
        complete_type(t->elem, n->pos);
        return n->type = t->elem;
    }
    case TK_MINUS: {
        Type *t = check_expr(c, n->a, hint);
        if (t->kind == TY_ERR) return n->type = ty_err;
        if (!(ty_is_signed(t) || t->kind == TY_F64)) { diag_error(n->pos, "cannot negate a value of type %s", type_str(t)); return n->type = ty_err; }
        return n->type = t;
    }
    case TK_BANG:
        return need_bool(c, &n->a, "operand of '!'") ? (n->type = ty_bool) : (n->type = ty_err);
    case TK_TILDE: {
        Type *t = check_expr(c, n->a, hint);
        if (t->kind == TY_ERR) return n->type = ty_err;
        if (!ty_is_int(t)) { diag_error(n->pos, "'~' needs an integer, found %s", type_str(t)); return n->type = ty_err; }
        return n->type = t;
    }
    }
    return n->type = ty_err;
}

static Type *check_cast(Ctx *c, Node *n) {
    Type *to = resolve_type(c, n->b);
    Type *from = check_expr(c, n->a, NULL);
    if (to->kind == TY_ERR || from->kind == TY_ERR) return n->type = to;
    if (!cast_ok(from, to)) {
        if (from->kind == TY_ARRAY && to->kind == TY_SLICE && from->elem == to->elem) {
            n->a = convert(c, n->a, to);
            return n->type = to;
        }
        diag_error(n->pos, "cannot cast %s to %s", type_str(from), type_str(to));
    }
    return n->type = to;
}

static Type *check_structlit(Ctx *c, Node *n) {
    Type *t = resolve_type(c, n->a);
    if (t->kind == TY_ERR) { for (int i = 0; i < n->list.n; i++) check_expr(c, n->list.d[i]->a, NULL); return n->type = ty_err; }
    if (t->kind != TY_STRUCT) { diag_error(n->pos, "'%s' is not a struct", type_str(t)); return n->type = ty_err; }
    complete_type(t, n->pos);
    for (int i = 0; i < n->list.n; i++) {
        Node *fi = n->list.d[i];
        Field *f = find_field(t, fi->name);
        if (!f) { diag_error(fi->pos, "struct %s has no field '%s'", type_str(t), fi->name); check_expr(c, fi->a, NULL); continue; }
        for (int j = 0; j < i; j++) if (!strcmp(n->list.d[j]->name, fi->name)) diag_error(fi->pos, "field '%s' is initialized twice", fi->name);
        fi->ival = (uint64_t)(f - t->fields);
        check_expr(c, fi->a, f->type);
        fi->a = convert(c, fi->a, f->type);
    }
    return n->type = t;
}

Type *check_expr(Ctx *c, Node *n, Type *hint) {
    if (n->type) return n->type;
    switch (n->kind) {
    case N_INT: return lit_int(n, hint);
    case N_FLOAT: return n->type = ty_f64;
    case N_STR: return n->type = ty_str;
    case N_BOOL: return n->type = ty_bool;
    case N_NULL: return n->type = ty_null;
    case N_IDENT: {
        Sym *s = lookup_unknown_report(c, n->name, n->pos);
        if (!s) return n->type = ty_err;
        return use_sym(c, n, s, hint);
    }
    case N_FIELD: return check_field(c, n, hint);
    case N_INDEX: return check_index_expr(c, n);
    case N_SLICE: {
        Type *bt = check_expr(c, n->a, NULL);
        if (n->b) check_expr(c, n->b, ty_i64);
        if (n->c) check_expr(c, n->c, ty_i64);
        if (bt->kind == TY_ERR) return n->type = ty_err;
        for (int i = 0; i < 2; i++) {
            Node *x = i ? n->c : n->b;
            if (x && x->type->kind != TY_ERR && !ty_is_int(x->type)) { diag_error(x->pos, "slice bounds must be integers"); return n->type = ty_err; }
        }
        if (bt->kind == TY_ARRAY) {
            if (!is_lvalue(n->a)) { diag_error(n->pos, "cannot slice a temporary array"); return n->type = ty_err; }
            mark_addr(n->a);
            return n->type = ty_slice(bt->elem);
        }
        if (bt->kind == TY_SLICE) return n->type = bt;
        if (bt->kind == TY_STR) return n->type = ty_str;
        diag_error(n->pos, "cannot slice a value of type %s", type_str(bt));
        return n->type = ty_err;
    }
    case N_UNARY: return check_unary(c, n, hint);
    case N_BINARY: return check_binary(c, n, hint);
    case N_CALL: return check_call(c, n, hint);
    case N_CAST: return check_cast(c, n);
    case N_STRUCTLIT: return check_structlit(c, n);
    case N_ARRAYLIT: {
        Type *eh = hint && hint->kind == TY_ARRAY ? hint->elem : NULL;
        Type *et = eh;
        for (int i = 0; i < n->list.n; i++) {
            Type *t = check_expr(c, n->list.d[i], et);
            if (!et && t->kind != TY_ERR) et = t;
        }
        if (!et || et->kind == TY_NULL || et->kind == TY_VOID || et->kind == TY_NEVER) { diag_error(n->pos, "cannot infer the element type of this array literal"); return n->type = ty_err; }
        for (int i = 0; i < n->list.n; i++) n->list.d[i] = convert(c, n->list.d[i], et);
        complete_type(et, n->pos);
        return n->type = ty_array(et, n->list.n);
    }
    case N_ARRAYREP: {
        Type *eh = hint && hint->kind == TY_ARRAY ? hint->elem : NULL;
        Type *et = check_expr(c, n->a, eh);
        ConstVal v;
        if (!eval_const(c, n->b, &v) || v.kind != 1 || v.i < 0 || v.i > (1 << 28)) { diag_error(n->b->pos, "array repeat count must be a constant non-negative integer"); return n->type = ty_err; }
        if (et->kind == TY_ERR) return n->type = ty_err;
        if (eh) { n->a = convert(c, n->a, eh); et = eh; }
        complete_type(et, n->pos);
        return n->type = ty_array(et, (int64_t)v.i);
    }
    case N_TRY: {
        Type *t = check_expr(c, n->a, NULL);
        if (t->kind == TY_ERR) return n->type = ty_err;
        if (t->kind != TY_ERRUNION) { diag_error(n->pos, "`try` needs an error-union value (T!), found %s", type_str(t)); return n->type = ty_err; }
        if (!c->fn || c->ret->kind != TY_ERRUNION) { diag_error(n->pos, "`try` can only be used in a function that returns an error union (e.g. `-> !`)"); return n->type = t->elem; }
        return n->type = t->elem;
    }
    case N_CATCH: {
        Type *t = check_expr(c, n->a, NULL);
        if (t->kind == TY_ERR) { if (n->b) check_expr(c, n->b, NULL); return n->type = ty_err; }
        if (t->kind != TY_ERRUNION) { diag_error(n->pos, "`catch` needs an error-union value (T!), found %s", type_str(t)); return n->type = ty_err; }
        Type *pt = t->elem;
        if (n->b) {
            Type *bt = check_expr(c, n->b, pt);
            if (pt->kind == TY_VOID) { if (bt->kind != TY_VOID && bt->kind != TY_ERR) diag_error(n->b->pos, "nothing to default to: the payload is void"); }
            else n->b = convert(c, n->b, pt);
        } else {
            ctx_push(c);
            Sym *s = sym_new(S_VAR, n->name, n->pos, c->mod);
            s->type = ty_errcode; s->used = true;
            ctx_declare(c, s);
            n->sym = s;
            // check the block statements in the same scope
            extern void check_block_nopush(Ctx *, Node *);
            check_block_nopush(c, n->c);
            ctx_pop(c);
            if (pt->kind != TY_VOID && !diverges(n->c))
                diag_error(n->c->pos, "a `catch |e| { ... }` block must end with return, break, continue or a call that never returns (or use `catch default_value`)");
        }
        return n->type = pt;
    }
    case N_SIZEOF: {
        Type *t;
        Sym *vs = (n->a->kind == N_TYPE_NAME && !n->a->name2 && !n->a->list.n) ? lookup(c, n->a->name) : NULL;
        if (vs && vs->kind == S_VAR) { if (vs->global) global_resolve(vs); t = vs->type; }
        else t = resolve_type(c, n->a);
        if (t->kind == TY_ERR) return n->type = ty_err;
        complete_type(t, n->pos);
        n->kind = N_INT; n->ival = (uint64_t)type_size(t); n->flag = false; n->aux = 0;
        return lit_int(n, hint);
    }
    case N_NEW: {
        Type *t = resolve_type(c, n->a);
        if (t->kind == TY_ERR) return n->type = ty_err;
        if (t->kind == TY_VOID || t->kind == TY_NEVER) { diag_error(n->pos, "cannot allocate void"); return n->type = ty_err; }
        complete_type(t, n->pos);
        return n->type = ty_ptr(t);
    }
    case N_MAKE: {
        Type *t = resolve_type(c, n->a);
        Type *ct = check_expr(c, n->b, ty_i64);
        if (t->kind == TY_ERR || ct->kind == TY_ERR) return n->type = ty_err;
        if (!ty_is_int(ct)) { diag_error(n->b->pos, "make count must be an integer"); return n->type = ty_err; }
        if (t->kind == TY_VOID || t->kind == TY_NEVER) { diag_error(n->pos, "cannot allocate void"); return n->type = ty_err; }
        complete_type(t, n->pos);
        return n->type = ty_slice(t);
    }
    default:
        diag_error(n->pos, "unexpected expression");
        return n->type = ty_err;
    }
}

// --------------------------------------------------------------------- statements
static void check_stmt(Ctx *c, Node *s);

void check_block_nopush(Ctx *c, Node *b) {
    for (int i = 0; i < b->list.n; i++) check_stmt(c, b->list.d[i]);
}
static void check_block(Ctx *c, Node *b) {
    ctx_push(c);
    check_block_nopush(c, b);
    ctx_pop(c);
}

static Sym *declare_local(Ctx *c, Node *n, const char *name, Type *t, Pos p) {
    Sym *s = sym_new(S_VAR, name, p, c->mod);
    s->type = t;
    ctx_declare(c, s);
    (void)n;
    return s;
}

static void check_stmt(Ctx *c, Node *s) {
    switch (s->kind) {
    case N_BLOCK: check_block(c, s); break;
    case N_VARDECL: {
        Type *t = NULL;
        if (s->a) { t = resolve_type(c, s->a); complete_type(t, s->pos); }
        if (s->b) {
            Type *it = check_expr(c, s->b, t);
            if (t) s->b = convert(c, s->b, t);
            else {
                t = it;
                if (t->kind == TY_NULL) { diag_error(s->b->pos, "cannot infer a type from `null`; annotate the variable"); t = ty_err; }
                else if (t->kind == TY_VOID || t->kind == TY_NEVER) { diag_error(s->b->pos, "cannot store a value of type %s", type_str(t)); t = ty_err; }
            }
        }
        if (t->kind == TY_VOID || t->kind == TY_NEVER) { diag_error(s->pos, "variable cannot have type %s", type_str(t)); t = ty_err; }
        s->sym = declare_local(c, s, s->name, t, s->pos);
        break;
    }
    case N_CONSTDECL: {
        Sym *cs = sym_new(S_CONST, s->name, s->pos, c->mod);
        cs->decl = s;
        const_sym_resolve_in(c, cs);
        ctx_declare(c, cs);
        break;
    }
    case N_ASSIGN: {
        Type *lt = check_expr(c, s->a, NULL);
        if (lt->kind == TY_ERR) { check_expr(c, s->b, NULL); break; }
        if (!is_lvalue(s->a)) { diag_error(s->a->pos, "left side of assignment cannot be assigned to"); check_expr(c, s->b, NULL); break; }
        Sym *rv = root_var(s->a);
        if (rv && rv->readonly) diag_error(s->a->pos, "cannot assign to loop variable '%s'", rv->name);
        if (s->op == 0) {
            check_expr(c, s->b, lt);
            s->b = convert(c, s->b, lt);
        } else {
            Node *tmp = node_new(N_BINARY, s->pos);
            tmp->op = s->op; tmp->a = s->a; tmp->b = s->b;
            Type *rt = check_binary(c, tmp, lt);
            s->b = tmp->b;
            if (rt->kind != TY_ERR && rt != lt) diag_error(s->pos, "compound assignment would change the type from %s to %s", type_str(lt), type_str(rt));
        }
        break;
    }
    case N_EXPRSTMT: {
        Type *t = check_expr(c, s->a, NULL);
        if (t->kind == TY_ERRUNION) diag_error(s->a->pos, "unhandled error: use `try`, or `catch` to handle the result of this call");
        else if (s->a->kind != N_CALL && s->a->kind != N_TRY && s->a->kind != N_CATCH && t->kind != TY_ERR)
            diag_error(s->a->pos, "expression result is unused");
        break;
    }
    case N_IF:
        need_bool(c, &s->a, "condition");
        check_block(c, s->b);
        if (s->c) { if (s->c->kind == N_IF) check_stmt(c, s->c); else check_block(c, s->c); }
        break;
    case N_WHILE:
        need_bool(c, &s->a, "condition");
        c->loops++; check_block(c, s->b); c->loops--;
        break;
    case N_LOOP:
        c->loops++; check_block(c, s->b); c->loops--;
        break;
    case N_FOR: {
        Type *vt = ty_err, *et = NULL;
        ctx_push(c);
        if (s->b) {
            if (s->name2) diag_error(s->pos, "range loops take a single loop variable");
            bool la = is_untyped_lit(c, s->a), lb = is_untyped_lit(c, s->b);
            Type *ta, *tb;
            if (la && !lb) { tb = check_expr(c, s->b, NULL); ta = check_expr(c, s->a, tb); }
            else { ta = check_expr(c, s->a, NULL); tb = check_expr(c, s->b, ta); }
            if (ta->kind != TY_ERR && tb->kind != TY_ERR) {
                if (!ty_is_int(ta)) diag_error(s->a->pos, "range bounds must be integers, found %s", type_str(ta));
                else { s->b = convert(c, s->b, ta); s->a = convert(c, s->a, tb == ta ? ta : ta); vt = ta; }
            }
            s->sym = declare_local(c, s, s->name, vt, s->pos);
            s->sym->readonly = true;
        } else {
            Type *it = check_expr(c, s->a, NULL);
            if (it->kind == TY_ARRAY || it->kind == TY_SLICE) et = it->elem;
            else if (it->kind == TY_STR) et = ty_u8;
            else if (it->kind != TY_ERR) diag_error(s->a->pos, "cannot iterate over a value of type %s (use an array, slice, str or a range a..b)", type_str(it));
            if (it->kind == TY_ARRAY && is_lvalue(s->a)) mark_addr(s->a);
            if (s->name2) {
                s->sym = declare_local(c, s, s->name, ty_i64, s->pos);
                s->sym->readonly = true;
                s->sym2 = declare_local(c, s, s->name2, et ? et : ty_err, s->pos);
                s->sym2->readonly = true;
            } else {
                s->sym2 = declare_local(c, s, s->name, et ? et : ty_err, s->pos);
                s->sym2->readonly = true;
            }
        }
        c->loops++; check_block(c, s->c); c->loops--;
        ctx_pop(c);
        break;
    }
    case N_BREAK: case N_CONTINUE:
        if (!c->loops) diag_error(s->pos, "`%s` outside of a loop", s->kind == N_BREAK ? "break" : "continue");
        break;
    case N_RETURN: {
        Type *r = c->ret;
        bool voidlike = r->kind == TY_VOID || r->kind == TY_NEVER || (r->kind == TY_ERRUNION && r->elem->kind == TY_VOID);
        if (r->kind == TY_NEVER) diag_error(s->pos, "a function returning `never` cannot return");
        if (s->a) {
            Type *h = r->kind == TY_ERRUNION ? r->elem : r;
            Type *t = check_expr(c, s->a, h);
            if (r->kind == TY_VOID) { diag_error(s->a->pos, "this function does not return a value"); }
            else if (r->kind == TY_ERRUNION && r->elem->kind == TY_VOID && t != r) diag_error(s->a->pos, "this function returns `!` (no value); use `return;`");
            else s->a = convert(c, s->a, r);
        } else if (!voidlike) diag_error(s->pos, "missing return value (expected %s)", type_str(r));
        break;
    }
    case N_FAIL: {
        Type *t = check_expr(c, s->a, ty_errcode);
        if (c->ret->kind != TY_ERRUNION) diag_error(s->pos, "`fail` can only be used in a function that returns an error union (e.g. `-> i64!`)");
        if (t->kind != TY_ERR && t->kind != TY_ERRCODE) diag_error(s->a->pos, "`fail` needs an error value, found %s (declare errors with `error Name;`)", type_str(t));
        break;
    }
    default:
        diag_error(s->pos, "unexpected statement");
    }
}

void check_func(Func *f) {
    if (f->is_extern) return;
    func_sig(f);
    Ctx c = {0};
    c.mod = f->mod; c.fn = f; c.ret = f->type->ret;
    ctx_push(&c);
    for (int i = 0; i < f->tparams.n; i++) vpush(c.scope, f->tparams.d[i]);
    Node *d = f->decl;
    for (int i = 0; i < d->list.n; i++) {
        Node *pm = d->list.d[i];
        Type *pt = f->type->params[i];
        complete_type(pt, pm->pos);
        Sym *s = sym_new(S_VAR, pm->name, pm->pos, f->mod);
        s->type = pt; s->is_param = true; s->used = true;
        ctx_declare(&c, s);
        pm->sym = s;
    }
    if (c.ret->kind != TY_VOID) complete_type(c.ret, d->pos);
    check_block(&c, d->b);
    Type *r = c.ret;
    bool needs = !(r->kind == TY_VOID || (r->kind == TY_ERRUNION && r->elem->kind == TY_VOID));
    if (needs && !diverges(d->b))
        diag_error(d->pos, "function '%s' may reach its end without returning a value of type %s", d->name, type_str(r));
}
