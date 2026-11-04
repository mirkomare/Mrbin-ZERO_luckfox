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
/* Opzioni come in /proc/mounts: "vfat ro,noatime,... 0 0". Il token è "ro",
 * non la sottostringa di errors=remount-ro. */
static bool mount_opts_readonly(const char *rest) {
    if (!rest || !rest[0]) {
        return false;
    }
    const char *opts = strchr(rest, ' ');
    if (!opts) {
        return false;
    }
    while (*opts == ' ') {
        ++opts;
    }
    const char *end = strchr(opts, ' ');
    size_t n = end ? (size_t)(end - opts) : strlen(opts);
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && opts[i] != ',') {
            ++i;
        }
        if ((i - start) == 2 && opts[start] == 'r' && opts[start + 1] == 'o') {
            return true;
        }
        if (i < n && opts[i] == ',') {
            ++i;
        }
    }
    return false;
}

static bool mount_fields(const char *mp, char *dev_out, size_t dev_len) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp || !mp) {
        if (fp) fclose(fp);
        return false;
    }
    char dev[128], dir[160], rest[256];
    bool ok = false;
    while (fscanf(fp, "%127s %159s %255[^\n]", dev, dir, rest) == 3) {
        if (strcmp(dir, mp) == 0) {
            if (dev_out && dev_len > 0) {
                snprintf(dev_out, dev_len, "%s", dev);
            }
            ok = true;
            break;
        }
    }
    fclose(fp);
    return ok;
}

static bool path_is_mounted(const char *mp) {
    return mount_fields(mp, nullptr, 0);
}

/* Dal mount point (anche bind) al block device. /sdcard → /mnt/sdcard → mmcblk. */
static bool resolve_block_dev(const char *mp, char *dev_out, size_t dev_len) {
    char cur[160];
    snprintf(cur, sizeof(cur), "%s", mp ? mp : "");
    for (int hop = 0; hop < 4; ++hop) {
        char dev[128];
        if (!mount_fields(cur, dev, sizeof(dev))) {
            return false;
        }
        if (strncmp(dev, "/dev/", 5) == 0) {
            snprintf(dev_out, dev_len, "%s", dev);
            return true;
        }
        snprintf(cur, sizeof(cur), "%s", dev);
    }
    return false;
}

static bool dev_is_system_emmc(const char *dev);

static bool try_mount_dev(const char *dev, const char *mp);

bool hal_sd_probe_write(const char *mp) {
    mp = sd_mount_point(mp);
    char dev[128];
    /* Senza questo, /sdcard non montata è una directory su eMMC e il probe
     * riesce sul rootfs, non sulla scheda. */
    if (!resolve_block_dev(mp, dev, sizeof(dev)) || dev_is_system_emmc(dev)) {
        return false;
    }
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
    char block[128];
    if (!resolve_block_dev(mp, block, sizeof(block))) {
        return false;
    }
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }
    char dev[128], dir[160], rest[256];
    bool ro = false;
    bool saw = false;
    while (fscanf(fp, "%127s %159s %255[^\n]", dev, dir, rest) == 3) {
        if (strcmp(dev, block) != 0) {
            continue;
        }
        saw = true;
        if (mount_opts_readonly(rest)) {
            ro = true;
        }
    }
    fclose(fp);
    (void)dir;
    return saw && ro;
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
        char cmd[320];
        /* stdin da /dev/null: fsck non può restare in attesa di [y/n]. */
        snprintf(cmd, sizeof(cmd),
                 "%s %s %s </dev/null >/tmp/mrbin-fsck.log 2>&1",
                 tools[i], flags ? flags : "", dev);
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

static bool dev_is_system_emmc(const char *dev);
static bool pick_sd_dev(char *out, size_t len);
static bool bind_existing_vfat(const char *mp);

struct sd_mnt_ent {
    char dev[128];
    char dir[160];
};

static int read_mounts(struct sd_mnt_ent *out, int max) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp || max <= 0) {
        if (fp) fclose(fp);
        return 0;
    }
    int n = 0;
    char rest[256];
    while (n < max && fscanf(fp, "%127s %159s %255[^\n]", out[n].dev, out[n].dir, rest) == 3) {
        n++;
    }
    fclose(fp);
    return n;
}

static bool umount_dir(const char *dir) {
    if (!dir || !dir[0]) {
        return false;
    }
    if (umount2(dir, 0) == 0) {
        return true;
    }
    if (umount2(dir, MNT_DETACH) == 0) {
        for (int i = 0; i < 25; ++i) {
            if (!path_is_mounted(dir)) {
                return true;
            }
            usleep(100000);
        }
    }
    return !path_is_mounted(dir);
}

/* Smonta bind e mount reali. fsck su un volume ancora montato corrompe la FAT. */
static bool umount_dev_everywhere(const char *dev) {
    if (!dev || !dev[0]) {
        return false;
    }
    for (int pass = 0; pass < 8; ++pass) {
        struct sd_mnt_ent ents[48];
        int n = read_mounts(ents, 48);
        int bind_i = -1;
        int real_i = -1;
        for (int i = 0; i < n; ++i) {
            if (strcmp(ents[i].dev, dev) == 0) {
                real_i = i;
                continue;
            }
            if (ents[i].dev[0] != '/') {
                continue;
            }
            for (int j = 0; j < n; ++j) {
                if (strcmp(ents[j].dir, ents[i].dev) == 0 && strcmp(ents[j].dev, dev) == 0) {
                    bind_i = i;
                    break;
                }
            }
        }
        if (bind_i < 0 && real_i < 0) {
            return true;
        }
        const char *victim = bind_i >= 0 ? ents[bind_i].dir : ents[real_i].dir;
        if (!umount_dir(victim)) {
            ESP_LOGE("hal_sd", "umount %s fallito, %s resta montato", victim, dev);
            return false;
        }
    }
    return false;
}

