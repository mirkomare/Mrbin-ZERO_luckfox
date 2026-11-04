#include "hal_muxer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUX_SPS_MAX 128
#define MUX_PPS_MAX 128

typedef struct {
    uint32_t count;
    uint32_t delta;
} stts_run_t;

typedef struct {
    esp_muxer_config_t cfg;
    esp_muxer_file_writer_t wr;
    void *file;
    int codec, w, h, fps;
    int type; /* 1=mp4 2=avi */
    uint64_t file_pos;
    uint64_t mdat_size_off;
    uint64_t mdat_data_off;
    bool header_written;
    uint8_t *mdat; /* AVI only (MJPEG) */
    size_t mdat_len, mdat_cap;
    uint32_t frames;
    uint32_t *sizes;
    uint32_t *pts;
    uint8_t *keys;
    size_t sizes_cap;
    uint8_t sps[MUX_SPS_MAX];
    int sps_len;
    uint8_t pps[MUX_PPS_MAX];
    int pps_len;
} mux_t;

void mp4_muxer_register(void) {}
void avi_muxer_register(void) {}

static const uint8_t k_matrix[36] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
};

static void wr_bytes(mux_t *m, const void *p, int n) {
    if (!m || n <= 0) {
        return;
    }
    if (m->wr.on_write && m->file) {
        m->wr.on_write(m->file, (void *)p, n);
    }
    m->file_pos += (uint64_t)n;
}

static void wr_seek(mux_t *m, uint64_t pos) {
    if (m->wr.on_seek && m->file) {
        m->wr.on_seek(m->file, pos);
    }
    m->file_pos = pos;
}

static void wr_u32be(mux_t *m, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    wr_bytes(m, b, 4);
}

static void wr_u16be(mux_t *m, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    wr_bytes(m, b, 2);
}

static void wr_u32le(mux_t *m, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    wr_bytes(m, b, 4);
}

static void wr_four(mux_t *m, const char *s) { wr_bytes(m, s, 4); }

static void wr_zeros(mux_t *m, int n) {
    uint8_t z[32] = {0};
    while (n > 0) {
        int c = n > 32 ? 32 : n;
        wr_bytes(m, z, c);
        n -= c;
    }
}

esp_muxer_handle_t esp_muxer_open(esp_muxer_config_t *cfg, size_t) {
    mux_t *m = (mux_t *)calloc(1, sizeof(*m));
    if (!m) {
        return nullptr;
    }
    if (cfg) {
        m->cfg = *cfg;
    }
    m->type = cfg ? cfg->muxer_type : ESP_MUXER_TYPE_MP4;
    m->fps = 12;
    return m;
}

esp_muxer_err_t esp_muxer_set_file_writer(esp_muxer_handle_t h, const esp_muxer_file_writer_t *w) {
    mux_t *m = (mux_t *)h;
    if (!m || !w) {
        return -1;
    }
    m->wr = *w;
    char path[256] = {0};
    if (m->cfg.url_pattern_ex) {
        esp_muxer_slice_info_t info = {};
        info.len = (int)sizeof(info.file_path);
        m->cfg.url_pattern_ex(&info, m->cfg.ctx);
        snprintf(path, sizeof(path), "%s", info.file_path);
    } else if (m->cfg.url_pattern) {
        snprintf(path, sizeof(path), "%s", m->cfg.url_pattern);
    }
    if (m->wr.on_open) {
        m->file = m->wr.on_open(path);
    }
    m->file_pos = 0;
    return m->file ? ESP_MUXER_ERR_OK : -1;
}

static int find_start(const uint8_t *d, int n, int off) {
    for (int i = off; i + 3 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            return i;
        }
        if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            return i;
        }
    }
    return -1;
}

static void take_nal(mux_t *m, const uint8_t *nal, int len) {
    if (!m || !nal || len <= 0) {
        return;
    }
    uint8_t t = nal[0] & 0x1f;
    if (t == 7 && len <= MUX_SPS_MAX) {
        memcpy(m->sps, nal, (size_t)len);
        m->sps_len = len;
    } else if (t == 8 && len <= MUX_PPS_MAX) {
        memcpy(m->pps, nal, (size_t)len);
        m->pps_len = len;
    }
}

