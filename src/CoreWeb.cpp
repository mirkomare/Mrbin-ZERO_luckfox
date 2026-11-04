#include "CoreWeb.h"
#include "CoreAuth.h"
#include "CoreClipQueue.h"
#include "CoreConfig.h"
#include "CoreDemo.h"
#include "CoreCrypto.h"
#include "CoreLive.h"
#include "CoreRtsp.h"
#include "CoreMrbinId.h"
#include "CoreRecorder.h"
#include "CoreSD.h"
#include "CoreSettings.h"
#include "CoreStatusLed.h"
#include "CoreVideo.h"
#include "CoreGPIO.h"
#include "CoreIspCal.h"
#include "CoreRtc.h"
#include "CoreAlarmSchedule.h"
#include "CoreTime.h"
#include "hal_net.h"

#include "dirent.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core_web";

#ifndef MRBIN_VERSION
#define MRBIN_VERSION "unknown"
#endif

#define CORE_CRYPTO_FILE_HEADER  20   // MRBI (4) + IV (16)
static httpd_handle_t s_server = nullptr;
static core_settings_t *s_settings = nullptr;
static bool s_boot_error_page = false;
static core_gpio_boot_snapshot_t s_boot_error_snap = {};
static bool s_wifi_events_registered = false;
static core_isp_cal_t s_isp_cal;

#define VIDEO_DL_TEMP_DIR       ".web_dl"
#define VIDEO_DL_IO_CHUNK       (128U * 1024U)   // PSRAM: poche syscall SD, AES a blocchi grandi

typedef struct {
    volatile bool running;
    volatile bool done;
    volatile bool failed;
    volatile int done_frames;
    volatile int total_frames;
    char base[128];
    char out_rel[128];
    char error[96];
    TaskHandle_t task;
} photo_export_job_t;

typedef struct {
    volatile bool running;
    volatile bool done;
    volatile bool failed;
    volatile uint64_t done_bytes;
    volatile uint64_t total_bytes;
    char src_rel[128];
    char out_rel[160];
    char dl_name[96];
    char error[96];
    TaskHandle_t task;
} video_download_job_t;

static photo_export_job_t s_photo_export = {};
static video_download_job_t s_video_download = {};

static bool web_heavy_io_busy(void) {
    return s_photo_export.running || s_video_download.running;
}

static const char *gpio_level_label(int level) {
    return level ? "HIGH" : "LOW";
}

// Confronta orario sistema vs DS3231 (lettura immediata dal chip).
static void web_clocks_html(char *out, size_t out_len, bool link_rtc_page) {
    if (!out || out_len == 0) {
        return;
    }
    char sys[48];
    char rtc[64];
    core_time_format_local(sys, sizeof(sys));
    core_rtc_format_local(rtc, sizeof(rtc));

    const char *sync = "—";
    const char *cls = "sd-info";
    struct tm sys_tm = {};
    struct tm rtc_tm = {};
    if (core_time_get_local_tm(&sys_tm) && core_rtc_get_local_tm(&rtc_tm) &&
        (rtc_tm.tm_year + 1900) >= 2024) {
        time_t ts = mktime(&sys_tm);
        time_t tr = mktime(&rtc_tm);
        if (ts != (time_t)-1 && tr != (time_t)-1) {
            long diff = (long)(ts - tr);
            if (diff < 0) {
                diff = -diff;
            }
            if (diff <= 2) {
                sync = "sincronizzati";
                cls = "sd-info";
            } else {
                sync = "NON sincronizzati";
                cls = "sd-info warn";
            }
        }
    } else if (core_rtc_is_present()) {
        sync = "RTC da aggiornare";
        cls = "sd-info warn";
    }

    snprintf(out, out_len,
             "<div class='%s'>"
             "<div><strong>Ora sistema:</strong> %s</div>"
             "<div><strong>Ora RTC:</strong> %s</div>"
             "<div class='hint' style='margin:6px 0 0'>Stato: <strong>%s</strong>"
             "%s</div></div>",
             cls, sys, rtc, sync,
             (link_rtc_page && core_alarm_schedule_hw_available())
                 ? " — <a href='/schedule'>Pagina RTC</a>"
                 : "");
}

// Nav dinamica: link RTC solo se DS3231 + AT24C32 rispondono.
static const char *web_nav(void) {
    static char nav[4096];
    const char *hw_link = core_rtc_hw_status_monitoring_enabled()
                              ? "<a href=\"/hw_status\">Stato HW</a>"
                              : "";
    const char *demo = core_demo_html_banner();
    if (core_alarm_schedule_hw_available()) {
        snprintf(nav, sizeof(nav),
                 "%s<nav><a href=\"/settings\">Impostazioni</a><a href=\"/wifi\">WiFi</a><a href=\"/live\">Live</a>"
                 "<a href=\"/calibrate\">Calibrazione</a><a href=\"/schedule\">RTC</a>"
                 "%s"
                 "<a href=\"/mrbin\">MrBin</a>"
                 "<a href=\"/rec_video\">Reg. video</a><a href=\"/channels\">Canali Video</a>"
                 "<a href=\"/videos\">Video</a>"
                 "<a href=\"/photos\">Foto/Timelapse</a><a href=\"/logout\">Logout</a></nav>",
                 demo, hw_link);
    } else {
        snprintf(nav, sizeof(nav),
                 "%s<nav><a href=\"/settings\">Impostazioni</a><a href=\"/wifi\">WiFi</a><a href=\"/live\">Live</a>"
                 "<a href=\"/calibrate\">Calibrazione</a>"
                 "%s"
                 "<a href=\"/mrbin\">MrBin</a>"
                 "<a href=\"/rec_video\">Reg. video</a><a href=\"/channels\">Canali Video</a><a href=\"/videos\">Video</a>"
                 "<a href=\"/photos\">Foto/Timelapse</a><a href=\"/logout\">Logout</a></nav>",
                 demo, hw_link);
    }
    return nav;
}

static void web_html_escape(const char *in, char *out, size_t out_len) {
    if (!out_len) {
        return;
    }
    if (!in) {
        out[0] = 0;
        return;
    }
    size_t j = 0;
    for (; *in && j + 1 < out_len; ++in) {
        const char *rep = nullptr;
        char single[2];
        if (*in == '&') {
            rep = "&amp;";
        } else if (*in == '<') {
            rep = "&lt;";
        } else if (*in == '>') {
            rep = "&gt;";
        } else if (*in == '"') {
            rep = "&quot;";
        } else if ((unsigned char)*in < 32) {
            continue;
        } else {
            single[0] = *in;
            single[1] = 0;
            rep = single;
        }
        for (const char *r = rep; *r && j + 1 < out_len; ++r) {
            out[j++] = *r;
        }
    }
    out[j] = 0;
}

static bool web_read_version_file(const char *path, char *out, size_t out_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    out[0] = 0;
    if (fgets(out, (int)out_len, fp)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' ||
                         out[n - 1] == '\t')) {
            out[--n] = 0;
        }
        char *start = out;
        while (*start == ' ' || *start == '\t') {
            ++start;
        }
        if (start != out) {
            memmove(out, start, strlen(start) + 1);
        }
    }
    fclose(fp);
    return out[0] != 0;
}

static void web_firmware_release_raw(char *out, size_t out_len) {
    out[0] = 0;
    if (!out_len) {
        return;
    }
    /* Ramdisk /etc stays at the flashed image. A core replaced on userdata
     * publishes its release here so the corner matches without a reflash. */
    if (!web_read_version_file("/userdata/mrbin/release", out, out_len) &&
        !web_read_version_file("/etc/mrbin-version", out, out_len)) {
        strncpy(out, MRBIN_VERSION, out_len - 1);
        out[out_len - 1] = 0;
    }
}

static void web_firmware_release_html(char *out, size_t out_len) {
    char raw[64];
    web_firmware_release_raw(raw, sizeof(raw));
    web_html_escape(raw, out, out_len);
}

static bool is_authed(httpd_req_t *req) {
    char cookie[512];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    char token[80] = {0};
    const char *p = strstr(cookie, "MRBIN_SESS=");
    if (!p) return false;
    p += 11;
    size_t i = 0;
    while (*p && *p != ';' && i < sizeof(token) - 1) token[i++] = *p++;
    return core_auth_validate_session_token(token);
}

static esp_err_t send_redirect(httpd_req_t *req, const char *loc) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, nullptr, 0);
}

static const char *LOGIN_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>MrBin CORE</title>
<style>body{font-family:sans-serif;max-width:420px;margin:40px auto}input{display:block;width:100%;margin:8px 0;padding:8px}button{padding:10px 16px}
.login-err{color:#c00;font-weight:bold;margin:0 0 16px}</style>
</head><body><h2>MrBin CORE</h2>
<form method="POST" action="/login"><label>User</label><input name="user" required>
<label>Password</label><input name="pass" type="password" required>
<button type="submit">Login</button></form></body></html>
)HTML";

static const char *LOGIN_ERR_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>MrBin CORE</title>
<style>body{font-family:sans-serif;max-width:420px;margin:40px auto}input{display:block;width:100%;margin:8px 0;padding:8px}button{padding:10px 16px}
.login-err{color:#c00;font-weight:bold;margin:0 0 16px}</style>
</head><body><h2>MrBin CORE</h2>
<p class="login-err">Login fallito: user o password non validi</p>
<form method="POST" action="/login"><label>User</label><input name="user" required>
<label>Password</label><input name="pass" type="password" required>
<button type="submit">Login</button></form></body></html>
)HTML";

