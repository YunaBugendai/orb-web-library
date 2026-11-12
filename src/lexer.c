#include "ast.h"
#include <ctype.h>

static const struct { const char *s; TokKind k; } keywords[] = {
    {"fn", TK_FN}, {"struct", TK_STRUCT}, {"enum", TK_ENUM}, {"const", TK_CONST}, {"use", TK_USE},
    {"pub", TK_PUB}, {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE}, {"for", TK_FOR},
    {"in", TK_IN}, {"loop", TK_LOOP}, {"break", TK_BREAK}, {"continue", TK_CONTINUE},
    {"return", TK_RETURN}, {"true", TK_TRUE}, {"false", TK_FALSE}, {"null", TK_NULL},
    {"as", TK_AS}, {"try", TK_TRY}, {"catch", TK_CATCH}, {"extern", TK_EXTERN},
    {"error", TK_ERROR}, {"fail", TK_FAIL},
};

static const char *tok_names[TK__COUNT] = {
    [TK_EOF] = "end of file", [TK_IDENT] = "identifier", [TK_INT] = "integer", [TK_FLOAT] = "float",
    [TK_STR] = "string", [TK_CHAR] = "character",
    [TK_FN] = "'fn'", [TK_STRUCT] = "'struct'", [TK_ENUM] = "'enum'", [TK_CONST] = "'const'",
    [TK_USE] = "'use'", [TK_PUB] = "'pub'", [TK_IF] = "'if'", [TK_ELSE] = "'else'",
    [TK_WHILE] = "'while'", [TK_FOR] = "'for'", [TK_IN] = "'in'", [TK_LOOP] = "'loop'",
    [TK_BREAK] = "'break'", [TK_CONTINUE] = "'continue'", [TK_RETURN] = "'return'",
    [TK_TRUE] = "'true'", [TK_FALSE] = "'false'", [TK_NULL] = "'null'", [TK_AS] = "'as'",
    [TK_TRY] = "'try'", [TK_CATCH] = "'catch'", [TK_EXTERN] = "'extern'", [TK_ERROR] = "'error'",
    [TK_FAIL] = "'fail'",
    [TK_LPAREN] = "'('", [TK_RPAREN] = "')'", [TK_LBRACE] = "'{'", [TK_RBRACE] = "'}'",
    [TK_LBRACK] = "'['", [TK_RBRACK] = "']'", [TK_COMMA] = "','", [TK_SEMI] = "';'",
    [TK_COLON] = "':'", [TK_DOT] = "'.'", [TK_DOTDOT] = "'..'", [TK_DOTDOTEQ] = "'..='",
    [TK_DECL] = "':='", [TK_ASSIGN] = "'='", [TK_EQ] = "'=='", [TK_NE] = "'!='", [TK_LT] = "'<'",
    [TK_LE] = "'<='", [TK_GT] = "'>'", [TK_GE] = "'>='", [TK_PLUS] = "'+'", [TK_MINUS] = "'-'",
    [TK_STAR] = "'*'", [TK_SLASH] = "'/'", [TK_PERCENT] = "'%'", [TK_AMP] = "'&'",
    [TK_PIPE] = "'|'", [TK_CARET] = "'^'", [TK_TILDE] = "'~'", [TK_BANG] = "'!'",
    [TK_ANDAND] = "'&&'", [TK_OROR] = "'||'", [TK_SHL] = "'<<'", [TK_SHR] = "'>>'",
    [TK_ARROW] = "'->'", [TK_PLUSEQ] = "'+='", [TK_MINUSEQ] = "'-='", [TK_STAREQ] = "'*='",
    [TK_SLASHEQ] = "'/='", [TK_PERCENTEQ] = "'%='", [TK_AMPEQ] = "'&='", [TK_PIPEEQ] = "'|='",
    [TK_CARETEQ] = "'^='", [TK_SHLEQ] = "'<<='", [TK_SHREQ] = "'>>='",
};
const char *tok_name(TokKind k) { return tok_names[k] ? tok_names[k] : "?"; }

typedef struct { const char *src; int pos, line, col, file; } Lx;

static int peekc(Lx *l, int o) { return (unsigned char)l->src[l->pos + o]; }
static void adv(Lx *l) {
    if (l->src[l->pos] == '\n') { l->line++; l->col = 1; } else l->col++;
    l->pos++;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// parses an escape after the backslash; returns byte value or -1 on error
static int lex_escape(Lx *l) {
    int c = peekc(l, 0);
    adv(l);
    switch (c) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case '0': return 0; case '\\': return '\\'; case '\'': return '\''; case '"': return '"';
    case 'x': {
        int h1 = hexval(peekc(l, 0)), h2 = hexval(peekc(l, 1));
        if (h1 < 0 || h2 < 0) return -1;
        adv(l); adv(l);
        return h1 * 16 + h2;
    }
    default: return -1;
    }
}

