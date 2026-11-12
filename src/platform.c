#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>

static char *quote_arg(const char *a) {
    if (*a && !strpbrk(a, " \t\"")) return xstrdup(a);
    size_t n = strlen(a);
    char *r = xcalloc(n * 2 + 3), *w = r;
    *w++ = '"';
    for (const char *p = a; *p; p++) { if (*p == '"') *w++ = '\\'; *w++ = *p; }
    *w++ = '"';
    return r;
}
int plat_run(char **argv, bool quiet) {
    (void)quiet;
    int n = 0;
    while (argv[n]) n++;
    char **q = xcalloc(sizeof(char *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) q[i] = quote_arg(argv[i]);
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)q);
    if (rc == -1) return 127;
    return (int)rc;
}
char *plat_mkdtemp(void) {
    const char *base = getenv("TEMP");
    if (!base) base = getenv("TMP");
    if (!base) base = ".";
    static int counter;
    char *p = xsprintf("%s\\orb%d_%d", base, (int)_getpid(), counter++);
    if (_mkdir(p) != 0) return NULL;
    return p;
}
void plat_cleanup(const char *dir, const char **files) {
    for (int i = 0; files && files[i]; i++) remove(plat_join(dir, files[i]));
    _rmdir(dir);
}
bool plat_stderr_tty(void) { return _isatty(2) != 0; }
int plat_mkdir(const char *path) { return _mkdir(path); }
char *plat_join(const char *dir, const char *name) { return xsprintf("%s\\%s", dir, name); }

#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

int plat_run(char **argv, bool quiet) {
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (quiet) {
            if (!freopen("/dev/null", "w", stdout)) _exit(127);
            if (!freopen("/dev/null", "w", stderr)) _exit(127);
        }
        execvp(argv[0], argv);
        if (!quiet) fprintf(stderr, "orb: cannot execute '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}
char *plat_mkdtemp(void) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    char *tmpl = xsprintf("%s/orbXXXXXX", base);
    return mkdtemp(tmpl);
}
void plat_cleanup(const char *dir, const char **files) {
    for (int i = 0; files && files[i]; i++) unlink(plat_join(dir, files[i]));
    rmdir(dir);
}
bool plat_stderr_tty(void) { return isatty(2); }
int plat_mkdir(const char *path) { return mkdir(path, 0755); }
char *plat_join(const char *dir, const char *name) { return xsprintf("%s/%s", dir, name); }
#endif
