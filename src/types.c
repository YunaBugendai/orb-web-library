#include "ast.h"

Type *ty_err, *ty_void, *ty_never, *ty_bool, *ty_i8, *ty_i16, *ty_i32, *ty_i64,
     *ty_u8, *ty_u16, *ty_u32, *ty_u64, *ty_f64, *ty_errcode, *ty_null, *ty_str;

static int next_type_id = 1;
static Map array_cache, func_cache;

static Type *mk(TypeKind k, int size, int align) {
    Type *t = xcalloc(sizeof(Type));
    t->kind = k; t->size = size; t->align = align; t->id = next_type_id++;
    t->state = 2;
    return t;
}

void types_init(void) {
    ty_err = mk(TY_ERR, 8, 8);
    ty_void = mk(TY_VOID, 0, 1);
    ty_never = mk(TY_NEVER, 0, 1);
    ty_bool = mk(TY_BOOL, 1, 1);
    ty_i8 = mk(TY_I8, 1, 1); ty_i16 = mk(TY_I16, 2, 2); ty_i32 = mk(TY_I32, 4, 4); ty_i64 = mk(TY_I64, 8, 8);
    ty_u8 = mk(TY_U8, 1, 1); ty_u16 = mk(TY_U16, 2, 2); ty_u32 = mk(TY_U32, 4, 4); ty_u64 = mk(TY_U64, 8, 8);
    ty_f64 = mk(TY_F64, 8, 8);
    ty_errcode = mk(TY_ERRCODE, 8, 8);
    ty_null = mk(TY_NULL, 8, 8);
    ty_str = mk(TY_STR, 16, 8);
}

Type *ty_ptr(Type *e) {
    if (!e->ptr_to) { Type *t = mk(TY_PTR, 8, 8); t->elem = e; e->ptr_to = t; }
    return e->ptr_to;
}
Type *ty_slice(Type *e) {
    if (!e->slice_of) { Type *t = mk(TY_SLICE, 16, 8); t->elem = e; e->slice_of = t; }
    return e->slice_of;
}
Type *ty_errunion(Type *p) {
    if (!p->eu_of) {
        Type *t = mk(TY_ERRUNION, 0, 8);
        t->elem = p;
        int ps = p->kind == TY_VOID ? 0 : type_size(p);
        int pa = p->kind == TY_VOID ? 1 : type_align(p);
        if (pa < 8) pa = 8;
        int off = 8;
        off = (off + pa - 1) / pa * pa;
        t->align = pa;
        t->size = (off + ps + pa - 1) / pa * pa;
        p->eu_of = t;
    }
    return p->eu_of;
}
Type *ty_array(Type *e, int64_t n) {
    char *key = xsprintf("%d:%lld", e->id, (long long)n);
    Type *t = map_get(&array_cache, key);
    if (t) { free(key); return t; }
    t = mk(TY_ARRAY, 0, 1);
    t->elem = e; t->len = n;
    t->size = (int)(n * type_size(e));
    t->align = type_align(e);
    map_put(&array_cache, key, t);
    return t;
}
Type *ty_func(Type **params, int n, Type *ret) {
    char *key = xsprintf("%d", ret->id);
    for (int i = 0; i < n; i++) key = xsprintf("%s,%d", key, params[i]->id);
    Type *t = map_get(&func_cache, key);
    if (t) return t;
    t = mk(TY_FUNC, 8, 8);
    t->params = xcalloc(sizeof(Type *) * (size_t)(n + 1));
    memcpy(t->params, params, sizeof(Type *) * (size_t)n);
    t->nparams = n; t->ret = ret;
    map_put(&func_cache, key, t);
    return t;
}
Type *ty_new_struct(const char *name, Module *m, Node *decl) {
    Type *t = mk(TY_STRUCT, 0, 1);
    t->name = xstrdup(name); t->mod = m; t->decl = decl;
    t->state = 0;
    return t;
}
Type *ty_new_enum(const char *name, Module *m, Node *decl) {
    Type *t = mk(TY_ENUM, 8, 8);
    t->name = xstrdup(name); t->mod = m; t->decl = decl;
    t->state = 0;
    return t;
}

void struct_layout(Type *t) {
    int off = 0, al = 1;
    for (int i = 0; i < t->nfields; i++) {
        int a = type_align(t->fields[i].type), s = type_size(t->fields[i].type);
        off = (off + a - 1) / a * a;
        t->fields[i].off = off;
        off += s;
        if (a > al) al = a;
    }
    t->align = al;
    t->size = (off + al - 1) / al * al;
    t->state = 2;
}

int type_size(Type *t) { return t->size; }
int type_align(Type *t) { return t->align ? t->align : 1; }

bool ty_is_int(Type *t) { return t->kind >= TY_I8 && t->kind <= TY_U64; }
bool ty_is_signed(Type *t) { return t->kind >= TY_I8 && t->kind <= TY_I64; }
bool ty_is_float(Type *t) { return t->kind == TY_F64; }
bool ty_is_intlike(Type *t) { return ty_is_int(t) || t->kind == TY_ENUM || t->kind == TY_ERRCODE; }
bool ty_is_numeric(Type *t) { return ty_is_int(t) || t->kind == TY_F64; }
bool ty_is_ptrlike(Type *t) { return t->kind == TY_PTR || t->kind == TY_FUNC; }
int ty_bits(Type *t) {
    switch (t->kind) {
    case TY_I8: case TY_U8: case TY_BOOL: return 8;
    case TY_I16: case TY_U16: return 16;
    case TY_I32: case TY_U32: return 32;
    default: return 64;
    }
}

TypeClass type_class(Type *t) {
    switch (t->kind) {
    case TY_VOID: case TY_NEVER: return TC_VOID;
    case TY_SLICE: case TY_STR: return TC_PAIR;
    case TY_STRUCT: case TY_ARRAY: return TC_MEM;
    case TY_ERRUNION: {
        Type *p = t->elem;
        if (p->kind == TY_VOID) return TC_PAIR;
        if (type_class(p) == TC_SCALAR && p->kind != TY_F64) return TC_PAIR;
        return TC_MEM;
    }
    default: return TC_SCALAR;
    }
}

char *type_str(Type *t) {
    switch (t->kind) {
    case TY_ERR: return "<error>";
    case TY_VOID: return "void";
    case TY_NEVER: return "never";
    case TY_BOOL: return "bool";
    case TY_I8: return "i8"; case TY_I16: return "i16"; case TY_I32: return "i32"; case TY_I64: return "i64";
    case TY_U8: return "u8"; case TY_U16: return "u16"; case TY_U32: return "u32"; case TY_U64: return "u64";
    case TY_F64: return "f64";
    case TY_ERRCODE: return "error";
    case TY_NULL: return "null";
    case TY_STR: return "str";
    case TY_PTR: return xsprintf("*%s", type_str(t->elem));
    case TY_SLICE: return xsprintf("[]%s", type_str(t->elem));
    case TY_ARRAY: return xsprintf("[%lld]%s", (long long)t->len, type_str(t->elem));
    case TY_ERRUNION: return t->elem->kind == TY_VOID ? "!" : xsprintf("%s!", type_str(t->elem));
    case TY_STRUCT: case TY_ENUM: return t->name;
    case TY_FUNC: {
        char *s = xstrdup("fn(");
        for (int i = 0; i < t->nparams; i++) s = xsprintf("%s%s%s", s, i ? ", " : "", type_str(t->params[i]));
        s = xsprintf("%s)", s);
        if (t->ret->kind != TY_VOID) s = xsprintf("%s -> %s", s, type_str(t->ret));
        return s;
    }
    }
    return "?";
}