static void scan_annexb_cfg(mux_t *m, const uint8_t *d, int n) {
    if (!d || n <= 0) {
        return;
    }
    int sc = find_start(d, n, 0);
    while (sc >= 0) {
        int hdr = (d[sc + 2] == 1) ? 3 : 4;
        int nal = sc + hdr;
        int next = find_start(d, n, nal);
        if (next < 0) {
            next = n;
        }
        if (nal < next) {
            take_nal(m, d + nal, next - nal);
        }
        sc = find_start(d, n, next);
        if (sc >= 0 && sc < next) {
            break;
        }
    }
}

static void scan_avcc_cfg(mux_t *m, const uint8_t *d, int n) {
    int off = 0;
    while (off + 4 <= n) {
        int sz = (d[off] << 24) | (d[off + 1] << 16) | (d[off + 2] << 8) | d[off + 3];
        off += 4;
        if (sz <= 0 || off + sz > n) {
            break;
        }
        take_nal(m, d + off, sz);
        off += sz;
    }
}

esp_muxer_err_t esp_muxer_add_video_stream(esp_muxer_handle_t h,
                                           const esp_muxer_video_stream_info_t *info, int *idx) {
    mux_t *m = (mux_t *)h;
    if (!m || !info) {
        return -1;
    }
    m->codec = info->codec;
    m->w = info->width;
    m->h = info->height;
    m->fps = info->fps > 0 ? info->fps : 12;
    if (info->codec_spec_info && info->spec_info_len > 0) {
        scan_annexb_cfg(m, info->codec_spec_info, info->spec_info_len);
        if (m->sps_len == 0) {
            scan_avcc_cfg(m, info->codec_spec_info, info->spec_info_len);
        }
    }
    if (idx) {
        *idx = 0;
    }
    return ESP_MUXER_ERR_OK;
}

static bool packet_has_idr(const uint8_t *d, int n) {
    int sc = find_start(d, n, 0);
    while (sc >= 0) {
        int hdr = (d[sc + 2] == 1) ? 3 : 4;
        int nal = sc + hdr;
        if (nal < n && (d[nal] & 0x1f) == 5) {
            return true;
        }
        int next = find_start(d, n, nal);
        if (next < 0) {
            break;
        }
        sc = next;
        if (sc < nal) {
            break;
        }
    }
    return false;
}

static void annexb_to_avcc(const uint8_t *in, int in_len, uint8_t **out, int *out_len) {
    *out = nullptr;
    *out_len = 0;
    uint8_t *buf = (uint8_t *)malloc((size_t)in_len + 16);
    if (!buf) {
        return;
    }
    int w = 0;
    int sc = find_start(in, in_len, 0);
    while (sc >= 0) {
        int hdr = (in[sc + 2] == 1) ? 3 : 4;
        int nal = sc + hdr;
        int next = find_start(in, in_len, nal);
        if (next < 0) {
            next = in_len;
        }
        int sz = next - nal;
        if (sz < 0) {
            break;
        }
        buf[w + 0] = (uint8_t)(sz >> 24);
        buf[w + 1] = (uint8_t)(sz >> 16);
        buf[w + 2] = (uint8_t)(sz >> 8);
        buf[w + 3] = (uint8_t)sz;
        if (sz > 0) {
            memcpy(buf + w + 4, in + nal, (size_t)sz);
        }
        w += 4 + sz;
        sc = find_start(in, in_len, next);
        if (sc >= 0 && sc < next) {
            break;
        }
    }
    *out = buf;
    *out_len = w;
}

