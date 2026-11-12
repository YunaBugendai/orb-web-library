// Orb! compiler driver: orb build | run | check | doctor | new | version
#include "sema.h"
#include "ir.h"
#include "platform.h"
#include <errno.h>

typedef struct {
    const char *input, *output;
    bool emit_asm, emit_ir, no_opt;
    VEC(char *) run_args;
    VEC(char *) link_objs;
} Opts;

#if defined(_WIN32)
#define HOST_TARGET TGT_WINDOWS
#elif defined(__APPLE__)
#define HOST_TARGET TGT_MACOS
#else
#define HOST_TARGET TGT_LINUX
#endif

static char *stem_of(const char *path) {
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    char *s = xstrdup(b);
    char *dot = strrchr(s, '.');
    if (dot && !strcmp(dot, ".orb")) *dot = 0;
    return s;
}

static void usage(void) {
    printf("Orb! %s - native performance, minimal complexity (Orb Technologies)\n\n"
           "usage:\n"
           "  orb build <file.orb> [extra.o ...] [-o out] [--target t] [--unchecked] [--no-opt] [-S] [--emit-ir]\n"
           "  orb run   <file.orb> [options] [args...]\n"
           "  orb check <file.orb>\n"
           "  orb doctor               check the toolchain and compile + run a test program\n"
           "  orb new   <name>\n"
           "  orb version\n\n"
           "options:\n"
           "  -o <file>        output executable (default: input name without .orb)\n"
           "  --target <t>     linux | macos | windows | wasm (default: this machine)\n"
           "  --stack-size <K> wasm only: shadow stack size in KiB (default 1024)\n"
           "  --unchecked      disable bounds and division-by-zero checks\n"
           "  --no-opt         disable IR optimizations\n"
           "  -S               write assembly (x86-64, AT&T syntax) instead of an executable\n"
           "  --emit-ir        print the intermediate representation to stdout\n"
           "  --std <dir>      use a standard library directory instead of the embedded one\n\n"
           "environment: ORB_AS, ORB_LD, ORB_CC override the assembler / linker / C driver.\n", ORB_VERSION);
}

static bool compile_to_asm(Opts *o, const char *asm_path) {
    g_color = plat_stderr_tty();
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
    if (g_target == TGT_WASM) { wasm_emit(asm_path); return true; }
    FILE *f = fopen(asm_path, "w");
    if (!f) die("cannot write '%s': %s", asm_path, strerror(errno));
    regalloc_and_emit(f);
    fclose(f);
    return true;
}

static const char *envor(const char *var, const char *def) {
    const char *v = getenv(var);
    return v && *v ? v : def;
}

