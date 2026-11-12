// Orb! compiler - tokens, AST, types, symbols
#ifndef ORB_AST_H
#define ORB_AST_H
#include "common.h"

// ---------------------------------------------------------------- tokens
typedef enum {
    TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_STR, TK_CHAR,
    TK_FN, TK_STRUCT, TK_ENUM, TK_CONST, TK_USE, TK_PUB, TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_IN,
    TK_LOOP, TK_BREAK, TK_CONTINUE, TK_RETURN, TK_TRUE, TK_FALSE, TK_NULL, TK_AS, TK_TRY,
    TK_CATCH, TK_EXTERN, TK_ERROR, TK_FAIL,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK, TK_COMMA, TK_SEMI,
    TK_COLON, TK_DOT, TK_DOTDOT, TK_DOTDOTEQ, TK_DECL, TK_ASSIGN,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_AMP, TK_PIPE, TK_CARET, TK_TILDE,
    TK_BANG, TK_ANDAND, TK_OROR, TK_SHL, TK_SHR, TK_ARROW,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ, TK_AMPEQ, TK_PIPEEQ,
    TK_CARETEQ, TK_SHLEQ, TK_SHREQ,
    TK__COUNT
} TokKind;

typedef struct {
    TokKind kind;
    Pos pos;
    char *s;        // identifier text / decoded string bytes
    int slen;
    uint64_t ival;
    double fval;
    int eline, ecol;
} Token;

typedef VEC(Token) TokVec;
TokVec lex_source(int file, const char *text);
const char *tok_name(TokKind k);

// ---------------------------------------------------------------- types
typedef enum {
    TY_ERR, TY_VOID, TY_NEVER, TY_BOOL,
    TY_I8, TY_I16, TY_I32, TY_I64, TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F64, TY_ERRCODE, TY_NULL,
    TY_PTR, TY_ARRAY, TY_SLICE, TY_STR, TY_STRUCT, TY_FUNC, TY_ERRUNION, TY_ENUM
} TypeKind;

typedef struct Type Type;
typedef struct Sym Sym;
typedef struct Node Node;
typedef struct Module Module;
typedef struct Func Func;

typedef struct { char *name; Type *type; int off; int64_t val; Pos pos; } Field;

struct Type {
    TypeKind kind;
    int id;
    Type *elem;             // ptr/array/slice/errunion payload
    int64_t len;            // array length
    char *name;             // struct/enum name
    Field *fields; int nfields;
    int size, align;
    int state;              // struct: 0 unresolved, 1 resolving, 2 done
    Node *decl;             // struct/enum declaration
    Module *mod;
    Type **params; int nparams; Type *ret;   // function types
    Sym *gsym; Type **targs; int ntargs;     // generic instance info
    Type *ptr_to, *slice_of, *eu_of;         // interning caches
};

extern Type *ty_err, *ty_void, *ty_never, *ty_bool, *ty_i8, *ty_i16, *ty_i32, *ty_i64,
            *ty_u8, *ty_u16, *ty_u32, *ty_u64, *ty_f64, *ty_errcode, *ty_null, *ty_str;
void types_init(void);
Type *ty_ptr(Type *e);
Type *ty_slice(Type *e);
Type *ty_array(Type *e, int64_t n);
Type *ty_func(Type **params, int n, Type *ret);
Type *ty_errunion(Type *payload);
Type *ty_new_struct(const char *name, Module *m, Node *decl);
Type *ty_new_enum(const char *name, Module *m, Node *decl);
int type_size(Type *t);
int type_align(Type *t);
char *type_str(Type *t);
bool ty_is_int(Type *t);
bool ty_is_signed(Type *t);
int ty_bits(Type *t);                       // bit width of integer-like types
bool ty_is_intlike(Type *t);                // ints, enums, error codes
bool ty_is_numeric(Type *t);
bool ty_is_ptrlike(Type *t);
typedef enum { TC_VOID, TC_SCALAR, TC_PAIR, TC_MEM } TypeClass;
TypeClass type_class(Type *t);
bool ty_is_float(Type *t);
void struct_layout(Type *t);                // computes offsets (fields must be set)