static void to_avcc(const uint8_t *in, int in_len, uint8_t **out, int *out_len) {
    annexb_to_avcc(in, in_len, out, out_len);
    if (*out && *out_len > 0) {
        return;
    }
    free(*out);
    *out = nullptr;
    *out_len = 0;
    if (in_len >= 5) {
        uint32_t sz = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                      ((uint32_t)in[2] << 8) | in[3];
        if (sz > 0 && sz + 4 <= (uint32_t)in_len) {
            *out = (uint8_t *)malloc((size_t)in_len);
            if (*out) {
                memcpy(*out, in, (size_t)in_len);
                *out_len = in_len;
            }
            return;
        }
    }
    *out = (uint8_t *)malloc((size_t)in_len + 4);
    if (!*out) {
        return;
    }
    (*out)[0] = (uint8_t)(in_len >> 24);
    (*out)[1] = (uint8_t)(in_len >> 16);
    (*out)[2] = (uint8_t)(in_len >> 8);
    (*out)[3] = (uint8_t)in_len;
    memcpy(*out + 4, in, (size_t)in_len);
    *out_len = in_len + 4;
}

static int grow_meta(mux_t *m) {
    if (m->frames + 1 <= m->sizes_cap) {
        return 0;
    }
    size_t nc = m->sizes_cap ? m->sizes_cap * 2 : 64;
    uint32_t *sz = (uint32_t *)realloc(m->sizes, nc * sizeof(uint32_t));
    if (!sz) {
        return -1;
    }
    m->sizes = sz;
    uint32_t *pts = (uint32_t *)realloc(m->pts, nc * sizeof(uint32_t));
    if (!pts) {
        return -1;
    }
    m->pts = pts;
    uint8_t *keys = (uint8_t *)realloc(m->keys, nc);
    if (!keys) {
        return -1;
    }
    m->keys = keys;
    m->sizes_cap = nc;
    return 0;
}

static int avi_append(mux_t *m, const uint8_t *data, int len) {
    if (m->mdat_len + (size_t)len > m->mdat_cap) {
        size_t cap = m->mdat_cap ? m->mdat_cap * 2 : (size_t)(1 << 20);
        while (cap < m->mdat_len + (size_t)len) {
            cap *= 2;
        }
        uint8_t *p = (uint8_t *)realloc(m->mdat, cap);
        if (!p) {
            return -1;
        }
        m->mdat = p;
        m->mdat_cap = cap;
    }
    memcpy(m->mdat + m->mdat_len, data, (size_t)len);
    m->mdat_len += (size_t)len;
    return 0;
}

static void write_ftyp_mdat_hdr(mux_t *m) {
    wr_u32be(m, 32);
    wr_four(m, "ftyp");
    wr_four(m, "isom");
    wr_u32be(m, 0x00000200);
    wr_four(m, "isom");
    wr_four(m, "iso2");
    wr_four(m, "avc1");
    wr_four(m, "mp41");
    m->mdat_size_off = m->file_pos;
    wr_u32be(m, 8);
    wr_four(m, "mdat");
    m->mdat_data_off = m->file_pos;
    m->header_written = true;
}

static int wr_annexb_as_avcc(mux_t *m, const uint8_t *in, int in_len) {
    int sc = find_start(in, in_len, 0);
    if (sc < 0) {
        return -1;
    }
    int written = 0;
    while (sc >= 0) {
        int hdr = (in[sc + 2] == 1) ? 3 : 4;
        int nal = sc + hdr;
        int next = find_start(in, in_len, nal);
        if (next < 0) {
            next = in_len;
        }
        int sz = next - nal;
        if (sz < 0) {
            break;
        }
        uint8_t lenb[4] = {
            (uint8_t)(sz >> 24),
            (uint8_t)(sz >> 16),
            (uint8_t)(sz >> 8),
            (uint8_t)sz,
        };
        wr_bytes(m, lenb, 4);
        if (sz > 0) {
            wr_bytes(m, in + nal, sz);
        }
        written += 4 + sz;
        sc = find_start(in, in_len, next);
        if (sc >= 0 && sc < next) {
            break;
        }
    }
    return written > 0 ? written : -1;
}

