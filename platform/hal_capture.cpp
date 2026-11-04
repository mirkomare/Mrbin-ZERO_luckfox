#include "hal_capture.h"
#include "hal_system.h"
#include "hal_thread.h"
#include "CoreConfig.h"
#include "CoreDemo.h"
#include "CoreIspCal.h"
#include "CoreVideo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#endif
#endif

#ifdef LUCKFOX_RKMPI
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#ifndef F_OK
#define F_OK 0
#endif
#endif

typedef struct {
    int fmt;
    uint16_t w, h;
    uint8_t fps;
    uint32_t bitrate;
    int fd;
    uint8_t *last;
    int last_len;
    int running;
    int mjpeg;
#ifdef LUCKFOX_RKMPI
    int mpi;
    int chn;
    int keep_fps;
    int pipe_fps;
    int vir_w;
    int vir_h;
    int wrap_line;
    int preview_fps;
    uint32_t gop;
    VENC_STREAM_S st;
    VENC_PACK_S pack;
#endif
} cap_t;

static cap_t s_cap;
static int s_enc_reg;
static uint8_t *s_replay;
static int s_replay_n;
static uint32_t s_replay_pts;

static void replay_clear(void) {
    free(s_replay);
    s_replay = nullptr;
    s_replay_n = 0;
}

void esp_video_enc_register_default(void) { s_enc_reg = 1; }
void esp_video_enc_unregister_default(void) { s_enc_reg = 0; }

esp_capture_video_src_if_t *esp_capture_new_video_v4l2_src(const esp_capture_video_v4l2_src_cfg_t *cfg) {
    (void)cfg;
    return (esp_capture_video_src_if_t *)calloc(1, sizeof(esp_capture_video_src_if_t));
}

