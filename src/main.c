// Orb! compiler driver: orb build | run | check | version | new
#include "sema.h"
#include "ir.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

typedef struct {
    const char *input, *output;
    bool emit_asm, emit_ir, no_opt, keep;
    VEC(char *) run_args;
    VEC(char *) link_objs;
} Opts;

static int run_cmd(char **argv, bool quiet) {
    pid_t pid = fork();
    if (pid < 0) die("fork failed");
    if (pid == 0) {
        if (quiet) { if (!freopen("/dev/null", "w", stdout)) _exit(127); }
        execvp(argv[0], argv);
        fprintf(stderr, "orb: cannot execute '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

static char *stem_of(const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char *s = xstrdup(b);
    char *dot = strrchr(s, '.');
    if (dot && !strcmp(dot, ".orb")) *dot = 0;
    return s;
}

static void usage(void) {
    printf("Orb! %s - native performance, minimal complexity (Orb Technologies)\n\n"
           "usage:\n"
           "  orb build <file.orb> [extra.o ...] [-o out] [--unchecked] [--no-opt] [-S] [--emit-ir]\n"
           "  orb run   <file.orb> [options] [-- args...]\n"
           "  orb check <file.orb>\n"
           "  orb new   <name>\n"
           "  orb version\n\n"
           "options:\n"
           "  -o <file>      output executable (default: input name without .orb)\n"
           "  --unchecked    disable bounds and division-by-zero checks\n"
           "  --no-opt       disable IR optimizations\n"
           "  -S             write assembly (x86-64, AT&T syntax) instead of an executable\n"
           "  --emit-ir      print the intermediate representation to stdout\n"
           "  --std <dir>    use a standard library directory instead of the embedded one\n", ORB_VERSION);
}

// returns true on success; frontend + backend up to assembly text
static bool compile_to_asm(Opts *o, const char *asm_path) {
    g_color = isatty(2);
    sema_init();
    if (getenv("ORB_STD")) g_std_dir = getenv("ORB_STD");
    Module *core = load_std_module("core");
    if (!core) die("internal error: the embedded standard library is missing 'core'");
    char *stem = stem_of(o->input);
    Module *mm = load_module_file(o->input, stem, true);
    if (!mm) die("cannot read '%s': %s", o->input, strerror(errno));
    if (g_errors) { fprintf(stderr, "orb: %d error%s\n", g_errors, g_errors == 1 ? "" : "s"); return false; }
    sema_run(mm);
    if (g_errors) { fprintf(stderr, "orb: %d error%s\n", g_errors, g_errors == 1 ? "" : "s"); return false; }
    if (!asm_path) return true;
    lower_program();
    if (!o->no_opt) for (int i = 0; i < g_irs.n; i++) opt_function(g_irs.d[i]);
    if (o->emit_ir) for (int i = 0; i < g_irs.n; i++) { ir_dump(stdout, g_irs.d[i]); printf("\n"); }
    FILE *f = fopen(asm_path, "w");
    if (!f) die("cannot write '%s': %s", asm_path, strerror(errno));
    regalloc_and_emit(f);
    fclose(f);
    return true;
}

static int cmd_build(Opts *o, bool run) {
    char tmpl[] = "/tmp/orbXXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) die("cannot create a temporary directory");
    char *asm_path = o->emit_asm && o->output ? (char *)o->output : xsprintf("%s/out.s", dir);
    if (!compile_to_asm(o, asm_path)) { if (!o->emit_asm) { run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true); } return 1; }
    if (o->emit_asm) {
        if (!o->output) { FILE *f = fopen(asm_path, "r"); int c; while ((c = fgetc(f)) != EOF) putchar(c); fclose(f); }
        run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true);
        return 0;
    }
    if (o->emit_ir) { run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true); return 0; }
    char *obj = xsprintf("%s/out.o", dir);
    char *exe = run ? xsprintf("%s/prog", dir) : (char *)(o->output ? o->output : stem_of(o->input));
    int rc = run_cmd((char *[]){ "as", "--64", "-o", obj, asm_path, NULL }, false);
    if (rc != 0) { fprintf(stderr, "orb: assembling failed (is GNU binutils 'as' installed?)\n"); if (!o->keep) run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true); return 1; }
    char **ldargv = xcalloc(sizeof(char *) * (size_t)(o->link_objs.n + 5));
    int lk = 0;
    ldargv[lk++] = "ld"; ldargv[lk++] = "-o"; ldargv[lk++] = exe; ldargv[lk++] = obj;
    for (int i = 0; i < o->link_objs.n; i++) ldargv[lk++] = o->link_objs.d[i];
    rc = run_cmd(ldargv, false);
    if (rc != 0) { fprintf(stderr, "orb: linking failed (is GNU binutils 'ld' installed?)\n"); run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true); return 1; }
    if (!run) { run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true); return 0; }
    char **argv = xcalloc(sizeof(char *) * (size_t)(o->run_args.n + 2));
    argv[0] = exe;
    for (int i = 0; i < o->run_args.n; i++) argv[i + 1] = o->run_args.d[i];
    fflush(stdout);
    int st = run_cmd(argv, false);
    run_cmd((char *[]){ "rm", "-rf", dir, NULL }, true);
    return st;
}