esp_muxer_err_t esp_muxer_add_video_packet(esp_muxer_handle_t h, int, esp_muxer_video_packet_t *pkt) {
    mux_t *m = (mux_t *)h;
    if (!m || !pkt || !pkt->data || pkt->len <= 0) {
        return -1;
    }

    if (m->codec == ESP_MUXER_VDEC_H264) {
        scan_annexb_cfg(m, pkt->data, pkt->len);
    }

    /* Stream Annex-B into mdat. A full AVCC copy is another 1–2 MB and
     * fails once the ring already holds an 8MP IDR, so the file stays empty. */
    if (m->type == ESP_MUXER_TYPE_MP4 && m->codec == ESP_MUXER_VDEC_H264 &&
        find_start(pkt->data, pkt->len, 0) >= 0) {
        if (grow_meta(m) != 0) {
            return -1;
        }
        if (!m->header_written) {
            write_ftyp_mdat_hdr(m);
        }
        int plen = wr_annexb_as_avcc(m, pkt->data, pkt->len);
        if (plen <= 0) {
            return -1;
        }
        bool key = pkt->key_frame || packet_has_idr(pkt->data, pkt->len);
        m->sizes[m->frames] = (uint32_t)plen;
        m->pts[m->frames] = pkt->pts;
        m->keys[m->frames] = key ? 1 : 0;
        m->frames++;
        return ESP_MUXER_ERR_OK;
    }

    uint8_t *payload = nullptr;
    int plen = 0;
    if (m->type == ESP_MUXER_TYPE_MP4 && m->codec == ESP_MUXER_VDEC_H264) {
        to_avcc(pkt->data, pkt->len, &payload, &plen);
        if (payload && plen > 0 && m->sps_len == 0) {
            scan_avcc_cfg(m, payload, plen);
        }
    } else {
        plen = pkt->len;
        payload = (uint8_t *)malloc((size_t)plen);
        if (payload) {
            memcpy(payload, pkt->data, (size_t)plen);
        }
    }
    if (!payload || plen <= 0) {
        free(payload);
        return -1;
    }

    if (grow_meta(m) != 0) {
        free(payload);
        return -1;
    }

    bool key = pkt->key_frame || packet_has_idr(pkt->data, pkt->len);
    if (m->type == ESP_MUXER_TYPE_MP4) {
        if (!m->header_written) {
            write_ftyp_mdat_hdr(m);
        }
        wr_bytes(m, payload, plen);
    } else if (avi_append(m, payload, plen) != 0) {
        free(payload);
        return -1;
    }

    m->sizes[m->frames] = (uint32_t)plen;
    m->pts[m->frames] = pkt->pts;
    m->keys[m->frames] = key ? 1 : 0;
    m->frames++;
    free(payload);
    return ESP_MUXER_ERR_OK;
}

static uint32_t default_delta(const mux_t *m) {
    if (!m->fps) {
        return 33;
    }
    uint32_t d = 1000u / (uint32_t)m->fps;
    return d ? d : 1;
}

static uint32_t sample_delta(const mux_t *m, uint32_t i) {
    uint32_t def = default_delta(m);
    /* Timelapse 1–3 fps: always use the muxer fps so stts/duration match
     * playback (wall-clock jitter + VENC 25 fps VUI confused players). */
    if (!m || m->fps <= 3) {
        return def;
    }
    if (!m->pts || m->frames < 2) {
        return def;
    }
    if (i + 1 < m->frames) {
        uint32_t a = m->pts[i];
        uint32_t b = m->pts[i + 1];
        if (b > a) {
            uint32_t d = b - a;
            if (d > 0 && d < 10000) {
                return d;
            }
        }
        return def;
    }
    return sample_delta(m, i - 1);
}

static uint32_t movie_duration(const mux_t *m) {
    uint32_t dur = 0;
    for (uint32_t i = 0; i < m->frames; ++i) {
        dur += sample_delta(m, i);
    }
    if (dur == 0) {
        dur = default_delta(m) * (m->frames ? m->frames : 1);
    }
    return dur;
}

