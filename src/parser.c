#include "ast.h"
#include <setjmp.h>

// ------------------------------------------------------------------ nodes
Node *node_new(NodeKind k, Pos p) {
    Node *n = xcalloc(sizeof(Node));
    n->kind = k; n->pos = p;
    return n;
}

Node *node_clone(Node *n) {
    if (!n) return NULL;
    Node *c = xcalloc(sizeof(Node));
    *c = *n;
    c->type = NULL; c->sym = NULL;
    c->a = node_clone(n->a); c->b = node_clone(n->b); c->c = node_clone(n->c);
    memset(&c->list, 0, sizeof c->list);
    for (int i = 0; i < n->list.n; i++) vpush(c->list, node_clone(n->list.d[i]));
    // generic names are shared (immutable)
    return c;
}

// ------------------------------------------------------------------ parser state
typedef struct {
    TokVec t;
    int pos;
    int nostruct;
    int spec;
    jmp_buf spec_jb;
    jmp_buf top_jb;
} Parser;
static Parser P;

static Token *cur(void) { return &P.t.d[P.pos]; }
static Token *peek(int n) {
    int i = P.pos + n;
    if (i >= P.t.n) i = P.t.n - 1;
    return &P.t.d[i];
}
static TokKind ck(void) { return cur()->kind; }
static Pos cpos(void) { return cur()->pos; }
static void adv(void) { if (P.pos < P.t.n - 1) P.pos++; }

static void fail(Pos p, const char *fmt, ...) {
    if (P.spec) longjmp(P.spec_jb, 1);
    va_list ap; va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_error(p, "%s", buf);
    longjmp(P.top_jb, 1);
}

static char *describe(Token *t) {
    if (t->kind == TK_IDENT) return xsprintf("identifier '%s'", t->s);
    return xstrdup(tok_name(t->kind));
}
static bool accept(TokKind k) { if (ck() == k) { adv(); return true; } return false; }
static void expect(TokKind k, const char *ctx) {
    if (ck() != k && k == TK_SEMI && P.pos > 0 && peek(-1)->eline > 0)
        fail((Pos){ peek(-1)->pos.file, peek(-1)->eline, peek(-1)->ecol }, "expected ';'%s%s", ctx ? " " : "", ctx ? ctx : "");
    if (ck() != k) fail(cpos(), "expected %s%s%s, found %s", tok_name(k), ctx ? " " : "", ctx ? ctx : "", describe(cur()));
    adv();
}
static char *expect_ident(const char *ctx) {
    if (ck() != TK_IDENT) fail(cpos(), "expected identifier%s%s, found %s", ctx ? " " : "", ctx ? ctx : "", describe(cur()));
    char *s = cur()->s;
    adv();
    return s;
}

// ------------------------------------------------------------------ types
static Node *parse_type(void);

static void expect_gt(void) {
    if (ck() == TK_SHR) { cur()->kind = TK_GT; return; }   // split '>>'
    if (ck() == TK_GE) { cur()->kind = TK_ASSIGN; return; }
    expect(TK_GT, NULL);
}

static void parse_type_args(Node *n) {
    expect(TK_LT, NULL);
    do { vpush(n->list, parse_type()); } while (accept(TK_COMMA));
    expect_gt();
}

static Node *parse_type_base(void) {
    Pos p = cpos();
    switch (ck()) {
    case TK_STAR: { adv(); Node *n = node_new(N_TYPE_PTR, p); n->a = parse_type_base(); return n; }
    case TK_LBRACK: {
        adv();
        if (accept(TK_RBRACK)) { Node *n = node_new(N_TYPE_SLICE, p); n->a = parse_type_base(); return n; }
        Node *len = NULL;
        int save = P.nostruct; P.nostruct = 0;
        extern Node *parse_expr_pub(void);
        len = parse_expr_pub();
        P.nostruct = save;
        expect(TK_RBRACK, NULL);
        Node *n = node_new(N_TYPE_ARRAY, p); n->a = len; n->b = parse_type_base();
        return n;
    }
    case TK_FN: {
        adv();
        Node *n = node_new(N_TYPE_FUNC, p);
        expect(TK_LPAREN, NULL);
        if (ck() != TK_RPAREN) do { vpush(n->list, parse_type()); } while (accept(TK_COMMA) && ck() != TK_RPAREN);
        expect(TK_RPAREN, NULL);
        if (accept(TK_ARROW)) n->a = parse_type();
        return n;
    }
    case TK_IDENT: {
        Node *n = node_new(N_TYPE_NAME, p);
        n->name = cur()->s; adv();
        if (ck() == TK_DOT && peek(1)->kind == TK_IDENT) {
            adv();
            n->name2 = n->name;
            n->name = cur()->s; adv();
        }
        if (ck() == TK_LT) parse_type_args(n);
        return n;
    }
    default:
        fail(p, "expected a type, found %s", describe(cur()));
    }
    return NULL;
}

