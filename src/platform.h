// Orb! compiler - operating system services used by the driver (POSIX and Win32).
#ifndef ORB_PLATFORM_H
#define ORB_PLATFORM_H
#include "common.h"

int plat_run(char **argv, bool quiet);                  // runs a program and waits; exit status (127 = cannot start)
char *plat_mkdtemp(void);                               // creates a private temporary directory
void plat_cleanup(const char *dir, const char **files); // removes the listed files, then the directory
bool plat_stderr_tty(void);
int plat_mkdir(const char *path);
char *plat_join(const char *dir, const char *name);
#endif