esp_capture_err_t esp_capture_open(const esp_capture_cfg_t *, esp_capture_handle_t *out) {
    replay_clear();
    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.fd = -1;
    if (out) *out = &s_cap;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t esp_capture_close(esp_capture_handle_t) {
#ifndef _WIN32
    if (s_cap.fd >= 0) close(s_cap.fd);
#endif
    replay_clear();
    free(s_cap.last);
    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.fd = -1;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t esp_capture_sink_setup(esp_capture_handle_t, int, const esp_capture_sink_cfg_t *cfg,
                                         esp_capture_sink_handle_t *out) {
    if (cfg) {
        s_cap.fmt = cfg->video_info.format_id;
        s_cap.w = cfg->video_info.width;
        s_cap.h = cfg->video_info.height;
        s_cap.fps = cfg->video_info.fps;
#ifdef LUCKFOX_RKMPI
        s_cap.keep_fps = (int)s_cap.fps;
#endif
        s_cap.mjpeg = (cfg->video_info.format_id == ESP_CAPTURE_FMT_ID_MJPEG);
        if (s_cap.bitrate == 0) {
            s_cap.bitrate = CORE_VIDEO_BITRATE_DEFAULT;
        }
    }
    if (out) *out = &s_cap;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t esp_capture_sink_enable(esp_capture_sink_handle_t, int) { return ESP_CAPTURE_ERR_OK; }
esp_capture_err_t esp_capture_sink_set_bitrate(esp_capture_sink_handle_t, int, uint32_t bps) {
    if (bps > 0) {
        s_cap.bitrate = bps;
    }
    return ESP_CAPTURE_ERR_OK;
}
esp_capture_err_t esp_capture_set_event_cb(esp_capture_handle_t, esp_capture_event_cb_t, void *) {
    return ESP_CAPTURE_ERR_OK;
}

#ifdef LUCKFOX_RKMPI
static cap_t *s_mpi_job_cap;
static volatile int s_mpi_job_done;
static volatile int s_mpi_job_ok;
static volatile int s_mpi_job_active;
static volatile int s_mpi_cancel;
static volatile int s_sys_up;
static volatile int s_prev_up;
static volatile int s_prev_luma_only;
static volatile int s_prev_hf = -1;
static volatile int s_prev_vf = -1;
static volatile int s_sub_up;
static volatile int s_live_up;
static volatile int s_sub_w;
static volatile int s_sub_h;
static volatile int s_sub_fps;
/* Mainpath left enabled so DEMO luma (ch1) is not stream-off'd on clip end. */
static volatile int s_main_left_up;
static volatile int s_main_left_w;
static volatile int s_main_left_h;
/* Defined with the stop-watch below. A size change may DisableChn from start. */
static volatile int s_mpi_stop_active;
static void rkmpi_arm_stop_watch(void);
static volatile int s_3a_up;
static volatile unsigned s_mpi_job_gen;
static pthread_mutex_t s_mpi_mu = PTHREAD_MUTEX_INITIALIZER;

static void rkmpi_vi_venc_stop(cap_t *c);

static char s_mpi_err[72];

static int rkmpi_fail(const char *what, int ret) {
    snprintf(s_mpi_err, sizeof(s_mpi_err), "%s ret=0x%x", what ? what : "RKMPI", (unsigned)ret);
    ESP_LOGE("hal_cap", "%s failed ret=0x%x", what, (unsigned)ret);
    FILE *fp = fopen("/tmp/mrbin-mpi-start.log", "a");
    if (fp) {
        fprintf(fp, "FAIL %s\n", s_mpi_err);
        fclose(fp);
    }
    return -1;
}

static int rkmpi_fail_release(cap_t *c, const char *what, int ret) {
    rkmpi_fail(what, ret);
    rkmpi_vi_venc_stop(c);
    return -1;
}

#define RKMPI_ALIGN16(x) (((x) + 15) & ~15)

static int rkmpi_rec_wants_3a(const cap_t *c) {
    /* H.264 rec. Live MJPEG uses live-pre. DEMO e PIR non aspettano rec-3a. */
    if (!c || c->mjpeg || core_rec_fast_requested() || core_demo_is_active()) {
        return 0;
    }
    return 1;
}

/* 3A is started before EnableChn. A 1200 ms sleep here sat on the Record
 * POST after stream-on. Recorder AE-discard covers the first kept frames. */

static int rkmpi_keep_to_pipe_fps(int keep_fps, int w, int h) {
    const int above_5m = ((uint32_t)w * (uint32_t)h > CORE_VIDEO_PIXELS_5M);
    if (keep_fps > (int)CORE_VIDEO_FPS_LOW_MAX) {
        if (above_5m && keep_fps > (int)CORE_VIDEO_FPS_ABOVE_5M) {
            return (int)CORE_VIDEO_FPS_ABOVE_5M;
        }
        return keep_fps;
    }
    /* 1–3 fps on VI starves AE/ISP. Hold a real sensor rate and drop in software.
     * IMX415 above 5 MP tops at 15; smaller sizes stay at the 1080p pipe rate. */
    if (above_5m) {
        return (int)CORE_VIDEO_FPS_ABOVE_5M;
    }
    return (int)CORE_VIDEO_FPS_DEFAULT;
}

/* Above 1080p a full NV12 plane (2304×1296 is already ~4.5 MB, 8 MP ~12 MB)
 * plus a second VI buffer and the VENC plane returns ENOMEM on ~24 MB CMA.
 * H.264 uses one VI buffer and the online wrap ring. */
static int rkmpi_above_1080p(int w, int h) {
    return ((uint32_t)w * (uint32_t)h) > (1920u * 1080u);
}

/* rkvenc online/wrap rejects enMirror ("online not support mirror") and
 * then every task times out with a zero-length packet. */
static void rkmpi_venc_mirror(VENC_CHN_ATTR_S *ea, int online) {
    (void)online;
    core_video_orient_via_sensor(1);
    ea->stVencAttr.enMirror = MIRROR_NONE;
    ESP_LOGI("hal_cap", "VENC senza mirror (orientamento sul sensore)");
}

/* MPP default lets QP reach 51. On motion the decoder copies whole
 * macroblocks and the picture looks like a sliding puzzle until the next
 * IDR. SetRcParam before StartRecvFrame is ignored, so call this after. */
static void rkmpi_h264_limit_qp(VENC_CHN ch, int big) {
    VENC_RC_PARAM_S p;
    memset(&p, 0, sizeof(p));
    (void)RK_MPI_VENC_GetRcParam(ch, &p);
    /* Max QP 26/22: above that, motion compensation copies whole tiles
     * and the picture looks like a puzzle until the next I-frame. */
    /* 8 MP at QP max 26 plus a re-encode never finished a frame
     * inside 66 ms (the file sat at ~1 fps). 36 is the project cap
     * and still stays clear of the QP 51 tile-copy look. */
    const RK_U32 mn = big ? 18U : 14U;
    const RK_U32 mx = big ? 36U : 22U;
    const RK_U32 imx = big ? 32U : 20U;
    p.s32FirstFrameStartQp = mn + 2U;
    p.stParamH264.u32StepQp = 1;
    p.stParamH264.u32MinQp = mn;
    p.stParamH264.u32MaxQp = mx;
    p.stParamH264.u32MinIQp = mn;
    p.stParamH264.u32MaxIQp = imx;
    p.stParamH264.u32FrmMinQp = mn + 1U;
    p.stParamH264.u32FrmMaxQp = mx;
    p.stParamH264.u32FrmMinIQp = mn + 1U;
    p.stParamH264.u32FrmMaxIQp = imx;
    p.stParamH264.s32DeltIpQp = 0;
    p.stParamH264.s32MaxReEncodeTimes = big ? 0 : 1;
    const int ret = RK_MPI_VENC_SetRcParam(ch, &p);
    if (ret != RK_SUCCESS) {
        ESP_LOGW("hal_cap", "VENC ch%d limite QP fallito ret=0x%x", (int)ch, (unsigned)ret);
    } else {
        ESP_LOGI("hal_cap", "VENC ch%d QP %u-%u", (int)ch, (unsigned)mn, (unsigned)mx);
    }
}

static int rkmpi_3k_wrap_line(int vir_h) {
    /* Sample default is H/4, then 16-aligned. Range is [128, H]. */
    int line = (vir_h / 4) & ~15;
    if (line < 128) {
        line = 128;
    }
    if (line > vir_h) {
        line = vir_h;
    }
    return line;
}

static int rkmpi_vi_venc_start(cap_t *c) {
    int w = c->w ? c->w : CORE_VIDEO_WIDTH_DEFAULT;
    int h = c->h ? c->h : CORE_VIDEO_HEIGHT_DEFAULT;
    if (w > CORE_VIDEO_ISP_PASS_W || h > CORE_VIDEO_ISP_PASS_H) {
        w = CORE_VIDEO_ISP_PASS_W;
        h = CORE_VIDEO_ISP_PASS_H;
    }
    w = core_video_align16((uint16_t)w, CORE_VIDEO_WIDTH_MAX);
    h = core_video_align16((uint16_t)h, CORE_VIDEO_HEIGHT_MAX);
    int keep_fps = c->keep_fps ? c->keep_fps : (c->fps ? (int)c->fps : CORE_VIDEO_FPS_DEFAULT);
    if (w > CORE_VIDEO_WIDTH_MAX) w = CORE_VIDEO_WIDTH_MAX;
    if (h > CORE_VIDEO_HEIGHT_MAX) h = CORE_VIDEO_HEIGHT_MAX;
    if (keep_fps < CORE_VIDEO_FPS_MIN) keep_fps = CORE_VIDEO_FPS_MIN;
    if (keep_fps > CORE_VIDEO_FPS_MAX) keep_fps = CORE_VIDEO_FPS_MAX;
    if ((uint32_t)w * (uint32_t)h > CORE_VIDEO_PIXELS_5M &&
        keep_fps > (int)CORE_VIDEO_FPS_ABOVE_5M) {
        keep_fps = (int)CORE_VIDEO_FPS_ABOVE_5M;
    }
    int fps = rkmpi_keep_to_pipe_fps(keep_fps, w, h);
    if (fps < CORE_VIDEO_FPS_MIN) fps = CORE_VIDEO_FPS_MIN;
    if (fps > CORE_VIDEO_FPS_MAX) fps = CORE_VIDEO_FPS_MAX;
    c->w = (uint16_t)w;
    c->h = (uint16_t)h;
    c->keep_fps = keep_fps;
    c->pipe_fps = fps;
    c->fps = (uint8_t)fps;
    c->chn = 0; /* 0=rkisp_mainpath /dev/video11 */
    s_mpi_err[0] = 0;
    int wrap_line = 0;
    {
        FILE *fp = fopen("/tmp/mrbin-mpi-start.log", "w");
        if (fp) {
            fprintf(fp, "open %dx%d keep %d pipe %d\n", w, h, keep_fps, fps);
            fclose(fp);
        }
    }
    int ret = RK_SUCCESS;
    if (!s_sys_up) {
        ret = RK_MPI_SYS_Init();
        if (ret != RK_SUCCESS) {
            return rkmpi_fail("RK_MPI_SYS_Init", ret);
        }
        s_sys_up = 1;
    }
    VI_DEV_ATTR_S da;
    VI_DEV_BIND_PIPE_S bp;
    memset(&da, 0, sizeof(da));
    memset(&bp, 0, sizeof(bp));
    ret = RK_MPI_VI_GetDevAttr(0, &da);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(0, &da);
        if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VI_SetDevAttr", ret);
    }
    if (RK_MPI_VI_GetDevIsEnable(0) != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(0);
        if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VI_EnableDev", ret);
        bp.u32Num = 1;
        bp.PipeId[0] = 0;
        ret = RK_MPI_VI_SetDevBindPipe(0, &bp);
        if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VI_SetDevBindPipe", ret);
    }
    c->vir_w = RKMPI_ALIGN16(w);
    c->vir_h = RKMPI_ALIGN16(h);

    VI_CHN_ATTR_S va;
    memset(&va, 0, sizeof(va));
    /* One VI buffer above 1080p. Two 8 MP planes make EnableChn
     * fail on this board even with the 100 MB heap. */
    const int above_1080 = rkmpi_above_1080p(w, h);
    /* Online wrap never finishes a picture on this IMX415: rkvenc times out
     * with vepu_cycle 0 and RTSP stays empty. One full VI buffer encodes. */
    const int h264_wrap = 0;
    const int scaled_hd = (w >= 1280 || h >= 720);
    va.stIspOpt.u32BufCount = above_1080 ? 1 : (scaled_hd ? 2 : 3);
    va.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    va.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE_MPLANE;
    va.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    strncpy(va.stIspOpt.aEntityName, CORE_VIDEO_RKISP_ENTITY,
            sizeof(va.stIspOpt.aEntityName) - 1);
    /* Full-frame max size forces the IMX415 into 3864×2192. The ISP
     * then read the whole 8 MP frame back twice and every preset,
     * including 1080p, stayed around 4–5 fps. 1080p and below ask
     * only for their own size, so the sensor takes the 1944×1097
     * binning mode at 30 fps. Larger presets still open the native
     * frame. */
    if (w <= 1920 && h <= 1088) {
        va.stIspOpt.stMaxSize.u32Width = (RK_U32)c->vir_w;
        va.stIspOpt.stMaxSize.u32Height = (RK_U32)c->vir_h;
    } else {
        va.stIspOpt.stMaxSize.u32Width = CORE_VIDEO_SENSOR_NATIVE_W;
        va.stIspOpt.stMaxSize.u32Height = CORE_VIDEO_SENSOR_NATIVE_H;
    }
    /* VI buffer stride must match VENC u32Vir* or MPP drops every frame
     * (dmesg: frame [1920,1080,1920,1080] vs prep [1920,1080,1920,1088]). */
    va.stSize.u32Width = (RK_U32)c->vir_w;
    va.stSize.u32Height = (RK_U32)c->vir_h;
    va.stFrameRate.s32SrcFrameRate = fps;
    va.stFrameRate.s32DstFrameRate = fps;
    va.enPixelFormat = RK_FMT_YUV420SP;
    va.enCompressMode = COMPRESS_MODE_NONE;
    va.u32Depth = 0;
    const int rec_3a = rkmpi_rec_wants_3a(c);
    /* SetChnAttr on a mainpath left enabled at another size stalls the ISP
     * and the GUI dies. DisableChn while selfpath/bypass is streaming does
     * not return, so refuse that case instead of hanging. */
    if (s_main_left_up &&
        (s_main_left_w != (int)c->vir_w || s_main_left_h != (int)c->vir_h)) {
        if (s_prev_up || s_sub_up || s_live_up) {
            ESP_LOGE("hal_cap",
                     "VI mainpath %dx%d ancora acceso, nuova %dx%d con un altro canale",
                     s_main_left_w, s_main_left_h, c->vir_w, c->vir_h);
            rkmpi_fail("VI mainpath busy", -1);
            return -1;
        }
        ESP_LOGI("hal_cap", "VI mainpath %dx%d -> %dx%d",
                 s_main_left_w, s_main_left_h, c->vir_w, c->vir_h);
        rkmpi_arm_stop_watch();
        (void)RK_MPI_VI_DisableChn(0, c->chn);
        s_mpi_stop_active = 0;
        s_main_left_up = 0;
        s_main_left_w = 0;
        s_main_left_h = 0;
    }
    if (s_main_left_up) {
        ESP_LOGI("hal_cap", "VI mainpath già %dx%d — salto SetChnAttr",
                 s_main_left_w, s_main_left_h);
        ret = RK_SUCCESS;
    } else {
        ret = RK_MPI_VI_SetChnAttr(0, c->chn, &va);
        if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VI_SetChnAttr rkisp_mainpath", ret);
    }
    /* Wrap before EnableChn: the encoder ring is wrap_line rows, not a full
     * NV12 plane. Picture size stays pic/vir (same stride as VENC). */
    if (h264_wrap) {
        wrap_line = rkmpi_3k_wrap_line(c->vir_h);
        VI_CHN_BUF_WRAP_S wr;
        memset(&wr, 0, sizeof(wr));
        wr.bEnable = RK_TRUE;
        wr.u32BufLine = (RK_U32)wrap_line;
        wr.u32WrapBufferSize = (RK_U32)wrap_line * (RK_U32)c->vir_w * 3U / 2U;
        ret = RK_MPI_VI_SetChnWrapBufAttr(0, c->chn, &wr);
        if (ret != RK_SUCCESS) {
            ESP_LOGW("hal_cap", "VI wrap %d lines failed ret=0x%x — continue with 1 VI buf",
                     wrap_line, (unsigned)ret);
            wrap_line = 0;
        } else {
            ESP_LOGI("hal_cap", "VI wrap line %d bytes %u vir %dx%d (VI bufs=1)",
                     wrap_line, (unsigned)wr.u32WrapBufferSize, c->vir_w, c->vir_h);
        }
    }
    c->wrap_line = wrap_line;
    /* Start 3A after VI is configured so it is waiting for the stream-on
     * event. EnableChn without 3A hits "params stream on timeout".
     * Starting 3A only after that timeout leaves the UV plane at 128
     * (3-component JPEG, but R=G=B). Live uses live-pre, not rec-3a. */
    if (s_mpi_cancel) {
        ESP_LOGW("hal_cap", "VI start annullato prima del 3A");
        rkmpi_vi_venc_stop(c);
        return -1;
    }
    if (!s_3a_up) {
        if ((core_rec_fast_requested() || core_demo_is_active()) && !c->mjpeg) {
            /* DEMO ha già il 3A della luma. Niente rec-3a: sono 8–12 s
             * e la clip si chiude prima del primo frame. */
            core_video_isp_pir_arm();
        } else if (rec_3a) {
            core_video_isp_rec_after_vi();
        } else if (c->mjpeg && !core_rec_fast_requested()) {
            core_video_isp_live_pre();
        }
        s_3a_up = 1;
    }
    if (s_mpi_cancel) {
        ESP_LOGW("hal_cap", "VI start annullato prima di EnableChn");
        rkmpi_vi_venc_stop(c);
        return -1;
    }
    if (s_main_left_up) {
        /* Clip precedente: niente DisableChn, il canale è ancora acceso. */
        ESP_LOGI("hal_cap", "VI mainpath già attivo — salto EnableChn");
        ret = RK_SUCCESS;
    } else {
        ret = RK_MPI_VI_EnableChn(0, c->chn);
        if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VI_EnableChn rkisp_mainpath", ret);
    }
    if (core_rec_fast_requested() && !c->mjpeg) {
        /* EnableChn resets SC3336 toward exposure ~122. Re-lock only
         * that dark value. 3A, if it is up, stays up through stream-off. */
        core_video_isp_pir_lock();
    }
    core_snap(rec_3a ? "vi enable rec" : (core_rec_fast_requested() ? "vi enable pir" : "vi enable live"));

    /* SetChnMirrorFlip a canale già acceso ferma i frame: la clip si chiude
     * a metà e al riavvio ne parte un'altra. I flag vanno sull'encoder. */
    core_video_reapply_flip();
    const int pipe_flip = 0;

    VENC_CHN_ATTR_S ea;
    memset(&ea, 0, sizeof(ea));
    RK_CODEC_ID_E enc = c->mjpeg ? RK_VIDEO_ID_MJPEG : RK_VIDEO_ID_AVC;
    RK_U32 bps = c->bitrate ? c->bitrate : (RK_U32)CORE_VIDEO_BITRATE_DEFAULT;
    if (bps < CORE_VIDEO_BITRATE_MIN) bps = CORE_VIDEO_BITRATE_MIN;
    if (bps > CORE_VIDEO_BITRATE_MAX) bps = CORE_VIDEO_BITRATE_MAX;
    RK_U32 kbps = bps / 1000U;
    if (kbps < 200U) kbps = 200U;
    if (enc == RK_VIDEO_ID_AVC) {
        ea.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        ea.stRcAttr.stH264Cbr.u32BitRate = kbps;
        ea.stRcAttr.stH264Cbr.u32Gop = (c->gop > 0) ? c->gop
            : ((keep_fps <= (int)CORE_VIDEO_FPS_LOW_MAX) ? 1U : (RK_U32)CORE_VIDEO_GOP_DEFAULT);
        ea.stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)fps;
        ea.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
        /* Timelapse only: VENC dst = keep 1–3. Normal 25/30: dst = pipe fps.
         * Setting dst=keep on a leftover 1–3 preset (or GOP=1 keep) capped every mode. */
        const RK_U32 venc_dst = (keep_fps <= (int)CORE_VIDEO_FPS_LOW_MAX)
                                    ? (RK_U32)keep_fps
                                    : (RK_U32)fps;
        ea.stRcAttr.stH264Cbr.fr32DstFrameRateNum = venc_dst;
        ea.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
        ea.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    } else {
        ea.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
        ea.stRcAttr.stMjpegCbr.u32BitRate = kbps;
        ea.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = (RK_U32)fps;
        ea.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
        /* Preview JPEG is a few fps. VI stays at the record pipe rate so a
         * later H.264 swap does not have to stream-off to change fps. */
        {
            int dst = fps;
            if (c->preview_fps > 0 && c->preview_fps < fps) {
                dst = c->preview_fps;
            }
            ea.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = (RK_U32)dst;
            ea.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;
        }
    }
    ea.stVencAttr.enType = enc;
    ea.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    ea.stVencAttr.u32PicWidth = (RK_U32)w;
    ea.stVencAttr.u32PicHeight = (RK_U32)h;
    ea.stVencAttr.u32VirWidth = (RK_U32)(c->vir_w > 0 ? c->vir_w : RKMPI_ALIGN16(w));
    ea.stVencAttr.u32VirHeight = (RK_U32)(c->vir_h > 0 ? c->vir_h : RKMPI_ALIGN16(h));
    ea.stVencAttr.u32StreamBufCnt = 2;
    if (above_1080) {
        /* A full NV12 plane above 1080p is the alloc
         * mpp_vcodec_chan_setup_hal_bufs rejected. Bitstream only. */
        RK_U32 stream = (RK_U32)w * (RK_U32)h / 2U;
        if (stream > (3072U * 1024U)) {
            stream = 3072U * 1024U;
        }
        if (stream < (512U * 1024U)) {
            stream = 512U * 1024U;
        }
        ea.stVencAttr.u32BufSize = stream;
        if (!c->mjpeg) {
            ea.stGopAttr.u32MaxLtrCount = 0;
        }
    } else {
        ea.stVencAttr.u32BufSize = (RK_U32)w * (RK_U32)h * 3 / 2;
    }
    if (pipe_flip) {
        ea.stVencAttr.enMirror = MIRROR_NONE;
    } else {
        rkmpi_venc_mirror(&ea, h264_wrap && wrap_line > 0);
    }
    ret = RK_MPI_VENC_CreateChn(0, &ea);
    if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VENC_CreateChn", ret);
    if (h264_wrap) {
        if (wrap_line > 0) {
            VENC_CHN_BUF_WRAP_S vw;
            memset(&vw, 0, sizeof(vw));
            vw.bEnable = RK_TRUE;
            vw.u32BufLine = (RK_U32)wrap_line;
            vw.u32WrapBufferSize = (RK_U32)wrap_line * (RK_U32)c->vir_w * 3U / 2U;
            ret = RK_MPI_VENC_SetChnBufWrapAttr(0, &vw);
            if (ret != RK_SUCCESS) {
                return rkmpi_fail_release(c, "RK_MPI_VENC_SetChnBufWrapAttr", ret);
            }
        }
        VENC_CHN_REF_BUF_SHARE_S share;
        memset(&share, 0, sizeof(share));
        share.bEnable = RK_TRUE;
        ret = RK_MPI_VENC_SetChnRefBufShareAttr(0, &share);
        if (ret != RK_SUCCESS) {
            ESP_LOGW("hal_cap", "VENC ref-buf share failed ret=0x%x", (unsigned)ret);
        }
        FILE *fp = fopen("/tmp/mrbin-mpi-start.log", "a");
        if (fp) {
            fprintf(fp, "wrap vir %dx%d vi_bufs 1 wrap %d stream %u\n",
                    c->vir_w, c->vir_h, wrap_line, (unsigned)ea.stVencAttr.u32BufSize);
            fclose(fp);
        }
    } else if (c->mjpeg && above_1080) {
        ESP_LOGI("hal_cap", "MJPEG sopra 1080p senza wrap, VI bufs=1");
        FILE *fp = fopen("/tmp/mrbin-mpi-start.log", "a");
        if (fp) {
            fprintf(fp, "mjpeg no wrap vir %dx%d stream %u\n",
                    c->vir_w, c->vir_h, (unsigned)ea.stVencAttr.u32BufSize);
            fclose(fp);
        }
    }
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof(rp));
    rp.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(0, &rp);
    if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_VENC_StartRecvFrame", ret);
    if (!c->mjpeg) {
        rkmpi_h264_limit_qp(0, above_1080);
    }

    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = c->chn;
    dst.enModId = RK_ID_VENC;
    ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) return rkmpi_fail_release(c, "RK_MPI_SYS_Bind VI->VENC", ret);
    memset(&c->st, 0, sizeof(c->st));
    memset(&c->pack, 0, sizeof(c->pack));
    c->st.pstPack = &c->pack;
    if (keep_fps != fps) {
        ESP_LOGI("hal_cap",
                 "RKMPI VI(%s)+VENC %dx%d@%d %s %ukbps (timelapse keep %d fps — pipe %d so AE/ISP settle)",
                 CORE_VIDEO_RKISP_ENTITY, w, h, fps, c->mjpeg ? "MJPEG" : "H264",
                 (unsigned)kbps, keep_fps, fps);
    } else {
        ESP_LOGI("hal_cap", "RKMPI VI(%s)+VENC %dx%d@%d %s %ukbps", CORE_VIDEO_RKISP_ENTITY, w, h,
                 fps, c->mjpeg ? "MJPEG" : "H264", (unsigned)kbps);
    }
    /* After isp.env CPROC 128, so a saved SC3336 calibration is the one that sticks. */
    (void)core_isp_cal_apply_active();
    return 0;
}

