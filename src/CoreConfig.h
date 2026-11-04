#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// =====================================================================
// Luckfox Pico Zero (RV1106G3) — linux sysfs.
// pin = bank*32 + (group*8 + X)  A=0 B=1 C=2 D=3
// Mappa completa: docs/gpio-map.md. Override: /etc/mrbin/pins.env.
//
// Vietati: SDIO WiFi SDMMC0 GPIO3_A1–A7 (97–103), reset WiFi GPIO1_A2 (34),
// UART BT GPIO1_A3/A4, eMMC GPIO4_A/B, strap BOOT SARADC_IN0 GPIO4_C0 (144),
// TF SDMMC1 GPIO2_A*. CSI IMX415: reset GPIO3_C5, I2C4_M2 GPIO3_C7/D0.
//
//   LED esterno  → GPIO1_C3   linux  51  pin 21. Non resta un GPIO pulito né un pin
//                              solo PWM. Il pinout stampa SPI0_MISO e PWM6; SPI0 è spento.
//                              Il USER LED GPIO3_C6 (118) è il gemello di scheda.
//                              Nel DTS Luckfox è GPIO_ACTIVE_HIGH: pin alto = acceso.
//   SQW          → GPIO1_C7   linux  55  pin 36, il pinout stampa solo PWM11
//   MODE         → GPIO1_C5   linux  53  pin 32, UART4_TX (non I2C1). Resta: i GPIO
//                              senza bus sul connettore sono già usati.
//   D1 PIR wake  → GPIO1_C4   linux  52  pin 33, UART4_RX (non I2C1). Stesso motivo.
//   D2 end       → GPIO1_D2   linux  58  pin 26, SPI0_CS1. Nessun altro GPIO pulito.
//   MODEM_PWR    → GPIO2_B1   linux  73  pin 27, I2C1_SDA + UART0_TX. Stesso motivo.
//   DONE TPL     → GPIO0_A4   linux   4  pin 40. Il pinout stampa solo PWM1, non SPI.
//   DEMO jumper  → GPIO1_D3   linux  59  pin 22. Il pinout non stampa I2C/SPI/UART.
//   RTC I2C      → SDA GPIO1_C6 (54, pin 12, solo PWM10), SCL GPIO0_A3 (3, pin 38,
//                  nessuna funzione sul pinout). Bitbang. Non più SPI0 C1/C2.
// =====================================================================
#define CORE_GPIO_D1_WAKE         52
#define CORE_GPIO_D2_END          58
#define CORE_GPIO_TPL_DONE        4
#define CORE_GPIO_MODE_CFG        53
#define CORE_GPIO_STATUS_LED      51
#define CORE_GPIO_BOARD_LED       118
#define CORE_GPIO_RTC_SQW         55
#define CORE_GPIO_MODEM_PWR       73
#define CORE_GPIO_DEMO            59

#ifndef CORE_GPIO_PIN_SELFTEST
#define CORE_GPIO_PIN_SELFTEST 0
#endif

#if defined(__cplusplus)
static_assert(CORE_GPIO_D1_WAKE  == 52, "PIN: D1 = linux 52 (GPIO1_C4)");
static_assert(CORE_GPIO_D2_END   == 58, "PIN: D2 = linux 58 (GPIO1_D2)");
static_assert(CORE_GPIO_TPL_DONE == 4, "PIN: DONE = linux 4 (GPIO0_A4, pin 40, solo PWM)");
static_assert(CORE_GPIO_MODE_CFG == 53, "PIN: MODE = linux 53 (GPIO1_C5)");
static_assert(CORE_GPIO_STATUS_LED == 51, "PIN: LED esterno = linux 51 (GPIO1_C3, pin 21)");
static_assert(CORE_GPIO_RTC_SQW == 55, "PIN: SQW = linux 55 (GPIO1_C7)");
static_assert(CORE_GPIO_MODEM_PWR == 73, "PIN: MODEM = linux 73 (GPIO2_B1)");
static_assert(CORE_GPIO_DEMO == 59, "PIN: DEMO = linux 59 (GPIO1_D3, non GPIO2_A0 SD)");
#endif

