"""Generate thin ESP-IDF include shims that pull in the Luckfox HAL."""
import os

ROOT = os.path.dirname(os.path.abspath(__file__))

STUBS = {
    "esp_log.h": '#include "hal_log.h"\n',
    "esp_timer.h": '#include "hal_timer.h"\n',
    "esp_random.h": '#include "hal_random.h"\n',
    "esp_system.h": '#include "hal_system.h"\n',
    "esp_heap_caps.h": '#include "hal_heap.h"\n',
    "esp_mac.h": '#include "hal_net.h"\n',
    "esp_idf_version.h": "#pragma once\n#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,0)\n",
    "esp_crt_bundle.h": "#pragma once\n#define esp_crt_bundle_attach nullptr\n",
    "esp_hosted.h": "#pragma once\nstatic inline int esp_hosted_deinit(void) { return 0; }\n",
    "sdkconfig.h": "#pragma once\n#define CONFIG_ESP_HOSTED_ENABLED 0\n#define CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE 0\n#define CONFIG_CAMERA_IMX708 1\n#define CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS 0\n#define CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS 0\n#define CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER 0\n#define CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER 1\n#define CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE 1\n#define SOC_SDMMC_IO_POWER_EXTERNAL 0\n",
    "nvs.h": '#include "hal_nvs.h"\n',
    "nvs_flash.h": '#include "hal_nvs.h"\n',
    "cJSON.h": '#include "hal_cjson.h"\n',
    "esp_http_server.h": '#include "hal_httpd.h"\n',
    "esp_http_client.h": '#include "hal_http_client.h"\n',
    "esp_wifi.h": '#include "hal_net.h"\n',
    "esp_event.h": '#include "hal_net.h"\n',
    "esp_netif.h": '#include "hal_net.h"\n',
    "esp_sntp.h": '#include "hal_time_sntp.h"\n',
    "esp_muxer.h": '#include "hal_muxer.h"\n',
    "esp_capture.h": '#include "hal_capture.h"\n',
    "esp_capture_advance.h": '#include "hal_capture.h"\n',
    "esp_capture_sink.h": '#include "hal_capture.h"\n',
    "esp_gmf_video_enc.h": '#include "hal_capture.h"\n',
    "esp_gmf_video_fps_cvt.h": '#include "hal_capture.h"\n',
    "esp_gmf_video_ppa.h": '#include "hal_capture.h"\n',
    "esp_gmf_video_types.h": '#include "hal_capture.h"\n',
    "encoder/esp_video_enc_default.h": '#include "hal_capture.h"\n',
    "impl/esp_capture_text_overlay.h": '#include "hal_capture.h"\n',
    "impl/esp_capture_video_v4l2_src.h": '#include "hal_capture.h"\n',
    "impl/mp4_muxer.h": '#include "hal_muxer.h"\n',
    "impl/avi_muxer.h": '#include "hal_muxer.h"\n',
    "esp_video_init.h": '#include "hal_video.h"\n',
    "esp_video_ioctl.h": '#include "hal_video.h"\n',
    "esp_video_device.h": '#include "hal_video.h"\n',
    "esp_video_isp_ioctl.h": '#include "hal_video.h"\n',
    "esp_video_isp_pipeline.h": '#include "hal_video.h"\n',
    "esp_ipa.h": '#include "hal_video.h"\n',
    "esp_cam_sensor_types.h": '#include "hal_video.h"\n',
    "esp_cam_motor_types.h": '#include "hal_video.h"\n',
    "ov5647_settings.h": "#pragma once\n",
    "ov5647_types.h": "#pragma once\n",
    "imx708.h": '#include "hal_video.h"\n',
    "esp_vfs.h": '#include "hal_encfs_vfs.h"\n',
    "esp_vfs_fat.h": '#include "hal_sd.h"\n',
    "driver/gpio.h": '#include "hal_gpio.h"\n',
    "driver/i2c_master.h": '#include "hal_i2c.h"\n',
    "driver/sdmmc_host.h": '#include "hal_sd.h"\n',
    "driver/sdmmc_default_configs.h": '#include "hal_sd.h"\n',
    "sdmmc_cmd.h": '#include "hal_sd.h"\n',
    "sd_pwr_ctrl_by_on_chip_ldo.h": '#include "hal_sd.h"\n',
    "esp_private/gpio.h": '#include "hal_gpio.h"\n',
    "soc/gpio_struct.h": '#include "hal_gpio.h"\n',
    "freertos/FreeRTOS.h": '#include "hal_thread.h"\n',
    "freertos/task.h": '#include "hal_thread.h"\n',
    "freertos/semphr.h": '#include "hal_thread.h"\n',
    "freertos/event_groups.h": '#include "hal_thread.h"\n',
    "freertos/queue.h": '#include "hal_thread.h"\n',
    "mbedtls/aes.h": '#include "hal_mbedtls.h"\n',
    "mbedtls/sha256.h": '#include "hal_mbedtls.h"\n',
    "mbedtls/gcm.h": '#include "hal_mbedtls.h"\n',
    "mbedtls/md.h": '#include "hal_mbedtls.h"\n',
    "mbedtls/private/aes.h": '#include "hal_mbedtls.h"\n',
    "mbedtls/private/sha256.h": '#include "hal_mbedtls.h"\n',
    "psa/crypto.h": '#include "hal_psa.h"\n',
    "linux/videodev2.h": '#include "hal_v4l2_stub.h"\n',
}

def main():
    for rel, body in STUBS.items():
        path = os.path.join(ROOT, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        print("wrote", rel)

if __name__ == "__main__":
    main()