/* rockit SYS_Exit joins a thread blocked in select() on the 3A
 * stream-off event. If that join never returns, Live and Registra
 * stay wedged until the process is replaced. 8 s is far past a healthy
 * stream-off (~0.5 s). Exit so mrbin-core-watch can restore the camera. */

static void *rkmpi_stop_watch(void *) {
    for (int i = 0; i < 80; ++i) {
        if (!s_mpi_stop_active) {
            return nullptr;
        }
        usleep(100000);
    }
    if (s_mpi_stop_active) {
        fprintf(stderr, "mrbin: VI teardown stuck 8s — exit for watchdog\n");
        _exit(86);
    }
    return nullptr;
}

static void rkmpi_arm_stop_watch(void) {
    s_mpi_stop_active = 1;
    pthread_t th;
    if (pthread_create(&th, nullptr, rkmpi_stop_watch, nullptr) == 0) {
        pthread_detach(th);
    }
}

/* VI is already streaming. Drop only the MJPEG encoder and bind H.264.
 * DisableChn is the multi-second stream-off; this path does not call it
 * and does not stop 3A. */
static int rkmpi_venc_swap_h264(cap_t *c, int w, int h, int keep_fps, uint32_t bitrate, uint32_t gop) {
    if (!c || !c->mpi || !s_sys_up) {
        return -1;
    }
    int pipe = c->pipe_fps ? c->pipe_fps : rkmpi_keep_to_pipe_fps(keep_fps, w, h);
    if ((int)c->w != w || (int)c->h != h || c->pipe_fps != pipe) {
        return -1;
    }
    core_snap("venc swap begin");
    pthread_mutex_lock(&s_mpi_mu);
    c->running = 0;
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = c->chn;
    dst.enModId = RK_ID_VENC;
    (void)RK_MPI_SYS_UnBind(&src, &dst);
    (void)RK_MPI_VENC_StopRecvFrame(0);
    (void)RK_MPI_VENC_DestroyChn(0);

    c->mjpeg = 0;
    c->keep_fps = keep_fps;
    c->bitrate = bitrate ? bitrate : (uint32_t)CORE_VIDEO_BITRATE_DEFAULT;
    c->gop = gop;
    int fps = pipe;
    int wrap_line = c->wrap_line;
    const int above_1080 = rkmpi_above_1080p(w, h);

    VENC_CHN_ATTR_S ea;
    memset(&ea, 0, sizeof(ea));
    RK_U32 bps = c->bitrate;
    if (bps < CORE_VIDEO_BITRATE_MIN) bps = CORE_VIDEO_BITRATE_MIN;
    if (bps > CORE_VIDEO_BITRATE_MAX) bps = CORE_VIDEO_BITRATE_MAX;
    RK_U32 kbps = bps / 1000U;
    if (kbps < 200U) kbps = 200U;
    ea.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    ea.stRcAttr.stH264Cbr.u32BitRate = kbps;
    ea.stRcAttr.stH264Cbr.u32Gop = (c->gop > 0) ? c->gop
        : ((keep_fps <= (int)CORE_VIDEO_FPS_LOW_MAX) ? 1U : (RK_U32)CORE_VIDEO_GOP_DEFAULT);
    ea.stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    const RK_U32 venc_dst = (keep_fps <= (int)CORE_VIDEO_FPS_LOW_MAX)
                                ? (RK_U32)keep_fps
                                : (RK_U32)fps;
    ea.stRcAttr.stH264Cbr.fr32DstFrameRateNum = venc_dst;
    ea.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    ea.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    ea.stVencAttr.enType = RK_VIDEO_ID_AVC;
    ea.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    ea.stVencAttr.u32PicWidth = (RK_U32)w;
    ea.stVencAttr.u32PicHeight = (RK_U32)h;
    ea.stVencAttr.u32VirWidth = (RK_U32)(c->vir_w > 0 ? c->vir_w : RKMPI_ALIGN16(w));
    ea.stVencAttr.u32VirHeight = (RK_U32)(c->vir_h > 0 ? c->vir_h : RKMPI_ALIGN16(h));
    ea.stVencAttr.u32StreamBufCnt = 2;
    rkmpi_venc_mirror(&ea, wrap_line > 0);
    if (above_1080) {
        RK_U32 stream = (RK_U32)w * (RK_U32)h / 2U;
        if (stream > (3072U * 1024U)) stream = 3072U * 1024U;
        if (stream < (512U * 1024U)) stream = 512U * 1024U;
        ea.stVencAttr.u32BufSize = stream;
        ea.stGopAttr.u32MaxLtrCount = 0;
    } else {
        ea.stVencAttr.u32BufSize = (RK_U32)w * (RK_U32)h * 3 / 2;
    }
    int ret = RK_MPI_VENC_CreateChn(0, &ea);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        rkmpi_fail("RK_MPI_VENC_CreateChn swap", ret);
        rkmpi_vi_venc_stop(c);
        return -1;
    }
    if (wrap_line > 0) {
        VENC_CHN_BUF_WRAP_S vw;
        memset(&vw, 0, sizeof(vw));
        vw.bEnable = RK_TRUE;
        vw.u32BufLine = (RK_U32)wrap_line;
        vw.u32WrapBufferSize = (RK_U32)wrap_line * (RK_U32)c->vir_w * 3U / 2U;
        ret = RK_MPI_VENC_SetChnBufWrapAttr(0, &vw);
        if (ret != RK_SUCCESS) {
            pthread_mutex_unlock(&s_mpi_mu);
            rkmpi_fail("RK_MPI_VENC_SetChnBufWrapAttr swap", ret);
            rkmpi_vi_venc_stop(c);
            return -1;
        }
        VENC_CHN_REF_BUF_SHARE_S share;
        memset(&share, 0, sizeof(share));
        share.bEnable = RK_TRUE;
        (void)RK_MPI_VENC_SetChnRefBufShareAttr(0, &share);
    }
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof(rp));
    rp.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(0, &rp);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        rkmpi_fail("RK_MPI_VENC_StartRecvFrame swap", ret);
        rkmpi_vi_venc_stop(c);
        return -1;
    }
    rkmpi_h264_limit_qp(0, above_1080);
    ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        rkmpi_fail("RK_MPI_SYS_Bind swap", ret);
        rkmpi_vi_venc_stop(c);
        return -1;
    }
    memset(&c->st, 0, sizeof(c->st));
    memset(&c->pack, 0, sizeof(c->pack));
    c->st.pstPack = &c->pack;
    c->st.u32PackCount = 1;
    c->mpi = 1;
    c->running = 1;
    pthread_mutex_unlock(&s_mpi_mu);
    core_snap("venc h264");
    ESP_LOGI("hal_cap", "VENC swap MJPEG→H264 %dx%d pipe %d keep %d (VI left up)",
             w, h, fps, keep_fps);
    return 0;
}