static uint32_t count_keys(const mux_t *m) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->frames; ++i) {
        if (m->keys && m->keys[i]) {
            n++;
        }
    }
    return n ? n : (m->frames ? 1 : 0);
}

static uint32_t build_stts(const mux_t *m, stts_run_t *runs, uint32_t max_runs) {
    uint32_t nrun = 0;
    if (m->frames == 0) {
        if (max_runs) {
            runs[0].count = 1;
            runs[0].delta = default_delta(m);
            return 1;
        }
        return 0;
    }
    uint32_t cur_d = sample_delta(m, 0);
    uint32_t cur_n = 1;
    for (uint32_t i = 1; i < m->frames; ++i) {
        uint32_t d = sample_delta(m, i);
        if (d == cur_d) {
            cur_n++;
        } else {
            if (nrun < max_runs) {
                runs[nrun].count = cur_n;
                runs[nrun].delta = cur_d;
            }
            nrun++;
            cur_d = d;
            cur_n = 1;
        }
    }
    if (nrun < max_runs) {
        runs[nrun].count = cur_n;
        runs[nrun].delta = cur_d;
    }
    nrun++;
    return nrun;
}

static uint32_t avcc_payload_size(const mux_t *m) {
    int sps = m->sps_len > 0 ? m->sps_len : 0;
    int pps = m->pps_len > 0 ? m->pps_len : 0;
    return (uint32_t)(11 + sps + pps);
}

static void write_avcc(mux_t *m) {
    uint32_t psz = avcc_payload_size(m);
    wr_u32be(m, 8 + psz);
    wr_four(m, "avcC");
    uint8_t profile = 0x64, compat = 0x00, level = 0x28;
    if (m->sps_len >= 4) {
        profile = m->sps[1];
        compat = m->sps[2];
        level = m->sps[3];
    }
    uint8_t hdr[6] = {
        1,
        profile,
        compat,
        level,
        0xFF,
        (uint8_t)(0xE0 | (m->sps_len > 0 ? 1 : 0)),
    };
    wr_bytes(m, hdr, 6);
    wr_u16be(m, (uint16_t)m->sps_len);
    if (m->sps_len > 0) {
        wr_bytes(m, m->sps, m->sps_len);
    }
    uint8_t npps = m->pps_len > 0 ? 1 : 0;
    wr_bytes(m, &npps, 1);
    wr_u16be(m, (uint16_t)m->pps_len);
    if (m->pps_len > 0) {
        wr_bytes(m, m->pps, m->pps_len);
    }
}

