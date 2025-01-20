/*
 * Copyright (c) 2023 Huseyin BIYIK
 * Copyright (c) 2023 NyanMisaka
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Rockchip MPP (Media Process Platform) video encoder
 */

#ifndef AVCODEC_RKMPPENC_H
#define AVCODEC_RKMPPENC_H

#include <rockchip/rk_mpi.h>

#include "codec_internal.h"
#include "encode.h"
#include "hwconfig.h"
#include "internal.h"

#include "libavutil/hwcontext_rkmpp.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#define H26X_HEADER_SIZE   1024
#define H26X_ASYNC_FRAMES  4
#define MJPEG_ASYNC_FRAMES 8
#define ALIGN_DOWN(a, b) ((a) & ~((b)-1))

typedef struct MPPEncFrame {
    AVFrame            *frame;
    MppFrame            mpp_frame;
    struct MPPEncFrame *next;
    int                 queued;
} MPPEncFrame;

typedef struct RKMPPEncContext {
    AVClass           *class;

    MppApi            *mapi;
    MppCtx             mctx;

    AVBufferRef       *hwdevice;
    AVBufferRef       *hwframe;

    MppEncCfg          mcfg;
    int                cfg_init;
    MppFrameFormat     mpp_fmt;
    enum AVPixelFormat pix_fmt;

    MPPEncFrame       *frame_list;
    int                async_frames;

    int                rc_mode;
    int                qp_init;
    int                qp_max;
    int                qp_min;
    int                qp_max_i;
    int                qp_min_i;
    int                profile;
    int                tier;
    int                level;
    int                coder;
    int                dct8x8;
} RKMPPEncContext;

static const AVRational mpp_tb = { 1, 1000000 };

#define PTS_TO_MPP_PTS(pts, pts_tb) ((pts_tb.num && pts_tb.den) ? \
    av_rescale_q(pts, pts_tb, mpp_tb) : pts)

#define MPP_PTS_TO_PTS(mpp_pts, pts_tb) ((pts_tb.num && pts_tb.den) ? \
    av_rescale_q(mpp_pts, mpp_tb, pts_tb) : mpp_pts)

#define OFFSET(x) offsetof(RKMPPEncContext, x)
#define VE (AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

#define RKMPP_ENC_COMMON_OPTS \
    { "rc_mode", "Set the encoding rate control mode", OFFSET(rc_mode), AV_OPT_TYPE_INT, \
            { .i64 = MPP_ENC_RC_MODE_BUTT }, MPP_ENC_RC_MODE_VBR, MPP_ENC_RC_MODE_BUTT, VE, "rc_mode"}, \
        { "VBR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_VBR }, 0, 0, VE, "rc_mode" }, \
        { "CBR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_CBR }, 0, 0, VE, "rc_mode" }, \
        { "CQP", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_FIXQP }, 0, 0, VE, "rc_mode" }, \
        { "AVBR", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MPP_ENC_RC_MODE_AVBR }, 0, 0, VE, "rc_mode" }, \
    { "qp_init", "Set the initial QP value", OFFSET(qp_init), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 51, VE, "qmin" }, \
    { "qp_max", "Set the max QP value for P and B frame", OFFSET(qp_max), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 51, VE, "qp_max" }, \
    { "qp_min", "Set the min QP value for P and B frame", OFFSET(qp_min), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 51, VE, "qp_min" }, \
    { "qp_max_i", "Set the max QP value for I frame", OFFSET(qp_max_i), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 51, VE, "qp_max_i" }, \
    { "qp_min_i", "Set the min QP value for I frame", OFFSET(qp_min_i), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 51, VE, "qp_min_i" }, \