static void rkmpi_vi_venc_stop(cap_t *c) {
    if (!c) {
        return;
    }
    pthread_mutex_lock(&s_mpi_mu);
    if (!c->mpi && !s_sys_up) {
        c->running = 0;
        pthread_mutex_unlock(&s_mpi_mu);
        return;
    }
    rkmpi_arm_stop_watch();
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = c->chn;
    dst.enModId = RK_ID_VENC;
    (void)RK_MPI_SYS_UnBind(&src, &dst);
    /* StopRecvFrame wakes a GetStream poll left behind by a short stop. */
    (void)RK_MPI_VENC_StopRecvFrame(0);
    (void)RK_MPI_VENC_DestroyChn(0);
    /* DisableChn on mainpath while another VI channel is streaming
     * does not return. The 8 s stop-watch then _exit's and the GUI dies. */
    if (s_prev_up || s_sub_up || s_live_up) {
        s_main_left_up = 1;
        s_main_left_w = (int)c->vir_w;
        s_main_left_h = (int)c->vir_h;
        ESP_LOGI("hal_cap", "VENC chiuso, VI mainpath lasciato su %dx%d",
                 s_main_left_w, s_main_left_h);
    } else {
        (void)RK_MPI_VI_DisableChn(0, c->chn);
        s_main_left_up = 0;
        s_main_left_w = 0;
        s_main_left_h = 0;
    }
    /* Preview, channel 2 and the local bypass share the VI device and SYS. */
    if (!s_prev_up && !s_sub_up && !s_live_up) {
        (void)RK_MPI_VI_DisableDev(0);
        (void)RK_MPI_SYS_Exit();
        s_sys_up = 0;
    }
    c->mpi = 0;
    c->running = 0;
    replay_clear();
    s_mpi_stop_active = 0;
    pthread_mutex_unlock(&s_mpi_mu);
}

static void rkmpi_start_task(void *) {
    const unsigned gen = s_mpi_job_gen;
    int ok = rkmpi_vi_venc_start(s_mpi_job_cap);
    if (gen != s_mpi_job_gen || s_mpi_cancel) {
        if (ok == 0 || s_sys_up) {
            rkmpi_vi_venc_stop(s_mpi_job_cap);
        }
        s_mpi_job_active = 0;
        vTaskDelete(nullptr);
        return;
    }
    s_mpi_job_ok = (ok == 0);
    s_mpi_job_done = 1;
    s_mpi_job_active = 0;
    vTaskDelete(nullptr);
}