static Node *parse_type(void) {
    Pos p = cpos();
    if (ck() == TK_BANG) { adv(); return node_new(N_TYPE_ERR, p); }
    Node *t = parse_type_base();
    if (ck() == TK_BANG) {
        Pos bp = cpos(); adv();
        Node *e = node_new(N_TYPE_ERR, bp); e->a = t;
        return e;
    }
    return t;
}

// ------------------------------------------------------------------ expressions
static Node *parse_expr(void);
static Node *parse_block(void);

Node *parse_expr_pub(void) { return parse_expr(); }

static Node *expr_to_typename(Node *e) {
    Node *t = node_new(N_TYPE_NAME, e->pos);
    if (e->kind == N_IDENT) t->name = e->name;
    else if (e->kind == N_FIELD && e->a->kind == N_IDENT) { t->name = e->name; t->name2 = e->a->name; }
    else return NULL;
    return t;
}

static bool looks_like_structlit(void) {
    if (ck() != TK_LBRACE || P.nostruct) return false;
    if (peek(1)->kind == TK_RBRACE) return true;
    return peek(1)->kind == TK_IDENT && peek(2)->kind == TK_COLON;
}

static Node *parse_structlit(Node *type) {
    Node *n = node_new(N_STRUCTLIT, type->pos);
    n->a = type;
    expect(TK_LBRACE, NULL);
    int save = P.nostruct; P.nostruct = 0;
    while (ck() != TK_RBRACE) {
        Node *f = node_new(N_FIELDINIT, cpos());
        f->name = expect_ident("for field name");
        expect(TK_COLON, "after field name");
        f->a = parse_expr();
        vpush(n->list, f);
        if (!accept(TK_COMMA)) break;
    }
    P.nostruct = save;
    expect(TK_RBRACE, "to close struct literal");
    return n;
}

static void parse_args(Node *call) {
    int save = P.nostruct; P.nostruct = 0;
    expect(TK_LPAREN, NULL);
    if (ck() != TK_RPAREN) {
        do { if (ck() == TK_RPAREN) break; vpush(call->list, parse_expr()); } while (accept(TK_COMMA));
    }
    P.nostruct = save;
    expect(TK_RPAREN, "to close argument list");
}

