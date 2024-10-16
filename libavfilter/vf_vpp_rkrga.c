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
 * Rockchip RGA (2D Raster Graphic Acceleration) video post-process (scale/crop/transpose)
 */

#include "config_components.h"

#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "scale_eval.h"
#include "transpose.h"

#include "rkrga_common.h"

typedef struct RGAVppContext {
    RKRGAContext rga;

    enum AVPixelFormat format;
    int transpose;
    int force_original_aspect_ratio;
    int force_divisible_by;
    int force_yuv;
    int force_chroma;
    int scheduler_core;

    int in_rotate_mode;

    char *ow, *oh;
    char *cx, *cy, *cw, *ch;
    int crop;

    int act_x, act_y;
    int act_w, act_h;
} RGAVppContext;

enum {
    FORCE_YUV_DISABLE,
    FORCE_YUV_AUTO,
    FORCE_YUV_8BIT,
    FORCE_YUV_10BIT,
    FORCE_YUV_NB
};

enum {
    FORCE_CHROMA_AUTO,
    FORCE_CHROMA_420SP,
    FORCE_CHROMA_420P,
    FORCE_CHROMA_422SP,
    FORCE_CHROMA_422P,
    FORCE_CHROMA_NB
};

static const char *const var_names[] = {
    "iw", "in_w",
    "ih", "in_h",
    "ow", "out_w", "w",
    "oh", "out_h", "h",
    "cw",
    "ch",
    "cx",
    "cy",
    "a", "dar",
    "sar",
    NULL
};

enum var_name {
    VAR_IW, VAR_IN_W,
    VAR_IH, VAR_IN_H,
    VAR_OW, VAR_OUT_W, VAR_W,
    VAR_OH, VAR_OUT_H, VAR_H,
    VAR_CW,
    VAR_CH,
    VAR_CX,
    VAR_CY,
    VAR_A, VAR_DAR,
    VAR_SAR,
    VAR_VARS_NB
};

static av_cold int eval_expr(AVFilterContext *ctx,
                             int *ret_w, int *ret_h,
                             int *ret_cx, int *ret_cy,
                             int *ret_cw, int *ret_ch)
{
#define PASS_EXPR(e, s) {\
    if (s) {\
        ret = av_expr_parse(&e, s, var_names, NULL, NULL, NULL, NULL, 0, ctx); \
        if (ret < 0) {                                                  \
            av_log(ctx, AV_LOG_ERROR, "Error when passing '%s'.\n", s); \
            goto release;                                               \
        }                                                               \
    }\
}
#define CALC_EXPR(e, v, i, d) {\
    if (e)\
        i = v = av_expr_eval(e, var_values, NULL);      \
    else\
        i = v = d;\
}
    RGAVppContext *r = ctx->priv;
    double  var_values[VAR_VARS_NB] = { NAN };
    AVExpr *w_expr  = NULL, *h_expr  = NULL;
    AVExpr *cw_expr = NULL, *ch_expr = NULL;
    AVExpr *cx_expr = NULL, *cy_expr = NULL;
    int     ret = 0;

    PASS_EXPR(cw_expr, r->cw);
    PASS_EXPR(ch_expr, r->ch);

    PASS_EXPR(w_expr, r->ow);
    PASS_EXPR(h_expr, r->oh);

    PASS_EXPR(cx_expr, r->cx);
    PASS_EXPR(cy_expr, r->cy);

    var_values[VAR_IW] =
    var_values[VAR_IN_W] = ctx->inputs[0]->w;

    var_values[VAR_IH] =
    var_values[VAR_IN_H] = ctx->inputs[0]->h;

    var_values[VAR_A] = (double)var_values[VAR_IN_W] / var_values[VAR_IN_H];
    var_values[VAR_SAR] = ctx->inputs[0]->sample_aspect_ratio.num ?
        (double)ctx->inputs[0]->sample_aspect_ratio.num / ctx->inputs[0]->sample_aspect_ratio.den : 1;
    var_values[VAR_DAR] = var_values[VAR_A] * var_values[VAR_SAR];

    /* crop params */
    CALC_EXPR(cw_expr, var_values[VAR_CW], *ret_cw, var_values[VAR_IW]);
    CALC_EXPR(ch_expr, var_values[VAR_CH], *ret_ch, var_values[VAR_IH]);

    /* calc again in case cw is relative to ch */
    CALC_EXPR(cw_expr, var_values[VAR_CW], *ret_cw, var_values[VAR_IW]);

    CALC_EXPR(w_expr,
              var_values[VAR_OUT_W] = var_values[VAR_OW] = var_values[VAR_W],
              *ret_w, var_values[VAR_CW]);
    CALC_EXPR(h_expr,
              var_values[VAR_OUT_H] = var_values[VAR_OH] = var_values[VAR_H],
              *ret_h, var_values[VAR_CH]);

    /* calc again in case ow is relative to oh */
    CALC_EXPR(w_expr,
              var_values[VAR_OUT_W] = var_values[VAR_OW] = var_values[VAR_W],
              *ret_w, var_values[VAR_CW]);

    CALC_EXPR(cx_expr, var_values[VAR_CX], *ret_cx, (var_values[VAR_IW] - var_values[VAR_OW]) / 2);
    CALC_EXPR(cy_expr, var_values[VAR_CY], *ret_cy, (var_values[VAR_IH] - var_values[VAR_OH]) / 2);

    /* calc again in case cx is relative to cy */
    CALC_EXPR(cx_expr, var_values[VAR_CX], *ret_cx, (var_values[VAR_IW] - var_values[VAR_OW]) / 2);

    r->crop = (*ret_cw != var_values[VAR_IW]) || (*ret_ch != var_values[VAR_IH]);

