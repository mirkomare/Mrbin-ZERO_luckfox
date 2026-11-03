#include "hal_sd.h"
#include "CoreConfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

static const char *sd_mount_point(const char *mp) {
    if (mp && mp[0]) return mp;
    const char *env = getenv("CORE_SD_MOUNT");
    if (env && env[0]) return env;
    return CORE_SD_MOUNT_POINT;
}

static bool dir_ok(const char *mp) {
    struct stat st;
    return stat(mp, &st) == 0 && S_ISDIR(st.st_mode);
}

#ifndef _WIN32
static bool path_mount_line(const char *mp, char *rest_out, size_t rest_len) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }
    char dev[128], dir[160], rest[256];
    bool ok = false;
    while (fscanf(fp, "%127s %159s %255[^\n]", dev, dir, rest) == 3) {
        if (strcmp(dir, mp) == 0 && strstr(dev, "mmcblk") != nullptr) {
            if (rest_out && rest_len > 0) {
                snprintf(rest_out, rest_len, "%s", rest);
            }
            ok = true;
            break;
        }
    }
    fclose(fp);
    return ok;
}

static bool path_is_mounted(const char *mp) {
    return path_mount_line(mp, nullptr, 0);
}

static bool mount_opts_readonly(const char *rest) {
    if (!rest || !rest[0]) {
        return false;
    }
    return strstr(rest, "(ro,") != nullptr || strstr(rest, ",ro,") != nullptr ||
           strstr(rest, "(ro)") != nullptr;
}

static bool try_mount_dev(const char *dev, const char *mp);

bool hal_sd_probe_write(const char *mp) {
    mp = sd_mount_point(mp);
    char path[192];
    snprintf(path, sizeof(path), "%s/.mrbin_rw_probe", mp);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    fclose(fp);
    unlink(path);
    return true;
}

bool hal_sd_mount_is_readonly(const char *mp) {
    mp = sd_mount_point(mp);
    char rest[256];
    if (!path_mount_line(mp, rest, sizeof(rest))) {
        return false;
    }
    return mount_opts_readonly(rest);
}

static void release_sdmmc_cmd(void) {
    if (access("/sys/class/gpio/gpio101", F_OK) != 0) return;
    FILE *fp = fopen("/sys/class/gpio/unexport", "w");
    if (fp) {
        fputs("101", fp);
        fclose(fp);
    }
}

static int try_mount_fs(const char *dev, const char *mp, const char *fstype, const char *data) {
    if (mount(dev, mp, fstype, 0, data ? data : "") == 0) return 0;
    return -1;
}

static bool try_mount_dev(const char *dev, const char *mp) {
    if (access(dev, F_OK) != 0) return false;
    if (try_mount_fs(dev, mp, "exfat", nullptr) == 0) return true;
    if (try_mount_fs(dev, mp, "vfat", "umask=000") == 0) return true;
    if (try_mount_fs(dev, mp, "ntfs3", nullptr) == 0) return true;
    if (try_mount_fs(dev, mp, "ntfs", nullptr) == 0) return true;
    if (try_mount_fs(dev, mp, "auto", nullptr) == 0) return true;
    if (access("/usr/bin/ntfs-3g", X_OK) == 0 || access("/sbin/ntfs-3g", X_OK) == 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ntfs-3g %s %s -o rw,umask=000", dev, mp);
        if (system(cmd) == 0) return true;
    }
    return false;
}

static bool path_mount_device(const char *mp, char *dev_out, size_t dev_len) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }
    char dev[128], dir[160], rest[256];
    bool ok = false;
    while (fscanf(fp, "%127s %159s %255[^\n]", dev, dir, rest) == 3) {
        if (strcmp(dir, mp) == 0 && strstr(dev, "mmcblk") != nullptr) {
            snprintf(dev_out, dev_len, "%s", dev);
            ok = true;
            break;
        }
    }
    fclose(fp);
    return ok;
}