static Node *parse_primary(void) {
    Pos p = cpos();
    Token *t = cur();
    switch (t->kind) {
    case TK_INT: { adv(); Node *n = node_new(N_INT, p); n->ival = t->ival; return n; }
    case TK_CHAR: { adv(); Node *n = node_new(N_INT, p); n->ival = t->ival; n->aux = 1; return n; }
    case TK_FLOAT: { adv(); Node *n = node_new(N_FLOAT, p); n->fval = t->fval; return n; }
    case TK_STR: { adv(); Node *n = node_new(N_STR, p); n->str = t->s; n->slen = t->slen; return n; }
    case TK_TRUE: case TK_FALSE: { adv(); Node *n = node_new(N_BOOL, p); n->ival = t->kind == TK_TRUE; return n; }
    case TK_NULL: { adv(); return node_new(N_NULL, p); }
    case TK_LPAREN: {
        adv();
        int save = P.nostruct; P.nostruct = 0;
        Node *e = parse_expr();
        P.nostruct = save;
        expect(TK_RPAREN, NULL);
        return e;
    }
    case TK_LBRACK: {
        adv();
        int save = P.nostruct; P.nostruct = 0;
        Node *n = node_new(N_ARRAYLIT, p);
        if (ck() == TK_RBRACK) fail(p, "empty array literal; declare it with a type, e.g. `a: [4]i64;`");
        Node *first = parse_expr();
        if (accept(TK_SEMI)) {
            n->kind = N_ARRAYREP; n->a = first; n->b = parse_expr();
        } else {
            vpush(n->list, first);
            while (accept(TK_COMMA)) { if (ck() == TK_RBRACK) break; vpush(n->list, parse_expr()); }
        }
        P.nostruct = save;
        expect(TK_RBRACK, "to close array literal");
        return n;
    }
    case TK_IDENT: {
        adv();
        if (peek(-1)->kind == TK_IDENT && ck() == TK_LPAREN) {
            const char *nm = t->s;
            if (!strcmp(nm, "sizeof") || !strcmp(nm, "new") || !strcmp(nm, "make")) {
                adv();
                int save = P.nostruct; P.nostruct = 0;
                Node *n = node_new(!strcmp(nm, "sizeof") ? N_SIZEOF : !strcmp(nm, "new") ? N_NEW : N_MAKE, p);
                n->a = parse_type();
                if (n->kind == N_MAKE) { expect(TK_COMMA, "after type in make(...)"); n->b = parse_expr(); }
                P.nostruct = save;
                expect(TK_RPAREN, NULL);
                return n;
            }
        }
        Node *n = node_new(N_IDENT, p);
        n->name = t->s;
        if (ck() == TK_LT) {
            // possible explicit generic arguments: name<T, U>(...)  or  Name<T>{...}
            int save = P.pos;
            Node *targs = node_new(N_TYPE_NAME, p);
            targs->name = n->name;
            bool ok = false;
            P.spec++;
            jmp_buf old; memcpy(&old, &P.spec_jb, sizeof old);
            if (setjmp(P.spec_jb) == 0) {
                parse_type_args(targs);
                ok = ck() == TK_LPAREN || (ck() == TK_LBRACE && looks_like_structlit());
            }
            memcpy(&P.spec_jb, &old, sizeof old);
            P.spec--;
            if (ok) {
                if (ck() == TK_LBRACE) return parse_structlit(targs);
                n->list = targs->list;     // generic args carried on the identifier
                n->flag = true;
            } else P.pos = save;
        }
        return n;
    }
    default:
        fail(p, "expected an expression, found %s", describe(cur()));
    }
    return NULL;
}

static Node *parse_postfix(void) {
    Node *e = parse_primary();
    for (;;) {
        Pos p = cpos();
        if (ck() == TK_LPAREN) {
            Node *c = node_new(N_CALL, e->pos);
            c->a = e;
            parse_args(c);
            e = c;
        } else if (ck() == TK_LBRACK) {
            adv();
            int save = P.nostruct; P.nostruct = 0;
            Node *lo = NULL;
            if (ck() != TK_DOTDOT && ck() != TK_DOTDOTEQ) lo = parse_expr();
            if (ck() == TK_DOTDOT || ck() == TK_DOTDOTEQ) {
                Node *s = node_new(N_SLICE, p);
                s->aux = ck() == TK_DOTDOTEQ;
                adv();
                s->a = e; s->b = lo;
                if (ck() != TK_RBRACK) s->c = parse_expr();
                e = s;
            } else {
                Node *ix = node_new(N_INDEX, p);
                ix->a = e; ix->b = lo;
                e = ix;
            }
            P.nostruct = save;
            expect(TK_RBRACK, "to close index");
        } else if (ck() == TK_DOT) {
            adv();
            Node *f = node_new(N_FIELD, p);
            f->a = e;
            f->name = expect_ident("after '.'");
            e = f;
        } else if (ck() == TK_LBRACE && (e->kind == N_IDENT || (e->kind == N_FIELD && e->a->kind == N_IDENT)) && looks_like_structlit()) {
            Node *ty = expr_to_typename(e);
            e = parse_structlit(ty);
        } else break;
    }
    return e;
}