static int rkmpi_vi_venc_start_timeout(cap_t *c, int timeout_ms) {
    if (s_mpi_job_active) {
        ESP_LOGW("hal_cap", "RKMPI init gia in corso — niente secondo avvio");
        return -1;
    }
    s_mpi_cancel = 0;
    s_mpi_job_cap = c;
    s_mpi_job_done = 0;
    s_mpi_job_ok = 0;
    s_mpi_job_active = 1;
    s_mpi_job_gen++;
    if (xTaskCreate(rkmpi_start_task, "mpi_init", 65536, nullptr, 5, nullptr) != pdPASS) {
        s_mpi_job_active = 0;
        return -1;
    }
    int waited = 0;
    while (!s_mpi_job_done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (!s_mpi_job_done) {
        s_mpi_cancel = 1;
        s_mpi_job_gen++;
        ESP_LOGW("hal_cap", "RKMPI init timeout %d ms — attendo la chiusura", timeout_ms);
        int extra = 0;
        while (s_mpi_job_active && extra < 4000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            extra += 50;
        }
        return -1;
    }
    return s_mpi_job_ok ? 0 : -1;
}

static void rkmpi_dev_ensure(void) {
    if (!s_sys_up) {
        if (RK_MPI_SYS_Init() == RK_SUCCESS) {
            s_sys_up = 1;
        }
    }
    VI_DEV_ATTR_S da;
    memset(&da, 0, sizeof(da));
    if (RK_MPI_VI_GetDevAttr(0, &da) == RK_ERR_VI_NOT_CONFIG) {
        (void)RK_MPI_VI_SetDevAttr(0, &da);
    }
    if (RK_MPI_VI_GetDevIsEnable(0) != RK_SUCCESS) {
        if (RK_MPI_VI_EnableDev(0) == RK_SUCCESS) {
            VI_DEV_BIND_PIPE_S bp;
            memset(&bp, 0, sizeof(bp));
            bp.u32Num = 1;
            bp.PipeId[0] = 0;
            (void)RK_MPI_VI_SetDevBindPipe(0, &bp);
        }
    }
}

int hal_preview_running(void) {
    return s_prev_up ? 1 : 0;
}

int hal_main_running(void) {
    return (s_cap.mpi && s_sys_up) ? 1 : 0;
}

void hal_mark_3a_down(void) {
    s_3a_up = 0;
}

void hal_vi_apply_flip(int chn) {
    VI_ISP_MIRROR_FLIP_S mf;
    memset(&mf, 0, sizeof(mf));
    mf.mirror = core_video_get_hflip() ? 1 : 0;
    mf.flip = core_video_get_vflip() ? 1 : 0;
    const int ret = RK_MPI_VI_SetChnMirrorFlip(0, chn, mf);
    if (ret == RK_SUCCESS) {
        ESP_LOGI("hal_cap", "VI ch%d pipeline specchia=%u capovolgi=%u",
                 chn, (unsigned)mf.mirror, (unsigned)mf.flip);
    } else if (mf.mirror || mf.flip) {
        ESP_LOGW("hal_cap", "VI SetChnMirrorFlip ch%d failed 0x%x",
                 chn, (unsigned)ret);
    }
}

void hal_preview_reapply_flip(void) {
    core_video_reapply_flip();
}

void hal_capture_request_idr(void) {
    if (s_cap.mpi && !s_cap.mjpeg) {
        (void)RK_MPI_VENC_RequestIDR(0, RK_FALSE);
    }
}

static int preview_want_hf(void) {
    return core_video_get_hflip() ? 1 : 0;
}

static int preview_want_vf(void) {
    return core_video_get_vflip() ? 1 : 0;
}

static void preview_note_mirror(int hf, int vf) {
    s_prev_hf = hf;
    s_prev_vf = vf;
}

int hal_preview_flip_matches(void) {
    if (!s_prev_up || s_prev_luma_only) {
        return 0;
    }
    return (s_prev_hf == preview_want_hf() && s_prev_vf == preview_want_vf()) ? 1 : 0;
}

int hal_preview_start(int w, int h, int fps, int vi_user_depth) {
    if (w < 160) {
        w = 160;
    }
    if (h < 120) {
        h = 120;
    }
    w &= ~1;
    h &= ~1;
    if (fps < 1) {
        fps = 5;
    }
    if (fps > 15) {
        fps = 15;
    }
    const int luma_only = vi_user_depth > 0;
    if (s_sub_up) {
        ESP_LOGI("hal_cap", "Anteprima: fermo il canale 2 per liberare il selfpath");
        hal_sub_stop();
    }
    if (s_prev_up) {
        const int mirror_ok = (s_prev_hf == preview_want_hf() && s_prev_vf == preview_want_vf());
        if (s_prev_luma_only == luma_only && (mirror_ok || luma_only)) {
            return 0;
        }
        hal_preview_stop_force();
    }
    pthread_mutex_lock(&s_mpi_mu);
    rkmpi_dev_ensure();
    if (!s_sys_up) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Preview: SYS non partito");
        return -1;
    }
    int started_3a = 0;
    if (!s_3a_up) {
        core_video_isp_live_pre();
        s_3a_up = 1;
        started_3a = 1;
    }
    VI_CHN_ATTR_S va;
    memset(&va, 0, sizeof(va));
    va.stIspOpt.u32BufCount = 2;
    va.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    va.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE_MPLANE;
    va.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    strncpy(va.stIspOpt.aEntityName, "rkisp_selfpath", sizeof(va.stIspOpt.aEntityName) - 1);
    va.stIspOpt.stMaxSize.u32Width = (RK_U32)w;
    va.stIspOpt.stMaxSize.u32Height = (RK_U32)h;
    va.stSize.u32Width = (RK_U32)w;
    va.stSize.u32Height = (RK_U32)h;
    va.stFrameRate.s32SrcFrameRate = fps;
    va.stFrameRate.s32DstFrameRate = fps;
    va.enPixelFormat = RK_FMT_YUV420SP;
    va.enCompressMode = COMPRESS_MODE_NONE;
    va.u32Depth = luma_only ? (RK_U32)vi_user_depth : 0;
    int ret = RK_MPI_VI_SetChnAttr(0, 1, &va);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Preview VI SetChnAttr ret=0x%x", (unsigned)ret);
        return -1;
    }
    ret = RK_MPI_VI_EnableChn(0, 1);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Preview VI EnableChn ret=0x%x", (unsigned)ret);
        return -1;
    }
    core_video_reapply_flip();
    if (luma_only) {
        s_prev_up = 1;
        s_prev_luma_only = 1;
        preview_note_mirror(preview_want_hf(), preview_want_vf());
        pthread_mutex_unlock(&s_mpi_mu);
        if (started_3a) {
            core_video_isp_live_after_vi();
        }
        (void)core_isp_cal_apply_active();
        hal_preview_reapply_flip();
        ESP_LOGI("hal_cap", "Preview luma selfpath %dx%d@%d (VI depth %d, no MJPEG)",
                 w, h, fps, vi_user_depth);
        return 0;
    }
    VENC_CHN_ATTR_S ea;
    memset(&ea, 0, sizeof(ea));
    ea.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
    ea.stRcAttr.stMjpegCbr.u32BitRate = 800;
    ea.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
    ea.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;
    ea.stVencAttr.enType = RK_VIDEO_ID_MJPEG;
    ea.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    ea.stVencAttr.u32PicWidth = (RK_U32)w;
    ea.stVencAttr.u32PicHeight = (RK_U32)h;
    ea.stVencAttr.u32VirWidth = (RK_U32)w;
    ea.stVencAttr.u32VirHeight = (RK_U32)h;
    ea.stVencAttr.u32StreamBufCnt = 2;
    ea.stVencAttr.u32BufSize = (RK_U32)w * (RK_U32)h;
    ea.stVencAttr.enMirror = MIRROR_NONE;
    preview_note_mirror(preview_want_hf(), preview_want_vf());
    ESP_LOGI("hal_cap", "Preview sensore specchia=%d capovolgi=%d",
             preview_want_hf(), preview_want_vf());
    ret = RK_MPI_VENC_CreateChn(1, &ea);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VI_DisableChn(0, 1);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Preview VENC CreateChn ret=0x%x", (unsigned)ret);
        return -1;
    }
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof(rp));
    rp.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(1, &rp);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VENC_DestroyChn(1);
        (void)RK_MPI_VI_DisableChn(0, 1);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Preview StartRecv ret=0x%x", (unsigned)ret);
        return -1;
    }
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = 1;
    dst.enModId = RK_ID_VENC;
    dst.s32ChnId = 1;
    ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VENC_StopRecvFrame(1);
        (void)RK_MPI_VENC_DestroyChn(1);
        (void)RK_MPI_VI_DisableChn(0, 1);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Preview Bind ret=0x%x", (unsigned)ret);
        return -1;
    }
    s_prev_up = 1;
    s_prev_luma_only = 0;
    pthread_mutex_unlock(&s_mpi_mu);
    if (started_3a) {
        core_video_isp_live_after_vi();
    }
    (void)core_isp_cal_apply_active();
    ESP_LOGI("hal_cap", "Preview MJPEG selfpath %dx%d@%d", w, h, fps);
    return 0;
}

int hal_preview_acquire_luma(uint8_t **y, int *w, int *h, int *stride, int wait_ms) {
    if (y) {
        *y = nullptr;
    }
    if (w) {
        *w = 0;
    }
    if (h) {
        *h = 0;
    }
    if (stride) {
        *stride = 0;
    }
    if (!s_prev_up || !s_prev_luma_only || !y || !w || !h || !stride) {
        return -1;
    }
    if (wait_ms < 0) {
        wait_ms = 0;
    }
    VIDEO_FRAME_INFO_S st;
    memset(&st, 0, sizeof(st));
    int ret = RK_MPI_VI_GetChnFrame(0, 1, &st, wait_ms > 0 ? wait_ms : 500);
    if (ret != RK_SUCCESS) {
        return -1;
    }
    const int fw = (int)st.stVFrame.u32Width;
    const int fh = (int)st.stVFrame.u32Height;
    const int st_stride = (int)st.stVFrame.u32VirWidth;
    if (fw < 32 || fh < 32 || st_stride < fw) {
        (void)RK_MPI_VI_ReleaseChnFrame(0, 1, &st);
        return -1;
    }
    void *p = RK_MPI_MB_Handle2VirAddr(st.stVFrame.pMbBlk);
    if (!p) {
        (void)RK_MPI_VI_ReleaseChnFrame(0, 1, &st);
        return -1;
    }
    const size_t ysz = (size_t)st_stride * (size_t)fh;
    uint8_t *copy = (uint8_t *)malloc(ysz);
    if (!copy) {
        (void)RK_MPI_VI_ReleaseChnFrame(0, 1, &st);
        return -1;
    }
    memcpy(copy, p, ysz);
    (void)RK_MPI_VI_ReleaseChnFrame(0, 1, &st);
    *y = copy;
    *w = fw;
    *h = fh;
    *stride = st_stride;
    return 0;
}

int hal_preview_acquire(uint8_t **data, int *len, int wait_ms) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    if (!s_prev_up || s_prev_luma_only || !data || !len) {
        return -1;
    }
    if (wait_ms < 0) {
        wait_ms = 0;
    }
    VENC_STREAM_S st;
    VENC_PACK_S pack;
    memset(&st, 0, sizeof(st));
    memset(&pack, 0, sizeof(pack));
    st.pstPack = &pack;
    st.u32PackCount = 1;
    int ret = RK_MPI_VENC_GetStream(1, &st, wait_ms > 0 ? wait_ms : 20);
    if (ret != RK_SUCCESS || !st.pstPack || st.pstPack->u32Len == 0) {
        if (ret == RK_SUCCESS) {
            (void)RK_MPI_VENC_ReleaseStream(1, &st);
        }
        return -1;
    }
    int n = (int)st.pstPack->u32Len;
    void *p = RK_MPI_MB_Handle2VirAddr(st.pstPack->pMbBlk);
    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy || !p) {
        free(copy);
        (void)RK_MPI_VENC_ReleaseStream(1, &st);
        return -1;
    }
    memcpy(copy, p, (size_t)n);
    (void)RK_MPI_VENC_ReleaseStream(1, &st);
    *data = copy;
    *len = n;
    return 0;
}