static const char *SETTINGS_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Impostazioni CORE</title>
<style>
body{font-family:sans-serif;max-width:640px;margin:24px auto}
input,button,select{padding:8px;margin:4px 0;width:100%;box-sizing:border-box}
nav a{margin-right:12px}
fieldset.gpio-cfg{border:1px solid #ccc;border-radius:8px;padding:12px 16px;margin:16px 0}
fieldset.gpio-cfg legend{font-weight:600;padding:0 6px}
.hint{font-size:14px;color:#555;margin:0 0 10px;line-height:1.45}
.sd-info{padding:10px 12px;margin:12px 0;border-radius:6px;background:#eef3ff;border:1px solid #c5d4f5}
.sd-info.warn{background:#fff8e6;border-color:#f0d080}
.alert{padding:12px 14px;margin:12px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
.overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:1000;align-items:center;justify-content:center}
.overlay.show{display:flex}
.overlay-box{background:#fff;padding:28px 32px;border-radius:10px;text-align:center;max-width:320px;box-shadow:0 8px 32px rgba(0,0,0,.2)}
.spinner{width:42px;height:42px;margin:0 auto 16px;border:4px solid #ddd;border-top-color:#c33;border-radius:50%;animation:spin .9s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.fmt-btn{background:#c33;color:#fff}
.fmt-btn:disabled{opacity:.6;cursor:wait}
.sd-actions{display:flex;gap:8px;margin:12px 0}
.sd-actions form{flex:1;margin:0}
.sd-actions button{width:100%}
.remount-btn{background:#2266cc;color:#fff}
.page-head{display:flex;justify-content:space-between;align-items:baseline;gap:12px;margin:0 0 8px}
.page-head h2{margin:0;flex:1}
.fw-release{font-size:13px;color:#666;white-space:nowrap;font-weight:normal}
.demo-badge{position:fixed;top:10px;right:10px;z-index:2000;background:#e65100;color:#fff;font-weight:700;padding:6px 14px;border-radius:6px;font-size:14px;letter-spacing:.06em;box-shadow:0 2px 10px rgba(0,0,0,.25)}
</style></head><body>
)HTML";

static const char *FORMAT_OVERLAY_HTML = R"HTML(
<div id="fmtOverlay" class="overlay" aria-live="polite">
<div class="overlay-box"><div class="spinner"></div><strong>Formattazione SD in corso…</strong>
<p style="margin:8px 0 0;font-size:14px;color:#555">Non chiudere la pagina.<br>Operazione di solito 10–60 s.</p></div></div>
<script>
function startFormat(e){
  if(!confirm('Formattare SD? Tutti i video verranno eliminati. Se non montata, verrà preparata in FAT32.')){e.preventDefault();return false;}
  document.getElementById('fmtOverlay').classList.add('show');
  var b=document.getElementById('fmtBtn'); if(b){b.disabled=true;}
  return true;
}
</script>
)HTML";

static const char *FORMAT_RESULT_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Formattazione SD</title>
<style>body{font-family:sans-serif;max-width:640px;margin:24px auto}
.alert{padding:14px 16px;margin:16px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
a.btn{display:inline-block;margin-top:12px;padding:10px 16px;background:#2266cc;color:#fff;text-decoration:none;border-radius:6px}
</style></head><body>
<h2>Formattazione SD</h2>
)HTML";

static const char *SCRUB_JS = R"JS(
function isImageMostlyBlack(src,threshold){
  if(!src)return true;
  var th=threshold||28,w,h,ctx,data;
  if(src.getContext){
    w=src.width;h=src.height;ctx=src.getContext("2d");
    data=ctx.getImageData(0,0,w,h).data;
  }else{
    if(!src.naturalWidth)return true;
    w=Math.min(64,src.naturalWidth);h=Math.min(36,src.naturalHeight);
    var c=document.createElement("canvas");
    c.width=w;c.height=h;
    ctx=c.getContext("2d");
    ctx.drawImage(src,0,0,w,h);
    data=ctx.getImageData(0,0,w,h).data;
  }
  var sum=0,n=data.length/4;
  for(var i=0;i<data.length;i+=4){
    sum+=0.299*data[i]+0.587*data[i+1]+0.114*data[i+2];
  }
  return (sum/n)<th;
}
function findFirstNonBlackIndex(frames,maxScan,cb){
  var idx=0,limit=maxScan||40;
  function tryNext(){
    if(!frames.length){cb(0);return;}
    if(idx>=frames.length||idx>=limit){cb(0);return;}
    var probe=new Image();
    probe.onload=function(){
      if(!isImageMostlyBlack(probe))cb(idx);
      else{idx++;tryNext();}
    };
    probe.onerror=function(){idx++;tryNext();};
    probe.src="/photos/image?f="+encodeURIComponent(frames[idx]);
  }
  tryNext();
}
function dxToVelocity(dx,sens,maxV,minV){
  sens=sens||5;maxV=maxV||12;minV=minV||1;
  var mag=Math.abs(dx);
  var v=mag/sens;
  if(v<minV)v=minV;
  if(v>maxV)v=maxV;
  return dx>0?v:-v;
}
function attachVelocityScrub(el,cfg){
  var lastX=null,velocity=0,tickTimer=null,accum=0;
  var TICK_MS=33,PPF=cfg.pixelsPerFrame||16;
  var SENS=cfg.sens||5,MAX_V=cfg.maxSpeed||12,MIN_V=cfg.minSpeed||1,MIN_DX=2;
  function applyAccum(){
    if(cfg.mode!=="frames")return;
    while(accum>=1){if(!cfg.advance(1)){accum=0;velocity=0;break;}accum-=1;}
    while(accum<=-1){if(!cfg.advance(-1)){accum=0;velocity=0;break;}accum+=1;}
  }
  function tick(){
    if(Math.abs(velocity)<0.01)return;
    var dt=TICK_MS/1000;
    if(cfg.mode==="frames"){
      accum+=velocity*dt;
      applyAccum();
    }else{
      cfg.advance(velocity*dt);
    }
    if(cfg.onHint)cfg.onHint(velocity);
  }
  function ensureTick(){
    if(!tickTimer)tickTimer=setInterval(tick,TICK_MS);
  }
  function stopTick(){
    if(tickTimer){clearInterval(tickTimer);tickTimer=null;}
  }
  function onMove(e){
    if(cfg.onActivate)cfg.onActivate();
    if(lastX===null){lastX=e.clientX;return;}
    var dx=e.clientX-lastX;
    lastX=e.clientX;
    if(Math.abs(dx)<MIN_DX)return;
    if(cfg.mode==="frames"){
      accum+=dx/PPF;
      applyAccum();
    }else if(cfg.instantSeek){
      cfg.instantSeek(dx);
    }
    velocity=dxToVelocity(dx,SENS,MAX_V,MIN_V);
    ensureTick();
    if(cfg.onHint)cfg.onHint(velocity);
  }
  function onEnter(e){
    if(cfg.onActivate)cfg.onActivate();
    lastX=e.clientX;
    velocity=0;accum=0;
    if(cfg.onHint)cfg.onHint(0);
  }
  function onLeave(){
    lastX=null;velocity=0;accum=0;
    stopTick();
    if(cfg.onLeave)cfg.onLeave();
    if(cfg.onHint)cfg.onHint(0);
  }
  el.addEventListener("mouseenter",onEnter);
  el.addEventListener("mousemove",onMove);
  el.addEventListener("mouseleave",onLeave);
}
function initPhotoPreview(el){
  var base=el.getAttribute("data-base");
  var img=el.querySelector("img");
  var busy=false,pending=-1,frames=[],cur=0;
  function show(i){
    if(!frames.length)return;
    if(i<0)i=0;if(i>=frames.length)i=frames.length-1;
    if(busy){pending=i;return;}
    busy=true;cur=i;pending=-1;
    img.onload=img.onerror=function(){
      busy=false;
      if(pending>=0){var n=pending;pending=-1;show(n);}
    };
    img.src="/photos/image?f="+encodeURIComponent(frames[cur]);
  }
  fetch("/photos/frames?b="+encodeURIComponent(base),{credentials:"same-origin"})
    .then(function(r){if(!r.ok)throw 0;return r.json();})
    .then(function(d){
      frames=d.frames||[];
      if(!frames.length)return;
      // Niente scan "neri": scaricare 40 JPEG FullHD satura i socket HTTP (errno 104).
      show(0);
      attachVelocityScrub(el,{
        mode:"frames",maxSpeed:6,minSpeed:1,sens:5,pixelsPerFrame:18,
        advance:function(dir){
          var ni=cur+dir;
          if(ni<0||ni>=frames.length)return false;
          show(ni);return true;
        }
      });
    }).catch(function(){});
}
function initListPreviews(){
  // Anteprime foto una alla volta (FullHD JPEG): evita saturazione socket HTTP.
  var photos=Array.prototype.slice.call(document.querySelectorAll(".prev-photo"));
  var i=0;
  function nextPhoto(){
    if(i>=photos.length)return;
    initPhotoPreview(photos[i++]);
    setTimeout(nextPhoto,350);
  }
  nextPhoto();
}
)JS";

static const char *MEDIA_LIST_HINT = R"HTML(
<p class="hint">Preview: muovi il mouse a <strong>destra/sinistra</strong> — l'ampiezza del movimento imposta velocità e verso; fermo dentro l'area la preview continua a quella velocità.</p>
<script>
)HTML";

static const char *MEDIA_LIST_TAIL = R"HTML(
</script>
<script>initListPreviews();</script>
</body></html>
)HTML";

static const char *VIDEO_LIST_TAIL = R"HTML(
</body></html>
)HTML";

static const char *VIDEOS_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Video CORE</title>
<style>
body{font-family:sans-serif;max-width:1100px;margin:24px auto}
nav a{margin-right:12px}
ul{list-style:none;padding:0;margin:0}
.vid-row{display:flex;align-items:center;gap:12px;padding:10px 0;border-bottom:1px solid #e8e8e8;flex-wrap:wrap}
.vid-preview{width:168px;height:94px;flex-shrink:0;background:#111;border-radius:4px;overflow:hidden}
.vid-icon{display:flex;align-items:center;justify-content:center;color:#bbb;font-size:12px;font-weight:700;letter-spacing:.04em}
.vid-preview img{width:100%;height:100%;object-fit:contain;display:block;background:#000}
.vid-info{flex:1;min-width:140px}
.vid-title-plain{font-weight:500;color:#222;display:inline;margin-bottom:4px;word-break:break-all}
.vid-title{text-decoration:none;color:#2266cc;font-weight:500;display:inline;margin-bottom:4px}
.vid-title-row{display:flex;align-items:center;gap:8px;margin-bottom:4px;flex-wrap:wrap}
.vid-lock-ico{display:inline-flex;align-items:center;justify-content:center;height:22px;padding:0 7px;
  border-radius:4px;background:#5a4a1a;color:#ffe9a8;font-size:11px;font-weight:700;letter-spacing:.04em;
  flex-shrink:0;line-height:1}
.vid-size{color:#666;font-size:14px}
.vid-actions{display:flex;align-items:center;gap:8px;flex-shrink:0}
.vid-btn{padding:6px 14px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600;
  text-decoration:none;display:inline-block;white-space:nowrap;line-height:1.35;font-family:sans-serif}
.vid-btn-dl{background:#2266cc;color:#fff !important}
.vid-btn-dl:hover{background:#1a52a3;color:#fff !important}
.vid-btn-del{background:#c33;color:#fff}
.vid-btn-del:hover{background:#a22}
.vid-toolbar{display:flex;gap:10px;flex-wrap:wrap;margin:12px 0 16px;align-items:center}
.vid-toolbar form{margin:0}
.alert{padding:12px 14px;margin:12px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
.vid-empty{color:#666;padding:16px 0}
.vid-meta{color:#555;font-size:14px;margin:8px 0 12px}
.vid-pending{color:#888;font-style:italic}
.hint{color:#666;font-size:14px;margin-top:12px;line-height:1.45}
.sd-warn{padding:12px 14px;margin:12px 0;border-radius:6px;background:#fff8e6;border:1px solid #f0d080;color:#664400}
.vid-pager{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:16px 0 8px}
.vid-pager a,.vid-pager span{padding:6px 12px;border-radius:4px;font-size:13px;font-weight:600;
  text-decoration:none;line-height:1.35}
.vid-pager a{background:#eef3ff;color:#2266cc}
.vid-pager a:hover{background:#dce6ff}
.vid-pager-cur{background:#2266cc;color:#fff}
.vid-pager-off{background:#f2f2f2;color:#aaa}
</style></head><body>
)HTML";

static const char *VIDEOS_HTML_BODY_START = R"HTML(
<h2>Video MP4</h2>
<p class="vid-meta">Registrazioni video H264 sulla SD. Badge <strong>AES</strong> = file cifrato.</p>
<p class="hint">I video <strong>non si riproducono</strong> sul dispositivo: usa <strong>Scarica</strong> e aprili sul PC (VLC).
File AES: decifratura con barra di avanzamento prima del download. Un solo download pesante alla volta.</p>
<div class="vid-toolbar">
<form method="POST" action="/videos/delete_all" onsubmit="return confirm('Eliminare TUTTI i video (MP4/AVI) sulla SD?');">
<button type="submit" class="vid-btn vid-btn-del">Elimina tutti i video</button></form>
</div>
)HTML";

static const char *PHOTOS_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Foto / Timelapse</title>
<style>
body{font-family:sans-serif;max-width:1100px;margin:24px auto}
nav a{margin-right:12px}
ul{list-style:none;padding:0;margin:0}
.vid-row{display:flex;align-items:center;gap:12px;padding:10px 0;border-bottom:1px solid #e8e8e8;flex-wrap:wrap}
.vid-preview{width:168px;height:94px;flex-shrink:0;background:#111;border-radius:4px;overflow:hidden;cursor:ew-resize}
.vid-preview img{width:100%;height:100%;object-fit:contain;display:block;background:#000}
.vid-info{flex:1;min-width:140px}
.vid-title{text-decoration:none;color:#2266cc;font-weight:500;display:inline;margin-bottom:4px}
.vid-title-row{display:flex;align-items:center;gap:8px;margin-bottom:4px;flex-wrap:wrap}
.vid-avi-ico{display:inline-flex;align-items:center;justify-content:center;width:28px;height:28px;
  border-radius:6px;background:#2a7a4b;color:#fff !important;text-decoration:none;font-size:15px;line-height:1;
  flex-shrink:0}
.vid-avi-ico:hover{background:#22633d;color:#fff !important}
.vid-lock-ico{display:inline-flex;align-items:center;justify-content:center;height:22px;padding:0 7px;
  border-radius:4px;background:#5a4a1a;color:#ffe9a8;font-size:11px;font-weight:700;letter-spacing:.04em;
  flex-shrink:0;line-height:1}
.vid-fps-ico{display:inline-flex;align-items:center;justify-content:center;height:22px;padding:0 7px;
  border-radius:4px;background:#1a3a5a;color:#c8e0ff;font-size:11px;font-weight:700;letter-spacing:.03em;
  flex-shrink:0;line-height:1}
.vid-size{color:#666;font-size:14px}
.vid-actions{display:flex;align-items:center;gap:8px;flex-shrink:0}
.vid-btn{padding:6px 14px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600;
  text-decoration:none;display:inline-block;white-space:nowrap;line-height:1.35;font-family:sans-serif}
.vid-btn-dl{background:#2266cc;color:#fff !important}
.vid-btn-dl:hover{background:#1a52a3;color:#fff !important}
.vid-btn-mp4{background:#2a7a4b;color:#fff !important}
.vid-btn-mp4:hover{background:#22633d;color:#fff !important}
.vid-btn-del{background:#c33;color:#fff}
.vid-btn-del:hover{background:#a22}
.vid-toolbar{display:flex;gap:10px;flex-wrap:wrap;margin:12px 0 16px;align-items:center}
.vid-toolbar form{margin:0}
.alert{padding:12px 14px;margin:12px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
.vid-empty{color:#666;padding:16px 0}
.vid-meta{color:#555;font-size:14px;margin:8px 0 12px}
.hint{color:#666;font-size:14px;margin-top:12px;line-height:1.45}
.sd-warn{padding:12px 14px;margin:12px 0;border-radius:6px;background:#fff8e6;border:1px solid #f0d080;color:#664400}
</style></head><body>
)HTML";

static const char *PHOTOS_HTML_BODY_START = R"HTML(
<h2>Foto / Timelapse</h2>
<p class="vid-meta">Sessioni JPEG a <strong>1 fps</strong>. Badge <strong>TL</strong>; verde = AVI; <strong>AES</strong> = cifrato.</p>
<div class="vid-toolbar">
<form method="POST" action="/photos/delete_all" onsubmit="return confirm('Eliminare TUTTI i timelapse foto?');">
<button type="submit" class="vid-btn vid-btn-del">Elimina tutti i timelapse</button></form>
</div>
)HTML";

static const char *PHOTOS_WATCH_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Timelapse foto</title>
<style>
body{font-family:sans-serif;max-width:1180px;margin:24px auto}
nav a{margin-right:12px}
.slideshow{display:flex;gap:16px;align-items:stretch;margin-top:12px}
.viewer{flex:1;background:#000;border-radius:8px;padding:8px;min-height:280px;display:flex;align-items:center;justify-content:center;cursor:ew-resize}
#frame{display:block;width:100%;max-height:75vh;border-radius:4px;background:#111}
.sidepanel{width:300px;flex-shrink:0;display:flex;flex-direction:column;min-height:280px}
.sidepanel h3{margin:0 0 8px;font-size:15px;color:#333}
.bar{margin:14px 0;display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.btn{padding:8px 16px;border-radius:4px;text-decoration:none;font-size:14px;font-weight:600;color:#fff !important;background:#555}
.btn-mp4{background:#2a7a4b}.btn-mp4:hover{background:#22633d}
.hint{color:#666;font-size:14px;margin-top:8px;line-height:1.45}
#idx{color:#333;font-size:14px;min-width:6em}
#scrubHint{color:#555;font-size:13px;margin-left:8px}
#err{color:#a33;font-size:14px;margin:8px 0;min-height:1.2em}
#meta{color:#555;font-size:13px;margin:4px 0 8px}
#photolist{list-style:none;padding:0;margin:0;flex:1;overflow-y:auto;border:1px solid #e0e0e0;border-radius:8px;max-height:75vh}
#photolist li{padding:6px 10px;border-bottom:1px solid #eee;font-size:12px;font-family:monospace}
#photolist li.active{background:#eef3ff;font-weight:600;border-left:4px solid #2266cc;padding-left:6px}
#photolist li:last-child{border-bottom:none}
#photolist a{color:#2266cc;text-decoration:none;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#photolist a:hover{text-decoration:underline}
@media(max-width:820px){.slideshow{flex-direction:column}.sidepanel{width:100%;max-height:40vh}}
</style></head><body>
)HTML";

static const char *PHOTOS_WATCH_HTML_TAIL = R"HTML(
<h2 id="title">Timelapse foto</h2>
<p id="meta"></p>
<div id="loadBar" class="hint" style="display:none;margin:8px 0;padding:10px 12px;background:#eef3ff;border:1px solid #c5d4f5;border-radius:6px">
<strong id="loadTitle">Preparazione…</strong> <span id="loadPct">0</span>% — <span id="loadCount">0</span> foto in cache.
<div style="margin-top:8px;height:8px;background:#dde5f5;border-radius:4px;overflow:hidden">
<div id="loadFill" style="height:100%;width:0;background:#2266cc;transition:width .15s"></div>
</div></div>
<div class="slideshow">
<div id="viewer" class="viewer">
<img id="frame" alt="frame timelapse">
</div>
<aside class="sidepanel">
<h3>Elenco foto</h3>
<ul id="photolist"></ul>
</aside>
</div>
<div class="bar"><span id="idx"></span><span id="scrubHint"></span>
<a class="btn btn-mp4" id="btnExportMp4" href="#">Crea video</a>
<a class="btn" href="/photos">Torna all'elenco</a></div>
<p id="err"></p>
<p class="hint">Al <strong>50%</strong> di preload parte la riproduzione; gli altri frame continuano a caricarsi in parallelo (2 alla volta).
Clic su un altro menu: navigazione immediata (il preload viene abbandonato).
<strong>Crea video</strong> genera un AVI MJPEG; se esiste già, avvisa e scarica subito.
Mouse sull'immagine: ampiezza → verso e velocità. Esci → 1 foto/s.</p>
<script>
(function(){
  var AUTO_MS=1000,SENS=5,MAX_FPS=8,MIN_FPS=1,PPF=16,TICK_MS=40,MIN_DX=2;
  var PRELOAD_CONC=2, START_PCT=50;
  var params=new URLSearchParams(location.search);
  var sessionBase=params.get("b");
  var viewer=document.getElementById("viewer");
  var img=document.getElementById("frame");
  var idxEl=document.getElementById("idx");
  var hintEl=document.getElementById("scrubHint");
  var errEl=document.getElementById("err");
  var metaEl=document.getElementById("meta");
  var listEl=document.getElementById("photolist");
  var loadBar=document.getElementById("loadBar");
  var loadTitle=document.getElementById("loadTitle");
  var loadPct=document.getElementById("loadPct");
  var loadCount=document.getElementById("loadCount");
  var loadFill=document.getElementById("loadFill");
  var frames=[],ready=[],cur=0,lastX=null,velocity=0,accum=0;
  var autoTimer=null,scrubTimer=null,busy=false,pending=-1,wantAuto=false,playReady=false;
  var captureLabel="",preloadDone=0,aborted=false,abortCtl=null;
  if(!sessionBase){ errEl.textContent="Parametro sessione mancante (?b=)"; return; }
  var btnMp4=document.getElementById("btnExportMp4");
  if(btnMp4){
    btnMp4.href="/photos/export?b="+encodeURIComponent(sessionBase);
  }
  function frameUrl(path){ return "/photos/image?f="+encodeURIComponent(path); }
  function abortPreload(){
    aborted=true;
    if(abortCtl){ try{abortCtl.abort();}catch(e){} }
    stopAuto(); stopScrub();
  }
  document.addEventListener("click",function(e){
    var a=e.target&&e.target.closest?e.target.closest("a"):null;
    if(!a)return;
    var href=a.getAttribute("href")||"";
    if(!href||href.charAt(0)==="#"||a.hasAttribute("download"))return;
    if(href.indexOf("javascript:")===0)return;
    abortPreload();
  },true);
  window.addEventListener("pagehide",abortPreload);
  function isReady(i){ return i>=0 && i<ready.length && !!ready[i]; }
  function scrubHint(v){
    if(!playReady){hintEl.textContent="attendi preload 50%…";return;}
    if(Math.abs(v)<0.05){hintEl.textContent="";return;}
    var d=v>0?"avanti":"indietro";
    hintEl.textContent=d+" ~"+Math.abs(v).toFixed(1)+" foto/s";
  }
  function stopAuto(){
    wantAuto=false;
    if(autoTimer){clearInterval(autoTimer);autoTimer=null;}
  }
  function stopScrub(){
    if(scrubTimer){clearInterval(scrubTimer);scrubTimer=null;}
  }
  function dxToFps(dx){
    var v=Math.abs(dx)/SENS;
    if(v<MIN_FPS)v=MIN_FPS;
    if(v>MAX_FPS)v=MAX_FPS;
    return dx>0?v:-v;
  }
  function nextReady(from, dir){
    var i=from;
    while(i>=0 && i<frames.length){
      if(isReady(i)) return i;
      i+=dir;
    }
    return -1;
  }
  function applyAccum(){
    while(accum>=1){
      if(cur>=frames.length-1){velocity=0;accum=0;scrubHint(0);break;}
      var n=nextReady(cur+1,1);
      if(n<0){accum=0;break;}
      showAt(n);accum-=1;
    }
    while(accum<=-1){
      if(cur<=0){velocity=0;accum=0;scrubHint(0);break;}
      var p=nextReady(cur-1,-1);
      if(p<0){accum=0;break;}
      showAt(p);accum+=1;
    }
  }
  function scrubTick(){
    if(!playReady||busy||Math.abs(velocity)<0.01)return;
    accum+=velocity*(TICK_MS/1000);
    applyAccum();
    scrubHint(velocity);
  }
  function ensureScrub(){
    if(!scrubTimer)scrubTimer=setInterval(scrubTick,TICK_MS);
  }
  function startAuto(){
    if(!playReady)return;
    stopScrub();velocity=0;accum=0;wantAuto=true;
    if(cur>=frames.length-1){hintEl.textContent="(fine)";return;}
    hintEl.textContent="riproduzione 1 fps";
    if(autoTimer)return;
    autoTimer=setInterval(function(){
      if(!wantAuto||busy)return;
      if(cur>=frames.length-1){stopAuto();hintEl.textContent="(fine)";return;}
      var n=nextReady(cur+1,1);
      if(n<0){
        hintEl.textContent="attendo cache…";
        return;
      }
      hintEl.textContent="riproduzione 1 fps";
      showAt(n);
    },AUTO_MS);
  }
  function onScrubMove(e){
    if(!playReady)return;
    stopAuto();
    if(lastX===null){lastX=e.clientX;return;}
    var dx=e.clientX-lastX;
    lastX=e.clientX;
    if(Math.abs(dx)<MIN_DX)return;
    accum+=dx/PPF;
    applyAccum();
    velocity=dxToFps(dx);
    ensureScrub();
    scrubHint(velocity);
  }
  function onScrubEnter(e){
    if(!playReady)return;
    stopAuto();lastX=e.clientX;velocity=0;accum=0;scrubHint(0);
  }
  function onScrubLeave(){
    if(!playReady)return;
    lastX=null;velocity=0;accum=0;stopScrub();startAuto();
  }
  viewer.addEventListener("mouseenter",onScrubEnter);
  viewer.addEventListener("mousemove",onScrubMove);
  viewer.addEventListener("mouseleave",onScrubLeave);
  function highlight(i){
    var lis=listEl.querySelectorAll("li");
    for(var j=0;j<lis.length;j++){ lis[j].classList.toggle("active", j===i); }
    var active=listEl.querySelector(".active");
    if(active){ active.scrollIntoView({block:"nearest",behavior:"smooth"}); }
  }
  function showAt(i){
    if(i<0||i>=frames.length) return;
    if(busy){ pending=i; return; }
    busy=true; cur=i; pending=-1;
    idxEl.textContent=(cur+1)+" / "+frames.length;
    highlight(cur);
    var url=frameUrl(frames[cur]);
    var finish=function(ok){
      busy=false;
      if(!ok) errEl.textContent="Frame non caricato";
      else errEl.textContent="";
      if(pending>=0){var n=pending;pending=-1;showAt(n);}
    };
    img.onerror=function(){ finish(false); };
    img.onload=function(){ finish(true); };
    if(img.src===url && img.complete && img.naturalWidth>0){
      finish(true);
      return;
    }
    img.src=url;
  }
  function setLoadProgress(done,total){
    var pct=total?Math.round(100*done/total):0;
    loadPct.textContent=String(pct);
    loadCount.textContent=done+" / "+total;
    loadFill.style.width=pct+"%";
  }
  function beginPlayback(){
    if(playReady) return;
    playReady=true;
    loadTitle.textContent="In riproduzione — cache in corso…";
    if(captureLabel){ metaEl.textContent="Cattura: "+captureLabel+" — playback avviato (preload continua)"; }
    else { metaEl.textContent="Playback avviato — preload in background"; }
    showAt(0);
    startAuto();
  }
  function finishPreload(){
    loadBar.style.display="none";
    if(captureLabel){ metaEl.textContent="Cattura: "+captureLabel+" — pronto ("+frames.length+" foto)"; }
    else { metaEl.textContent="Pronto — "+frames.length+" foto in cache"; }
    if(!playReady) beginPlayback();
    else if(wantAuto && hintEl.textContent.indexOf("attendo")>=0){
      hintEl.textContent="riproduzione 1 fps";
    }
  }
  function preloadAll(paths){
    return new Promise(function(resolve){
      if(!paths.length){ resolve(); return; }
      if(aborted){ resolve(); return; }
      abortCtl= (typeof AbortController!=="undefined") ? new AbortController() : null;
      loadBar.style.display="block";
      loadTitle.textContent="Preparazione…";
      ready=new Array(paths.length);
      var next=0,done=0,active=0;
      var startAt=Math.max(1, Math.ceil(paths.length*START_PCT/100));
      function settle(idx,ok){
        if(aborted){ resolve(); return; }
        ready[idx]=!!ok;
        active--; done++;
        preloadDone=done;
        setLoadProgress(done,paths.length);
        if(!playReady && done>=startAt) beginPlayback();
        if(done===paths.length){ finishPreload(); resolve(); }
        else pump();
      }
      function pump(){
        if(aborted){ resolve(); return; }
        while(active<PRELOAD_CONC && next<paths.length){
          (function(idx){
            active++;
            var opts={credentials:"same-origin",cache:"force-cache"};
            if(abortCtl) opts.signal=abortCtl.signal;
            fetch(frameUrl(paths[idx]),opts)
              .then(function(r){
                if(aborted) throw new Error("aborted");
                if(!r.ok) throw new Error("HTTP "+r.status);
                return r.blob();
              })
              .then(function(blob){
                if(aborted){ settle(idx,false); return; }
                var url=URL.createObjectURL(blob);
                var im=new Image();
                im.onload=function(){ URL.revokeObjectURL(url); settle(idx,true); };
                im.onerror=function(){ URL.revokeObjectURL(url); settle(idx,false); };
                im.src=url;
              })
              .catch(function(){ settle(idx,false); });
          })(next++);
        }
      }
      setLoadProgress(0,paths.length);
      pump();
    });
  }
  fetch("/photos/frames?b="+encodeURIComponent(sessionBase),{credentials:"same-origin"})
    .then(function(r){ if(!r.ok) throw new Error("HTTP "+r.status); return r.json(); })
    .then(function(data){
      if(aborted) return;
      frames=data.frames||[];
      if(!frames.length){ errEl.textContent="Nessuna foto in questa sessione"; return; }
      var label=(data.base||sessionBase).split("/").pop();
      document.getElementById("title").textContent="Timelapse foto: "+label;
      captureLabel=data.capture||"";
      if(captureLabel){ metaEl.textContent="Cattura: "+captureLabel+" — preload fino al 50%, poi playback"; }
      else { metaEl.textContent="Preload fino al 50%, poi playback"; }
      frames.forEach(function(path,i){
        var li=document.createElement("li");
        li.id="pf"+i;
        var name=path.split("/").pop();
        var a=document.createElement("a");
        a.href="/photos/download?f="+encodeURIComponent(path);
        a.download=name;
        a.textContent=name;
        li.appendChild(a);
        listEl.appendChild(li);
      });
      hintEl.textContent="attendi preload 50%…";
      return preloadAll(frames);
    })
    .catch(function(e){
      if(aborted) return;
      errEl.textContent="Caricamento fallito: "+e.message;
      loadBar.style.display="none";
    });
})();
</script></body></html>
)HTML";

static const char *LIVE_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Live CORE</title>
<style>
body{font-family:sans-serif;max-width:960px;margin:24px auto}
nav a{margin-right:12px}
.live-wrap{background:#111;border-radius:8px;padding:8px;text-align:center;min-height:240px;display:flex;align-items:center;justify-content:center}
.live-wrap img{max-width:100%;height:auto;border-radius:4px;background:#000;transform-origin:center center}
.live-err{color:#fff;background:#5a1a1a;border:1px solid #a33;padding:20px;border-radius:6px;text-align:left;line-height:1.5}
.live-meta{color:#666;font-size:14px;margin-top:8px}
.rec-bar{display:flex;align-items:center;gap:12px;margin:12px 0;flex-wrap:wrap;position:relative;z-index:2}
.rec-bar button{padding:10px 18px;font-size:15px;border:none;border-radius:6px;cursor:pointer;pointer-events:auto}
#btnRecStart{background:#c33;color:#fff}
#btnRecStop{background:#444;color:#fff}
#btnImgReset{background:#2266cc;color:#fff}
#btnRecStart:disabled,#btnRecStop:disabled,#btnImgReset:disabled{opacity:.45;cursor:not-allowed;pointer-events:none}
#btnRecStop:not(:disabled){cursor:pointer}
.rec-status{font-size:14px;color:#333;min-height:1.4em}
.rec-status.rec-on{color:#c33;font-weight:bold}
.rec-overlay{color:#ccc;font-size:18px;padding:40px;pointer-events:none;position:static}
#focusBar{align-items:center}
#focusBar label{flex:1;min-width:220px;font-size:14px;color:#333}
#focusSlider{width:100%;accent-color:#2266cc}
#focusVal{min-width:3.2em;font-variant-numeric:tabular-nums;font-size:15px;font-weight:600;color:#1a4480}
#btnFocusSave{background:#2266cc;color:#fff;padding:8px 14px;border:none;border-radius:6px;cursor:pointer;font-size:14px}
#btnFocusSave:disabled{opacity:.45;cursor:not-allowed}
</style></head><body>
)HTML";

static const char *REC_VIDEO_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Registrazione video</title>
<style>
body{font-family:sans-serif;max-width:640px;margin:24px auto}
input,button,select{padding:8px;margin:4px 0;width:100%;box-sizing:border-box}
nav a{margin-right:12px}
fieldset{border:1px solid #ccc;border-radius:8px;padding:12px 16px;margin:16px 0}
fieldset legend{font-weight:600;padding:0 6px}
.hint{font-size:14px;color:#555;margin:8px 0;line-height:1.45}
.alert{padding:12px 14px;margin:12px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
.stats{background:#eef3ff;border:1px solid #c5d4f5;padding:10px 12px;border-radius:6px;font-size:14px;margin:12px 0}
.row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
@media(max-width:520px){.row{grid-template-columns:1fr}}
</style></head><body>
)HTML";

static const char *REC_VIDEO_FORM_TAIL = R"HTML(
<p class="hint">Camera <strong>IMX415</strong> (8 MP) + <strong>RV1106</strong> VENC H.264.
Nativo sensore: <strong>3840×2160</strong>. Default <strong>1920×1080 @ 30 fps</strong>.
Dal FullHD fino a 8 MP: 2304×1296, 2560×1440, 2880×1620 (ancora a 30 fps),
3072×1728 e 3840×2160 a <strong>15 fps</strong> (sopra i 5 MP).
Restano 720p, VGA e 1280×960. Il Live resta anteprima 640×480 MJPEG.
Si applica al prossimo avvio registrazione (PIR o web).</p>
<form method="POST" action="/rec_video" id="videoForm" onsubmit="return syncBeforeSave()">
<fieldset>
<legend>Tipo registrazione</legend>
<label>Contenuto su SD</label>
<select name="rec_media" id="rec_media" onchange="onMediaChange()">
<option value="0"%s>Video H264 (MP4)</option>
<option value="1"%s>Timelapse foto (JPEG, 1 fps)</option>
</select>
<p class="hint" id="mediaHint" style="margin:6px 0 0">In modalità <strong>foto</strong> vengono salvati file <code>…_foto0001.jpg</code> a <strong>1 fps</strong> fisso.
Per la massima definizione usa <strong>3840×2160</strong>. Con cifratura attiva JPEG e AVI usano AES (MRBI).</p>
</fieldset>
<fieldset>
<legend>Modi IMX415 (1080p fino a 8 MP, più le risoluzioni basse)</legend>
<label>Preset</label>
<select name="video_preset" id="preset" onchange="applyPreset()">
<option value="">— personalizzato —</option>
<optgroup label="8 MP nativo — H.264 (3840×2160, max 15 fps)">
<option value="3840,2160,15">3840×2160 @ 15 fps (8 MP)</option>
<option value="3840,2160,10">3840×2160 @ 10 fps</option>
</optgroup>
<optgroup label="Oltre 5 MP — H.264 (3072×1728, max 15 fps)">
<option value="3072,1728,15">3072×1728 @ 15 fps</option>
<option value="3072,1728,10">3072×1728 @ 10 fps</option>
</optgroup>
<optgroup label="Quasi 5 MP — H.264 (2880×1620)">
<option value="2880,1620,30">2880×1620 @ 30 fps</option>
<option value="2880,1620,25">2880×1620 @ 25 fps</option>
<option value="2880,1620,20">2880×1620 @ 20 fps</option>
<option value="2880,1620,15">2880×1620 @ 15 fps</option>
<option value="2880,1620,10">2880×1620 @ 10 fps</option>
</optgroup>
<optgroup label="QHD — H.264 (2560×1440)">
<option value="2560,1440,30">2560×1440 @ 30 fps</option>
<option value="2560,1440,25">2560×1440 @ 25 fps</option>
<option value="2560,1440,20">2560×1440 @ 20 fps</option>
<option value="2560,1440,15">2560×1440 @ 15 fps</option>
<option value="2560,1440,10">2560×1440 @ 10 fps</option>
</optgroup>
<optgroup label="3 MP — H.264 (2304×1296)">
<option value="2304,1296,30">2304×1296 @ 30 fps</option>
<option value="2304,1296,25">2304×1296 @ 25 fps</option>
<option value="2304,1296,20">2304×1296 @ 20 fps</option>
<option value="2304,1296,15">2304×1296 @ 15 fps</option>
<option value="2304,1296,10">2304×1296 @ 10 fps</option>
</optgroup>
<optgroup label="FullHD — H.264 (1920×1080, default)">
<option value="1920,1080,30">1920×1080 @ 30 fps (default)</option>
<option value="1920,1080,25">1920×1080 @ 25 fps</option>
<option value="1920,1080,20">1920×1080 @ 20 fps</option>
<option value="1920,1080,15">1920×1080 @ 15 fps</option>
<option value="1920,1080,10">1920×1080 @ 10 fps</option>
</optgroup>
<optgroup label="HD 720p — H.264 (1280×720)">
<option value="1280,720,30">1280×720 @ 30 fps</option>
<option value="1280,720,25">1280×720 @ 25 fps</option>
<option value="1280,720,20">1280×720 @ 20 fps</option>
<option value="1280,720,15">1280×720 @ 15 fps</option>
<option value="1280,720,10">1280×720 @ 10 fps</option>
</optgroup>
<optgroup label="ISP 4:3 — H.264">
<option value="1280,960,30">1280×960 @ 30 fps</option>
<option value="1280,960,15">1280×960 @ 15 fps</option>
</optgroup>
<optgroup label="ISP VGA — H.264 (640×480)">
<option value="640,480,30">640×480 @ 30 fps</option>
<option value="640,480,20">640×480 @ 20 fps</option>
<option value="640,480,15">640×480 @ 15 fps</option>
</optgroup>
<optgroup label="Timelapse video — 1–3 fps">
<option value="3840,2160,1">3840×2160 @ 1 fps (8 MP)</option>
<option value="3840,2160,2">3840×2160 @ 2 fps</option>
<option value="3840,2160,3">3840×2160 @ 3 fps</option>
<option value="3072,1728,1">3072×1728 @ 1 fps</option>
<option value="3072,1728,2">3072×1728 @ 2 fps</option>
<option value="3072,1728,3">3072×1728 @ 3 fps</option>
<option value="2880,1620,1">2880×1620 @ 1 fps</option>
<option value="2880,1620,2">2880×1620 @ 2 fps</option>
<option value="2880,1620,3">2880×1620 @ 3 fps</option>
<option value="2560,1440,1">2560×1440 @ 1 fps</option>
<option value="2560,1440,2">2560×1440 @ 2 fps</option>
<option value="2560,1440,3">2560×1440 @ 3 fps</option>
<option value="2304,1296,1">2304×1296 @ 1 fps</option>
<option value="2304,1296,2">2304×1296 @ 2 fps</option>
<option value="2304,1296,3">2304×1296 @ 3 fps</option>
<option value="1920,1080,1">1920×1080 @ 1 fps</option>
<option value="1920,1080,2">1920×1080 @ 2 fps</option>
<option value="1920,1080,3">1920×1080 @ 3 fps</option>
<option value="1280,720,1">1280×720 @ 1 fps</option>
<option value="1280,720,2">1280×720 @ 2 fps</option>
<option value="1280,720,3">1280×720 @ 3 fps</option>
</optgroup>
<optgroup label="Timelapse foto JPEG — 1 fps">
<option value="3840,2160,1">3840×2160 @ 1 fps (8 MP)</option>
<option value="3072,1728,1">3072×1728 @ 1 fps</option>
<option value="2880,1620,1">2880×1620 @ 1 fps</option>
<option value="2560,1440,1">2560×1440 @ 1 fps</option>
<option value="2304,1296,1">2304×1296 @ 1 fps</option>
<option value="1920,1080,1">1920×1080 @ 1 fps</option>
<option value="1280,720,1">1280×720 @ 1 fps</option>
<option value="640,480,1">640×480 @ 1 fps</option>
</optgroup>
</select>
<p class="hint" style="margin:6px 0 0">Il <strong>preset selezionato</strong> ha priorità sui campi numerici al salvataggio.</p>
<p class="hint" style="margin:6px 0 0">Per timelapse e foto puoi impostare manualmente fino a <strong>%u×%u</strong> px nei campi sotto.</p>
</fieldset>
<div class="row">
<label>Larghezza (px)<input name="video_w" id="video_w" type="number" min="%u" max="%u" step="2" value="%u" required></label>
<label>Altezza (px)<input name="video_h" id="video_h" type="number" min="%u" max="%u" step="2" value="%u" required></label>
<label>Frame rate (fps)<input name="video_fps" id="video_fps" type="number" min="%u" max="%u" value="%u" required></label>
</div>
<fieldset>
<legend>Orientamento immagine</legend>
<label style="display:flex;align-items:center;gap:8px"><input type="checkbox" name="video_vflip" value="1" style="width:auto;margin:0"%s>Capovolgi immagine (flip verticale)</label>
<label style="display:flex;align-items:center;gap:8px"><input type="checkbox" name="video_hflip" value="1" style="width:auto;margin:0"%s>Specchia immagine (flip orizzontale)</label>
<p class="hint" style="margin:6px 0 0">Applicato dal sensore: vale sia per il live sia per la registrazione su SD. Se la camera è attiva, ha effetto immediato al salvataggio.</p>
</fieldset>
<fieldset>
<legend>Streamer RTSP</legend>
<p class="hint" style="margin:6px 0 0">RTSP si accende e si spegne dalla pagina <a href="/channels">Canali Video</a>.
Canale 1 usa questa risoluzione. Un client collegato non accende lo stream da solo.</p>
</fieldset>
<fieldset>
<legend>Sicurezza registrazione</legend>
<label style="display:flex;align-items:center;gap:8px"><input type="checkbox" name="rec_encrypt" value="1" style="width:auto;margin:0"%s>Cifra media su SD (video, foto, AVI — AES)</label>
<p class="hint" style="margin:6px 0 0">Con la spunta: registrazione web cifra a fine clip; i clip PIR restano in chiaro
fino al boot CONFIG, poi AES (MRBI) in automatico. Senza spunta: MP4 sempre in chiaro.</p>
</fieldset>
<fieldset>
<legend>OSD data/ora sul video</legend>
<label style="display:flex;align-items:center;gap:8px"><input type="checkbox" name="osd_en" id="osd_en" value="1" style="width:auto;margin:0"%s>Mostra data e ora nell'immagine (burn-in)</label>
<label>Dimensione font
<select name="osd_font" id="osd_font" onchange="osdUpdatePreview()">
<option value="12"%s>12 px</option>
<option value="16"%s>16 px</option>
<option value="20"%s>20 px</option>
<option value="24"%s>24 px</option>
<option value="32"%s>32 px</option>
</select></label>
<p class="hint" style="margin:6px 0 8px">Trascina il riquadro giallo sul riquadro sotto per scegliere dove appare la data. Anteprima font sotto (approssimata in browser; sul video usa il font del chip).</p>
<div id="osdStage" style="position:relative;width:100%%;max-width:640px;aspect-ratio:16/9;background:linear-gradient(160deg,#1a1a1a,#333);border-radius:8px;overflow:hidden;touch-action:none;user-select:none;margin:8px 0">
<div id="osdLabel" style="position:absolute;left:2%%;top:2%%;padding:4px 8px;background:rgba(0,0,0,.75);color:#ffe14a;font-family:monospace;font-weight:700;cursor:grab;white-space:nowrap;border:1px solid #ffe14a;border-radius:3px;line-height:1.2">26/07/2026 12:00:00</div>
</div>
<input type="hidden" name="osd_x_pct" id="osd_x_pct" value="%u">
<input type="hidden" name="osd_y_pct" id="osd_y_pct" value="%u">
<div id="osdFontPreview" style="margin:8px 0;padding:12px;background:#111;color:#ffe14a;font-family:monospace;font-weight:700;border-radius:6px;display:inline-block">26/07/2026 12:00:00</div>
<p class="hint" id="osdPosHint" style="margin:4px 0 0"></p>
</fieldset>
<div id="perfWarn" class="alert err" style="display:none"></div>
<button type="submit">Salva impostazioni video</button>
</form>
<script>
function applyPreset(){
  var v=document.getElementById('preset').value;
  if(!v)return;
  var p=v.split(',');
  document.getElementById('video_w').value=p[0];
  document.getElementById('video_h').value=p[1];
  document.getElementById('video_fps').value=p[2];
  updateWarn();
}
function fpsCap(){
  var w=parseInt(document.getElementById('video_w').value)||0;
  var h=parseInt(document.getElementById('video_h').value)||0;
  if((w*h)>PIX5M) return N5MF;
  return N1080F;
}
function clampFullHdFps(){
  var fEl=document.getElementById('video_fps');
  var f=parseInt(fEl.value)||0;
  var photo=document.getElementById('rec_media').value==='1';
  if(photo){
    fEl.value=1;
    return;
  }
  var cap=fpsCap();
  if(f>cap){
    fEl.value=cap;
  }
}
function syncBeforeSave(){
  applyPreset();
  clampFullHdFps();
  alert('Reg. video è legato al canale 1. I nuovi parametri aggiornano anche il canale 1.');
  return true;
}
function onMediaChange(){
  var photo=document.getElementById('rec_media').value==='1';
  var hint=document.getElementById('mediaHint');
  var fEl=document.getElementById('video_fps');
  if(photo){
    hint.innerHTML="Modalit\u00E0 <strong>foto</strong>: JPEG <code>…_foto0001.jpg</code> a <strong>1 fps</strong> fisso. "
      +"L'AVI esportato usa 1 fps. Max IMX415: 3840\u00D72160.";
    fEl.value=1;
    clampFullHdFps();
  }else{
    hint.innerHTML="Video H264 (MP4). IMX415 nativo 3840\u00D72160. Default 1920\u00D71080 @ 30 fps. "
      +"Sopra 5 MP il tetto \u00E8 15 fps. Codec: H.264.";
  }
  updateWarn();
}
var NW=%u,NH=%u,NF=%u,N1080F=%u,N5MF=%u,PIX5M=%u,MAXW=%u,MAXH=%u;
function updateWarn(){
  var w=parseInt(document.getElementById('video_w').value)||0;
  var h=parseInt(document.getElementById('video_h').value)||0;
  var f=parseInt(document.getElementById('video_fps').value)||0;
  var photo=document.getElementById('rec_media').value==='1';
  var m=[];
  if(w>MAXW||h>MAXH)m.push("supera il massimo IMX415 "+MAXW+"\u00D7"+MAXH);
  if((w*h)>PIX5M)m.push("sopra 5 MP: encode a "+N5MF+" fps (8 MP \u00E8 3840\u00D72160)");
  else if(w>1920||h>1080)m.push("sopra il FullHD: l'ISP scala dal nativo 3840\u00D72160");
  if(photo&&f!==1)m.push("timelapse foto: solo 1 fps (verrà forzato)");
  if(!photo&&f>fpsCap())m.push("frame rate oltre il tetto di questa risoluzione ("+fpsCap()+" fps)");
  if(!photo&&f>NF)m.push("frame rate oltre il massimo ("+NF+" fps): non supportato");
  if(!photo&&m.length===0&&w*h*f>NW*NH*NF)m.push("pixel-rate superiore al profilo di riferimento "+NW+"\u00D7"+NH+"@"+NF);
  var d=document.getElementById('perfWarn');
  if(m.length){
    d.innerHTML="<strong>Attenzione:</strong> "+m.join("; ")+". Possibili rallentamenti o frame persi. Il salvataggio \u00E8 comunque consentito: prova e verifica il risultato.";
    d.style.display="block";
  }else{
    d.style.display="none";
  }
}
["video_w","video_h","video_fps","rec_media"].forEach(function(id){
  document.getElementById(id).addEventListener("input",function(){
    if(id!=='rec_media'){document.getElementById('preset').value="";clampFullHdFps();}
    updateWarn();
  });
  document.getElementById(id).addEventListener("change",function(){
    if(id!=='rec_media'){clampFullHdFps();}
    updateWarn();
  });
});
onMediaChange();
updateWarn();
(function(){
  var w=parseInt(document.getElementById('video_w').value)||0;
  var h=parseInt(document.getElementById('video_h').value)||0;
  var f=parseInt(document.getElementById('video_fps').value)||0;
  var sel=document.getElementById('preset');
  for(var i=0;i<sel.options.length;i++){
    var v=sel.options[i].value;
    if(!v)continue;
    var p=v.split(',');
    if(parseInt(p[0])===w&&parseInt(p[1])===h&&parseInt(p[2])===f){sel.selectedIndex=i;break;}
  }
})();
function osdUpdatePreview(){
  var font=parseInt(document.getElementById('osd_font').value)||20;
  var label=document.getElementById('osdLabel');
  var prev=document.getElementById('osdFontPreview');
  label.style.fontSize=font+'px';
  prev.style.fontSize=font+'px';
  var now=new Date();
  var pad=function(n){return (n<10?'0':'')+n;};
  var txt=pad(now.getDate())+'/'+pad(now.getMonth()+1)+'/'+now.getFullYear()+' '+pad(now.getHours())+':'+pad(now.getMinutes())+':'+pad(now.getSeconds());
  label.textContent=txt;
  prev.textContent=txt;
  osdApplyPct();
  document.getElementById('osdPosHint').textContent='Posizione: '+document.getElementById('osd_x_pct').value+'%% , '+document.getElementById('osd_y_pct').value+'%% (font '+font+' px)';
}
function osdApplyPct(){
  var stage=document.getElementById('osdStage');
  var label=document.getElementById('osdLabel');
  var xp=parseInt(document.getElementById('osd_x_pct').value)||0;
  var yp=parseInt(document.getElementById('osd_y_pct').value)||0;
  if(xp<0)xp=0; if(xp>100)xp=100;
  if(yp<0)yp=0; if(yp>100)yp=100;
  var maxX=Math.max(0, stage.clientWidth-label.offsetWidth);
  var maxY=Math.max(0, stage.clientHeight-label.offsetHeight);
  label.style.left=(maxX*xp/100)+'px';
  label.style.top=(maxY*yp/100)+'px';
}
function osdBindDrag(){
  var stage=document.getElementById('osdStage');
  var label=document.getElementById('osdLabel');
  var dragging=false, ox=0, oy=0;
  function setFromPointer(clientX, clientY){
    var r=stage.getBoundingClientRect();
    var x=clientX-r.left-ox;
    var y=clientY-r.top-oy;
    var maxX=Math.max(0, stage.clientWidth-label.offsetWidth);
    var maxY=Math.max(0, stage.clientHeight-label.offsetHeight);
    if(x<0)x=0; if(x>maxX)x=maxX;
    if(y<0)y=0; if(y>maxY)y=maxY;
    label.style.left=x+'px';
    label.style.top=y+'px';
    var xp=maxX>0?Math.round(100*x/maxX):0;
    var yp=maxY>0?Math.round(100*y/maxY):0;
    document.getElementById('osd_x_pct').value=String(xp);
    document.getElementById('osd_y_pct').value=String(yp);
    document.getElementById('osdPosHint').textContent='Posizione: '+xp+'%% , '+yp+'%% (font '+(document.getElementById('osd_font').value)+' px)';
  }
  label.addEventListener('pointerdown',function(e){
    dragging=true; label.setPointerCapture(e.pointerId); label.style.cursor='grabbing';
    var lr=label.getBoundingClientRect();
    ox=e.clientX-lr.left; oy=e.clientY-lr.top;
    e.preventDefault();
  });
  label.addEventListener('pointermove',function(e){ if(dragging) setFromPointer(e.clientX,e.clientY); });
  label.addEventListener('pointerup',function(){ dragging=false; label.style.cursor='grab'; });
  label.addEventListener('pointercancel',function(){ dragging=false; label.style.cursor='grab'; });
  window.addEventListener('resize',osdApplyPct);
  osdUpdatePreview();
  setInterval(osdUpdatePreview,1000);
}
osdBindDrag();
</script>
</body></html>
)HTML";

static const char *LIVE_PAGE_OK = R"HTML(
<div class="rec-bar">
<button id="btnRecStart" type="button">&#9679; Registra</button>
<button id="btnRecStop" type="button" disabled>&#9632; Stop</button>
<button id="btnImgReset" type="button">Reset immagine</button>
<span id="recStatus" class="rec-status"></span>
<span id="imgResetStatus" style="font-size:13px;color:#888"></span>
</div>
<div class="rec-bar" style="gap:18px">
<label style="display:flex;align-items:center;gap:6px;font-size:14px;color:#333">
<input id="flipV" type="checkbox" onchange="flipChange()"%s>Capovolgi</label>
<label style="display:flex;align-items:center;gap:6px;font-size:14px;color:#333">
<input id="flipH" type="checkbox" onchange="flipChange()"%s>Specchia</label>
<span id="flipStatus" style="font-size:13px;color:#888"></span>
</div>
<div id="focusBar" class="rec-bar" style="display:none">
<label>Fuoco (VCM)
<input id="focusSlider" type="range" min="0" max="1023" value="400">
</label>
<span id="focusVal">400</span>
<button id="btnFocusSave" type="button">Salva fuoco</button>
<span id="focusStatus" style="font-size:13px;color:#888"></span>
</div>
<p id="focusHint" class="live-meta" style="display:none">Modulo AF: regola il fuoco sull'immagine live, poi <strong>Salva fuoco</strong> (usato da PIR e da tutti i profili colore).</p>
<div class="live-wrap" id="liveWrap">
<img id="liveImg" alt="Live camera">
</div>
<p class="live-meta">Modulo: %s — %s</p>
<p class="live-meta">Live MJPEG %dx%d @ %d fps — anteprima (la registrazione usa il preset in Reg. video)</p>
<p class="live-meta">Registrazione SD: H264 %dx%d @ %d fps — durante la registrazione il live viene sospeso</p>
)HTML";

/* JS is NOT a printf format — LIVE_PAGE_OK grew past tail[8192] and snprintf
 * truncated the script (flipChange/onclick never attached → all Live buttons dead). */
static const char *LIVE_PAGE_JS = R"HTML(
<script>
var recPhase='idle';
var recStartBusy=false;
function recEl(id){return document.getElementById(id);}
function recIsOn(j){
  return !!(j&&typeof j==='object'&&(j.recording===true||j.active===true||j.starting===true));
}
function recSafeMsg(x){
  var s=(x==null)?'':String(x);
  if(!s) return '';
  var low=s.toLowerCase();
  if(s.charAt(0)==='<'||low.indexOf('<!doctype')>=0||low.indexOf('<html')>=0)
    return 'Sessione scaduta — ricarica la pagina';
  if(/typeerror|syntaxerror|referenceerror|failed to fetch|networkerror/i.test(s))
    return 'Errore di rete — riprova';
  if(s.length>180) return s.slice(0,180);
  return s;
}
function recParse(t,http){
  if(t==null) return {ok:false,msg:'Risposta vuota (HTTP '+(http||'?')+')'};
  var s=String(t).replace(/^\uFEFF/,'').trim();
  if(!s) return {ok:false,msg:'Risposta vuota (HTTP '+(http||'?')+')'};
  if(s.charAt(0)!=='{'&&s.charAt(0)!=='[')
    return {ok:false,msg:recSafeMsg(s)||('HTTP '+(http||'?'))};
  try{
    var j=JSON.parse(s);
    if(!j||typeof j!=='object') return {ok:false,msg:'Risposta non valida'};
    return j;
  }catch(_){return {ok:false,msg:recSafeMsg(s)||'Risposta non JSON'};}
}
function recApplyUi(on,label,stopping){
  var start=recEl('btnRecStart');
  var stop=recEl('btnRecStop');
  var st=recEl('recStatus');
  var img=recEl('liveImg');
  var wrap=recEl('liveWrap');
  if(start) start.disabled=!!on;
  if(stop) stop.disabled=!on||!!stopping;
  if(on){
    if(st){st.textContent=label||'Registrazione in corso…';st.className='rec-status rec-on';}
    if(img&&img.getAttribute('src')) img.removeAttribute('src');
    if(wrap&&!wrap.querySelector('.rec-overlay')){
      var o=document.createElement('div');
      o.className='rec-overlay';
      o.textContent='Registrazione H264 su SD… — Stop per terminare';
      wrap.appendChild(o);
    }
  }else{
    if(st){st.className='rec-status';st.textContent=label||'Pronto';}
    if(wrap){var ov=wrap.querySelector('.rec-overlay');if(ov) ov.remove();}
  }
}
async function recPost(path, timeoutMs){
  var ctrl=null, timer=null;
  var opts={method:'POST',credentials:'same-origin',redirect:'manual'};
  if(timeoutMs>0 && typeof AbortController!=='undefined'){
    ctrl=new AbortController();
    opts.signal=ctrl.signal;
    timer=setTimeout(function(){ try{ctrl.abort();}catch(ex){} }, timeoutMs);
  }
  try{
    const r=await fetch(path, opts);
    if(timer) clearTimeout(timer);
    if(r.type==='opaqueredirect'||r.status===301||r.status===302||r.status===303||r.status===307||r.status===308)
      return {ok:false,msg:'Sessione scaduta — ricarica la pagina'};
    const t=await r.text();
    return recParse(t,r.status);
  }catch(e){
    if(timer) clearTimeout(timer);
    var aborted=e&&(e.name==='AbortError'||String(e&&e.message||e).indexOf('abort')>=0);
    if(aborted) return {ok:false,timeout:true,msg:'Avvio fallito — timeout'};
    return {ok:false,msg:recSafeMsg(e&&e.message?e.message:e)||'Errore di rete — riprova'};
  }
}
async function recGetStatus(){
  try{
    const r=await fetch('/live/record/status',{credentials:'same-origin',redirect:'manual'});
    if(r.type==='opaqueredirect'||r.status===301||r.status===302||r.status===401)
      return {ok:false,auth:false};
    if(!r.ok) return {ok:false};
    const t=await r.text();
    return recParse(t,r.status);
  }catch(e){return {ok:false,msg:recSafeMsg(e&&e.message?e.message:e)};}
}
function recMsgLooksStreamBusy(m){
  if(!m) return false;
  var s=String(m).toLowerCase();
  return s.indexOf('stream')>=0 && (s.indexOf('attiv')>=0 || s.indexOf('busy')>=0);
}
async function recPoll(){
  try{
    const j=await recGetStatus();
    if(!j||j.auth===false) return;
    if(j.ok===false&&j.recording==null&&j.active==null&&j.starting==null) return;
    const on=recIsOn(j);
    var fv=recEl('flipV'), fhFlip=recEl('flipH');
    if(typeof j.flip_v==='boolean'&&fv&&!fv.dataset.user){
      fv.checked=j.flip_v;
      if(fhFlip) fhFlip.checked=j.flip_h;
    }
    var fb=recEl('focusBar');
    var fh=recEl('focusHint');
    if(j.af){
      if(fb) fb.style.display='flex';
      if(fh) fh.style.display='block';
      var sl=recEl('focusSlider');
      var sv=recEl('btnFocusSave');
      var fval=recEl('focusVal');
      if(sl&&!sl.dataset.ready){
        sl.min=String(j.focus_min||0);
        sl.max=String(j.focus_max||1023);
        sl.value=String(j.focus||400);
        if(fval) fval.textContent=sl.value;
        sl.dataset.ready='1';
      }
      if(sl) sl.disabled=on;
      if(sv) sv.disabled=on;
    }else{
      if(fb) fb.style.display='none';
      if(fh) fh.style.display='none';
    }
    if(recStartBusy && recPhase==='starting' && !on){
      return;
    }
    if(recPhase==='starting' && !on && !j.error){
      return;
    }
    if(j.finalizing && !j.last_saved){
      recPhase='idle';
      recApplyUi(false,'Pronto',false);
      var bsHold=recEl('btnRecStart');
      if(bsHold) bsHold.disabled=true;
      var imgH=recEl('liveImg');
      if(imgH&&(!imgH.getAttribute('src')||(imgH.complete&&!imgH.naturalWidth))){
        imgH.src='/live/stream?ts='+Date.now();
      }
      return;
    }
    if(recPhase==='stopping' && on && !j.finalizing){
      recApplyUi(true,'Salvataggio su SD…',true);
      return;
    }
    if(recPhase==='stopping' && (j.finalizing||!on)){
      recPhase='idle';
      recApplyUi(false,'Pronto',false);
      if(j.finalizing){
        var bsFin=recEl('btnRecStart');
        if(bsFin) bsFin.disabled=true;
      }
      var imgFin=recEl('liveImg');
      if(imgFin&&(!imgFin.getAttribute('src')||(imgFin.complete&&!imgFin.naturalWidth))){
        imgFin.src='/live/stream?ts='+Date.now();
      }
      return;
    }
    if(on){
      recPhase=j.stopping?'stopping':(j.starting?'starting':'recording');
      var lab=j.starting?'Avvio…':'Registrazione in corso…';
      if(j.enc_w&&!j.starting) lab+=' '+j.enc_w+'×'+j.enc_h+'@'+j.enc_fps;
      recApplyUi(true,lab,!!j.stopping);
    }else{
      var lab2='Pronto';
      if(j.last_saved&&j.file) lab2='Salvato: '+j.file;
      else if(j.error && !recMsgLooksStreamBusy(j.error)) lab2=recSafeMsg(j.error);
      recPhase='idle';
      recApplyUi(false,lab2,false);
      var img=recEl('liveImg');
      if(img&&(!img.getAttribute('src')||(img.complete&&!img.naturalWidth))){
        img.src='/live/stream?ts='+Date.now();
      }
    }
  }catch(e){console.error('recPoll',e);}
}
(function(){
  var bStart=recEl('btnRecStart');
  if(bStart) bStart.onclick=async function(){
    if(recStartBusy||recPhase!=='idle') return;
    recStartBusy=true;
    recPhase='starting';
    bStart.disabled=true;
    recApplyUi(true,'Avvio…',false);
    try{
      const j=await recPost('/live/record/start', 15000);
      if(recPhase==='stopping'){ recStartBusy=false; recPoll(); return; }
      if(j&&j.timeout){
        recPhase='idle';
        recStartBusy=false;
        recApplyUi(false, j.msg, false);
        recPost('/live/record/stop');
        alert(j.msg);
        liveKick();
        return;
      }
      /* A slow 200 is success. Only a real failed fetch is a network error. */
      if(j&&(j.ok===true||j.already||recIsOn(j))){
        recPhase='recording';
        recStartBusy=false;
        recApplyUi(true,'Registrazione in corso…',false);
        recPoll();
        return;
      }
      const s=await recGetStatus();
      if(s&&recIsOn(s)){
        recPhase=s.starting?'starting':'recording';
        recStartBusy=false;
        recApplyUi(true,s.starting?'Avvio…':'Registrazione in corso…',false);
        recPoll();
        return;
      }
      recPhase='idle';
      recStartBusy=false;
      var fail=recSafeMsg(j&&j.msg)||'Avvio fallito';
      if(recMsgLooksStreamBusy(fail)) fail='Avvio fallito — riprova';
      recApplyUi(false,fail,false);
      alert(fail);
      liveKick();
    }catch(e){
      recPhase='idle';
      recStartBusy=false;
      var m=recSafeMsg(e&&e.message?e.message:e)||'Avvio fallito';
      recApplyUi(false,m,false);
      alert(m);
    }
  };
  var bStop=recEl('btnRecStop');
  if(bStop) bStop.onclick=async function(){
    try{
      if(recPhase==='idle') return;
      recPhase='stopping';
      recApplyUi(true,'Stop…',true);
      const j=await recPost('/live/record/stop');
      if(!j||!j.ok){ alert(recSafeMsg(j&&j.msg)||'Stop fallito'); recPoll(); return; }
      var uiIdle=false;
      for(let i=0;i<80;i++){
        if(i) await new Promise(r=>setTimeout(r,80));
        const s=await recGetStatus();
        if(!s) continue;
        const down=s.active===false && s.stopping!==true;
        if(down && !uiIdle){
          uiIdle=true;
          recPhase='idle';
          recStartBusy=false;
          recApplyUi(false,'Pronto',false);
          liveKick();
        }
        if(down && s.finalizing && !s.last_saved){
          var bs=recEl('btnRecStart');
          if(bs) bs.disabled=true;
          continue;
        }
        if(down && (s.last_saved||!s.finalizing)){
          recPhase='idle';
          recStartBusy=false;
          var lab=s.last_saved&&s.file?('Salvato: '+s.file):'Pronto';
          recApplyUi(false,lab,false);
          if(s.last_saved) alert('Registrazione salvata su SD'+(s.file?'\n'+s.file:''));
          else alert(recSafeMsg(s&&s.error)||'Salvataggio fallito');
          return;
        }
      }
      alert('Timeout salvataggio — controlla la pagina Video');
      recPoll();
    }catch(e){
      alert(recSafeMsg(e&&e.message?e.message:e)||'Stop fallito');
      recPoll();
    }
  };
})();
function applyFlipCss(v,h){
  var img=document.getElementById('liveImg');
  if(!img) return;
  img.style.transform='scale('+(h?-1:1)+','+(v?-1:1)+')';
}
async function flipChange(){
  document.getElementById('flipV').dataset.user='1';
  document.getElementById('flipH').dataset.user='1';
  const v=document.getElementById('flipV').checked?1:0;
  const h=document.getElementById('flipH').checked?1:0;
  const st=document.getElementById('flipStatus');
  const img=document.getElementById('liveImg');
  st.textContent='Applico…';
  try{
    const r=await fetch('/live/flip',{method:'POST',credentials:'same-origin',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'vflip='+v+'&hflip='+h});
    if(r.ok){
      var j={}; try{j=JSON.parse(await r.text());}catch(e){}
      if(j.hw){
        /* Multipart MJPEG often never fires img.onload, so a CSS scale
         * left on the element cancels the encoder mirror and the picture
         * snaps back to the previous orientation. */
        applyFlipCss(0,0);
        st.textContent='Salvato — anteprima hardware';
        if(img){ img.onload=null; img.src='/live/stream?ts='+Date.now(); }
      }else{
        applyFlipCss(v,h);
        st.textContent='Salvato — anteprima CSS';
      }
    }else{
      st.textContent='Errore salvataggio';
    }
  }catch(e){st.textContent='Errore rete';}
  setTimeout(()=>{st.textContent='';},3000);
}
var focusTimer=null;
function focusPost(path,pos){
  return fetch(path,{method:'POST',credentials:'same-origin',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'pos='+encodeURIComponent(pos)});
}
(function(){
  var sl=document.getElementById('focusSlider');
  var val=document.getElementById('focusVal');
  var st=document.getElementById('focusStatus');
  sl.addEventListener('input',function(){
    val.textContent=sl.value;
    st.textContent='Sposto…';
    if(focusTimer)clearTimeout(focusTimer);
    focusTimer=setTimeout(async()=>{
      try{
        const r=await focusPost('/live/focus',sl.value);
        const j=await r.json().catch(()=>({ok:false}));
        st.textContent=j.ok?'Anteprima':(j.msg||'Errore fuoco');
      }catch(e){st.textContent='Errore rete';}
    },80);
  });
  document.getElementById('btnFocusSave').onclick=async()=>{
    st.textContent='Salvo…';
    try{
      const r=await focusPost('/live/focus/save',sl.value);
      const j=await r.json().catch(()=>({ok:false}));
      st.textContent=j.ok?('Salvato '+sl.value): (j.msg||'Salvataggio fallito');
    }catch(e){st.textContent='Errore rete';}
    setTimeout(()=>{if(st.textContent.indexOf('Salvato')===0)st.textContent='';},4000);
  };
})();
function liveKick(){
  var img=document.getElementById('liveImg');
  if(!img) return;
  if(recPhase!=='idle') return;
  img.src='/live/stream?ts='+Date.now();
}
(function(){
  var b=document.getElementById('btnImgReset');
  if(!b) return;
  b.onclick=async function(){
    var st=document.getElementById('imgResetStatus');
    if(recPhase!=='idle'){
      if(st) st.textContent='Attendi la fine della registrazione';
      return;
    }
    b.disabled=true;
    if(st) st.textContent='Ricalibro…';
    var img=document.getElementById('liveImg');
    if(img) img.removeAttribute('src');
    try{
      const j=await recPost('/live/image-reset');
      if(!j||j.ok!==true){
        if(st) st.textContent=recSafeMsg(j&&j.msg)||'Reset fallito';
        b.disabled=false;
        liveKick();
        return;
      }
      if(st) st.textContent='Immagine ricalibrata';
      liveKick();
    }catch(e){
      if(st) st.textContent='Errore rete';
      liveKick();
    }
    b.disabled=false;
    setTimeout(function(){ if(st&&st.textContent==='Immagine ricalibrata') st.textContent=''; }, 4000);
  };
})();
(function(){
  var img=document.getElementById('liveImg');
  if(!img) return;
  if(recPhase==='idle'&&!img.getAttribute('src')) liveKick();
  var fails=0;
  img.onerror=function(){
    if(fails>40) return;
    fails++;
    setTimeout(liveKick,700);
  };
  img.addEventListener('load',function(){ fails=0; });
  document.querySelectorAll('a[href="/live"]').forEach(function(a){
    a.addEventListener('click',function(e){
      if(location.pathname==='/live'){
        e.preventDefault();
        liveKick();
      }
    });
  });
})();
setInterval(recPoll,1000);
recPoll();
</script>
</body></html>
)HTML";

static esp_err_t handle_live_stream(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    if (core_recorder_manual_is_active()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "Registrazione in corso", HTTPD_RESP_USE_STRLEN);
    }
    return core_live_stream(req);
}

static void recording_path_for_web(const char *full_path, char *out, size_t out_len) {
    if (!full_path || !out || out_len == 0) {
        return;
    }
    const char *prefix = CORE_SD_MOUNT_POINT "/";
    int prec = (int)(out_len - 1);
    if (strncmp(full_path, prefix, strlen(prefix)) == 0) {
        snprintf(out, out_len, "%.*s", prec, full_path + strlen(prefix));
    } else {
        snprintf(out, out_len, "%.*s", prec, full_path);
    }
}

static void json_escape_str(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    size_t j = 0;
    if (!in) {
        out[0] = 0;
        return;
    }
    for (size_t i = 0; in[i] && j + 2 < out_len; ++i) {
        const char c = in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= out_len) {
                break;
            }
            out[j++] = '\\';
            out[j++] = c;
        } else if ((unsigned char)c < 32) {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_unauth(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    return send_json(req, "{\"ok\":false,\"msg\":\"Sessione scaduta — ricarica la pagina\"}");
}

static esp_err_t handle_live_record_status(httpd_req_t *req) {
    if (!is_authed(req)) return send_json_unauth(req);

    core_recorder_manual_status_t st;
    core_recorder_manual_get_status(&st);
    char rel[128] = {0};
    recording_path_for_web(st.last_path, rel, sizeof(rel));

    uint16_t focus = CORE_ISP_FOCUS_DEFAULT;
    uint16_t focus_min = CORE_ISP_FOCUS_MIN;
    uint16_t focus_max = CORE_ISP_FOCUS_MAX;
    const bool af = core_video_module_probed() && core_video_module_is_af();
    if (af) {
        (void)core_video_focus_get_range(&focus_min, &focus_max);
        if (!core_video_focus_get(&focus)) {
            const core_isp_cal_t *cal = core_isp_cal_get();
            const core_isp_profile_t *p = core_isp_cal_profile_for_now(cal);
            if (p) {
                focus = p->focus_pos;
            }
        }
    }

    char file_esc[160] = {0};
    char err_esc[128] = {0};
    json_escape_str(rel, file_esc, sizeof(file_esc));
    json_escape_str(st.error, err_esc, sizeof(err_esc));

    char json[896];
    snprintf(json, sizeof(json),
             "{\"active\":%s,\"recording\":%s,\"starting\":%s,\"stopping\":%s,"
             "\"finalizing\":%s,\"last_saved\":%s,\"frames\":%lu,\"file\":\"%s\",\"error\":\"%s\","
             "\"flip_v\":%s,\"flip_h\":%s,\"af\":%s,\"focus\":%u,\"focus_min\":%u,\"focus_max\":%u,"
             "\"enc_w\":%u,\"enc_h\":%u,\"enc_fps\":%u}",
             st.active ? "true" : "false",
             st.active ? "true" : "false",
             st.starting ? "true" : "false",
             st.stopping ? "true" : "false",
             st.finalizing ? "true" : "false",
             st.last_saved ? "true" : "false",
             (unsigned long)st.frame_count,
             file_esc,
             err_esc,
             (s_settings && s_settings->video_vflip) ? "true" : "false",
             (s_settings && s_settings->video_hflip) ? "true" : "false",
             af ? "true" : "false",
             (unsigned)focus, (unsigned)focus_min, (unsigned)focus_max,
             (unsigned)st.enc_w, (unsigned)st.enc_h, (unsigned)st.enc_fps);
    return send_json(req, json);
}

static esp_err_t handle_live_record_start(httpd_req_t *req) {
    if (!is_authed(req)) return send_json_unauth(req);

    char json[320];
    if (core_recorder_manual_is_active()) {
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"msg\":\"Registrazione già in corso\","
                 "\"already\":true,\"active\":true,\"recording\":true}");
        return send_json(req, json);
    }
    if (!s_settings) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Impostazioni non disponibili\"}");
        return send_json(req, json);
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"SD non montata\"}");
        return send_json(req, json);
    }
    if (!core_recorder_manual_mark_starting()) {
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"msg\":\"Registrazione già in corso\","
                 "\"already\":true,\"active\":true,\"recording\":true}");
        return send_json(req, json);
    }

    /* If Live VI is already at the record size, leave it up and let the
     * recorder swap MJPEG→H.264. Otherwise drop VI (DisableChn while 3A
     * is still up) and open again. No socket shutdown. */
#ifndef _WIN32
    {
        FILE *sf = fopen("/tmp/mrbin-snap.log", "w");
        if (sf) fclose(sf);
    }
#endif
    core_snap("http start");
#ifndef _WIN32
    if (access("/tmp/mrbin-force-busy", F_OK) == 0) {
        core_live_force_release();
    } else
#endif
    if (s_settings && core_live_can_handoff(s_settings)) {
        core_live_expect_handoff();
        core_snap("http handoff");
    } else {
        core_live_force_release();
        if (!core_live_wait_idle(400)) {
            core_live_force_release();
            if (!core_live_wait_idle(400)) {
                core_recorder_manual_clear_starting();
                snprintf(json, sizeof(json),
                         "{\"ok\":false,\"msg\":\"Camera occupata — riprova\"}");
                return send_json(req, json);
            }
        }
    }
    /* Drill: VI+3A already dropped. Return the busy JSON and let the
     * page reopen /live/stream (image comes back, sensor is free). */
    if (core_live_consume_busy_drill()) {
        core_recorder_manual_clear_starting();
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"msg\":\"Camera occupata — riprova\"}");
        return send_json(req, json);
    }
    ESP_LOGI(TAG, "Record start — handoff se VI è già alla size di registrazione");

    core_recorder_manual_status_t st;
    core_recorder_manual_get_status(&st);
    if (st.stopping) {
        core_recorder_manual_clear_starting();
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Avvio annullato\"}");
        return send_json(req, json);
    }

    if (!core_recorder_manual_start(s_settings)) {
        core_recorder_manual_clear_starting();
        core_recorder_manual_get_status(&st);
        char err_esc[128] = {0};
        json_escape_str(st.error[0] ? st.error : "Avvio registrazione fallito",
                        err_esc, sizeof(err_esc));
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", err_esc);
        return send_json(req, json);
    }

    /* Capture ready, or a hard cap so the button cannot sit on Avvio.
     * 8 s covers Live release plus a warm rec-3a arm. Still starting
     * after that is a failure, not a silent ok. Stop during the wait
     * must leave this handler so the Stop POST can finish the session. */
    bool ready = false;
    for (int i = 0; i < 80; ++i) {
        core_recorder_manual_get_status(&st);
        if (st.stopping) {
            snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Avvio annullato\"}");
            return send_json(req, json);
        }
        if (!st.active) {
            char err_esc[128] = {0};
            json_escape_str(st.error[0] ? st.error : "Avvio registrazione fallito",
                            err_esc, sizeof(err_esc));
            snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", err_esc);
            return send_json(req, json);
        }
        if (!st.starting) {
            ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!ready) {
        core_recorder_manual_stop();
        ESP_LOGW(TAG, "Avvio registrazione oltre 8 s — stop e errore al client");
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"msg\":\"Avvio troppo lento — riprova\"}");
        return send_json(req, json);
    }

    ESP_LOGI(TAG, "Registrazione manuale avviata da Web GUI");
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"msg\":\"Registrazione avviata\",\"active\":true,\"recording\":true}");
    return send_json(req, json);
}

static esp_err_t handle_live_record_stop(httpd_req_t *req) {
    if (!is_authed(req)) return send_json_unauth(req);

    char json[384];
    if (!core_recorder_manual_is_active()) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Nessuna registrazione attiva\"}");
        return send_json(req, json);
    }

    ESP_LOGI(TAG, "Stop registrazione manuale da Web GUI");
    // Non bloccare httpd sul finalize (fino a 120 s): la GUI restava morta.
    // Il client fa poll su /live/record/status fino a active=false.
    if (core_demo_is_active()) {
        core_demo_notify_external_stop();
    }
    core_recorder_manual_stop();
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"msg\":\"Stop inviato — salvataggio in corso\",\"stopping\":true}");
    return send_json(req, json);
}


static int read_post_body(httpd_req_t *req, char *buf, size_t buf_len);
static bool form_get_value(const char *body, const char *name, char *out, size_t out_len);

static esp_err_t handle_demo_rtsp(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_json_unauth(req);
    }
    if (!s_settings || !core_demo_is_active()) {
        return send_json(req, "{\"ok\":false,\"msg\":\"DEMO non attiva\"}");
    }
    char buf[32] = {0};
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Body mancante\"}");
    }
    char on_s[4] = {0};
    form_get_value(buf, "on", on_s, sizeof(on_s));
    const bool on = on_s[0] == '1';
    s_settings->rtsp_always = on;
    if (!core_settings_save(s_settings)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"NVS non salvata\"}");
    }
    if (!core_demo_set_rtsp_view(on, s_settings)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"RTSP non commutato\"}");
    }
    char url[80];
    core_rtsp_url(url, sizeof(url));
    char json[160];
    snprintf(json, sizeof(json), "{\"ok\":true,\"on\":%s,\"url\":\"%s\"}",
             core_demo_rtsp_view() ? "true" : "false", url);
    return send_json(req, json);
}

static esp_err_t handle_live_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    /* Page HTML only. Camera/RKMPI start on /live/stream (async worker). */

    if (core_video_sensor_kind() == CORE_VIDEO_SENSOR_UNKNOWN) {
        core_video_detect_sensor();
    }
    uint16_t rec_w = s_settings ? s_settings->video_width : (uint16_t)CORE_VIDEO_WIDTH_DEFAULT;
    uint16_t rec_h = s_settings ? s_settings->video_height : (uint16_t)CORE_VIDEO_HEIGHT_DEFAULT;
    uint8_t rec_fps = s_settings ? s_settings->video_fps : (uint8_t)CORE_VIDEO_FPS_DEFAULT;
    uint16_t live_w = (uint16_t)CORE_LIVE_WIDTH;
    uint16_t live_h = (uint16_t)CORE_LIVE_HEIGHT;
    core_live_preview_size(rec_w, rec_h, &live_w, &live_h);

    /* Markup only (format string). JS is sent as a raw chunk — never snprintf. */
    char tail[4096];
    if (s_settings && core_video_sensor_kind() == CORE_VIDEO_SENSOR_IMX708 &&
        (s_settings->video_bayer_mode == CORE_VIDEO_BAYER_BGGR ||
         s_settings->video_bayer_mode == CORE_VIDEO_BAYER_GRBG)) {
        s_settings->video_bayer_mode = CORE_VIDEO_BAYER_AUTO;
    }
    const char *cam_mod = core_video_sensor_label();
    char bayer_info[56];
    {
        const uint8_t bm = s_settings ? s_settings->video_bayer_mode : CORE_VIDEO_BAYER_AUTO;
        if (bm != CORE_VIDEO_BAYER_AUTO) {
            snprintf(bayer_info, sizeof(bayer_info), "Bayer %s (NVS)",
                     core_video_bayer_mode_label(bm));
        } else if (core_video_is_ready()) {
            snprintf(bayer_info, sizeof(bayer_info), "Bayer auto → %s",
                     core_video_bayer_effective_name(core_video_get_vflip(), core_video_get_hflip()));
        } else {
            snprintf(bayer_info, sizeof(bayer_info), "Bayer auto");
        }
    }
    snprintf(tail, sizeof(tail), LIVE_PAGE_OK,
             (s_settings && s_settings->video_vflip) ? " checked" : "",
             (s_settings && s_settings->video_hflip) ? " checked" : "",
             cam_mod, bayer_info,
             (int)live_w, (int)live_h, CORE_LIVE_FPS,
             (int)rec_w, (int)rec_h, (int)rec_fps);
    /* Drop a leftover mark_starting lock if no rec task is running. */
    core_recorder_manual_clear_starting();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, LIVE_HTML_HEAD);
    httpd_resp_sendstr_chunk(req, web_nav());
    httpd_resp_sendstr_chunk(req, "<h2>Live camera</h2>");
    httpd_resp_sendstr_chunk(req, tail);
    if (core_demo_is_active() && !core_demo_rtsp_view()) {
        httpd_resp_sendstr_chunk(req,
                                 "<p class='live-meta'><strong>DEMO:</strong> il video locale resta acceso "
                                 "anche con RTSP spento. Il movimento continua finche non premi Attiva RTSP.</p>");
    } else if (core_demo_is_active()) {
        char url[80];
        core_rtsp_url(url, sizeof(url));
        char note[192];
        snprintf(note, sizeof(note),
                 "<p class='live-meta'><strong>DEMO:</strong> RTSP acceso, movimento in pausa. "
                 "Stream: <code>%s</code></p>",
                 url);
        httpd_resp_sendstr_chunk(req, note);
    }
    httpd_resp_sendstr_chunk(req, LIVE_PAGE_JS);
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t handle_root(httpd_req_t *req) {
    if (is_authed(req)) return send_redirect(req, "/settings");

    char body[1024];
    if (s_boot_error_page) {
        snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>MrBin CORE</title>"
            "<style>body{font-family:sans-serif;max-width:520px;margin:40px auto}"
            "input{display:block;width:100%%;margin:8px 0;padding:8px}button{padding:10px 16px}"
            ".boot-err{color:#c00;font-weight:bold;font-size:1.05em;margin:0 0 20px;padding:12px;"
            "border:2px solid #c00;background:#fff0f0}</style></head><body><h2>MrBin CORE</h2>"
            "<p class=\"boot-err\">ERRORE BOOT PIN non riconosciuto, D1=%s D2=%s MODE=%s</p>"
            "<form method=\"POST\" action=\"/login\"><label>User</label><input name=\"user\" required>"
            "<label>Password</label><input name=\"pass\" type=\"password\" required>"
            "<button type=\"submit\">Login</button></form></body></html>",
            gpio_level_label(s_boot_error_snap.d1_level),
            gpio_level_label(s_boot_error_snap.d2_level),
            gpio_level_label(s_boot_error_snap.mode_level));
    } else {
        char q[80] = {0};
        httpd_req_get_url_query_str(req, q, sizeof(q));
        const char *page = strstr(q, "err=") ? LOGIN_ERR_HTML : LOGIN_HTML;
        strncpy(body, page, sizeof(body) - 1);
        body[sizeof(body) - 1] = 0;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool form_get_value(const char *body, const char *name, char *out, size_t out_len);

static esp_err_t handle_login(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    char user[32] = {0}, pass[64] = {0};
    if (!form_get_value(buf, "user", user, sizeof(user))) {
        char *u = strstr(buf, "user=");
        if (u) sscanf(u, "user=%31[^&]", user);
    }
    if (!form_get_value(buf, "pass", pass, sizeof(pass))) {
        char *p = strstr(buf, "pass=");
        if (p) sscanf(p, "pass=%63[^&]", pass);
    }

    if (!core_auth_check_credentials(user, pass)) {
        return send_redirect(req, "/?err=1");
    }
    char token[65];
    core_auth_create_session_token(token, sizeof(token));
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "MRBIN_SESS=%s; Path=/; HttpOnly", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_redirect(req, "/settings");
}

static esp_err_t handle_logout(httpd_req_t *req) {
    core_auth_clear_session();
    httpd_resp_set_hdr(req, "Set-Cookie", "MRBIN_SESS=; Path=/; Max-Age=0");
    return send_redirect(req, "/");
}

static void key_to_hex(const uint8_t *key, char *out, size_t out_len) {
    for (int i = 0; i < 16 && (size_t)(i * 2 + 2) < out_len; ++i) {
        snprintf(out + i * 2, 3, "%02x", key[i]);
    }
}

static void format_bytes_human(uint64_t bytes, char *out, size_t out_len) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_len, "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static void url_decode(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_len; ++i) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

static int read_post_body(httpd_req_t *req, char *buf, size_t buf_len) {
    if (!req || !buf || buf_len == 0) {
        return -1;
    }
    size_t total = 0;
    int remaining = req->content_len;
    if (remaining < 0 || (size_t)remaining >= buf_len) {
        remaining = (int)(buf_len - 1);
    }
    while (total < (size_t)remaining) {
        int got = httpd_req_recv(req, buf + total, remaining - (int)total);
        if (got <= 0) {
            return got;
        }
        total += (size_t)got;
    }
    buf[total] = 0;
    return (int)total;
}

static bool form_get_value(const char *body, const char *name, char *out, size_t out_len) {
    if (!body || !name || !out || out_len == 0) {
        return false;
    }
    char key[48];
    snprintf(key, sizeof(key), "%s=", name);
    const size_t key_len = strlen(key);
    const char *p = body;
    const char *hit = nullptr;
    while ((p = strstr(p, key)) != nullptr) {
        if (p == body || p[-1] == '&') {
            hit = p;
            break;
        }
        p += key_len;
    }
    if (!hit) {
        return false;
    }
    p = hit + key_len;
    char raw[192];
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < sizeof(raw)) {
        raw[i++] = *p++;
    }
    raw[i] = 0;
    url_decode(raw, out, out_len);
    return true;
}

static void url_encode_query(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < out_len; ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-') {
            out[o++] = c;
        } else if (c == '/') {
            out[o++] = '%';
            out[o++] = '2';
            out[o++] = 'F';
        } else {
            snprintf(out + o, out_len - o, "%%%02X", (unsigned char)c);
            o += 3;
        }
    }
    out[o] = 0;
}

#define VIDEOS_LIST_MAX 256
#define VIDEOS_PAGE_SIZE 25
#define PHOTO_SLIDESHOW_MAX_FRAMES 200

typedef struct {
    char rel[96];
    char day[16];
    char name[80];
    char when_h[40];   // "25/07/2026 14:30:52" o solo data
    char detail_h[64]; // "1:05 · 780 frame · 12.3 MB"
    char size_h[32];
    uint32_t duration_sec;
    uint32_t frames;
    bool pending;
    bool encrypted;  // header MRBI (AES su SD)
    core_clip_state_t upload_st;
} video_list_item_t;

typedef struct {
    char rel_base[96];
    char day[16];
    char name[72];
    char when_h[40];
    char detail_h[64];
    uint32_t frames;
    uint32_t duration_sec;
    uint64_t total_bytes;
    char size_h[32];
    bool has_avi;    // esiste già …_tl.avi
    bool encrypted;  // header MRBI sul primo JPEG
    uint8_t fps;     // da .meta (0 = legacy → 1; nuove sessioni sempre 1)
} photo_session_item_t;

static bool media_file_is_encrypted(const char *abs_path) {
    if (!abs_path || !abs_path[0]) {
        return false;
    }
    FILE *fp = fopen(abs_path, "rb");
    if (!fp) {
        return false;
    }
    uint32_t magic = 0;
    size_t n = fread(&magic, 1, sizeof(magic), fp);
    fclose(fp);
    return n == sizeof(magic) && magic == CORE_CRYPTO_MAGIC;
}

static bool photo_session_avi_exists(const char *rel_base) {
    if (!rel_base || !rel_base[0]) {
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s_tl.avi", CORE_SD_MOUNT_POINT, rel_base);
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 1024);
}

static void format_day_folder(const char *day8, char *out, size_t out_len) {
    if (!day8 || !out || out_len == 0) {
        return;
    }
    if (strlen(day8) == 8) {
        snprintf(out, out_len, "%.2s/%.2s/%.4s", day8, day8 + 2, day8 + 4);
    } else {
        snprintf(out, out_len, "%s", day8);
    }
}

static void format_duration_h(uint32_t sec, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    if (sec >= 3600U) {
        snprintf(out, out_len, "%u:%02u:%02u",
                 (unsigned)(sec / 3600U), (unsigned)((sec % 3600U) / 60U), (unsigned)(sec % 60U));
    } else {
        snprintf(out, out_len, "%u:%02u", (unsigned)(sec / 60U), (unsigned)(sec % 60U));
    }
}

// Estrae HHMMSS da nomi tipo 53531_073_20261011_143052[.mp4|_part01|_foto0001]
// (anche legacy senza YYYYMMDD: …_143052.mp4).
static bool media_name_parse_time(const char *fname, char *time_disp, size_t time_len) {
    if (!fname || !time_disp || time_len < 9) {
        return false;
    }
    time_disp[0] = 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", fname);
    char *dot = strrchr(buf, '.');
    if (dot) {
        *dot = 0;
    }
    char *cut = strstr(buf, "_part");
    if (cut) {
        *cut = 0;
    }
    cut = strstr(buf, "_foto");
    if (cut) {
        *cut = 0;
    }
    // Export timelapse: …_HHMMSS_tl.avi / …_tl.mp4
    size_t blen = strlen(buf);
    if (blen > 3 && strcmp(buf + blen - 3, "_tl") == 0) {
        buf[blen - 3] = 0;
    }
    cut = strstr(buf, "_p");
    if (cut && strlen(cut) == 6) {  // legacy _p0001 già tolto dal base; qui difensivo
        bool digits = true;
        for (int i = 2; i < 6; ++i) {
            if (cut[i] < '0' || cut[i] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            *cut = 0;
        }
    }
    size_t len = strlen(buf);
    if (len < 7 || buf[len - 7] != '_') {
        return false;
    }
    const char *hhmmss = buf + len - 6;
    for (int i = 0; i < 6; ++i) {
        if (hhmmss[i] < '0' || hhmmss[i] > '9') {
            return false;
        }
    }
    snprintf(time_disp, time_len, "%.2s:%.2s:%.2s", hhmmss, hhmmss + 2, hhmmss + 4);
    return true;
}

static void media_format_when(const char *day8, const char *fname, char *out, size_t out_len) {
    char day_h[16];
    format_day_folder(day8, day_h, sizeof(day_h));
    char time_h[16] = {0};
    if (media_name_parse_time(fname, time_h, sizeof(time_h))) {
        snprintf(out, out_len, "%s %s", day_h, time_h);
    } else {
        snprintf(out, out_len, "%s", day_h);
    }
}

static void media_format_detail(uint32_t duration_sec, uint32_t frames, const char *size_h,
                                char *out, size_t out_len) {
    char dur_h[16] = {0};
    if (duration_sec > 0) {
        format_duration_h(duration_sec, dur_h, sizeof(dur_h));
    }
    if (dur_h[0] && frames > 0 && size_h && size_h[0]) {
        snprintf(out, out_len, "%s · %lu frame · %s", dur_h, (unsigned long)frames, size_h);
    } else if (dur_h[0] && frames > 0) {
        snprintf(out, out_len, "%s · %lu frame", dur_h, (unsigned long)frames);
    } else if (frames > 0 && size_h && size_h[0]) {
        snprintf(out, out_len, "%lu frame · %s", (unsigned long)frames, size_h);
    } else if (dur_h[0] && size_h && size_h[0]) {
        snprintf(out, out_len, "%s · %s", dur_h, size_h);
    } else if (size_h && size_h[0]) {
        snprintf(out, out_len, "%s", size_h);
    } else {
        out[0] = 0;
    }
}

static uint32_t media_rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t media_rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Scan a buffer of MP4 atoms for mvhd → duration_sec. */
static bool media_mvhd_duration_from_buf(const uint8_t *buf, size_t n, uint32_t *out_sec) {
    if (!buf || n < 16 || !out_sec) {
        return false;
    }
    size_t off = 0;
    while (off + 8 <= n) {
        uint32_t atom_size = media_rd_be32(buf + off);
        const char *typ = (const char *)(buf + off + 4);
        uint64_t payload_off = off + 8;
        uint64_t atom_end;
        if (atom_size == 1) {
            if (off + 16 > n) {
                break;
            }
            atom_end = ((uint64_t)media_rd_be32(buf + off + 8) << 32) |
                       (uint64_t)media_rd_be32(buf + off + 12);
            if (atom_end < 16) {
                break;
            }
            atom_end += off;
            payload_off = off + 16;
        } else if (atom_size == 0) {
            atom_end = n;
        } else {
            atom_end = off + atom_size;
        }
        if (atom_end > n || atom_end <= payload_off) {
            break;
        }
        if (memcmp(typ, "moov", 4) == 0 || memcmp(typ, "trak", 4) == 0 ||
            memcmp(typ, "mdia", 4) == 0) {
            if (media_mvhd_duration_from_buf(buf + payload_off, (size_t)(atom_end - payload_off),
                                            out_sec)) {
                return true;
            }
        } else if (memcmp(typ, "mvhd", 4) == 0) {
            const uint8_t *b = buf + payload_off;
            size_t blen = (size_t)(atom_end - payload_off);
            if (blen < 20) {
                return false;
            }
            uint8_t ver = b[0];
            uint32_t timescale;
            uint64_t duration;
            if (ver == 1) {
                if (blen < 32) {
                    return false;
                }
                timescale = media_rd_be32(b + 20);
                duration = ((uint64_t)media_rd_be32(b + 24) << 32) | (uint64_t)media_rd_be32(b + 28);
            } else {
                timescale = media_rd_be32(b + 12);
                duration = media_rd_be32(b + 16);
            }
            if (timescale == 0) {
                return false;
            }
            uint64_t sec = (duration + (uint64_t)timescale / 2ULL) / (uint64_t)timescale;
            if (sec == 0 && duration > 0) {
                sec = 1;
            }
            if (sec > 24ULL * 3600ULL * 7ULL) {
                return false;
            }
            *out_sec = (uint32_t)sec;
            return true;
        }
        off = (size_t)atom_end;
    }
    return false;
}

/* Plain MP4 (ftyp…): prefer moov-at-end window, then head. Encrypted MRBI skipped. */
static bool media_probe_mp4_duration_sec(const char *path, uint32_t *out_sec) {
    if (!path || !out_sec) {
        return false;
    }
    *out_sec = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t head[32];
    if (fread(head, 1, sizeof(head), fp) != sizeof(head)) {
        fclose(fp);
        return false;
    }
    if (head[0] == 'M' && head[1] == 'R' && head[2] == 'B' && head[3] == 'I') {
        fclose(fp);
        return false;
    }
    if (!(head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p')) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long fsz = ftell(fp);
    if (fsz < 64) {
        fclose(fp);
        return false;
    }
    const size_t win = 512 * 1024;
    uint8_t *buf = (uint8_t *)malloc(win);
    if (!buf) {
        fclose(fp);
        return false;
    }
    bool ok = false;
    long tail = (fsz > (long)win) ? (fsz - (long)win) : 0;
    size_t tail_n = (size_t)(fsz - tail);
    if (fseek(fp, tail, SEEK_SET) == 0 && fread(buf, 1, tail_n, fp) == tail_n) {
        ok = media_mvhd_duration_from_buf(buf, tail_n, out_sec);
        /* Tail window may start mid-atom — scan for mvhd fourcc. */
        if (!ok) {
            for (size_t i = 4; i + 24 < tail_n; ++i) {
                if (buf[i] == 'm' && buf[i + 1] == 'v' && buf[i + 2] == 'h' &&
                    buf[i + 3] == 'd') {
                    ok = media_mvhd_duration_from_buf(buf + i - 4, tail_n - (i - 4), out_sec);
                    if (ok) {
                        break;
                    }
                }
            }
        }
    }
    if (!ok) {
        size_t head_n = (fsz < (long)win) ? (size_t)fsz : win;
        if (fseek(fp, 0, SEEK_SET) == 0 && fread(buf, 1, head_n, fp) == head_n) {
            ok = media_mvhd_duration_from_buf(buf, head_n, out_sec);
        }
    }
    free(buf);
    fclose(fp);
    return ok;
}

/* AVI avih: dwMicroSecPerFrame @0, dwTotalFrames @16 → seconds. */
static bool media_probe_avi_duration_sec(const char *path, uint32_t *out_sec) {
    if (!path || !out_sec) {
        return false;
    }
    *out_sec = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 12 || !(buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
                    buf[8] == 'A' && buf[9] == 'V' && buf[10] == 'I' && buf[11] == ' ')) {
        return false;
    }
    for (size_t i = 0; i + 8 <= n; ++i) {
        if (buf[i] == 'a' && buf[i + 1] == 'v' && buf[i + 2] == 'i' && buf[i + 3] == 'h') {
            if (i + 8 + 20 > n) {
                return false;
            }
            const uint8_t *avih = buf + i + 8;
            uint32_t us_pf = media_rd_le32(avih + 0);
            uint32_t frames = media_rd_le32(avih + 16);
            if (us_pf == 0 || frames == 0) {
                return false;
            }
            uint64_t sec = ((uint64_t)frames * (uint64_t)us_pf + 500000ULL) / 1000000ULL;
            if (sec == 0) {
                sec = 1;
            }
            if (sec > 24ULL * 3600ULL * 7ULL) {
                return false;
            }
            *out_sec = (uint32_t)sec;
            return true;
        }
    }
    return false;
}

/* Clips in /sdcard/mrbin/ (PIR flat dir, not DDMMYYYY/…). */
static void media_format_when_mrbin(const char *fname, const struct stat *fst, char *out,
                                    size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    char time_h[16] = {0};
    if (fname && media_name_parse_time(fname, time_h, sizeof(time_h))) {
        snprintf(out, out_len, "PIR · %s", time_h);
        return;
    }
    if (fname && strncmp(fname, "PIR_", 4) == 0) {
        snprintf(out, out_len, "PIR · %s", fname);
        return;
    }
    if (fst) {
        struct tm t;
        localtime_r(&fst->st_mtime, &t);
        snprintf(out, out_len, "PIR · %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        return;
    }
    if (fname && fname[0]) {
        snprintf(out, out_len, "mrbin/%s", fname);
    } else {
        snprintf(out, out_len, "mrbin");
    }
}

static bool videos_name_is_listable_media(const char *name, bool *is_mp4, bool *is_avi, bool *is_tmp) {
    if (!name || name[0] == '.' || name[0] == 0) {
        return false;
    }
    if (is_tmp) {
        *is_tmp = (strstr(name, ".mp4.tmp") != nullptr);
    }
    bool tmp = is_tmp && *is_tmp;
    size_t name_len = strlen(name);
    if (!tmp && name_len >= 5 && strcmp(name + name_len - 5, ".part") == 0) {
        return false;
    }
    bool mp4 = (!tmp && name_len >= 4 && strcmp(name + name_len - 4, ".mp4") == 0);
    bool avi = (name_len >= 4 && strcmp(name + name_len - 4, ".avi") == 0);
    if (is_mp4) {
        *is_mp4 = mp4;
    }
    if (is_avi) {
        *is_avi = avi;
    }
    return tmp || mp4 || avi;
}

static bool videos_list_push_file(video_list_item_t *items, size_t *count, size_t max_items,
                                  const char *rel, const char *when_day8, const char *fname,
                                  const char *fpath, bool mrbin_flat_when) {
    if (!items || !count || !rel || !fname || !fpath || *count >= max_items) {
        return false;
    }
    bool is_tmp = false;
    bool is_mp4 = false;
    bool is_avi = false;
    if (!videos_name_is_listable_media(fname, &is_mp4, &is_avi, &is_tmp)) {
        return false;
    }
    struct stat fst;
    if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode)) {
        return false;
    }
    const off_t min_sz = is_avi ? (off_t)1024 : (off_t)CORE_CRYPTO_FILE_HEADER;
    if (!is_tmp && fst.st_size < min_sz) {
        return false;
    }

    video_list_item_t *it = &items[(*count)++];
    snprintf(it->rel, sizeof(it->rel), "%s", rel);
    if (when_day8) {
        snprintf(it->day, sizeof(it->day), "%s", when_day8);
    } else {
        it->day[0] = 0;
    }
    snprintf(it->name, sizeof(it->name), "%s", fname);
    format_bytes_human((uint64_t)fst.st_size, it->size_h, sizeof(it->size_h));
    it->pending = is_tmp;
    it->duration_sec = 0;
    it->frames = 0;
    it->encrypted = !is_tmp && (is_mp4 || is_avi) && media_file_is_encrypted(fpath);
    it->upload_st = is_mp4 ? core_clip_queue_state_for_mp4(fpath) : CORE_CLIP_NONE;
    if (mrbin_flat_when) {
        media_format_when_mrbin(fname, &fst, it->when_h, sizeof(it->when_h));
    } else if (when_day8) {
        media_format_when(when_day8, fname, it->when_h, sizeof(it->when_h));
    } else {
        it->when_h[0] = 0;
    }
    if (!is_tmp) {
        uint32_t meta_fps = 0;
        (void)core_sd_read_rec_meta(fpath, &it->duration_sec, &it->frames, &meta_fps);
        if (it->duration_sec == 0 && it->frames > 0) {
            uint32_t fps =
                (meta_fps >= 1 && meta_fps <= 60) ? meta_fps : (uint32_t)CORE_VIDEO_FPS_DEFAULT;
            it->duration_sec = (it->frames + fps / 2U) / fps;
        }
        /* PIR/rkmuxer clips often lack .meta — read duration from container. */
        if (it->duration_sec == 0 && !it->encrypted) {
            uint32_t probed = 0;
            if (is_mp4 && media_probe_mp4_duration_sec(fpath, &probed)) {
                it->duration_sec = probed;
            } else if (is_avi && media_probe_avi_duration_sec(fpath, &probed)) {
                it->duration_sec = probed;
            }
        }
    }
    media_format_detail(it->duration_sec, it->frames, it->size_h, it->detail_h,
                        sizeof(it->detail_h));
    return true;
}

static void videos_collect_mrbin_flat(video_list_item_t *items, size_t *count, size_t max_items) {
    if (!items || !count) {
        return;
    }
    char dirpath[72];
    snprintf(dirpath, sizeof(dirpath), "%s/mrbin", CORE_SD_MOUNT_POINT);
    struct stat dst;
    if (stat(dirpath, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
        return;
    }
    DIR *subdir = opendir(dirpath);
    if (!subdir) {
        return;
    }
    struct dirent *f;
    while ((f = readdir(subdir)) != nullptr && *count < max_items) {
        char rel[96];
        char fpath[160];
        snprintf(rel, sizeof(rel), "mrbin/%.70s", f->d_name);
        snprintf(fpath, sizeof(fpath), "%s/%.90s", dirpath, f->d_name);
        (void)videos_list_push_file(items, count, max_items, rel, "mrbin", f->d_name, fpath, true);
    }
    closedir(subdir);
}

static int photo_session_cmp_desc(const void *a, const void *b) {
    const photo_session_item_t *pa = (const photo_session_item_t *)a;
    const photo_session_item_t *pb = (const photo_session_item_t *)b;
    return strcmp(pb->rel_base, pa->rel_base);
}

static bool photo_frame_parse_suffix(const char *fname, const char *marker, size_t marker_len,
                                       int num_digits, char *base_out, size_t base_len) {
    const char *m = strstr(fname, marker);
    if (!m) {
        return false;
    }
    const char *dot = strrchr(fname, '.');
    if (!dot || strcmp(dot, ".jpg") != 0) {
        return false;
    }
    if ((size_t)(dot - m) != marker_len + (size_t)num_digits) {
        return false;
    }
    for (int i = (int)marker_len; i < (int)marker_len + num_digits; ++i) {
        if (m[i] < '0' || m[i] > '9') {
            return false;
        }
    }
    size_t blen = (size_t)(m - fname);
    if (blen == 0 || blen >= base_len) {
        return false;
    }
    memcpy(base_out, fname, blen);
    base_out[blen] = '\0';
    return true;
}

static bool photo_frame_parse_base(const char *fname, char *base_out, size_t base_len) {
    // Schema: {CORE_ID}_{seq}_{YYYYMMDD}_{HHMMSS}_foto0001.jpg (stesso base dei video)
    if (photo_frame_parse_suffix(fname, "_foto", 5, 4, base_out, base_len)) {
        return true;
    }
    // Legacy timelapse precedente al fix parser/naming
    return photo_frame_parse_suffix(fname, "_p", 2, 4, base_out, base_len);
}

static photo_session_item_t *photo_session_find(photo_session_item_t *items, size_t count,
                                                  const char *day, const char *base) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i].day, day) == 0 && strcmp(items[i].name, base) == 0) {
            return &items[i];
        }
    }
    return nullptr;
}

static size_t photos_collect_sessions(photo_session_item_t *items, size_t max_items) {
    size_t count = 0;
    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        return 0;
    }

    struct dirent *day;
    while ((day = readdir(root)) != nullptr) {
        if (day->d_name[0] == '.' || strlen(day->d_name) != 8) {
            continue;
        }
        char daypath[64];
        snprintf(daypath, sizeof(daypath), "%s/%.40s", CORE_SD_MOUNT_POINT, day->d_name);
        struct stat day_st;
        if (stat(daypath, &day_st) != 0 || !S_ISDIR(day_st.st_mode)) {
            continue;
        }

        DIR *subdir = opendir(daypath);
        if (!subdir) {
            continue;
        }
        struct dirent *f;
        while ((f = readdir(subdir)) != nullptr) {
            char base[64];
            if (!photo_frame_parse_base(f->d_name, base, sizeof(base))) {
                continue;
            }
            char fpath[160];
            snprintf(fpath, sizeof(fpath), "%.60s/%.90s", daypath, f->d_name);
            struct stat fst;
            if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size <= 0) {
                continue;
            }

            photo_session_item_t *sess = photo_session_find(items, count, day->d_name, base);
            if (!sess) {
                if (count >= max_items) {
                    continue;
                }
                sess = &items[count++];
                snprintf(sess->rel_base, sizeof(sess->rel_base), "%.40s/%.60s", day->d_name, base);
                snprintf(sess->day, sizeof(sess->day), "%s", day->d_name);
                snprintf(sess->name, sizeof(sess->name), "%s", base);
                sess->frames = 0;
                sess->duration_sec = 0;
                sess->total_bytes = 0;
                sess->has_avi = false;
                sess->encrypted = media_file_is_encrypted(fpath);
                sess->fps = 0;
                media_format_when(day->d_name, base, sess->when_h, sizeof(sess->when_h));
            }
            sess->frames++;
            sess->total_bytes += (uint64_t)fst.st_size;
        }
        closedir(subdir);
    }
    closedir(root);

    for (size_t i = 0; i < count; ++i) {
        format_bytes_human(items[i].total_bytes, items[i].size_h, sizeof(items[i].size_h));
        char full_base[160];
        snprintf(full_base, sizeof(full_base), "%s/%s", CORE_SD_MOUNT_POINT, items[i].rel_base);
        uint32_t meta_dur = 0;
        uint32_t meta_frames = 0;
        uint32_t meta_fps = 0;
        if (core_sd_read_rec_meta(full_base, &meta_dur, &meta_frames, &meta_fps)) {
            items[i].duration_sec = meta_dur;
            if (meta_frames > 0) {
                items[i].frames = meta_frames;
            }
            if (meta_fps >= 1 && meta_fps <= CORE_REC_PHOTO_META_FPS_MAX) {
                items[i].fps = (uint8_t)meta_fps;
            }
        }
        if (items[i].fps == 0) {
            items[i].fps = 1;  // legacy senza campo fps
        }
        if (items[i].duration_sec == 0 && items[i].frames > 0) {
            items[i].duration_sec = (items[i].frames + items[i].fps - 1) / items[i].fps;
        }
        items[i].has_avi = photo_session_avi_exists(items[i].rel_base);
        media_format_detail(items[i].duration_sec, items[i].frames, items[i].size_h,
                            items[i].detail_h, sizeof(items[i].detail_h));
    }
    if (count > 1) {
        qsort(items, count, sizeof(items[0]), photo_session_cmp_desc);
    }
    return count;
}

static bool photo_rel_path_valid(const char *rel) {
    if (!rel || rel[0] == 0 || strstr(rel, "..") != nullptr) {
        return false;
    }
    const char *slash = strchr(rel, '/');
    if (!slash || slash == rel || slash[1] == 0) {
        return false;
    }
    if ((size_t)(slash - rel) != 8) {
        return false;
    }
    const char *name = slash + 1;
    size_t name_len = strlen(name);
    if (name_len < 8 || name_len > 80) {
        return false;
    }
    if (strcmp(name + name_len - 4, ".jpg") != 0) {
        return false;
    }
    char base[80];
    return photo_frame_parse_base(name, base, sizeof(base));
}

static bool photo_session_base_valid(const char *base) {
    if (!base || base[0] == 0 || strstr(base, "..") != nullptr) {
        return false;
    }
    const char *slash = strchr(base, '/');
    if (!slash || slash == base || slash[1] == 0) {
        return false;
    }
    if ((size_t)(slash - base) != 8) {
        return false;
    }
    const char *name = slash + 1;
    if (strlen(name) < 3 || strlen(name) > 72) {
        return false;
    }
    if (strchr(name, '/') != nullptr || strchr(name, '.') != nullptr) {
        return false;
    }
    return true;
}

static bool photo_query_get_base(httpd_req_t *req, char *base, size_t base_len) {
    char query[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[128] = {0};
    if (httpd_query_key_value(query, "b", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(raw, base, base_len);
    return photo_session_base_valid(base);
}

static bool photo_query_get_rel(httpd_req_t *req, char *rel, size_t rel_len) {
    char query[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[128] = {0};
    if (httpd_query_key_value(query, "f", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(raw, rel, rel_len);
    return photo_rel_path_valid(rel);
}

static int photo_session_collect_frames(const char *rel_base, char frames[][96], int max_frames) {
    if (!photo_session_base_valid(rel_base) || max_frames <= 0) {
        return 0;
    }
    const char *slash = strchr(rel_base, '/');
    char daypath[64];
    snprintf(daypath, sizeof(daypath), "%s/%.8s", CORE_SD_MOUNT_POINT, rel_base);
    const char *prefix = slash + 1;

    DIR *subdir = opendir(daypath);
    if (!subdir) {
        return 0;
    }

    int count = 0;
    struct dirent *f;
    while ((f = readdir(subdir)) != nullptr && count < max_frames) {
        if (f->d_name[0] == '.') {
            continue;
        }
        if (strncmp(f->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        char base[64];
        if (!photo_frame_parse_base(f->d_name, base, sizeof(base))) {
            continue;
        }
        if (strcmp(base, prefix) != 0) {
            continue;
        }
        snprintf(frames[count], 96, "%.8s/%.80s", rel_base, f->d_name);
        count++;
    }
    closedir(subdir);

    // Sort by filename (foto0001, foto0002, …)
    for (int i = 0; i < count - 1; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (strcmp(frames[i], frames[j]) > 0) {
                char tmp[96];
                memcpy(tmp, frames[i], sizeof(tmp));
                memcpy(frames[i], frames[j], sizeof(frames[i]));
                memcpy(frames[j], tmp, sizeof(tmp));
            }
        }
    }
    return count;
}

static esp_err_t send_plain_err(httpd_req_t *req, const char *status, const char *msg);

static void *web_alloc_psram(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static bool photo_session_delete(const char *rel_base) {
    if (!photo_session_base_valid(rel_base)) {
        return false;
    }
    char (*frame_rels)[96] = (char (*)[96])web_alloc_psram(PHOTO_SLIDESHOW_MAX_FRAMES * 96);
    if (!frame_rels) {
        return false;
    }
    int frame_count = photo_session_collect_frames(rel_base, frame_rels, PHOTO_SLIDESHOW_MAX_FRAMES);
    if (frame_count <= 0) {
        free(frame_rels);
        return false;
    }
    int removed = 0;
    for (int i = 0; i < frame_count; ++i) {
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, frame_rels[i]);
        if (remove(path) == 0) {
            removed++;
        } else {
            ESP_LOGW(TAG, "remove %s fallito (errno=%d)", path, errno);
        }
    }
    free(frame_rels);
    char meta_path[192];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", CORE_SD_MOUNT_POINT, rel_base);
    (void)remove(meta_path);
    char tl_path[192];
    snprintf(tl_path, sizeof(tl_path), "%s/%s_tl.avi", CORE_SD_MOUNT_POINT, rel_base);
    (void)remove(tl_path);
    snprintf(tl_path, sizeof(tl_path), "%s/%s_tl.mp4", CORE_SD_MOUNT_POINT, rel_base);
    (void)remove(tl_path);
    ESP_LOGI(TAG, "Timelapse eliminato %s (%d/%d JPEG)", rel_base, removed, frame_count);
    return removed > 0;
}

static esp_err_t photos_send_jpeg_file(httpd_req_t *req, const char *rel, bool as_download) {
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, rel);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "JPEG non trovato: %s", path);
        return send_plain_err(req, "404 Not Found", "File non trovato");
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return send_plain_err(req, "500 Internal Server Error", "Seek fallito");
    }
    long fsize = ftell(fp);
    if (fsize <= 0 || fsize > (long)(CORE_PHOTO_JPEG_WEB_MAX_BYTES + CORE_CRYPTO_FILE_HEADER)) {
        ESP_LOGW(TAG, "JPEG dimensione non valida: %s (%ld B, max %u)",
                 path, fsize, (unsigned)CORE_PHOTO_JPEG_WEB_MAX_BYTES);
        fclose(fp);
        return send_plain_err(req, "500 Internal Server Error", "Dimensione JPEG non valida");
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return send_plain_err(req, "500 Internal Server Error", "Seek fallito");
    }

    uint32_t magic = 0;
    bool encrypted = (fread(&magic, 1, sizeof(magic), fp) == sizeof(magic) &&
                      magic == CORE_CRYPTO_MAGIC);
    core_crypto_ctx_t ctx = {};
    uint64_t plain_len = (uint64_t)fsize;
    if (encrypted) {
        if (!s_settings || !s_settings->aes_key_valid) {
            fclose(fp);
            return send_plain_err(req, "503 Service Unavailable", "Chiave AES non disponibile");
        }
        uint8_t iv[16];
        if (fread(iv, 1, sizeof(iv), fp) != sizeof(iv)) {
            fclose(fp);
            return send_plain_err(req, "400 Bad Request", "Header cifratura incompleto");
        }
        if (fsize <= (long)CORE_CRYPTO_FILE_HEADER) {
            fclose(fp);
            return send_plain_err(req, "400 Bad Request", "JPEG cifrato vuoto");
        }
        plain_len = (uint64_t)fsize - CORE_CRYPTO_FILE_HEADER;
        if (!core_crypto_init_ctx(&ctx, s_settings->aes_key, iv)) {
            fclose(fp);
            return send_plain_err(req, "500 Internal Server Error", "Init decifratura fallita");
        }
        uint8_t peek[16] = {0};
        long peek_pos = ftell(fp);
        size_t peek_n = fread(peek, 1, sizeof(peek), fp);
        fseek(fp, peek_pos, SEEK_SET);
        if (peek_n >= 2 && peek[0] == 0xFF && peek[1] == 0xD8) {
            ESP_LOGW(TAG, "JPEG: payload già in chiaro dopo header MRBI");
            encrypted = false;
            core_crypto_deinit_ctx(&ctx);
        }
    } else {
        if (fseek(fp, 0, SEEK_SET) != 0) {
            fclose(fp);
            return send_plain_err(req, "500 Internal Server Error", "Seek fallito");
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    if (as_download) {
        const char *slash = strrchr(rel, '/');
        const char *fname = slash ? slash + 1 : rel;
        char disp[160];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }

    const size_t chunk = 32 * 1024;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)malloc(chunk);
    }
    if (!buf) {
        if (encrypted) {
            core_crypto_deinit_ctx(&ctx);
        }
        fclose(fp);
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    bool client_gone = false;
    uint64_t sent = 0;
    while (sent < plain_len) {
        size_t want = (size_t)(plain_len - sent);
        if (want > chunk) {
            want = chunk;
        }
        size_t n = fread(buf, 1, want, fp);
        if (n == 0) {
            ESP_LOGW(TAG, "JPEG lettura interrotta: %s (%llu/%llu B)", path,
                     (unsigned long long)sent, (unsigned long long)plain_len);
            if (encrypted) {
                core_crypto_deinit_ctx(&ctx);
            }
            fclose(fp);
            free(buf);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
        if (encrypted && !core_crypto_crypt_at(&ctx, buf, n, sent)) {
            core_crypto_deinit_ctx(&ctx);
            fclose(fp);
            free(buf);
            httpd_resp_send_chunk(req, nullptr, 0);
            return send_plain_err(req, "500 Internal Server Error", "Decifratura JPEG fallita");
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            client_gone = true;
            break;
        }
        sent += n;
    }
    if (encrypted) {
        core_crypto_deinit_ctx(&ctx);
    }
    fclose(fp);
    free(buf);
    if (!client_gone) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    return ESP_OK;
}

static int video_list_item_cmp_desc(const void *a, const void *b) {
    const video_list_item_t *va = (const video_list_item_t *)a;
    const video_list_item_t *vb = (const video_list_item_t *)b;
    return strcmp(vb->rel, va->rel);
}

static size_t videos_collect_list(video_list_item_t *items, size_t max_items) {
    size_t count = 0;
    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGW(TAG, "opendir %s fallito (errno=%d)", CORE_SD_MOUNT_POINT, errno);
        return 0;
    }

    struct dirent *day;
    while ((day = readdir(root)) != nullptr && count < max_items) {
        if (day->d_name[0] == '.' || day->d_name[0] == 0 || strlen(day->d_name) != 8) {
            continue;
        }
        char daypath[64];
        snprintf(daypath, sizeof(daypath), "%s/%.40s", CORE_SD_MOUNT_POINT, day->d_name);
        struct stat day_st;
        if (stat(daypath, &day_st) != 0 || !S_ISDIR(day_st.st_mode)) {
            continue;
        }

        DIR *subdir = opendir(daypath);
        if (!subdir) {
            continue;
        }
        struct dirent *f;
        while ((f = readdir(subdir)) != nullptr && count < max_items) {
            char rel[96];
            char fpath[160];
            snprintf(rel, sizeof(rel), "%.40s/%.70s", day->d_name, f->d_name);
            snprintf(fpath, sizeof(fpath), "%.60s/%.90s", daypath, f->d_name);
            (void)videos_list_push_file(items, &count, max_items, rel, day->d_name, f->d_name,
                                        fpath, false);
        }
        closedir(subdir);
    }
    closedir(root);

    videos_collect_mrbin_flat(items, &count, max_items);

    if (count > 1) {
        qsort(items, count, sizeof(items[0]), video_list_item_cmp_desc);
    }
    return count;
}

static bool video_rel_path_valid(const char *rel);

static bool video_query_get_rel(httpd_req_t *req, char *rel, size_t rel_len) {
    char query[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[128] = {0};
    if (httpd_query_key_value(query, "f", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(raw, rel, rel_len);
    return video_rel_path_valid(rel);
}

static bool video_rel_path_valid(const char *rel) {
    if (!rel || rel[0] == 0) {
        return false;
    }
    if (strstr(rel, "..") != nullptr) {
        return false;
    }
    const char *slash = strchr(rel, '/');
    if (!slash || slash == rel || slash[1] == 0) {
        return false;
    }
    size_t day_len = (size_t)(slash - rel);
    if (day_len == 5 && strncmp(rel, "mrbin", 5) == 0) {
        const char *name = slash + 1;
        size_t name_len = strlen(name);
        if (name_len < 5 || name_len > 80 || strchr(name, '/') != nullptr) {
            return false;
        }
        if (strstr(name, ".tmp") != nullptr) {
            return false;
        }
        bool ok_ext = (strcmp(name + name_len - 4, ".mp4") == 0) ||
                      (name_len >= 4 && strcmp(name + name_len - 4, ".avi") == 0);
        if (!ok_ext) {
            return false;
        }
        for (const char *p = rel; *p; ++p) {
            char c = *p;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                c == '_' || c == '.' || c == '/') {
                continue;
            }
            return false;
        }
        return true;
    }
    if (day_len != 8) {
        return false;
    }
    const char *name = slash + 1;
    size_t name_len = strlen(name);
    if (name_len < 5 || name_len > 80) {
        return false;
    }
    if (strstr(name, ".tmp") != nullptr) {
        return false;
    }
    bool ok_ext = (strcmp(name + name_len - 4, ".mp4") == 0) ||
                  (name_len >= 4 && strcmp(name + name_len - 4, ".avi") == 0);
    if (!ok_ext) {
        return false;
    }
    for (const char *p = rel; *p; ++p) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '_' || c == '.' || c == '/') {
            continue;
        }
        return false;
    }
    return true;
}

static void videos_append_flash(char *body, size_t *off, size_t max, httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }
    char val[16] = {0};
    const bool ok_del = httpd_query_key_value(query, "deleted", val, sizeof(val)) == ESP_OK ||
                        httpd_query_key_value(query, "vdel", val, sizeof(val)) == ESP_OK;
    if (ok_del) {
        *off += snprintf(body + *off, max - *off,
                           "<div class='alert ok'>File eliminato.</div>");
    }
    if (httpd_query_key_value(query, "del_all_v", val, sizeof(val)) == ESP_OK) {
        *off += snprintf(body + *off, max - *off,
                           "<div class='alert ok'>Tutti i video MP4 eliminati.</div>");
    }
    if (!ok_del && httpd_query_key_value(query, "err", val, sizeof(val)) == ESP_OK) {
        if (strcmp(val, "del") == 0) {
            *off += snprintf(body + *off, max - *off,
                               "<div class='alert err'>Eliminazione fallita.</div>");
        } else if (strcmp(val, "del_all") == 0) {
            *off += snprintf(body + *off, max - *off,
                               "<div class='alert err'>Eliminazione multipla fallita.</div>");
        }
    }
}

static unsigned videos_query_page(httpd_req_t *req) {
    char query[64] = {0};
    char val[12] = {0};
    if (!req || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return 1;
    }
    if (httpd_query_key_value(query, "page", val, sizeof(val)) != ESP_OK) {
        return 1;
    }
    int p = atoi(val);
    if (p < 1) {
        return 1;
    }
    return (unsigned)p;
}

static void videos_append_pager(char *body, size_t *off, size_t max,
                                unsigned page, unsigned pages) {
    if (!body || !off || pages <= 1) {
        return;
    }
    *off += snprintf(body + *off, max - *off, "<nav class='vid-pager'>");
    if (page > 1) {
        *off += snprintf(body + *off, max - *off,
                         "<a href='/videos?page=%u'>« Precedente</a>", page - 1);
    } else {
        *off += snprintf(body + *off, max - *off,
                         "<span class='vid-pager-off'>« Precedente</span>");
    }
    for (unsigned i = 1; i <= pages; i++) {
        if (i == page) {
            *off += snprintf(body + *off, max - *off,
                             "<span class='vid-pager-cur'>%u</span>", i);
        } else {
            *off += snprintf(body + *off, max - *off,
                             "<a href='/videos?page=%u'>%u</a>", i, i);
        }
    }
    if (page < pages) {
        *off += snprintf(body + *off, max - *off,
                         "<a href='/videos?page=%u'>Successiva »</a>", page + 1);
    } else {
        *off += snprintf(body + *off, max - *off,
                         "<span class='vid-pager-off'>Successiva »</span>");
    }
    *off += snprintf(body + *off, max - *off, "</nav>");
}

static void photos_append_flash(char *body, size_t *off, size_t max, httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }
    char val[16] = {0};
    if (httpd_query_key_value(query, "del_all_p", val, sizeof(val)) == ESP_OK) {
        *off += snprintf(body + *off, max - *off,
                           "<div class='alert ok'>Tutti i timelapse foto eliminati.</div>");
    }
    if (httpd_query_key_value(query, "pdel", val, sizeof(val)) == ESP_OK) {
        *off += snprintf(body + *off, max - *off,
                           "<div class='alert ok'>Timelapse foto eliminato.</div>");
    }
    if (httpd_query_key_value(query, "err", val, sizeof(val)) == ESP_OK) {
        if (strcmp(val, "del") == 0) {
            *off += snprintf(body + *off, max - *off,
                               "<div class='alert err'>Eliminazione fallita.</div>");
        } else if (strcmp(val, "del_all") == 0) {
            *off += snprintf(body + *off, max - *off,
                               "<div class='alert err'>Eliminazione multipla fallita.</div>");
        }
    }
}

static bool video_delete_rel(const char *rel) {
    if (!video_rel_path_valid(rel)) {
        return false;
    }
    if (!core_sd_ensure_rw()) {
        ESP_LOGE(TAG, "Eliminazione %s: SD non scrivibile (solo lettura)", rel);
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, rel);

    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    if (remove(path) != 0) {
        const int e1 = errno;
        (void)core_sd_ensure_rw();
        if (remove(path) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "remove %s fallito (errno=%d, primo=%d)", path, errno, e1);
            return false;
        }
    }
    ESP_LOGI(TAG, "Video eliminato: %s", path);
    {
        size_t plen = strlen(path);
        if (plen > 4 &&
            (strcmp(path + plen - 4, ".mp4") == 0 || strcmp(path + plen - 4, ".avi") == 0)) {
            char meta_path[192];
            if (plen - 4 + 5 < sizeof(meta_path)) {
                memcpy(meta_path, path, plen - 4);
                memcpy(meta_path + plen - 4, ".meta", 6);
                (void)remove(meta_path);
            }
        }
    }

    if (strncmp(rel, "mrbin/", 6) != 0) {
        char daypath[48];
        snprintf(daypath, sizeof(daypath), "%s/%.8s", CORE_SD_MOUNT_POINT, rel);
        DIR *day_dir = opendir(daypath);
        if (day_dir) {
            bool empty = true;
            struct dirent *ent;
            while ((ent = readdir(day_dir)) != nullptr) {
                if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                    empty = false;
                    break;
                }
            }
            closedir(day_dir);
            if (empty) {
                rmdir(daypath);
            }
        }
    }
    return true;
}

static size_t photos_delete_all_sessions(void) {
    photo_session_item_t *items = (photo_session_item_t *)web_alloc_psram(
        VIDEOS_LIST_MAX * sizeof(photo_session_item_t));
    if (!items) {
        return 0;
    }
    size_t count = photos_collect_sessions(items, VIDEOS_LIST_MAX);
    size_t deleted = 0;
    for (size_t i = 0; i < count; ++i) {
        if (photo_session_delete(items[i].rel_base)) {
            deleted++;
        }
    }
    free(items);
    ESP_LOGI(TAG, "Eliminate %u sessioni timelapse foto", (unsigned)deleted);
    return deleted;
}

static size_t videos_delete_all_mp4(void) {
    video_list_item_t *items = (video_list_item_t *)web_alloc_psram(
        VIDEOS_LIST_MAX * sizeof(video_list_item_t));
    if (!items) {
        return 0;
    }
    size_t count = videos_collect_list(items, VIDEOS_LIST_MAX);
    size_t deleted = 0;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].pending) {
            continue;
        }
        if (video_delete_rel(items[i].rel)) {
            deleted++;
        }
    }
    free(items);
    ESP_LOGI(TAG, "Eliminati %u video (MP4/AVI)", (unsigned)deleted);
    return deleted;
}

static esp_err_t handle_settings_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    char keyhex[40] = {0};
    if (s_settings && s_settings->aes_key_valid) {
        key_to_hex(s_settings->aes_key, keyhex, sizeof(keyhex));
    }

    if (!core_sd_is_mounted()) {
        core_sd_reinit();
    }

    char sd_info[320];
    if (core_sd_is_mounted()) {
        char free_h[32];
        format_bytes_human(core_sd_free_bytes(), free_h, sizeof(free_h));
        snprintf(sd_info, sizeof(sd_info),
            "<div class='sd-info'>SD montata — spazio libero: <strong>%s</strong></div>", free_h);
    } else {
        snprintf(sd_info, sizeof(sd_info),
            "<div class='sd-info warn'>SD non montata. Se la scheda è inserita (128 GB nuove spesso hanno exFAT), "
            "usa <strong>Rileva SD</strong> o <strong>Formatta SD</strong> per prepararla in FAT32.</div>");
    }

    char flash[192] = {0};
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char msg[12] = {0};
        if (httpd_query_key_value(query, "sd", msg, sizeof(msg)) == ESP_OK) {
            if (strcmp(msg, "ok") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert ok'>SD formattata con successo.</div>");
            } else if (strcmp(msg, "err") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert err'>Formattazione SD fallita. Verificare la scheda e riprovare.</div>");
            } else if (strcmp(msg, "detect_ok") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert ok'>SD rilevata e montata.</div>");
            } else if (strcmp(msg, "detect_err") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert err'>SD non rilevata. Verificare inserimento e contatto, poi riprovare o formattare.</div>");
            }
        } else if (httpd_query_key_value(query, "saved", msg, sizeof(msg)) == ESP_OK) {
            if (strcmp(msg, "1") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert ok'>Impostazioni salvate.</div>");
            } else if (strcmp(msg, "0") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert err'>Salvataggio fallito — verificare i valori (delay %u–%u ms, durata max %u–%u min).</div>",
                    (unsigned)CORE_D2_DELAY_MIN_MS, (unsigned)CORE_D2_DELAY_MAX_MS,
                    (unsigned)(CORE_REC_MAX_DURATION_SEC_MIN / 60U),
                    (unsigned)(CORE_REC_MAX_DURATION_SEC_MAX / 60U));
            }
        }
    }

    // ~1.6 KB reali: un buffer scarso troncava l'HTML dentro <script>, inghiottendo
    // il resto della pagina (pulsante Salva compreso).
    char pin_cfg[2560];
    uint8_t rec_start = s_settings ? s_settings->rec_gpio_start : (uint8_t)CORE_GPIO_D1_WAKE;
    uint8_t rec_stop = s_settings ? s_settings->rec_gpio_stop : (uint8_t)CORE_GPIO_D2_END;
    uint8_t stop_mode = s_settings ? s_settings->rec_stop_mode : CORE_REC_STOP_MODE_GPIO;
    uint32_t sess_sec = s_settings ? s_settings->rec_session_sec : CORE_REC_SESSION_SEC_DEFAULT;
    snprintf(pin_cfg, sizeof(pin_cfg),
        "<fieldset class='gpio-cfg'>"
        "<legend>Pin START e STOP registrazione</legend>"
        "<p class='hint'>All'accensione PIR la sessione parte subito (wake). "
        "A fine registrazione la scheda invia DONE e si spegne.<br>"
        "START/STOP: LOW attivo, debounce 50 ms.</p>"
        "<label>Pin di start (wake / inizio)</label>"
        "<select name='rec_gpio_start' id='rec_start'>"
        "<option value='%d'%s>D1 — GPIO1_C4 linux %d (PIR, default)</option>"
        "<option value='%d'%s>D2 — GPIO1_D2 linux %d</option>"
        "</select>"
        "<label>Pin di stop (fine video — consigliato)</label>"
        "<select name='rec_gpio_stop' id='rec_stop'>"
        "<option value='%d'%s>D2 — GPIO1_D2 linux %d (default)</option>"
        "<option value='%d'%s>D1 — GPIO1_C4 linux %d</option>"
        "</select>"
        "</fieldset>"
        "<fieldset>"
        "<legend>Fine registrazione</legend>"
        "<label>Modalità stop sessione</label>"
        "<select name='rec_stop_mode' id='rec_stop_mode' onchange='toggleSessionSec()'>"
        "<option value='0'%s>Attendi pin STOP (consigliato)</option>"
        "<option value='1'%s>Durata fissa (secondi) — pin STOP ignorato</option>"
        "</select>"
        "<label id='sessSecLabel'>Durata registrazione PIR (secondi, %u–%u) — solo durata fissa</label>"
        "<input name='rec_session_sec' id='rec_session_sec' type='number' min='%u' max='%u' value='%lu'>"
        "<p class='hint'><strong>Durata fissa:</strong> il PIR registra esattamente questi secondi "
        "(pin STOP ignorato), poi DONE.<br>"
        "<strong>Attendi pin STOP:</strong> termina su D2; la durata qui è disabilitata — "
        "il tetto di sicurezza è «Durata segmento PIR» sotto.</p>"
        "<script>function toggleSessionSec(){"
        "var t=document.getElementById('rec_stop_mode').value==='1';"
        "var el=document.getElementById('rec_session_sec');"
        /* readOnly (not disabled): il valore resta nel POST anche in modalità STOP. */
        "el.readOnly=!t; el.style.opacity=t?'1':'0.55';"
        "document.getElementById('sessSecLabel').style.opacity=t?'1':'0.5';"
        "}toggleSessionSec();</script>"
        "</fieldset>",
        (int)CORE_GPIO_D1_WAKE, rec_start == (uint8_t)CORE_GPIO_D1_WAKE ? " selected" : "",
        (int)CORE_GPIO_D1_WAKE,
        (int)CORE_GPIO_D2_END, rec_start == (uint8_t)CORE_GPIO_D2_END ? " selected" : "",
        (int)CORE_GPIO_D2_END,
        (int)CORE_GPIO_D2_END, rec_stop == (uint8_t)CORE_GPIO_D2_END ? " selected" : "",
        (int)CORE_GPIO_D2_END,
        (int)CORE_GPIO_D1_WAKE, rec_stop == (uint8_t)CORE_GPIO_D1_WAKE ? " selected" : "",
        (int)CORE_GPIO_D1_WAKE,
        stop_mode == CORE_REC_STOP_MODE_GPIO ? " selected" : "",
        stop_mode == CORE_REC_STOP_MODE_TIMER ? " selected" : "",
        (unsigned)CORE_REC_SESSION_SEC_MIN, (unsigned)CORE_REC_SESSION_SEC_MAX,
        (unsigned)CORE_REC_SESSION_SEC_MIN, (unsigned)CORE_REC_SESSION_SEC_MAX,
        (unsigned long)sess_sec);

    char body[12288];
    char fw_rel[80];
    web_firmware_release_html(fw_rel, sizeof(fw_rel));
    uint32_t rec_max_min = s_settings
        ? (s_settings->rec_max_duration_sec / 60U) : (CORE_REC_MAX_DURATION_SEC_DEFAULT / 60U);
    char clocks[420];
    web_clocks_html(clocks, sizeof(clocks), true);

    char rtc_err[360] = {0};
    if (core_rtc_time_setup_failed()) {
        if (core_alarm_schedule_hw_available()) {
            snprintf(rtc_err, sizeof(rtc_err),
                     "<div class='alert err'><strong>Errore RTC</strong> — impossibile impostare "
                     "l'orario sul DS3231 (modulo nuovo o ora non valida). "
                     "Connetti il WiFi per SNTP, oppure usa <strong>Aggiorna orario ora</strong> "
                     "nella <a href='/schedule'>pagina RTC</a>.</div>");
        } else {
            snprintf(rtc_err, sizeof(rtc_err),
                     "<div class='alert err'><strong>Errore RTC</strong> — impossibile impostare "
                     "l'orario sul DS3231 (modulo nuovo o ora non valida). "
                     "Connetti il WiFi con Internet per sincronizzare l'ora via SNTP.</div>");
        }
    }

    int body_len = snprintf(body, sizeof(body),
        "%s"
        "%s"
        "<div class='page-head'><h2>Impostazioni CORE</h2>"
        "<span class='fw-release' title='Release firmware'>%s</span></div>"
        "%s"
        "%s"
        "%s"
        "%s"
        "<form method='POST' action='/settings'>"
        "<label>CORE ID (5 cifre)</label><input name='core_id' value='%05lu'>"
        "%s"
        "<label>Durata segmento PIR / tetto sicurezza (minuti, %u–%u)</label>"
        "<input name='rec_max_min' type='number' min='%u' max='%u' value='%lu'>"
        "<p class='hint'>Con <strong>attendi pin STOP</strong>: tetto massimo in secondi sul PIR "
        "(<code>REC_SECONDS</code>). Con <strong>durata fissa</strong>: durata massima di un "
        "singolo file su SD se la sessione è più lunga. "
        "Resta sotto i <strong>%d min</strong> del TPL5110.</p>"
        "<label>Delay dopo pin stop (ms, %u–%u)</label><input name='d2_delay' type='number' min='%u' max='%u' value='%lu'>"
        "<label>WiFi SSID</label><input name='wifi_ssid' value='%s'>"
        "<label>WiFi Password</label><input name='wifi_pass' type='password' value='%s'>"
        "<label>Chiave AES-128 (hex, sola lettura)</label><input readonly value='%s'>"
        "<p class='hint'>Upload server (sidecar + AES-256-GCM): <a href='/mrbin'>pagina MrBin</a>.</p>"
        "<button type='submit'>Salva</button></form>"
        "<div class='sd-actions'>"
        "<form method='POST' action='/sd/remount'>"
        "<button type='submit' class='remount-btn'>Rileva SD</button></form>"
        "<form method='POST' action='/format_sd' onsubmit='return startFormat(event);'>"
        "<button id='fmtBtn' type='submit' class='fmt-btn'>Formatta SD</button></form>"
        "</div>"
        "%s"
        "</body></html>",
        SETTINGS_HTML_HEAD,
        web_nav(),
        fw_rel,
        flash,
        rtc_err,
        clocks,
        sd_info,
        s_settings ? (unsigned long)s_settings->core_id : 0UL,
        pin_cfg,
        (unsigned)(CORE_REC_MAX_DURATION_SEC_MIN / 60U),
        (unsigned)(CORE_REC_MAX_DURATION_SEC_MAX / 60U),
        (unsigned)(CORE_REC_MAX_DURATION_SEC_MIN / 60U),
        (unsigned)(CORE_REC_MAX_DURATION_SEC_MAX / 60U),
        (unsigned long)rec_max_min,
        CORE_TPL_ON_MAX_MINUTES,
        (unsigned)CORE_D2_DELAY_MIN_MS,
        (unsigned)CORE_D2_DELAY_MAX_MS,
        (unsigned)CORE_D2_DELAY_MIN_MS,
        (unsigned)CORE_D2_DELAY_MAX_MS,
        s_settings ? (unsigned long)s_settings->d2_post_delay_ms : (unsigned long)CORE_D2_POST_DELAY_MS,
        s_settings ? s_settings->wifi_ssid : "",
        s_settings ? s_settings->wifi_pass : "",
        keyhex,
        FORMAT_OVERLAY_HTML);
    if (body_len >= (int)sizeof(body)) {
        ESP_LOGW(TAG, "Pagina settings troncata (%d B > %u) — HTML incompleto",
                 body_len, (unsigned)sizeof(body));
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_settings_post(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) return send_redirect(req, "/");
    char buf[4096];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/settings?saved=0");
    }

    char id_s[16] = {0}, delay_s[16] = {0}, max_min_s[16] = {0}, ssid[64] = {0}, pass[128] = {0};
    char start_s[8] = {0}, stop_s[8] = {0}, stop_mode_s[8] = {0}, sess_sec_s[16] = {0};
    form_get_value(buf, "core_id", id_s, sizeof(id_s));
    form_get_value(buf, "d2_delay", delay_s, sizeof(delay_s));
    form_get_value(buf, "rec_max_min", max_min_s, sizeof(max_min_s));
    form_get_value(buf, "rec_gpio_start", start_s, sizeof(start_s));
    form_get_value(buf, "rec_gpio_stop", stop_s, sizeof(stop_s));
    form_get_value(buf, "rec_stop_mode", stop_mode_s, sizeof(stop_mode_s));
    form_get_value(buf, "rec_session_sec", sess_sec_s, sizeof(sess_sec_s));
    form_get_value(buf, "wifi_ssid", ssid, sizeof(ssid));
    form_get_value(buf, "wifi_pass", pass, sizeof(pass));

    bool delay_ok = false;
    bool max_dur_ok = false;
    bool stop_ok = true;
    uint32_t id = (uint32_t)atoi(id_s);
    if (id >= CORE_ID_MIN && id <= CORE_ID_MAX) {
        s_settings->core_id = id;
    }
    uint32_t delay = (uint32_t)atoi(delay_s);
    if (delay_s[0] == 0) {
        delay_ok = true;
    } else if (delay >= CORE_D2_DELAY_MIN_MS && delay <= CORE_D2_DELAY_MAX_MS) {
        s_settings->d2_post_delay_ms = delay;
        delay_ok = true;
    } else {
        ESP_LOGW(TAG, "Delay stop rifiutato: %lu ms (range %u–%u)",
                 (unsigned long)delay, (unsigned)CORE_D2_DELAY_MIN_MS, (unsigned)CORE_D2_DELAY_MAX_MS);
    }
    uint32_t max_min = (uint32_t)atoi(max_min_s);
    if (max_min_s[0] == 0) {
        max_dur_ok = true;
    } else {
        uint32_t max_sec = max_min * 60U;
        if (core_settings_rec_max_duration_valid(max_sec)) {
            s_settings->rec_max_duration_sec = max_sec;
            max_dur_ok = true;
        } else {
            ESP_LOGW(TAG, "Durata max rifiutata: %lu min (range %u–%u min)",
                     (unsigned long)max_min,
                     (unsigned)(CORE_REC_MAX_DURATION_SEC_MIN / 60U),
                     (unsigned)(CORE_REC_MAX_DURATION_SEC_MAX / 60U));
        }
    }
    uint8_t start_gpio = (uint8_t)atoi(start_s);
    uint8_t stop_gpio = (uint8_t)atoi(stop_s);
    if (core_settings_rec_pins_valid(start_gpio, stop_gpio)) {
        s_settings->rec_gpio_start = start_gpio;
        s_settings->rec_gpio_stop = stop_gpio;
    } else {
        ESP_LOGW(TAG, "Pin start/stop non validi (%u/%u) — ignorati", (unsigned)start_gpio,
                 (unsigned)stop_gpio);
    }
    /* Campo assente: non forzare GPIO (atoi("")==0 sembrava un salvataggio valido). */
    if (stop_mode_s[0] != 0) {
        uint8_t stop_mode = (uint8_t)atoi(stop_mode_s);
        if (core_settings_rec_stop_mode_valid(stop_mode)) {
            s_settings->rec_stop_mode = stop_mode;
        } else {
            stop_ok = false;
            ESP_LOGW(TAG, "rec_stop_mode rifiutato: '%s'", stop_mode_s);
        }
    } else {
        ESP_LOGW(TAG, "rec_stop_mode assente nel POST — lascio %u",
                 (unsigned)s_settings->rec_stop_mode);
    }
    uint32_t sess_sec = (uint32_t)atoi(sess_sec_s);
    if (sess_sec_s[0] == 0) {
        if (s_settings->rec_stop_mode == CORE_REC_STOP_MODE_TIMER) {
            stop_ok = false;
            ESP_LOGW(TAG, "Durata sessione assente con modalità durata fissa");
        }
    } else if (core_settings_rec_session_sec_valid(sess_sec)) {
        s_settings->rec_session_sec = sess_sec;
    } else {
        stop_ok = false;
        ESP_LOGW(TAG, "Durata sessione rifiutata: %lu s", (unsigned long)sess_sec);
    }
    snprintf(s_settings->wifi_ssid, sizeof(s_settings->wifi_ssid), "%s", ssid);
    snprintf(s_settings->wifi_pass, sizeof(s_settings->wifi_pass), "%s", pass);

    ESP_LOGI(TAG, "POST settings: stop_mode=%u sess=%lu s max_min=%lu (raw mode='%s' sess='%s')",
             (unsigned)s_settings->rec_stop_mode, (unsigned long)s_settings->rec_session_sec,
             (unsigned long)max_min, stop_mode_s, sess_sec_s);

    if (!stop_ok) {
        return send_redirect(req, "/settings?saved=0");
    }

    bool saved = core_settings_save(s_settings);
    if (saved) {
        core_gpio_set_rec_pins(s_settings->rec_gpio_start, s_settings->rec_gpio_stop);
    }
    if (!delay_ok && delay_s[0] != 0) {
        return send_redirect(req, "/settings?saved=0");
    }
    if (!max_dur_ok && max_min_s[0] != 0) {
        return send_redirect(req, "/settings?saved=0");
    }
    return send_redirect(req, saved ? "/settings?saved=1" : "/settings?saved=0");
}

static esp_err_t handle_rec_video_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    uint16_t vw = s_settings ? s_settings->video_width : (uint16_t)CORE_VIDEO_WIDTH_DEFAULT;
    uint16_t vh = s_settings ? s_settings->video_height : (uint16_t)CORE_VIDEO_HEIGHT_DEFAULT;
    uint8_t vf = s_settings ? s_settings->video_fps : (uint8_t)CORE_VIDEO_FPS_DEFAULT;

    core_settings_t tmp = s_settings ? *s_settings : core_settings_t{};
    if (!s_settings) {
        core_settings_video_set_defaults(&tmp);
    }
    uint32_t bitrate = core_settings_video_bitrate(&tmp);
    uint32_t gop = core_settings_video_gop(&tmp);

    char flash[256] = {0};
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char msg[8] = {0};
        if (httpd_query_key_value(query, "saved", msg, sizeof(msg)) == ESP_OK) {
            if (strcmp(msg, "1") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert ok'>Impostazioni video salvate in NVS.</div>");
            } else if (strcmp(msg, "0") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert err'>Salvataggio fallito — verificare risoluzione (%u–%u × %u–%u px, pari) "
                    "e frame rate (%u–%u fps).</div>",
                    (unsigned)CORE_VIDEO_WIDTH_MIN, (unsigned)CORE_VIDEO_WIDTH_MAX,
                    (unsigned)CORE_VIDEO_HEIGHT_MIN, (unsigned)CORE_VIDEO_HEIGHT_MAX,
                    (unsigned)CORE_VIDEO_FPS_MIN, (unsigned)CORE_VIDEO_FPS_MAX);
            }
        }
    }

    char last_pipe[280] = {0};
    {
        core_recorder_manual_status_t rst = {};
        core_recorder_manual_get_status(&rst);
        if (rst.enc_w && rst.enc_h) {
            if (rst.error[0]) {
                snprintf(last_pipe, sizeof(last_pipe),
                    "<div class='alert err'>Ultima pipeline: <strong>%u×%u@%u</strong> — %s "
                    "(nessun fallback silenzioso a 1280×960)</div>",
                    (unsigned)rst.enc_w, (unsigned)rst.enc_h, (unsigned)rst.enc_fps, rst.error);
            } else {
                snprintf(last_pipe, sizeof(last_pipe),
                    "<div class='stats'>Ultima pipeline VI+VENC: <strong>%u×%u@%u</strong></div>",
                    (unsigned)rst.enc_w, (unsigned)rst.enc_h, (unsigned)rst.enc_fps);
            }
        }
    }
    char stats[480];
    bool rec_enc = s_settings ? s_settings->rec_encrypt : true;
    uint8_t media = s_settings ? s_settings->rec_media_mode : CORE_REC_MEDIA_VIDEO;
    const char *media_label = (media == CORE_REC_MEDIA_PHOTO) ? "timelapse foto" : "video H264";
    if (media == CORE_REC_MEDIA_PHOTO) {
        snprintf(stats, sizeof(stats),
            "<div class='stats'>Attuale: <strong>%s</strong> — <strong>%u×%u @ 1 fps</strong> "
            "(max %u×%u) — cifratura SD: <strong>%s</strong></div>",
            media_label, (unsigned)vw, (unsigned)vh,
            (unsigned)CORE_VIDEO_WIDTH_MAX, (unsigned)CORE_VIDEO_HEIGHT_MAX,
            rec_enc ? "attiva (JPEG/AVI)" : "disattiva");
    } else {
        snprintf(stats, sizeof(stats),
            "<div class='stats'>Attuale: <strong>%s</strong> — %u×%u @ %u fps — "
            "bitrate ~%.1f Mbps, GOP %lu frame (~%.1f s) — "
            "cifratura SD: <strong>%s</strong></div>",
            media_label,
            (unsigned)vw, (unsigned)vh, (unsigned)vf,
            (double)bitrate / 1000000.0,
            (unsigned long)gop,
            vf > 0 ? (double)gop / (double)vf : 0.0,
            rec_enc ? "attiva" : "disattiva (max fps)");
    }

    // HTML from NVS/settings only — never open V4L2 / RKMPI / I2C sensor here.
    // Luckfox HAL chunked responses used to omit the header blank line and
    // freeze the browser (and the single-thread HTTP server) on this page.
    const size_t html_cap = 65536;
    char *html = (char *)malloc(html_cap);
    if (!html) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    uint8_t osd_font = s_settings ? s_settings->osd_font : (uint8_t)CORE_REC_OSD_FONT_DEFAULT;
    uint8_t osd_xp = s_settings ? s_settings->osd_x_pct : (uint8_t)CORE_REC_OSD_X_PCT_DEFAULT;
    uint8_t osd_yp = s_settings ? s_settings->osd_y_pct : (uint8_t)CORE_REC_OSD_Y_PCT_DEFAULT;
    bool osd_en = s_settings ? s_settings->osd_enable : true;
    int off = snprintf(html, html_cap, "%s%s<h2>Registrazione video IMX415 (H264 SD)</h2>%s%s%s",
                       REC_VIDEO_HTML_HEAD, web_nav(), flash, stats, last_pipe);
    if (off < 0) {
        off = 0;
    }
    if (off >= (int)html_cap) {
        off = (int)html_cap - 1;
        html[off] = 0;
    }
    int form_len = snprintf(html + off, html_cap - (size_t)off, REC_VIDEO_FORM_TAIL,
             (media == CORE_REC_MEDIA_VIDEO) ? " selected" : "",
             (media == CORE_REC_MEDIA_PHOTO) ? " selected" : "",
             (unsigned)CORE_VIDEO_WIDTH_MAX, (unsigned)CORE_VIDEO_HEIGHT_MAX,
             (unsigned)CORE_VIDEO_WIDTH_MIN, (unsigned)CORE_VIDEO_WIDTH_MAX, (unsigned)vw,
             (unsigned)CORE_VIDEO_HEIGHT_MIN, (unsigned)CORE_VIDEO_HEIGHT_MAX, (unsigned)vh,
             (unsigned)CORE_VIDEO_FPS_MIN, (unsigned)CORE_VIDEO_FPS_MAX, (unsigned)vf,
             (s_settings && s_settings->video_vflip) ? " checked" : "",
             (s_settings && s_settings->video_hflip) ? " checked" : "",
             rec_enc ? " checked" : "",
             osd_en ? " checked" : "",
             (osd_font == 12) ? " selected" : "",
             (osd_font == 16) ? " selected" : "",
             (osd_font == 20) ? " selected" : "",
             (osd_font == 24) ? " selected" : "",
             (osd_font == 32) ? " selected" : "",
             (unsigned)osd_xp, (unsigned)osd_yp,
             (unsigned)CORE_VIDEO_PRESET_OK_WIDTH, (unsigned)CORE_VIDEO_PRESET_OK_HEIGHT,
             (unsigned)CORE_VIDEO_PRESET_OK_FPS, (unsigned)CORE_VIDEO_FPS_1080_MAX,
             (unsigned)CORE_VIDEO_FPS_ABOVE_5M, (unsigned)CORE_VIDEO_PIXELS_5M,
             (unsigned)CORE_VIDEO_WIDTH_MAX, (unsigned)CORE_VIDEO_HEIGHT_MAX);
    if (form_len < 0 || (off + form_len) >= (int)html_cap) {
        ESP_LOGW(TAG, "Form rec_video troncato (%d+%d B > %u)", off, form_len,
                 (unsigned)html_cap);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t handle_rec_video_post(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) return send_redirect(req, "/");

    char buf[2048];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/rec_video?saved=0");
    }

    char w_s[16] = {0}, h_s[16] = {0}, fps_s[8] = {0}, preset_s[32] = {0};
    form_get_value(buf, "video_w", w_s, sizeof(w_s));
    form_get_value(buf, "video_h", h_s, sizeof(h_s));
    form_get_value(buf, "video_fps", fps_s, sizeof(fps_s));
    form_get_value(buf, "video_preset", preset_s, sizeof(preset_s));

    uint16_t w = (uint16_t)atoi(w_s);
    uint16_t h = (uint16_t)atoi(h_s);
    uint8_t fps = (uint8_t)atoi(fps_s);

    if (preset_s[0]) {
        unsigned pw = 0;
        unsigned ph = 0;
        unsigned pf = 0;
        if (sscanf(preset_s, "%u,%u,%u", &pw, &ph, &pf) == 3) {
            w = (uint16_t)pw;
            h = (uint16_t)ph;
            fps = (uint8_t)pf;
            ESP_LOGI(TAG, "Video preset '%s' → %ux%u@%u", preset_s,
                     (unsigned)w, (unsigned)h, (unsigned)fps);
        } else {
            ESP_LOGW(TAG, "Video preset non valido: '%s'", preset_s);
        }
    }

    ESP_LOGI(TAG, "Video POST: preset=%s w=%s h=%s fps=%s → %ux%u@%u",
             preset_s[0] ? preset_s : "-",
             w_s[0] ? w_s : "?", h_s[0] ? h_s : "?", fps_s[0] ? fps_s : "?",
             (unsigned)w, (unsigned)h, (unsigned)fps);

    {
        const uint8_t cap = core_video_fps_ceiling(w, h);
        if (fps > cap) {
            ESP_LOGW(TAG, "Video %ux%u: fps %u > tetto %u — ridotto",
                     (unsigned)w, (unsigned)h, (unsigned)fps, (unsigned)cap);
            fps = cap;
        }
    }

    if (!core_settings_video_valid(w, h, fps)) {
        ESP_LOGW(TAG, "Video NVS rifiutato: %ux%u@%u", (unsigned)w, (unsigned)h, (unsigned)fps);
        return send_redirect(req, "/rec_video?saved=0");
    }

    // Checkbox HTML: presenti nel body solo se selezionate.
    char vflip_s[4] = {0};
    char hflip_s[4] = {0};
    char rec_enc_s[4] = {0};
    char media_s[8] = {0};
    char osd_en_s[4] = {0};
    char osd_font_s[8] = {0};
    char osd_xp_s[8] = {0};
    char osd_yp_s[8] = {0};
    form_get_value(buf, "video_vflip", vflip_s, sizeof(vflip_s));
    form_get_value(buf, "video_hflip", hflip_s, sizeof(hflip_s));
    form_get_value(buf, "rec_encrypt", rec_enc_s, sizeof(rec_enc_s));
    form_get_value(buf, "rec_media", media_s, sizeof(media_s));
    form_get_value(buf, "osd_en", osd_en_s, sizeof(osd_en_s));
    form_get_value(buf, "osd_font", osd_font_s, sizeof(osd_font_s));
    form_get_value(buf, "osd_x_pct", osd_xp_s, sizeof(osd_xp_s));
    form_get_value(buf, "osd_y_pct", osd_yp_s, sizeof(osd_yp_s));

    uint8_t media_mode = (uint8_t)atoi(media_s);
    if (core_settings_rec_media_mode_valid(media_mode)) {
        s_settings->rec_media_mode = media_mode;
    }
    if (s_settings->rec_media_mode == CORE_REC_MEDIA_PHOTO && fps != CORE_REC_PHOTO_FPS_MAX) {
        ESP_LOGW(TAG, "Foto timelapse: fps forzato a 1 (era %u)", (unsigned)fps);
        fps = (uint8_t)CORE_REC_PHOTO_FPS_MAX;
    }

    s_settings->video_width = w;
    s_settings->video_height = h;
    s_settings->video_fps = fps;
    s_settings->video_vflip = (vflip_s[0] == '1');
    s_settings->video_hflip = (hflip_s[0] == '1');
    s_settings->rec_encrypt = (rec_enc_s[0] == '1');
    s_settings->osd_enable = (osd_en_s[0] == '1');
    s_settings->osd_font = (uint8_t)atoi(osd_font_s);
    s_settings->osd_x_pct = (uint8_t)atoi(osd_xp_s);
    s_settings->osd_y_pct = (uint8_t)atoi(osd_yp_s);
    core_settings_video_normalize(s_settings);
    core_settings_osd_normalize(s_settings);
    if (s_settings->rec_encrypt) {
        (void)core_settings_ensure_aes_key(s_settings);
    }

    bool saved = core_settings_save(s_settings);
    if (saved && s_settings->rec_encrypt && s_settings->aes_key_valid) {
        /* Opportunistic: encrypt any PIR clips waiting with .encrypt-later. */
        (void)core_recorder_process_pending_encrypt(s_settings);
    }
    if (core_live_is_running()) {
        core_live_force_release();
    }
    /* Remembers flip; ioctl only if camera already up — never open MPI/V4L2. */
    core_video_set_flip(s_settings->video_vflip, s_settings->video_hflip);
    core_rtsp_apply(s_settings);
    ESP_LOGI(TAG, "Video web: %ux%u@%u media=%u vflip=%d hflip=%d rtsp_always=%d encrypt=%d "
                  "osd=%d font=%u pos=%u%%,%u%% → %s",
             (unsigned)s_settings->video_width, (unsigned)s_settings->video_height,
             (unsigned)s_settings->video_fps, (unsigned)s_settings->rec_media_mode,
             (int)s_settings->video_vflip, (int)s_settings->video_hflip,
             (int)s_settings->rtsp_always, (int)s_settings->rec_encrypt,
             (int)s_settings->osd_enable, (unsigned)s_settings->osd_font,
             (unsigned)s_settings->osd_x_pct, (unsigned)s_settings->osd_y_pct,
             saved ? "OK" : "FAIL");
    return send_redirect(req, saved ? "/rec_video?saved=1" : "/rec_video?saved=0");
}

// Flip live: ferma lo stream, aggiorna flip+Bayer ISP, persiste NVS; il client riapre MJPEG.
static esp_err_t handle_live_flip(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    if (!s_settings) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Impostazioni non disponibili\"}");
    }

    char buf[64];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Body mancante\"}");
    }
    char v_s[4] = {0}, h_s[4] = {0};
    form_get_value(buf, "vflip", v_s, sizeof(v_s));
    form_get_value(buf, "hflip", h_s, sizeof(h_s));

    s_settings->video_vflip = (v_s[0] == '1');
    s_settings->video_hflip = (h_s[0] == '1');

    // S_SENSOR_FMT a stream attivo spegne i frame: takeover/stop prima del Bayer.
    if (core_live_is_running()) {
        core_live_force_release();
    }

    core_video_set_flip(s_settings->video_vflip, s_settings->video_hflip);
    bool saved = core_settings_save(s_settings);
    bool hw = core_video_flip_hw_applied();
    ESP_LOGI(TAG, "Flip live: vflip=%d hflip=%d hw=%d → %s",
             (int)s_settings->video_vflip, (int)s_settings->video_hflip,
             (int)hw, saved ? "salvato" : "NVS FAIL");

    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"hw\":%s}",
             saved ? "true" : "false", hw ? "true" : "false");
    return send_json(req, json);
}

/* User reset: drop Live + 3A so the next /live/stream runs live-pre
 * (3A armed before EnableChn, CPROC 128, AE 816/635 only if exposure < 200). */
static esp_err_t handle_live_image_reset(httpd_req_t *req) {
    if (!is_authed(req)) return send_json_unauth(req);
    if (core_recorder_manual_is_active()) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Registrazione in corso\"}");
    }
    if (core_live_is_running()) {
        core_live_force_release();
    }
    /* After the worker has released VI. A parallel stop-3a used to race
     * DisableChn and take mrbin-core down. */
    core_video_isp_stop_3a();
    ESP_LOGI(TAG, "Live reset immagine");
    return send_json(req, "{\"ok\":true,\"msg\":\"Ricalibro\"}");
}

static bool live_parse_focus_pos(httpd_req_t *req, uint16_t *out_pos) {
    char buf[64] = {0};
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return false;
    }
    char pos_s[8] = {0};
    if (!form_get_value(buf, "pos", pos_s, sizeof(pos_s))) {
        return false;
    }
    int pos = atoi(pos_s);
    if (pos < 0) {
        pos = 0;
    }
    if (pos > CORE_ISP_FOCUS_MAX) {
        pos = CORE_ISP_FOCUS_MAX;
    }
    *out_pos = (uint16_t)pos;
    return true;
}

static esp_err_t handle_live_focus(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (core_recorder_manual_is_active()) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Registrazione in corso\"}");
    }
    uint16_t pos = CORE_ISP_FOCUS_DEFAULT;
    if (!live_parse_focus_pos(req, &pos)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"pos mancante\"}");
    }
    if (!core_video_module_probed() || !core_video_module_is_af()) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Modulo senza AF\"}");
    }
    bool ok = core_isp_cal_apply_focus(pos);
    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"pos\":%u}", ok ? "true" : "false", (unsigned)pos);
    return send_json(req, json);
}

static esp_err_t handle_live_focus_save(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (core_recorder_manual_is_active()) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Registrazione in corso\"}");
    }
    uint16_t pos = CORE_ISP_FOCUS_DEFAULT;
    if (!live_parse_focus_pos(req, &pos)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"pos mancante\"}");
    }
    if (!core_video_module_probed() || !core_video_module_is_af()) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Modulo senza AF\"}");
    }
    bool ok = core_isp_cal_save_focus_all(pos);
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"pos\":%u,\"msg\":\"%s\"}",
             ok ? "true" : "false", (unsigned)pos,
             ok ? "Fuoco salvato" : "Salvataggio NVS fallito");
    return send_json(req, json);
}