release:
    av_expr_free(w_expr);
    av_expr_free(h_expr);
    av_expr_free(cw_expr);
    av_expr_free(ch_expr);
    av_expr_free(cx_expr);
    av_expr_free(cy_expr);
#undef PASS_EXPR
#undef CALC_EXPR

    return ret;
}

static av_cold int set_size_info(AVFilterContext *ctx,
                                 AVFilterLink *inlink,
                                 AVFilterLink *outlink)
{
    RGAVppContext *r = ctx->priv;
    int w, h, ret;

    if (inlink->w < 2 || inlink->w > 8192 ||
        inlink->h < 2 || inlink->h > 8192) {
        av_log(ctx, AV_LOG_ERROR, "Supported input size is range from 2x2 ~ 8192x8192\n");
        return AVERROR(EINVAL);
    }

    if ((ret = eval_expr(ctx, &w, &h, &r->act_x, &r->act_y, &r->act_w, &r->act_h)) < 0)
        return ret;

    r->act_x = FFMAX(FFMIN(r->act_x, inlink->w), 0);
    r->act_y = FFMAX(FFMIN(r->act_y, inlink->h), 0);
    r->act_w = FFMAX(FFMIN(r->act_w, inlink->w), 0);
    r->act_h = FFMAX(FFMIN(r->act_h, inlink->h), 0);

    r->act_x = FFMIN(r->act_x, inlink->w - r->act_w);
    r->act_y = FFMIN(r->act_y, inlink->h - r->act_h);
    r->act_w = FFMIN(r->act_w, inlink->w - r->act_x);
    r->act_h = FFMIN(r->act_h, inlink->h - r->act_y);

    ff_scale_adjust_dimensions(inlink, &w, &h,
                               r->force_original_aspect_ratio, r->force_divisible_by);

    if (((int64_t)h * inlink->w) > INT_MAX ||
        ((int64_t)w * inlink->h) > INT_MAX) {
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");
        return AVERROR(EINVAL);
    }

    outlink->w = w;
    outlink->h = h;
    if (outlink->w < 2 || outlink->w > 8192 ||
        outlink->h < 2 || outlink->h > 8192) {
        av_log(ctx, AV_LOG_ERROR, "Supported output size is range from 2x2 ~ 8192x8192\n");
        return AVERROR(EINVAL);
    }

    if (inlink->sample_aspect_ratio.num)
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink->w,
                                                             outlink->w * inlink->h},
                                                inlink->sample_aspect_ratio);
    else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    if (r->transpose >= 0) {
        switch (r->transpose) {
        case TRANSPOSE_CCLOCK_FLIP:
            r->in_rotate_mode = 0x07 | (0x01 << 4); /* HAL_TRANSFORM_ROT_270 | (HAL_TRANSFORM_FLIP_H << 4) */
            FFSWAP(int, outlink->w, outlink->h);
            FFSWAP(int, outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den);
            break;
        case TRANSPOSE_CLOCK:
            r->in_rotate_mode = 0x04; /* HAL_TRANSFORM_ROT_90 */
            FFSWAP(int, outlink->w, outlink->h);
            FFSWAP(int, outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den);
            break;
        case TRANSPOSE_CCLOCK:
            r->in_rotate_mode = 0x07; /* HAL_TRANSFORM_ROT_270 */
            FFSWAP(int, outlink->w, outlink->h);
            FFSWAP(int, outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den);
            break;
        case TRANSPOSE_CLOCK_FLIP:
            r->in_rotate_mode = 0x04 | (0x01 << 4); /* HAL_TRANSFORM_ROT_90 | (HAL_TRANSFORM_FLIP_H << 4) */
            FFSWAP(int, outlink->w, outlink->h);
            FFSWAP(int, outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den);
            break;
        case TRANSPOSE_REVERSAL:
            r->in_rotate_mode = 0x03; /* HAL_TRANSFORM_ROT_180 */
            break;
        case TRANSPOSE_HFLIP:
            r->in_rotate_mode = 0x01; /* HAL_TRANSFORM_FLIP_H */
            break;
        case TRANSPOSE_VFLIP:
            r->in_rotate_mode = 0x02; /* HAL_TRANSFORM_FLIP_V */
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Failed to set transpose mode to %d\n", r->transpose);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static av_cold void config_force_format(AVFilterContext *ctx,
                                        enum AVPixelFormat in_format,
                                        enum AVPixelFormat *out_format)
{
    RGAVppContext *r = ctx->priv;
    const AVPixFmtDescriptor *desc;
    const char *rga_ver = NULL;
    int has_rga3 = 0;
    int out_depth, force_chroma;
    int is_yuv, is_fully_planar;

    if (!out_format)
        return;

    if (r->force_yuv == FORCE_YUV_AUTO)
        out_depth = (in_format == AV_PIX_FMT_NV15 ||
                     in_format == AV_PIX_FMT_NV20) ? 10 : 0;
    else
        out_depth = (r->force_yuv == FORCE_YUV_8BIT) ? 8 :
                    (r->force_yuv == FORCE_YUV_10BIT) ? 10 : 0;

    if (!out_depth)
        return;

    /* Auto fallback to 8-bit fmts on RGA2 */
    rga_ver = querystring(RGA_VERSION);
    has_rga3 = !!strstr(rga_ver, "RGA_3");
    if (out_depth >= 10 && !has_rga3)
        out_depth = 8;

    desc = av_pix_fmt_desc_get(in_format);
    is_yuv = !(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components >= 2;

    force_chroma = r->force_chroma;
    if (is_yuv && force_chroma == FORCE_CHROMA_AUTO) {
        is_fully_planar = (desc->flags & AV_PIX_FMT_FLAG_PLANAR) &&
                           desc->comp[1].plane != desc->comp[2].plane;
        if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
            force_chroma = is_fully_planar ? FORCE_CHROMA_420P : FORCE_CHROMA_420SP;
        else if (desc->log2_chroma_w == 1 && !desc->log2_chroma_h)
            force_chroma = is_fully_planar ? FORCE_CHROMA_422P : FORCE_CHROMA_422SP;
    }

    switch (force_chroma) {
    case FORCE_CHROMA_422P:
        *out_format = AV_PIX_FMT_YUV422P;
        break;
    case FORCE_CHROMA_422SP:
        *out_format = out_depth == 10 ?
            AV_PIX_FMT_P210 : AV_PIX_FMT_NV16;
        break;
    case FORCE_CHROMA_420P:
        *out_format = AV_PIX_FMT_YUV420P;
        break;
    case FORCE_CHROMA_420SP:
    default:
        *out_format = out_depth == 10 ?
            AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    }
}

static av_cold int rgavpp_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    RGAVppContext     *r = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVHWFramesContext *in_frames_ctx;
    enum AVPixelFormat in_format;
    enum AVPixelFormat out_format;
    RKRGAParam param = { NULL };
    int ret;

    if (!inlink->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    in_frames_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
    in_format     = in_frames_ctx->sw_format;
    out_format    = (r->format == AV_PIX_FMT_NONE) ? in_format : r->format;

    config_force_format(ctx, in_format, &out_format);

    ret = set_size_info(ctx, inlink, outlink);
    if (ret < 0)
        return ret;

    param.filter_frame   = NULL;
    param.out_sw_format  = out_format;
    param.in_rotate_mode = r->in_rotate_mode;
    param.in_crop        = r->crop;
    param.in_crop_x      = r->act_x;
    param.in_crop_y      = r->act_y;
    param.in_crop_w      = r->act_w;
    param.in_crop_h      = r->act_h;

    ret = ff_rkrga_init(ctx, &param);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d fmt:%s -> w:%d h:%d fmt:%s\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(in_format),
           outlink->w, outlink->h, av_get_pix_fmt_name(out_format));

    return 0;
}

static int rgavpp_activate(AVFilterContext *ctx)
{
    AVFilterLink  *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    RGAVppContext      *r = ctx->priv;
    AVFrame *in = NULL;
    int ret, at_eof = 0, status = 0;
    int64_t pts = AV_NOPTS_VALUE;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    if (r->rga.eof)
        at_eof = 1;
    else {
        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;

        if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
            if (status == AVERROR_EOF) {
                at_eof = 1;
            }
        }
    }

    if (in) {
        ret = ff_rkrga_filter_frame(&r->rga, inlink, in, NULL, NULL);
        av_frame_free(&in);
        if (ret < 0)
            return ret;
        else if (!r->rga.got_frame)
            goto not_ready;

        if (at_eof) {
            r->rga.eof = 1;
            goto eof;
        }

        if (r->rga.got_frame) {
            r->rga.got_frame = 0;
            return 0;
        }
    }

not_ready:
    if (at_eof) {
        r->rga.eof = 1;
        goto eof;
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);
    return FFERROR_NOT_READY;

eof:
    ff_rkrga_filter_frame(&r->rga, inlink, NULL, NULL, NULL);

    pts = av_rescale_q(pts, inlink->time_base, outlink->time_base);
    ff_outlink_set_status(outlink, AVERROR_EOF, pts);
    return 0;
}

