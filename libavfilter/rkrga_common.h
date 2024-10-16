/*
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
 * Rockchip RGA (2D Raster Graphic Acceleration) base function
 */

#ifndef AVFILTER_RKRGA_COMMON_H
#define AVFILTER_RKRGA_COMMON_H

#include <rga/RgaApi.h>
#include <rga/im2d.h>

#include "avfilter.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_rkmpp.h"

#define RK_RGA_YUV_ALIGN                2
#define RK_RGA_AFBC_16x16_STRIDE_ALIGN  16
#define RK_RGA_RFBC_64x4_STRIDE_ALIGN_W 64
#define RK_RGA_RFBC_64x4_STRIDE_ALIGN_H 4

#define ALIGN_DOWN(a, b) ((a) & ~((b)-1))
#define FF_INLINK_IDX(link)  ((int)((link)->dstpad - (link)->dst->input_pads))
#define FF_OUTLINK_IDX(link) ((int)((link)->srcpad - (link)->src->output_pads))

typedef struct RGAFrame {
    AVFrame          *frame;
    rga_info_t        info;
    struct RGAFrame  *next;
    int               queued;
    int               locked;
} RGAFrame;

typedef struct RGAFrameInfo {
    enum _Rga_SURF_FORMAT     rga_fmt;
    enum AVPixelFormat        pix_fmt;
    const AVPixFmtDescriptor *pix_desc;
    float                     bytes_pp;
    int                       act_x;
    int                       act_y;
    int                       act_w;
    int                       act_h;
    int                       uncompact_10b_msb;
    int                       rotate_mode;
    int                       blend_mode;
    int                       crop;
    int                       scheduler_core;
    int                       overlay_x;
    int                       overlay_y;
} RGAFrameInfo;

typedef struct RKRGAContext {
    const AVClass      *class;

    int (*filter_frame) (AVFilterLink *outlink, AVFrame *frame);
    enum AVPixelFormat  out_sw_format;

    RGAFrame           *src_frame_list;
    RGAFrame           *dst_frame_list;
    RGAFrame           *pat_frame_list;

    AVBufferRef        *pat_preproc_hwframes_ctx;
    RGAFrame           *pat_preproc_frame_list;

    RGAFrameInfo       *in_rga_frame_infos;
    RGAFrameInfo        out_rga_frame_info;

    int scheduler_core;
    int async_depth;
    int afbc_out;

    int has_rga2;
    int has_rga2l;
    int has_rga2e;
    int has_rga2p;
    int has_rga3;
    int is_rga2_used;
    int is_overlay_offset_valid;

    int eof;
    int got_frame;

    AVFifo *async_fifo;
} RKRGAContext;

typedef struct RKRGAParam {
    int (*filter_frame)(AVFilterLink *outlink, AVFrame *frame);

    enum AVPixelFormat out_sw_format;

    int in_rotate_mode;
    int in_global_alpha;

    int in_crop;
    int in_crop_x;
    int in_crop_y;
    int in_crop_w;
    int in_crop_h;

    int overlay_x;
    int overlay_y;
} RKRGAParam;

int ff_rkrga_init(AVFilterContext *avctx, RKRGAParam *param);
int ff_rkrga_close(AVFilterContext *avctx);
int ff_rkrga_filter_frame(RKRGAContext *r,
                          AVFilterLink *inlink_src, AVFrame *picref_src,
                          AVFilterLink *inlink_pat, AVFrame *picref_pat);

#endif /* AVFILTER_RKRGA_COMMON_H */
