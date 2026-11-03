#pragma once

#include "hal_log.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_VFS_FLAG_DEFAULT 0

typedef struct {
    int flags;
    int (*open)(const char *path, int flags, int mode);
    ssize_t (*write)(int fd, const void *data, size_t size);
    ssize_t (*read)(int fd, void *dst, size_t size);
    off_t (*lseek)(int fd, off_t off, int whence);
    int (*close)(int fd);
    int (*fstat)(int fd, struct stat *st);
    int (*fsync)(int fd);
} esp_vfs_t;

esp_err_t esp_vfs_register(const char *base, const esp_vfs_t *vfs, void *ctx);

#ifdef __cplusplus
}
#endif
