#include "CoreSidecar.h"
#include "CoreConfig.h"
#include "CoreMrbinId.h"
#include "CoreRtc.h"
#include "CoreTime.h"
#include "CoreVideo.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "core_sidecar";

static void json_escape(FILE *fp, const char *s) {
    fputc('"', fp);
    if (!s) {
        fputc('"', fp);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else if (*p < 0x20) {
            fprintf(fp, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

static void json_kv_str(FILE *fp, const char *key, const char *val, bool comma) {
    fprintf(fp, "  ");
    json_escape(fp, key);
    fprintf(fp, ": ");
    json_escape(fp, val);
    fprintf(fp, "%s\n", comma ? "," : "");
}

static void json_kv_int(FILE *fp, const char *key, long val, bool comma) {
    fprintf(fp, "  \"%s\": %ld%s\n", key, val, comma ? "," : "");
}

static void json_kv_bool(FILE *fp, const char *key, bool val, bool comma) {
    fprintf(fp, "  \"%s\": %s%s\n", key, val ? "true" : "false", comma ? "," : "");
}

static bool atomic_replace(const char *tmp_path, const char *final_path) {
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGE(TAG, "rename %s → %s fallito", tmp_path, final_path);
        remove(tmp_path);
        return false;
    }
    return true;
}

bool core_sidecar_write(const char *json_path, const core_sidecar_info_t *info) {
    if (!json_path || !info) {
        return false;
    }
    const core_mrbin_id_t *id = core_mrbin_id_get();
    if (!id || !id->uid_valid) {
        ESP_LOGW(TAG, "Sidecar: device_uid assente — skip");
        return false;
    }

    char tmp[220];
    snprintf(tmp, sizeof(tmp), "%s.tmp", json_path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Apertura %s fallita", tmp);
        return false;
    }

    bool synced = core_time_is_valid();
    char utc[40] = "unsynced";
    char local_iso[48] = "unsynced";
    long epoch = 0;
    const char *time_source = "unsynced";
    if (synced) {
        time_t now = time(nullptr);
        epoch = (long)now;
        struct tm utc_tm;
        gmtime_r(&now, &utc_tm);
        snprintf(utc, sizeof(utc), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                 utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
        struct tm loc;
        localtime_r(&now, &loc);
        char tz[8] = "+0000";
        strftime(tz, sizeof(tz), "%z", &loc);
        snprintf(local_iso, sizeof(local_iso), "%04d-%02d-%02dT%02d:%02d:%02d%c%c%c:%c%c",
                 loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday,
                 loc.tm_hour, loc.tm_min, loc.tm_sec,
                 tz[0], tz[1], tz[2], tz[3], tz[4]);
        time_source = core_rtc_is_present() ? "rtc" : "sntp";
    }

    int fps = info->fps > 0 ? info->fps : (info->fps_recorded > 0 ? info->fps_recorded : 1);
    const char *video = info->video_file[0] ? info->video_file : "clip.h264";
    const char *side = info->sidecar_file[0] ? info->sidecar_file : "clip.json";

    fprintf(fp, "{\n");
    json_kv_str(fp, "schema", "mrbin-recorder-sidecar-v1", true);
    json_kv_str(fp, "sidecar_type", "mrbin_video_metadata", true);
    json_kv_str(fp, "device_id", id->device_id, true);
    json_kv_str(fp, "hardware_id", id->hardware_id, true);
    json_kv_str(fp, "device_type", CORE_MRBIN_DEVICE_TYPE, true);
    json_kv_str(fp, "device_name", id->device_id, true);
    json_kv_str(fp, "device_uid", id->device_uid, true);
    json_kv_str(fp, "camera_name", core_video_sensor_name(), true);
    json_kv_str(fp, "camera_key", id->camera_key, true);
    json_kv_str(fp, "camera_label", id->camera_label, true);
    json_kv_str(fp, "recorded_at_utc", utc, true);
    json_kv_str(fp, "recorded_at_local", local_iso, true);
    json_kv_str(fp, "timestamp_local", local_iso, true);
    json_kv_int(fp, "timestamp_epoch", epoch, true);
    json_kv_bool(fp, "time_synced", synced, true);
    json_kv_str(fp, "time_source", time_source, true);
    json_kv_str(fp, "format", "h264", true);
    json_kv_str(fp, "codec", "h264", true);
    json_kv_str(fp, "container", "raw_h264_annex_b", true);
    json_kv_str(fp, "video_file", video, true);
    json_kv_str(fp, "sidecar_file", side, true);
    json_kv_int(fp, "width", info->width, true);
    json_kv_int(fp, "height", info->height, true);
    json_kv_int(fp, "fps", fps, true);
    json_kv_int(fp, "duration_ms_recorded", (long)info->duration_ms, true);
    if (info->osd_enable && synced) {
        json_kv_str(fp, "overlay_text", local_iso, true);
    }
    if (info->fps_source > 0) {
        json_kv_int(fp, "fps_source", info->fps_source, true);
    }
    if (info->fps_target > 0) {
        json_kv_int(fp, "fps_target", info->fps_target, true);
    }
    json_kv_int(fp, "fps_recorded", info->fps_recorded > 0 ? info->fps_recorded : fps, true);
    if (info->bitrate > 0) {
        json_kv_int(fp, "bitrate", (long)info->bitrate, true);
    }
    if (info->gop > 0) {
        json_kv_int(fp, "gop", (long)info->gop, true);
    }
    if (info->qp_min > 0) {
        json_kv_int(fp, "qp_min", (long)info->qp_min, true);
    }
    if (info->qp_max > 0) {
        json_kv_int(fp, "qp_max", (long)info->qp_max, true);
    }
    if (info->preset[0]) {
        json_kv_str(fp, "preset", info->preset, true);
        json_kv_str(fp, "profile_name", info->preset, true);
    }
    json_kv_bool(fp, "motion_extend", info->motion_extend, true);
    json_kv_bool(fp, "pir_enabled", info->pir_enabled, true);
    json_kv_int(fp, "sequence", (long)info->sequence, false);
    fprintf(fp, "}\n");

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (!atomic_replace(tmp, json_path)) {
        return false;
    }
    ESP_LOGI(TAG, "Sidecar scritto: %s", json_path);
    return true;
}
