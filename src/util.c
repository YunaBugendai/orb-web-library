#include "common.h"

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "orb: out of memory\n"); exit(2); }
    return p;
}
void *xcalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "orb: out of memory\n"); exit(2); }
    return p;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) {
    char *r = xcalloc(n + 1);
    memcpy(r, s, n);
    return r;
}
char *xsprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *r = xcalloc((size_t)n + 1);
    vsnprintf(r, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return r;
}

// ---- hash map -------------------------------------------------------------
static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}
static void map_grow(Map *m) {
    int ncap = m->cap ? m->cap * 2 : 32;
    const char **nk = xcalloc((size_t)ncap * sizeof(char *));
    void **nv = xcalloc((size_t)ncap * sizeof(void *));
    for (int i = 0; i < m->cap; i++) if (m->keys[i]) {
        uint32_t h = hash_str(m->keys[i]) & (uint32_t)(ncap - 1);
        while (nk[h]) h = (h + 1) & (uint32_t)(ncap - 1);
        nk[h] = m->keys[i]; nv[h] = m->vals[i];
    }
    free(m->keys); free(m->vals);
    m->keys = nk; m->vals = nv; m->cap = ncap;
}
void *map_get(Map *m, const char *k) {
    if (!m->cap) return NULL;
    uint32_t h = hash_str(k) & (uint32_t)(m->cap - 1);
    while (m->keys[h]) {
        if (!strcmp(m->keys[h], k)) return m->vals[h];
        h = (h + 1) & (uint32_t)(m->cap - 1);
    }
    return NULL;
}
void map_put(Map *m, const char *k, void *v) {
    if ((m->n + 1) * 2 > m->cap) map_grow(m);
    uint32_t h = hash_str(k) & (uint32_t)(m->cap - 1);
    while (m->keys[h]) {
        if (!strcmp(m->keys[h], k)) { m->vals[h] = v; return; }
        h = (h + 1) & (uint32_t)(m->cap - 1);
    }
    m->keys[h] = k; m->vals[h] = v; m->n++;
}

// ---- sources & diagnostics ---------------------------------------------------
typedef struct { char *path; char *text; } Source;
static VEC(Source) g_sources;
int g_errors = 0;
bool g_color = false;

int add_source(const char *path, char *text) {
    Source s = { xstrdup(path), text };
    vpush(g_sources, s);
    return g_sources.n - 1;
}
const char *source_path(int file) {
    return (file >= 0 && file < g_sources.n) ? g_sources.d[file].path : "<unknown>";
}
char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xcalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    return buf;
}

static void show_line(Pos p, const char *colorcode) {
    if (p.file < 0 || p.file >= g_sources.n || p.line <= 0) return;
    const char *t = g_sources.d[p.file].text;
    int line = 1;
    while (*t && line < p.line) { if (*t == '\n') line++; t++; }
    const char *e = t;
    while (*e && *e != '\n') e++;
    fprintf(stderr, " %4d | %.*s\n", p.line, (int)(e - t), t);
    fprintf(stderr, "      | ");
    for (int i = 1; i < p.col && i - 1 < (int)(e - t); i++) fputc(t[i - 1] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "%s^%s\n", g_color ? colorcode : "", g_color ? "\033[0m" : "");
}
static void vdiag(Pos p, const char *kind, const char *col, const char *fmt, va_list ap) {
    if (g_color) fprintf(stderr, "\033[1m%s:%d:%d: %s%s:\033[0m\033[1m ", source_path(p.file), p.line, p.col, col, kind);
    else fprintf(stderr, "%s:%d:%d: %s: ", source_path(p.file), p.line, p.col, kind);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s\n", g_color ? "\033[0m" : "");
    show_line(p, col);
}
void diag_error(Pos p, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vdiag(p, "error", "\033[31m", fmt, ap);
    va_end(ap);
    g_errors++;
    if (g_errors >= 30) { fprintf(stderr, "orb: too many errors, stopping\n"); exit(1); }
}
void diag_note(Pos p, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vdiag(p, "note", "\033[36m", fmt, ap);
    va_end(ap);
}
void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "orb: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = a[i - 1] == b[j - 1] ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}
