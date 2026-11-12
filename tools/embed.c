// Generates a C translation unit embedding Orb!'s standard library and browser runtime.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int emit_bytes(FILE *f, const char *symbol) {
    printf("static const unsigned char %s[] = {", symbol);
    int c, n = 0;
    while ((c = fgetc(f)) != EOF) {
        if (n++ % 20 == 0) printf("\\n");
        printf("%u,", (unsigned char)c);
    }
    printf("0};\\n");
    return ferror(f) ? 1 : 0;
}

static const char *base_name(const char *path) {
    const char *p = path, *b = path;
    for (; *p; p++) if (*p == '/' || *p == '\\\\') b = p + 1;
    return b;
}

int main(int argc, char **argv) {
    const char *runtime_path = NULL;
    const char **files = calloc((size_t)argc, sizeof(char *));
    int count = 0;
    if (!files) return 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--runtime")) {
            if (++i >= argc || runtime_path) {
                fprintf(stderr, "embed: --runtime requires exactly one file\\n");
                return 1;
            }
            runtime_path = argv[i];
        } else {
            files[count++] = argv[i];
        }
    }
    if (!runtime_path) {
        fprintf(stderr, "embed: missing --runtime web/orb.js\\n");
        return 1;
    }

    printf("#include <string.h>\\n#include <stddef.h>\\n");
    for (int i = 0; i < count; i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f) { fprintf(stderr, "embed: cannot open %s\\n", files[i]); return 1; }
        char symbol[64];
        snprintf(symbol, sizeof(symbol), "std_data_%d", i);
        int rc = emit_bytes(f, symbol);
        fclose(f);
        if (rc) { fprintf(stderr, "embed: failed reading %s\\n", files[i]); return 1; }
    }

    FILE *runtime = fopen(runtime_path, "rb");
    if (!runtime) { fprintf(stderr, "embed: cannot open runtime %s\\n", runtime_path); return 1; }
    int runtime_rc = emit_bytes(runtime, "orb_web_runtime_data");
    fclose(runtime);
    if (runtime_rc) { fprintf(stderr, "embed: failed reading runtime %s\\n", runtime_path); return 1; }

    printf("static const struct { const char *name; const char *data; } files[] = {\\n");
    for (int i = 0; i < count; i++) {
        const char *p = base_name(files[i]);
        char name[256];
        size_t n = strlen(p);
        if (n >= sizeof(name)) return 1;
        memcpy(name, p, n + 1);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        printf("  {\\\"%s\\\", (const char *)std_data_%d},\\n", name, i);
    }
    printf("  {NULL, NULL}\\n};\\n");
    printf("const char *std_source(const char *name) {\\n"
           "  for (int i = 0; files[i].name; i++) if (!strcmp(files[i].name, name)) return files[i].data;\\n"
           "  return NULL;\\n"
           "}\\n");
    printf("const char *web_runtime_source(void) { return (const char *)orb_web_runtime_data; }\\n");
    free(files);
    return 0;
}
