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

#include "libavutil/common.h"
#include "libavutil/pixdesc.h"

#include "internal.h"
#include "video.h"

#include "rkrga_common.h"

typedef struct RGAAsyncFrame {
    RGAFrame *src;
    RGAFrame *dst;
    RGAFrame *pat;
} RGAAsyncFrame;

typedef struct RGAFormatMap {
    enum AVPixelFormat    pix_fmt;
    enum _Rga_SURF_FORMAT rga_fmt;
} RGAFormatMap;

#define RK_FORMAT_YCbCr_444_SP (0x32 << 8)
#define RK_FORMAT_YCrCb_444_SP (0x33 << 8)

#define YUV_FORMATS \
    { AV_PIX_FMT_GRAY8,    RK_FORMAT_YCbCr_400 },        /* RGA2 only */ \
    { AV_PIX_FMT_YUV420P,  RK_FORMAT_YCbCr_420_P },      /* RGA2 only */ \
    { AV_PIX_FMT_YUVJ420P, RK_FORMAT_YCbCr_420_P },      /* RGA2 only */ \
    { AV_PIX_FMT_YUV422P,  RK_FORMAT_YCbCr_422_P },      /* RGA2 only */ \
    { AV_PIX_FMT_YUVJ422P, RK_FORMAT_YCbCr_422_P },      /* RGA2 only */ \
    { AV_PIX_FMT_NV12,     RK_FORMAT_YCbCr_420_SP }, \
    { AV_PIX_FMT_NV21,     RK_FORMAT_YCrCb_420_SP }, \
    { AV_PIX_FMT_NV16,     RK_FORMAT_YCbCr_422_SP }, \
    { AV_PIX_FMT_NV24,     RK_FORMAT_YCbCr_444_SP },     /* RGA2-Pro only */ \
    { AV_PIX_FMT_NV42,     RK_FORMAT_YCrCb_444_SP },     /* RGA2-Pro only */ \
    { AV_PIX_FMT_P010,     RK_FORMAT_YCbCr_420_SP_10B }, /* RGA3 only */ \
    { AV_PIX_FMT_P210,     RK_FORMAT_YCbCr_422_SP_10B }, /* RGA3 only */ \
    { AV_PIX_FMT_NV15,     RK_FORMAT_YCbCr_420_SP_10B }, /* RGA2 only input, aka P010 compact */ \
    { AV_PIX_FMT_NV20,     RK_FORMAT_YCbCr_422_SP_10B }, /* RGA2 only input, aka P210 compact */ \
    { AV_PIX_FMT_YUYV422,  RK_FORMAT_YUYV_422 }, \
    { AV_PIX_FMT_YVYU422,  RK_FORMAT_YVYU_422 }, \
    { AV_PIX_FMT_UYVY422,  RK_FORMAT_UYVY_422 },

#define RGB_FORMATS \
    { AV_PIX_FMT_RGB555LE, RK_FORMAT_BGRA_5551 },        /* RGA2 only */ \
    { AV_PIX_FMT_BGR555LE, RK_FORMAT_RGBA_5551 },        /* RGA2 only */ \
    { AV_PIX_FMT_RGB565LE, RK_FORMAT_BGR_565 }, \
    { AV_PIX_FMT_BGR565LE, RK_FORMAT_RGB_565 }, \
    { AV_PIX_FMT_RGB24,    RK_FORMAT_RGB_888 }, \
    { AV_PIX_FMT_BGR24,    RK_FORMAT_BGR_888 }, \
    { AV_PIX_FMT_RGBA,     RK_FORMAT_RGBA_8888 }, \
    { AV_PIX_FMT_RGB0,     RK_FORMAT_RGBA_8888 },        /* RK_FORMAT_RGBX_8888 triggers RGA2 on multicore RGA */ \
    { AV_PIX_FMT_BGRA,     RK_FORMAT_BGRA_8888 }, \
    { AV_PIX_FMT_BGR0,     RK_FORMAT_BGRA_8888 },        /* RK_FORMAT_BGRX_8888 triggers RGA2 on multicore RGA */ \
    { AV_PIX_FMT_ARGB,     RK_FORMAT_ARGB_8888 },        /* RGA3 only input */ \
    { AV_PIX_FMT_0RGB,     RK_FORMAT_ARGB_8888 },        /* RGA3 only input */ \
    { AV_PIX_FMT_ABGR,     RK_FORMAT_ABGR_8888 },        /* RGA3 only input */ \
    { AV_PIX_FMT_0BGR,     RK_FORMAT_ABGR_8888 },        /* RGA3 only input */

static const RGAFormatMap supported_formats_main[] = {
    YUV_FORMATS
    RGB_FORMATS
};

static const RGAFormatMap supported_formats_overlay[] = {
    RGB_FORMATS
};
#undef YUV_FORMATS
#undef RGB_FORMATS

static int map_av_to_rga_format(enum AVPixelFormat in_format,
                                enum _Rga_SURF_FORMAT *out_format, int is_overlay)
{
    int i;

    if (is_overlay)
        goto overlay;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats_main); i++) {
        if (supported_formats_main[i].pix_fmt == in_format) {
            if (out_format)
                *out_format = supported_formats_main[i].rga_fmt;
            return 1;
        }
    }
    return 0;

overlay:
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats_overlay); i++) {
        if (supported_formats_overlay[i].pix_fmt == in_format) {
            if (out_format)
                *out_format = supported_formats_overlay[i].rga_fmt;
            return 1;
        }
    }
    return 0;
}

static int get_pixel_stride(const AVDRMObjectDescriptor *object,
                            const AVDRMLayerDescriptor *layer,
                            int is_rgb, int is_planar,
                            float bytes_pp, int *ws, int *hs)
{
    const AVDRMPlaneDescriptor *plane0, *plane1;
    const int is_packed_fmt = is_rgb || (!is_rgb && !is_planar);

    if (!object || !layer || !ws || !hs || bytes_pp <= 0)
        return AVERROR(EINVAL);

    plane0 = &layer->planes[0];
    plane1 = &layer->planes[1];

    *ws = is_packed_fmt ?
        (plane0->pitch / bytes_pp) :
        plane0->pitch;
    *hs = is_packed_fmt ?
        ALIGN_DOWN(object->size / plane0->pitch, is_rgb ? 1 : 2) :
        (plane1->offset / plane0->pitch);

    return (*ws > 0 && *hs > 0) ? 0 : AVERROR(EINVAL);
}

static int get_afbc_pixel_stride(float bytes_pp, int *stride, int reverse)
{
    if (!stride || *stride <= 0 || bytes_pp <= 0)
        return AVERROR(EINVAL);

    *stride = reverse ? (*stride / bytes_pp) : (*stride * bytes_pp);

    return (*stride > 0) ? 0 : AVERROR(EINVAL);
}