static bool hal_sd_run_on_dev(const char *const *tools, const char *flags, const char *dev) {
    if (!tools || !dev) {
        return false;
    }
    for (int i = 0; tools[i]; ++i) {
        if (access(tools[i], X_OK) != 0) {
            continue;
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s %s %s 2>/dev/null", tools[i], flags, dev);
        if (system(cmd) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const k_fsck_fat[] = {
    "/usr/sbin/fsck.fat",
    "/usr/sbin/fsck.vfat",
    "/userdata/mrbin/fsck.fat",
    "/oem/usr/sbin/fsck.fat",
    nullptr,
};

static const char *const k_mkfs_fat[] = {
    "/usr/sbin/mkfs.fat",
    "/usr/sbin/mkfs.vfat",
    "/userdata/mrbin/mkfs.fat",
    "/oem/usr/sbin/mkfs.fat",
    nullptr,
};

static bool hal_sd_mount_dev_rw(const char *dev, const char *mp) {
    mkdir(mp, 0755);
    if (try_mount_fs(dev, mp, "vfat", "umask=000,errors=continue") == 0) {
        return !hal_sd_mount_is_readonly(mp);
    }
    if (try_mount_fs(dev, mp, "vfat", "umask=000") == 0) {
        return !hal_sd_mount_is_readonly(mp);
    }
    return try_mount_dev(dev, mp) && !hal_sd_mount_is_readonly(mp);
}

bool hal_sd_repair_vfat(const char *mp) {
    mp = sd_mount_point(mp);
    char dev[128];
    if (!path_mount_device(mp, dev, sizeof(dev))) {
        snprintf(dev, sizeof(dev), "/dev/mmcblk1p1");
    }
    if (path_is_mounted(mp)) {
        umount2(mp, MNT_DETACH);
    }
    sync();
    (void)hal_sd_run_on_dev(k_fsck_fat, "-a -w", dev);
    (void)hal_sd_run_on_dev(k_fsck_fat, "-r", dev);
    return hal_sd_mount_dev_rw(dev, mp);
}

bool hal_sd_format_vfat(const char *mp, const char *dev_hint) {
    mp = sd_mount_point(mp);
    char dev[128];
    if (dev_hint && dev_hint[0]) {
        snprintf(dev, sizeof(dev), "%s", dev_hint);
    } else if (!path_mount_device(mp, dev, sizeof(dev))) {
        snprintf(dev, sizeof(dev), "/dev/mmcblk1p1");
    }
    if (path_is_mounted(mp)) {
        umount2(mp, MNT_DETACH);
    }
    sync();
    bool mk = false;
    for (int i = 0; k_mkfs_fat[i]; ++i) {
        if (access(k_mkfs_fat[i], X_OK) != 0) {
            continue;
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s -F 32 -n MRBIN %s 2>/dev/null", k_mkfs_fat[i], dev);
        if (system(cmd) == 0) {
            mk = true;
            break;
        }
    }
    if (!mk) {
        return false;
    }
    return hal_sd_mount_dev_rw(dev, mp);
}

bool hal_sd_remount_rw(const char *mp) {
    mp = sd_mount_point(mp);
    if (!path_is_mounted(mp)) {
        return false;
    }
    if (mount(nullptr, mp, nullptr, MS_REMOUNT, "rw") == 0 &&
        !hal_sd_mount_is_readonly(mp)) {
        return true;
    }
    if (mount(nullptr, mp, "vfat", MS_REMOUNT, "rw,umask=000") == 0 &&
        !hal_sd_mount_is_readonly(mp)) {
        return true;
    }
    char dev[128];
    if (!path_mount_device(mp, dev, sizeof(dev))) {
        return hal_sd_repair_vfat(mp);
    }
    umount2(mp, MNT_DETACH);
    if (hal_sd_mount_dev_rw(dev, mp)) {
        return true;
    }
    return hal_sd_repair_vfat(mp);
}

static bool ensure_mounted(const char *mp) {
    if (path_is_mounted(mp)) {
        if (hal_sd_mount_is_readonly(mp)) {
            (void)hal_sd_remount_rw(mp);
        }
        return true;
    }
    mkdir(mp, 0755);
    if (strcmp(mp, "/sdcard") != 0) {
        mkdir("/sdcard", 0755);
    }
    release_sdmmc_cmd();
    static const char *devs[] = {
        "/dev/mmcblk1p1", "/dev/mmcblk0p1", "/dev/mmcblk1", "/dev/mmcblk0", nullptr
    };
    for (int i = 0; devs[i]; ++i) {
        if (try_mount_dev(devs[i], mp) && path_is_mounted(mp)) return true;
    }
    return path_is_mounted(mp);
}
#else
bool hal_sd_mount_is_readonly(const char *mp) {
    (void)mp;
    return false;
}

bool hal_sd_probe_write(const char *mp) {
    (void)mp;
    return true;
}

bool hal_sd_remount_rw(const char *mp) {
    (void)mp;
    return true;
}

bool hal_sd_repair_vfat(const char *mp) {
    (void)mp;
    return true;
}

bool hal_sd_format_vfat(const char *mp, const char *dev) {
    (void)mp;
    (void)dev;
    return true;
}
#endif

bool hal_sd_probe_mount(const char *mp) {
    mp = sd_mount_point(mp);
#ifdef _WIN32
    if (dir_ok(mp)) return true;
    _mkdir("sdcard");
    return dir_ok("sdcard") || dir_ok(mp);
#else
    return ensure_mounted(mp);
#endif
}

esp_err_t esp_vfs_fat_sdmmc_mount(const char *mp, const sdmmc_host_t *,
                                  const sdmmc_slot_config_t *,
                                  const esp_vfs_fat_sdmmc_mount_config_t *,
                                  sdmmc_card_t **out) {
    if (out) *out = (sdmmc_card_t *)1;
    mp = sd_mount_point(mp);
    if (hal_sd_probe_mount(mp)) return ESP_OK;
#ifdef _WIN32
    _mkdir("sdcard");
    return ESP_OK;
#else
    return ESP_FAIL;
#endif
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char *mp, sdmmc_card_t *) {
#ifndef _WIN32
    mp = sd_mount_point(mp);
    if (path_is_mounted(mp)) {
        umount2(mp, MNT_DETACH);
    }
#endif
    return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_format(const char *mp, sdmmc_card_t *) {
#ifndef _WIN32
    mp = sd_mount_point(mp);
    return hal_sd_format_vfat(mp, nullptr) ? ESP_OK : ESP_FAIL;
#else
    (void)mp;
    return ESP_FAIL;
#endif
}

esp_err_t esp_vfs_fat_info(const char *mp, uint64_t *total, uint64_t *free_b) {
#ifndef _WIN32
    mp = sd_mount_point(mp);
    if (!path_is_mounted(mp)) return ESP_FAIL;
    struct statvfs v;
    if (statvfs(mp, &v) != 0) return ESP_FAIL;
    if (total) *total = (uint64_t)v.f_blocks * v.f_frsize;
    if (free_b) *free_b = (uint64_t)v.f_bavail * v.f_frsize;
    return ESP_OK;
#else
    (void)mp;
    if (total) *total = 4ULL * 1024 * 1024 * 1024;
    if (free_b) *free_b = 2ULL * 1024 * 1024 * 1024;
    return ESP_OK;
#endif
}

void sdmmc_card_print_info(void *, sdmmc_card_t *) {}
esp_err_t sdmmc_host_init(void) { return ESP_OK; }
esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *, sd_pwr_ctrl_handle_t *h) {
    if (h) *h = (sd_pwr_ctrl_handle_t)1;
    return ESP_OK;
}
esp_err_t sd_pwr_ctrl_set_io_voltage(sd_pwr_ctrl_handle_t, int) { return ESP_OK; }
