#pragma once
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "posix_compat.h"

struct dirent {
    char d_name[260];
    unsigned char d_type;
};

typedef struct {
    struct _finddata_t ff;
    intptr_t handle;
    int first;
    struct dirent cur;
    char pattern[280];
} DIR;

static inline DIR *opendir(const char *path) {
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    snprintf(d->pattern, sizeof(d->pattern), "%s\\*", path);
    d->handle = _findfirst(d->pattern, &d->ff);
    d->first = 1;
    if (d->handle == -1) {
        free(d);
        return NULL;
    }
    return d;
}

static inline struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (!d->first) {
        if (_findnext(d->handle, &d->ff) != 0) return NULL;
    }
    d->first = 0;
    snprintf(d->cur.d_name, sizeof(d->cur.d_name), "%s", d->ff.name);
    d->cur.d_type = (d->ff.attrib & _A_SUBDIR) ? DT_DIR : DT_REG;
    return &d->cur;
}

static inline int closedir(DIR *d) {
    if (!d) return -1;
    if (d->handle != -1) _findclose(d->handle);
    free(d);
    return 0;
}
#else
#include_next <dirent.h>
#endif