static int cmd_new(const char *name) {
    if (mkdir(name, 0755) != 0) die("cannot create directory '%s': %s", name, strerror(errno));
    char *p = xsprintf("%s/main.orb", name);
    FILE *f = fopen(p, "w");
    if (!f) die("cannot write %s", p);
    fprintf(f, "// %s - an Orb! program\n\nfn main() {\n    println(\"Hello from %s!\");\n}\n", name, name);
    fclose(f);
    printf("created %s/main.orb\nrun it with: orb run %s/main.orb\n", name, name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-v")) {
        printf("Orb! %s (Orb Technologies) x86-64 Linux\n", ORB_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(); return 0; }
    if (!strcmp(cmd, "new")) {
        if (argc < 3) die("usage: orb new <name>");
        return cmd_new(argv[2]);
    }
    bool build = !strcmp(cmd, "build"), run = !strcmp(cmd, "run"), check = !strcmp(cmd, "check");
    if (!build && !run && !check) { fprintf(stderr, "orb: unknown command '%s'\n\n", cmd); usage(); return 1; }
    Opts o = {0};
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { for (int k = i + 1; k < argc; k++) vpush(o.run_args, argv[k]); break; }
        else if (!strcmp(a, "-o") && i + 1 < argc) o.output = argv[++i];
        else if (!strcmp(a, "--unchecked")) g_prog.unchecked = true;
        else if (!strcmp(a, "--no-opt")) o.no_opt = true;
        else if (!strcmp(a, "-S")) o.emit_asm = true;
        else if (!strcmp(a, "--emit-ir")) o.emit_ir = true;
        else if (!strcmp(a, "--std") && i + 1 < argc) g_std_dir = argv[++i];
        else if (a[0] == '-') die("unknown option '%s'", a);
        else if (!o.input && strlen(a) > 2 && (!strcmp(a + strlen(a) - 2, ".o") || !strcmp(a + strlen(a) - 2, ".a"))) die("the first argument must be the .orb source file");
        else if (o.input && strlen(a) > 2 && (!strcmp(a + strlen(a) - 2, ".o") || !strcmp(a + strlen(a) - 2, ".a"))) vpush(o.link_objs, (char *)a);
        else if (!o.input) o.input = a;
        else if (run) vpush(o.run_args, (char *)a);
        else die("unexpected argument '%s'", a);
    }
    if (!o.input) die("missing input file (usage: orb %s <file.orb>)", cmd);
    if (check) {
        if (!compile_to_asm(&o, NULL)) return 1;
        printf("%s: ok\n", o.input);
        return 0;
    }
    return cmd_build(&o, run);
}