/* Demo banco: preview VI ch1 (luma Y, no MJPEG) + movimento → H.264 mainpath. */
#define CORE_DEMO_PREVIEW_FPS       5
/* Percentuale di pixel cambiati nella finestra più mossa (0–100). */
#define CORE_DEMO_MOTION_THRESHOLD       8
#define CORE_DEMO_MOTION_STILL_THRESHOLD 4
#define CORE_DEMO_NO_MOTION_STOP_MS      8000
#define CORE_DEMO_MOTION_START_FRAMES    3
/* Frame sotto soglia prima di azzerare streak / annullare prep (evita buchi 1 frame). */
#define CORE_DEMO_MOTION_MISS_FRAMES     5
#define CORE_DEMO_CLIP_COOLDOWN_MS       3000
/* Dopo Stop da GUI Live: niente auto-restart movimento per questo intervallo. */
#define CORE_DEMO_UI_STOP_COOLDOWN_MS    20000
/* Tetto assoluto singola clip DEMO (wall clock), indipendente dal movimento. */
#define CORE_DEMO_MAX_CLIP_MS       90000
/* Tap RTSP: niente reopen VI. Pre-arm AE al 2° frame movimento. */
#define CORE_DEMO_AE_PREARM_FRAMES       1
/* Attesa max prep encoder prima di reopen VI (ms). Non bloccare il movimento. */
#define CORE_DEMO_PREP_WAIT_MS           350
/* Settle AE solo in prep, non sulla clip. */
#define CORE_DEMO_PREP_AE_SETTLE_MS      500
/* Rolling preroll H264 già esposto (~1.5 s). Taglio solo a inizio GOP. */
#define CORE_DEMO_PREP_PREROLL_MAX_FRAMES  45

#define CORE_MODEM_TEST_ON_MS     10000
#define CORE_STATUS_LED_ON_LEVEL  1
/* USER LED di scheda, GPIO3_C6: acceso con il pin a 1 (GPIO_ACTIVE_HIGH). */
#define CORE_BOARD_LED_ON_LEVEL   1
#define CORE_STATUS_LED_ERROR_BLINK_MS  100
#define CORE_STATUS_LED_AP_ON_MS        400
#define CORE_STATUS_LED_AP_BLINK_MS     1600
#define CORE_STATUS_LED_STA_BLINK_MS    1000
#define CORE_STATUS_LED_STA_BLINK_COUNT 3
#define CORE_STATUS_LED_STA_PULSE_MS    1000
/* Un tentativo di associazione, poi 10 s, poi 5 s. Al terzo fallimento: AP. */
#define CORE_WIFI_ATTEMPT_MS            8000
#define CORE_WIFI_RETRY_GAP1_MS         10000
#define CORE_WIFI_RETRY_GAP2_MS         5000
#define CORE_STATUS_LED_D2_BLINK_COUNT  2
#define CORE_STATUS_LED_D2_PULSE_MS     150
#define CORE_STATUS_LED_D2_GAP_MS       200
#define CORE_STATUS_LED_SAVING_ON_MS    80
#define CORE_STATUS_LED_SAVING_OFF_MS   80
#define CORE_GPIO_STOP_POLL_PRIO        8
#define CORE_STATUS_LED_TASK_PRIO       6
#define CORE_STATUS_LED_TASK_STACK      3072

#define CORE_WEB_PORT       1510

/* IMX415 (CMK-OT2022-PX1) su CSI 4 lane, I2C 0x37. Nativo 3840×2160.
 * Pagina prodotto Pico Zero: ISP 8M@15, encode 5M@30. Il default resta
 * 1920×1080@30. Sopra i 5 Mpixel il fps di encode scende a 15. */