static esp_err_t handle_sd_remount(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    ESP_LOGI(TAG, "Rileva SD richiesto da web GUI");
    bool ok = core_sd_reinit();
    if (ok) {
        ok = core_sd_ensure_rw();
    }
    return send_redirect(req, ok ? "/settings?sd=detect_ok" : "/settings?sd=detect_err");
}

static esp_err_t handle_format_sd(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    ESP_LOGI(TAG, "Formattazione SD richiesta da web GUI");
    bool ok = core_sd_format();
    if (ok) {
        ESP_LOGI(TAG, "Formattazione SD completata");
    } else {
        ESP_LOGE(TAG, "Formattazione SD fallita");
    }

    char body[1024];
    if (ok) {
        char detail[80] = {0};
        if (core_sd_is_mounted()) {
            char free_h[32];
            format_bytes_human(core_sd_free_bytes(), free_h, sizeof(free_h));
            snprintf(detail, sizeof(detail), "<br>Spazio libero: <strong>%s</strong>", free_h);
        }
        snprintf(body, sizeof(body),
            "%s"
            "<div class='alert ok'><strong>Formattazione completata.</strong>%s</div>"
            "<p>La scheda SD è pronta per nuove registrazioni.</p>"
            "<a class='btn' href='/settings?sd=ok'>Torna alle impostazioni</a>"
            "<meta http-equiv='refresh' content='6;url=/settings?sd=ok'>"
            "</body></html>",
            FORMAT_RESULT_HEAD, detail);
    } else {
        snprintf(body, sizeof(body),
            "%s"
            "<div class='alert err'><strong>Formattazione fallita.</strong>"
            "<br>Verificare che la SD sia inserita correttamente. Le schede nuove (128 GB) "
            "vanno formattate in FAT32 da qui — riprovare o usare Rileva SD.</div>"
            "<a class='btn' href='/settings?sd=err'>Torna alle impostazioni</a>"
            "</body></html>",
            FORMAT_RESULT_HEAD);
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_videos_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }

    const size_t body_cap = 32768;
    char *body = (char *)web_alloc_psram(body_cap);
    video_list_item_t *items = (video_list_item_t *)web_alloc_psram(
        VIDEOS_LIST_MAX * sizeof(video_list_item_t));
    if (!body || !items) {
        free(body);
        free(items);
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    size_t off = 0;
    off += snprintf(body + off, body_cap - off, "%s%s%s", VIDEOS_HTML_HEAD, web_nav(), VIDEOS_HTML_BODY_START);
    videos_append_flash(body, &off, body_cap, req);

    if (!core_sd_is_mounted()) {
        core_sd_reinit();
    }
    if (!core_sd_is_mounted()) {
        off += snprintf(body + off, body_cap - off,
                        "<div class='sd-warn'>SD non montata — prova <a href='/settings'>Rileva SD</a> "
                        "o Formatta SD nelle impostazioni.</div>");
    }

    size_t file_count = videos_collect_list(items, VIDEOS_LIST_MAX);
    unsigned pages = 1;
    if (file_count > 0) {
        pages = (unsigned)((file_count + VIDEOS_PAGE_SIZE - 1) / VIDEOS_PAGE_SIZE);
    }
    unsigned page = videos_query_page(req);
    if (page > pages) {
        page = pages;
    }
    size_t start = (file_count > 0) ? ((size_t)(page - 1) * VIDEOS_PAGE_SIZE) : 0;
    size_t end = start + VIDEOS_PAGE_SIZE;
    if (end > file_count) {
        end = file_count;
    }

    if (file_count > 0) {
        off += snprintf(body + off, body_cap - off,
                        "<p class='vid-meta'>%u video trovat%s (più recenti in alto)"
                        " — pagina %u di %u, max %u per pagina.</p>",
                        (unsigned)file_count,
                        file_count == 1 ? "o" : "i",
                        page, pages, (unsigned)VIDEOS_PAGE_SIZE);
    }

    videos_append_pager(body, &off, body_cap, page, pages);
    off += snprintf(body + off, body_cap - off, "<ul>");
    bool truncated = false;
    for (size_t i = start; i < end; ++i) {
        if (off >= body_cap - 768) {
            truncated = true;
            break;
        }
        const video_list_item_t *it = &items[i];
        if (it->pending) {
            off += snprintf(body + off, body_cap - off,
                "<li class='vid-row vid-pending'>"
                "<div class='vid-preview'></div>"
                "<div class='vid-info'>"
                "<span class='vid-title'>%s</span>"
                "<span class='vid-size'>%s · %s</span></div>"
                "<span>Elaborazione…</span></li>",
                it->name, it->when_h, it->detail_h[0] ? it->detail_h : it->size_h);
            continue;
        }
        char rel_enc[128];
        url_encode_query(it->rel, rel_enc, sizeof(rel_enc));
        const char *lock_ico = it->encrypted
            ? "<span class='vid-lock-ico' title='Cifrato AES su SD'>AES</span>"
            : "";
        const char *up_ico = "";
        if (it->upload_st == CORE_CLIP_UPLOADED_VERIFIED) {
            up_ico = "<span class='vid-lock-ico' title='Inviato al server'>inviato</span>";
        } else if (it->upload_st == CORE_CLIP_PENDING_UPLOAD ||
                   it->upload_st == CORE_CLIP_UPLOADING ||
                   it->upload_st == CORE_CLIP_ENCRYPTED_LOCAL) {
            up_ico = "<span class='vid-lock-ico' title='In coda upload'>coda</span>";
        } else if (it->upload_st == CORE_CLIP_ERROR) {
            up_ico = "<span class='vid-lock-ico' title='Errore upload'>err</span>";
        }
        const char *pir_ico = (strncmp(it->rel, "mrbin/", 6) == 0)
                                  ? "<span class='vid-lock-ico' title='Clip PIR (/sdcard/mrbin)'>PIR</span>"
                                  : "";
        const char *kind_ico = (strstr(it->rel, ".avi") != nullptr) ? "AVI" : "MP4";
        off += snprintf(body + off, body_cap - off,
            "<li class='vid-row'>"
            "<div class='vid-preview vid-icon'>%s</div>"
            "<div class='vid-info'>"
            "<div class='vid-title-row'>"
            "<span class='vid-title-plain'>%s</span>%s%s%s</div>"
            "<span class='vid-size'>%s · %s%s</span></div>"
            "<div class='vid-actions'>"
            "<a class='vid-btn vid-btn-dl' href='/videos/download?f=%s'>Scarica</a>"
            "<form method='POST' action='/videos/delete' style='margin:0' "
            "onsubmit=\"return confirm('Eliminare %s?');\">"
            "<input type='hidden' name='f' value='%s'>"
            "<button type='submit' class='vid-btn vid-btn-del'>Elimina</button>"
            "</form></div></li>",
            kind_ico, it->name, pir_ico, lock_ico, up_ico,
            it->when_h, it->detail_h, it->encrypted ? " · cifrato" : "",
            rel_enc,
            it->name, it->rel);
    }
    if (file_count == 0) {
        off += snprintf(body + off, body_cap - off,
                        "<li class='vid-empty'>Nessun video (MP4/AVI) trovato "
                        "(cartelle data o <code>mrbin/</code>).</li>");
    } else if (truncated) {
        off += snprintf(body + off, body_cap - off,
                        "<li class='vid-empty'>Elenco troncato — troppi file da mostrare in una pagina.</li>");
    }
    off += snprintf(body + off, body_cap - off, "</ul>");
    videos_append_pager(body, &off, body_cap, page, pages);
    off += snprintf(body + off, body_cap - off, "%s", VIDEO_LIST_TAIL);

    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    free(items);
    return err;
}

static esp_err_t handle_photos_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }

    const size_t body_cap = 32768;
    char *body = (char *)web_alloc_psram(body_cap);
    photo_session_item_t *photos = (photo_session_item_t *)web_alloc_psram(
        VIDEOS_LIST_MAX * sizeof(photo_session_item_t));
    if (!body || !photos) {
        free(body);
        free(photos);
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    size_t off = 0;
    off += snprintf(body + off, body_cap - off, "%s%s%s", PHOTOS_HTML_HEAD, web_nav(), PHOTOS_HTML_BODY_START);
    photos_append_flash(body, &off, body_cap, req);

    if (!core_sd_is_mounted()) {
        core_sd_reinit();
    }
    if (!core_sd_is_mounted()) {
        off += snprintf(body + off, body_cap - off,
                        "<div class='sd-warn'>SD non montata — prova <a href='/settings'>Rileva SD</a> "
                        "o Formatta SD nelle impostazioni.</div>");
    }

    size_t photo_count = photos_collect_sessions(photos, VIDEOS_LIST_MAX);
    if (photo_count > 0) {
        off += snprintf(body + off, body_cap - off,
                        "<p class='vid-meta'>%u session%s trovata%s (più recenti in alto).</p>",
                        (unsigned)photo_count,
                        photo_count == 1 ? "e" : "i",
                        photo_count == 1 ? "" : "e");
    }

    off += snprintf(body + off, body_cap - off, "<ul>");
    bool truncated = false;
    for (size_t i = 0; i < photo_count; ++i) {
        if (off >= body_cap - 960) {
            truncated = true;
            break;
        }
        const photo_session_item_t *ps = &photos[i];
        char base_enc[128];
        url_encode_query(ps->rel_base, base_enc, sizeof(base_enc));
        char icons[384] = {0};
        size_t ic_off = 0;
        uint8_t show_fps = ps->fps >= 1 ? ps->fps : 1;
        ic_off += (size_t)snprintf(icons + ic_off, sizeof(icons) - ic_off,
            "<span class='vid-fps-ico' title='Timelapse foto %u fps'>TL</span>",
            (unsigned)show_fps);
        if (ps->encrypted) {
            ic_off += (size_t)snprintf(icons + ic_off, sizeof(icons) - ic_off,
                "<span class='vid-lock-ico' title='Cifrato AES su SD'>AES</span>");
        }
        if (ps->has_avi) {
            ic_off += (size_t)snprintf(icons + ic_off, sizeof(icons) - ic_off,
                     "<a class='vid-avi-ico' href='/photos/export/file?b=%s' "
                     "title='Scarica video AVI' download>&#8681;</a>",
                     base_enc);
        }
        char meta_extra[64] = {0};
        {
            size_t mo = 0;
            mo += (size_t)snprintf(meta_extra + mo, sizeof(meta_extra) - mo, " · %u fps",
                                   (unsigned)show_fps);
            if (ps->encrypted) {
                mo += (size_t)snprintf(meta_extra + mo, sizeof(meta_extra) - mo, " · cifrato");
            }
            if (ps->has_avi) {
                mo += (size_t)snprintf(meta_extra + mo, sizeof(meta_extra) - mo, " · video pronto");
            }
            (void)mo;
        }
        off += snprintf(body + off, body_cap - off,
            "<li class='vid-row'>"
            "<div class='vid-preview prev-photo' data-base='%s'>"
            "<img alt='' src=''></div>"
            "<div class='vid-info'>"
            "<div class='vid-title-row'>"
            "<a class='vid-title' href='/photos/watch?b=%s'>%s</a>%s</div>"
            "<span class='vid-size'>%s · %s%s</span></div>"
            "<div class='vid-actions'>"
            "<a class='vid-btn vid-btn-dl' href='/photos/watch?b=%s'>Riproduci</a>"
            "<a class='vid-btn vid-btn-mp4' href='/photos/export?b=%s'>%s</a>"
            "<form method='POST' action='/photos/delete' style='margin:0' "
            "onsubmit=\"return confirm('Eliminare tutto il timelapse %s (%lu foto)?');\">"
            "<input type='hidden' name='b' value='%s'>"
            "<button type='submit' class='vid-btn vid-btn-del'>Elimina</button>"
            "</form></div></li>",
            ps->rel_base, base_enc, ps->name, icons,
            ps->when_h, ps->detail_h, meta_extra,
            base_enc, base_enc, "Crea video",
            ps->name, (unsigned long)ps->frames, ps->rel_base);
    }
    if (photo_count == 0) {
        off += snprintf(body + off, body_cap - off,
                        "<li class='vid-empty'>Nessun timelapse foto "
                        "(file <code>…_foto0001.jpg</code>) sulla SD. "
                        "I video timelapse H264 (1–3 fps) sono in <a href='/videos'>Video</a>; "
                        "qui solo la modalità <strong>Timelapse foto</strong> da Reg. video.</li>");
    } else if (truncated) {
        off += snprintf(body + off, body_cap - off,
                        "<li class='vid-empty'>Elenco troncato — troppi file da mostrare in una pagina.</li>");
    }
    off += snprintf(body + off, body_cap - off, "</ul>%s%s%s",
                    MEDIA_LIST_HINT, SCRUB_JS, MEDIA_LIST_TAIL);

    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    free(photos);
    return err;
}