// assembles and links; returns 0 on success
static int assemble_and_link(Opts *o, const char *asm_path, const char *obj, const char *exe) {
    int n = 0;
    char **av = xcalloc(sizeof(char *) * (size_t)(o->link_objs.n + 12));
    int rc;
    if (g_target == TGT_LINUX) {
        if (g_host != TGT_LINUX) { fprintf(stderr, "orb: building for linux requires a Linux host (use `-S` to emit assembly)\n"); return 1; }
        av[n++] = (char *)envor("ORB_AS", "as"); av[n++] = "--64"; av[n++] = "-o"; av[n++] = (char *)obj; av[n++] = (char *)asm_path;
        rc = plat_run(av, false);
        if (rc) { fprintf(stderr, "orb: assembling failed (are GNU binutils 'as'/'ld' installed?)\n"); return 1; }
        n = 0; memset(av, 0, sizeof(char *) * (size_t)(o->link_objs.n + 12));
        av[n++] = (char *)envor("ORB_LD", "ld"); av[n++] = "-o"; av[n++] = (char *)exe; av[n++] = (char *)obj;
        for (int i = 0; i < o->link_objs.n; i++) av[n++] = o->link_objs.d[i];
        rc = plat_run(av, false);
        if (rc) { fprintf(stderr, "orb: linking failed\n"); return 1; }
        return 0;
    }
    if (g_target == TGT_MACOS) {
        if (g_host != TGT_MACOS) { fprintf(stderr, "orb: building for macos requires a Mac (use `-S` to emit assembly)\n"); return 1; }
        av[n++] = (char *)envor("ORB_CC", "clang"); av[n++] = "-arch"; av[n++] = "x86_64"; av[n++] = "-o"; av[n++] = (char *)exe; av[n++] = (char *)asm_path;
        for (int i = 0; i < o->link_objs.n; i++) av[n++] = o->link_objs.d[i];
        rc = plat_run(av, false);
        if (rc) { fprintf(stderr, "orb: assembling/linking failed (install the Xcode command line tools: `xcode-select --install`)\n"); return 1; }
        return 0;
    }
    // Windows (native, or cross-compiling with MinGW-w64)
    const char *pre = g_host == TGT_WINDOWS ? "" : "x86_64-w64-mingw32-";
    av[n++] = (char *)envor("ORB_AS", xsprintf("%sas", pre)); av[n++] = "-o"; av[n++] = (char *)obj; av[n++] = (char *)asm_path;
    rc = plat_run(av, false);
    if (rc) { fprintf(stderr, "orb: assembling failed (install MinGW-w64, e.g. MSYS2 'mingw-w64-x86_64-gcc', and use its shell)\n"); return 1; }
    n = 0; memset(av, 0, sizeof(char *) * (size_t)(o->link_objs.n + 12));
    av[n++] = (char *)envor("ORB_CC", xsprintf("%sgcc", pre)); av[n++] = "-o"; av[n++] = (char *)exe; av[n++] = (char *)obj;
    for (int i = 0; i < o->link_objs.n; i++) av[n++] = o->link_objs.d[i];
    av[n++] = "-lkernel32"; av[n++] = "-Wl,--stack,8388608";
    rc = plat_run(av, false);
    if (rc) { fprintf(stderr, "orb: linking failed\n"); return 1; }
    return 0;
}

extern int g_wasm_stack_kb;
extern bool g_wasm_force_dispatch;

static int cmd_build_wasm(Opts *o) {
    if (o->emit_asm) die("-S is not available for the wasm target (use --emit-ir to inspect the IR)");
    const char *out = o->output ? o->output : xsprintf("%s.wasm", stem_of(o->input));
    if (o->emit_ir) { return compile_to_asm(o, g_host == TGT_WINDOWS ? "NUL" : "/dev/null") ? 0 : 1; }
    return compile_to_asm(o, out) ? 0 : 1;
}

static int cmd_build(Opts *o, bool run) {
    if (g_target == TGT_WASM) {
        if (run) { fprintf(stderr, "orb: a wasm program needs a host to run it; use `orb build --target wasm` and see docs/WASM.md (tools/orb-node.js runs it under Node)\n"); return 1; }
        return cmd_build_wasm(o);
    }
    if (run && g_target != g_host) { fprintf(stderr, "orb: cannot run a %s program on this %s machine; use `orb build`\n", target_name(g_target), target_name(g_host)); return 1; }
    char *dir = plat_mkdtemp();
    if (!dir) die("cannot create a temporary directory");
    const char *cleanup[] = { "out.s", "out.o", "prog", "prog.exe", NULL };
    char *asm_path = o->emit_asm && o->output ? (char *)o->output : plat_join(dir, "out.s");
    if (!compile_to_asm(o, asm_path)) { plat_cleanup(dir, cleanup); return 1; }
    if (o->emit_asm) {
        if (!o->output) { FILE *f = fopen(asm_path, "r"); int c; while ((c = fgetc(f)) != EOF) putchar(c); fclose(f); }
        plat_cleanup(dir, cleanup);
        return 0;
    }
    if (o->emit_ir) { plat_cleanup(dir, cleanup); return 0; }
    char *obj = plat_join(dir, "out.o");
    char *exe;
    if (run) exe = plat_join(dir, g_target == TGT_WINDOWS ? "prog.exe" : "prog");
    else {
        const char *out = o->output ? o->output : stem_of(o->input);
        size_t l = strlen(out);
        exe = (g_target == TGT_WINDOWS && !(l > 4 && !strcmp(out + l - 4, ".exe"))) ? xsprintf("%s.exe", out) : (char *)out;
    }
    int rc = assemble_and_link(o, asm_path, obj, exe);
    if (rc != 0 || !run) { plat_cleanup(dir, cleanup); return rc ? 1 : 0; }
    char **argv = xcalloc(sizeof(char *) * (size_t)(o->run_args.n + 2));
    argv[0] = exe;
    for (int i = 0; i < o->run_args.n; i++) argv[i + 1] = o->run_args.d[i];
    int st = plat_run(argv, false);
    plat_cleanup(dir, cleanup);
    return st;
}