static void write_mp4_moov(mux_t *m) {
    const uint32_t n = m->frames;
    const uint32_t dur = movie_duration(m);
    const uint32_t nkeys = count_keys(m);
    const uint16_t w = (uint16_t)(m->w > 0 ? m->w : 1280);
    const uint16_t h = (uint16_t)(m->h > 0 ? m->h : 720);

    stts_run_t one_run;
    stts_run_t *runs = nullptr;
    uint32_t nrun = 0;
    if (n > 0 && m->fps > 3) {
        runs = (stts_run_t *)malloc((size_t)n * sizeof(stts_run_t));
        if (runs) {
            nrun = build_stts(m, runs, n);
        }
    }
    if (!runs) {
        one_run.count = n ? n : 1;
        one_run.delta = default_delta(m);
        runs = &one_run;
        nrun = 1;
    }

    const uint32_t avcc_sz = 8 + avcc_payload_size(m);
    const uint32_t avc1_sz = 8 + 78 + avcc_sz;
    const uint32_t stsd_sz = 8 + 8 + avc1_sz;
    const uint32_t stts_sz = 8 + 8 + 8 * nrun;
    const uint32_t stsc_sz = 8 + 8 + 12;
    const uint32_t stsz_sz = 8 + 12 + 4 * n;
    const uint32_t stco_sz = 8 + 8 + 4;
    const uint32_t stss_sz = 8 + 8 + 4 * nkeys;
    const uint32_t stbl_sz = 8 + stsd_sz + stts_sz + stsc_sz + stsz_sz + stco_sz + stss_sz;
    const uint32_t vmhd_sz = 20;
    const uint32_t dinf_sz = 36;
    const uint32_t minf_sz = 8 + vmhd_sz + dinf_sz + stbl_sz;
    const uint32_t mdhd_sz = 32;
    const uint32_t hdlr_sz = 45;
    const uint32_t mdia_sz = 8 + mdhd_sz + hdlr_sz + minf_sz;
    const uint32_t tkhd_sz = 92;
    const uint32_t trak_sz = 8 + tkhd_sz + mdia_sz;
    const uint32_t mvhd_sz = 108;
    const uint32_t moov_sz = 8 + mvhd_sz + trak_sz;

    wr_u32be(m, moov_sz);
    wr_four(m, "moov");

    wr_u32be(m, mvhd_sz);
    wr_four(m, "mvhd");
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_u32be(m, 1000);
    wr_u32be(m, dur);
    wr_u32be(m, 0x00010000);
    wr_u16be(m, 0x0100);
    wr_u16be(m, 0);
    wr_zeros(m, 8);
    wr_bytes(m, k_matrix, 36);
    wr_zeros(m, 24);
    wr_u32be(m, 2);

    wr_u32be(m, trak_sz);
    wr_four(m, "trak");

    wr_u32be(m, tkhd_sz);
    wr_four(m, "tkhd");
    wr_u32be(m, 0x00000F);
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_u32be(m, 1);
    wr_u32be(m, 0);
    wr_u32be(m, dur);
    wr_zeros(m, 8);
    wr_u16be(m, 0);
    wr_u16be(m, 0);
    wr_u16be(m, 0);
    wr_u16be(m, 0);
    wr_bytes(m, k_matrix, 36);
    wr_u32be(m, (uint32_t)w << 16);
    wr_u32be(m, (uint32_t)h << 16);

    wr_u32be(m, mdia_sz);
    wr_four(m, "mdia");

    wr_u32be(m, mdhd_sz);
    wr_four(m, "mdhd");
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_u32be(m, 1000);
    wr_u32be(m, dur);
    wr_u16be(m, 0x55C4);
    wr_u16be(m, 0);

    wr_u32be(m, hdlr_sz);
    wr_four(m, "hdlr");
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_four(m, "vide");
    wr_zeros(m, 12);
    wr_bytes(m, "VideoHandler", 13);

    wr_u32be(m, minf_sz);
    wr_four(m, "minf");

    wr_u32be(m, vmhd_sz);
    wr_four(m, "vmhd");
    wr_u32be(m, 1);
    wr_u32be(m, 0);
    wr_u32be(m, 0);

    wr_u32be(m, dinf_sz);
    wr_four(m, "dinf");
    wr_u32be(m, 28);
    wr_four(m, "dref");
    wr_u32be(m, 0);
    wr_u32be(m, 1);
    wr_u32be(m, 12);
    wr_four(m, "url ");
    wr_u32be(m, 1);

    wr_u32be(m, stbl_sz);
    wr_four(m, "stbl");

    wr_u32be(m, stsd_sz);
    wr_four(m, "stsd");
    wr_u32be(m, 0);
    wr_u32be(m, 1);

    wr_u32be(m, avc1_sz);
    wr_four(m, "avc1");
    wr_zeros(m, 6);
    wr_u16be(m, 1);
    wr_zeros(m, 16);
    wr_u16be(m, w);
    wr_u16be(m, h);
    wr_u32be(m, 0x00480000);
    wr_u32be(m, 0x00480000);
    wr_u32be(m, 0);
    wr_u16be(m, 1);
    wr_zeros(m, 32);
    wr_u16be(m, 0x0018);
    wr_u16be(m, 0xFFFF);
    write_avcc(m);

    wr_u32be(m, stts_sz);
    wr_four(m, "stts");
    wr_u32be(m, 0);
    wr_u32be(m, nrun);
    if (runs) {
        for (uint32_t i = 0; i < nrun; ++i) {
            wr_u32be(m, runs[i].count);
            wr_u32be(m, runs[i].delta);
        }
        if (runs != &one_run) {
            free(runs);
        }
    } else {
        wr_u32be(m, 1);
        wr_u32be(m, default_delta(m));
    }

    wr_u32be(m, stsc_sz);
    wr_four(m, "stsc");
    wr_u32be(m, 0);
    wr_u32be(m, 1);
    wr_u32be(m, 1);
    wr_u32be(m, n ? n : 1);
    wr_u32be(m, 1);

    wr_u32be(m, stsz_sz);
    wr_four(m, "stsz");
    wr_u32be(m, 0);
    wr_u32be(m, 0);
    wr_u32be(m, n);
    for (uint32_t i = 0; i < n; ++i) {
        wr_u32be(m, m->sizes[i]);
    }

    wr_u32be(m, stco_sz);
    wr_four(m, "stco");
    wr_u32be(m, 0);
    wr_u32be(m, 1);
    wr_u32be(m, (uint32_t)m->mdat_data_off);

    wr_u32be(m, stss_sz);
    wr_four(m, "stss");
    wr_u32be(m, 0);
    wr_u32be(m, nkeys);
    uint32_t wrote_keys = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (m->keys && m->keys[i]) {
            wr_u32be(m, i + 1);
            wrote_keys++;
        }
    }
    if (wrote_keys == 0 && nkeys > 0) {
        wr_u32be(m, 1);
    }

    ESP_LOGI("hal_mux", "MP4 moov %u frames %ux%u@%d sps=%d pps=%d dur_ms=%u",
             (unsigned)n, (unsigned)w, (unsigned)h, m->fps, m->sps_len, m->pps_len,
             (unsigned)dur);
}