static esp_err_t handle_videos_delete(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    char buf[384] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_redirect(req, "/videos?err=del");
    }
    buf[len] = 0;

    char raw[128] = {0};
    if (httpd_query_key_value(buf, "f", raw, sizeof(raw)) != ESP_OK) {
        return send_redirect(req, "/videos?err=del");
    }

    char rel[128] = {0};
    snprintf(rel, sizeof(rel), "%s", raw);
    if (!core_sd_ensure_rw()) {
        return send_redirect(req, "/videos?err=del");
    }
    if (!video_delete_rel(rel)) {
        return send_redirect(req, "/videos?err=del");
    }
    return send_redirect(req, "/videos?vdel=1");
}

static esp_err_t send_plain_err(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static void web_release_video_for_io(void) {
    // Una sola volta per sessione I/O: il preload foto chiama questo per ogni JPEG.
    if (!core_video_is_ready() && !core_live_is_running()) {
        return;
    }
    core_live_force_release();
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void web_free_stream_buf(void *buf, bool from_caps) {
    if (!buf) {
        return;
    }
    if (from_caps) {
        heap_caps_free(buf);
    } else {
        free(buf);
    }
}

typedef bool (*core_crypto_decrypt_fn)(core_crypto_ctx_t *, uint8_t *, size_t, uint64_t);

static bool mp4_plain_header_valid(const uint8_t *p, size_t n) {
    if (n < 8) {
        return false;
    }
    return p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p';
}

static bool avi_plain_header_valid(const uint8_t *p, size_t n) {
    if (n < 12) {
        return false;
    }
    return p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
           p[8] == 'A' && p[9] == 'V' && p[10] == 'I' && p[11] == ' ';
}

static bool media_plain_header_valid(const uint8_t *p, size_t n) {
    return mp4_plain_header_valid(p, n) || avi_plain_header_valid(p, n);
}

/* Payload already plaintext after MRBI (legacy stub-AES files). Do not XOR. */
static bool stream_crypt_identity(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t off) {
    (void)ctx;
    (void)buf;
    (void)len;
    (void)off;
    return true;
}

static core_crypto_decrypt_fn stream_pick_decrypt_fn(FILE *fp, core_crypto_ctx_t *ctx) {
    uint8_t enc[32];
    long pos = ftell(fp);
    if (fread(enc, 1, sizeof(enc), fp) != sizeof(enc)) {
        fseek(fp, pos, SEEK_SET);
        return core_crypto_crypt_at;
    }
    fseek(fp, pos, SEEK_SET);

    /* Check raw bytes first: old Luckfox builds wrote MRBI+IV then left the
     * MP4 in chiaro because AES-CTR was a no-op. Real AES would corrupt them. */
    if (media_plain_header_valid(enc, sizeof(enc))) {
        ESP_LOGW(TAG, "Stream media: payload già in chiaro dopo header MRBI");
        return stream_crypt_identity;
    }

    uint8_t probe_at[32];
    uint8_t probe_legacy[32];
    memcpy(probe_at, enc, sizeof(enc));
    memcpy(probe_legacy, enc, sizeof(enc));
    core_crypto_crypt_at(ctx, probe_at, sizeof(probe_at), 0);
    if (mp4_plain_header_valid(probe_at, sizeof(probe_at)) ||
        avi_plain_header_valid(probe_at, sizeof(probe_at))) {
        return core_crypto_crypt_at;
    }
    core_crypto_crypt_buffer_legacy(ctx, probe_legacy, sizeof(probe_legacy), 0);
    if (mp4_plain_header_valid(probe_legacy, sizeof(probe_legacy)) ||
        avi_plain_header_valid(probe_legacy, sizeof(probe_legacy))) {
        ESP_LOGW(TAG, "Stream media: decifra con schema legacy (encrypt_mp4_file)");
        return core_crypto_crypt_buffer_legacy;
    }
    ESP_LOGW(TAG, "Stream media: header ftyp/AVI non trovato — uso schema enc_writer (crypt_at)");
    return core_crypto_crypt_at;
}

static bool stream_parse_range(const char *range_hdr, uint64_t plain_len,
                               uint64_t *start, uint64_t *end_inclusive) {
    if (!range_hdr || strncmp(range_hdr, "bytes=", 6) != 0) {
        return false;
    }
    const char *p = range_hdr + 6;
    const char *dash = strchr(p, '-');
    if (!dash) {
        return false;
    }
    *start = (uint64_t)strtoull(p, nullptr, 10);
    if (dash[1] == '\0') {
        *end_inclusive = plain_len > 0 ? plain_len - 1 : 0;
    } else {
        *end_inclusive = (uint64_t)strtoull(dash + 1, nullptr, 10);
    }
    if (*start >= plain_len) {
        return false;
    }
    if (*end_inclusive >= plain_len) {
        *end_inclusive = plain_len - 1;
    }
    return *end_inclusive >= *start;
}

static esp_err_t httpd_send_fixed_headers(httpd_req_t *req, const char *status,
                                          const char *content_type, uint64_t body_len,
                                          const char *content_range_or_null,
                                          const char *content_disposition_or_null) {
    char hdr[640];
    int pos = snprintf(hdr, sizeof(hdr),
                       "HTTP/1.1 %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %llu\r\n",
                       status, content_type, (unsigned long long)body_len);
    if (content_disposition_or_null) {
        pos += snprintf(hdr + pos, sizeof(hdr) - pos, "Content-Disposition: %s\r\n",
                        content_disposition_or_null);
    }
    if (content_range_or_null) {
        pos += snprintf(hdr + pos, sizeof(hdr) - pos, "Content-Range: %s\r\n", content_range_or_null);
    }
    pos += snprintf(hdr + pos, sizeof(hdr) - pos, "Connection: close\r\n\r\n");
    if (httpd_send(req, hdr, (size_t)pos) < 0) {
        return ESP_FAIL;
    }
    req->chunked = -1; /* raw body follows; do not emit a second HTTP response */
    return ESP_OK;
}

static bool httpd_send_all_bytes(httpd_req_t *req, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = httpd_send(req, buf + sent, len - sent);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            vTaskDelay(1);
            continue;
        }
        sent += (size_t)n;
    }
    return true;
}