int hal_live_running(void) {
    return s_live_up ? 1 : 0;
}

int hal_live_start(int w, int h, int fps) {
    if (w < 160) {
        w = 160;
    }
    if (h < 120) {
        h = 120;
    }
    w &= ~1;
    h &= ~1;
    if (fps < 1) {
        fps = 5;
    }
    if (fps > 15) {
        fps = 15;
    }
    if (s_live_up) {
        return 0;
    }
    pthread_mutex_lock(&s_mpi_mu);
    rkmpi_dev_ensure();
    if (!s_sys_up) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Live bypass: SYS non partito");
        return -1;
    }
    int started_3a = 0;
    if (!s_3a_up) {
        core_video_isp_live_pre();
        s_3a_up = 1;
        started_3a = 1;
    }
    VI_CHN_ATTR_S va;
    memset(&va, 0, sizeof(va));
    va.stIspOpt.u32BufCount = 1;
    va.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    va.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE_MPLANE;
    va.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    strncpy(va.stIspOpt.aEntityName, "rkisp_bypasspath", sizeof(va.stIspOpt.aEntityName) - 1);
    va.stIspOpt.stMaxSize.u32Width = (RK_U32)w;
    va.stIspOpt.stMaxSize.u32Height = (RK_U32)h;
    va.stSize.u32Width = (RK_U32)w;
    va.stSize.u32Height = (RK_U32)h;
    va.stFrameRate.s32SrcFrameRate = fps;
    va.stFrameRate.s32DstFrameRate = fps;
    va.enPixelFormat = RK_FMT_YUV420SP;
    va.enCompressMode = COMPRESS_MODE_NONE;
    va.u32Depth = 0;
    int ret = RK_MPI_VI_SetChnAttr(0, 2, &va);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Live bypass SetChnAttr ret=0x%x", (unsigned)ret);
        return -1;
    }
    ret = RK_MPI_VI_EnableChn(0, 2);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Live bypass EnableChn ret=0x%x", (unsigned)ret);
        return -1;
    }
    core_video_reapply_flip();
    VENC_CHN_ATTR_S ea;
    memset(&ea, 0, sizeof(ea));
    ea.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
    ea.stRcAttr.stMjpegCbr.u32BitRate = 800;
    ea.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
    ea.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;
    ea.stVencAttr.enType = RK_VIDEO_ID_MJPEG;
    ea.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    ea.stVencAttr.u32PicWidth = (RK_U32)w;
    ea.stVencAttr.u32PicHeight = (RK_U32)h;
    ea.stVencAttr.u32VirWidth = (RK_U32)w;
    ea.stVencAttr.u32VirHeight = (RK_U32)h;
    ea.stVencAttr.u32StreamBufCnt = 2;
    ea.stVencAttr.u32BufSize = (RK_U32)w * (RK_U32)h;
    ea.stVencAttr.enMirror = MIRROR_NONE;
    ret = RK_MPI_VENC_CreateChn(3, &ea);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VI_DisableChn(0, 2);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Live bypass VENC ret=0x%x", (unsigned)ret);
        return -1;
    }
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof(rp));
    rp.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(3, &rp);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VENC_DestroyChn(3);
        (void)RK_MPI_VI_DisableChn(0, 2);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Live bypass StartRecv ret=0x%x", (unsigned)ret);
        return -1;
    }
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = 2;
    dst.enModId = RK_ID_VENC;
    dst.s32ChnId = 3;
    ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VENC_StopRecvFrame(3);
        (void)RK_MPI_VENC_DestroyChn(3);
        (void)RK_MPI_VI_DisableChn(0, 2);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Live bypass Bind ret=0x%x", (unsigned)ret);
        return -1;
    }
    s_live_up = 1;
    pthread_mutex_unlock(&s_mpi_mu);
    if (started_3a) {
        core_video_isp_live_after_vi();
    }
    (void)core_isp_cal_apply_active();
    ESP_LOGI("hal_cap", "Live MJPEG bypasspath %dx%d@%d", w, h, fps);
    return 0;
}

int hal_live_acquire(uint8_t **data, int *len, int wait_ms) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    if (!s_live_up || !data || !len) {
        return -1;
    }
    if (wait_ms < 0) {
        wait_ms = 0;
    }
    VENC_STREAM_S st;
    VENC_PACK_S pack;
    memset(&st, 0, sizeof(st));
    memset(&pack, 0, sizeof(pack));
    st.pstPack = &pack;
    st.u32PackCount = 1;
    int ret = RK_MPI_VENC_GetStream(3, &st, wait_ms > 0 ? wait_ms : 20);
    if (ret != RK_SUCCESS || !st.pstPack || st.pstPack->u32Len == 0) {
        if (ret == RK_SUCCESS) {
            (void)RK_MPI_VENC_ReleaseStream(3, &st);
        }
        return -1;
    }
    int n = (int)st.pstPack->u32Len;
    void *p = RK_MPI_MB_Handle2VirAddr(st.pstPack->pMbBlk);
    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy || !p) {
        free(copy);
        (void)RK_MPI_VENC_ReleaseStream(3, &st);
        return -1;
    }
    memcpy(copy, p, (size_t)n);
    (void)RK_MPI_VENC_ReleaseStream(3, &st);
    *data = copy;
    *len = n;
    return 0;
}

void hal_live_stop(void) {
    if (!s_live_up) {
        return;
    }
    pthread_mutex_lock(&s_mpi_mu);
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = 2;
    dst.enModId = RK_ID_VENC;
    dst.s32ChnId = 3;
    (void)RK_MPI_SYS_UnBind(&src, &dst);
    (void)RK_MPI_VENC_StopRecvFrame(3);
    (void)RK_MPI_VENC_DestroyChn(3);
    (void)RK_MPI_VI_DisableChn(0, 2);
    s_live_up = 0;
    int main_up = s_cap.mpi;
    if (s_main_left_up && !s_prev_up && !s_sub_up) {
        (void)RK_MPI_VI_DisableChn(0, 0);
        s_main_left_up = 0;
    }
    if (!main_up && !s_prev_up && !s_sub_up && s_sys_up) {
        (void)RK_MPI_VI_DisableDev(0);
        (void)RK_MPI_SYS_Exit();
        s_sys_up = 0;
    }
    pthread_mutex_unlock(&s_mpi_mu);
    if (!main_up && !s_prev_up && !s_sub_up) {
        core_video_isp_stop_3a();
        s_3a_up = 0;
        core_video_deinit();
    }
    ESP_LOGI("hal_cap", "Live bypass fermato");
}

int hal_sub_running(void) {
    return s_sub_up ? 1 : 0;
}

int hal_sub_start(int w, int h, int fps, uint32_t bitrate) {
    if (w < 160) {
        w = 160;
    }
    if (h < 120) {
        h = 120;
    }
    w &= ~1;
    h &= ~1;
    if (fps < 1) {
        fps = 15;
    }
    if (fps > 30) {
        fps = 30;
    }
    if (s_prev_up) {
        ESP_LOGW("hal_cap", "Canale 2: selfpath occupato dall'anteprima");
        return -1;
    }
    if (s_sub_up && s_sub_w == w && s_sub_h == h && s_sub_fps == fps) {
        return 0;
    }
    if (s_sub_up) {
        hal_sub_stop();
    }
    pthread_mutex_lock(&s_mpi_mu);
    rkmpi_dev_ensure();
    if (!s_sys_up) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Canale 2: SYS non partito");
        return -1;
    }
    VI_CHN_ATTR_S va;
    memset(&va, 0, sizeof(va));
    const int above = rkmpi_above_1080p(w, h);
    const int main_big = s_cap.mpi && rkmpi_above_1080p((int)s_cap.w, (int)s_cap.h);
    va.stIspOpt.u32BufCount = (above || main_big) ? 1 : 2;
    va.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    va.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE_MPLANE;
    va.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    strncpy(va.stIspOpt.aEntityName, "rkisp_selfpath", sizeof(va.stIspOpt.aEntityName) - 1);
    va.stIspOpt.stMaxSize.u32Width = (RK_U32)w;
    va.stIspOpt.stMaxSize.u32Height = (RK_U32)h;
    va.stSize.u32Width = (RK_U32)w;
    va.stSize.u32Height = (RK_U32)h;
    va.stFrameRate.s32SrcFrameRate = fps;
    va.stFrameRate.s32DstFrameRate = fps;
    va.enPixelFormat = RK_FMT_YUV420SP;
    va.enCompressMode = COMPRESS_MODE_NONE;
    va.u32Depth = 0;
    int ret = RK_MPI_VI_SetChnAttr(0, 1, &va);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Canale 2 VI SetChnAttr ret=0x%x", (unsigned)ret);
        return -1;
    }
    ret = RK_MPI_VI_EnableChn(0, 1);
    if (ret != RK_SUCCESS) {
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Canale 2 VI EnableChn ret=0x%x", (unsigned)ret);
        return -1;
    }
    VENC_CHN_ATTR_S ea;
    memset(&ea, 0, sizeof(ea));
    RK_U32 kbps = bitrate / 1000U;
    if (kbps < 200U) {
        kbps = 200U;
    }
    ea.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    ea.stRcAttr.stH264Cbr.u32BitRate = kbps;
    /* Short GOP so a coarse motion vector clears in about half a second. */
    RK_U32 sub_gop = (RK_U32)fps / 2U;
    if (sub_gop < 4U) {
        sub_gop = 4U;
    }
    ea.stRcAttr.stH264Cbr.u32Gop = sub_gop;
    ea.stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    ea.stRcAttr.stH264Cbr.fr32DstFrameRateNum = (RK_U32)fps;
    ea.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    ea.stVencAttr.enType = RK_VIDEO_ID_AVC;
    ea.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    ea.stVencAttr.u32PicWidth = (RK_U32)w;
    ea.stVencAttr.u32PicHeight = (RK_U32)h;
    ea.stVencAttr.u32VirWidth = (RK_U32)w;
    ea.stVencAttr.u32VirHeight = (RK_U32)h;
    ea.stVencAttr.u32StreamBufCnt = 2;
    ea.stVencAttr.u32BufSize = (RK_U32)w * (RK_U32)h;
    if (ea.stVencAttr.u32BufSize > (3072U * 1024U)) {
        ea.stVencAttr.u32BufSize = 3072U * 1024U;
    }
    ea.stVencAttr.enMirror = MIRROR_NONE;
    ret = RK_MPI_VENC_CreateChn(2, &ea);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VI_DisableChn(0, 1);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Canale 2 VENC CreateChn ret=0x%x", (unsigned)ret);
        return -1;
    }
    VENC_RECV_PIC_PARAM_S rp;
    memset(&rp, 0, sizeof(rp));
    rp.s32RecvPicNum = -1;
    ret = RK_MPI_VENC_StartRecvFrame(2, &rp);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VENC_DestroyChn(2);
        (void)RK_MPI_VI_DisableChn(0, 1);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Canale 2 StartRecv ret=0x%x", (unsigned)ret);
        return -1;
    }
    rkmpi_h264_limit_qp(2, above);
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = 1;
    dst.enModId = RK_ID_VENC;
    dst.s32ChnId = 2;
    ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) {
        (void)RK_MPI_VENC_StopRecvFrame(2);
        (void)RK_MPI_VENC_DestroyChn(2);
        (void)RK_MPI_VI_DisableChn(0, 1);
        pthread_mutex_unlock(&s_mpi_mu);
        ESP_LOGE("hal_cap", "Canale 2 Bind ret=0x%x", (unsigned)ret);
        return -1;
    }
    s_sub_w = w;
    s_sub_h = h;
    s_sub_fps = fps;
    s_sub_up = 1;
    pthread_mutex_unlock(&s_mpi_mu);
    ESP_LOGI("hal_cap", "Canale 2 H.264 selfpath %dx%d@%d %u kbps", w, h, fps, (unsigned)kbps);
    return 0;
}