/* Canonical formats: https://dri.freedesktop.org/docs/drm/gpu/afbc.html */
static uint32_t get_drm_afbc_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_NV12:     return DRM_FORMAT_YUV420_8BIT;
    case AV_PIX_FMT_NV15:     return DRM_FORMAT_YUV420_10BIT;
    case AV_PIX_FMT_NV16:     return DRM_FORMAT_YUYV;
    case AV_PIX_FMT_NV20:     return DRM_FORMAT_Y210;
    case AV_PIX_FMT_NV24:     return DRM_FORMAT_VUY888;
    case AV_PIX_FMT_RGB565LE: return DRM_FORMAT_RGB565;
    case AV_PIX_FMT_BGR565LE: return DRM_FORMAT_BGR565;
    case AV_PIX_FMT_RGB24:    return DRM_FORMAT_RGB888;
    case AV_PIX_FMT_BGR24:    return DRM_FORMAT_BGR888;
    case AV_PIX_FMT_RGBA:     return DRM_FORMAT_ABGR8888;
    case AV_PIX_FMT_RGB0:     return DRM_FORMAT_XBGR8888;
    case AV_PIX_FMT_BGRA:     return DRM_FORMAT_ARGB8888;
    case AV_PIX_FMT_BGR0:     return DRM_FORMAT_XRGB8888;
    default:                  return DRM_FORMAT_INVALID;
    }
}

static uint32_t get_drm_rfbc_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_NV12:     return DRM_FORMAT_YUV420_8BIT;
    case AV_PIX_FMT_NV15:     return DRM_FORMAT_YUV420_10BIT;
    case AV_PIX_FMT_NV16:     return DRM_FORMAT_YUYV;
    case AV_PIX_FMT_NV20:     return DRM_FORMAT_Y210;
    case AV_PIX_FMT_NV24:     return DRM_FORMAT_VUY888;
    default:                  return DRM_FORMAT_INVALID;
    }
}

static int is_pixel_stride_rga3_compat(int ws, int hs,
                                       enum _Rga_SURF_FORMAT fmt)
{
    switch (fmt) {
    case RK_FORMAT_YCbCr_420_SP:
    case RK_FORMAT_YCrCb_420_SP:
    case RK_FORMAT_YCbCr_422_SP:     return !(ws % 16) && !(hs % 2);
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_422_SP_10B: return !(ws % 64) && !(hs % 2);
    case RK_FORMAT_YUYV_422:
    case RK_FORMAT_YVYU_422:
    case RK_FORMAT_UYVY_422:         return !(ws % 8) && !(hs % 2);
    case RK_FORMAT_RGB_565:
    case RK_FORMAT_BGR_565:          return !(ws % 8);
    case RK_FORMAT_RGB_888:
    case RK_FORMAT_BGR_888:          return !(ws % 16);
    case RK_FORMAT_RGBA_8888:
    case RK_FORMAT_BGRA_8888:
    case RK_FORMAT_ARGB_8888:
    case RK_FORMAT_ABGR_8888:        return !(ws % 4);
    default:                         return 0;
    }
}

static void clear_unused_frames(RGAFrame *list)
{
    while (list) {
        if (list->queued == 1 && !list->locked) {
            av_frame_free(&list->frame);
            list->queued = 0;
        }
        list = list->next;
    }
}

static void clear_frame_list(RGAFrame **list)
{
    while (*list) {
        RGAFrame *frame = NULL;

        frame = *list;
        *list = (*list)->next;
        av_frame_free(&frame->frame);
        av_freep(&frame);
    }
}

static RGAFrame *get_free_frame(RGAFrame **list)
{
    RGAFrame *out = *list;

    for (; out; out = out->next) {
        if (!out->queued) {
            out->queued = 1;
            break;
        }
    }

    if (!out) {
        out = av_mallocz(sizeof(*out));
        if (!out) {
            av_log(NULL, AV_LOG_ERROR, "Cannot alloc new output frame\n");
            return NULL;
        }
        out->queued = 1;
        out->next   = *list;
        *list       = out;
    }

    return out;
}