static void download_tune_socket(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return;
    }
    int nodelay = 0;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 65535;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

static esp_err_t stream_plain_mp4_body(httpd_req_t *req, FILE *fp, const char *rel,
                                       uint64_t plain_len, bool as_download) {
    char range_hdr[96] = {0};
    bool has_range = !as_download &&
                     (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK);
    uint64_t range_start = 0;
    uint64_t range_end = plain_len > 0 ? plain_len - 1 : 0;
    if (has_range && !stream_parse_range(range_hdr, plain_len, &range_start, &range_end)) {
        return send_plain_err(req, "416 Range Not Satisfiable", "Range non valido");
    }
    if (!has_range) {
        range_start = 0;
        range_end = plain_len > 0 ? plain_len - 1 : 0;
    }
    uint64_t send_len = range_end - range_start + 1;

    char *fbuf = (char *)heap_caps_malloc(CORE_SD_FILE_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool fbuf_caps = true;
    if (!fbuf) {
        fbuf = (char *)malloc(CORE_SD_FILE_BUF_BYTES);
        fbuf_caps = false;
    }
    if (fbuf) {
        setvbuf(fp, fbuf, _IOFBF, CORE_SD_FILE_BUF_BYTES);
    }

    size_t io_buf_size = VIDEO_DL_IO_CHUNK;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool buf_caps = true;
    if (!buf) {
        buf = (uint8_t *)malloc(io_buf_size);
        buf_caps = false;
    }
    if (!buf) {
        io_buf_size = CORE_CRYPTO_STREAM_IO_BYTES;
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        buf_caps = true;
    }
    if (!buf) {
        web_free_stream_buf(fbuf, fbuf_caps);
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente per lo stream");
    }

    const char *status = has_range ? "206 Partial Content" : "200 OK";
    bool is_avi = (strstr(rel, ".avi") != nullptr);
    const char *ctype = as_download ? "application/octet-stream"
                                    : (is_avi ? "video/x-msvideo" : "video/mp4");
    char content_range[80] = {0};
    const char *cr_ptr = nullptr;
    char disposition[160] = {0};
    const char *disp_ptr = nullptr;
    if (has_range) {
        snprintf(content_range, sizeof(content_range), "bytes %llu-%llu/%llu",
                 (unsigned long long)range_start, (unsigned long long)range_end,
                 (unsigned long long)plain_len);
        cr_ptr = content_range;
    }
    if (as_download) {
        const char *slash = strrchr(rel, '/');
        const char *fname = slash ? slash + 1 : rel;
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", fname);
        disp_ptr = disposition;
    } else {
        disp_ptr = "inline";
    }

    if (fseek(fp, (long)range_start, SEEK_SET) != 0) {
        web_free_stream_buf(fbuf, fbuf_caps);
        web_free_stream_buf(buf, buf_caps);
        return send_plain_err(req, "500 Internal Server Error", "Seek file fallito");
    }

    download_tune_socket(req);
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "Stream MP4 %s (chiaro): %s (%llu B%s%s)",
             as_download ? "download" : "play", rel, (unsigned long long)send_len,
             has_range ? ", Range " : "", has_range ? range_hdr : "");

    esp_err_t err = ESP_OK;
    if (httpd_send_fixed_headers(req, status, ctype, send_len, cr_ptr, disp_ptr) != ESP_OK) {
        err = ESP_FAIL;
    } else {
        uint64_t remaining = send_len;
        while (remaining > 0) {
            size_t to_read = io_buf_size;
            if ((uint64_t)to_read > remaining) {
                to_read = (size_t)remaining;
            }
            size_t n = fread(buf, 1, to_read, fp);
            if (n == 0) {
                break;
            }
            if (!httpd_send_all_bytes(req, (const char *)buf, n)) {
                ESP_LOGI(TAG, "Stream MP4: client disconnesso (file chiaro)");
                err = ESP_FAIL;
                break;
            }
            remaining -= n;
        }
    }
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (ms > 0 && err == ESP_OK) {
        ESP_LOGI(TAG, "Download HTTP: %llu B in %lld ms (%.1f KB/s)",
                 (unsigned long long)send_len, (long long)ms,
                 (double)send_len / (double)ms);
    }
    web_free_stream_buf(fbuf, fbuf_caps);
    web_free_stream_buf(buf, buf_caps);
    return err;
}