static av_cold int rgavpp_init(AVFilterContext *ctx)
{
    return 0;
}

static av_cold void rgavpp_uninit(AVFilterContext *ctx)
{
    ff_rkrga_close(ctx);
}

#define OFFSET(x) offsetof(RGAVppContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

#define RKRGA_VPP_COMMON_OPTS \
    { "force_yuv",    "Enforce planar YUV format output", OFFSET(force_yuv), AV_OPT_TYPE_INT, { .i64 = FORCE_YUV_DISABLE }, 0, FORCE_YUV_NB - 1, FLAGS, "force_yuv" }, \
        { "disable",  NULL,                     0, AV_OPT_TYPE_CONST, { .i64 = FORCE_YUV_DISABLE  }, 0, 0, FLAGS, "force_yuv" }, \
        { "auto",     "Match in/out bit depth", 0, AV_OPT_TYPE_CONST, { .i64 = FORCE_YUV_AUTO     }, 0, 0, FLAGS, "force_yuv" }, \
        { "8bit",     "8-bit",                  0, AV_OPT_TYPE_CONST, { .i64 = FORCE_YUV_8BIT     }, 0, 0, FLAGS, "force_yuv" }, \
        { "10bit",    "10-bit uncompact/8-bit", 0, AV_OPT_TYPE_CONST, { .i64 = FORCE_YUV_10BIT    }, 0, 0, FLAGS, "force_yuv" }, \
    { "force_chroma", "Enforce chroma of planar YUV format output", OFFSET(force_chroma), AV_OPT_TYPE_INT, { .i64 = FORCE_CHROMA_AUTO }, 0, FORCE_CHROMA_NB - 1, FLAGS, "force_chroma" }, \
        { "auto",     "Match in/out chroma",    0, AV_OPT_TYPE_CONST, { .i64 = FORCE_CHROMA_AUTO  }, 0, 0, FLAGS, "force_chroma" }, \
        { "420sp",    "4:2:0 semi-planar",      0, AV_OPT_TYPE_CONST, { .i64 = FORCE_CHROMA_420SP }, 0, 0, FLAGS, "force_chroma" }, \
        { "420p",     "4:2:0 fully-planar",     0, AV_OPT_TYPE_CONST, { .i64 = FORCE_CHROMA_420P  }, 0, 0, FLAGS, "force_chroma" }, \
        { "422sp",    "4:2:2 semi-planar",      0, AV_OPT_TYPE_CONST, { .i64 = FORCE_CHROMA_422SP }, 0, 0, FLAGS, "force_chroma" }, \
        { "422p",     "4:2:2 fully-planar",     0, AV_OPT_TYPE_CONST, { .i64 = FORCE_CHROMA_422P  }, 0, 0, FLAGS, "force_chroma" }, \
    { "core", "Set multicore RGA scheduler core [use with caution]", OFFSET(rga.scheduler_core), AV_OPT_TYPE_FLAGS, { .i64 = 0 }, 0, INT_MAX, FLAGS, "core" }, \
        { "default",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, "core" }, \
        { "rga3_core0", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGS, "core" }, /* RGA3_SCHEDULER_CORE0 */ \
        { "rga3_core1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, FLAGS, "core" }, /* RGA3_SCHEDULER_CORE1 */ \
        { "rga2_core0", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 }, 0, 0, FLAGS, "core" }, /* RGA2_SCHEDULER_CORE0 */ \
        { "rga2_core1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 8 }, 0, 0, FLAGS, "core" }, /* RGA2_SCHEDULER_CORE1 */ \
    { "async_depth", "Set the internal parallelization depth", OFFSET(rga.async_depth), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, 4, .flags = FLAGS }, \
    { "afbc", "Enable AFBC (Arm Frame Buffer Compression) to save bandwidth", OFFSET(rga.afbc_out), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, .flags = FLAGS },