static void set_colorspace_info(RGAFrameInfo *in_info, const AVFrame *in,
                                RGAFrameInfo *out_info, AVFrame *out,
                                int *color_space_mode)
{
    if (!in_info || !out_info || !in || !out || !color_space_mode)
        return;

    *color_space_mode = 0;

    /* rgb2yuv */
    if ((in_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB) &&
        !(out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        /* rgb full -> yuv full/limit */
        if (in->color_range == AVCOL_RANGE_JPEG) {
            switch (in->colorspace) {
            case AVCOL_SPC_BT709:
                out->colorspace   = AVCOL_SPC_BT709;
                *color_space_mode = 0xb << 8; /* rgb2yuv_709_limit */
                break;
            case AVCOL_SPC_BT470BG:
                out->colorspace   = AVCOL_SPC_BT470BG;
                *color_space_mode = 2 << 2; /* IM_RGB_TO_YUV_BT601_LIMIT */
                break;
            }
        }
        if (*color_space_mode) {
            out->color_trc       = AVCOL_TRC_UNSPECIFIED;
            out->color_primaries = AVCOL_PRI_UNSPECIFIED;
            out->color_range     = AVCOL_RANGE_MPEG;
        }
    }

    /* yuv2rgb */
    if (!(in_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB) &&
        (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        /* yuv full/limit -> rgb full */
        switch (in->color_range) {
        case AVCOL_RANGE_MPEG:
            if (in->colorspace == AVCOL_SPC_BT709) {
                out->colorspace   = AVCOL_SPC_BT709;
                *color_space_mode = 3 << 0; /* IM_YUV_TO_RGB_BT709_LIMIT */
            }
            if (in->colorspace == AVCOL_SPC_BT470BG) {
                out->colorspace   = AVCOL_SPC_BT470BG;
                *color_space_mode = 1 << 0; /* IM_YUV_TO_RGB_BT601_LIMIT */
            }
            break;
        case AVCOL_RANGE_JPEG:
#if 0
            if (in->colorspace == AVCOL_SPC_BT709) {
                out->colorspace   = AVCOL_SPC_BT709;
                *color_space_mode = 0xc << 8; /* yuv2rgb_709_full */
            }
#endif
            if (in->colorspace == AVCOL_SPC_BT470BG) {
                out->colorspace   = AVCOL_SPC_BT470BG;
                *color_space_mode = 2 << 0; /* IM_YUV_TO_RGB_BT601_FULL */
            }
            break;
        }
        if (*color_space_mode) {
            out->color_trc       = AVCOL_TRC_UNSPECIFIED;
            out->color_primaries = AVCOL_PRI_UNSPECIFIED;
            out->color_range     = AVCOL_RANGE_JPEG;
        }
    }

    /* yuvj2yuv */
    if ((in_info->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         in_info->pix_fmt == AV_PIX_FMT_YUVJ422P) &&
        !(out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        out->color_range = AVCOL_RANGE_JPEG;
    }
}

static int verify_rga_frame_info_io_dynamic(AVFilterContext *avctx,
                                            RGAFrameInfo *in, RGAFrameInfo *out)
{
    RKRGAContext *r = avctx->priv;

    if (!in || !out)
        return AVERROR(EINVAL);

    if (r->is_rga2_used && !r->has_rga2) {
        av_log(avctx, AV_LOG_ERROR, "RGA2 is requested but not available\n");
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used &&
        (in->pix_fmt == AV_PIX_FMT_P010 ||
         out->pix_fmt == AV_PIX_FMT_P010)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P010));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used &&
        (in->pix_fmt == AV_PIX_FMT_P210 ||
         out->pix_fmt == AV_PIX_FMT_P210)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P210));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used &&
        (out->pix_fmt == AV_PIX_FMT_NV15 ||
         out->pix_fmt == AV_PIX_FMT_NV20)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as output is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(out->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (!r->has_rga2p && r->is_rga2_used && in->crop && in->pix_desc->comp[0].depth >= 10) {
        av_log(avctx, AV_LOG_ERROR, "Cropping 10-bit '%s' input is not supported if RGA2 (non-Pro) is requested\n",
               av_get_pix_fmt_name(in->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used && !r->has_rga2p &&
        (out->act_w > 4096 || out->act_h > 4096)) {
        av_log(avctx, AV_LOG_ERROR, "Max supported output size of RGA2 (non-Pro) is 4096x4096\n");
        return AVERROR(EINVAL);
    }
    if (!r->is_rga2_used &&
        (in->act_w < 68 || in->act_h < 2)) {
        av_log(avctx, AV_LOG_ERROR, "Min supported input size of RGA3 is 68x2\n");
        return AVERROR(EINVAL);
    }
    if (!r->is_rga2_used &&
        (out->act_w > 8128 || out->act_h > 8128)) {
        av_log(avctx, AV_LOG_ERROR, "Max supported output size of RGA3 is 8128x8128\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static RGAFrame *submit_frame(RKRGAContext *r, AVFilterLink *inlink,
                              AVFrame *picref, int do_overlay, int pat_preproc)
{
    RGAFrame        *rga_frame;
    AVFilterContext *ctx = inlink->dst;
    rga_info_t info = { .mmuFlag = 1, };
    int nb_link = FF_INLINK_IDX(inlink);
    RGAFrameInfo *in_info = &r->in_rga_frame_infos[nb_link];
    RGAFrameInfo *out_info = &r->out_rga_frame_info;
    int w_stride = 0, h_stride = 0;
    const AVDRMFrameDescriptor *desc;
    const AVDRMLayerDescriptor *layer;
    const AVDRMPlaneDescriptor *plane0;
    RGAFrame **frame_list = NULL;
    int is_afbc = 0, is_rfbc = 0;
    int ret, is_fbc = 0;

    if (pat_preproc && !nb_link)
        return NULL;

    frame_list = nb_link ?
        (pat_preproc ? &r->pat_preproc_frame_list : &r->pat_frame_list) : &r->src_frame_list;

    clear_unused_frames(*frame_list);

    rga_frame = get_free_frame(frame_list);
    if (!rga_frame)
        return NULL;

    if (picref->format != AV_PIX_FMT_DRM_PRIME) {
        av_log(ctx, AV_LOG_ERROR, "RGA gets a wrong frame\n");
        return NULL;
    }
    rga_frame->frame = av_frame_clone(picref);

    desc = (AVDRMFrameDescriptor *)rga_frame->frame->data[0];
    if (desc->objects[0].fd < 0)
        return NULL;

    is_afbc = drm_is_afbc(desc->objects[0].format_modifier);
    is_rfbc = drm_is_rfbc(desc->objects[0].format_modifier);
    is_fbc = is_afbc || is_rfbc;
    if (!is_fbc) {
        ret = get_pixel_stride(&desc->objects[0],
                               &desc->layers[0],
                               (in_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                               (in_info->pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                               in_info->bytes_pp, &w_stride, &h_stride);
        if (ret < 0 || !w_stride || !h_stride) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            return NULL;
        }
    }

    info.fd           = desc->objects[0].fd;
    info.format       = in_info->rga_fmt;
    info.in_fence_fd  = -1;
    info.out_fence_fd = -1;

    if (in_info->uncompact_10b_msb)
        info.is_10b_compact = info.is_10b_endian = 1;

    if (!nb_link) {
        info.rotation = in_info->rotate_mode;
        info.blend    = (do_overlay && !pat_preproc) ? in_info->blend_mode : 0;
    }

    if (is_fbc && !r->has_rga2p && (r->is_rga2_used || out_info->scheduler_core == 0x4)) {
        av_log(ctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported by RGA2 (non-Pro)\n",
               av_get_pix_fmt_name(in_info->pix_fmt));
        return NULL;
    }

    /* verify inputs pixel stride */
    if (out_info->scheduler_core > 0 &&
        out_info->scheduler_core == (out_info->scheduler_core & 0x3)) {
        if (!is_afbc && !is_pixel_stride_rga3_compat(w_stride, h_stride, in_info->rga_fmt)) {
            r->is_rga2_used = 1;
            av_log(ctx, AV_LOG_WARNING, "Input pixel stride (%dx%d) format '%s' is not supported by RGA3\n",
                   w_stride, h_stride, av_get_pix_fmt_name(in_info->pix_fmt));
        }

        if ((ret = verify_rga_frame_info_io_dynamic(ctx, in_info, out_info)) < 0)
            return NULL;

        if (r->is_rga2_used)
            out_info->scheduler_core = 0x4;
    }

    if (pat_preproc) {
        RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
        rga_set_rect(&info.rect, 0, 0,
                     FFMIN((in0_info->act_w - in_info->overlay_x), in_info->act_w),
                     FFMIN((in0_info->act_h - in_info->overlay_y), in_info->act_h),
                     w_stride, h_stride, in_info->rga_fmt);
    } else
        rga_set_rect(&info.rect, in_info->act_x, in_info->act_y,
                     in_info->act_w, in_info->act_h,
                     w_stride, h_stride, in_info->rga_fmt);

    if (is_fbc) {
        int afbc_offset_y = 0;
        int fbc_align_w =
            is_afbc ? RK_RGA_AFBC_16x16_STRIDE_ALIGN : RK_RGA_RFBC_64x4_STRIDE_ALIGN_W;
        int fbc_align_h =
            is_afbc ? RK_RGA_AFBC_16x16_STRIDE_ALIGN : RK_RGA_RFBC_64x4_STRIDE_ALIGN_H;
        uint32_t drm_fbc_fmt =
            is_afbc ? get_drm_afbc_format(in_info->pix_fmt) : get_drm_rfbc_format(in_info->pix_fmt);

        if (rga_frame->frame->crop_top > 0) {
            afbc_offset_y = is_afbc ? rga_frame->frame->crop_top : 0;
            info.rect.yoffset += afbc_offset_y;
        }

        layer = &desc->layers[0];
        plane0 = &layer->planes[0];
        if (drm_fbc_fmt == layer->format) {
            info.rect.wstride = plane0->pitch;
            if ((ret = get_afbc_pixel_stride(in_info->bytes_pp, &info.rect.wstride, 1)) < 0)
                return NULL;

            if (info.rect.wstride % fbc_align_w)
                info.rect.wstride = FFALIGN(inlink->w, fbc_align_w);

            info.rect.hstride = FFALIGN(inlink->h + afbc_offset_y, fbc_align_h);
        } else {
            av_log(ctx, AV_LOG_ERROR, "Input format '%s' with AFBC/RFBC modifier is not supported\n",
                   av_get_pix_fmt_name(in_info->pix_fmt));
            return NULL;
        }

        info.rd_mode =
            is_afbc ? (1 << 1)  /* IM_AFBC16x16_MODE */
                    : (1 << 4); /* IM_RKFBC64x4_MODE */
    }

    rga_frame->info = info;

    return rga_frame;
}

static RGAFrame *query_frame(RKRGAContext *r, AVFilterLink *outlink,
                             const AVFrame *in, int pat_preproc)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    RGAFrame        *out_frame;
    rga_info_t info = { .mmuFlag = 1, };
    RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
    RGAFrameInfo *in1_info = ctx->nb_inputs > 1 ? &r->in_rga_frame_infos[1] : NULL;
    RGAFrameInfo *out_info = pat_preproc ? in1_info : &r->out_rga_frame_info;
    AVBufferRef *hw_frame_ctx = pat_preproc ? r->pat_preproc_hwframes_ctx : outlink->hw_frames_ctx;
    int w_stride = 0, h_stride = 0;
    AVDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor *layer;
    RGAFrame **frame_list = NULL;
    int ret, is_afbc = 0;

    if (!out_info || !hw_frame_ctx)
        return NULL;

    frame_list = pat_preproc ? &r->pat_frame_list : &r->dst_frame_list;

    clear_unused_frames(*frame_list);

    out_frame = get_free_frame(frame_list);
    if (!out_frame)
        return NULL;

    out_frame->frame = av_frame_alloc();
    if (!out_frame->frame)
        return NULL;

    if (in && (ret = av_frame_copy_props(out_frame->frame, in)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to copy metadata fields from in to out: %d\n", ret);
        goto fail;
    }
    out_frame->frame->crop_top = 0;

    if ((ret = av_hwframe_get_buffer(hw_frame_ctx, out_frame->frame, 0)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot allocate an internal frame: %d\n", ret);
        goto fail;
    }

    desc = (AVDRMFrameDescriptor *)out_frame->frame->data[0];
    if (desc->objects[0].fd < 0)
        goto fail;

    if (r->is_rga2_used || out_info->scheduler_core == 0x4) {
        if (!r->has_rga2p && pat_preproc && (info.rect.width > 4096 || info.rect.height > 4096)) {
            av_log(ctx, AV_LOG_ERROR, "Max supported output size of RGA2 (non-Pro) is 4096x4096\n");
            goto fail;
        }
        if (r->afbc_out && !pat_preproc) {
            av_log(ctx, AV_LOG_WARNING, "Output format '%s' with AFBC modifier is not supported by RGA2\n",
                   av_get_pix_fmt_name(out_info->pix_fmt));
            r->afbc_out = 0;
        }
    }

    is_afbc = r->afbc_out && !pat_preproc;
    ret = get_pixel_stride(&desc->objects[0],
                           &desc->layers[0],
                           (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                           (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                           out_info->bytes_pp, &w_stride, &h_stride);
    if (!is_afbc && (ret < 0 || !w_stride || !h_stride)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get frame strides\n");
        goto fail;
    }

    info.fd           = desc->objects[0].fd;
    info.format       = out_info->rga_fmt;
    info.core         = out_info->scheduler_core;
    info.in_fence_fd  = -1;
    info.out_fence_fd = -1;
    info.sync_mode    = RGA_BLIT_ASYNC;

    if (out_info->uncompact_10b_msb)
        info.is_10b_compact = info.is_10b_endian = 1;

    if (!pat_preproc)
        set_colorspace_info(in0_info, in, out_info, out_frame->frame, &info.color_space_mode);

    if (pat_preproc)
        rga_set_rect(&info.rect, in1_info->overlay_x, in1_info->overlay_y,
                     FFMIN((in0_info->act_w - in1_info->overlay_x), in1_info->act_w),
                     FFMIN((in0_info->act_h - in1_info->overlay_y), in1_info->act_h),
                     w_stride, h_stride, in1_info->rga_fmt);
    else
        rga_set_rect(&info.rect, out_info->act_x, out_info->act_y,
                     out_info->act_w, out_info->act_h,
                     w_stride, h_stride, out_info->rga_fmt);

    if (is_afbc) {
        uint32_t drm_afbc_fmt = get_drm_afbc_format(out_info->pix_fmt);

        if (drm_afbc_fmt == DRM_FORMAT_INVALID) {
            av_log(ctx, AV_LOG_WARNING, "Output format '%s' with AFBC modifier is not supported\n",
                   av_get_pix_fmt_name(out_info->pix_fmt));
            r->afbc_out = 0;
            goto exit;
        }

        w_stride = FFALIGN(pat_preproc ? inlink->w : outlink->w, RK_RGA_AFBC_16x16_STRIDE_ALIGN);
        h_stride = FFALIGN(pat_preproc ? inlink->h : outlink->h, RK_RGA_AFBC_16x16_STRIDE_ALIGN);

        if ((info.rect.format == RK_FORMAT_YCbCr_420_SP_10B ||
             info.rect.format == RK_FORMAT_YCbCr_422_SP_10B) && (w_stride % 64)) {
            av_log(ctx, AV_LOG_WARNING, "Output pixel wstride '%d' format '%s' is not supported by RGA3 AFBC\n",
                   w_stride, av_get_pix_fmt_name(out_info->pix_fmt));
            r->afbc_out = 0;
            goto exit;
        }

        /* Inverted RGB/BGR order in FBCE */
        switch (info.rect.format) {
        case RK_FORMAT_RGBA_8888:
            info.rect.format = RK_FORMAT_BGRA_8888;
            break;
        case RK_FORMAT_BGRA_8888:
            info.rect.format = RK_FORMAT_RGBA_8888;
            break;
        }

        info.rect.wstride = w_stride;
        info.rect.hstride = h_stride;
        info.rd_mode = 1 << 1; /* IM_AFBC16x16_MODE */

        desc->objects[0].format_modifier =
            DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);

        layer = &desc->layers[0];
        layer->format = drm_afbc_fmt;
        layer->nb_planes = 1;

        layer->planes[0].offset = 0;
        layer->planes[0].pitch  = info.rect.wstride;

        if ((ret = get_afbc_pixel_stride(out_info->bytes_pp, (int *)&layer->planes[0].pitch, 0)) < 0)
            goto fail;
    }

exit:
    out_frame->info = info;

    return out_frame;

fail:
    if (out_frame && out_frame->frame)
        av_frame_free(&out_frame->frame);

    return NULL;
}

static av_cold int init_hwframes_ctx(AVFilterContext *avctx)
{
    RKRGAContext      *r       = avctx->priv;
    AVFilterLink      *inlink  = avctx->inputs[0];
    AVFilterLink      *outlink = avctx->outputs[0];
    AVHWFramesContext *hwfc_in;
    AVHWFramesContext *hwfc_out;
    AVBufferRef       *hwfc_out_ref;
    AVHWDeviceContext *device_ctx;
    AVRKMPPFramesContext *rkmpp_fc;
    int                ret;

    if (!inlink->hw_frames_ctx)
        return AVERROR(EINVAL);

    hwfc_in = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
    device_ctx = (AVHWDeviceContext *)hwfc_in->device_ref->data;

    if (!device_ctx || device_ctx->type != AV_HWDEVICE_TYPE_RKMPP)
        return AVERROR(EINVAL);

    hwfc_out_ref = av_hwframe_ctx_alloc(hwfc_in->device_ref);
    if (!hwfc_out_ref)
        return AVERROR(ENOMEM);

    hwfc_out = (AVHWFramesContext *)hwfc_out_ref->data;
    hwfc_out->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc_out->sw_format = r->out_sw_format;
    hwfc_out->width     = outlink->w;
    hwfc_out->height    = outlink->h;

    rkmpp_fc = hwfc_out->hwctx;
    rkmpp_fc->flags |= MPP_BUFFER_FLAGS_CACHABLE;

    ret = av_hwframe_ctx_init(hwfc_out_ref);
    if (ret < 0) {
        av_buffer_unref(&hwfc_out_ref);
        av_log(avctx, AV_LOG_ERROR, "Error creating frames_ctx for output pad: %d\n", ret);
        return ret;
    }

    av_buffer_unref(&outlink->hw_frames_ctx);
    outlink->hw_frames_ctx = hwfc_out_ref;

    return 0;
}

static av_cold int init_pat_preproc_hwframes_ctx(AVFilterContext *avctx)
{
    RKRGAContext      *r = avctx->priv;
    AVFilterLink      *inlink0 = avctx->inputs[0];
    AVFilterLink      *inlink1 = avctx->inputs[1];
    AVHWFramesContext *hwfc_in0, *hwfc_in1;
    AVHWFramesContext *hwfc_pat;
    AVBufferRef       *hwfc_pat_ref;
    AVHWDeviceContext *device_ctx0;
    int                ret;

    if (!inlink0->hw_frames_ctx || !inlink1->hw_frames_ctx)
        return AVERROR(EINVAL);

    hwfc_in0 = (AVHWFramesContext *)inlink0->hw_frames_ctx->data;
    hwfc_in1 = (AVHWFramesContext *)inlink1->hw_frames_ctx->data;
    device_ctx0 = (AVHWDeviceContext *)hwfc_in0->device_ref->data;

    if (!device_ctx0 || device_ctx0->type != AV_HWDEVICE_TYPE_RKMPP)
        return AVERROR(EINVAL);

    hwfc_pat_ref = av_hwframe_ctx_alloc(hwfc_in0->device_ref);
    if (!hwfc_pat_ref)
        return AVERROR(ENOMEM);

    hwfc_pat = (AVHWFramesContext *)hwfc_pat_ref->data;
    hwfc_pat->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc_pat->sw_format = hwfc_in1->sw_format;
    hwfc_pat->width     = inlink0->w;
    hwfc_pat->height    = inlink0->h;

    ret = av_hwframe_ctx_init(hwfc_pat_ref);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error creating frames_ctx for pat preproc: %d\n", ret);
        av_buffer_unref(&hwfc_pat_ref);
        return ret;
    }

    av_buffer_unref(&r->pat_preproc_hwframes_ctx);
    r->pat_preproc_hwframes_ctx = hwfc_pat_ref;

    return 0;
}

static av_cold int verify_rga_frame_info(AVFilterContext *avctx,
                                         RGAFrameInfo *src, RGAFrameInfo *dst, RGAFrameInfo *pat)
{
    RKRGAContext *r = avctx->priv;
    float scale_ratio_min, scale_ratio_max;
    float scale_ratio_w, scale_ratio_h;
    int ret;

    if (!src || !dst)
        return AVERROR(EINVAL);

    scale_ratio_w = (float)dst->act_w / (float)src->act_w;
    scale_ratio_h = (float)dst->act_h / (float)src->act_h;

    /* P010 requires RGA3 */
    if (!r->has_rga3 &&
        (src->pix_fmt == AV_PIX_FMT_P010 ||
         dst->pix_fmt == AV_PIX_FMT_P010)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is only supported by RGA3\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P010));
        return AVERROR(ENOSYS);
    }
    /* P210 requires RGA3 */
    if (!r->has_rga3 &&
        (src->pix_fmt == AV_PIX_FMT_P210 ||
         dst->pix_fmt == AV_PIX_FMT_P210)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is only supported by RGA3\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P210));
        return AVERROR(ENOSYS);
    }
    /* NV24/NV42 requires RGA2-Pro */
    if (!r->has_rga2p &&
        (src->pix_fmt == AV_PIX_FMT_NV24 ||
         src->pix_fmt == AV_PIX_FMT_NV42 ||
         dst->pix_fmt == AV_PIX_FMT_NV24 ||
         dst->pix_fmt == AV_PIX_FMT_NV42)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' and '%s' are only supported by RGA2-Pro\n",
               av_get_pix_fmt_name(AV_PIX_FMT_NV24),
               av_get_pix_fmt_name(AV_PIX_FMT_NV42));
        return AVERROR(ENOSYS);
    }
    /* Input formats that requires RGA2 */
    if (!r->has_rga2 &&
        (src->pix_fmt == AV_PIX_FMT_GRAY8 ||
         src->pix_fmt == AV_PIX_FMT_YUV420P ||
         src->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         src->pix_fmt == AV_PIX_FMT_YUV422P ||
         src->pix_fmt == AV_PIX_FMT_YUVJ422P ||
         src->pix_fmt == AV_PIX_FMT_RGB555LE ||
         src->pix_fmt == AV_PIX_FMT_BGR555LE)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as input is only supported by RGA2\n",
               av_get_pix_fmt_name(src->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* Output formats that requires RGA2 */
    if (!r->has_rga2 &&
        (dst->pix_fmt == AV_PIX_FMT_GRAY8 ||
         dst->pix_fmt == AV_PIX_FMT_YUV420P ||
         dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         dst->pix_fmt == AV_PIX_FMT_YUV422P ||
         dst->pix_fmt == AV_PIX_FMT_YUVJ422P ||
         dst->pix_fmt == AV_PIX_FMT_RGB555LE ||
         dst->pix_fmt == AV_PIX_FMT_BGR555LE ||
         dst->pix_fmt == AV_PIX_FMT_ARGB ||
         dst->pix_fmt == AV_PIX_FMT_0RGB ||
         dst->pix_fmt == AV_PIX_FMT_ABGR ||
         dst->pix_fmt == AV_PIX_FMT_0BGR)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as output is only supported by RGA2\n",
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* non-YUVJ format to YUVJ format is not supported */
    if ((dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         dst->pix_fmt == AV_PIX_FMT_YUVJ422P) &&
         (src->pix_fmt != AV_PIX_FMT_YUVJ420P &&
          src->pix_fmt != AV_PIX_FMT_YUVJ422P)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' to '%s' is not supported\n",
               av_get_pix_fmt_name(src->pix_fmt),
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* P010/P210 requires RGA3 but it can't handle certain formats */
    if ((src->pix_fmt == AV_PIX_FMT_P010 ||
         src->pix_fmt == AV_PIX_FMT_P210) &&
         (dst->pix_fmt == AV_PIX_FMT_GRAY8 ||
          dst->pix_fmt == AV_PIX_FMT_YUV420P ||
          dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
          dst->pix_fmt == AV_PIX_FMT_YUV422P ||
          dst->pix_fmt == AV_PIX_FMT_YUVJ422P ||
          dst->pix_fmt == AV_PIX_FMT_RGB555LE ||
          dst->pix_fmt == AV_PIX_FMT_BGR555LE ||
          dst->pix_fmt == AV_PIX_FMT_ARGB ||
          dst->pix_fmt == AV_PIX_FMT_0RGB ||
          dst->pix_fmt == AV_PIX_FMT_ABGR ||
          dst->pix_fmt == AV_PIX_FMT_0BGR)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' to '%s' is not supported\n",
               av_get_pix_fmt_name(src->pix_fmt),
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* RGA3 only format to RGA2 only format is not supported */
    if ((dst->pix_fmt == AV_PIX_FMT_P010 ||
         dst->pix_fmt == AV_PIX_FMT_P210) &&
         (src->pix_fmt == AV_PIX_FMT_GRAY8 ||
          src->pix_fmt == AV_PIX_FMT_YUV420P ||
          src->pix_fmt == AV_PIX_FMT_YUVJ420P ||
          src->pix_fmt == AV_PIX_FMT_YUV422P ||
          src->pix_fmt == AV_PIX_FMT_YUVJ422P ||
          src->pix_fmt == AV_PIX_FMT_RGB555LE ||
          src->pix_fmt == AV_PIX_FMT_BGR555LE)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' to '%s' is not supported\n",
               av_get_pix_fmt_name(src->pix_fmt),
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }

    if (src->pix_fmt == AV_PIX_FMT_GRAY8 ||
        src->pix_fmt == AV_PIX_FMT_YUV420P ||
        src->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        src->pix_fmt == AV_PIX_FMT_YUV422P ||
        src->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        src->pix_fmt == AV_PIX_FMT_NV24 ||
        src->pix_fmt == AV_PIX_FMT_NV42 ||
        src->pix_fmt == AV_PIX_FMT_RGB555LE ||
        src->pix_fmt == AV_PIX_FMT_BGR555LE ||
        dst->pix_fmt == AV_PIX_FMT_GRAY8 ||
        dst->pix_fmt == AV_PIX_FMT_YUV420P ||
        dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        dst->pix_fmt == AV_PIX_FMT_YUV422P ||
        dst->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        dst->pix_fmt == AV_PIX_FMT_NV24 ||
        dst->pix_fmt == AV_PIX_FMT_NV42 ||
        dst->pix_fmt == AV_PIX_FMT_RGB555LE ||
        dst->pix_fmt == AV_PIX_FMT_BGR555LE ||
        dst->pix_fmt == AV_PIX_FMT_ARGB ||
        dst->pix_fmt == AV_PIX_FMT_0RGB ||
        dst->pix_fmt == AV_PIX_FMT_ABGR ||
        dst->pix_fmt == AV_PIX_FMT_0BGR) {
        r->is_rga2_used = 1;
    }

    r->is_rga2_used = r->is_rga2_used || !r->has_rga3;
    if (r->has_rga3) {
        if (scale_ratio_w < 0.125f ||
            scale_ratio_w > 8.0f ||
            scale_ratio_h < 0.125f ||
            scale_ratio_h > 8.0f) {
            r->is_rga2_used = 1;
        }
        if (src->act_w < 68 ||
            src->act_w > 8176 ||
            src->act_h > 8176 ||
            dst->act_w < 68) {
            r->is_rga2_used = 1;
        }
        if (pat && (pat->act_w < 68 ||
             pat->act_w > 8176 ||
             pat->act_h > 8176)) {
            r->is_rga2_used = 1;
        }
    }

    if ((ret = verify_rga_frame_info_io_dynamic(avctx, src, dst)) < 0)
        return ret;

    if (r->is_rga2_used) {
        r->scheduler_core = 0x4;
        if (r->has_rga2p)
            r->scheduler_core |= 0x8;
    }

    /* Prioritize RGA3 on multicore RGA hw to avoid dma32 & algorithm quirks as much as possible */
    if (r->has_rga3 && r->has_rga2e && !r->is_rga2_used &&
        (r->scheduler_core == 0 || avctx->nb_inputs > 1 ||
         scale_ratio_w != 1.0f || scale_ratio_h != 1.0f ||
         src->crop || src->uncompact_10b_msb || dst->uncompact_10b_msb)) {
        r->scheduler_core = 0x3;
    }

    scale_ratio_max = 16.0f;
    if ((r->is_rga2_used && r->has_rga2l) ||
        (!r->is_rga2_used && r->has_rga3 && !r->has_rga2) ||
        (r->scheduler_core > 0 && r->scheduler_core == (r->scheduler_core & 0x3))) {
        scale_ratio_max = 8.0f;
    }
    scale_ratio_min = 1.0f / scale_ratio_max;

    if (scale_ratio_w < scale_ratio_min || scale_ratio_w > scale_ratio_max ||
        scale_ratio_h < scale_ratio_min || scale_ratio_h > scale_ratio_max) {
        av_log(avctx, AV_LOG_ERROR, "RGA scale ratio (%.04fx%.04f) exceeds %.04f ~ %.04f.\n",
               scale_ratio_w, scale_ratio_h, scale_ratio_min, scale_ratio_max);
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold int fill_rga_frame_info_by_link(AVFilterContext *avctx,
                                               RGAFrameInfo *info,
                                               AVFilterLink *link,
                                               int nb_link, int is_inlink)
{
    AVHWFramesContext *hwfc;
    RKRGAContext *r = avctx->priv;

    if (!link->hw_frames_ctx || link->format != AV_PIX_FMT_DRM_PRIME)
        return AVERROR(EINVAL);

    hwfc = (AVHWFramesContext *)link->hw_frames_ctx->data;

    if (!map_av_to_rga_format(hwfc->sw_format, &info->rga_fmt, (is_inlink && nb_link > 0))) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported '%s' pad %d format: '%s'\n",
               (is_inlink ? "input" : "output"), nb_link,
               av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(ENOSYS);
    }

    info->pix_fmt  = hwfc->sw_format;
    info->pix_desc = av_pix_fmt_desc_get(info->pix_fmt);
    info->bytes_pp = av_get_padded_bits_per_pixel(info->pix_desc) / 8.0f;

    info->act_x    = 0;
    info->act_y    = 0;
    info->act_w    = link->w;
    info->act_h    = link->h;

    /* The w/h of RGA YUV image needs to be 2 aligned */
    if (!(info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        info->act_w = ALIGN_DOWN(info->act_w, RK_RGA_YUV_ALIGN);
        info->act_h = ALIGN_DOWN(info->act_h, RK_RGA_YUV_ALIGN);
    }

    info->uncompact_10b_msb = info->pix_fmt == AV_PIX_FMT_P010 ||
                              info->pix_fmt == AV_PIX_FMT_P210;

    if (link->w * link->h > (3840 * 2160 * 3))
        r->async_depth = FFMIN(r->async_depth, 1);

    return 0;
}

av_cold int ff_rkrga_init(AVFilterContext *avctx, RKRGAParam *param)
{
    RKRGAContext *r = avctx->priv;
    int i, ret;
    int rga_core_mask = 0x7;
    const char *rga_ver = querystring(RGA_VERSION);

    r->got_frame = 0;

    r->has_rga2  = !!strstr(rga_ver, "RGA_2");
    r->has_rga2l = !!strstr(rga_ver, "RGA_2_lite");
    r->has_rga2e = !!strstr(rga_ver, "RGA_2_Enhance");
    r->has_rga2p = !!strstr(rga_ver, "RGA_2_PRO");
    r->has_rga3  = !!strstr(rga_ver, "RGA_3");

    if (!(r->has_rga2 || r->has_rga3)) {
        av_log(avctx, AV_LOG_ERROR, "No RGA2/RGA3 hw available\n");
        return AVERROR(ENOSYS);
    }

    if (r->has_rga2p)
        rga_core_mask = 0xf;

    /* RGA core */
    if (r->scheduler_core && !(r->has_rga2 && r->has_rga3) && !r->has_rga2p) {
        av_log(avctx, AV_LOG_WARNING, "Scheduler core cannot be set on non-multicore RGA hw, ignoring\n");
        r->scheduler_core = 0;
    }
    if (r->scheduler_core && r->scheduler_core != (r->scheduler_core & rga_core_mask)) {
        av_log(avctx, AV_LOG_WARNING, "Invalid scheduler core set, ignoring\n");
        r->scheduler_core = 0;
    }
    if (r->scheduler_core && r->scheduler_core == (r->scheduler_core & 0x3))
        r->has_rga2 = r->has_rga2l = r->has_rga2e = r->has_rga2p = 0;
    if (r->scheduler_core == 0x4 && !r->has_rga2p)
        r->has_rga3 = 0;

    r->filter_frame = param->filter_frame;
    if (!r->filter_frame)
         r->filter_frame = ff_filter_frame;
    r->out_sw_format = param->out_sw_format;

    /* OUT hwfc */
    ret = init_hwframes_ctx(avctx);
    if (ret < 0)
        goto fail;

    /* IN RGAFrameInfo */
    r->in_rga_frame_infos = av_calloc(avctx->nb_inputs, sizeof(*r->in_rga_frame_infos));
    if (!r->in_rga_frame_infos) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    for (i = 0; i < avctx->nb_inputs; i++) {
        ret = fill_rga_frame_info_by_link(avctx, &r->in_rga_frame_infos[i], avctx->inputs[i], i, 1);
        if (ret < 0)
            goto fail;
    }
    if (avctx->nb_inputs == 1) {
        r->in_rga_frame_infos[0].rotate_mode = param->in_rotate_mode;

        if (param->in_crop) {
            /* The x/y/w/h of RGA YUV image needs to be 2 aligned */
            if (!(r->in_rga_frame_infos[0].pix_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
                param->in_crop_x = ALIGN_DOWN(param->in_crop_x, RK_RGA_YUV_ALIGN);
                param->in_crop_y = ALIGN_DOWN(param->in_crop_y, RK_RGA_YUV_ALIGN);
                param->in_crop_w = ALIGN_DOWN(param->in_crop_w, RK_RGA_YUV_ALIGN);
                param->in_crop_h = ALIGN_DOWN(param->in_crop_h, RK_RGA_YUV_ALIGN);
            }
            r->in_rga_frame_infos[0].crop = 1;
            r->in_rga_frame_infos[0].act_x = param->in_crop_x;
            r->in_rga_frame_infos[0].act_y = param->in_crop_y;
            r->in_rga_frame_infos[0].act_w = param->in_crop_w;
            r->in_rga_frame_infos[0].act_h = param->in_crop_h;
        }
    }
    if (avctx->nb_inputs > 1) {
        const int premultiplied_alpha = r->in_rga_frame_infos[1].pix_desc->flags & AV_PIX_FMT_FLAG_ALPHA;

        /* IM_ALPHA_BLEND_DST_OVER */
        if (param->in_global_alpha > 0 && param->in_global_alpha < 0xff) {
            r->in_rga_frame_infos[0].blend_mode = premultiplied_alpha ? (0x4 | (1 << 12)) : 0x4;
            r->in_rga_frame_infos[0].blend_mode |= (param->in_global_alpha & 0xff) << 16; /* fg_global_alpha */
            r->in_rga_frame_infos[0].blend_mode |= 0xff << 24;                            /* bg_global_alpha */
        } else
            r->in_rga_frame_infos[0].blend_mode = premultiplied_alpha ? 0x504 : 0x501;

        r->in_rga_frame_infos[1].overlay_x = FFMAX(param->overlay_x, 0);
        r->in_rga_frame_infos[1].overlay_y = FFMAX(param->overlay_y, 0);

        r->is_overlay_offset_valid = (param->overlay_x < r->in_rga_frame_infos[0].act_w - 2) &&
            (param->overlay_y < r->in_rga_frame_infos[0].act_h - 2);
        if (r->is_overlay_offset_valid)
            init_pat_preproc_hwframes_ctx(avctx);
    }

    /* OUT RGAFrameInfo */
    ret = fill_rga_frame_info_by_link(avctx, &r->out_rga_frame_info, avctx->outputs[0], 0, 0);
    if (ret < 0)
        goto fail;

    /* Pre-check RGAFrameInfo */
    ret = verify_rga_frame_info(avctx, &r->in_rga_frame_infos[0],
                                &r->out_rga_frame_info,
                                (avctx->nb_inputs > 1 ? &r->in_rga_frame_infos[1] : NULL));
    if (ret < 0)
        goto fail;

    r->out_rga_frame_info.scheduler_core = r->scheduler_core;

    /* keep fifo size at least 1. Even when async_depth is 0, fifo is used. */
    r->async_fifo  = av_fifo_alloc2(r->async_depth + 1, sizeof(RGAAsyncFrame), 0);
    if (!r->async_fifo) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    ff_rkrga_close(avctx);
    return ret;
}

static void set_rga_async_frame_lock_status(RGAAsyncFrame *frame, int lock)
{
    int status = !!lock;

    if (!frame)
        return;

    if (frame->src)
        frame->src->locked = status;
    if (frame->dst)
        frame->dst->locked = status;
    if (frame->pat)
        frame->pat->locked = status;
}

static void rga_drain_fifo(RKRGAContext *r)
{
    RGAAsyncFrame aframe;

    while (r->async_fifo && av_fifo_read(r->async_fifo, &aframe, 1) >= 0) {
        if (imsync(aframe.dst->info.out_fence_fd) != IM_STATUS_SUCCESS)
            av_log(NULL, AV_LOG_WARNING, "RGA sync failed\n");

        set_rga_async_frame_lock_status(&aframe, 0);
    }
}

av_cold int ff_rkrga_close(AVFilterContext *avctx)
{
    RKRGAContext *r = avctx->priv;

    /* Drain the fifo during filter reset */
    rga_drain_fifo(r);

    clear_frame_list(&r->src_frame_list);
    clear_frame_list(&r->dst_frame_list);
    clear_frame_list(&r->pat_frame_list);

    clear_frame_list(&r->pat_preproc_frame_list);

    av_fifo_freep2(&r->async_fifo);

    av_buffer_unref(&r->pat_preproc_hwframes_ctx);

    return 0;
}

static int call_rkrga_blit(AVFilterContext *avctx,
                          rga_info_t *src_info,
                          rga_info_t *dst_info,
                          rga_info_t *pat_info)
{
    int ret;

    if (!src_info || !dst_info)
        return AVERROR(EINVAL);

#define PRINT_RGA_INFO(ctx, info, name) do { \
    if (info && name) \
        av_log(ctx, AV_LOG_DEBUG, "RGA %s | fd:%d mmu:%d rd_mode:%d | x:%d y:%d w:%d h:%d ws:%d hs:%d fmt:0x%x\n", \
               name, info->fd, info->mmuFlag, (info->rd_mode >> 1), info->rect.xoffset, info->rect.yoffset, \
               info->rect.width, info->rect.height, info->rect.wstride, info->rect.hstride, (info->rect.format >> 8)); \
} while (0)

    PRINT_RGA_INFO(avctx, src_info, "src");
    PRINT_RGA_INFO(avctx, dst_info, "dst");
    PRINT_RGA_INFO(avctx, pat_info, "pat");
#undef PRINT_RGA_INFO

    if ((ret = c_RkRgaBlit(src_info, dst_info, pat_info)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "RGA blit failed: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    if (dst_info->sync_mode == RGA_BLIT_ASYNC &&
        dst_info->out_fence_fd <= 0) {
        av_log(avctx, AV_LOG_ERROR, "RGA async blit returned invalid fence_fd: %d\n",
               dst_info->out_fence_fd);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

int ff_rkrga_filter_frame(RKRGAContext *r,
                          AVFilterLink *inlink_src, AVFrame *picref_src,
                          AVFilterLink *inlink_pat, AVFrame *picref_pat)
{
    AVFilterContext  *ctx = inlink_src->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    RGAAsyncFrame aframe;
    RGAFrame *src_frame = NULL;
    RGAFrame *dst_frame = NULL;
    RGAFrame *pat_frame = NULL;
    int ret, filter_ret;
    int do_overlay = ctx->nb_inputs > 1 &&
                     r->is_overlay_offset_valid &&
                     inlink_pat && picref_pat;

    /* Sync & Drain */
    while (r->eof && av_fifo_read(r->async_fifo, &aframe, 1) >= 0) {
        if (imsync(aframe.dst->info.out_fence_fd) != IM_STATUS_SUCCESS)
            av_log(ctx, AV_LOG_WARNING, "RGA sync failed\n");

        set_rga_async_frame_lock_status(&aframe, 0);

        filter_ret = r->filter_frame(outlink, aframe.dst->frame);
        if (filter_ret < 0) {
            av_frame_free(&aframe.dst->frame);
            return filter_ret;
        }
        aframe.dst->queued--;
        r->got_frame = 1;
        aframe.dst->frame = NULL;
    }

    if (!picref_src)
        return 0;

    /* SRC */
    if (!(src_frame = submit_frame(r, inlink_src, picref_src, do_overlay, 0))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to submit frame on input: %d\n",
               FF_INLINK_IDX(inlink_src));
        return AVERROR(ENOMEM);
    }

    /* DST */
    if (!(dst_frame = query_frame(r, outlink, src_frame->frame, 0))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to query an output frame\n");
        return AVERROR(ENOMEM);
    }

    /* PAT */
    if (do_overlay) {
        RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
        RGAFrameInfo *in1_info = &r->in_rga_frame_infos[1];
        RGAFrameInfo *out_info = &r->out_rga_frame_info;
        RGAFrame *pat_in = NULL;
        RGAFrame *pat_out = NULL;

        /* translate PAT from top-left to (x,y) on a new image with the same size of SRC */
        if (in1_info->act_w != in0_info->act_w ||
            in1_info->act_h != in0_info->act_h ||
            in1_info->overlay_x > 0 ||
            in1_info->overlay_y > 0) {
            if (!(pat_in = submit_frame(r, inlink_pat, picref_pat, 0, 1))) {
                av_log(ctx, AV_LOG_ERROR, "Failed to submit frame on input: %d\n",
                       FF_INLINK_IDX(inlink_pat));
                return AVERROR(ENOMEM);
            }
            if (!(pat_out = query_frame(r, outlink, picref_pat, 1))) {
                av_log(ctx, AV_LOG_ERROR, "Failed to query an output frame\n");
                return AVERROR(ENOMEM);
            }
            dst_frame->info.core = out_info->scheduler_core;

            pat_out->info.priority = 1;
            pat_out->info.core = dst_frame->info.core;
            pat_out->info.sync_mode = RGA_BLIT_SYNC;

            /* Sync Blit Pre-Proc */
            ret = call_rkrga_blit(ctx, &pat_in->info, &pat_out->info, NULL);
            if (ret < 0)
                return ret;

            pat_out->info.rect.xoffset = 0;
            pat_out->info.rect.yoffset = 0;
            pat_out->info.rect.width   = in0_info->act_w;
            pat_out->info.rect.height  = in0_info->act_h;

            pat_frame = pat_out;
        }

        if (!pat_frame && !(pat_frame = submit_frame(r, inlink_pat, picref_pat, 0, 0))) {
            av_log(ctx, AV_LOG_ERROR, "Failed to submit frame on input: %d\n",
                   FF_INLINK_IDX(inlink_pat));
            return AVERROR(ENOMEM);
        }
        dst_frame->info.core = out_info->scheduler_core;
    }

    /* Async Blit */
    ret = call_rkrga_blit(ctx,
                          &src_frame->info,
                          &dst_frame->info,
                          pat_frame ? &pat_frame->info : NULL);
    if (ret < 0)
        return ret;

    dst_frame->queued++;
    aframe = (RGAAsyncFrame){ src_frame, dst_frame, pat_frame };
    set_rga_async_frame_lock_status(&aframe, 1);
    av_fifo_write(r->async_fifo, &aframe, 1);

    /* Sync & Retrieve */
    if (av_fifo_can_read(r->async_fifo) > r->async_depth) {
        av_fifo_read(r->async_fifo, &aframe, 1);
        if (imsync(aframe.dst->info.out_fence_fd) != IM_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "RGA sync failed\n");
            return AVERROR_EXTERNAL;
        }
        set_rga_async_frame_lock_status(&aframe, 0);

        filter_ret = r->filter_frame(outlink, aframe.dst->frame);
        if (filter_ret < 0) {
            av_frame_free(&aframe.dst->frame);
            return filter_ret;
        }
        aframe.dst->queued--;
        r->got_frame = 1;
        aframe.dst->frame = NULL;
    }

    return 0;
}