static esp_err_t stream_decrypted_mp4(httpd_req_t *req, const char *rel, bool as_download) {
    if (!video_rel_path_valid(rel)) {
        ESP_LOGW(TAG, "Stream MP4: path non valido");
        return send_plain_err(req, "400 Bad Request", "Path non valido");
    }

    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGE(TAG, "Stream MP4: SD non montata");
        return send_plain_err(req, "503 Service Unavailable", "SD non disponibile");
    }

    web_release_video_for_io();

    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, rel);

    struct stat fst;
    if (stat(path, &fst) != 0 || fst.st_size < 1024) {
        ESP_LOGW(TAG, "Stream MP4: file assente o troppo piccolo (%s)", path);
        return send_plain_err(req, "404 Not Found", "File non trovato");
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Stream MP4: fopen fallita (%s, errno=%d)", path, errno);
        return send_plain_err(req, "500 Internal Server Error", "Impossibile aprire il file");
    }

    uint8_t head[12] = {0};
    size_t head_n = fread(head, 1, sizeof(head), fp);
    fseek(fp, 0, SEEK_SET);

    uint32_t magic = 0;
    if (head_n >= sizeof(magic)) {
        memcpy(&magic, head, sizeof(magic));
    }

    // File in chiaro: MP4 (ftyp) o AVI (RIFF/AVI) da export timelapse.
    if (magic != CORE_CRYPTO_MAGIC &&
        (mp4_plain_header_valid(head, head_n) || avi_plain_header_valid(head, head_n))) {
        esp_err_t err = stream_plain_mp4_body(req, fp, rel, (uint64_t)fst.st_size, as_download);
        fclose(fp);
        return err;
    }

    if (magic != CORE_CRYPTO_MAGIC) {
        fclose(fp);
        ESP_LOGW(TAG, "Stream media: header MRBI/ftyp/AVI assente (%s)", path);
        return send_plain_err(req, "400 Bad Request", "File non valido");
    }

    uint64_t plain_len = (uint64_t)fst.st_size - CORE_CRYPTO_FILE_HEADER;

    if (fread(&magic, 1, sizeof(magic), fp) != sizeof(magic) || magic != CORE_CRYPTO_MAGIC) {
        fclose(fp);
        return send_plain_err(req, "400 Bad Request", "File non cifrato MRBI");
    }
    uint8_t iv[16];
    if (fread(iv, 1, sizeof(iv), fp) != sizeof(iv)) {
        fclose(fp);
        return send_plain_err(req, "400 Bad Request", "Header cifratura incompleto");
    }

    core_crypto_ctx_t ctx;
    if (!core_crypto_init_ctx(&ctx, s_settings->aes_key, iv)) {
        fclose(fp);
        return send_plain_err(req, "500 Internal Server Error", "Init decifratura fallita");
    }

    core_crypto_decrypt_fn decrypt_fn = stream_pick_decrypt_fn(fp, &ctx);

    char range_hdr[96] = {0};
    bool has_range = (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK);
    uint64_t range_start = 0;
    uint64_t range_end = plain_len > 0 ? plain_len - 1 : 0;
    if (has_range && !stream_parse_range(range_hdr, plain_len, &range_start, &range_end)) {
        core_crypto_deinit_ctx(&ctx);
        fclose(fp);
        return send_plain_err(req, "416 Range Not Satisfiable", "Range non valido");
    }
    if (!has_range) {
        range_start = 0;
        range_end = plain_len > 0 ? plain_len - 1 : 0;
    }
    uint64_t send_len = range_end - range_start + 1;

    char *fbuf = (char *)heap_caps_malloc(CORE_SD_FILE_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool fbuf_caps = true;
    if (!fbuf) {
        fbuf = (char *)heap_caps_malloc(CORE_SD_FILE_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (fbuf) {
        setvbuf(fp, fbuf, _IOFBF, CORE_SD_FILE_BUF_BYTES);
    }

    size_t io_buf_size = CORE_CRYPTO_STREAM_IO_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool buf_caps = true;
    if (!buf) {
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        io_buf_size = 8192;
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        io_buf_size = 8192;
        buf = (uint8_t *)malloc(io_buf_size);
        buf_caps = false;
    }
    if (!buf) {
        ESP_LOGE(TAG, "Stream MP4: buffer I/O esaurito (plain=%llu B)", (unsigned long long)plain_len);
        core_crypto_deinit_ctx(&ctx);
        web_free_stream_buf(fbuf, fbuf_caps);
        fclose(fp);
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente per lo stream");
    }

    const char *status = has_range ? "206 Partial Content" : "200 OK";
    const char *ctype = "video/mp4";
    if (as_download) {
        ctype = "application/octet-stream";
    } else {
        size_t rlen = strlen(rel);
        if (rlen >= 4 && strcmp(rel + rlen - 4, ".avi") == 0) {
            ctype = "video/x-msvideo";
        }
    }
    char content_range[80] = {0};
    const char *cr_ptr = nullptr;
    char disposition[160] = {0};
    const char *disp_ptr = nullptr;
    if (has_range) {
        snprintf(content_range, sizeof(content_range), "bytes %llu-%llu/%llu",
                 (unsigned long long)range_start, (unsigned long long)range_end,
                 (unsigned long long)plain_len);
        cr_ptr = content_range;
    }
    if (as_download) {
        const char *slash = strrchr(rel, '/');
        const char *fname = slash ? slash + 1 : rel;
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", fname);
        disp_ptr = disposition;
    } else {
        disp_ptr = "inline";
    }

    if (fseek(fp, (long)(CORE_CRYPTO_FILE_HEADER + range_start), SEEK_SET) != 0) {
        core_crypto_deinit_ctx(&ctx);
        web_free_stream_buf(fbuf, fbuf_caps);
        web_free_stream_buf(buf, buf_caps);
        fclose(fp);
        return send_plain_err(req, "500 Internal Server Error", "Seek file fallito");
    }

    ESP_LOGI(TAG, "Stream MP4 %s: %s (%llu B decifrati%s%s)",
             as_download ? "download" : "play", rel, (unsigned long long)send_len,
             has_range ? ", Range " : "",
             has_range ? range_hdr : "");

    if (httpd_send_fixed_headers(req, status, ctype, send_len, cr_ptr, disp_ptr) != ESP_OK) {
        core_crypto_deinit_ctx(&ctx);
        web_free_stream_buf(fbuf, fbuf_caps);
        web_free_stream_buf(buf, buf_caps);
        fclose(fp);
        return ESP_FAIL;
    }

    uint64_t offset = range_start;
    uint64_t remaining = send_len;
    esp_err_t err = ESP_OK;
    bool logged_ftyp = false;
    while (remaining > 0) {
        size_t to_read = io_buf_size;
        if ((uint64_t)to_read > remaining) {
            to_read = (size_t)remaining;
        }
        size_t n = fread(buf, 1, to_read, fp);
        if (n == 0) {
            break;
        }
        if (!decrypt_fn(&ctx, buf, n, offset)) {
            ESP_LOGE(TAG, "Stream MP4: decifratura fallita a offset %llu", (unsigned long long)offset);
            err = ESP_FAIL;
            break;
        }
        if (!logged_ftyp && offset == range_start && n >= 8) {
            logged_ftyp = true;
            if (mp4_plain_header_valid(buf, n)) {
                ESP_LOGI(TAG, "Stream media: header decifrato OK (ftyp)");
            } else if (avi_plain_header_valid(buf, n)) {
                ESP_LOGI(TAG, "Stream media: header decifrato OK (RIFF/AVI)");
            } else {
                ESP_LOGW(TAG, "Stream media: dati decifrati senza ftyp/AVI — file corrotto o chiave errata");
            }
        }
        if (httpd_send(req, (const char *)buf, n) < 0) {
            ESP_LOGI(TAG, "Stream MP4: client disconnesso a offset %llu", (unsigned long long)offset);
            err = ESP_FAIL;
            break;
        }
        offset += n;
        remaining -= n;
    }
    fclose(fp);
    web_free_stream_buf(fbuf, fbuf_caps);
    web_free_stream_buf(buf, buf_caps);
    core_crypto_deinit_ctx(&ctx);
    return err;
}

static void video_download_make_temp_rel(const char *src_rel, char *out, size_t out_len) {
    char safe[128] = {0};
    size_t j = 0;
    for (size_t i = 0; src_rel[i] && j + 1 < sizeof(safe); ++i) {
        char c = src_rel[i];
        if (c == '/' || c == '\\') {
            c = '_';
        } else if (c == ' ') {
            c = '_';
        }
        safe[j++] = c;
    }
    snprintf(out, out_len, VIDEO_DL_TEMP_DIR "/%s.plain", safe);
}

static bool video_download_ensure_temp_dir(void) {
    char path[192];
    snprintf(path, sizeof(path), "%s/" VIDEO_DL_TEMP_DIR, CORE_SD_MOUNT_POINT);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (mkdir(path, 0755) == 0) {
        return true;
    }
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool video_file_info(const char *src_rel, uint64_t *plain_len_out, bool *encrypted_out) {
    if (!src_rel || !plain_len_out || !encrypted_out) {
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, src_rel);
    struct stat fst;
    if (stat(path, &fst) != 0 || fst.st_size < 1024) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t head[12] = {0};
    size_t head_n = fread(head, 1, sizeof(head), fp);
    fclose(fp);

    uint32_t magic = 0;
    if (head_n >= sizeof(magic)) {
        memcpy(&magic, head, sizeof(magic));
    }
    if (magic != CORE_CRYPTO_MAGIC &&
        (mp4_plain_header_valid(head, head_n) || avi_plain_header_valid(head, head_n))) {
        *encrypted_out = false;
        *plain_len_out = (uint64_t)fst.st_size;
        return true;
    }
    if (magic == CORE_CRYPTO_MAGIC) {
        *encrypted_out = true;
        *plain_len_out = (uint64_t)fst.st_size - CORE_CRYPTO_FILE_HEADER;
        return true;
    }
    return false;
}

static bool video_plain_cache_valid(const char *temp_rel, uint64_t expected_plain) {
    if (!temp_rel || !temp_rel[0] || expected_plain < 1024) {
        return false;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, temp_rel);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if ((uint64_t)st.st_size != expected_plain) {
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t head[12] = {0};
    size_t n = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    return media_plain_header_valid(head, n);
}

static void video_download_task(void *arg) {
    video_download_job_t *job = (video_download_job_t *)arg;

    if (!core_sd_is_mounted()) {
        core_sd_reinit();
    }
    if (!core_sd_is_mounted() || !video_download_ensure_temp_dir()) {
        snprintf(job->error, sizeof(job->error), "SD non disponibile");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    char src_abs[192];
    snprintf(src_abs, sizeof(src_abs), "%s/%s", CORE_SD_MOUNT_POINT, job->src_rel);
    char dst_abs[256];
    snprintf(dst_abs, sizeof(dst_abs), "%s/%s", CORE_SD_MOUNT_POINT, job->out_rel);

    FILE *src = fopen(src_abs, "rb");
    if (!src) {
        snprintf(job->error, sizeof(job->error), "Apertura sorgente fallita");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint8_t head[12] = {0};
    size_t head_n = fread(head, 1, sizeof(head), src);
    fseek(src, 0, SEEK_SET);
    uint32_t magic = 0;
    if (head_n >= sizeof(magic)) {
        memcpy(&magic, head, sizeof(magic));
    }
    if (magic != CORE_CRYPTO_MAGIC) {
        fclose(src);
        snprintf(job->error, sizeof(job->error), "File non cifrato");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (fread(&magic, 1, sizeof(magic), src) != sizeof(magic) || magic != CORE_CRYPTO_MAGIC) {
        fclose(src);
        snprintf(job->error, sizeof(job->error), "Header MRBI assente");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    uint8_t iv[16];
    if (fread(iv, 1, sizeof(iv), src) != sizeof(iv)) {
        fclose(src);
        snprintf(job->error, sizeof(job->error), "Header IV incompleto");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    core_crypto_ctx_t ctx;
    if (!core_crypto_init_ctx(&ctx, s_settings->aes_key, iv)) {
        fclose(src);
        snprintf(job->error, sizeof(job->error), "Init AES fallita");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    core_crypto_decrypt_fn decrypt_fn = stream_pick_decrypt_fn(src, &ctx);
    ESP_LOGI(TAG, "Decifratura video: schema %s",
             decrypt_fn == stream_crypt_identity ? "passthrough (già chiaro)" :
             decrypt_fn == core_crypto_crypt_buffer_legacy ? "legacy CTR" : "AES-128-CTR");
    FILE *dst = fopen(dst_abs, "wb");
    if (!dst) {
        core_crypto_deinit_ctx(&ctx);
        fclose(src);
        snprintf(job->error, sizeof(job->error), "Creazione file temporaneo fallita");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Senza setvbuf FAT usa ~512 B: 16 MB → decine di migliaia di write SD (~minuti).
    const size_t fbuf_sz = CORE_SD_FILE_BUF_BYTES;
    char *src_fbuf = (char *)heap_caps_malloc(fbuf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *dst_fbuf = (char *)heap_caps_malloc(fbuf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (src_fbuf) {
        setvbuf(src, src_fbuf, _IOFBF, fbuf_sz);
    }
    if (dst_fbuf) {
        setvbuf(dst, dst_fbuf, _IOFBF, fbuf_sz);
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(VIDEO_DL_IO_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool buf_caps = true;
    if (!buf) {
        buf = (uint8_t *)malloc(VIDEO_DL_IO_CHUNK);
        buf_caps = false;
    }
    if (!buf) {
        fclose(dst);
        unlink(dst_abs);
        core_crypto_deinit_ctx(&ctx);
        fclose(src);
        web_free_stream_buf(src_fbuf, true);
        web_free_stream_buf(dst_fbuf, true);
        snprintf(job->error, sizeof(job->error), "Memoria insufficiente");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "Decifratura video → %s (%llu B, chunk %u)", job->out_rel,
             (unsigned long long)job->total_bytes, (unsigned)VIDEO_DL_IO_CHUNK);

    uint64_t offset = 0;
    bool ok = true;
    while (offset < job->total_bytes) {
        size_t to_read = VIDEO_DL_IO_CHUNK;
        if ((uint64_t)to_read > job->total_bytes - offset) {
            to_read = (size_t)(job->total_bytes - offset);
        }
        size_t n = fread(buf, 1, to_read, src);
        if (n == 0) {
            ok = false;
            break;
        }
        if (!decrypt_fn(&ctx, buf, n, offset)) {
            ok = false;
            break;
        }
        size_t nw = fwrite(buf, 1, n, dst);
        if (nw != n) {
            ok = false;
            break;
        }
        offset += n;
        job->done_bytes = offset;
    }

    web_free_stream_buf(buf, buf_caps);
    fflush(dst);
    fclose(dst);
    fclose(src);
    web_free_stream_buf(src_fbuf, true);
    web_free_stream_buf(dst_fbuf, true);
    core_crypto_deinit_ctx(&ctx);

    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (ok && offset == job->total_bytes) {
        FILE *chk = fopen(dst_abs, "rb");
        uint8_t head[12] = {0};
        size_t hn = chk ? fread(head, 1, sizeof(head), chk) : 0;
        if (chk) {
            fclose(chk);
        }
        if (!media_plain_header_valid(head, hn)) {
            ok = false;
            ESP_LOGE(TAG, "Decifratura: output senza ftyp/AVI (prime=%02x %02x %02x %02x)",
                     head[0], head[1], head[2], head[3]);
            snprintf(job->error, sizeof(job->error),
                     "Output non è MP4/AVI — chiave o file corrotto");
        }
    }
    if (!ok || offset != job->total_bytes) {
        unlink(dst_abs);
        if (!job->error[0]) {
            snprintf(job->error, sizeof(job->error), "Decifratura interrotta");
        }
        job->failed = true;
        ESP_LOGW(TAG, "Decifratura fallita dopo %lld ms (%llu/%llu B)",
                 (long long)ms, (unsigned long long)offset,
                 (unsigned long long)job->total_bytes);
    } else {
        job->error[0] = 0;
        ESP_LOGI(TAG, "Decifratura video completata: %s (%llu B in %lld ms)",
                 job->out_rel, (unsigned long long)offset, (long long)ms);
    }
    job->done = true;
    job->running = false;
    job->task = nullptr;
    vTaskDelete(nullptr);
}

static const char *VIDEOS_DOWNLOAD_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Scarica video</title>
<style>
body{font-family:sans-serif;max-width:640px;margin:24px auto}
nav a{margin-right:12px}
.box{margin:20px 0;padding:16px;border:1px solid #c5d4f5;border-radius:8px;background:#eef3ff}
#title{margin:0 0 8px}
#meta{color:#555;font-size:14px;margin:0 0 14px;word-break:break-all}
.bar-wrap{height:18px;background:#dde5f5;border-radius:9px;overflow:hidden;margin:10px 0}
#fill{height:100%;width:0;background:#2266cc;transition:width .2s}
#pct{font-size:28px;font-weight:700;color:#1a4480;margin:8px 0}
#status{color:#333;font-size:14px;min-height:1.4em}
#err{color:#a33;margin-top:10px}
.btn{display:inline-block;margin-top:14px;padding:10px 16px;border-radius:4px;text-decoration:none;
  font-weight:600;color:#fff !important;background:#555}
.btn-dl{background:#2266cc;font-size:16px;padding:12px 20px}
.btn-dl:hover{background:#1a52a3}
.btn-dl.pulse{animation:dlpulse 1.2s ease-in-out infinite}
@keyframes dlpulse{0%,100%{transform:scale(1)}50%{transform:scale(1.04)}}
input[type=range]{width:100%;margin:8px 0}
</style></head><body>
)HTML";

static const char *VIDEOS_DOWNLOAD_JS = R"JS(
<script>
(function(){
  var rel=window.__VID_DL_REL__||"";
  var pctEl=document.getElementById("pct"),fill=document.getElementById("fill"),
      slider=document.getElementById("slider"),st=document.getElementById("status"),
      err=document.getElementById("err"),dl=document.getElementById("dl"),
      title=document.getElementById("title");
  if(!rel){err.textContent="Parametro file mancante";return;}
  function setP(p,msg){
    p=Math.max(0,Math.min(100,p|0));
    pctEl.textContent=p+"%";
    fill.style.width=p+"%";
    slider.value=String(p);
    if(msg)st.textContent=msg;
  }
  function fmtBytes(n){
    n=Number(n)||0;
    if(n<1024)return n+" B";
    if(n<1048576)return (n/1024).toFixed(1)+" KB";
    return (n/1048576).toFixed(1)+" MB";
  }
  var pollTimer=null;
  function dlUrl(){return "/videos/download/file?f="+encodeURIComponent(rel);}
  function dlName(){return rel.split("/").pop()||"video.mp4";}
  function showDownloadButton(autoMsg){
    var url=dlUrl(),name=dlName();
    dl.href=url;
    dl.setAttribute("download",name);
    dl.style.display="inline-block";
    dl.className="btn btn-dl pulse";
    dl.textContent="Scarica "+name;
    dl.onclick=function(){
      st.textContent="Trasferimento file… — attendi il gestore download del browser";
      setP(100,"Download in corso");
    };
    setP(100,autoMsg||"Pronto per il download");
    st.textContent="Clicca il pulsante blu «Scarica» per salvare il file sul PC";
    title.textContent="Download pronto";
  }
  fetch("/videos/download/start?f="+encodeURIComponent(rel),{credentials:"same-origin"})
  .then(function(r){if(!r.ok)throw new Error("HTTP "+r.status);return r.json();})
  .then(function(j){
    if(j&&j.plain){showDownloadButton("File in chiaro");return;}
    if(j&&j.cached){showDownloadButton("File già decifrato");return;}
    if(j&&j.ok===false)throw new Error(j.error||"Avvio fallito");
    title.textContent="Decifratura AES";
    st.textContent="Decifratura in corso…";
    pollTimer=setInterval(function(){
      fetch("/videos/download/status",{credentials:"same-origin"})
      .then(function(r){return r.json();})
      .then(function(j){
        var total=Number(j.total)||0,done=Number(j.done)||0;
        var p=total?Math.round(100*done/total):0;
        if(j.ready)p=100;
        setP(p,fmtBytes(done)+" / "+fmtBytes(total));
        if(j.failed){
          clearInterval(pollTimer);pollTimer=null;
          err.textContent=j.error||"Errore decifratura";st.textContent="Fallito";return;
        }
        if(j.ready){
          clearInterval(pollTimer);pollTimer=null;
          showDownloadButton("Decifratura completata");
        }
      }).catch(function(e){err.textContent=e.message||String(e);});
    },800);
  }).catch(function(e){err.textContent="Avvio fallito: "+(e.message||String(e));st.textContent="Errore";});
})();
</script>
)JS";

static esp_err_t handle_videos_watch(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return send_plain_err(req, "400 Bad Request", "Parametro f mancante o non valido");
    }
    char rel_enc[128];
    url_encode_query(rel, rel_enc, sizeof(rel_enc));
    char loc[192];
    snprintf(loc, sizeof(loc), "/videos/download?f=%s", rel_enc);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t handle_videos_download(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return send_plain_err(req, "400 Bad Request", "Parametro f mancante o non valido");
    }

    const char *slash = strrchr(rel, '/');
    const char *fname = slash ? slash + 1 : rel;
    char rel_enc[128];
    url_encode_query(rel, rel_enc, sizeof(rel_enc));

    char abs_path[192];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", CORE_SD_MOUNT_POINT, rel);
    const bool encrypted = media_file_is_encrypted(abs_path);
    if (encrypted && (!s_settings || !s_settings->aes_key_valid)) {
        return send_plain_err(req, "500 Internal Server Error", "Chiave AES non configurata");
    }

    char rel_js[200];
    size_t o = 0;
    rel_js[o++] = '"';
    for (size_t i = 0; rel[i] && o + 2 < sizeof(rel_js); ++i) {
        char c = rel[i];
        if (c == '\\' || c == '"') {
            rel_js[o++] = '\\';
        }
        rel_js[o++] = c;
    }
    rel_js[o++] = '"';
    rel_js[o] = 0;

    char mid[640];
    snprintf(mid, sizeof(mid),
             "<div class='box'>"
             "<h2 id='title'>Scarica video</h2>"
             "<p id='meta'>%s%s</p>"
             "<div id='pct'>0%%</div>"
             "<div class='bar-wrap'><div id='fill'></div></div>"
             "<input type='range' id='slider' min='0' max='100' value='0' disabled>"
             "<p id='status'>Preparazione…</p>"
             "<p id='err'></p>"
             "<a class='btn btn-dl' id='dl' style='display:none' href='#'>Scarica file</a> "
             "<a class='btn' href='/videos'>Torna all'elenco</a>"
             "</div>"
             "<script>window.__VID_DL_REL__=%s;</script>",
             fname, encrypted ? " · <strong>AES</strong>" : "", rel_js);

    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_sendstr_chunk(req, VIDEOS_DOWNLOAD_HTML);
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, web_nav());
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, mid);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, VIDEOS_DOWNLOAD_JS);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, "</body></html>");
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, nullptr);
    }
    return err;
}

static void video_download_reset_stale(void) {
    if (s_video_download.running && s_video_download.task == nullptr) {
        if (!s_video_download.done) {
            s_video_download.failed = true;
            if (!s_video_download.error[0]) {
                snprintf(s_video_download.error, sizeof(s_video_download.error),
                         "Decifratura interrotta");
            }
            s_video_download.done = true;
        }
        s_video_download.running = false;
    }
}

static esp_err_t handle_videos_download_start(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return send_plain_err(req, "400 Bad Request", "Parametro f mancante o non valido");
    }
    video_download_reset_stale();
    if (!core_sd_is_mounted() && !core_sd_init()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD non montata\"}");
    }

    uint64_t plain_len = 0;
    bool encrypted = false;
    if (!video_file_info(rel, &plain_len, &encrypted)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"File non valido\"}");
    }
    if (!encrypted) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"plain\":true}");
    }
    if (!s_settings || !s_settings->aes_key_valid) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Chiave AES assente\"}");
    }

    char temp_rel[160];
    video_download_make_temp_rel(rel, temp_rel, sizeof(temp_rel));
    if (video_plain_cache_valid(temp_rel, plain_len)) {
        memset(&s_video_download, 0, sizeof(s_video_download));
        snprintf(s_video_download.src_rel, sizeof(s_video_download.src_rel), "%s", rel);
        snprintf(s_video_download.out_rel, sizeof(s_video_download.out_rel), "%s", temp_rel);
        snprintf(s_video_download.dl_name, sizeof(s_video_download.dl_name), "%s",
                 strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel);
        s_video_download.total_bytes = plain_len;
        s_video_download.done_bytes = plain_len;
        s_video_download.done = true;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"cached\":true}");
    }

    if (s_video_download.running && strcmp(s_video_download.src_rel, rel) == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    if (web_heavy_io_busy()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Operazione pesante già in corso\"}");
    }

    memset(&s_video_download, 0, sizeof(s_video_download));
    snprintf(s_video_download.src_rel, sizeof(s_video_download.src_rel), "%s", rel);
    snprintf(s_video_download.out_rel, sizeof(s_video_download.out_rel), "%s", temp_rel);
    const char *slash = strrchr(rel, '/');
    const char *fname = slash ? slash + 1 : rel;
    snprintf(s_video_download.dl_name, sizeof(s_video_download.dl_name), "%s", fname);
    s_video_download.total_bytes = plain_len;
    s_video_download.running = true;

    web_release_video_for_io();

    if (xTaskCreate(video_download_task, "vid_dl", 8192, &s_video_download, 5,
                    &s_video_download.task) != pdPASS) {
        s_video_download.running = false;
        s_video_download.failed = true;
        snprintf(s_video_download.error, sizeof(s_video_download.error), "Task non avviato");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Task non avviato\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_videos_download_status(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    bool ready = s_video_download.done && !s_video_download.failed;
    char json[400];
    snprintf(json, sizeof(json),
             "{\"running\":%s,\"finished\":%s,\"ready\":%s,\"failed\":%s,"
             "\"done\":%" PRIu64 ",\"total\":%" PRIu64 ",\"error\":%s%s%s}",
             s_video_download.running ? "true" : "false",
             s_video_download.done ? "true" : "false",
             ready ? "true" : "false",
             s_video_download.failed ? "true" : "false",
             (uint64_t)s_video_download.done_bytes,
             (uint64_t)s_video_download.total_bytes,
             s_video_download.error[0] ? "\"" : "null",
             s_video_download.error[0] ? s_video_download.error : "",
             s_video_download.error[0] ? "\"" : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_videos_download_file(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return send_plain_err(req, "400 Bad Request", "Parametro f mancante o non valido");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_plain_err(req, "503 Service Unavailable", "SD non montata");
    }

    char src_abs[192];
    snprintf(src_abs, sizeof(src_abs), "%s/%s", CORE_SD_MOUNT_POINT, rel);
    const bool encrypted = media_file_is_encrypted(src_abs);

    web_release_video_for_io();

    ESP_LOGI(TAG, "Download file: %s (encrypted_src=%d)", rel, encrypted ? 1 : 0);

    const char *stream_rel = rel;
    char temp_rel[160];
    if (encrypted) {
        video_download_make_temp_rel(rel, temp_rel, sizeof(temp_rel));
        uint64_t plain_len = 0;
        bool enc_flag = false;
        if (!video_file_info(rel, &plain_len, &enc_flag)) {
            return send_plain_err(req, "400 Bad Request", "File sorgente non valido");
        }
        if (!video_plain_cache_valid(temp_rel, plain_len)) {
            if (s_video_download.running && strcmp(s_video_download.src_rel, rel) == 0) {
                return send_plain_err(req, "503 Service Unavailable", "Decifratura in corso");
            }
            return send_plain_err(req, "404 Not Found",
                                  "File decifrato non pronto — torna indietro e riprova");
        }
        stream_rel = temp_rel;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, stream_rel);
    struct stat fst;
    if (stat(path, &fst) != 0 || fst.st_size < 1024) {
        return send_plain_err(req, "404 Not Found", "File non trovato");
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return send_plain_err(req, "500 Internal Server Error", "Impossibile aprire il file");
    }

    uint8_t head[12] = {0};
    size_t hn = fread(head, 1, sizeof(head), fp);
    fseek(fp, 0, SEEK_SET);
    uint32_t magic = 0;
    if (hn >= sizeof(magic)) {
        memcpy(&magic, head, sizeof(magic));
    }
    if (magic == CORE_CRYPTO_MAGIC) {
        fclose(fp);
        ESP_LOGW(TAG, "Download file: rifiuto MRBI grezzo %s", stream_rel);
        return send_plain_err(req, "409 Conflict",
                              "File ancora cifrato — attendi la decifratura 0–100%");
    }
    if (!media_plain_header_valid(head, hn)) {
        fclose(fp);
        return send_plain_err(req, "500 Internal Server Error",
                              "File decifrato non valido (manca ftyp/AVI)");
    }

    esp_err_t err = stream_plain_mp4_body(req, fp, rel, (uint64_t)fst.st_size, true);
    fclose(fp);
    return err;
}

static esp_err_t handle_photos_image(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_plain_err(req, "401 Unauthorized", "Login richiesto");
    }
    char rel[128] = {0};
    if (!photo_query_get_rel(req, rel, sizeof(rel))) {
        return send_plain_err(req, "400 Bad Request", "Parametro f non valido");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_plain_err(req, "503 Service Unavailable", "SD non montata");
    }
    web_release_video_for_io();
    return photos_send_jpeg_file(req, rel, false);
}

static esp_err_t handle_photos_download(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_plain_err(req, "401 Unauthorized", "Login richiesto");
    }
    char rel[128] = {0};
    if (!photo_query_get_rel(req, rel, sizeof(rel))) {
        return send_plain_err(req, "400 Bad Request", "Parametro f non valido");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_plain_err(req, "503 Service Unavailable", "SD non montata");
    }
    web_release_video_for_io();
    return photos_send_jpeg_file(req, rel, true);
}

static esp_err_t handle_photos_delete(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char buf[256] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_redirect(req, "/photos?err=del");
    }
    buf[len] = 0;
    char raw[128] = {0};
    if (!form_get_value(buf, "b", raw, sizeof(raw))) {
        return send_redirect(req, "/photos?err=del");
    }
    char base[128] = {0};
    url_decode(raw, base, sizeof(base));
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_redirect(req, "/photos?err=del");
    }
    if (!photo_session_delete(base)) {
        return send_redirect(req, "/photos?err=del");
    }
    return send_redirect(req, "/photos?pdel=1");
}

static esp_err_t handle_photos_delete_all(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_redirect(req, "/photos?err=del_all");
    }
    if (photos_delete_all_sessions() == 0) {
        return send_redirect(req, "/photos?err=del_all");
    }
    return send_redirect(req, "/photos?del_all_p=1");
}

static esp_err_t handle_videos_delete_all(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_redirect(req, "/videos?err=del_all");
    }
    if (videos_delete_all_mp4() == 0) {
        return send_redirect(req, "/videos?err=del_all");
    }
    return send_redirect(req, "/videos?del_all_v=1");
}

