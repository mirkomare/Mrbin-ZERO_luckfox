#pragma once
/* Forced include on WIN32 HOST_SIM so CORE sources that use POSIX compile. */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef access
#define access _access
#endif
#ifndef open
#define open _open
#endif
#ifndef mkdir
#define mkdir(p, m) _mkdir(p)
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef rmdir
#define rmdir _rmdir
#endif
#ifndef close
#define close _close
#endif
#ifndef read
#define read _read
#endif
#ifndef write
#define write _write
#endif
#ifndef lseek
#define lseek _lseek
#endif
#ifndef getpid
#define getpid _getpid
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef DT_REG
#define DT_REG 8
#define DT_DIR 4
#define DT_UNKNOWN 0
#endif

#ifndef S_IRWXU
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif

static inline struct tm *localtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) return NULL;
    return localtime_s(out, t) == 0 ? out : NULL;
}

static inline struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) return NULL;
    return gmtime_s(out, t) == 0 ? out : NULL;
}

static inline int ioctl(int, unsigned long, ...) { return -1; }
static inline int fsync(int) { return 0; }
static inline int ftruncate(int, long) { return -1; }
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static inline unsigned int sleep(unsigned int s) {
    Sleep(s * 1000);
    return 0;
}
static inline int usleep(unsigned int us) {
    Sleep((us + 999) / 1000);
    return 0;
}

static inline int setenv(const char *name, const char *val, int) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=%s", name, val ? val : "");
    return _putenv(buf);
}

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#endif /* _WIN32 */