static bool s_fsck_ran = false;

bool hal_sd_repair_vfat(const char *mp) {
    mp = sd_mount_point(mp);
    char dev[128];
    if (!resolve_block_dev(mp, dev, sizeof(dev)) && !pick_sd_dev(dev, sizeof(dev))) {
        return false;
    }
    if (dev_is_system_emmc(dev)) {
        return false;
    }
    if (s_fsck_ran) {
        return hal_sd_probe_write(mp);
    }
    if (!umount_dev_everywhere(dev)) {
        ESP_LOGE("hal_sd", "fsck saltato: %s ancora montato", dev);
        return false;
    }
    sync();
    s_fsck_ran = true;
    /* Solo -a. -r è interattivo e blocca il thread HTTP su [y/n]. */
    (void)hal_sd_run_on_dev(k_fsck_fat, "-a -w", dev);
    return hal_sd_mount_dev_rw(dev, mp) && hal_sd_probe_write(mp);
}

bool hal_sd_format_vfat(const char *mp, const char *dev_hint) {
    mp = sd_mount_point(mp);
    char dev[128];
    if (dev_hint && dev_hint[0]) {
        snprintf(dev, sizeof(dev), "%s", dev_hint);
    } else if (!path_mount_device(mp, dev, sizeof(dev)) && !pick_sd_dev(dev, sizeof(dev))) {
        return false;
    }
    if (dev_is_system_emmc(dev)) {
        return false;
    }
    if (!umount_dev_everywhere(dev)) {
        return false;
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
    char dev[128];
    if (!resolve_block_dev(mp, dev, sizeof(dev)) && !pick_sd_dev(dev, sizeof(dev))) {
        return false;
    }
    if (dev_is_system_emmc(dev)) {
        return false;
    }
    struct sd_mnt_ent ents[48];
    int n = read_mounts(ents, 48);
    for (int i = 0; i < n; ++i) {
        if (strcmp(ents[i].dev, dev) != 0) {
            continue;
        }
        if (mount(nullptr, ents[i].dir, nullptr, MS_REMOUNT, "rw") != 0) {
            (void)mount(nullptr, ents[i].dir, "vfat", MS_REMOUNT, "rw,umask=000");
        }
    }
    if (!path_is_mounted(mp)) {
        if (!bind_existing_vfat(mp)) {
            (void)hal_sd_mount_dev_rw(dev, mp);
        }
    }
    if (hal_sd_mount_is_readonly(mp) || !hal_sd_probe_write(mp)) {
        char src[128];
        if (mount_fields(mp, src, sizeof(src)) && strncmp(src, "/dev/", 5) != 0) {
            (void)umount_dir(mp);
            (void)bind_existing_vfat(mp);
        }
    }
    return !hal_sd_mount_is_readonly(mp) && hal_sd_probe_write(mp);
}

/* Pico Zero eMMC: mmcblk0 è il disco di sistema. La SD è mmcblk2
 * (il kernel la monta già su /mnt/sdcard). mmcblk1 è il WiFi SDIO. */
static bool dev_is_system_emmc(const char *dev) {
    return dev && strncmp(dev, "/dev/mmcblk0", 13) == 0;
}

static bool pick_sd_dev(char *out, size_t len) {
    static const char *cands[] = {
        "/dev/mmcblk2p1", "/dev/mmcblk1p1", "/dev/mmcblk2", "/dev/mmcblk1", nullptr
    };
    for (int i = 0; cands[i]; ++i) {
        if (dev_is_system_emmc(cands[i])) continue;
        if (access(cands[i], F_OK) == 0) {
            snprintf(out, len, "%s", cands[i]);
            return true;
        }
    }
    return false;
}

static bool bind_existing_vfat(const char *mp) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return false;
    char dev[128], dir[160], fstype[32], rest[256];
    char src[160] = {0};
    while (fscanf(fp, "%127s %159s %31s %255[^\n]", dev, dir, fstype, rest) == 4) {
        if (strcmp(dir, mp) == 0) continue;
        if (strstr(dev, "mmcblk") == nullptr) continue;
        if (dev_is_system_emmc(dev)) continue;
        if (strcmp(fstype, "vfat") != 0 && strcmp(fstype, "exfat") != 0) continue;
        snprintf(src, sizeof(src), "%s", dir);
        break;
    }
    fclose(fp);
    if (!src[0]) return false;
    mkdir(mp, 0755);
    if (mount(src, mp, nullptr, MS_BIND, nullptr) != 0) return false;
    return path_is_mounted(mp);
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
    if (bind_existing_vfat(mp)) {
        return true;
    }
    release_sdmmc_cmd();
    char dev[128];
    if (pick_sd_dev(dev, sizeof(dev)) && try_mount_dev(dev, mp) && path_is_mounted(mp)) {
        return true;
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