static void write_avi(mux_t *m) {
    uint32_t framesz = 0;
    for (uint32_t i = 0; i < m->frames; ++i) {
        framesz += m->sizes[i] + 8;
        if (m->sizes[i] & 1) {
            framesz += 1;
        }
    }
    wr_four(m, "RIFF");
    wr_u32le(m, 12 + 8 + 64 + 8 + framesz);
    wr_four(m, "AVI ");
    wr_four(m, "LIST");
    wr_u32le(m, 64);
    wr_four(m, "hdrl");
    wr_four(m, "avih");
    wr_u32le(m, 56);
    uint8_t avih[56] = {0};
    wr_bytes(m, avih, 56);
    wr_four(m, "LIST");
    wr_u32le(m, framesz + 4);
    wr_four(m, "movi");
    size_t off = 0;
    for (uint32_t i = 0; i < m->frames; ++i) {
        wr_four(m, "00dc");
        wr_u32le(m, m->sizes[i]);
        wr_bytes(m, m->mdat + off, (int)m->sizes[i]);
        off += m->sizes[i];
        if (m->sizes[i] & 1) {
            uint8_t pad = 0;
            wr_bytes(m, &pad, 1);
        }
    }
}

esp_muxer_err_t esp_muxer_close(esp_muxer_handle_t h) {
    mux_t *m = (mux_t *)h;
    if (!m) {
        return ESP_MUXER_ERR_OK;
    }
    if (m->file) {
        if (m->type == ESP_MUXER_TYPE_AVI) {
            wr_seek(m, 0);
            write_avi(m);
        } else if (m->frames > 0 || m->header_written) {
            if (!m->header_written) {
                write_ftyp_mdat_hdr(m);
            }
            uint32_t mdat_box = (uint32_t)(8ull + (m->file_pos - m->mdat_data_off));
            uint64_t moov_at = m->file_pos;
            write_mp4_moov(m);
            uint64_t end_pos = m->file_pos;
            wr_seek(m, m->mdat_size_off);
            wr_u32be(m, mdat_box);
            wr_seek(m, end_pos);
            (void)moov_at;
        }
        if (m->wr.on_close) {
            m->wr.on_close(m->file);
        }
    }
    free(m->mdat);
    free(m->sizes);
    free(m->pts);
    free(m->keys);
    free(m);
    return ESP_MUXER_ERR_OK;
}