static esp_err_t handle_photos_frames_json(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_plain_err(req, "401 Unauthorized", "Login richiesto");
    }
    char base[128] = {0};
    if (!photo_query_get_base(req, base, sizeof(base))) {
        return send_plain_err(req, "400 Bad Request", "Parametro b mancante o non valido");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_plain_err(req, "503 Service Unavailable", "SD non montata");
    }

    char (*frame_rels)[96] = (char (*)[96])web_alloc_psram(PHOTO_SLIDESHOW_MAX_FRAMES * 96);
    if (!frame_rels) {
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }
    int frame_count = photo_session_collect_frames(base, frame_rels, PHOTO_SLIDESHOW_MAX_FRAMES);
    if (frame_count <= 0) {
        free(frame_rels);
        return send_plain_err(req, "404 Not Found", "Nessuna foto in questa sessione");
    }

    size_t json_cap = 192 + (size_t)frame_count * 112;
    char *json = (char *)web_alloc_psram(json_cap);
    if (!json) {
        free(frame_rels);
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    char capture[96] = "";
    if (s_settings) {
        uint16_t sw = 0;
        uint16_t sh = 0;
        snprintf(capture, sizeof(capture), "%ux%u impostati",
                 (unsigned)s_settings->video_width, (unsigned)s_settings->video_height);
        if (core_video_get_sensor_size(&sw, &sh)) {
            char tmp[96];
            snprintf(tmp, sizeof(tmp), "%s · sensore %ux%u", capture, (unsigned)sw, (unsigned)sh);
            strncpy(capture, tmp, sizeof(capture) - 1);
            capture[sizeof(capture) - 1] = '\0';
        }
    }

    size_t off = 0;
    off += snprintf(json + off, json_cap - off, "{\"base\":\"%s\",\"capture\":\"%s\",\"frames\":[",
                    base, capture);
    for (int i = 0; i < frame_count && off < json_cap - 96; ++i) {
        off += snprintf(json + off, json_cap - off, "%s\"%s\"", i ? "," : "", frame_rels[i]);
    }
    off += snprintf(json + off, json_cap - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    free(frame_rels);
    return err;
}

static esp_err_t handle_photos_watch(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char base[128] = {0};
    if (!photo_query_get_base(req, base, sizeof(base))) {
        return send_plain_err(req, "400 Bad Request", "Parametro b mancante o non valido");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PHOTOS_WATCH_HTML);
    httpd_resp_sendstr_chunk(req, web_nav());
    httpd_resp_sendstr_chunk(req, PHOTOS_WATCH_HTML_TAIL);
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static void photo_export_progress_cb(int done, int total, void *user) {
    photo_export_job_t *job = (photo_export_job_t *)user;
    if (!job) {
        return;
    }
    job->done_frames = done;
    job->total_frames = total;
}

static void photo_export_task(void *arg) {
    photo_export_job_t *job = (photo_export_job_t *)arg;
    char (*frame_rels)[96] = (char (*)[96])web_alloc_psram(PHOTO_SLIDESHOW_MAX_FRAMES * 96);
    char (*abs_paths)[192] = (char (*)[192])web_alloc_psram(PHOTO_SLIDESHOW_MAX_FRAMES * 192);
    if (!frame_rels || !abs_paths) {
        free(frame_rels);
        free(abs_paths);
        snprintf(job->error, sizeof(job->error), "Memoria insufficiente");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!core_sd_is_mounted()) {
        core_sd_reinit();
    }
    web_release_video_for_io();

    int frame_count = photo_session_collect_frames(job->base, frame_rels, PHOTO_SLIDESHOW_MAX_FRAMES);
    if (frame_count <= 0) {
        free(frame_rels);
        free(abs_paths);
        snprintf(job->error, sizeof(job->error), "Nessuna foto in sessione");
        job->failed = true;
        job->done = true;
        job->running = false;
        job->task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    job->total_frames = frame_count;
    job->done_frames = 0;
    for (int i = 0; i < frame_count; ++i) {
        snprintf(abs_paths[i], sizeof(abs_paths[i]), "%s/%s", CORE_SD_MOUNT_POINT, frame_rels[i]);
    }

    snprintf(job->out_rel, sizeof(job->out_rel), "%s_tl.avi", job->base);
    char out_abs[192];
    snprintf(out_abs, sizeof(out_abs), "%s/%s", CORE_SD_MOUNT_POINT, job->out_rel);

    const bool encrypt_out = s_settings && s_settings->rec_encrypt && s_settings->aes_key_valid;
    const uint8_t *aes_key = (s_settings && s_settings->aes_key_valid) ? s_settings->aes_key
                                                                       : nullptr;
    uint32_t export_fps = CORE_PHOTO_EXPORT_FPS;
    {
        char meta_base[192];
        snprintf(meta_base, sizeof(meta_base), "%s/%s", CORE_SD_MOUNT_POINT, job->base);
        uint32_t md = 0, mf = 0, mp = 0;
        if (core_sd_read_rec_meta(meta_base, &md, &mf, &mp) &&
            mp >= 1 && mp <= CORE_REC_PHOTO_META_FPS_MAX) {
            export_fps = mp;
        }
    }
    ESP_LOGI(TAG, "Export timelapse → AVI: %s (%d frame @ %lu fps, AES=%d)", job->out_rel,
             frame_count, (unsigned long)export_fps, encrypt_out ? 1 : 0);
    bool ok = core_recorder_export_jpegs_avi(out_abs, abs_paths, frame_count, (uint8_t)export_fps,
                                             aes_key, encrypt_out, photo_export_progress_cb, job);
    free(frame_rels);
    free(abs_paths);
    if (!ok) {
        snprintf(job->error, sizeof(job->error), "Creazione AVI fallita");
        job->failed = true;
    } else {
        job->done_frames = job->total_frames;
        job->error[0] = 0;
        uint32_t dur = (export_fps > 0)
                           ? (uint32_t)frame_count / export_fps
                           : (uint32_t)frame_count;
        (void)core_sd_write_rec_meta(out_abs, dur, (uint32_t)frame_count, export_fps);
    }
    job->done = true;
    job->running = false;
    job->task = nullptr;
    vTaskDelete(nullptr);
}

static const char *PHOTOS_EXPORT_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Crea video timelapse</title>
<style>
body{font-family:sans-serif;max-width:640px;margin:24px auto}
nav a{margin-right:12px}
.box{margin:20px 0;padding:16px;border:1px solid #c5d4f5;border-radius:8px;background:#eef3ff}
#title{margin:0 0 8px}
#meta{color:#555;font-size:14px;margin:0 0 14px}
.bar-wrap{height:18px;background:#dde5f5;border-radius:9px;overflow:hidden;margin:10px 0}
#fill{height:100%;width:0;background:#2a7a4b;transition:width .2s}
#pct{font-size:28px;font-weight:700;color:#1b5e20;margin:8px 0}
#status{color:#333;font-size:14px;min-height:1.4em}
#err{color:#a33;margin-top:10px}
.btn{display:inline-block;margin-top:14px;padding:10px 16px;border-radius:4px;text-decoration:none;
  font-weight:600;color:#fff !important;background:#555}
.btn-dl{background:#2a7a4b}.btn-dl:hover{background:#22633d}
input[type=range]{width:100%;margin:8px 0}
</style></head><body>
)HTML";

static esp_err_t handle_photos_export_page(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char base[128] = {0};
    if (!photo_query_get_base(req, base, sizeof(base))) {
        return send_plain_err(req, "400 Bad Request", "Parametro b mancante o non valido");
    }

    char base_js[160];
    size_t o = 0;
    base_js[o++] = '"';
    for (size_t i = 0; base[i] && o + 2 < sizeof(base_js); ++i) {
        char c = base[i];
        if (c == '\\' || c == '"') {
            base_js[o++] = '\\';
        }
        base_js[o++] = c;
    }
    base_js[o++] = '"';
    base_js[o] = 0;

    char base_enc[128];
    url_encode_query(base, base_enc, sizeof(base_enc));

    char body[4096];
    snprintf(body, sizeof(body),
        "%s%s"
        "<div class='box'>"
        "<h2 id='title'>Creazione video</h2>"
        "<p id='meta'>Sessione: %s</p>"
        "<div id='pct'>0%%</div>"
        "<div class='bar-wrap'><div id='fill'></div></div>"
        "<input type='range' id='slider' min='0' max='100' value='0' disabled>"
        "<p id='status'>Avvio…</p>"
        "<p id='err'></p>"
        "<a class='btn btn-dl' id='dl' style='display:none' href='#'>Scarica video</a> "
        "<a class='btn' href='/photos/watch?b=%s'>Torna allo slideshow</a> "
        "<a class='btn' href='/photos'>Elenco</a>"
        "</div>"
        "<script>(function(){"
        "var base=%s;"
        "var pctEl=document.getElementById('pct'),fill=document.getElementById('fill'),"
        "slider=document.getElementById('slider'),st=document.getElementById('status'),"
        "err=document.getElementById('err'),dl=document.getElementById('dl'),"
        "title=document.getElementById('title');"
        "function setP(p,msg){p=Math.max(0,Math.min(100,p|0));pctEl.textContent=p+'%%';"
        "fill.style.width=p+'%%';slider.value=String(p);if(msg)st.textContent=msg;}"
        "function startDownload(){"
        "dl.href='/photos/export/file?b='+encodeURIComponent(base);"
        "dl.style.display='inline-block';"
        "setTimeout(function(){dl.click();},200);}"
        "fetch('/photos/export/start?b='+encodeURIComponent(base),{credentials:'same-origin'})"
        ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
        ".then(function(j){"
        "if(j&&j.exists){"
        "title.textContent='Video già creato';"
        "setP(100,'Il video di questo timelapse è già pronto — download…');"
        "startDownload();"
        "return;"
        "}"
        "if(j&&j.ok===false){throw new Error(j.error||'Avvio fallito');}"
        "var t=setInterval(function(){"
        "fetch('/photos/export/status',{credentials:'same-origin'}).then(function(r){return r.json();})"
        ".then(function(j){"
        "var p=j.total?Math.round(100*j.done/j.total):0;"
        "setP(p,(j.done||0)+' / '+(j.total||0)+' foto');"
        "if(j.failed){clearInterval(t);err.textContent=j.error||'Errore';st.textContent='Fallito';return;}"
        "if(j.finished){clearInterval(t);setP(100,'Completato — download…');"
        "startDownload();}"
        "}).catch(function(e){err.textContent=e.message;});"
        "},400);"
        "}).catch(function(e){err.textContent='Avvio fallito: '+e.message;st.textContent='Errore';});"
        "})();</script></body></html>",
        PHOTOS_EXPORT_HTML, web_nav(), base, base_enc, base_js);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_photos_export_start(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char base[128] = {0};
    if (!photo_query_get_base(req, base, sizeof(base))) {
        return send_plain_err(req, "400 Bad Request", "Parametro b mancante o non valido");
    }
    if (photo_session_avi_exists(base)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"exists\":true}");
    }
    if (web_heavy_io_busy()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Operazione pesante già in corso\"}");
    }

    memset(&s_photo_export, 0, sizeof(s_photo_export));
    snprintf(s_photo_export.base, sizeof(s_photo_export.base), "%s", base);
    s_photo_export.running = true;

    if (xTaskCreate(photo_export_task, "photo_exp", 20480, &s_photo_export, 3,
                    &s_photo_export.task) != pdPASS) {
        s_photo_export.running = false;
        s_photo_export.failed = true;
        snprintf(s_photo_export.error, sizeof(s_photo_export.error), "Task non avviato");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Task non avviato\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_photos_export_status(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char json[256];
    snprintf(json, sizeof(json),
             "{\"running\":%s,\"finished\":%s,\"failed\":%s,\"done\":%d,\"total\":%d,\"error\":%s%s%s}",
             s_photo_export.running ? "true" : "false",
             s_photo_export.done ? "true" : "false",
             s_photo_export.failed ? "true" : "false",
             (int)s_photo_export.done_frames,
             (int)s_photo_export.total_frames,
             s_photo_export.error[0] ? "\"" : "null",
             s_photo_export.error[0] ? s_photo_export.error : "",
             s_photo_export.error[0] ? "\"" : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_photos_export_file(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char base[128] = {0};
    if (!photo_query_get_base(req, base, sizeof(base))) {
        return send_plain_err(req, "400 Bad Request", "Parametro b mancante o non valido");
    }
    char out_rel[128];
    snprintf(out_rel, sizeof(out_rel), "%s_tl.avi", base);
    if (s_photo_export.failed || (s_photo_export.done && s_photo_export.failed)) {
        return send_plain_err(req, "500 Internal Server Error",
                              s_photo_export.error[0] ? s_photo_export.error : "Export fallito");
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, out_rel);
    struct stat st;
    if (stat(path, &st) != 0) {
        return send_plain_err(req, "404 Not Found", "Video non ancora pronto");
    }
    return stream_decrypted_mp4(req, out_rel, true);
}

static bool s_wifi_driver_started = false;
static bool s_wifi_radio_started = false;
static volatile bool s_sta_has_ip = false;
static core_web_wifi_mode_t s_active_wifi_mode = CORE_WEB_WIFI_STA;

// Power-save modem → latenze 200–1000 ms tipiche su esp_hosted; GUI sembra morta.
static void wifi_disable_power_save(void) {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_PS_NONE fallito: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi power-save disabilitato (WIFI_PS_NONE)");
    }
}

static void wifi_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_has_ip = true;
        wifi_disable_power_save();
        ESP_LOGI(TAG, "WiFi STA connesso — lampeggio LED x3");
        core_status_led_notify_sta_connected();
    }
}

static void wifi_sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi STA associato, attendo DHCP");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_has_ip = false;
        ESP_LOGW(TAG, "WiFi STA disconnesso — riconnetto");
        (void)esp_wifi_connect();
    }
}

static esp_err_t init_once(esp_err_t err, const char *what) {
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return ESP_OK;
    ESP_LOGE(TAG, "%s fallito: %s", what, esp_err_to_name(err));
    return err;
}

static bool ensure_network_stack(void) {
    if (init_once(esp_netif_init(), "esp_netif_init") != ESP_OK ||
        init_once(esp_event_loop_create_default(), "esp_event_loop_create_default") != ESP_OK) {
        return false;
    }
    if (!s_wifi_events_registered) {
        esp_err_t err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_ip_event_handler, nullptr, nullptr);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "registrazione IP_EVENT fallita: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_sta_event_handler, nullptr, nullptr);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "registrazione WIFI_EVENT fallita: %s", esp_err_to_name(err));
            return false;
        }
        s_wifi_events_registered = true;
    }
    return true;
}

static bool wifi_init_driver(void) {
    if (s_wifi_driver_started) return true;
    if (!ensure_network_stack()) return false;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (init_once(esp_wifi_init(&cfg), "esp_wifi_init") != ESP_OK) return false;
    s_wifi_driver_started = true;
    return true;
}

static bool wifi_start_sta(const core_settings_t *s) {
    if (!s || s->wifi_ssid[0] == 0) return false;
    if (s_wifi_radio_started && s_active_wifi_mode == CORE_WEB_WIFI_STA) return true;
    s_sta_has_ip = false;
    if (!wifi_init_driver()) return false;

    esp_netif_create_default_wifi_sta();

    if (init_once(esp_wifi_set_mode(WIFI_MODE_STA), "esp_wifi_set_mode") != ESP_OK) return false;
    wifi_config_t wcfg = {};
    snprintf((char *)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), "%s", s->wifi_ssid);
    snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", s->wifi_pass);
    if (init_once(esp_wifi_set_config(WIFI_IF_STA, &wcfg), "esp_wifi_set_config") != ESP_OK) {
        return false;
    }
    if (init_once(esp_wifi_start(), "esp_wifi_start") != ESP_OK) return false;
    wifi_disable_power_save();
    if (init_once(esp_wifi_connect(), "esp_wifi_connect") != ESP_OK) return false;
    s_wifi_radio_started = true;
    s_active_wifi_mode = CORE_WEB_WIFI_STA;
    ESP_LOGI(TAG, "WiFi STA avviato: %s", s->wifi_ssid);
    return true;
}

static bool wifi_start_ap(const core_settings_t *s) {
    if (!s) return false;
    if (s_wifi_radio_started && s_active_wifi_mode == CORE_WEB_WIFI_AP) return true;
    if (!wifi_init_driver()) return false;

    esp_netif_create_default_wifi_ap();

    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "MrBin-%05lu", (unsigned long)s->core_id);

    wifi_config_t wcfg = {};
    snprintf((char *)wcfg.ap.ssid, sizeof(wcfg.ap.ssid), "%s", ap_ssid);
    wcfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    wcfg.ap.channel = 1;
    wcfg.ap.max_connection = 4;
    snprintf((char *)wcfg.ap.password, sizeof(wcfg.ap.password), "12345678");
    wcfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (init_once(esp_wifi_set_mode(WIFI_MODE_AP), "esp_wifi_set_mode") != ESP_OK) return false;
    if (init_once(esp_wifi_set_config(WIFI_IF_AP, &wcfg), "esp_wifi_set_config") != ESP_OK) {
        return false;
    }
    if (init_once(esp_wifi_start(), "esp_wifi_start") != ESP_OK) return false;
    wifi_disable_power_save();
    s_wifi_radio_started = true;
    s_active_wifi_mode = CORE_WEB_WIFI_AP;
    ESP_LOGI(TAG, "WiFi AP avviato: %s (pass 12345678)", ap_ssid);
    return true;
}

static bool sync_sntp_and_rtc(void);

bool core_web_sync_time_sta(const core_settings_t *s, uint32_t timeout_ms) {
    if (!s || s->wifi_ssid[0] == 0) {
        return false;
    }
    if (!wifi_start_sta(s)) {
        return false;
    }

    core_time_init();
    const uint32_t step_ms = 200;
    // Il router (GPIO5) a volte associa lo STA prima che il DHCP sia pronto:
    // un solo wait da 20 s lasciava l'interfaccia senza IP per sempre.
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t waited = 0;
        while (waited < timeout_ms) {
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip = {};
                if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
                    return sync_sntp_and_rtc();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(step_ms));
            waited += step_ms;
        }
        ESP_LOGW(TAG, "Timeout IP WiFi STA (%lu ms) — retry DHCP %d/2",
                 (unsigned long)timeout_ms, attempt + 1);
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            (void)esp_netif_dhcpc_stop(netif);
            (void)esp_netif_dhcpc_start(netif);
        }
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(400));
        (void)esp_wifi_connect();
    }
    return false;
}

static void calibrate_load_state(void) {
    s_isp_cal = *core_isp_cal_get();
}

// Applica fase Bayer (ferma live se attivo, sync ISP, opzionale NVS).
static bool web_apply_bayer_mode(uint8_t mode, bool save_nvs) {
    if (!core_settings_bayer_mode_valid(mode)) {
        return false;
    }
    if (s_settings) {
        s_settings->video_bayer_mode = mode;
    }
    core_video_set_bayer_mode(mode);

    if (core_live_is_running()) {
        core_live_force_release();
    }

    bool ok = false;
    if (!core_video_is_ready()) {
        ok = core_video_prepare_for_stream(s_settings);
    } else {
        ok = core_video_apply_bayer_sync();
        if (ok) {
            (void)core_isp_cal_apply_active();
        }
    }
    if (!ok) {
        return false;
    }
    if (save_nvs && s_settings) {
        return core_settings_save(s_settings);
    }
    return true;
}

static int parse_form_int(const char *body, const char *name, int def) {
    char tmp[24];
    if (!form_get_value(body, name, tmp, sizeof(tmp)) || tmp[0] == '\0') {
        return def;
    }
    return atoi(tmp);
}

static unsigned calibrate_ui_level(int v) {
    if (v < 0 || v > 255) {
        return 128;
    }
    return (unsigned)v;
}

static unsigned calibrate_ui_hue(unsigned hue) {
    if (hue == 0 || hue > 255) {
        return 128;
    }
    return hue;
}

static bool sync_sntp_and_rtc(void) {
    bool ok = core_time_sync_sntp(10000);
    if (ok && core_rtc_is_present()) {
        if (core_rtc_sync_from_system()) {
            ESP_LOGI(TAG, "Ora SNTP scritta sul DS3231");
            (void)core_rtc_sync_system_time();
        } else {
            ESP_LOGE(TAG, "Scrittura ora su DS3231 fallita — banner GUI attivo");
        }
    }
    return ok;
}

static void calibrate_sntp_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    /* P4 path: STA + SSID. Luckfox: eth0 already up from mrbin-init (no SSID). */
    if (s_settings && s_settings->wifi_ssid[0] != '\0') {
        if (core_web_sync_time_sta(s_settings, 20000)) {
            vTaskDelete(nullptr);
            return;
        }
    }
    core_time_init();
    /* Retry: DHCP may replace the static lab IP / default route after a few seconds. */
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (hal_net_refresh_ip()) {
            if (sync_sntp_and_rtc()) {
                vTaskDelete(nullptr);
                return;
            }
        } else {
            ESP_LOGW(TAG, "SNTP boot: nessun IP (tentativo %d/6)", attempt + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGW(TAG, "SNTP boot: timeout — orario non sincronizzato");
    vTaskDelete(nullptr);
}

static esp_err_t handle_calibrate_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }

    calibrate_load_state();
    const core_isp_profile_t *p = &s_isp_cal.profiles[0];
    const unsigned bri = calibrate_ui_level(p->brightness);
    const unsigned con = calibrate_ui_level(p->contrast);
    const unsigned sat = calibrate_ui_level(p->saturation);
    const unsigned hue = calibrate_ui_hue(p->hue);

    char *html = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        html = (char *)malloc(16384);
    }
    if (!html) {
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    snprintf(
        html, 16384,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Calibrazione</title>"
        "<style>body{font-family:sans-serif;max-width:1100px;margin:20px auto;padding:0 12px;background:#fff;color:#222}"
        "nav a{margin-right:12px}label{display:block;margin-top:14px;font-weight:600}"
        "input[type=range]{width:100%%;margin:6px 0}button{padding:8px 14px;margin:8px 8px 0 0}"
        ".layout{display:grid;grid-template-columns:minmax(280px,420px) minmax(0,1fr);gap:16px;align-items:start}"
        "@media(max-width:800px){.layout{grid-template-columns:1fr}}"
        ".box{border:1px solid #ccc;border-radius:8px;padding:12px;background:#fff}"
        ".hint{font-size:13px;color:#555;line-height:1.45}"
        ".val{font-weight:500;color:#222}"
        "img{width:100%%;background:#111;min-height:200px;max-height:70vh;object-fit:contain}</style></head><body>"
        "%s"
        "<h2>Calibrazione</h2>"
        "<p class=\"hint\">Una sola regolazione per la <strong>SC3336</strong>. "
        "Muovi gli slider: l'anteprima cambia subito. "
        "<strong>128</strong> &egrave; il neutro. Salva per usarla anche nei clip PIR.</p>"
        "<form id=\"calForm\">"
        "<div class=\"layout\"><div class=\"box\">"
        "<label><input type=\"checkbox\" id=\"manualIspEn\" name=\"manual_isp_en\" value=\"1\" %s style=\"width:auto\"> "
        "Calibrazione manuale</label>"
        "<p class=\"hint\">Spenta: immagine automatica (128 su tutti i controlli).</p>"
        "<label>Luminosit&agrave; <span class=\"val\" id=\"vBri\">%u</span></label>"
        "<input class=\"isp\" name=\"p_bri_0\" id=\"bri\" type=\"range\" min=\"0\" max=\"255\" value=\"%u\">"
        "<label>Contrasto <span class=\"val\" id=\"vCon\">%u</span></label>"
        "<input class=\"isp\" name=\"p_con_0\" id=\"con\" type=\"range\" min=\"0\" max=\"255\" value=\"%u\">"
        "<label>Saturazione <span class=\"val\" id=\"vSat\">%u</span></label>"
        "<input class=\"isp\" name=\"p_sat_0\" id=\"sat\" type=\"range\" min=\"0\" max=\"255\" value=\"%u\">"
        "<label>Tinta <span class=\"val\" id=\"vHue\">%u</span></label>"
        "<input class=\"isp\" name=\"p_hue_0\" id=\"hue\" type=\"range\" min=\"0\" max=\"255\" value=\"%u\">"
        "<div>"
        "<button type=\"button\" id=\"resetBtn\">Neutro</button>"
        "<button type=\"submit\">Salva</button>"
        "</div>"
        "<p id=\"saveStatus\" class=\"hint\"></p>"
        "</div>"
        "<div class=\"box\">"
        "<img id=\"liveImg\" alt=\"Anteprima\">"
        "<p id=\"prevStatus\" class=\"hint\">Avvio anteprima…</p>"
        "</div></div></form>"
        "<script>"
        "const map={bri:'vBri',con:'vCon',sat:'vSat',hue:'vHue'};"
        "function show(){Object.keys(map).forEach(id=>{document.getElementById(map[id]).textContent=document.getElementById(id).value;});}"
        "function body(){"
        " const fd=new URLSearchParams(); fd.set('idx','0');"
        " if(document.getElementById('manualIspEn').checked) fd.set('manual_isp_en','1');"
        " ['p_bri','p_con','p_sat','p_hue'].forEach(k=>{"
        "  const el=document.querySelector('[name=\"'+k+'_0\"]'); if(el) fd.set(k+'_0',el.value);"
        " }); return fd;}"
        "let pvT=null;"
        "async function applyPreview(){"
        " const st=document.getElementById('prevStatus');"
        " const man=document.getElementById('manualIspEn').checked;"
        " st.textContent=man?'Applico…':'Automatico…';"
        " try{const r=await fetch('/calibrate/preview',{method:'POST',credentials:'same-origin',"
        " headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body().toString()});"
        " const j=await r.json();"
        " st.textContent=(r.ok&&j.ok)?(man?'Anteprima aggiornata':'Automatico (128)'):'Errore anteprima';"
        " }catch(e){st.textContent='Errore rete';}}"
        "function sched(){clearTimeout(pvT);pvT=setTimeout(applyPreview,80);}"
        "document.querySelectorAll('input.isp').forEach(el=>{"
        " el.oninput=()=>{document.getElementById('manualIspEn').checked=true;show();sched();};"
        "});"
        "document.getElementById('manualIspEn').onchange=()=>sched();"
        "document.getElementById('resetBtn').onclick=()=>{"
        " ['bri','con','sat','hue'].forEach(id=>{document.getElementById(id).value='128';});"
        " document.getElementById('manualIspEn').checked=false; show(); sched();"
        "};"
        "document.getElementById('calForm').onsubmit=async(ev)=>{"
        " ev.preventDefault();"
        " const st=document.getElementById('saveStatus');"
        " st.textContent='Salvataggio…';"
        " try{"
        "  const r=await fetch('/calibrate',{method:'POST',credentials:'same-origin',"
        "  headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body().toString()});"
        "  st.textContent=(r.ok&&r.url.indexOf('err=')<0)?'Salvata':'Errore salvataggio';"
        "  applyPreview();"
        " }catch(e){st.textContent='Errore rete';}"
        "};"
        "show();"
        "document.getElementById('liveImg').src='/live/stream?ts='+Date.now();"
        "setTimeout(applyPreview,900);"
        "setTimeout(applyPreview,2500);"
        "</script></body></html>",
        web_nav(), s_isp_cal.manual_isp_enabled ? "checked" : "",
        bri, bri, con, con, sat, sat, hue, hue);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    heap_caps_free(html);
    return err;
}

static void calibrate_parse_profile_from_form(const char *body, unsigned idx, core_isp_profile_t *p) {
    char key[24];
    char tmp[32];
    snprintf(key, sizeof(key), "p_en_%u", idx);
    p->enabled = form_get_value(body, key, tmp, sizeof(tmp));
    snprintf(key, sizeof(key), "p_mwb_%u", idx);
    p->manual_wb = form_get_value(body, key, tmp, sizeof(tmp));
    snprintf(key, sizeof(key), "p_name_%u", idx);
    if (form_get_value(body, key, tmp, sizeof(tmp))) {
        snprintf(p->name, sizeof(p->name), "%s", tmp);
    }
    snprintf(key, sizeof(key), "p_h0_%u", idx);
    p->hour_start = (uint8_t)parse_form_int(body, key, p->hour_start);
    snprintf(key, sizeof(key), "p_m0_%u", idx);
    p->min_start = (uint8_t)parse_form_int(body, key, p->min_start);
    snprintf(key, sizeof(key), "p_h1_%u", idx);
    p->hour_end = (uint8_t)parse_form_int(body, key, p->hour_end);
    snprintf(key, sizeof(key), "p_m1_%u", idx);
    p->min_end = (uint8_t)parse_form_int(body, key, p->min_end);
    snprintf(key, sizeof(key), "p_red_%u", idx);
    p->red_gain_x1000 = (uint16_t)parse_form_int(body, key, p->red_gain_x1000);
    snprintf(key, sizeof(key), "p_blue_%u", idx);
    p->blue_gain_x1000 = (uint16_t)parse_form_int(body, key, p->blue_gain_x1000);
    snprintf(key, sizeof(key), "p_sat_%u", idx);
    p->saturation = (int16_t)parse_form_int(body, key, p->saturation);
    snprintf(key, sizeof(key), "p_con_%u", idx);
    p->contrast = (int16_t)parse_form_int(body, key, p->contrast);
    snprintf(key, sizeof(key), "p_bri_%u", idx);
    p->brightness = (int16_t)parse_form_int(body, key, p->brightness);
    snprintf(key, sizeof(key), "p_hue_%u", idx);
    p->hue = (uint16_t)parse_form_int(body, key, p->hue);
    snprintf(key, sizeof(key), "p_focus_%u", idx);
    p->focus_pos = (uint16_t)parse_form_int(body, key, p->focus_pos);
    core_isp_cal_normalize_profile(p);
}

static esp_err_t handle_calibrate_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }

    char buf[8192];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/calibrate?err=body");
    }

    calibrate_load_state();
    char tmp[8];
    s_isp_cal.manual_isp_enabled = form_get_value(buf, "manual_isp_en", tmp, sizeof(tmp));
    s_isp_cal.schedule_enabled = false;
    s_isp_cal.default_profile = 0;
    calibrate_parse_profile_from_form(buf, 0, &s_isp_cal.profiles[0]);
    s_isp_cal.profiles[0].enabled = true;
    s_isp_cal.profiles[0].manual_wb = s_isp_cal.manual_isp_enabled;
    if (s_isp_cal.profiles[0].name[0] == '\0') {
        snprintf(s_isp_cal.profiles[0].name, sizeof(s_isp_cal.profiles[0].name), "Calibrazione");
    }
    for (unsigned i = 1; i < CORE_ISP_PROFILE_COUNT; ++i) {
        s_isp_cal.profiles[i] = s_isp_cal.profiles[0];
    }

    if (!core_isp_cal_save(&s_isp_cal)) {
        return send_redirect(req, "/calibrate?err=save");
    }
    if (!core_rec_fast_requested()) {
        core_video_isp_live_pre();
    }
    (void)core_isp_cal_apply_active();
    return send_redirect(req, "/calibrate?ok=1");
}

static esp_err_t handle_calibrate_bayer_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Non autenticato\"}");
    }
    char buf[128];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Body mancante\"}");
    }
    char tmp[8];
    const unsigned mode_u = (unsigned)parse_form_int(buf, "bayer_mode", CORE_VIDEO_BAYER_AUTO);
    if (mode_u > CORE_VIDEO_BAYER_MAX) {
        return send_json(req, "{\"ok\":false,\"msg\":\"Fase non valida\"}");
    }
    const uint8_t mode = (uint8_t)mode_u;
    const bool save = form_get_value(buf, "save", tmp, sizeof(tmp));
    const bool ok = web_apply_bayer_mode(mode, save);
    char json[160];
    snprintf(json, sizeof(json),
             "{\"ok\":%s,\"label\":\"%s\",\"effective\":\"%s\",\"saved\":%s}",
             ok ? "true" : "false", core_video_bayer_mode_label(mode),
             core_video_bayer_effective_name(core_video_get_vflip(), core_video_get_hflip()),
             (ok && save) ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t handle_calibrate_preview_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }

    char buf[1024];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_json(req, "{\"ok\":false}");
    }

    unsigned idx = (unsigned)parse_form_int(buf, "idx", 0);
    if (idx >= CORE_ISP_PROFILE_COUNT) {
        idx = 0;
    }

    calibrate_load_state();
    char tmp[8];
    bool manual_isp = form_get_value(buf, "manual_isp_en", tmp, sizeof(tmp));
    bool focus_only = form_get_value(buf, "focus_only", tmp, sizeof(tmp));
    core_isp_profile_t preview = s_isp_cal.profiles[idx];
    calibrate_parse_profile_from_form(buf, idx, &preview);
    bool ok = core_isp_cal_apply_preview(&preview, manual_isp, focus_only);
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// Ritorna i gain WB attuali dell'ISP (con AWB attivo = valori dell'automatico).
static esp_err_t handle_calibrate_wb_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    uint16_t red = 0, blue = 0;
    if (!core_isp_cal_read_wb(&red, &blue)) {
        return send_json(req, "{\"ok\":false}");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"red\":%u,\"blue\":%u}", (unsigned)red,
             (unsigned)blue);
    return send_json(req, json);
}

static size_t hw_status_append_a1f_log_html(char *html, size_t cap, size_t off) {
    const unsigned n = core_rtc_a1f_log_count();
    off += (size_t)snprintf(
        html + off, cap - off,
        "<h3>Ultimi azzeramenti flag A1F</h3>"
        "<p class=\"hint\">Registro degli ultimi %d clear del bit <strong>A1F</strong> "
        "(allarme 1 scattato) sul DS3231 — tipicamente al boot da sveglia RTC.</p>",
        CORE_RTC_A1F_LOG_MAX);
    if (n == 0) {
        off += (size_t)snprintf(html + off, cap - off,
                                "<p class=\"hint\"><em>Nessun azzeramento A1F registrato.</em></p>");
        return off;
    }
    off += (size_t)snprintf(html + off, cap - off,
                            "<table><tr><th>#</th><th>Data e ora</th></tr>");
    for (unsigned i = 0; i < n && off < cap - 256; ++i) {
        int64_t epoch = 0;
        if (!core_rtc_a1f_log_get(i, &epoch)) {
            break;
        }
        char when[32];
        core_rtc_a1f_log_format_ts(epoch, when, sizeof(when));
        off += (size_t)snprintf(html + off, cap - off,
                                "<tr><td>%u</td><td>%s</td></tr>", i + 1, when);
    }
    off += (size_t)snprintf(html + off, cap - off, "</table>");
    return off;
}

static void hw_status_fmt_a1f(bool rtc_present, bool a1f_set, char *out, size_t out_len) {
    if (!rtc_present) {
        snprintf(out, out_len, "N/A (DS3231 assente)");
        return;
    }
    snprintf(out, out_len, "%s", a1f_set ? "SET (1)" : "CLEAR (0)");
}

static void hw_status_fmt_hub(int level, bool hub_on, char *out, size_t out_len) {
    snprintf(out, out_len, "%s — <strong>%s</strong>", level ? "HIGH" : "LOW",
             hub_on ? "HUB ON" : "HUB OFF");
}

static void hw_status_fmt_modem(int level, char *out, size_t out_len) {
    const bool on = level != 0;
    snprintf(out, out_len, "%s — <strong>%s</strong>", level ? "HIGH" : "LOW", on ? "MODEM ON" : "MODEM OFF");
}

static void hw_status_fmt_mode(int level, char *out, size_t out_len) {
    snprintf(out, out_len, "%s — %s", level ? "HIGH" : "LOW", level ? "CONFIG" : "PIR");
}

static size_t hw_status_append_pin_panel(char *html, size_t cap, size_t off, const char *title,
                                        const core_gpio_boot_snapshot_t *snap, bool rtc_present,
                                        bool a1f_set, bool hub_on, bool with_controls) {
    char a1f[48];
    char hub[48];
    char modem[48];
    char mode[48];
    hw_status_fmt_a1f(rtc_present, a1f_set, a1f, sizeof(a1f));
    hw_status_fmt_hub(snap->sqw_level, hub_on, hub, sizeof(hub));
    hw_status_fmt_modem(snap->modem_level, modem, sizeof(modem));
    hw_status_fmt_mode(snap->mode_level, mode, sizeof(mode));

    // Comandi manuali solo sul pannello NOW: su BOOT sarebbero uno snapshot storico.
    char hub_btn[224] = "";
    char modem_btn[224] = "";
    if (with_controls) {
        snprintf(hub_btn, sizeof(hub_btn),
                 "<form method=\"POST\" action=\"/hw_status/hub_%s\" class=\"pin-form\">"
                 "<button type=\"submit\" class=\"pin-btn\">%s</button></form>",
                 hub_on ? "off" : "on", hub_on ? "SPEGNI" : "ACCENDI");
        // Stato firmware, non livello del pad: se il pad è guasto il pulsante resta coerente.
        const bool modem_on = core_gpio_modem_is_on();
        snprintf(modem_btn, sizeof(modem_btn),
                 "<form method=\"POST\" action=\"/hw_status/modem_%s\" class=\"pin-form\">"
                 "<button type=\"submit\" class=\"pin-btn\">%s</button></form>",
                 modem_on ? "off" : "on", modem_on ? "SPEGNI" : "ACCENDI");
    }

    off += (size_t)snprintf(
        html + off, cap - off,
        "<div class=\"status-box\"><h4>%s</h4>"
        "<div class=\"status-line\"><span>D1</span><span>GPIO%d — %s</span></div>"
        "<div class=\"status-line\"><span>D2</span><span>GPIO%d — %s</span></div>"
        "<div class=\"status-line\"><span>D10</span><span>GPIO%d — %s</span></div>"
        "<div class=\"status-line\"><span>A1F</span><span>%s</span></div>"
        "<div class=\"status-line\"><span>MODE</span><span>GPIO%d — %s</span></div>"
        "<div class=\"status-line\"><span>HUB</span><span>GPIO%d — %s%s</span></div>"
        "<div class=\"status-line\"><span>MODEM</span><span>GPIO%d — %s%s</span></div>"
        "</div>",
        title,
        (int)CORE_GPIO_D1_WAKE, gpio_level_label(snap->d1_level),
        (int)CORE_GPIO_D2_END, gpio_level_label(snap->d2_level),
        (int)CORE_GPIO_TPL_DONE, gpio_level_label(snap->d10_level),
        a1f,
        (int)CORE_GPIO_MODE_CFG, mode,
        (int)CORE_GPIO_RTC_SQW, hub, hub_btn,
        (int)CORE_GPIO_MODEM_PWR, modem, modem_btn);
    return off;
}

static esp_err_t handle_hw_status_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_rtc_hw_status_monitoring_enabled()) {
        return send_redirect(req, "/settings");
    }

    core_gpio_boot_snapshot_t boot_snap = {};
    core_gpio_get_boot_snapshot(&boot_snap);

    core_gpio_boot_snapshot_t now_snap = {
        .d1_level = gpio_get_level(CORE_GPIO_D1_WAKE),
        .d2_level = gpio_get_level(CORE_GPIO_D2_END),
        .mode_level = gpio_get_level(CORE_GPIO_MODE_CFG),
        .sqw_level = gpio_get_level(CORE_GPIO_RTC_SQW),
        .d10_level = gpio_get_level(CORE_GPIO_TPL_DONE),
        .modem_level = gpio_get_level(CORE_GPIO_MODEM_PWR),
    };

    const bool rtc_present = core_rtc_is_present();
    /* BOOT = latch al probe (prima del clear); NOW = lettura live STATUS.0x0F */
    const bool boot_a1f = rtc_present && core_rtc_boot_was_alarm_wake();
    const bool now_a1f = rtc_present && core_rtc_alarm1_fired();

    char *html = (char *)heap_caps_malloc(20480, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        html = (char *)malloc(20480);
    }
    if (!html) {
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    size_t off = (size_t)snprintf(
        html, 20480,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Stato HW</title>"
        "<style>body{font-family:sans-serif;max-width:900px;margin:20px auto}"
        "nav a{margin-right:12px}table{border-collapse:collapse;width:100%%;margin:12px 0}"
        "th,td{border:1px solid #ccc;padding:8px;text-align:left;font-size:14px}"
        ".hint{font-size:13px;color:#555;line-height:1.45}"
        ".refresh{margin:12px 0}"
        ".status-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin:16px 0}"
        ".status-box{border:1px solid #ccc;border-radius:8px;padding:12px 14px;background:#fafafa}"
        ".status-box h4{margin:0 0 12px;font-size:15px;letter-spacing:0.04em}"
        ".status-line{display:flex;justify-content:space-between;gap:12px;padding:8px 0;"
        "border-bottom:1px solid #e6e6e6;font-size:14px;line-height:1.35}"
        ".status-line:last-child{border-bottom:none}"
        ".status-line span:first-child{font-weight:600;min-width:72px}"
        ".status-line span:last-child{text-align:right}"
        ".pin-form{display:inline;margin-left:10px}"
        ".pin-btn{background:#2266cc;color:#fff;border:none;cursor:pointer;padding:4px 12px;"
        "font-size:12px;font-weight:600;border-radius:5px}"
        ".pin-btn:hover{background:#1a52a3}"
        ".shutdown-box{border:1px solid #e0b4b4;border-radius:8px;padding:14px;margin:16px 0;"
        "background:#fff5f5}"
        ".mode-actions{display:flex;gap:12px;flex-wrap:wrap;margin-top:12px}"
        ".reboot-btn{background:#2266cc;color:#fff;border:none;cursor:pointer;padding:10px 18px;"
        "font-weight:600;border-radius:6px}"
        ".reboot-btn:hover{background:#1a52a3}"
        ".shutdown-btn{background:#c33;color:#fff;border:none;cursor:pointer;padding:10px 18px;"
        "font-weight:600;border-radius:6px}"
        ".shutdown-btn:hover{background:#a22}"
        "@media(max-width:720px){.status-grid{grid-template-columns:1fr}}</style></head><body>"
        "%s"
        "<h2>Stato hardware</h2>"
        "<p class=\"hint\">STATUS-BOOT: A1F latched al boot (prima del clear; MODE HIGH tiene la GUI). "
        "STATUS-NOW: lettura attuale del registro STATUS DS3231.</p>"
        "<p class=\"refresh\"><a href=\"/hw_status\">Aggiorna pagina</a></p>"
        "<div class=\"status-grid\">",
        web_nav());

    // BOOT: SQW LOW dell'RTC non è HUB ON. NOW: HUB ON solo dopo GPIO46 OUTPUT LOW.
    off = hw_status_append_pin_panel(html, 20480, off, "STATUS-BOOT", &boot_snap, rtc_present,
                                    boot_a1f, false, false);
    off = hw_status_append_pin_panel(html, 20480, off, "STATUS-NOW", &now_snap, rtc_present,
                                    now_a1f, core_gpio_hub_is_on(), true);
    off += (size_t)snprintf(
        html + off, 20480 - off,
        "</div>"
        "<div class=\"shutdown-box\">"
        "<h3>Attivare modalità</h3>"
        "<p class=\"hint\">D10 (GPIO%d) attuale: <strong>%s</strong>. "
        "Scegli REBOOT (sveglia RTC tra 1 min) o SHUTDOWN (spegnimento definitivo).</p>"
        "<div class=\"mode-actions\">"
        "<form method=\"POST\" action=\"/hw_status/reboot\">"
        "<button type=\"submit\" class=\"reboot-btn\">REBOOT</button></form>"
        "<form method=\"POST\" action=\"/hw_status/shutdown\">"
        "<button type=\"submit\" class=\"shutdown-btn\" "
        "onclick=\"return confirm('Si sta spegnendo la scheda definitivamente. "
        "Non sarà più raggiungibile sino a prossimo schedule RTC o accensione manuale "
        "in modalità programmazione. Vuoi continuare?');\">"
        "SHUTDOWN</button></form></div></div>",
        (int)CORE_GPIO_TPL_DONE,
        gpio_level_label(now_snap.d10_level));

    off = hw_status_append_a1f_log_html(html, 20480, off);
    off += (size_t)snprintf(html + off, 20480 - off, "</body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t res = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return res;
}

static esp_err_t handle_hw_status_reboot_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_rtc_hw_status_monitoring_enabled()) {
        return send_redirect(req, "/settings");
    }
    if (!core_alarm_schedule_schedule_reboot(60)) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(
            req,
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>REBOOT</title></head><body>"
            "<h2>REBOOT non riuscito</h2>"
            "<p>Impossibile programmare la sveglia RTC (ora non valida, EEPROM piena o DS3231).</p>"
            "<p><a href=\"/hw_status\">Torna a Stato HW</a></p></body></html>",
            HTTPD_RESP_USE_STRLEN);
    }
    core_gpio_request_shutdown();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(
        req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>REBOOT</title></head><body>"
        "<h2>REBOOT in corso…</h2>"
        "<p>Allarme RTC tra 1 minuto programmato. D10 (DONE) → HIGH — spegnimento TPL.</p>"
        "</body></html>",
        HTTPD_RESP_USE_STRLEN);
}

// Comandi manuali dei pin dalla pagina Stato HW. Tornano sempre a /hw_status così
// la pagina si ridisegna con il livello aggiornato.
static esp_err_t hw_status_pin_cmd(httpd_req_t *req, bool modem, bool on) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_rtc_hw_status_monitoring_enabled()) {
        return send_redirect(req, "/settings");
    }
    if (modem) {
        core_gpio_modem_pwr_set(on);  // GPIO5: HIGH = modem acceso
    } else if (on) {
        core_gpio_sqw_drive_low();    // GPIO46: LOW = HUB acceso
    } else {
        core_gpio_sqw_release_high();
    }
    return send_redirect(req, "/hw_status");
}

static esp_err_t handle_hw_status_modem_on_post(httpd_req_t *req) {
    return hw_status_pin_cmd(req, true, true);
}

static esp_err_t handle_hw_status_modem_off_post(httpd_req_t *req) {
    return hw_status_pin_cmd(req, true, false);
}

static esp_err_t handle_hw_status_hub_on_post(httpd_req_t *req) {
    return hw_status_pin_cmd(req, false, true);
}

static esp_err_t handle_hw_status_hub_off_post(httpd_req_t *req) {
    return hw_status_pin_cmd(req, false, false);
}

static esp_err_t handle_hw_status_shutdown_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_rtc_hw_status_monitoring_enabled()) {
        return send_redirect(req, "/settings");
    }
    core_gpio_request_shutdown();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(
        req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>Spegnimento</title></head><body>"
        "<h2>Spegnimento in corso…</h2>"
        "<p>D10 (DONE) → HIGH — il dispositivo si spegnerà tra pochi secondi.</p>"
        "</body></html>",
        HTTPD_RESP_USE_STRLEN);
}

static void schedule_format_entry(const core_alarm_entry_t *e, char *buf, size_t len) {
    if (!e || !buf || len == 0) {
        return;
    }
    if (e->flags & CORE_ALARM_FLAG_REPEAT_DAILY) {
        snprintf(buf, len, "Ogni giorno %02u:%02u:%02u", (unsigned)e->hour, (unsigned)e->min,
                 (unsigned)e->sec);
    } else {
        snprintf(buf, len, "%02u/%02u/%04u %02u:%02u:%02u", (unsigned)e->day, (unsigned)e->month,
                 (unsigned)e->year, (unsigned)e->hour, (unsigned)e->min, (unsigned)e->sec);
    }
}