#define CORE_VIDEO_WIDTH_DEFAULT    1920
#define CORE_VIDEO_HEIGHT_DEFAULT   1080
#define CORE_VIDEO_FPS_DEFAULT      30
#define CORE_VIDEO_WIDTH_MIN        640
#define CORE_VIDEO_WIDTH_MAX        3840
#define CORE_VIDEO_HEIGHT_MIN       480
#define CORE_VIDEO_HEIGHT_MAX       2160
#define CORE_VIDEO_FPS_MIN          1
#define CORE_VIDEO_FPS_MAX          30
#define CORE_VIDEO_FPS_LOW_MAX      3
#define CORE_VIDEO_FPS_1080_MAX     30
/* Pico Zero: encode 5M@30, 8M@15. Sopra questa area il tetto fps è 15. */
#define CORE_VIDEO_PIXELS_5M        5000000u
#define CORE_VIDEO_FPS_ABOVE_5M     15
/* SC3336 rkisp only has 25/30 modes. Drive VI/VENC at this rate for 1–3 fps
 * timelapse; keep the NVS rate in software. 1 fps on VI blacks AE/ISP. */
#define CORE_VIDEO_TL_PIPE_FPS      25
#define CORE_VIDEO_PRESET_OK_WIDTH   1920
#define CORE_VIDEO_PRESET_OK_HEIGHT  1080
#define CORE_VIDEO_PRESET_OK_FPS     30
#define CORE_VIDEO_SENSOR_NATIVE_W   3840
#define CORE_VIDEO_SENSOR_NATIVE_H   2160
/* One pass on this Pico Zero ISP. Wider than this and the ISP drops
 * the MIPI frame, so 3840x2160 opens VENC and then never returns a
 * picture (GUI: Capture failed, no frame). */
#define CORE_VIDEO_ISP_PASS_W        3072
#define CORE_VIDEO_ISP_PASS_H        1728

/* 2300 is not the sensor mode: only a size that lands on the native edge
 * is lifted (2300 → 2304). 1080 stays 1080 — FullHD is a preset, and the
 * capture path aligns the stride without changing the picture size. */
/* 3840x2160 (and anything past the one-pass ISP) becomes 3072x1728,
 * which is the frame the ISP actually delivers. */
static inline void core_video_clamp_isp_pass(uint16_t *w, uint16_t *h) {
    if (!w || !h) {
        return;
    }
    if (*w > (uint16_t)CORE_VIDEO_ISP_PASS_W || *h > (uint16_t)CORE_VIDEO_ISP_PASS_H) {
        *w = (uint16_t)CORE_VIDEO_ISP_PASS_W;
        *h = (uint16_t)CORE_VIDEO_ISP_PASS_H;
    }
}

static inline uint16_t core_video_align16(uint16_t v, uint16_t native) {
    if (v >= native) {
        return v;
    }
    uint32_t up = ((uint32_t)v + 15u) & ~15u;
    if (up == native) {
        return native;
    }
    return v;
}

