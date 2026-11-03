#pragma once

/* Host-sim V4L2 types. On Luckfox (Linux) the real <linux/videodev2.h> is used
   when not HOST_SIM. This stub lets Windows/host syntax-check. */

#include <stdint.h>
#include <string.h>

#ifndef VIDIOC_QUERYCAP
#define VIDIOC_QUERYCAP      0x80685600
#define VIDIOC_G_FMT         0xC0D05604
#define VIDIOC_S_FMT         0xC0D05605
#define VIDIOC_QUERY_EXT_CTRL 0xC0C856C7
#define VIDIOC_S_CTRL        0xC008561C
#define VIDIOC_G_CTRL        0xC008561B
#define VIDIOC_S_EXT_CTRLS   0xC0C85648
#define VIDIOC_G_EXT_CTRLS   0xC0C85647
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_CID_USER_CLASS  0x00980001
#define V4L2_CTRL_CLASS_USER 0x00980000
#define V4L2_CID_CAMERA_CLASS 0x009a0001
#define V4L2_CID_VFLIP       0x00980915
#define V4L2_CID_HFLIP       0x00980914
#define V4L2_CID_SATURATION  0x00980903
#define V4L2_CID_CONTRAST    0x00980901
#define V4L2_CID_BRIGHTNESS  0x00980900
#define V4L2_CID_HUE         0x00980902
#define V4L2_CID_FOCUS_ABSOLUTE 0x009a090a
#define V4L2_CID_USER_ESP_ISP_WB  0x00A00001
#define V4L2_CID_USER_ESP_ISP_AWB 0x00A00002

struct v4l2_capability {
    uint8_t driver[16];
    uint8_t card[32];
    uint8_t bus_info[32];
};

struct v4l2_pix_format {
    uint32_t width, height;
};

struct v4l2_format {
    uint32_t type;
    struct {
        struct v4l2_pix_format pix;
    } fmt;
};

struct v4l2_control {
    uint32_t id;
    int32_t value;
};

struct v4l2_ext_control {
    uint32_t id;
    uint32_t size;
    int32_t value;
    uint8_t *p_u8;
};

struct v4l2_ext_controls {
    uint32_t ctrl_class;
    uint32_t count;
    struct v4l2_ext_control *controls;
};

struct v4l2_query_ext_ctrl {
    uint32_t id;
    int64_t minimum, maximum;
};
#endif
