// Orb! compiler - common definitions
#ifndef ORB_COMMON_H
#define ORB_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define ORB_VERSION "0.2.0"

typedef struct { int file, line, col; } Pos;

#define VEC(T) struct { T *d; int n, cap; }
void *xrealloc(void *p, size_t n);
void *xcalloc(size_t n);
#define vpush(v, x) do { __typeof__(*(v).d) vpush_tmp_ = (x); \
    if ((v).n == (v).cap) { (v).cap = (v).cap ? (v).cap * 2 : 8; \
    (v).d = xrealloc((v).d, (size_t)(v).cap * sizeof(*(v).d)); } (v).d[(v).n++] = vpush_tmp_; } while (0)

char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xsprintf(const char *fmt, ...);

// string-keyed hash map (keys are not copied)
typedef struct { const char **keys; void **vals; int cap, n; } Map;
void *map_get(Map *m, const char *k);
void map_put(Map *m, const char *k, void *v);

// sources & diagnostics
int add_source(const char *path, char *text);
const char *source_path(int file);
char *read_file_text(const char *path);
extern int g_errors;
extern bool g_color;
void diag_error(Pos p, const char *fmt, ...);
void diag_note(Pos p, const char *fmt, ...);
void die(const char *fmt, ...);
int edit_distance(const char *a, const char *b);

#endif