/* Tetto fps che VENC può davvero tenere a questa misura. */
static inline uint8_t core_video_fps_ceiling(uint16_t w, uint16_t h) {
    if ((uint32_t)w * (uint32_t)h > CORE_VIDEO_PIXELS_5M) {
        return (uint8_t)CORE_VIDEO_FPS_ABOVE_5M;
    }
    return (uint8_t)CORE_VIDEO_FPS_MAX;
}
#define CORE_IMX708_CLEAN_W          1904
#define CORE_IMX708_CLEAN_H          1072
#define CORE_VIDEO_WIDTH            CORE_VIDEO_WIDTH_DEFAULT
#define CORE_VIDEO_HEIGHT           CORE_VIDEO_HEIGHT_DEFAULT
#define CORE_VIDEO_FPS              CORE_VIDEO_FPS_DEFAULT
#define CORE_VIDEO_BITRATE_DEFAULT  7000000
#define CORE_VIDEO_BITRATE_MIN      2000000
#define CORE_VIDEO_BITRATE_MAX      32000000
#define CORE_VIDEO_BITRATE_1080_MIN 12000000
#define CORE_VIDEO_BITRATE          CORE_VIDEO_BITRATE_DEFAULT
#define CORE_VIDEO_GOP_DEFAULT      24
#define CORE_VIDEO_GOP              CORE_VIDEO_GOP_DEFAULT
#define CORE_VIDEO_QP_MIN   20
#define CORE_VIDEO_QP_MAX   36
#define CORE_VIDEO_QP_1080_MIN  16
#define CORE_VIDEO_QP_1080_MAX  30
#define CORE_VIDEO_IS_FULLHD(w, h) (((uint32_t)(w) >= 1800u) || ((uint32_t)(h) >= 1000u))
#define CORE_IMX708_FOCUS_MIN       448
#define CORE_IMX708_FOCUS_DEFAULT   576
#define CORE_IMX708_AE_TARGET       70
#define CORE_VIDEO_DEV_NAME "/dev/video0" /* CIF probe; do not hold rkisp_mainpath */
#define CORE_VIDEO_RKISP_ENTITY "rkisp_mainpath"
#define CORE_VIDEO_RKISP_DEV_NAME "/dev/video11"
/* HTTP thread: first RKMPI VI+VENC on SC3336/rkisp often exceeds 3 s. */
#define CORE_VIDEO_OPEN_TIMEOUT_MS  12000
#define CORE_VIDEO_BAYER_AUTO  0
#define CORE_VIDEO_BAYER_RGGB  1
#define CORE_VIDEO_BAYER_GRBG  2
#define CORE_VIDEO_BAYER_GBRG  3
#define CORE_VIDEO_BAYER_BGGR  4
#define CORE_VIDEO_BAYER_MAX   CORE_VIDEO_BAYER_BGGR
#define CORE_REC_WARMUP_DROP_FRAMES     16
#define CORE_REC_WARMUP_DROP_FRAMES_PIR 0
#define CORE_REC_WARMUP_DROP_FRAMES_TL  12
/* Max wait for Live to drop rkisp. First VI DisableChn is ~2.5 s
 * (stream-off); whole release ~3.2 s. Cap 4 s, not the old 10 s+. */
#define CORE_LIVE_RELEASE_WAIT_MS       4000
/* Quiet gap after VI/CSI is idle, before the next EnableChn.
 * Live drops VI while 3A is still up (stream-off ack), then kills 3A.
 * A fixed sleep here only delayed Avvio. */
#define CORE_REC_AFTER_LIVE_MS          0
#define CORE_REC_AE_SETTLE_MS           1000
#define CORE_REC_AE_SETTLE_MS_PIR       900
#define CORE_REC_AE_SETTLE_MS_PIR_AF    900
/* Rec-fast / superfastboot: first encoded frame budget from kernel boot. */
#define CORE_REC_START_BUDGET_MS        500
#define CORE_REC_VCM_SETTLE_MS_AF       0
#define CORE_REC_PIR_GOP                12
#define CORE_REC_SD_WAIT_MS             0
#define CORE_VIDEO_FRAME_DUR_MS_DEFAULT (1000 / CORE_VIDEO_FPS_DEFAULT)
#define CORE_VIDEO_FRAME_DUR_MS         CORE_VIDEO_FRAME_DUR_MS_DEFAULT

#define CORE_LIVE_WIDTH     640
#define CORE_LIVE_HEIGHT    480
#define CORE_LIVE_FPS       5

#define CORE_SD_PIN_CLK     (-1)
#define CORE_SD_PIN_CMD     (-1)
#define CORE_SD_PIN_D0      (-1)
#define CORE_SD_PIN_D1      (-1)
#define CORE_SD_PIN_D2      (-1)
#define CORE_SD_PIN_D3      (-1)
#define CORE_SD_LDO_CHAN_ID 0

#define CORE_CSI_I2C_SCL    54
#define CORE_CSI_I2C_SDA    55

/* Bitbang. Non SPI0 (C1/C2) e non I2C4 della camera (GPIO3_C7/D0). */
#define CORE_RTC_I2C_PORT_NUM  (-1)
#define CORE_RTC_I2C_SDA       54
#define CORE_RTC_I2C_SCL       3
#define CORE_RTC_I2C_ADDR      0x68
#define CORE_RTC_EEPROM_ADDR   0x57
#define CORE_RTC_I2C_HZ        100000
#define CORE_ALARM_EEPROM_OFF    0x0000
#define CORE_ALARM_EEPROM_SIZE   2048

