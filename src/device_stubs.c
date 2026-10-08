/* Minimal newlib syscalls for the device build.  uMac/Musashi print
 * diagnostics via stdio; send those to the Playdate console and stub
 * the rest out.
 */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "pd_api.h"

extern PlaydateAPI *marble_pd;

int _write(int fd, const char *buf, int len)
{
        static char line[128];
        static int n;
        (void)fd;
        for (int i = 0; i < len; i++) {
                char c = buf[i];
                if (c != '\n' && n < (int)sizeof line - 1)
                        line[n++] = c;
                if (c == '\n' || n == (int)sizeof line - 1) {
                        line[n] = 0;
                        if (marble_pd)
                                marble_pd->system->logToConsole("%s", line);
                        n = 0;
                }
        }
        return len;
}

int _read(int fd, char *buf, int len)  { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                     { (void)fd; return -1; }
int _lseek(int fd, int off, int w)     { (void)fd; (void)off; (void)w; return 0; }
int _fstat(int fd, struct stat *st)    { (void)fd; memset(st, 0, sizeof *st); st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)                    { (void)fd; return 1; }
int _getpid(void)                      { return 1; }
int _kill(int pid, int sig)            { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status)                 { (void)status; for (;;) ; }
void _fini(void)                       { }

/* Referenced by libgcc's unwinder, which never actually runs here */
const char __exidx_start[0];
const char __exidx_end[0];
