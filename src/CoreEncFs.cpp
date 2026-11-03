#include "CoreEncFs.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "core_encfs";

#define ENCFS_HDR_BYTES   20   // MRBI (4) + IV (16)
#define ENCFS_MAX_FILES    3
#define ENCFS_CHUNK      4096

typedef struct {
    bool               used;
    int                real_fd;
    uint64_t           logical_pos;   // posizione nel flusso in chiaro (0 = primo byte MP4)
    core_crypto_ctx_t  crypto;
} enc_file_t;

static enc_file_t       s_files[ENCFS_MAX_FILES];
static SemaphoreHandle_t s_mtx;
static uint8_t          s_key[16];
static bool             s_key_set;
static bool             s_registered;

static enc_file_t *slot_from_fd(int fd) {
    if (fd < 0 || fd >= ENCFS_MAX_FILES || !s_files[fd].used) {
        return nullptr;
    }
    return &s_files[fd];
}

static int encfs_open(const char *path, int flags, int mode) {
    // `path` arriva già privato del prefisso "/enc": è il path reale su SD.
    if (!s_key_set) {
        ESP_LOGE(TAG, "Chiave non impostata: apertura %s negata", path);
        errno = EACCES;
        return -1;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < ENCFS_MAX_FILES; ++i) {
        if (!s_files[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_mtx);
        errno = EMFILE;
        return -1;
    }
    enc_file_t *f = &s_files[idx];
    memset(f, 0, sizeof(*f));

    int rfd = open(path, flags, mode);
    if (rfd < 0) {
        xSemaphoreGive(s_mtx);
        return -1;  // errno impostato da open()
    }

    uint8_t iv[16];
    bool writing = ((flags & O_ACCMODE) != O_RDONLY);
    bool creating = (flags & O_CREAT) != 0;

    if (writing && creating) {
        esp_fill_random(iv, sizeof(iv));
        uint32_t magic = CORE_CRYPTO_MAGIC;
        if (lseek(rfd, 0, SEEK_SET) < 0 ||
            write(rfd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
            write(rfd, iv, sizeof(iv)) != (ssize_t)sizeof(iv)) {
            ESP_LOGE(TAG, "Scrittura header cifrato fallita: %s", path);
            close(rfd);
            xSemaphoreGive(s_mtx);
            errno = EIO;
            return -1;
        }
    } else {
        uint32_t magic = 0;
        if (lseek(rfd, 0, SEEK_SET) < 0 ||
            read(rfd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
            magic != CORE_CRYPTO_MAGIC ||
            read(rfd, iv, sizeof(iv)) != (ssize_t)sizeof(iv)) {
            ESP_LOGE(TAG, "Header cifrato non valido: %s", path);
            close(rfd);
            xSemaphoreGive(s_mtx);
            errno = EIO;
            return -1;
        }
    }

    if (!core_crypto_init_ctx(&f->crypto, s_key, iv)) {
        close(rfd);
        xSemaphoreGive(s_mtx);
        errno = EIO;
        return -1;
    }
    f->real_fd = rfd;
    f->logical_pos = 0;
    f->used = true;
    xSemaphoreGive(s_mtx);
    return idx;
}

static ssize_t encfs_write(int fd, const void *data, size_t size) {
    enc_file_t *f = slot_from_fd(fd);
    if (!f) { errno = EBADF; return -1; }
    if (size == 0) return 0;

    const uint8_t *src = (const uint8_t *)data;
    uint8_t chunk[ENCFS_CHUNK];
    size_t done = 0;
    while (done < size) {
        size_t n = size - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        memcpy(chunk, src + done, n);
        core_crypto_crypt_at(&f->crypto, chunk, n, f->logical_pos);
        if (lseek(f->real_fd, (off_t)(f->logical_pos + ENCFS_HDR_BYTES), SEEK_SET) < 0) {
            return done ? (ssize_t)done : -1;
        }
        ssize_t w = write(f->real_fd, chunk, n);
        if (w < 0) {
            return done ? (ssize_t)done : -1;
        }
        f->logical_pos += (uint64_t)w;
        done += (size_t)w;
        if ((size_t)w < n) break;
    }
    return (ssize_t)done;
}

static ssize_t encfs_read(int fd, void *dst, size_t size) {
    enc_file_t *f = slot_from_fd(fd);
    if (!f) { errno = EBADF; return -1; }
    if (size == 0) return 0;

    if (lseek(f->real_fd, (off_t)(f->logical_pos + ENCFS_HDR_BYTES), SEEK_SET) < 0) {
        return -1;
    }
    ssize_t n = read(f->real_fd, dst, size);
    if (n <= 0) return n;
    core_crypto_crypt_at(&f->crypto, (uint8_t *)dst, (size_t)n, f->logical_pos);
    f->logical_pos += (uint64_t)n;
    return n;
}

static off_t encfs_lseek(int fd, off_t offset, int whence) {
    enc_file_t *f = slot_from_fd(fd);
    if (!f) { errno = EBADF; return -1; }

    off_t base;
    if (whence == SEEK_SET) {
        base = offset;
    } else if (whence == SEEK_CUR) {
        base = (off_t)f->logical_pos + offset;
    } else if (whence == SEEK_END) {
        off_t real_end = lseek(f->real_fd, 0, SEEK_END);
        if (real_end < 0) return -1;
        off_t logical_end = (real_end >= ENCFS_HDR_BYTES) ? (real_end - ENCFS_HDR_BYTES) : 0;
        base = logical_end + offset;
    } else {
        errno = EINVAL;
        return -1;
    }
    if (base < 0) { errno = EINVAL; return -1; }
    f->logical_pos = (uint64_t)base;
    return base;
}

static int encfs_fstat(int fd, struct stat *st) {
    enc_file_t *f = slot_from_fd(fd);
    if (!f) { errno = EBADF; return -1; }
    int r = fstat(f->real_fd, st);
    if (r == 0 && st->st_size >= ENCFS_HDR_BYTES) {
        st->st_size -= ENCFS_HDR_BYTES;
    }
    return r;
}

static int encfs_fsync(int fd) {
    enc_file_t *f = slot_from_fd(fd);
    if (!f) { errno = EBADF; return -1; }
    return fsync(f->real_fd);
}

static int encfs_close(int fd) {
    enc_file_t *f = slot_from_fd(fd);
    if (!f) { errno = EBADF; return -1; }
    int r = close(f->real_fd);
    core_crypto_deinit_ctx(&f->crypto);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    f->used = false;
    xSemaphoreGive(s_mtx);
    return r;
}

bool core_encfs_register(void) {
    if (s_registered) return true;
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return false;
    }
    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.open = encfs_open;
    vfs.write = encfs_write;
    vfs.read = encfs_read;
    vfs.lseek = encfs_lseek;
    vfs.close = encfs_close;
    vfs.fstat = encfs_fstat;
    vfs.fsync = encfs_fsync;

    esp_err_t e = esp_vfs_register(CORE_ENCFS_PREFIX, &vfs, nullptr);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register(%s) fallita: %d", CORE_ENCFS_PREFIX, (int)e);
        return false;
    }
    s_registered = true;
    ESP_LOGI(TAG, "VFS cifrante attivo su %s (cifratura MP4 real-time)", CORE_ENCFS_PREFIX);
    return true;
}

void core_encfs_set_key(const uint8_t key[16]) {
    memcpy(s_key, key, 16);
    s_key_set = true;
}

bool core_encfs_make_path(const char *real_path, char *out, unsigned out_len) {
    if (!real_path || !out || out_len == 0) return false;
    int n = snprintf(out, out_len, "%s%s", CORE_ENCFS_PREFIX, real_path);
    return n > 0 && (unsigned)n < out_len;
}