static Node *parse_unary(void) {
    Pos p = cpos();
    switch (ck()) {
    case TK_MINUS:
        if (peek(1)->kind == TK_INT || peek(1)->kind == TK_FLOAT) {
            adv();
            Node *lit = parse_postfix();
            if (lit->kind == N_INT) { lit->ival = (uint64_t)0 - lit->ival; lit->flag = true; lit->pos = p; return lit; }
            if (lit->kind == N_FLOAT) { lit->fval = -lit->fval; lit->pos = p; return lit; }
            Node *n = node_new(N_UNARY, p); n->op = TK_MINUS; n->a = lit; return n;
        }
        /* fallthrough */
    case TK_BANG: case TK_TILDE: case TK_STAR: case TK_AMP: {
        TokKind op = ck();
        adv();
        Node *n = node_new(N_UNARY, p);
        n->op = op; n->a = parse_unary();
        return n;
    }
    case TK_TRY: {
        adv();
        Node *n = node_new(N_TRY, p);
        n->a = parse_unary();
        return n;
    }
    default: return parse_postfix();
    }
}

static Node *parse_cast(void) {
    Node *e = parse_unary();
    while (ck() == TK_AS) {
        Pos p = cpos(); adv();
        Node *c = node_new(N_CAST, p);
        c->a = e; c->b = parse_type_base();
        e = c;
    }
    return e;
}

static int binprec(TokKind k) {
    switch (k) {
    case TK_OROR: return 1;
    case TK_ANDAND: return 2;
    case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE: return 3;
    case TK_PIPE: return 4;
    case TK_CARET: return 5;
    case TK_AMP: return 6;
    case TK_SHL: case TK_SHR: return 7;
    case TK_PLUS: case TK_MINUS: return 8;
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 9;
    default: return 0;
    }
}

static Node *parse_bin(int minprec) {
    Node *l = parse_cast();
    for (;;) {
        int pr = binprec(ck());
        if (pr == 0 || pr < minprec) break;
        Token *op = cur();
        adv();
        Node *r = parse_bin(pr + 1);
        Node *n = node_new(N_BINARY, op->pos);
        n->op = op->kind; n->a = l; n->b = r;
        l = n;
    }
    return l;
}

static Node *parse_expr(void) {
    Node *e = parse_bin(1);
    while (ck() == TK_CATCH) {
        Pos p = cpos(); adv();
        Node *c = node_new(N_CATCH, p);
        c->a = e;
        if (ck() == TK_PIPE) {
            adv();
            c->name = expect_ident("for error capture");
            expect(TK_PIPE, NULL);
            int save = P.nostruct; P.nostruct = 0;
            c->c = parse_block();
            P.nostruct = save;
        } else c->b = parse_bin(1);
        e = c;
    }
    return e;
}

// ------------------------------------------------------------------ statements
static Node *parse_stmt(void);

static Node *parse_block(void) {
    Node *b = node_new(N_BLOCK, cpos());
    expect(TK_LBRACE, "to start a block");
    int save = P.nostruct; P.nostruct = 0;
    while (ck() != TK_RBRACE && ck() != TK_EOF) vpush(b->list, parse_stmt());
    P.nostruct = save;
    expect(TK_RBRACE, "to close block");
    return b;
}

static Node *parse_cond(void) {
    int save = P.nostruct; P.nostruct = 1;
    Node *e = parse_expr();
    P.nostruct = save;
    return e;
}

static Node *parse_if(void) {
    Node *n = node_new(N_IF, cpos());
    expect(TK_IF, NULL);
    n->a = parse_cond();
    n->b = parse_block();
    if (accept(TK_ELSE)) {
        if (ck() == TK_IF) n->c = parse_if();
        else n->c = parse_block();
    }
    return n;
}

static TokKind assign_binop(TokKind k) {
    switch (k) {
    case TK_PLUSEQ: return TK_PLUS; case TK_MINUSEQ: return TK_MINUS; case TK_STAREQ: return TK_STAR;
    case TK_SLASHEQ: return TK_SLASH; case TK_PERCENTEQ: return TK_PERCENT; case TK_AMPEQ: return TK_AMP;
    case TK_PIPEEQ: return TK_PIPE; case TK_CARETEQ: return TK_CARET; case TK_SHLEQ: return TK_SHL;
    case TK_SHREQ: return TK_SHR; default: return TK_EOF;
    }
}

static Node *parse_vardecl(void) {
    Node *n = node_new(N_VARDECL, cpos());
    n->name = expect_ident(NULL);
    if (accept(TK_DECL)) {
        n->b = parse_expr();
    } else {
        expect(TK_COLON, NULL);
        n->a = parse_type();
        if (accept(TK_ASSIGN)) n->b = parse_expr();
    }
    expect(TK_SEMI, "after declaration");
    return n;
}