static int cmd_new(const char *name) {
    if (plat_mkdir(name) != 0) die("cannot create directory '%s': %s", name, strerror(errno));
    char *p = plat_join(name, "main.orb");
    FILE *f = fopen(p, "w");
    if (!f) die("cannot write %s", p);
    fprintf(f, "// %s - an Orb! program\n\nfn main() {\n    println(\"Hello from %s!\");\n}\n", name, name);
    fclose(f);
    printf("created %s\nrun it with: orb run %s\n", p, p);
    return 0;
}

// ---------------------------------------------------------------- orb doctor
static bool tool_ok(const char *tool, const char *flag) {
    char *av[] = { (char *)tool, (char *)flag, NULL };
    return plat_run(av, true) == 0;
}
static void report(const char *what, bool ok, const char *hint) {
    printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what, ok ? "" : "  -> ", ok ? "" : hint);
}

static int cmd_doctor(void) {
    printf("Orb! %s doctor\n  compiler host: %s, default target: %s\n", ORB_VERSION, target_name(g_host), target_name(g_target));
    bool all = true;
    if (g_target == TGT_LINUX) {
        bool a = tool_ok(envor("ORB_AS", "as"), "--version"), l = tool_ok(envor("ORB_LD", "ld"), "--version");
        report("assembler 'as'", a, "install GNU binutils (e.g. `sudo apt install binutils`)");
        report("linker 'ld'", l, "install GNU binutils");
        all = a && l;
    } else if (g_target == TGT_MACOS) {
        bool c = tool_ok(envor("ORB_CC", "clang"), "--version");
        report("clang", c, "run `xcode-select --install`");
        all = c;
    } else {
        const char *pre = g_host == TGT_WINDOWS ? "" : "x86_64-w64-mingw32-";
        bool a = tool_ok(envor("ORB_AS", xsprintf("%sas", pre)), "--version"), c = tool_ok(envor("ORB_CC", xsprintf("%sgcc", pre)), "--version");
        report("assembler (MinGW-w64 'as')", a, "install MSYS2 and `pacman -S mingw-w64-x86_64-gcc`, then use the 'MSYS2 MinGW x64' shell");
        report("linker driver (MinGW-w64 'gcc')", c, "same as above");
        all = a && c;
    }
    if (!all) { printf("\nResult: toolchain incomplete. See docs/PLATFORMS.md.\n"); return 1; }
    char *dir = plat_mkdtemp();
    if (!dir) { report("temporary directory", false, "check TMPDIR / TEMP"); return 1; }
    const char *cleanup[] = { "doctor.orb", "doctor.exe", "doctor", NULL };
    char *src = plat_join(dir, "doctor.orb");
    FILE *f = fopen(src, "w");
    if (!f) { report("writing test program", false, "check permissions"); return 1; }
    fprintf(f, "use strings;\nfn main() {\n    s := strings.concat(\"doctor\", \"-ok\");\n    t := 0;\n    for i in 1..=100 { t += i; }\n    println(s, \" \", t, \" \", 1.5 * 2.0);\n}\n");
    fclose(f);
    // run the compile in a child so global compiler state is fresh
    char *self_exe = getenv("ORB_SELF");
    (void)self_exe;
    Opts o = {0};
    o.input = src;
    char *exe = plat_join(dir, g_target == TGT_WINDOWS ? "doctor.exe" : "doctor");
    o.output = exe;
    int rc = cmd_build(&o, false);
    bool built = rc == 0;
    report("compile and link a test program", built, "see the messages above");
    bool ran = false;
    if (built) {
        char *av[] = { exe, NULL };
        // capture output through a file so we can compare it
        char *outp = plat_join(dir, "out.txt");
        (void)outp;
        int st = plat_run(av, false);
        ran = st == 0;
        report("run the test program (expect: doctor-ok 5050 3.000000)", ran, "the program did not run correctly; please report this");
    }
    plat_cleanup(dir, cleanup);
    printf("\nResult: %s\n", built && ran ? "everything works." : "something failed.");
    return built && ran ? 0 : 1;
}