int hal_sub_acquire(uint8_t **data, int *len, int wait_ms) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    if (!s_sub_up || !data || !len) {
        return -1;
    }
    if (wait_ms < 0) {
        wait_ms = 0;
    }
    VENC_STREAM_S st;
    VENC_PACK_S pack;
    memset(&st, 0, sizeof(st));
    memset(&pack, 0, sizeof(pack));
    st.pstPack = &pack;
    st.u32PackCount = 1;
    int ret = RK_MPI_VENC_GetStream(2, &st, wait_ms > 0 ? wait_ms : 20);
    if (ret != RK_SUCCESS || !st.pstPack || st.pstPack->u32Len == 0) {
        if (ret == RK_SUCCESS) {
            (void)RK_MPI_VENC_ReleaseStream(2, &st);
        }
        return -1;
    }
    int n = (int)st.pstPack->u32Len;
    void *p = RK_MPI_MB_Handle2VirAddr(st.pstPack->pMbBlk);
    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy || !p) {
        free(copy);
        (void)RK_MPI_VENC_ReleaseStream(2, &st);
        return -1;
    }
    memcpy(copy, p, (size_t)n);
    (void)RK_MPI_VENC_ReleaseStream(2, &st);
    *data = copy;
    *len = n;
    return 0;
}

void hal_sub_stop(void) {
    if (!s_sub_up) {
        return;
    }
    pthread_mutex_lock(&s_mpi_mu);
    MPP_CHN_S src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.enModId = RK_ID_VI;
    src.s32ChnId = 1;
    dst.enModId = RK_ID_VENC;
    dst.s32ChnId = 2;
    (void)RK_MPI_SYS_UnBind(&src, &dst);
    (void)RK_MPI_VENC_StopRecvFrame(2);
    (void)RK_MPI_VENC_DestroyChn(2);
    (void)RK_MPI_VI_DisableChn(0, 1);
    s_sub_up = 0;
    s_sub_w = 0;
    s_sub_h = 0;
    s_sub_fps = 0;
    int main_up = s_cap.mpi;
    if (!main_up && !s_prev_up && !s_live_up && s_sys_up) {
        (void)RK_MPI_VI_DisableDev(0);
        (void)RK_MPI_SYS_Exit();
        s_sys_up = 0;
    }
    pthread_mutex_unlock(&s_mpi_mu);
    ESP_LOGI("hal_cap", "Canale 2 fermato");
}

static void hal_preview_stop_impl(int force) {
    if (!s_prev_up && !s_sys_up) {
        return;
    }
    if (!force && s_prev_luma_only && core_demo_is_active()) {
        ESP_LOGW("hal_cap", "Preview luma DEMO: stop ignorato");
        return;
    }
    pthread_mutex_lock(&s_mpi_mu);
    if (s_prev_up) {
        if (!s_prev_luma_only) {
            MPP_CHN_S src, dst;
            memset(&src, 0, sizeof(src));
            memset(&dst, 0, sizeof(dst));
            src.enModId = RK_ID_VI;
            src.s32ChnId = 1;
            dst.enModId = RK_ID_VENC;
            dst.s32ChnId = 1;
            (void)RK_MPI_SYS_UnBind(&src, &dst);
            (void)RK_MPI_VENC_StopRecvFrame(1);
            (void)RK_MPI_VENC_DestroyChn(1);
        }
        (void)RK_MPI_VI_DisableChn(0, 1);
        s_prev_up = 0;
        s_prev_luma_only = 0;
        s_prev_hf = -1;
        s_prev_vf = -1;
    }
    if (s_main_left_up && !s_sub_up && !s_live_up) {
        (void)RK_MPI_VI_DisableChn(0, 0);
        s_main_left_up = 0;
    }
    int main_up = s_cap.mpi;
    if (!main_up && !s_sub_up && !s_live_up && s_sys_up) {
        (void)RK_MPI_VI_DisableDev(0);
        (void)RK_MPI_SYS_Exit();
        s_sys_up = 0;
    }
    pthread_mutex_unlock(&s_mpi_mu);
    if (!main_up && !s_sub_up && !s_live_up) {
        core_video_isp_stop_3a();
        s_3a_up = 0;
        core_video_deinit();
    }
    ESP_LOGI("hal_cap", "Preview fermata");
}

void hal_preview_stop(void) {
    hal_preview_stop_impl(0);
}

void hal_preview_stop_force(void) {
    hal_preview_stop_impl(1);
}
#endif

#ifdef LUCKFOX_RKMPI
void hal_capture_user_request(void) {
}
#endif

esp_capture_err_t esp_capture_start(esp_capture_handle_t) {
#ifdef LUCKFOX_RKMPI
    ESP_LOGI("hal_cap", "RKMPI open requested %dx%d@%d %s",
             (int)s_cap.w, (int)s_cap.h, (int)s_cap.fps, s_cap.mjpeg ? "MJPEG" : "H264");
    /* VI+VENC on rkisp_mainpath needs seconds, not 300 ms. */
    if (rkmpi_vi_venc_start_timeout(&s_cap, CORE_VIDEO_OPEN_TIMEOUT_MS) == 0) {
        s_cap.mpi = 1;
        s_cap.running = 1;
        s_cap.st.u32PackCount = 1;
        return ESP_CAPTURE_ERR_OK;
    }
    ESP_LOGE("hal_cap", "RKMPI VI+VENC FAILED %dx%d@%d %s — no silent 1280x960 fallback",
             (int)s_cap.w, (int)s_cap.h, (int)s_cap.fps, s_cap.mjpeg ? "MJPEG" : "H264");
    s_cap.running = 0;
    if (!s_mpi_job_active) {
        rkmpi_vi_venc_stop(&s_cap);
    }
    return -1;
#endif
    s_cap.running = 1;
    return ESP_CAPTURE_ERR_OK;
}

void esp_capture_set_preview_fps(int fps) {
#ifdef LUCKFOX_RKMPI
    s_cap.preview_fps = fps;
#else
    (void)fps;
#endif
}

int esp_capture_vi_matches(int w, int h, int keep_fps) {
#ifdef LUCKFOX_RKMPI
    if (!s_cap.mpi || !s_sys_up || !s_cap.mjpeg) {
        return 0;
    }
    const int pipe = rkmpi_keep_to_pipe_fps(keep_fps, w, h);
    if ((int)s_cap.w != w || (int)s_cap.h != h || s_cap.pipe_fps != pipe) {
        return 0;
    }
    return 1;
#else
    (void)w;
    (void)h;
    (void)keep_fps;
    return 0;
#endif
}

esp_capture_handle_t esp_capture_active_handle(void) {
#ifdef LUCKFOX_RKMPI
    if (s_cap.mpi || s_sys_up) {
        return &s_cap;
    }
    return nullptr;
#else
    return s_cap.running ? (esp_capture_handle_t)&s_cap : nullptr;
#endif
}

int esp_capture_retarget_h264(int w, int h, int keep_fps, uint32_t bitrate, uint32_t gop) {
#ifdef LUCKFOX_RKMPI
    return rkmpi_venc_swap_h264(&s_cap, w, h, keep_fps, bitrate, gop);
#else
    (void)w;
    (void)h;
    (void)keep_fps;
    (void)bitrate;
    (void)gop;
    return -1;
#endif
}

void esp_capture_replay_next(const uint8_t *data, int len, uint32_t pts) {
    replay_clear();
    if (!data || len <= 0) {
        return;
    }
    s_replay = (uint8_t *)malloc((size_t)len);
    if (!s_replay) {
        return;
    }
    memcpy(s_replay, data, (size_t)len);
    s_replay_n = len;
    s_replay_pts = pts;
}