static Node *parse_constdecl(bool pub) {
    Node *n = node_new(N_CONSTDECL, cpos());
    expect(TK_CONST, NULL);
    n->pub = pub;
    n->name = expect_ident("for constant name");
    if (accept(TK_COLON)) n->a = parse_type();
    expect(TK_ASSIGN, "in constant declaration");
    n->b = parse_expr();
    expect(TK_SEMI, "after constant declaration");
    return n;
}

static Node *parse_stmt(void) {
    Pos p = cpos();
    switch (ck()) {
    case TK_LBRACE: return parse_block();
    case TK_IF: return parse_if();
    case TK_WHILE: {
        adv();
        Node *n = node_new(N_WHILE, p);
        n->a = parse_cond();
        n->b = parse_block();
        return n;
    }
    case TK_LOOP: {
        adv();
        Node *n = node_new(N_LOOP, p);
        n->b = parse_block();
        return n;
    }
    case TK_FOR: {
        adv();
        Node *n = node_new(N_FOR, p);
        n->name = expect_ident("for loop variable");
        if (accept(TK_COMMA)) { n->name2 = expect_ident("for loop variable"); }
        expect(TK_IN, "in for loop");
        int save = P.nostruct; P.nostruct = 1;
        n->a = parse_expr();
        if (ck() == TK_DOTDOT || ck() == TK_DOTDOTEQ) {
            n->flag = ck() == TK_DOTDOTEQ;
            adv();
            n->b = parse_expr();
        }
        P.nostruct = save;
        n->c = parse_block();
        return n;
    }
    case TK_BREAK: { adv(); expect(TK_SEMI, NULL); return node_new(N_BREAK, p); }
    case TK_CONTINUE: { adv(); expect(TK_SEMI, NULL); return node_new(N_CONTINUE, p); }
    case TK_RETURN: {
        adv();
        Node *n = node_new(N_RETURN, p);
        if (ck() != TK_SEMI) n->a = parse_expr();
        expect(TK_SEMI, "after return");
        return n;
    }
    case TK_FAIL: {
        adv();
        Node *n = node_new(N_FAIL, p);
        n->a = parse_expr();
        expect(TK_SEMI, "after fail");
        return n;
    }
    case TK_CONST: return parse_constdecl(false);
    case TK_IDENT:
        if (peek(1)->kind == TK_DECL || peek(1)->kind == TK_COLON) return parse_vardecl();
        /* fallthrough */
    default: {
        Node *e = parse_expr();
        TokKind k = ck();
        if (k == TK_ASSIGN || assign_binop(k) != TK_EOF) {
            Node *n = node_new(N_ASSIGN, cpos());
            adv();
            n->op = k == TK_ASSIGN ? 0 : (int)assign_binop(k);
            n->a = e;
            n->b = parse_expr();
            expect(TK_SEMI, "after assignment");
            return n;
        }
        Node *n = node_new(N_EXPRSTMT, e->pos);
        n->a = e;
        expect(TK_SEMI, "after expression");
        return n;
    }
    }
}

// ------------------------------------------------------------------ declarations
static void parse_generic_names(Node *n) {
    if (!accept(TK_LT)) return;
    do { vpush(n->gen, expect_ident("for type parameter")); } while (accept(TK_COMMA));
    expect(TK_GT, NULL);
}

static Node *parse_fn(bool pub, bool ext) {
    Node *n = node_new(N_FN, cpos());
    expect(TK_FN, NULL);
    n->pub = pub; n->ext = ext;
    n->pos = cpos();
    n->name = expect_ident("for function name");
    parse_generic_names(n);
    expect(TK_LPAREN, NULL);
    if (ck() != TK_RPAREN) {
        do {
            if (ck() == TK_RPAREN) break;
            Node *pm = node_new(N_PARAM, cpos());
            pm->name = expect_ident("for parameter name");
            expect(TK_COLON, "after parameter name");
            pm->a = parse_type();
            vpush(n->list, pm);
        } while (accept(TK_COMMA));
    }
    expect(TK_RPAREN, "to close parameter list");
    if (accept(TK_ARROW)) n->a = parse_type();
    if (ext) expect(TK_SEMI, "after extern function");
    else n->b = parse_block();
    return n;
}

