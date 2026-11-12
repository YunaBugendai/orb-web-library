// Generates a C file embedding the standard library sources.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    printf("#include <string.h>\n#include <stddef.h>\n");
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "embed: cannot open %s\n", argv[i]); return 1; }
        printf("static const unsigned char data%d[] = {", i);
        int c, n = 0;
        while ((c = fgetc(f)) != EOF) { if (n++ % 20 == 0) printf("\n"); printf("%d,", c); }
        printf("0};\n");
        fclose(f);
    }
    printf("static const struct { const char *name; const char *data; } files[] = {\n");
    for (int i = 1; i < argc; i++) {
        const char *p = strrchr(argv[i], '/'); p = p ? p + 1 : argv[i];
        char *nm = strdup(p); char *dot = strrchr(nm, '.'); if (dot) *dot = 0;
        printf("  {\"%s\", (const char *)data%d},\n", nm, i);
    }
    printf("  {NULL, NULL}\n};\n");
    printf("const char *std_source(const char *name) {\n  for (int i = 0; files[i].name; i++) if (!strcmp(files[i].name, name)) return files[i].data;\n  return NULL;\n}\n");
    return 0;
}
