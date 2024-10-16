/*
 * Copyright (c) 2017 Lionel CHAZALLON
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
 * Rockchip MPP (Media Process Platform) video decoder
 */

#ifndef AVCODEC_RKMPPDEC_H
#define AVCODEC_RKMPPDEC_H

#include <rockchip/rk_mpi.h>

#include "codec_internal.h"
#include "decode.h"
#include "hwconfig.h"
#include "internal.h"

#include "libavutil/avstring.h"
#include "libavutil/hwcontext_rkmpp.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#define MAX_ERRINFO_COUNT   100
#define MAX_SOC_NAME_LENGTH 128

typedef struct RKMPPDecContext {
    AVClass       *class;

    MppApi        *mapi;
    MppCtx         mctx;
    MppBufferGroup buf_group;

    AVBufferRef   *hwdevice;
    AVBufferRef   *hwframe;

    AVPacket       last_pkt;
    int            eof;
    int            draining;
    int            info_change;
    int            errinfo_cnt;
    int            got_frame;
    int            use_rfbc;

    int            deint;
    int            afbc;
    int            fast_parse;
    int            buf_mode;
} RKMPPDecContext;

enum {
    RKMPP_DEC_AFBC_OFF    = 0,
    RKMPP_DEC_AFBC_ON     = 1,
    RKMPP_DEC_AFBC_ON_RGA = 2,
};

enum {
    RKMPP_DEC_HALF_INTERNAL = 0,
    RKMPP_DEC_PURE_EXTERNAL = 1,
};

static const AVRational mpp_tb = { 1, 1000000 };

#define PTS_TO_MPP_PTS(pts, pts_tb) ((pts_tb.num && pts_tb.den) ? \
    av_rescale_q(pts, pts_tb, mpp_tb) : pts)

#define MPP_PTS_TO_PTS(mpp_pts, pts_tb) ((pts_tb.num && pts_tb.den) ? \
    av_rescale_q(mpp_pts, mpp_tb, pts_tb) : mpp_pts)

#define OFFSET(x) offsetof(RKMPPDecContext, x)
#define VD (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption options[] = {
    { "deint",      "Enable IEP (Image Enhancement Processor) for de-interlacing", OFFSET(deint), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VD },
    { "afbc",       "Enable AFBC (Arm Frame Buffer Compression) to save bandwidth", OFFSET(afbc), AV_OPT_TYPE_INT, { .i64 = RKMPP_DEC_AFBC_OFF }, 0, 2, VD, "afbc" },
        { "off",    "Disable AFBC support",                    0, AV_OPT_TYPE_CONST, { .i64 = RKMPP_DEC_AFBC_OFF    }, 0, 0, VD, "afbc" },
        { "on",     "Enable AFBC support",                     0, AV_OPT_TYPE_CONST, { .i64 = RKMPP_DEC_AFBC_ON     }, 0, 0, VD, "afbc" },
        { "rga",    "Enable AFBC if capable RGA is available", 0, AV_OPT_TYPE_CONST, { .i64 = RKMPP_DEC_AFBC_ON_RGA }, 0, 0, VD, "afbc" },
    { "fast_parse", "Enable fast parsing to improve decoding parallelism", OFFSET(fast_parse), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VD },
    { "buf_mode",   "Set the buffer mode for MPP decoder", OFFSET(buf_mode), AV_OPT_TYPE_INT, { .i64 = RKMPP_DEC_HALF_INTERNAL }, 0, 1, VD, "buf_mode" },
        { "half",   "Half internal mode",                      0, AV_OPT_TYPE_CONST, { .i64 = RKMPP_DEC_HALF_INTERNAL }, 0, 0, VD, "buf_mode" },
        { "ext",    "Pure external mode",                      0, AV_OPT_TYPE_CONST, { .i64 = RKMPP_DEC_PURE_EXTERNAL }, 0, 0, VD, "buf_mode" },
    { NULL }
};

static const enum AVPixelFormat rkmpp_dec_pix_fmts[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV16,
    AV_PIX_FMT_NV15,
    AV_PIX_FMT_NV20,
    AV_PIX_FMT_DRM_PRIME,
    AV_PIX_FMT_NONE,
};

static const AVCodecHWConfigInternal *const rkmpp_dec_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public = {
            .pix_fmt     = AV_PIX_FMT_DRM_PRIME,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                           AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
                           AV_CODEC_HW_CONFIG_METHOD_INTERNAL,
            .device_type = AV_HWDEVICE_TYPE_RKMPP,
        },
        .hwaccel = NULL,
    },
    NULL
};

#define DEFINE_RKMPP_DECODER(x, X, bsf_name) \
static const AVClass x##_rkmpp_decoder_class = { \
    .class_name = #x "_rkmpp_decoder", \
    .item_name  = av_default_item_name, \
    .option     = options, \
    .version    = LIBAVUTIL_VERSION_INT, \
}; \
const FFCodec ff_##x##_rkmpp_decoder = { \
    .p.name         = #x "_rkmpp", \
    CODEC_LONG_NAME("Rockchip MPP (Media Process Platform) " #X " decoder"), \
    .p.type         = AVMEDIA_TYPE_VIDEO, \
    .p.id           = AV_CODEC_ID_##X, \
    .priv_data_size = sizeof(RKMPPDecContext), \
    .p.priv_class   = &x##_rkmpp_decoder_class, \
    .init           = rkmpp_decode_init, \
    .close          = rkmpp_decode_close, \
    FF_CODEC_RECEIVE_FRAME_CB(rkmpp_decode_receive_frame), \
    .flush          = rkmpp_decode_flush, \
    .bsfs           = bsf_name, \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | \
                      AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | \
                      FF_CODEC_CAP_SETS_FRAME_PROPS, \
    .p.pix_fmts     = rkmpp_dec_pix_fmts, \
    .hw_configs     = rkmpp_dec_hw_configs, \
    .p.wrapper_name = "rkmpp", \
};

#endif /* AVCODEC_RKMPPDEC_H */