static const AVOption h264_options[] = {
    RKMPP_ENC_COMMON_OPTS
    { "profile", "Set the encoding profile restriction", OFFSET(profile), AV_OPT_TYPE_INT,
            { .i64 = FF_PROFILE_H264_HIGH }, -1, FF_PROFILE_H264_HIGH, VE, "profile" },
        { "baseline",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE }, INT_MIN, INT_MAX, VE, "profile" },
        { "main",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN },     INT_MIN, INT_MAX, VE, "profile" },
        { "high",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH },     INT_MIN, INT_MAX, VE, "profile" },
    { "level", "Set the encoding level restriction", OFFSET(level), AV_OPT_TYPE_INT,
            { .i64 = 0 }, FF_LEVEL_UNKNOWN, 62, VE, "level" },
        { "1",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 10 }, 0, 0, VE, "level" },
        { "1.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 11 }, 0, 0, VE, "level" },
        { "1.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 12 }, 0, 0, VE, "level" },
        { "1.3",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 13 }, 0, 0, VE, "level" },
        { "2",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 20 }, 0, 0, VE, "level" },
        { "2.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 21 }, 0, 0, VE, "level" },
        { "2.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 22 }, 0, 0, VE, "level" },
        { "3",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0, VE, "level" },
        { "3.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 31 }, 0, 0, VE, "level" },
        { "3.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 32 }, 0, 0, VE, "level" },
        { "4",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 40 }, 0, 0, VE, "level" },
        { "4.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 41 }, 0, 0, VE, "level" },
        { "4.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 42 }, 0, 0, VE, "level" },
        { "5",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 50 }, 0, 0, VE, "level" },
        { "5.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 51 }, 0, 0, VE, "level" },
        { "5.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 52 }, 0, 0, VE, "level" },
        { "6",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 60 }, 0, 0, VE, "level" },
        { "6.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 61 }, 0, 0, VE, "level" },
        { "6.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 62 }, 0, 0, VE, "level" },
    { "coder", "Set the entropy coder type (from 0 to 1) (default cabac)", OFFSET(coder), AV_OPT_TYPE_INT,
            { .i64 = 1 }, 0, 1, VE, "coder" },
        { "cavlc", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, INT_MIN, INT_MAX, VE, "coder" },
        { "cabac", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, INT_MIN, INT_MAX, VE, "coder" },
    { "8x8dct", "Set the high profile 8x8 transform", OFFSET(dct8x8), AV_OPT_TYPE_BOOL,
            { .i64 = 1 }, 0, 1, VE, "8x8dct" },
    { NULL },
};

static const AVOption hevc_options[] = {
    RKMPP_ENC_COMMON_OPTS
    { "profile", "Set the encoding profile restriction", OFFSET(profile), AV_OPT_TYPE_INT,
            { .i64 = FF_PROFILE_HEVC_MAIN }, -1, FF_PROFILE_HEVC_MAIN, VE, "profile" },
        { "main",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_HEVC_MAIN }, INT_MIN, INT_MAX, VE, "profile" },
    { "tier", "Set the encoding profile tier restriction", OFFSET(tier), AV_OPT_TYPE_INT,
            { .i64 = 1 }, 0, 1, VE, "tier" },
        { "main",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, INT_MIN, INT_MAX, VE, "tier" },
        { "high",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, INT_MIN, INT_MAX, VE, "tier" },
    { "level", "Set the encoding level restriction", OFFSET(level), AV_OPT_TYPE_INT,
            { .i64 = 0 }, FF_LEVEL_UNKNOWN, 186, VE, "level" },
        { "1",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0, VE, "level" },
        { "2",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 60 }, 0, 0, VE, "level" },
        { "2.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 63 }, 0, 0, VE, "level" },
        { "3",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 90 }, 0, 0, VE, "level" },
        { "3.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 93 }, 0, 0, VE, "level" },
        { "4",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 120 }, 0, 0, VE, "level" },
        { "4.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 123 }, 0, 0, VE, "level" },
        { "5",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 150 }, 0, 0, VE, "level" },
        { "5.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 153 }, 0, 0, VE, "level" },
        { "5.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 156 }, 0, 0, VE, "level" },
        { "6",          NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 180 }, 0, 0, VE, "level" },
        { "6.1",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 183 }, 0, 0, VE, "level" },
        { "6.2",        NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 186 }, 0, 0, VE, "level" },
    { NULL },
};

static const AVOption mjpeg_options[] = {
    { "qp_init", "Set the initial QP/Q_Factor value", OFFSET(qp_init), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 99, VE, "qmin" }, \
    { "qp_max", "Set the max QP/Q_Factor value", OFFSET(qp_max), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 99, VE, "qp_max" }, \
    { "qp_min", "Set the min QP/Q_Factor value", OFFSET(qp_min), AV_OPT_TYPE_INT, \
            { .i64 = -1 }, -1, 99, VE, "qp_min" }, \
    { NULL },
};

static const enum AVPixelFormat rkmpp_enc_pix_fmts_h26x[] = {
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV21,
    AV_PIX_FMT_NV16,
    AV_PIX_FMT_NV24,
    AV_PIX_FMT_YUYV422,
    AV_PIX_FMT_YVYU422,
    AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_BGR24,
    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_RGB0,
    AV_PIX_FMT_BGRA,
    AV_PIX_FMT_BGR0,
    AV_PIX_FMT_ARGB,
    AV_PIX_FMT_0RGB,
    AV_PIX_FMT_ABGR,
    AV_PIX_FMT_0BGR,
    AV_PIX_FMT_DRM_PRIME,
    AV_PIX_FMT_NONE,
};

static const enum AVPixelFormat rkmpp_enc_pix_fmts_mjpeg[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUYV422,
    AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_RGB444BE,
    AV_PIX_FMT_BGR444BE,
    AV_PIX_FMT_RGB555BE,
    AV_PIX_FMT_BGR555BE,
    AV_PIX_FMT_RGB565BE,
    AV_PIX_FMT_BGR565BE,
    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_RGB0,
    AV_PIX_FMT_BGRA,
    AV_PIX_FMT_BGR0,
    AV_PIX_FMT_ARGB,
    AV_PIX_FMT_0RGB,
    AV_PIX_FMT_ABGR,
    AV_PIX_FMT_0BGR,
    AV_PIX_FMT_X2RGB10BE,
    AV_PIX_FMT_X2BGR10BE,
    AV_PIX_FMT_DRM_PRIME,
    AV_PIX_FMT_NONE,
};

static const AVCodecHWConfigInternal *const rkmpp_enc_hw_configs[] = {
    HW_CONFIG_ENCODER_DEVICE(NONE,      RKMPP),
    HW_CONFIG_ENCODER_FRAMES(DRM_PRIME, RKMPP),
    HW_CONFIG_ENCODER_FRAMES(DRM_PRIME, DRM),
    NULL,
};

static const FFCodecDefault rkmpp_enc_defaults[] = {
    { "b",  "2M"  },
    { "g",  "250" },
    { NULL },
};

#define DEFINE_RKMPP_ENCODER(x, X, xx) \
static const AVClass x##_rkmpp_encoder_class = { \
    .class_name = #x "_rkmpp_encoder", \
    .item_name  = av_default_item_name, \
    .option     = x##_options, \
    .version    = LIBAVUTIL_VERSION_INT, \
}; \
const FFCodec ff_##x##_rkmpp_encoder = { \
    .p.name         = #x "_rkmpp", \
    CODEC_LONG_NAME("Rockchip MPP (Media Process Platform) " #X " encoder"), \
    .p.type         = AVMEDIA_TYPE_VIDEO, \
    .p.id           = AV_CODEC_ID_##X, \
    .priv_data_size = sizeof(RKMPPEncContext), \
    .p.priv_class   = &x##_rkmpp_encoder_class, \
    .init           = rkmpp_encode_init, \
    .close          = rkmpp_encode_close, \
    FF_CODEC_ENCODE_CB(rkmpp_encode_frame), \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | \
                      FF_CODEC_CAP_INIT_CLEANUP, \
    .p.pix_fmts     = rkmpp_enc_pix_fmts_##xx, \
    .hw_configs     = rkmpp_enc_hw_configs, \
    .defaults       = rkmpp_enc_defaults, \
    .p.wrapper_name = "rkmpp", \
};

#endif /* AVCODEC_RKMPPENC_H */