static Node *parse_struct(bool pub) {
    Node *n = node_new(N_STRUCT, cpos());
    expect(TK_STRUCT, NULL);
    n->pub = pub;
    n->pos = cpos();
    n->name = expect_ident("for struct name");
    parse_generic_names(n);
    expect(TK_LBRACE, "to start struct body");
    while (ck() != TK_RBRACE && ck() != TK_EOF) {
        Node *f = node_new(N_STRUCTFIELD, cpos());
        f->name = expect_ident("for field name");
        expect(TK_COLON, "after field name");
        f->a = parse_type();
        vpush(n->list, f);
        if (!accept(TK_COMMA)) break;
    }
    expect(TK_RBRACE, "to close struct");
    return n;
}

static Node *parse_enum(bool pub) {
    Node *n = node_new(N_ENUM, cpos());
    expect(TK_ENUM, NULL);
    n->pub = pub;
    n->pos = cpos();
    n->name = expect_ident("for enum name");
    expect(TK_LBRACE, "to start enum body");
    while (ck() != TK_RBRACE && ck() != TK_EOF) {
        Node *it = node_new(N_ENUMITEM, cpos());
        it->name = expect_ident("for enum variant");
        if (accept(TK_ASSIGN)) it->a = parse_expr();
        vpush(n->list, it);
        if (!accept(TK_COMMA)) break;
    }
    expect(TK_RBRACE, "to close enum");
    return n;
}

static void parse_decl(Node *file) {
    bool pub = accept(TK_PUB);
    Pos p = cpos();
    switch (ck()) {
    case TK_FN: vpush(file->list, parse_fn(pub, false)); break;
    case TK_EXTERN: adv(); vpush(file->list, parse_fn(pub, true)); break;
    case TK_STRUCT: vpush(file->list, parse_struct(pub)); break;
    case TK_ENUM: vpush(file->list, parse_enum(pub)); break;
    case TK_CONST: vpush(file->list, parse_constdecl(pub)); break;
    case TK_USE: {
        adv();
        Node *n = node_new(N_USE, p);
        n->name = expect_ident("for module name");
        expect(TK_SEMI, "after use");
        vpush(file->list, n);
        break;
    }
    case TK_ERROR: {
        adv();
        do {
            Node *n = node_new(N_ERRDECL, cpos());
            n->pub = pub;
            n->name = expect_ident("for error name");
            vpush(file->list, n);
        } while (accept(TK_COMMA));
        expect(TK_SEMI, "after error declaration");
        break;
    }
    case TK_IDENT:
        if (peek(1)->kind == TK_DECL || peek(1)->kind == TK_COLON) {
            Node *n = parse_vardecl();
            n->pub = pub;
            vpush(file->list, n);
            break;
        }
        /* fallthrough */
    default:
        fail(p, "expected a declaration (fn, struct, enum, const, use, error or a global variable), found %s", describe(cur()));
    }
}

Node *parse_file(int file, const char *text) {
    memset(&P, 0, sizeof P);
    P.t = lex_source(file, text);
    Node *root = node_new(N_FILE, (Pos){ file, 1, 1 });
    if (setjmp(P.top_jb) != 0) {
        // error recovery: skip to the next top-level declaration
        int depth = 0;
        for (;;) {
            TokKind k = ck();
            if (k == TK_EOF) break;
            if (k == TK_LBRACE) depth++;
            else if (k == TK_RBRACE) { if (depth > 0) depth--; }
            else if (depth == 0 && (k == TK_FN || k == TK_STRUCT || k == TK_ENUM || k == TK_PUB ||
                     k == TK_USE || k == TK_CONST || k == TK_EXTERN || k == TK_ERROR)) break;
            adv();
        }
    }
    while (ck() != TK_EOF) {
        if (setjmp(P.top_jb) != 0) {
            int depth = 0;
            adv();
            for (;;) {
                TokKind k = ck();
                if (k == TK_EOF) break;
                if (k == TK_LBRACE) depth++;
                else if (k == TK_RBRACE) { if (depth > 0) depth--; }
                else if (depth == 0 && (k == TK_FN || k == TK_STRUCT || k == TK_ENUM || k == TK_PUB ||
                         k == TK_USE || k == TK_CONST || k == TK_EXTERN || k == TK_ERROR)) break;
                adv();
            }
            continue;
        }
        parse_decl(root);
    }
    return root;
}