#define CORE_REC_OSD_ENABLE        1
#define CORE_REC_OSD_FONT_DEFAULT  20
#define CORE_REC_OSD_X_PCT_DEFAULT 2
#define CORE_REC_OSD_Y_PCT_DEFAULT 2
#define CORE_REC_OSD_REFRESH_US    500000LL
#define CORE_REC_OSD_TEXT_CHARS    19

#define CORE_D2_POST_DELAY_MS  5000
#define CORE_D2_DELAY_MIN_MS      100
#define CORE_D2_DELAY_MAX_MS   600000
#define CORE_TPL_ON_MAX_MINUTES    50
#define CORE_REC_MAX_DURATION_SEC_DEFAULT (10U * 60U)
#define CORE_REC_MAX_DURATION_SEC_MIN     60U
#define CORE_REC_MAX_DURATION_SEC_MAX     ((CORE_TPL_ON_MAX_MINUTES * 60U) - 120U)
#define CORE_REC_STOP_DEBOUNCE_MS  50
#define CORE_REC_STOP_POLL_MS      10
#define CORE_REC_START_DEBOUNCE_MS 50
#define CORE_REC_START_POLL_MS     10
#define CORE_REC_STOP_MODE_GPIO     0
#define CORE_REC_STOP_MODE_TIMER    1
#define CORE_REC_SESSION_SEC_MIN    1U
#define CORE_REC_SESSION_SEC_MAX    3600U
#define CORE_REC_SESSION_SEC_DEFAULT 30U
#define CORE_REC_MEDIA_VIDEO        0
#define CORE_REC_MEDIA_PHOTO        1
#define CORE_REC_PHOTO_FPS_MAX      1
#define CORE_REC_PHOTO_INTERVAL_MS  1000U
#define CORE_REC_PHOTO_META_FPS_MAX CORE_VIDEO_FPS_LOW_MAX
#define CORE_PHOTO_EXPORT_FPS       1U
#define CORE_PHOTO_JPEG_QUALITY       92
#define CORE_PHOTO_JPEG_WEB_MAX_BYTES (4U * 1024U * 1024U)
#define CORE_TPL_DONE_PULSE_MS  1000
#define CORE_TPL_DONE_GAP_MS     100
#define CORE_MODE_POLL_MS         50
#define CORE_MODE_EXIT_DEBOUNCE_MS 150

#define CORE_SD_MOUNT_POINT "/sdcard"
#define CORE_NVS_NAMESPACE  "mrbin_core"
/* Fast rootfs is erofs (ro); /var has no lib/. Persist on UBIFS userdata. */
#define CORE_NVS_DIR        "/userdata/mrbin"

#define CORE_ID_DIGITS      5
#define CORE_ID_MIN         10000
#define CORE_ID_MAX         99999

#define CORE_REC_FILE_SEQ_MIN  0
#define CORE_REC_FILE_SEQ_MAX  999
#define CORE_REC_FILE_SEQ_COUNT (CORE_REC_FILE_SEQ_MAX - CORE_REC_FILE_SEQ_MIN + 1)

#define CORE_AES_KEY_BYTES  16
#define CORE_CRYPTO_MAGIC   0x4D524249