static esp_err_t handle_schedule_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_alarm_schedule_hw_available()) {
        return send_redirect(req, "/settings");
    }

    /* Pulisce one-shot scaduti e aggiorna prossimo su DS3231. */
    (void)core_alarm_schedule_program_rtc();

    const core_alarm_schedule_t *sched = core_alarm_schedule_get();
    char when_rtc[64];
    core_rtc_format_local(when_rtc, sizeof(when_rtc));  // lettura immediata dal chip
    char clocks[420];
    web_clocks_html(clocks, sizeof(clocks), false);

    char next_hw[48] = "—";
    if (sched->master_enabled && core_time_is_valid()) {
        core_alarm_entry_t next = {};
        if (core_alarm_schedule_find_next(time(nullptr), &next, nullptr)) {
            schedule_format_entry(&next, next_hw, sizeof(next_hw));
        } else {
            snprintf(next_hw, sizeof(next_hw), "Nessuno");
        }
    }

    char flash[220] = {0};
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (strstr(q, "time=ok")) {
            snprintf(flash, sizeof(flash),
                     "<div class='alert ok'><strong>OK</strong> — orario scritto sul DS3231: %s</div>",
                     when_rtc);
        } else if (strstr(q, "time=ko")) {
            snprintf(flash, sizeof(flash),
                     "<div class='alert err'><strong>KO</strong> — aggiornamento orario fallito "
                     "(WiFi/SNTP o scrittura DS3231).</div>");
        } else if (strstr(q, "ok=1")) {
            snprintf(flash, sizeof(flash), "<div class='alert ok'>Salvato.</div>");
        } else if (strstr(q, "err=add")) {
            snprintf(flash, sizeof(flash),
                     "<div class='alert err'>Allarme non aggiunto — verificare data/ora o lista piena.</div>");
        } else if (strstr(q, "err=body")) {
            snprintf(flash, sizeof(flash), "<div class='alert err'>Richiesta non valida.</div>");
        }
    }

    char *html = (char *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html) {
        html = (char *)malloc(32768);
    }
    if (!html) {
        return send_plain_err(req, "503 Service Unavailable", "Memoria insufficiente");
    }

    struct tm now_tm;
    int def_y = 2026, def_m = 1, def_d = 1;
    if (core_time_get_local_tm(&now_tm)) {
        def_y = now_tm.tm_year + 1900;
        def_m = now_tm.tm_mon + 1;
        def_d = now_tm.tm_mday;
    }

    size_t off = (size_t)snprintf(
        html, 32768,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>RTC</title>"
        "<style>body{font-family:sans-serif;max-width:900px;margin:20px auto}"
        "nav a{margin-right:12px}table{border-collapse:collapse;width:100%%;margin:12px 0}"
        "th,td{border:1px solid #ccc;padding:8px;text-align:left;font-size:14px}"
        "input,button,select{padding:8px;margin:4px 0}label{display:block;margin-top:6px;font-weight:600}"
        ".hint{font-size:13px;color:#555;line-height:1.45}.alert{padding:12px;margin:12px 0;border-radius:6px}"
        ".alert.ok{background:#e8f7ea;border:1px solid #8fd19a}"
        ".alert.err{background:#fdecea;border:1px solid #f5a8a0}"
        ".grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}"
        ".del{background:#c33;color:#fff;border:none;cursor:pointer;padding:6px 10px}"
        ".sync-btn{background:#2266cc;color:#fff;border:none;cursor:pointer;padding:10px 16px;font-weight:600}"
        ".sync-bar{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin:12px 0}"
        ".sd-info{padding:10px 12px;margin:12px 0;border-radius:6px;background:#eef3ff;border:1px solid #c5d4f5}"
        ".sd-info.warn{background:#fff8e6;border-color:#f0d080}"
        "</style></head><body>"
        "%s"
        "<h2>RTC (DS3231)</h2>"
        "%s"
        "%s"
        "<div class=\"sync-bar\">"
        "<form method=\"POST\" action=\"/schedule\" style=\"margin:0\">"
        "<input type=\"hidden\" name=\"action\" value=\"sync_time\">"
        "<button type=\"submit\" class=\"sync-btn\">Aggiorna orario ora</button></form>"
        "<span class=\"hint\">Sincronizza da Internet (SNTP) e scrive l'ora sul DS3231.</span>"
        "</div>",
        web_nav(), flash, clocks);

    off += (size_t)snprintf(
        html + off, 32768 - off,
        "<h3>Allarmi sveglia → TPL</h3>"
        "<p class=\"hint\">Lista su <strong>AT24C32</strong>. Con scheduling attivo il prossimo allarme "
        "viene scritto sul DS3231 subito (e di nuovo allo spegnimento). "
        "Gli allarmi a <strong>data fissa</strong> (non ricorrenti) vengono <strong>rimossi dalla lista</strong> "
        "dopo lo scatto; i giornalieri restano. "
        "<strong>SQW va a GND solo allo scatto</strong> (non mentre è in attesa). "
        "Prossimo sul chip: <strong>%s</strong>.</p>"
        "%s"
        "<form method=\"POST\" action=\"/schedule\" style=\"margin:16px 0\">"
        "<input type=\"hidden\" name=\"action\" value=\"master\">"
        "<label><input type=\"checkbox\" name=\"master_en\" value=\"1\" %s> "
        "<strong>Abilita scheduling</strong> — senza questo SQW resta spento</label>"
        "<button type=\"submit\">Salva</button></form>"
        "<h3>Allarmi configurati (%u)</h3>"
        "<table><tr><th>#</th><th>Quando</th><th>Tipo</th><th>Attivo</th><th></th></tr>",
        next_hw,
        (!sched->master_enabled && sched->count > 0)
            ? "<div class='alert err'><strong>Scheduling spento</strong> — gli allarmi in lista "
              "non vengono programmati sul DS3231. Spunta la casella sotto e salva.</div>"
            : "",
        sched->master_enabled ? "checked" : "",
        (unsigned)sched->count);

    for (unsigned i = 0; i < sched->count && off < 26000; ++i) {
        const core_alarm_entry_t *e = &sched->entries[i];
        char label[48];
        schedule_format_entry(e, label, sizeof(label));
        off += (size_t)snprintf(
            html + off, 32768 - off,
            "<tr><td>%u</td><td>%s</td><td>%s</td><td>%s</td>"
            "<td><form method=\"POST\" action=\"/schedule\" style=\"margin:0\">"
            "<input type=\"hidden\" name=\"action\" value=\"delete\">"
            "<input type=\"hidden\" name=\"idx\" value=\"%u\">"
            "<button type=\"submit\" class=\"del\" onclick=\"return confirm('Eliminare allarme %u?')\">Elimina</button>"
            "</form></td></tr>",
            i + 1, label,
            (e->flags & CORE_ALARM_FLAG_REPEAT_DAILY) ? "Giornaliero" : "Data fissa",
            (e->flags & CORE_ALARM_FLAG_ENABLED) ? "Si" : "No", i, i + 1);
    }

    off += (size_t)snprintf(
        html + off, 32768 - off,
        "</table>"
        "<h3>Aggiungi allarme</h3>"
        "<form method=\"POST\" action=\"/schedule\">"
        "<input type=\"hidden\" name=\"action\" value=\"add\">"
        "<label><input type=\"checkbox\" name=\"en\" value=\"1\" checked> Attivo</label>"
        "<label><input type=\"checkbox\" name=\"daily\" value=\"1\" id=\"dailyCb\"> "
        "Ripeti ogni giorno (ignora data sotto)</label>"
        "<div class=\"grid\">"
        "<div><label>Giorno</label><input name=\"day\" type=\"number\" min=\"1\" max=\"31\" value=\"%d\"></div>"
        "<div><label>Mese</label><input name=\"month\" type=\"number\" min=\"1\" max=\"12\" value=\"%d\"></div>"
        "<div><label>Anno</label><input name=\"year\" type=\"number\" min=\"2024\" max=\"2099\" value=\"%d\"></div>"
        "<div><label>Ora</label><input name=\"hour\" type=\"number\" min=\"0\" max=\"23\" value=\"8\"></div>"
        "<div><label>Minuti</label><input name=\"min\" type=\"number\" min=\"0\" max=\"59\" value=\"0\"></div>"
        "<div><label>Secondi</label><input name=\"sec\" type=\"number\" min=\"0\" max=\"59\" value=\"0\"></div>"
        "</div>"
        "<button type=\"submit\">Aggiungi</button></form>"
        "<form method=\"POST\" action=\"/schedule\" style=\"margin-top:20px\">"
        "<input type=\"hidden\" name=\"action\" value=\"program\">"
        "<button type=\"submit\">Riprogramma adesso il prossimo allarme su DS3231</button>"
        "</form>"
        "<script>document.getElementById('dailyCb').onchange=function(){"
        "document.querySelectorAll('[name=day],[name=month],[name=year]').forEach(el=>{"
        "el.disabled=this.checked; el.style.opacity=this.checked?'0.45':'1';});};</script>"
        "</body></html>",
        def_d, def_m, def_y);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t res = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return res;
}

static esp_err_t handle_schedule_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!core_alarm_schedule_hw_available()) {
        return send_redirect(req, "/settings");
    }

    char buf[512];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/schedule?err=body");
    }

    char action[24];
    if (!form_get_value(buf, "action", action, sizeof(action))) {
        return send_redirect(req, "/schedule?err=body");
    }

    if (strcmp(action, "master") == 0) {
        bool en = form_get_value(buf, "master_en", action, sizeof(action));
        if (!core_alarm_schedule_set_master(en)) {
            return send_redirect(req, "/schedule?err=body");
        }
        if (en) {
            (void)core_alarm_schedule_program_rtc();
        } else {
            core_rtc_disable_alarms();
        }
        return send_redirect(req, "/schedule?ok=1");
    }

    if (strcmp(action, "delete") == 0) {
        char idx_s[8];
        if (!form_get_value(buf, "idx", idx_s, sizeof(idx_s))) {
            return send_redirect(req, "/schedule?err=body");
        }
        unsigned idx = (unsigned)atoi(idx_s);
        if (!core_alarm_schedule_delete(idx)) {
            return send_redirect(req, "/schedule?err=body");
        }
        (void)core_alarm_schedule_program_rtc();
        return send_redirect(req, "/schedule?ok=1");
    }

    if (strcmp(action, "program") == 0) {
        (void)core_alarm_schedule_program_rtc();
        return send_redirect(req, "/schedule?ok=1");
    }

    if (strcmp(action, "sync_time") == 0) {
        bool sntp_ok = false;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip = {};
            if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
                sntp_ok = core_time_sync_sntp(10000);
            }
        }
        if (!sntp_ok && !core_time_is_valid()) {
            ESP_LOGW(TAG, "Sync orario manuale KO: SNTP fallito e ora di sistema non valida");
            return send_redirect(req, "/schedule?time=ko");
        }
        if (!core_rtc_is_present() || !core_rtc_sync_from_system()) {
            ESP_LOGW(TAG, "Sync orario manuale KO: scrittura DS3231 fallita");
            return send_redirect(req, "/schedule?time=ko");
        }
        (void)core_rtc_sync_system_time();
        char when_log[40];
        core_time_format_local(when_log, sizeof(when_log));
        ESP_LOGI(TAG, "Sync orario manuale OK → DS3231: %s (sntp=%d)", when_log, (int)sntp_ok);
        return send_redirect(req, "/schedule?time=ok");
    }

    if (strcmp(action, "add") == 0) {
        core_alarm_entry_t e = {};
        e.flags = 0;
        if (form_get_value(buf, "en", action, sizeof(action))) {
            e.flags |= CORE_ALARM_FLAG_ENABLED;
        }
        if (form_get_value(buf, "daily", action, sizeof(action))) {
            e.flags |= CORE_ALARM_FLAG_REPEAT_DAILY;
        }

        e.day = (uint8_t)parse_form_int(buf, "day", 1);
        e.month = (uint8_t)parse_form_int(buf, "month", 1);
        e.year = (uint16_t)parse_form_int(buf, "year", 2026);
        e.hour = (uint8_t)parse_form_int(buf, "hour", 0);
        e.min = (uint8_t)parse_form_int(buf, "min", 0);
        e.sec = (uint8_t)parse_form_int(buf, "sec", 0);

        if (!core_alarm_entry_valid(&e) || !core_alarm_schedule_add(&e)) {
            return send_redirect(req, "/schedule?err=add");
        }
        // Aggiungere un allarme attivo arma anche lo scheduling (altrimenti SQW resta spento).
        if ((e.flags & CORE_ALARM_FLAG_ENABLED) && !core_alarm_schedule_get()->master_enabled) {
            (void)core_alarm_schedule_set_master(true);
            ESP_LOGI(TAG, "Scheduling abilitato automaticamente (nuovo allarme attivo)");
        }
        (void)core_alarm_schedule_program_rtc();
        return send_redirect(req, "/schedule?ok=1");
    }

    return send_redirect(req, "/schedule?err=body");
}

static const char *mrbin_enroll_label(const core_mrbin_id_t *id) {
    if (!id) {
        return "sconosciuto";
    }
    if (id->content_key_valid) {
        return "issued";
    }
    if (id->enroll_pending) {
        return "pending";
    }
    return "non enrolled";
}

static esp_err_t handle_mrbin_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    const core_mrbin_id_t *id = core_mrbin_id_get();
    core_clip_queue_counts_t qc = {};
    core_clip_queue_counts(&qc);

    char flash[220] = {0};
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (strstr(q, "saved=1")) {
            snprintf(flash, sizeof(flash), "<div class='alert ok'>Impostazioni MrBin salvate.</div>");
        } else if (strstr(q, "saved=0")) {
            snprintf(flash, sizeof(flash), "<div class='alert err'>Salvataggio fallito.</div>");
        } else if (strstr(q, "retry=1")) {
            snprintf(flash, sizeof(flash),
                     "<div class='alert ok'>Errori riportati in coda — ritento allo slot rete.</div>");
        }
    }

    char html[4096];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>MrBin</title>"
        "<style>body{font-family:sans-serif;max-width:820px;margin:20px auto}"
        "nav a{margin-right:12px}label{display:block;margin-top:12px}"
        "input{width:100%%;padding:8px;box-sizing:border-box}"
        ".hint{color:#555;font-size:13px}code{background:#eee;padding:1px 4px}</style></head><body>"
        "%s"
        "<h2>MrBin server</h2>%s"
        "<p class='hint'>Enrollment e upload partono al wake RTC (CONFIG + WiFi STA). "
        "La chiave content e la X25519 privata non sono mostrate.</p>"
        "<p><strong>device_uid</strong>: <code>%s</code><br>"
        "<strong>enroll</strong>: %s%s<br>"
        "<strong>coda</strong>: pending %d · upload %d · verificati %d · errori %d · plaintext %d</p>"
        "<form method='POST' action='/mrbin'>"
        "<label>API base URL</label><input name='api_base' value='%s'>"
        "<label>device_id</label><input name='device_id' value='%s'>"
        "<label>hardware_id</label><input name='hardware_id' value='%s'>"
        "<label>camera_key</label><input name='camera_key' value='%s'>"
        "<label>camera_label</label><input name='camera_label' value='%s'>"
        "<p class='hint'>Non aggiungere <code>/api/v1</code> due volte. Default: "
        "<code>https://mrbin.planetel.dev/api/v1</code></p>"
        "<button type='submit' name='act' value='save'>Salva</button></form>"
        "<form method='POST' action='/mrbin' style='margin-top:16px'>"
        "<button type='submit' name='act' value='retry'>Riprova upload (stessi ID)</button>"
        "</form></body></html>",
        web_nav(), flash,
        (id && id->uid_valid) ? id->device_uid : "(ancora assente — serve STA)",
        mrbin_enroll_label(id),
        (id && id->key_id[0]) ? " · key_id presente" : "",
        qc.pending, qc.uploading, qc.verified, qc.error, qc.plaintext,
        id ? id->api_base_url : CORE_MRBIN_API_BASE_DEFAULT,
        id ? id->device_id : "",
        id ? id->hardware_id : "",
        id ? id->camera_key : "",
        id ? id->camera_label : "");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_mrbin_post(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char buf[1024];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/mrbin?saved=0");
    }
    char act[16] = {0};
    form_get_value(buf, "act", act, sizeof(act));
    if (strcmp(act, "retry") == 0) {
        (void)core_clip_queue_clear_errors();
        return send_redirect(req, "/mrbin?retry=1");
    }
    char api[96] = {0}, did[32] = {0}, hw[32] = {0}, ck[16] = {0}, lab[48] = {0};
    form_get_value(buf, "api_base", api, sizeof(api));
    form_get_value(buf, "device_id", did, sizeof(did));
    form_get_value(buf, "hardware_id", hw, sizeof(hw));
    form_get_value(buf, "camera_key", ck, sizeof(ck));
    form_get_value(buf, "camera_label", lab, sizeof(lab));
    bool ok = core_mrbin_id_set_public_fields(api, hw, did, ck, lab);
    return send_redirect(req, ok ? "/mrbin?saved=1" : "/mrbin?saved=0");
}

bool core_web_sta_has_ip(void) {
    return s_sta_has_ip;
}

static esp_err_t handle_wifi_get(httpd_req_t *req);
static esp_err_t handle_wifi_post(httpd_req_t *req);

static const char *CHANNELS_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Canali Video</title>
<style>
body{font-family:sans-serif;max-width:720px;margin:24px auto}
input,button,select{padding:8px;margin:4px 0;width:100%%;box-sizing:border-box}
nav a{margin-right:12px}
fieldset{border:1px solid #ccc;border-radius:8px;padding:12px 16px;margin:16px 0}
.hint{font-size:14px;color:#555;line-height:1.45}
.alert{padding:12px;margin:12px 0;border-radius:6px}
.ok{background:#e8f7ea;border:1px solid #8fd19a}
.err{background:#fdecea;border:1px solid #f5a8a0}
.row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
</style></head><body>
%s
<h2>Canali Video</h2>
%s
<form method="POST" action="/rtsp/switch">
<input type="hidden" name="on" value="%s">
<button type="submit">%s</button>
<p class="hint">Un solo pulsante accende o spegne sempre entrambi i canali. Spento di default: un client collegato non avvia lo stream.</p>
</form>
<fieldset>
<legend>Canale 1 — registrazione e RTSP</legend>
<p class="hint">È la stessa risoluzione di Reg. video. Se la cambi, Reg. video viene sovrascritta con gli stessi numeri.</p>
<form method="POST" action="/channels" onsubmit="return ch1Save()">
<input type="hidden" name="channel" value="1">
<label>Preset</label>
<select name="preset" id="p1" onchange="usePreset('p1','w1','h1','f1')">
<option value="">— personalizzato —</option>
<option value="3840,2160,15">3840×2160 @ 15 fps</option>
<option value="3072,1728,15">3072×1728 @ 15 fps</option>
<option value="2560,1440,30">2560×1440 @ 30 fps</option>
<option value="2304,1296,30">2304×1296 @ 30 fps</option>
<option value="1920,1080,30">1920×1080 @ 30 fps</option>
<option value="1920,1080,15">1920×1080 @ 15 fps</option>
<option value="1280,720,30">1280×720 @ 30 fps</option>
<option value="1280,720,15">1280×720 @ 15 fps</option>
<option value="640,480,15">640×480 @ 15 fps</option>
</select>
<div class="row">
<label>Larghezza<input name="w" id="w1" type="number" min="640" max="3840" step="2" value="%u" required></label>
<label>Altezza<input name="h" id="h1" type="number" min="480" max="2160" step="2" value="%u" required></label>
<label>fps<input name="fps" id="f1" type="number" min="1" max="30" value="%u" required></label>
</div>
<button type="submit">Salva canale 1</button>
</form>
<p class="hint">%s</p>
</fieldset>
<fieldset>
<legend>Canale 2 — movimento e RTSP</legend>
<p class="hint">Non registra su SD. Serve al rilevamento movimento, se attivo, e allo stream RTSP. Sopra 1920×1080 il movimento usa 1280×720.</p>
<form method="POST" action="/channels">
<input type="hidden" name="channel" value="2">
<label>Preset</label>
<select name="preset" id="p2" onchange="usePreset('p2','w2','h2','f2')">
<option value="">— personalizzato —</option>
<option value="1920,1080,15">1920×1080 @ 15 fps</option>
<option value="1280,720,15">1280×720 @ 15 fps</option>
<option value="1280,720,10">1280×720 @ 10 fps</option>
<option value="640,480,15">640×480 @ 15 fps</option>
<option value="640,360,10">640×360 @ 10 fps</option>
</select>
<div class="row">
<label>Larghezza<input name="w" id="w2" type="number" min="640" max="3840" step="2" value="%u" required></label>
<label>Altezza<input name="h" id="h2" type="number" min="360" max="2160" step="2" value="%u" required></label>
<label>fps<input name="fps" id="f2" type="number" min="1" max="30" value="%u" required></label>
</div>
<button type="submit">Salva canale 2</button>
</form>
<p class="hint">%s</p>
</fieldset>
<script>
var IW=%u,IH=%u,IF=%u;
function usePreset(sel,w,h,f){
  var v=document.getElementById(sel).value;
  if(!v)return;
  var p=v.split(',');
  document.getElementById(w).value=p[0];
  document.getElementById(h).value=p[1];
  document.getElementById(f).value=p[2];
}
function ch1Save(){
  usePreset('p1','w1','h1','f1');
  var w=parseInt(document.getElementById('w1').value,10)||0;
  var h=parseInt(document.getElementById('h1').value,10)||0;
  var f=parseInt(document.getElementById('f1').value,10)||0;
  if(w!==IW||h!==IH||f!==IF){
    alert('Canale 1 è legato a Reg. video. Questi parametri sovrascrivono Reg. video con gli stessi valori.');
  }
  return true;
}
</script>
</body></html>
)HTML";

static bool channels_read_size(const char *body, uint16_t *w, uint16_t *h, uint8_t *fps) {
    char w_s[16] = {0};
    char h_s[16] = {0};
    char fps_s[8] = {0};
    char preset[40] = {0};
    form_get_value(body, "w", w_s, sizeof(w_s));
    form_get_value(body, "h", h_s, sizeof(h_s));
    form_get_value(body, "fps", fps_s, sizeof(fps_s));
    form_get_value(body, "preset", preset, sizeof(preset));
    unsigned pw = (unsigned)atoi(w_s);
    unsigned ph = (unsigned)atoi(h_s);
    unsigned pf = (unsigned)atoi(fps_s);
    if (preset[0]) {
        unsigned a = 0, b = 0, c = 0;
        if (sscanf(preset, "%u,%u,%u", &a, &b, &c) == 3) {
            pw = a;
            ph = b;
            pf = c;
        }
    }
    const uint8_t cap = core_video_fps_ceiling((uint16_t)pw, (uint16_t)ph);
    if (pf > cap) {
        pf = cap;
    }
    if (!core_settings_video_valid((uint16_t)pw, (uint16_t)ph, (uint8_t)pf)) {
        return false;
    }
    *w = (uint16_t)pw;
    *h = (uint16_t)ph;
    *fps = (uint8_t)pf;
    return true;
}

static esp_err_t handle_channels_get(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    if (!s_settings) {
        return send_redirect(req, "/");
    }
    char query[32] = {0};
    char flash[160] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char msg[8] = {0};
        if (httpd_query_key_value(query, "saved", msg, sizeof(msg)) == ESP_OK) {
            if (strcmp(msg, "1") == 0) {
                snprintf(flash, sizeof(flash), "<div class='alert ok'>Salvato.</div>");
            } else if (strcmp(msg, "0") == 0) {
                snprintf(flash, sizeof(flash), "<div class='alert err'>Salvataggio rifiutato.</div>");
            }
        }
    }
    char url1[80] = {0};
    char url2[80] = {0};
    char link1[160];
    char link2[160];
    core_rtsp_url_ch(1, url1, sizeof(url1));
    core_rtsp_url_ch(2, url2, sizeof(url2));
    if (s_settings->rtsp_always) {
        snprintf(link1, sizeof(link1), "RTSP attivo: <code>%s</code> (anche /live)", url1);
        snprintf(link2, sizeof(link2), "RTSP attivo: <code>%s</code>", url2);
    } else {
        snprintf(link1, sizeof(link1), "RTSP spento");
        snprintf(link2, sizeof(link2), "RTSP spento");
    }
    const size_t cap = 12288;
    char *html = (char *)malloc(cap);
    if (!html) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    int n = snprintf(html, cap, CHANNELS_HTML,
                     web_nav(), flash,
                     s_settings->rtsp_always ? "0" : "1",
                     s_settings->rtsp_always ? "Disattiva RTSP (entrambi i canali)"
                                            : "Attiva RTSP (entrambi i canali)",
                     (unsigned)s_settings->video_width, (unsigned)s_settings->video_height,
                     (unsigned)s_settings->video_fps, link1,
                     (unsigned)s_settings->ch2_width, (unsigned)s_settings->ch2_height,
                     (unsigned)s_settings->ch2_fps, link2,
                     (unsigned)s_settings->video_width, (unsigned)s_settings->video_height,
                     (unsigned)s_settings->video_fps);
    if (n < 0 || n >= (int)cap) {
        free(html);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pagina troncata");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t handle_channels_post(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) {
        return send_redirect(req, "/");
    }
    char buf[512];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/channels?saved=0");
    }
    char which[8] = {0};
    form_get_value(buf, "channel", which, sizeof(which));
    uint16_t w = 0;
    uint16_t h = 0;
    uint8_t fps = 0;
    if (!channels_read_size(buf, &w, &h, &fps)) {
        return send_redirect(req, "/channels?saved=0");
    }
    if (which[0] == '2') {
        s_settings->ch2_width = w;
        s_settings->ch2_height = h;
        s_settings->ch2_fps = fps;
    } else {
        s_settings->video_width = w;
        s_settings->video_height = h;
        s_settings->video_fps = fps;
        core_settings_video_normalize(s_settings);
    }
    const bool saved = core_settings_save(s_settings);
    if (saved) {
        core_rtsp_apply(s_settings);
    }
    ESP_LOGI(TAG, "Canale %s %ux%u@%u → %s", which[0] == '2' ? "2" : "1",
             (unsigned)w, (unsigned)h, (unsigned)fps, saved ? "OK" : "FAIL");
    return send_redirect(req, saved ? "/channels?saved=1" : "/channels?saved=0");
}

static esp_err_t handle_rtsp_switch(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) {
        return send_redirect(req, "/");
    }
    char buf[64];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) {
        return send_redirect(req, "/channels?saved=0");
    }
    char on_s[4] = {0};
    form_get_value(buf, "on", on_s, sizeof(on_s));
    const bool on = on_s[0] == '1';
    s_settings->rtsp_always = on;
    const bool saved = core_settings_save(s_settings);
    if (!saved) {
        return send_redirect(req, "/channels?saved=0");
    }
    if (core_demo_is_active()) {
        if (!core_demo_set_rtsp_view(on, s_settings)) {
            return send_redirect(req, "/channels?saved=0");
        }
    } else {
        core_rtsp_apply(s_settings);
    }
    ESP_LOGI(TAG, "RTSP %s (entrambi i canali)", on ? "acceso" : "spento");
    return send_redirect(req, "/channels?saved=1");
}

static bool start_http_server(void) {
    if (s_server) return true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CORE_WEB_PORT;
    config.stack_size = 32768;
    config.max_uri_handlers = 72;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    // Timeout corti: con WiFi ballerino i socket morti non devono restare 60 s.
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 600;  // download video grandi (default 5–8 s interrompe lo stream)

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start fallito");
        return false;
    }

    httpd_uri_t uris[] = {
        {"/", HTTP_GET, handle_root, nullptr},
        {"/login", HTTP_POST, handle_login, nullptr},
        {"/logout", HTTP_GET, handle_logout, nullptr},
        {"/settings", HTTP_GET, handle_settings_get, nullptr},
        {"/settings", HTTP_POST, handle_settings_post, nullptr},
        {"/wifi", HTTP_GET, handle_wifi_get, nullptr},
        {"/wifi", HTTP_POST, handle_wifi_post, nullptr},
        {"/rec_video", HTTP_GET, handle_rec_video_get, nullptr},
        {"/rec_video", HTTP_POST, handle_rec_video_post, nullptr},
        {"/channels", HTTP_GET, handle_channels_get, nullptr},
        {"/channels", HTTP_POST, handle_channels_post, nullptr},
        {"/rtsp/switch", HTTP_POST, handle_rtsp_switch, nullptr},
        {"/format_sd", HTTP_POST, handle_format_sd, nullptr},
        {"/sd/remount", HTTP_POST, handle_sd_remount, nullptr},
        {"/demo/rtsp", HTTP_POST, handle_demo_rtsp, nullptr},
        {"/live", HTTP_GET, handle_live_get, nullptr},
        {"/live/stream", HTTP_GET, handle_live_stream, nullptr},
        {"/live/flip", HTTP_POST, handle_live_flip, nullptr},
        {"/live/image-reset", HTTP_POST, handle_live_image_reset, nullptr},
        {"/live/focus", HTTP_POST, handle_live_focus, nullptr},
        {"/live/focus/save", HTTP_POST, handle_live_focus_save, nullptr},
        {"/live/record/start", HTTP_POST, handle_live_record_start, nullptr},
        {"/live/record/stop", HTTP_POST, handle_live_record_stop, nullptr},
        {"/live/record/status", HTTP_GET, handle_live_record_status, nullptr},
        {"/videos", HTTP_GET, handle_videos_get, nullptr},
        {"/videos/watch", HTTP_GET, handle_videos_watch, nullptr},
        {"/videos/delete", HTTP_POST, handle_videos_delete, nullptr},
        {"/videos/delete_all", HTTP_POST, handle_videos_delete_all, nullptr},
        {"/videos/download", HTTP_GET, handle_videos_download, nullptr},
        {"/videos/download/start", HTTP_GET, handle_videos_download_start, nullptr},
        {"/videos/download/status", HTTP_GET, handle_videos_download_status, nullptr},
        {"/videos/download/file", HTTP_GET, handle_videos_download_file, nullptr},
        {"/photos", HTTP_GET, handle_photos_get, nullptr},
        {"/photos/watch", HTTP_GET, handle_photos_watch, nullptr},
        {"/photos/frames", HTTP_GET, handle_photos_frames_json, nullptr},
        {"/photos/image", HTTP_GET, handle_photos_image, nullptr},
        {"/photos/download", HTTP_GET, handle_photos_download, nullptr},
        {"/photos/export", HTTP_GET, handle_photos_export_page, nullptr},
        {"/photos/export/start", HTTP_GET, handle_photos_export_start, nullptr},
        {"/photos/export/status", HTTP_GET, handle_photos_export_status, nullptr},
        {"/photos/export/file", HTTP_GET, handle_photos_export_file, nullptr},
        {"/photos/delete", HTTP_POST, handle_photos_delete, nullptr},
        {"/photos/delete_all", HTTP_POST, handle_photos_delete_all, nullptr},
        {"/calibrate", HTTP_GET, handle_calibrate_get, nullptr},
        {"/calibrate", HTTP_POST, handle_calibrate_post, nullptr},
        {"/calibrate/bayer", HTTP_POST, handle_calibrate_bayer_post, nullptr},
        {"/calibrate/preview", HTTP_POST, handle_calibrate_preview_post, nullptr},
        {"/calibrate/wb", HTTP_GET, handle_calibrate_wb_get, nullptr},
        {"/schedule", HTTP_GET, handle_schedule_get, nullptr},
        {"/schedule", HTTP_POST, handle_schedule_post, nullptr},
        {"/mrbin", HTTP_GET, handle_mrbin_get, nullptr},
        {"/mrbin", HTTP_POST, handle_mrbin_post, nullptr},
        {"/hw_status", HTTP_GET, handle_hw_status_get, nullptr},
        {"/hw_status/reboot", HTTP_POST, handle_hw_status_reboot_post, nullptr},
        {"/hw_status/shutdown", HTTP_POST, handle_hw_status_shutdown_post, nullptr},
        {"/hw_status/modem_on", HTTP_POST, handle_hw_status_modem_on_post, nullptr},
        {"/hw_status/modem_off", HTTP_POST, handle_hw_status_modem_off_post, nullptr},
        {"/hw_status/hub_on", HTTP_POST, handle_hw_status_hub_on_post, nullptr},
        {"/hw_status/hub_off", HTTP_POST, handle_hw_status_hub_off_post, nullptr},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        esp_err_t reg = httpd_register_uri_handler(s_server, &uris[i]);
        if (reg != ESP_OK) {
            ESP_LOGE(TAG, "Registrazione handler fallita: %s (%s)",
                     uris[i].uri, esp_err_to_name(reg));
        }
    }
    ESP_LOGI(TAG, "Web GUI attiva su porta %d", CORE_WEB_PORT);
    return true;
}

static void wifi_html_escape(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    if (!out || out_len == 0) return;
    if (!in) in = "";
    for (size_t i = 0; in[i] && o + 6 < out_len; ++i) {
        const char c = in[i];
        const char *rep = nullptr;
        if (c == '&') rep = "&amp;";
        else if (c == '<') rep = "&lt;";
        else if (c == '>') rep = "&gt;";
        else if (c == '"') rep = "&quot;";
        if (rep) {
            size_t n = strlen(rep);
            memcpy(out + o, rep, n);
            o += n;
        } else if (c == '\n' || c == '\r') {
            continue;
        } else {
            out[o++] = c;
        }
    }
    out[o] = 0;
}

static esp_err_t handle_wifi_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    if (!hal_wifi_allowed()) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(req, "<html><body><p>WiFi spento in questa modalità.</p></body></html>");
    }
    char query[32] = {0};
    bool do_scan = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        strstr(query, "scan=1")) {
        do_scan = true;
    }
    char raw[2048] = {0};
    if (do_scan) {
        if (hal_wifi_scan(raw, sizeof(raw)) < 0) {
            snprintf(raw, sizeof(raw), "(scan rifiutato)");
        } else if (raw[0] == 0) {
            snprintf(raw, sizeof(raw), "(nessuna rete)");
        }
    }
    char list[3072] = {0};
    size_t used = 0;
    const char *p = raw;
    while (*p && used + 80 < sizeof(list)) {
        char ssid[64] = {0};
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n + 1 < sizeof(ssid)) {
            ssid[n] = p[n];
            n++;
        }
        ssid[n] = 0;
        if (p[n] == '\n') n++;
        p += n;
        if (ssid[0] == 0) continue;
        char esc[96];
        wifi_html_escape(ssid, esc, sizeof(esc));
        int w = snprintf(list + used, sizeof(list) - used,
                         "<label><input type='radio' name='wifi_ssid' value='%s'> %s</label>",
                         esc, esc);
        if (w < 0 || (size_t)w >= sizeof(list) - used) break;
        used += (size_t)w;
    }
    char cur[96] = {0};
    wifi_html_escape(s_settings ? s_settings->wifi_ssid : "", cur, sizeof(cur));
    char page[6000];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html><head><meta charset='utf-8'><title>WiFi</title>"
             "<style>body{font-family:sans-serif;max-width:36em;margin:1em}"
             "label{display:block;margin:6px 0}input[type=text],input[type=password]{width:100%%;padding:8px}"
             "</style></head><body>%s<h2>WiFi stazione</h2>"
             "<p>Radio accesa solo in Config e RTC. In PIR l'interfaccia resta giù.</p>"
             "<p>Rete salvata: <strong>%s</strong></p>"
             "<p><a href='/wifi?scan=1'>Scansiona reti</a></p>"
             "<form method='POST' action='/wifi'>%s"
             "<label>SSID</label><input name='wifi_ssid' value='%s'>"
             "<label>Password</label><input name='wifi_pass' type='password' value=''>"
             "<p>Lascia la password vuota per non cambiarla.</p>"
             "<button type='submit'>Salva e connetti</button></form></body></html>",
             web_nav(), cur[0] ? cur : "(nessuna)", list, cur);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_wifi_post(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) return send_redirect(req, "/");
    if (!hal_wifi_allowed()) return send_redirect(req, "/wifi");
    char buf[1024];
    if (read_post_body(req, buf, sizeof(buf)) <= 0) return send_redirect(req, "/wifi");
    char ssid[64] = {0};
    char pass[128] = {0};
    form_get_value(buf, "wifi_ssid", ssid, sizeof(ssid));
    form_get_value(buf, "wifi_pass", pass, sizeof(pass));
    if (ssid[0]) {
        snprintf(s_settings->wifi_ssid, sizeof(s_settings->wifi_ssid), "%s", ssid);
    }
    if (pass[0]) {
        snprintf(s_settings->wifi_pass, sizeof(s_settings->wifi_pass), "%s", pass);
    }
    (void)core_settings_save(s_settings);
    (void)hal_wifi_sta_apply(s_settings->wifi_ssid, s_settings->wifi_pass);
    return send_redirect(req, "/wifi");
}

bool core_web_start(core_settings_t *settings, core_web_wifi_mode_t wifi_mode,
                    const core_gpio_boot_snapshot_t *boot_error) {
    s_settings = settings;
    s_boot_error_page = false;
    core_live_bind_settings(s_settings);

    ESP_LOGI(TAG, "Boot config: WiFi → SD → Web (camera lazy su /live/stream)");

    if (wifi_mode == CORE_WEB_WIFI_AP_BOOT_ERROR) {
        if (!boot_error) return false;
        s_boot_error_page = true;
        s_boot_error_snap = *boot_error;
        core_status_led_set_mode(CORE_LED_ERROR);
        if (!wifi_start_ap(settings)) {
            ESP_LOGE(TAG, "AP errore boot non avviato");
            return false;
        }
    } else if (wifi_mode == CORE_WEB_WIFI_AP) {
        core_status_led_set_mode(CORE_LED_AP);
        if (!wifi_start_ap(settings)) {
            ESP_LOGE(TAG, "WiFi AP non avviato");
            return false;
        }
    } else {
        if (!settings || settings->wifi_ssid[0] == 0) {
            ESP_LOGI(TAG, "SSID vuoto — radio su solo per lo scan, nessuna associazione");
            (void)hal_wifi_up_for_scan();
        } else if (!wifi_start_sta(settings)) {
            ESP_LOGW(TAG, "STA non associata — GUI resta su, radio su per un nuovo scan");
            (void)hal_wifi_up_for_scan();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(CORE_CONFIG_SD_SETTLE_MS));

    if (!core_sd_is_mounted() && !core_sd_init_boot()) {
        ESP_LOGW(TAG, "SD non montata — pagina video limitata");
    }

    core_live_async_init();
    calibrate_load_state();
    core_time_init();
    /* CONFIG: always try SNTP (eth0 lab/DHCP on Luckfox; WiFi STA on P4). */
    if (wifi_mode != CORE_WEB_WIFI_AP_BOOT_ERROR) {
        xTaskCreate(calibrate_sntp_task, "sntp", 4096, nullptr, 3, nullptr);
    }

    return start_http_server();
}

void core_web_stop(void) {
    s_sta_has_ip = false;
    core_live_request_stop();
    for (int i = 0; i < 100 && core_live_is_running(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    core_video_deinit();
    core_status_led_set_mode(CORE_LED_OFF);
    s_boot_error_page = false;
}