const char *esp_capture_last_mpi_error(void) {
#ifdef LUCKFOX_RKMPI
    return s_mpi_err;
#else
    return "";
#endif
}

int esp_capture_mpi_busy(void) {
#ifdef LUCKFOX_RKMPI
    return (s_mpi_job_active || s_cap.mpi || s_sys_up || s_cap.running) ? 1 : 0;
#else
    return s_cap.running ? 1 : 0;
#endif
}

void esp_capture_release_leftover(void) {
#ifdef LUCKFOX_RKMPI
    s_mpi_cancel = 1;
    s_mpi_job_gen++;
    /* EnableChn owns the MPI lock. The start task stops itself when it
     * sees cancel. DisableChn from here would deadlock that wait. */
    if (s_mpi_job_active) {
        return;
    }
    rkmpi_vi_venc_stop(&s_cap);
#else
    s_cap.running = 0;
#endif
}

esp_capture_err_t esp_capture_stop(esp_capture_handle_t) {
    s_cap.running = 0;
#ifdef LUCKFOX_RKMPI
    rkmpi_vi_venc_stop(&s_cap);
#endif
#ifndef _WIN32
    if (s_cap.fd >= 0) {
        close(s_cap.fd);
        s_cap.fd = -1;
    }
#endif
    return ESP_CAPTURE_ERR_OK;
}

/* Minimal Annex-B IDR+SPS/PPS so muxer/recorder paths stay alive in HOST_SIM. */
static const uint8_t k_fake_h264[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0x8d, 0x68, 0x1e, 0x03,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10, 0xff, 0xff, 0xf0,
};

static const uint8_t k_fake_jpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
    0x09, 0x08, 0x0a, 0x0c, 0x14, 0x0d, 0x0c, 0x0b, 0x0b, 0x0c, 0x19, 0x12,
    0x13, 0x0f, 0x14, 0x1d, 0x1a, 0x1f, 0x1e, 0x1d, 0x1a, 0x1c, 0x1c, 0x20,
    0x24, 0x2e, 0x27, 0x20, 0x22, 0x2c, 0x23, 0x1c, 0x1c, 0x28, 0x37, 0x29,
    0x2c, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1f, 0x27, 0x39, 0x3d, 0x38, 0x32,
    0x3c, 0x2e, 0x33, 0x34, 0x32, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x10,
    0x00, 0x10, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00,
    0x3f, 0x00, 0x7f, 0xaa, 0xff, 0xd9,
};

esp_capture_err_t esp_capture_sink_acquire_frame(esp_capture_sink_handle_t, esp_capture_stream_frame_t *f,
                                                 bool no_wait) {
    if (!f || !s_cap.running) return -1;
#ifdef LUCKFOX_RKMPI
    if (s_cap.mpi) {
        /* no_wait: Stop/Live abort must not block 200 ms per empty poll.
         * A full drain budget was spent here while VI was still producing. */
        int wait_ms = 200;
        if (no_wait) {
            wait_ms = 30;
        } else if (s_cap.keep_fps > 0 && s_cap.keep_fps <= (int)CORE_VIDEO_FPS_LOW_MAX) {
            wait_ms = 1500;
        }
        if (s_replay && s_replay_n > 0) {
            free(s_cap.last);
            s_cap.last = s_replay;
            s_cap.last_len = s_replay_n;
            s_replay = nullptr;
            s_replay_n = 0;
            f->data = s_cap.last;
            f->size = s_cap.last_len;
            f->stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO;
            f->pts = s_replay_pts;
            return ESP_CAPTURE_ERR_OK;
        }
        s_cap.st.u32PackCount = 1;
        if (!s_cap.st.pstPack) {
            s_cap.st.pstPack = &s_cap.pack;
        }
        int ret = RK_MPI_VENC_GetStream(0, &s_cap.st, wait_ms);
        if (ret == RK_SUCCESS && s_cap.st.pstPack) {
            void *p = RK_MPI_MB_Handle2VirAddr(s_cap.st.pstPack->pMbBlk);
            int n = (int)s_cap.st.pstPack->u32Len;
            if (p && n > 0) {
                free(s_cap.last);
                s_cap.last = (uint8_t *)malloc((size_t)n);
                if (!s_cap.last) {
                    (void)RK_MPI_VENC_ReleaseStream(0, &s_cap.st);
                    return -1;
                }
                memcpy(s_cap.last, p, (size_t)n);
                s_cap.last_len = n;
                f->data = s_cap.last;
                f->size = n;
                f->stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO;
                f->pts = (uint32_t)s_cap.st.pstPack->u64PTS;
                core_note_first_encoded_frame("rkmpi", (unsigned long)n);
                (void)RK_MPI_VENC_ReleaseStream(0, &s_cap.st);
                return ESP_CAPTURE_ERR_OK;
            }
            (void)RK_MPI_VENC_ReleaseStream(0, &s_cap.st);
        }
        return -1;
    }
#endif
    const uint8_t *src = s_cap.mjpeg ? k_fake_jpeg : k_fake_h264;
    int n = s_cap.mjpeg ? (int)sizeof(k_fake_jpeg) : (int)sizeof(k_fake_h264);
    free(s_cap.last);
    s_cap.last = (uint8_t *)malloc((size_t)n);
    if (!s_cap.last) return -1;
    memcpy(s_cap.last, src, (size_t)n);
    s_cap.last_len = n;
    f->data = s_cap.last;
    f->size = n;
    f->stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t esp_capture_sink_release_frame(esp_capture_sink_handle_t, esp_capture_stream_frame_t *) {
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_err_t esp_capture_sink_add_overlay(esp_capture_sink_handle_t, esp_capture_overlay_if_t *) {
    return ESP_CAPTURE_ERR_OK;
}
esp_capture_err_t esp_capture_sink_enable_overlay(esp_capture_sink_handle_t, bool) {
    return ESP_CAPTURE_ERR_OK;
}
esp_capture_err_t esp_capture_sink_get_element_by_tag(esp_capture_sink_handle_t, int, const char *,
                                                      esp_gmf_element_handle_t *el) {
    if (el) *el = nullptr;
    return -1;
}

int esp_gmf_video_ppa_set_cropped_rgn(esp_gmf_element_handle_t, const esp_gmf_video_rgn_t *) {
    return ESP_GMF_ERR_OK;
}
int esp_gmf_video_fps_cvt_set_fps(esp_gmf_element_handle_t, uint8_t) { return ESP_GMF_ERR_OK; }
int esp_gmf_video_enc_set_bitrate(esp_gmf_element_handle_t, uint32_t) { return ESP_GMF_ERR_OK; }
int esp_gmf_video_enc_set_gop(esp_gmf_element_handle_t, uint32_t gop) {
#ifdef LUCKFOX_RKMPI
    if (gop > 0) {
        s_cap.gop = gop;
    }
#else
    (void)gop;
#endif
    return ESP_GMF_ERR_OK;
}
int esp_gmf_video_enc_set_qp(esp_gmf_element_handle_t, uint32_t, uint32_t) {
    return ESP_GMF_ERR_OK;
}

static int ov_open(esp_capture_overlay_if_t *) { return ESP_CAPTURE_ERR_OK; }
static void ov_close(esp_capture_overlay_if_t *s) { free(s); }
static void ov_alpha(esp_capture_overlay_if_t *, uint8_t) {}

esp_capture_overlay_if_t *esp_capture_new_text_overlay(const esp_capture_rgn_t *) {
    esp_capture_overlay_if_t *o = (esp_capture_overlay_if_t *)calloc(1, sizeof(*o));
    o->open = ov_open;
    o->close = ov_close;
    o->set_alpha = ov_alpha;
    return o;
}
esp_capture_err_t esp_capture_text_overlay_draw_start(esp_capture_overlay_if_t *) {
    return ESP_CAPTURE_ERR_OK;
}
esp_capture_err_t esp_capture_text_overlay_draw_finished(esp_capture_overlay_if_t *) {
    return ESP_CAPTURE_ERR_OK;
}
esp_capture_err_t esp_capture_text_overlay_clear(esp_capture_overlay_if_t *, const esp_capture_rgn_t *,
                                                 uint16_t) {
    return ESP_CAPTURE_ERR_OK;
}
esp_capture_err_t esp_capture_text_overlay_draw_text_fmt(esp_capture_overlay_if_t *,
                                                         const esp_capture_text_overlay_draw_info_t *,
                                                         const char *, ...) {
    return ESP_CAPTURE_ERR_OK;
}

#ifndef LUCKFOX_RKMPI
int hal_preview_running(void) { return 0; }
int hal_main_running(void) { return 0; }
void hal_mark_3a_down(void) {}
void hal_capture_request_idr(void) {}
int hal_preview_flip_matches(void) { return 0; }
int hal_preview_start(int, int, int, int) { return -1; }
void hal_preview_reapply_flip(void) {}
void hal_vi_apply_flip(int) {}
int hal_preview_acquire_luma(uint8_t **y, int *w, int *h, int *stride, int) {
    if (y) {
        *y = nullptr;
    }
    if (w) {
        *w = 0;
    }
    if (h) {
        *h = 0;
    }
    if (stride) {
        *stride = 0;
    }
    return -1;
}
int hal_preview_acquire(uint8_t **data, int *len, int) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    return -1;
}
int hal_sub_start(int, int, int, uint32_t) { return -1; }
int hal_sub_acquire(uint8_t **data, int *len, int) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    return -1;
}
void hal_sub_stop(void) {}
int hal_sub_running(void) { return 0; }
int hal_live_start(int, int, int) { return -1; }
int hal_live_acquire(uint8_t **data, int *len, int) {
    if (data) {
        *data = nullptr;
    }
    if (len) {
        *len = 0;
    }
    return -1;
}
void hal_live_stop(void) {}
int hal_live_running(void) { return 0; }
void hal_preview_stop(void) {}
void hal_preview_stop_force(void) {}
#endif