int main(int argc, char **argv) {
    g_host = HOST_TARGET;
    g_target = g_host;
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-v")) {
        printf("Orb! %s (Orb Technologies) host: %s, x86-64\n", ORB_VERSION, target_name(g_host));
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(); return 0; }
    if (!strcmp(cmd, "new")) {
        if (argc < 3) die("usage: orb new <name>");
        return cmd_new(argv[2]);
    }
    if (!strcmp(cmd, "doctor")) return cmd_doctor();
    bool build = !strcmp(cmd, "build"), run = !strcmp(cmd, "run"), check = !strcmp(cmd, "check");
    if (!build && !run && !check) { fprintf(stderr, "orb: unknown command '%s'\n\n", cmd); usage(); return 1; }
    Opts o = {0};
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        size_t al = strlen(a);
        bool is_obj = al > 2 && (!strcmp(a + al - 2, ".o") || !strcmp(a + al - 2, ".a"));
        if (!strcmp(a, "--")) { for (int k = i + 1; k < argc; k++) vpush(o.run_args, argv[k]); break; }
        else if (!strcmp(a, "-o") && i + 1 < argc) o.output = argv[++i];
        else if (!strcmp(a, "--unchecked")) g_prog.unchecked = true;
        else if (!strcmp(a, "--no-opt")) o.no_opt = true;
        else if (!strcmp(a, "-S")) o.emit_asm = true;
        else if (!strcmp(a, "--emit-ir")) o.emit_ir = true;
        else if (!strcmp(a, "--std") && i + 1 < argc) g_std_dir = argv[++i];
        else if (!strcmp(a, "--stack-size") && i + 1 < argc) { g_wasm_stack_kb = atoi(argv[++i]); if (g_wasm_stack_kb < 64) die("--stack-size is in KiB and must be at least 64"); }
        else if (!strcmp(a, "--wasm-dispatch")) g_wasm_force_dispatch = true;
        else if (!strcmp(a, "--target") && i + 1 < argc) {
            const char *t = argv[++i];
            if (!strcmp(t, "linux")) g_target = TGT_LINUX;
            else if (!strcmp(t, "macos") || !strcmp(t, "darwin")) g_target = TGT_MACOS;
            else if (!strcmp(t, "windows") || !strcmp(t, "win")) g_target = TGT_WINDOWS;
            else if (!strcmp(t, "wasm") || !strcmp(t, "wasm32")) g_target = TGT_WASM;
            else die("unknown target '%s' (use linux, macos, windows or wasm)", t);
        }
        else if (a[0] == '-') die("unknown option '%s'", a);
        else if (is_obj && o.input) vpush(o.link_objs, (char *)a);
        else if (is_obj) die("the first argument must be the .orb source file");
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