#define CORE_MRBIN_API_BASE_DEFAULT     "https://mrbin.planetel.dev/api/v1"
#define CORE_MRBIN_HARDWARE_ID_DEFAULT  "mrbin-core-luckfox"
#define CORE_MRBIN_CAMERA_KEY_DEFAULT   "C1"
#define CORE_MRBIN_CAMERA_LABEL_DEFAULT "Camera"
#define CORE_MRBIN_CAMERA_NAME_DEFAULT  "SC3336"
// Enroll/upload headers unchanged; device_type is sidecar metadata only.
#define CORE_MRBIN_DEVICE_TYPE          "luckfox-rv1103"
#define CORE_MRBIN_DEVICE_UID_PREFIX    "luckfox-"
#define CORE_MRBIN_AES_KEY_BYTES        32
#define CORE_MRBIN_GCM_NONCE_BYTES      12
#define CORE_MRBIN_GCM_TAG_BYTES        16
#define CORE_MRBIN_CHUNK_BYTES          (4U * 1024U * 1024U)
#define CORE_MRBIN_DEVICE_UID_MAX       40
#define CORE_MRBIN_UPLOAD_ID_MAX        180
#define CORE_MRBIN_KEY_ID_MAX           64
#define CORE_MRBIN_API_URL_MAX          96
#define CORE_MRBIN_SYNC_TIMEOUT_MS      (5U * 60U * 1000U)
#define CORE_MRBIN_PING_PERIOD_MS       2000U
#define CORE_MRBIN_PING_TRY_MS          4000U

#define CORE_SD_MIN_FREE_BYTES  (50ULL * 1024ULL * 1024ULL)

// 64 MB DDR2, about 24 MB CMA. One 1080p NV12 frame is ~3 MB
// (1920*1080*3/2). 2–3 s of raw 30 fps is hundreds of MB and does not fit.
// PIR preroll is encoded H.264 only: 7 Mb/s * 3 s ≈ 2.6 MB. 4 MB covers
// about 2–3 s, including one large IDR. Not raw frames.
#define CORE_PIR_H264_RING_BYTES (4U * 1024U * 1024U)
#define CORE_REC_MUXER_OPEN_MIN_MS          0
#define CORE_REC_MUXER_OPEN_MIN_FRAMES_PIR 1
#define CORE_REC_MUXER_OPEN_MIN_FRAMES     1
#define CORE_REC_MUXER_FPS_MIN              1
#define CORE_REC_SD_LAG_MS            0
#define CORE_REC_SD_LAG_MS_PIR            0
#define CORE_REC_SD_LAG_CATCHUP_PCT_ENC   35
#define CORE_REC_SD_LAG_URGENT_PCT_ENC    50
#define CORE_RING_PAUSE_PCT_PIR_ENC       94
#define CORE_H264_PSRAM_USE_PCT_PIR_ENC   50
#define CORE_REC_SD_LAG_CATCHUP_PCT_PLAIN 35
#define CORE_REC_SD_LAG_URGENT_PCT_PLAIN  75
#define CORE_RING_PAUSE_PCT_PIR_PLAIN     92
#define CORE_H264_PSRAM_USE_PCT_PIR_PLAIN 50
#define CORE_REC_SD_LAG_CATCHUP_PCT   CORE_REC_SD_LAG_CATCHUP_PCT_ENC
#define CORE_REC_SD_LAG_URGENT_PCT    CORE_REC_SD_LAG_URGENT_PCT_ENC
#define CORE_RING_PAUSE_PCT_PIR       CORE_RING_PAUSE_PCT_PIR_ENC
#define CORE_REC_SD_BURST_FRAMES      16
#define CORE_REC_SD_BURST_FRAMES_PIR  16
#define CORE_REC_SD_BURST_FRAMES_PIR_ENC 32
#define CORE_REC_SD_BURST_FRAMES_PIR_ENC_URGENT 48
#define CORE_REC_SD_BURST_STOP        64
/* 8MP IDR is often past 1 MB (seen 1.3 MB at 2880x1620). A 1 MB cap
 * discarded the only keyframe, so the MP4 was never created. */
#define CORE_REC_SD_FRAME_COPY_MAX    (4U * 1024U * 1024U)
/* Web Stop: pull frames already in VENC, then halt. The pipeline keeps
 * producing until DisableChn, so a multi-second budget was a hard wait. */