static void add_tok(TokVec *v, TokKind k, Pos p) {
    Token t = { .kind = k, .pos = p };
    vpush(*v, t);
}

TokVec lex_source(int file, const char *text) {
    Lx l = { text, 0, 1, 1, file };
    TokVec out = {0};
    for (;;) {
        if (out.n) { out.d[out.n - 1].eline = l.line; out.d[out.n - 1].ecol = l.col; }
        // skip whitespace and comments
        for (;;) {
            int c = peekc(&l, 0);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') adv(&l);
            else if (c == '/' && peekc(&l, 1) == '/') { while (peekc(&l, 0) && peekc(&l, 0) != '\n') adv(&l); }
            else if (c == '/' && peekc(&l, 1) == '*') {
                Pos sp = { file, l.line, l.col };
                adv(&l); adv(&l);
                while (peekc(&l, 0) && !(peekc(&l, 0) == '*' && peekc(&l, 1) == '/')) adv(&l);
                if (!peekc(&l, 0)) { diag_error(sp, "unterminated block comment"); break; }
                adv(&l); adv(&l);
            } else break;
        }
        Pos p = { file, l.line, l.col };
        int c = peekc(&l, 0);
        if (!c) { add_tok(&out, TK_EOF, p); break; }

        if (isalpha(c) || c == '_') {
            int st = l.pos;
            while (isalnum(peekc(&l, 0)) || peekc(&l, 0) == '_') adv(&l);
            char *s = xstrndup(text + st, (size_t)(l.pos - st));
            TokKind k = TK_IDENT;
            for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++)
                if (!strcmp(keywords[i].s, s)) { k = keywords[i].k; break; }
            add_tok(&out, k, p);
            out.d[out.n - 1].s = s;
            continue;
        }
        if (isdigit(c)) {
            int st = l.pos;
            bool isfloat = false;
            uint64_t v = 0;
            bool overflow = false;
            if (c == '0' && (peekc(&l, 1) == 'x' || peekc(&l, 1) == 'X')) {
                adv(&l); adv(&l);
                int nd = 0;
                while (hexval(peekc(&l, 0)) >= 0 || peekc(&l, 0) == '_') {
                    if (peekc(&l, 0) != '_') { if (v >> 60) overflow = true; v = v * 16 + (uint64_t)hexval(peekc(&l, 0)); nd++; }
                    adv(&l);
                }
                if (!nd) diag_error(p, "hexadecimal literal has no digits");
            } else if (c == '0' && (peekc(&l, 1) == 'b' || peekc(&l, 1) == 'B')) {
                adv(&l); adv(&l);
                int nd = 0;
                while (peekc(&l, 0) == '0' || peekc(&l, 0) == '1' || peekc(&l, 0) == '_') {
                    if (peekc(&l, 0) != '_') { if (v >> 63) overflow = true; v = v * 2 + (uint64_t)(peekc(&l, 0) - '0'); nd++; }
                    adv(&l);
                }
                if (!nd) diag_error(p, "binary literal has no digits");
            } else {
                while (isdigit(peekc(&l, 0)) || peekc(&l, 0) == '_') {
                    if (peekc(&l, 0) != '_') {
                        uint64_t d = (uint64_t)(peekc(&l, 0) - '0');
                        if (v > (UINT64_MAX - d) / 10) overflow = true;
                        v = v * 10 + d;
                    }
                    adv(&l);
                }
                if (peekc(&l, 0) == '.' && isdigit(peekc(&l, 1))) {
                    isfloat = true;
                    adv(&l);
                    while (isdigit(peekc(&l, 0)) || peekc(&l, 0) == '_') adv(&l);
                }
                if ((peekc(&l, 0) == 'e' || peekc(&l, 0) == 'E') &&
                    (isdigit(peekc(&l, 1)) || ((peekc(&l, 1) == '-' || peekc(&l, 1) == '+') && isdigit(peekc(&l, 2))))) {
                    isfloat = true;
                    adv(&l); adv(&l);
                    while (isdigit(peekc(&l, 0))) adv(&l);
                }
            }
            if (isalpha(peekc(&l, 0)) || peekc(&l, 0) == '_')
                diag_error((Pos){ file, l.line, l.col }, "unexpected character after number literal");
            if (isfloat) {
                char *buf = xcalloc((size_t)(l.pos - st) + 1);
                int n = 0;
                for (int i = st; i < l.pos; i++) if (text[i] != '_') buf[n++] = text[i];
                add_tok(&out, TK_FLOAT, p);
                out.d[out.n - 1].fval = strtod(buf, NULL);
                free(buf);
            } else {
                if (overflow) diag_error(p, "integer literal is too large");
                add_tok(&out, TK_INT, p);
                out.d[out.n - 1].ival = v;
            }
            continue;
        }
        if (c == '"') {
            adv(&l);
            char *buf = NULL; int n = 0, cap = 0;
            for (;;) {
                int ch = peekc(&l, 0);
                if (!ch || ch == '\n') { diag_error(p, "unterminated string literal"); break; }
                adv(&l);
                if (ch == '"') break;
                if (ch == '\\') {
                    Pos ep = { file, l.line, l.col - 1 };
                    ch = lex_escape(&l);
                    if (ch < 0) { diag_error(ep, "invalid escape sequence in string"); ch = '?'; }
                }
                if (n + 1 >= cap) { cap = cap ? cap * 2 : 16; buf = xrealloc(buf, (size_t)cap); }
                buf[n++] = (char)ch;
            }
            if (!buf) buf = xcalloc(1); else buf[n] = 0;
            add_tok(&out, TK_STR, p);
            out.d[out.n - 1].s = buf; out.d[out.n - 1].slen = n;
            continue;
        }
        if (c == '\'') {
            adv(&l);
            int ch = peekc(&l, 0);
            if (ch == '\\') { adv(&l); ch = lex_escape(&l); if (ch < 0) { diag_error(p, "invalid escape in character literal"); ch = 0; } }
            else if (ch == '\'' || ch == '\n' || !ch) { diag_error(p, "empty or malformed character literal"); }
            else adv(&l);
            if (peekc(&l, 0) == '\'') adv(&l);
            else diag_error(p, "unterminated character literal (characters are single bytes)");
            add_tok(&out, TK_CHAR, p);
            out.d[out.n - 1].ival = (uint64_t)(ch & 0xff);
            continue;
        }

        // operators
        int c1 = peekc(&l, 1), c2 = peekc(&l, 2);
        TokKind k = TK_EOF; int len = 1;
#define OP2(a, b, kk) if (c == a && c1 == b) { k = kk; len = 2; goto found; }
#define OP3(a, b, d, kk) if (c == a && c1 == b && c2 == d) { k = kk; len = 3; goto found; }
        OP3('<', '<', '=', TK_SHLEQ) OP3('>', '>', '=', TK_SHREQ) OP3('.', '.', '=', TK_DOTDOTEQ)
        OP2(':', '=', TK_DECL) OP2('=', '=', TK_EQ) OP2('!', '=', TK_NE) OP2('<', '=', TK_LE)
        OP2('>', '=', TK_GE) OP2('&', '&', TK_ANDAND) OP2('|', '|', TK_OROR) OP2('<', '<', TK_SHL)
        OP2('>', '>', TK_SHR) OP2('-', '>', TK_ARROW) OP2('.', '.', TK_DOTDOT)
        OP2('+', '=', TK_PLUSEQ) OP2('-', '=', TK_MINUSEQ) OP2('*', '=', TK_STAREQ)
        OP2('/', '=', TK_SLASHEQ) OP2('%', '=', TK_PERCENTEQ) OP2('&', '=', TK_AMPEQ)
        OP2('|', '=', TK_PIPEEQ) OP2('^', '=', TK_CARETEQ)
        switch (c) {
        case '(': k = TK_LPAREN; break; case ')': k = TK_RPAREN; break;
        case '{': k = TK_LBRACE; break; case '}': k = TK_RBRACE; break;
        case '[': k = TK_LBRACK; break; case ']': k = TK_RBRACK; break;
        case ',': k = TK_COMMA; break; case ';': k = TK_SEMI; break; case ':': k = TK_COLON; break;
        case '.': k = TK_DOT; break; case '=': k = TK_ASSIGN; break; case '<': k = TK_LT; break;
        case '>': k = TK_GT; break; case '+': k = TK_PLUS; break; case '-': k = TK_MINUS; break;
        case '*': k = TK_STAR; break; case '/': k = TK_SLASH; break; case '%': k = TK_PERCENT; break;
        case '&': k = TK_AMP; break; case '|': k = TK_PIPE; break; case '^': k = TK_CARET; break;
        case '~': k = TK_TILDE; break; case '!': k = TK_BANG; break;
        default:
            diag_error(p, "unexpected character '%c'", c >= 32 && c < 127 ? c : '?');
            adv(&l);
            continue;
        }
    found:
        for (int i = 0; i < len; i++) adv(&l);
        add_tok(&out, k, p);
    }
    return out;
}