// ---------------------------------------------------------------- AST
typedef enum {
    N_INT, N_FLOAT, N_STR, N_BOOL, N_NULL, N_IDENT, N_BINARY, N_UNARY, N_CALL, N_INDEX, N_SLICE,
    N_FIELD, N_CAST, N_STRUCTLIT, N_FIELDINIT, N_ARRAYLIT, N_ARRAYREP, N_TRY, N_CATCH,
    N_SIZEOF, N_NEW, N_MAKE, N_CONV,
    N_TYPE_NAME, N_TYPE_PTR, N_TYPE_ARRAY, N_TYPE_SLICE, N_TYPE_FUNC, N_TYPE_ERR,
    N_BLOCK, N_VARDECL, N_CONSTDECL, N_ASSIGN, N_EXPRSTMT, N_IF, N_WHILE, N_LOOP, N_FOR,
    N_BREAK, N_CONTINUE, N_RETURN, N_FAIL,
    N_FN, N_PARAM, N_STRUCT, N_STRUCTFIELD, N_ENUM, N_ENUMITEM, N_USE, N_ERRDECL, N_FILE
} NodeKind;

// N_CONV kinds
enum { CV_ARR2SLICE = 1, CV_ERRWRAP, CV_NEVER };
// builtin ids for N_CALL (n->aux)
enum { B_NONE = 0, B_PRINT, B_PRINTLN, B_EPRINT, B_EPRINTLN, B_FREE, B_SYSCALL, B_SQRT,
       B_MEMCPY, B_MEMZERO, B_ERRNAME };
// call kinds in n->op for N_CALL
enum { CALL_DIRECT = 1, CALL_INDIRECT, CALL_BUILTIN };

struct Node {
    NodeKind kind;
    Pos pos;
    int op;                         // operator token / call kind / misc
    char *name, *name2;
    Node *a, *b, *c;
    VEC(Node *) list;
    VEC(char *) gen;                // generic parameter names
    uint64_t ival;
    double fval;
    char *str; int slen;
    bool pub, ext, flag;
    Type *type;
    Sym *sym, *sym2;
    int aux;
};

Node *node_new(NodeKind k, Pos p);
Node *node_clone(Node *n);
Node *parse_file(int file, const char *text);

// ---------------------------------------------------------------- symbols & program
typedef enum { S_VAR, S_FUNC, S_CONST, S_TYPE, S_MODULE, S_ERROR, S_GFUNC, S_GTYPE } SymKind;

typedef struct {
    int kind;                   // 0 none, 1 int, 2 float, 3 bool, 4 str
    __int128 i; double f;
    char *s; int slen;
    bool untyped;
} ConstVal;

struct Sym {
    SymKind kind;
    char *name;
    Pos pos;
    Module *mod;
    Type *type;
    Node *decl;
    bool pub;
    bool global, is_param, addr_taken, readonly, used;
    int loc, r0, r1, slot;      // lowering storage info
    ConstVal cv; int cstate;
    Func *func;
    Map insts;                  // generic instantiations
    int errcode;
    Module *target;             // S_MODULE
    int gid;                    // global symbol id for emission
    Node *init;                 // global initializer
    int state;
};

struct Func {
    Sym *sym;
    Node *decl;
    Module *mod;
    Type *type;                 // TY_FUNC
    char *asm_name;
    bool is_extern, checked, sig_done, queued;
    VEC(Sym *) tparams;
    int id;
};

struct Module {
    char *name, *path;
    int file;
    Node *ast;
    Map syms;
    VEC(Module *) imports;
    bool is_core, is_user;
    int state;
};

typedef struct {
    VEC(Module *) mods;
    VEC(Func *) funcs;
    VEC(Sym *) globals;
    VEC(Sym *) errors;
    Module *core;
    Func *main_fn;
    bool unchecked;             // disable bounds / div checks
} Program;
extern Program g_prog;

// sema entry points
Module *load_module_file(const char *path, const char *modname, bool is_user);
Module *load_std_module(const char *name);
void sema_init(void);
void sema_run(Module *main_mod);
Sym *core_sym(const char *name);                 // lookup pub symbol in core module
const char *std_source(const char *name);        // embedded std library lookup (generated)
const char *std_override_dir(void);
extern const char *g_std_dir;                    // optional directory override

#endif