static const AVFilterPad rgavpp_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad rgavpp_outputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .config_props     = rgavpp_config_props,
    },
};

#if CONFIG_SCALE_RKRGA_FILTER

static const AVOption rgascale_options[] = {
    { "w",  "Output video width",  OFFSET(ow), AV_OPT_TYPE_STRING, { .str = "iw" }, 0, 0, FLAGS },
    { "h",  "Output video height", OFFSET(oh), AV_OPT_TYPE_STRING, { .str = "ih" }, 0, 0, FLAGS },
    { "format", "Output video pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, INT_MIN, INT_MAX, .flags = FLAGS },
    { "force_original_aspect_ratio", "Decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 2, FLAGS, "force_oar" }, \
        { "disable",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, "force_oar" }, \
        { "decrease", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGS, "force_oar" }, \
        { "increase", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, FLAGS, "force_oar" }, \
    { "force_divisible_by", "Enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 2 }, 1, 256, FLAGS }, \
    RKRGA_VPP_COMMON_OPTS
    { NULL },
};

static av_cold int rgascale_preinit(AVFilterContext *ctx)
{
    RGAVppContext *r = ctx->priv;

    r->transpose = -1;
    return 0;
}

AVFILTER_DEFINE_CLASS(rgascale);

const AVFilter ff_vf_scale_rkrga = {
    .name           = "scale_rkrga",
    .description    = NULL_IF_CONFIG_SMALL("Rockchip RGA (2D Raster Graphic Acceleration) video resizer and format converter"),
    .priv_size      = sizeof(RGAVppContext),
    .priv_class     = &rgascale_class,
    .preinit        = rgascale_preinit,
    .init           = rgavpp_init,
    .uninit         = rgavpp_uninit,
    FILTER_INPUTS(rgavpp_inputs),
    FILTER_OUTPUTS(rgavpp_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .activate       = rgavpp_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

#endif

#if CONFIG_VPP_RKRGA_FILTER

static const AVOption rgavpp_options[] = {
    { "w",  "Output video width",                  OFFSET(ow), AV_OPT_TYPE_STRING, { .str = "cw" }, 0, 0, FLAGS },
    { "h",  "Output video height",                 OFFSET(oh), AV_OPT_TYPE_STRING, { .str = "w*ch/cw" }, 0, 0, FLAGS },
    { "cw", "Set the width crop area expression",  OFFSET(cw), AV_OPT_TYPE_STRING, { .str = "iw" }, 0, 0, FLAGS },
    { "ch", "Set the height crop area expression", OFFSET(ch), AV_OPT_TYPE_STRING, { .str = "ih" }, 0, 0, FLAGS },
    { "cx", "Set the x crop area expression",      OFFSET(cx), AV_OPT_TYPE_STRING, { .str = "(in_w-out_w)/2" }, 0, 0, FLAGS },
    { "cy", "Set the y crop area expression",      OFFSET(cy), AV_OPT_TYPE_STRING, { .str = "(in_h-out_h)/2" }, 0, 0, FLAGS },
    { "format", "Output video pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, INT_MIN, INT_MAX, .flags = FLAGS },
    { "transpose", "Set transpose direction", OFFSET(transpose), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 6, FLAGS, "transpose" },
        { "cclock_hflip", "Rotate counter-clockwise with horizontal flip", 0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK_FLIP }, 0, 0, FLAGS, "transpose" },
        { "clock",        "Rotate clockwise",                              0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK       }, 0, 0, FLAGS, "transpose" },
        { "cclock",       "Rotate counter-clockwise",                      0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK      }, 0, 0, FLAGS, "transpose" },
        { "clock_hflip",  "Rotate clockwise with horizontal flip",         0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK_FLIP  }, 0, 0, FLAGS, "transpose" },
        { "reversal",     "Rotate by half-turn",                           0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_REVERSAL    }, 0, 0, FLAGS, "transpose" },
        { "hflip",        "Flip horizontally",                             0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_HFLIP       }, 0, 0, FLAGS, "transpose" },
        { "vflip",        "Flip vertically",                               0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_VFLIP       }, 0, 0, FLAGS, "transpose" },
    RKRGA_VPP_COMMON_OPTS
    { NULL },
};

AVFILTER_DEFINE_CLASS(rgavpp);

const AVFilter ff_vf_vpp_rkrga = {
    .name           = "vpp_rkrga",
    .description    = NULL_IF_CONFIG_SMALL("Rockchip RGA (2D Raster Graphic Acceleration) video post-process (scale/crop/transpose)"),
    .priv_size      = sizeof(RGAVppContext),
    .priv_class     = &rgavpp_class,
    .init           = rgavpp_init,
    .uninit         = rgavpp_uninit,
    FILTER_INPUTS(rgavpp_inputs),
    FILTER_OUTPUTS(rgavpp_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .activate       = rgavpp_activate,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

#endif