#define CORE_REC_STOP_CAPTURE_DRAIN_MS     120
#define CORE_REC_STOP_CAPTURE_DRAIN_MS_PIR 2000
#define CORE_REC_STOP_CAPTURE_WAIT_MS      5500
#define CORE_REC_STOP_CAPTURE_WAIT_MS_PIR  3500
#define CORE_REC_STOP_SD_WAIT_MS      45000
#define CORE_REC_STOP_SD_WAIT_MS_PIR  60000
#define CORE_RING_PAUSE_PCT           65
#define CORE_H264_PSRAM_USE_PCT       40
#define CORE_H264_PSRAM_USE_PCT_PIR   CORE_H264_PSRAM_USE_PCT_PIR_ENC
#define CORE_H264_ENCODER_RESERVE_BYTES (512U * 1024U)
#define CORE_REC_CAPTURE_TASK_STACK   8192
#define CORE_REC_SD_TASK_STACK        10240
#define CORE_REC_CAPTURE_TASK_PRIO    4
#define CORE_REC_SD_TASK_PRIO         5
#define CORE_REC_PRIME_TIMEOUT_MS     4000
#define CORE_H264_PSRAM_FLUSH_PCT     95
#define CORE_H264_PSRAM_MIN_BYTES     (256U * 1024U)
#define CORE_PSRAM_TAIL_RESERVE_BYTES (128U * 1024U)
#define CORE_MUXER_RAM_CACHE_ENC_BYTES (256U * 1024U)
#define CORE_MUXER_RAM_CACHE_BYTES  CORE_MUXER_RAM_CACHE_ENC_BYTES
#define CORE_PSRAM_FLUSH_RESERVE_BYTES (CORE_MUXER_RAM_CACHE_ENC_BYTES + 64U * 1024U)
#define CORE_REC_FLUSH_PTS_GAP_MS     500
#define CORE_V4L2_BUF_COUNT         4
#define CORE_REC_ENCRYPT_IO_BYTES   (128U * 1024U)
#define CORE_ENC_WRITER_SCRATCH_BYTES (256U * 1024U)
#define CORE_ENC_SD_FILE_BUF_BYTES  (256U * 1024U)
#define CORE_ENC_WRITER_FLUSH_BYTES CORE_ENC_WRITER_SCRATCH_BYTES
#define CORE_ENC_FSYNC_BYTES        (8U * 1024U * 1024U)

#define CORE_REC_PLAIN_SD_BENCHMARK 0
#define CORE_REC_PLAIN_SD_FILE_BUF_BYTES (32U * 1024U)
#define CORE_MUXER_RAM_CACHE_PLAIN_BYTES (128U * 1024U)
#define CORE_CRYPTO_STREAM_IO_BYTES (64U * 1024U)
#define CORE_SD_FILE_BUF_BYTES      (32U * 1024U)
#define CORE_SD_PREP_TASK_STACK     4096
#define CORE_SD_PREP_TASK_PRIO      3
#define CORE_REC_ACQUIRE_TIMEOUT_MS 100
#define CORE_SD_BOOT_RETRY_COUNT    5
#define CORE_SD_BOOT_RETRY_MS       400
#define CORE_CONFIG_SD_SETTLE_MS    200
#define CORE_CONFIG_CAMERA_DELAY_MS 800
#define CORE_CONFIG_RECOVER_TMP_DELAY_MS 5000
#define CORE_REC_TMP_ZOMBIE_SEC          120
#define CORE_REC_TMP_SWEEP_MS            20000
#define CORE_SD_MOUNT_MAX_FILES     8

extern const uint8_t CORE_AUTH_SHA256[32];

#ifdef __cplusplus
extern "C" {
#endif
extern int64_t g_core_boot_us;
int64_t core_boot_elapsed_ms(void);
/* True when --pir / --rec-fast / MRBIN_REC_FAST: skip AE settle and late init. */
bool core_rec_fast_requested(void);
/* Log first H.264/JPEG encode (once). Budget is from the PIR pin decision
 * when /tmp/mrbin-pir-decision exists, otherwise from main(). */
void core_note_first_encoded_frame(const char *where, unsigned long nbytes);
/* First bytes handed to the MP4 file (once). */
void core_note_first_file_bytes(const char *where, unsigned long nbytes);
int64_t core_main_elapsed_ms(void);
int64_t core_since_pir_decision_ms(void);
void core_time_init(void);
#ifdef __cplusplus
}
#endif
