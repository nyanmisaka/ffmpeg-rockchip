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

#include "config_components.h"
#include "rkmppenc.h"

#include <fcntl.h>
#include <math.h>
#include <unistd.h>

static MppCodingType rkmpp_get_coding_type(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:  return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:  return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_MJPEG: return MPP_VIDEO_CodingMJPEG;
    default:                return MPP_VIDEO_CodingUnused;
    }
}

static MppFrameFormat rkmpp_get_mpp_fmt_h26x(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_GRAY8:   return MPP_FMT_YUV400;
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV420P: return MPP_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUV422P: return MPP_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P: return MPP_FMT_YUV444P;
    case AV_PIX_FMT_NV12:    return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_NV21:    return MPP_FMT_YUV420SP_VU;
    case AV_PIX_FMT_NV16:    return MPP_FMT_YUV422SP;
    case AV_PIX_FMT_NV24:    return MPP_FMT_YUV444SP;
    case AV_PIX_FMT_YUYV422: return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_YVYU422: return MPP_FMT_YUV422_YVYU;
    case AV_PIX_FMT_UYVY422: return MPP_FMT_YUV422_UYVY;
    case AV_PIX_FMT_RGB24:   return MPP_FMT_RGB888;
    case AV_PIX_FMT_BGR24:   return MPP_FMT_BGR888;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_RGB0:    return MPP_FMT_RGBA8888;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_BGR0:    return MPP_FMT_BGRA8888;
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_0RGB:    return MPP_FMT_ARGB8888;
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_0BGR:    return MPP_FMT_ABGR8888;
    default:                 return MPP_FMT_BUTT;
    }
}

static MppFrameFormat rkmpp_get_mpp_fmt_mjpeg(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV420P:   return MPP_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUV422P:   return MPP_FMT_YUV422P;     /* RK3576+ only */
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P:   return MPP_FMT_YUV444P;     /* RK3576+ only */
    case AV_PIX_FMT_NV12:      return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_NV21:      return MPP_FMT_YUV420SP_VU; /* RK3576+ only */
    case AV_PIX_FMT_NV16:      return MPP_FMT_YUV422SP;    /* RK3576+ only */
    case AV_PIX_FMT_NV24:      return MPP_FMT_YUV444SP;    /* RK3576+ only */
    case AV_PIX_FMT_YUYV422:   return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_UYVY422:   return MPP_FMT_YUV422_UYVY;
    case AV_PIX_FMT_YVYU422:   return MPP_FMT_YUV422_YVYU; /* RK3576+ only */

    /* RGB: pre-RK3576 only */
    case AV_PIX_FMT_RGB444BE:  return MPP_FMT_RGB444;
    case AV_PIX_FMT_BGR444BE:  return MPP_FMT_BGR444;
    case AV_PIX_FMT_RGB555BE:  return MPP_FMT_RGB555;
    case AV_PIX_FMT_BGR555BE:  return MPP_FMT_BGR555;
    case AV_PIX_FMT_RGB565BE:  return MPP_FMT_RGB565;
    case AV_PIX_FMT_BGR565BE:  return MPP_FMT_BGR565;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_RGB0:      return MPP_FMT_RGBA8888;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_BGR0:      return MPP_FMT_BGRA8888;
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_0RGB:      return MPP_FMT_ARGB8888;
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_0BGR:      return MPP_FMT_ABGR8888;
    case AV_PIX_FMT_X2RGB10BE: return MPP_FMT_RGB101010;
    case AV_PIX_FMT_X2BGR10BE: return MPP_FMT_BGR101010;
    default:                   return MPP_FMT_BUTT;
    }
}

static uint32_t rkmpp_get_drm_afbc_format(MppFrameFormat mpp_fmt)
{
    switch (mpp_fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP: return DRM_FORMAT_YUV420_8BIT;
    case MPP_FMT_YUV422SP: return DRM_FORMAT_YUYV;
    default:               return DRM_FORMAT_INVALID;
    }
}

static MppFrameChromaFormat rkmpp_fix_chroma_fmt(int chroma_fmt,
                                                 enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    int log2_chroma_sum = desc->log2_chroma_w + desc->log2_chroma_h;
    int is_yuv = !(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
                 desc->nb_components >= 2;

    if (!is_yuv)
        return MPP_CHROMA_UNSPECIFIED;

    switch (chroma_fmt) {
    case -1:
        return log2_chroma_sum == 0 ? MPP_CHROMA_444 :
               log2_chroma_sum == 1 ? MPP_CHROMA_422 :
                                      MPP_CHROMA_UNSPECIFIED;
    case MPP_CHROMA_400:
        return chroma_fmt;
    case MPP_CHROMA_420:
        return log2_chroma_sum <= 2 ?
            chroma_fmt : MPP_CHROMA_UNSPECIFIED;
    case MPP_CHROMA_422:
        return log2_chroma_sum <= 1 ?
            chroma_fmt : MPP_CHROMA_UNSPECIFIED;
    case MPP_CHROMA_444:
        return log2_chroma_sum == 0 ?
            chroma_fmt : MPP_CHROMA_UNSPECIFIED;
    default:
        return MPP_CHROMA_UNSPECIFIED;
    }
}

static int get_byte_stride(const AVDRMObjectDescriptor *object,
                           const AVDRMLayerDescriptor *layer,
                           int is_rgb, int is_planar,
                           int *hs, int *vs)
{
    const AVDRMPlaneDescriptor *plane0, *plane1;
    const int is_packed_fmt = is_rgb || (!is_rgb && !is_planar);

    if (!object || !layer || !hs || !vs)
        return AVERROR(EINVAL);

    plane0 = &layer->planes[0];
    plane1 = &layer->planes[1];

    *hs = plane0->pitch;
    *vs = is_packed_fmt ?
        ALIGN_DOWN(object->size / plane0->pitch, is_rgb ? 1 : 2) :
        (plane1->offset / plane0->pitch);

    return (*hs > 0 && *vs > 0) ? 0 : AVERROR(EINVAL);
}

static int get_afbc_byte_stride(const AVPixFmtDescriptor *desc,
                                int *stride, int reverse)
{
    if (!desc || !stride || *stride <= 0)
        return AVERROR(EINVAL);

    if (desc->nb_components == 1 ||
        (desc->flags & AV_PIX_FMT_FLAG_RGB) ||
        (!(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
         !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)))
        return 0;

    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        *stride = reverse ? (*stride * 2 / 3) : (*stride * 3 / 2);
    else if (desc->log2_chroma_w == 1 && !desc->log2_chroma_h)
        *stride = reverse ? (*stride / 2) : (*stride * 2);
    else if (!desc->log2_chroma_w && !desc->log2_chroma_h)
        *stride = reverse ? (*stride / 3) : (*stride * 3);
    else
        return AVERROR(EINVAL);

    return (*stride > 0) ? 0 : AVERROR(EINVAL);
}

static unsigned get_used_frame_count(MPPEncFrame *list)
{
    unsigned count = 0;

    while (list) {
        if (list->queued == 1 &&
            (list->frame || list->mpp_frame))
            ++count;
        list = list->next;
    }

    return count;
}

static void clear_unused_frames(MPPEncFrame *list)
{
    while (list) {
        if (list->queued == 1) {
            MppFrame mpp_frame = list->mpp_frame;
            MppBuffer mpp_buf = NULL;

            if (mpp_frame)
                mpp_buf = mpp_frame_get_buffer(mpp_frame);

            if (mpp_buf &&
                mpp_buffer_get_index(mpp_buf) < 0) {
                mpp_buffer_put(mpp_buf);

                mpp_frame_deinit(&list->mpp_frame);
                list->mpp_frame = NULL;

                av_freep(&list->mpp_sei_set.datas);
                list->mpp_sei_set.count = 0;

                av_freep(&list->mpp_roi_regions);
                list->mpp_roi_cfg.number  = 0;
                list->mpp_roi_cfg.regions = NULL;
                memset(&list->mpp_roi_cfg2, 0, sizeof(list->mpp_roi_cfg2));

                av_frame_free(&list->frame);
                list->queued = 0;
            }
        }
        list = list->next;
    }
}

static void clear_frame_list(MPPEncFrame **list)
{
    while (*list) {
        MPPEncFrame *frame = NULL;
        MppFrame mpp_frame = NULL;
        MppBuffer mpp_buf = NULL;

        frame = *list;
        *list = (*list)->next;

        mpp_frame = frame->mpp_frame;
        if (mpp_frame) {
            mpp_buf = mpp_frame_get_buffer(mpp_frame);
            if (mpp_buf &&
                mpp_buffer_get_index(mpp_buf) >= 0)
                mpp_buffer_put(mpp_buf);

            mpp_frame_deinit(&frame->mpp_frame);
            frame->mpp_frame = NULL;
        }

        av_freep(&frame->mpp_sei_set.datas);
        frame->mpp_sei_set.count = 0;

        av_freep(&frame->mpp_roi_regions);
        frame->mpp_roi_cfg.number  = 0;
        frame->mpp_roi_cfg.regions = NULL;
        if (frame->mpp_roi_base_buf) {
            mpp_buffer_put(frame->mpp_roi_base_buf);
            frame->mpp_roi_base_buf = NULL;
        }
        if (frame->mpp_roi_qp_buf) {
            mpp_buffer_put(frame->mpp_roi_qp_buf);
            frame->mpp_roi_qp_buf = NULL;
        }

        av_frame_free(&frame->frame);
        av_freep(&frame);
    }
}

static MPPEncFrame *get_free_frame(MPPEncFrame **list)
{
    MPPEncFrame *out = *list;

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

static int rkmpp_set_enc_cfg_prep(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppEncCfg cfg = r->mcfg;
    MppFrameFormat mpp_fmt = r->mpp_fmt;
    int ret, is_afbc = 0;
    int hor_stride = 0, ver_stride = 0;
    const AVPixFmtDescriptor *pix_desc;
    const AVDRMFrameDescriptor *drm_desc;

    if (r->cfg_init)
        return 0;

    if (!frame)
        return AVERROR(EINVAL);

    drm_desc = (AVDRMFrameDescriptor *)frame->data[0];
    if (drm_desc->objects[0].fd < 0)
        return AVERROR(ENOMEM);

    pix_desc = av_pix_fmt_desc_get(r->pix_fmt);
    is_afbc = drm_is_afbc(drm_desc->objects[0].format_modifier);
    if (!is_afbc &&
        drm_desc->objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR) {
        av_log(avctx, AV_LOG_ERROR, "Only linear and AFBC modifiers are supported\n");
        return AVERROR(ENOSYS);
    }
    if (is_afbc &&
        !(avctx->codec_id == AV_CODEC_ID_H264 ||
          avctx->codec_id == AV_CODEC_ID_HEVC)) {
        av_log(avctx, AV_LOG_ERROR, "AFBC is not supported in codec '%s'\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    }
    if (!is_afbc) {
        ret = get_byte_stride(&drm_desc->objects[0],
                              &drm_desc->layers[0],
                              (pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                              (pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                              &hor_stride, &ver_stride);
        if (ret < 0 || !hor_stride || !ver_stride) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            return AVERROR(EINVAL);
        }

        mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride);
        mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride);
    }

    mpp_enc_cfg_set_s32(cfg, "prep:width", avctx->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", avctx->height);

    if (pix_desc->flags & AV_PIX_FMT_FLAG_RGB) /* RGB -> BT709 CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", AVCOL_SPC_BT709);
    else
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", avctx->colorspace);

    mpp_enc_cfg_set_s32(cfg, "prep:colorprim", avctx->color_primaries);
    mpp_enc_cfg_set_s32(cfg, "prep:colortrc", avctx->color_trc);

    mpp_enc_cfg_set_s32(cfg, "prep:colorrange", avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_enc_cfg_set_s32(cfg, "prep:colorrange", AVCOL_RANGE_JPEG);
    }

    if (avctx->codec_id == AV_CODEC_ID_MJPEG) {
        /* always output full range if the MJPEG encoder supports CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:range_out", AVCOL_RANGE_JPEG);
        mpp_enc_cfg_set_s32(cfg, "prep:format_out", rkmpp_fix_chroma_fmt(r->chroma_fmt, r->pix_fmt));
    }

    if (is_afbc) {
        const AVDRMLayerDescriptor *layer = &drm_desc->layers[0];
        uint32_t drm_afbc_fmt = rkmpp_get_drm_afbc_format(mpp_fmt);

        if (drm_afbc_fmt != layer->format) {
            av_log(avctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported\n",
                   av_get_pix_fmt_name(r->pix_fmt));
            return AVERROR(ENOSYS);
        }
        mpp_fmt |= MPP_FRAME_FBC_AFBC_V2;
    }
    mpp_enc_cfg_set_s32(cfg, "prep:format", mpp_fmt);

    if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_CFG, cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config with frame: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    r->cfg_init = 1;
    av_log(avctx, AV_LOG_VERBOSE, "Configured with size: %dx%d | pix_fmt: %s | sw_pix_fmt: %s\n",
           avctx->width, avctx->height,
           av_get_pix_fmt_name(avctx->pix_fmt), av_get_pix_fmt_name(r->pix_fmt));

    return 0;
}

static int rkmpp_set_enc_cfg(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppEncCfg cfg = r->mcfg;
    const AVPixFmtDescriptor *pix_desc;
    RK_U32 rc_mode, fps_num, fps_den;
    MppEncHeaderMode header_mode;
    MppEncSeiMode sei_mode;
    int64_t target_bps = FFMIN(avctx->bit_rate, INT_MAX);
    int64_t max_bps = FFMIN(avctx->rc_max_rate, INT_MAX);
    int64_t min_bps = FFMIN(avctx->rc_min_rate, INT_MAX);
    int qp_init, qp_max, qp_min, qp_max_i, qp_min_i;
    int ret;

    mpp_enc_cfg_set_s32(cfg, "prep:width", avctx->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", avctx->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", FFALIGN(avctx->width, 64));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", FFALIGN(avctx->height, 64));
    mpp_enc_cfg_set_s32(cfg, "prep:format", r->mpp_fmt);
    mpp_enc_cfg_set_s32(cfg, "prep:mirroring", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:rotation", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:flip", 0);

    pix_desc = av_pix_fmt_desc_get(r->pix_fmt);
    if (pix_desc->flags & AV_PIX_FMT_FLAG_RGB) /* RGB -> BT709 CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", AVCOL_SPC_BT709);
    else
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", avctx->colorspace);

    mpp_enc_cfg_set_s32(cfg, "prep:colorprim", avctx->color_primaries);
    mpp_enc_cfg_set_s32(cfg, "prep:colortrc", avctx->color_trc);

    mpp_enc_cfg_set_s32(cfg, "prep:colorrange", avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_enc_cfg_set_s32(cfg, "prep:colorrange", AVCOL_RANGE_JPEG);
    }

    if (avctx->codec_id == AV_CODEC_ID_MJPEG) {
        /* always output full range if the MJPEG encoder supports CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:range_out", AVCOL_RANGE_JPEG);
        mpp_enc_cfg_set_s32(cfg, "prep:format_out", rkmpp_fix_chroma_fmt(r->chroma_fmt, r->pix_fmt));
    }

    if (avctx->framerate.den > 0 && avctx->framerate.num > 0)
        av_reduce(&fps_num, &fps_den, avctx->framerate.num, avctx->framerate.den, 65535);
    else
        av_reduce(&fps_num, &fps_den, avctx->time_base.den, avctx->time_base.num, 65535);

    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_num);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", fps_den);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", fps_den);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",fps_num);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", fps_den);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", fps_den);

    mpp_enc_cfg_set_s32(cfg, "rc:gop", FFMAX(avctx->gop_size, 1));

    rc_mode = avctx->codec_id == AV_CODEC_ID_MJPEG ? MPP_ENC_RC_MODE_FIXQP : r->rc_mode;
    if (rc_mode == MPP_ENC_RC_MODE_BUTT) {
        if (r->qp_init >= 0)
            rc_mode = MPP_ENC_RC_MODE_FIXQP;
        else if (max_bps > 0)
            rc_mode = MPP_ENC_RC_MODE_VBR;
        else
            rc_mode = MPP_ENC_RC_MODE_CBR;
    }

    switch (rc_mode) {
    case MPP_ENC_RC_MODE_VBR:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to VBR\n"); break;
    case MPP_ENC_RC_MODE_CBR:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to CBR\n"); break;
    case MPP_ENC_RC_MODE_FIXQP:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to CQP\n"); break;
    case MPP_ENC_RC_MODE_AVBR:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to AVBR\n"); break;
    }
    mpp_enc_cfg_set_u32(cfg, "rc:mode", rc_mode);

    switch (rc_mode) {
    case MPP_ENC_RC_MODE_FIXQP:
        /* do not setup bitrate on FIXQP mode */
        break;
    case MPP_ENC_RC_MODE_VBR:
    case MPP_ENC_RC_MODE_AVBR:
        /* VBR mode has wide bound */
        max_bps = (max_bps > 0 && max_bps >= target_bps)
                  ? max_bps : (target_bps * 17 / 16);
        min_bps = (min_bps > 0 && min_bps <= target_bps)
                  ? min_bps : (target_bps * 1 / 16);
        break;
    case MPP_ENC_RC_MODE_CBR:
    default:
        /* CBR mode has narrow bound */
        max_bps = target_bps * 17 / 16;
        min_bps = target_bps * 15 / 16;
        break;
    }
    max_bps = FFMIN(max_bps, INT_MAX);
    if (rc_mode == MPP_ENC_RC_MODE_CBR ||
        rc_mode == MPP_ENC_RC_MODE_VBR ||
        rc_mode == MPP_ENC_RC_MODE_AVBR) {
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target", (int32_t)target_bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (int32_t)max_bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (int32_t)min_bps);
        av_log(avctx, AV_LOG_VERBOSE, "Bitrate Target/Min/Max is set to %"PRId32"/%"PRId32"/%"PRId32"\n",
               (int32_t)target_bps, (int32_t)min_bps, (int32_t)max_bps);
    }

    if (avctx->rc_buffer_size > 0 &&
        (rc_mode == MPP_ENC_RC_MODE_CBR ||
         rc_mode == MPP_ENC_RC_MODE_VBR ||
         rc_mode == MPP_ENC_RC_MODE_AVBR)) {
        int stats_time_in_sec = avctx->rc_buffer_size / max_bps;
        if (stats_time_in_sec > 0) {
            mpp_enc_cfg_set_u32(cfg, "rc:stats_time", stats_time_in_sec);
            av_log(avctx, AV_LOG_VERBOSE, "Stats time is set to %d\n", stats_time_in_sec);
        }
    }

    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
        {
            switch (rc_mode) {
            case MPP_ENC_RC_MODE_FIXQP:
                qp_init = r->qp_init >= 0 ? r->qp_init : 26;
                if (r->roi) {
                    /*
                     * The hardware clamps per-region ROI QP to the rc
                     * [qp_min, qp_max] range; widen it so ROI targets can
                     * take effect. The base frame QP stays fixed at qp_init.
                     */
                    qp_max = r->qp_max >= 0 ? r->qp_max : 51;
                    qp_min = FFMIN(r->qp_min >= 0 ? r->qp_min : 0, qp_max);
                    qp_max_i = r->qp_max_i >= 0 ? r->qp_max_i : qp_max;
                    qp_min_i = FFMIN(r->qp_min_i >= 0 ? r->qp_min_i : qp_min, qp_max_i);
                } else {
                    qp_max = qp_min = qp_max_i = qp_min_i = qp_init;
                }
                mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 0);
                if (r->roi)
                    av_log(avctx, AV_LOG_VERBOSE,
                           "Note: on some SoCs (e.g. rk3588) the CQP mode pins the "
                           "per-frame QP range to qp_init, so ROI QP adjustments only "
                           "take effect with VBR/CBR/AVBR\n");
                break;
            case MPP_ENC_RC_MODE_CBR:
            case MPP_ENC_RC_MODE_VBR:
            case MPP_ENC_RC_MODE_AVBR:
                qp_max = r->qp_max >= 0 ? r->qp_max : 48;
                qp_min = FFMIN(r->qp_min >= 0 ? r->qp_min : 0, qp_max);
                qp_max_i = r->qp_max_i >= 0 ? r->qp_max_i : 48;
                qp_min_i = FFMIN(r->qp_min_i >= 0 ? r->qp_min_i : 0, qp_max_i);
                qp_init = FFMIN3(r->qp_init >= 0 ? r->qp_init : 26, qp_max, qp_max_i);
                mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
                break;
            default:
                return AVERROR(EINVAL);
            }
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qp_init);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qp_max);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qp_min);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i",qp_max_i);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qp_min_i);

            /* Intra Refresh / GDR */
            if (r->intra_refresh && r->refresh_num) {
                mpp_enc_cfg_set_u32(cfg, "rc:refresh_en", 1);
                mpp_enc_cfg_set_u32(cfg, "rc:refresh_mode", r->refresh_mode);
                mpp_enc_cfg_set_u32(cfg, "rc:refresh_num", r->refresh_num);
                av_log(avctx, AV_LOG_VERBOSE, "Requested to use Intra Refresh, "
                       "Mode/Num is set to %d/%d\n", r->refresh_mode, r->refresh_num);
            }
        }
        break;
    case AV_CODEC_ID_MJPEG:
        {
            qp_init = r->qp_init >= 1 ? r->qp_init : 80;
            qp_max = r->qp_max >= 1 ? r->qp_max : 99;
            qp_min = r->qp_min >= 1 ? r->qp_min : 1;
            qp_max_i = qp_min_i = 0;
            /* jpeg use special codec config to control qtable */
            mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", qp_init);
            mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", qp_max);
            mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", qp_min);
        }
        break;
    default:
        return AVERROR(EINVAL);
    }

    av_log(avctx, AV_LOG_VERBOSE, "QP Init/Max/Min/Max_I/Min_I is set to %d/%d/%d/%d/%d\n",
           qp_init, qp_max, qp_min, qp_max_i, qp_min_i);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
        {
            avctx->profile = r->profile;
            avctx->level = r->level;
            mpp_enc_cfg_set_s32(cfg, "h264:profile", avctx->profile);
            mpp_enc_cfg_set_s32(cfg, "h264:level", avctx->level);
            mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", r->coder);
            mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
            mpp_enc_cfg_set_s32(cfg, "h264:trans8x8",
                                (r->dct8x8 && avctx->profile == AV_PROFILE_H264_HIGH));

            mpp_enc_cfg_set_s32(cfg, "h264:prefix_mode", r->prefix_mode);

            switch (avctx->profile) {
            case AV_PROFILE_H264_BASELINE:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to BASELINE\n"); break;
            case AV_PROFILE_H264_MAIN:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to MAIN\n"); break;
            case AV_PROFILE_H264_HIGH:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to HIGH\n");
                if (r->dct8x8)
                    av_log(avctx, AV_LOG_VERBOSE, "8x8 Transform is enabled\n");
                break;
            }
            av_log(avctx, AV_LOG_VERBOSE, "Level is set to %d\n", avctx->level);
            av_log(avctx, AV_LOG_VERBOSE, "Coder is set to %s\n", r->coder ? "CABAC" : "CAVLC");
        }
        break;
    case AV_CODEC_ID_HEVC:
        {
            avctx->profile = r->pix_fmt == AV_PIX_FMT_GRAY8
                ? AV_PROFILE_HEVC_REXT : AV_PROFILE_HEVC_MAIN;
            avctx->level = r->level;
            mpp_enc_cfg_set_s32(cfg, "h265:profile", avctx->profile);
            mpp_enc_cfg_set_s32(cfg, "h265:level", avctx->level);
            if (avctx->level >= 120) {
                mpp_enc_cfg_set_s32(cfg, "h265:tier", r->tier);
                av_log(avctx, AV_LOG_VERBOSE, "Tier is set to %d\n", r->tier);
            }

            switch (avctx->profile) {
            case AV_PROFILE_HEVC_MAIN:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to MAIN\n"); break;
            case AV_PROFILE_HEVC_REXT:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to REXT\n"); break;
            }
            av_log(avctx, AV_LOG_VERBOSE, "Level is set to %d\n", avctx->level / 3);
        }
        break;
    case AV_CODEC_ID_MJPEG:
        break;
    default:
        return AVERROR(EINVAL);
    }

    if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_CFG, cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if (avctx->codec_id == AV_CODEC_ID_H264 ||
        avctx->codec_id == AV_CODEC_ID_HEVC) {
        sei_mode = (r->udu_sei || (r->intra_refresh && r->refresh_num))
                   ? MPP_ENC_SEI_MODE_ONE_FRAME : MPP_ENC_SEI_MODE_DISABLE;
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_SEI_CFG, &sei_mode)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set SEI config: %d\n", ret);
            return AVERROR_EXTERNAL;
        }

        header_mode = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
                      ? MPP_ENC_HEADER_MODE_DEFAULT : MPP_ENC_HEADER_MODE_EACH_IDR;
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_HEADER_MODE, &header_mode)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set header mode: %d\n", ret);
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

static int rkmpp_prepare_udu_sei_data(AVCodecContext *avctx, MPPEncFrame *mpp_enc_frame)
{
    int i, ret, sei_count = 0;

    if (!mpp_enc_frame ||
        !mpp_enc_frame->frame ||
        !mpp_enc_frame->mpp_frame)
        return AVERROR(EINVAL);

    /* user data unregistered SEI of H26X */
    for (i = 0; i < mpp_enc_frame->frame->nb_side_data; i++) {
        MppEncUserDataSet *mpp_sei_set = &mpp_enc_frame->mpp_sei_set;
        AVFrameSideData *sd = mpp_enc_frame->frame->side_data[i];
        uint8_t *user_data = sd->data;
        void *buf = NULL;

        if (sd->type != AV_FRAME_DATA_SEI_UNREGISTERED)
            continue;

        if (sd->size < AV_UUID_LEN) {
            av_log(avctx, AV_LOG_WARNING, "Invalid UDU SEI data: "
                   "(%zu < UUID(%d-bytes)), skipping\n",
                   sd->size, AV_UUID_LEN);
            continue;
        }

        buf = av_fast_realloc(mpp_sei_set->datas,
                              &mpp_sei_set->count,
                              (sei_count + 1) * sizeof(*(mpp_sei_set->datas)));
        if (!buf) {
            av_log(avctx, AV_LOG_ERROR, "Failed to realloc UDU SEI buffer\n");
            return AVERROR(ENOMEM);
        } else {
            mpp_sei_set->datas = (MppEncUserDataFull *)buf;

            mpp_sei_set->datas[sei_count].len   = sd->size - AV_UUID_LEN;
            mpp_sei_set->datas[sei_count].uuid  = (RK_U8 *)user_data;
            mpp_sei_set->datas[sei_count].pdata = &user_data[AV_UUID_LEN];

            mpp_sei_set->count = ++sei_count;
        }
    }

    if (sei_count > 0) {
        MppMeta mpp_meta = mpp_frame_get_meta(mpp_enc_frame->mpp_frame);
        if (!mpp_meta) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame meta\n");
            return AVERROR_EXTERNAL;
        }
        if ((ret = mpp_meta_set_ptr(mpp_meta, KEY_USER_DATAS,
                                    &mpp_enc_frame->mpp_sei_set)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set the UDU SEI datas ptr\n");
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

#define RKMPP_MAX_ROI_REGIONS 8

static void rkmpp_enc_read_soc_name(AVCodecContext *avctx, char *name, int size)
{
    const char *dt_path = "/proc/device-tree/compatible";
    int fd = open(dt_path, O_RDONLY);

    snprintf(name, size - 1, "unknown");
    if (fd < 0)
        return;

    ssize_t len = read(fd, name, size - 1);
    if (len > 0) {
        name[len] = '\0';
        /* replace embedded NULs between compatible strings with spaces */
        for (ssize_t i = 0; i < len - 1; i++)
            if (!name[i])
                name[i] = ' ';
        av_log(avctx, AV_LOG_VERBOSE, "Found SoC name from device-tree: '%s'\n", name);
    }
    close(fd);
}

/*
 * TYPE_2 ROI data (KEY_ROI_DATA2) consumed by the vepu580 HAL.
 *
 * The buffer layouts below describe the hardware register interface of the
 * vepu580 encoder core (see the roi_en/roi_addr handling in the MPP vepu580
 * HAL); the cu16/cu8 z-order indexing follows the scan order defined by the
 * HEVC specification. Implemented independently of Rockchip's ROI utility.
 *
 * base cfg buffer (bit-addressed, little-endian), per 16x16 MB (h264) or
 * per 64x64 CTU (hevc):
 *   h264: 64 bits/MB - bit 61 force_intra, bit 62 qp_adj_en
 *   hevc: 512 bits/CTU with 85 entries at four hierarchy levels
 *         (cu8 0-63, cu16 64-79, cu32 80-83, cu64 84):
 *           force_inter 2b @ 2*e, force_intra 2b @ 170+2*e,
 *           force_split 1b @ 340+e, qp_adj 1b @ 425+e
 * qp cfg buffer, u16 entries at the same indices:
 *   bits 15 qp_adj_mode (0 = relative), bits 8-14 qp_adj (s7),
 *   bits 4-7 qp_area_idx
 * Sizes required by the HAL (per frame):
 *   h264: base mb_w*mb_h*8, qp mb_w*mb_h*2   (mb grid aligned to 64)
 *   hevc: base ctu_w*ctu_h*64, qp ctu_w*ctu_h*256
 *
 * Hardware constraints (verified on rk3588 / vepu580):
 *   - the qp adjustment only takes effect in absolute mode (qp_adj_mode = 1);
 *     relative adjustments are ignored
 *   - the applied QP is clamped to the rate control [qp_min, qp_max] range
 */
typedef struct RKMPPRoiCell {
    int16_t qp_adj;
    uint8_t qp_mode; /* 0 relative, 1 absolute */
    uint8_t force_intra;
    uint8_t written;
} RKMPPRoiCell;

typedef struct RKMPPRoiRegion {
    int x, y, w, h;
    int qp_adj;
} RKMPPRoiRegion;

static av_always_inline void roi_buf_set_bit(uint32_t *buf, uint32_t pos, uint32_t val)
{
    buf[pos >> 5] |= (val & 1u) << (pos & 31);
}

/* Morton/z-scan order of (x,y) within an n-by-n block, n a power of two */
static av_always_inline uint32_t roi_zscan(uint32_t x, uint32_t y, int n)
{
    uint32_t z = 0;
    for (int i = 0; i < n; i++)
        z |= ((x >> i) & 1u) << (2 * i) | ((y >> i) & 1u) << (2 * i + 1);
    return z;
}

static void roi_h264_set_entry(uint32_t *base, uint16_t *qp, int idx,
                               const RKMPPRoiCell *cell)
{
    uint64_t v = 0;

    if (cell->force_intra)
        v |= UINT64_C(1) << 61;
    if (cell->qp_adj)
        v |= UINT64_C(1) << 62;
    base[idx * 2]     = (uint32_t)v;
    base[idx * 2 + 1] = (uint32_t)(v >> 32);

    qp[idx] = ((uint16_t)(cell->qp_adj & 0x7f) << 8) |
              ((uint16_t)cell->qp_mode << 15);
}

static void roi_hevc_set_entry(uint32_t *base, uint16_t *qp, int idx,
                               const RKMPPRoiCell *cell)
{
    int fi = cell->force_intra ? 1 : 0;
    int qa = cell->qp_adj ? 1 : 0;

    roi_buf_set_bit(base, 170 + idx * 2,     fi & 1);
    roi_buf_set_bit(base, 170 + idx * 2 + 1, (fi >> 1) & 1);
    roi_buf_set_bit(base, 425 + idx,         qa);
    qp[idx] = ((uint16_t)(cell->qp_adj & 0x7f) << 8) |
              ((uint16_t)cell->qp_mode << 15);
}

static void rkmpp_gen_roi_type2_h264(RKMPPEncContext *r,
                                     uint32_t *base, uint16_t *qp,
                                     const RKMPPRoiCell *cells)
{
    for (int j = 0; j < r->roi_mb_h; j++) {
        for (int k = 0; k < r->roi_mb_w; k++) {
            const RKMPPRoiCell *cell = &cells[j * r->roi_stride_h + k];
            if (cell->written)
                roi_h264_set_entry(base, qp, j * r->roi_stride_h + k, cell);
        }
    }
}

static void rkmpp_gen_roi_type2_hevc(RKMPPEncContext *r,
                                     uint32_t *base, uint16_t *qp,
                                     const RKMPPRoiCell *cells,
                                     const uint8_t *ctus)
{
    const int cu16_line = r->roi_ctu_w * 4;

    for (int cy = 0; cy < r->roi_ctu_h; cy++) {
        for (int cx = 0; cx < r->roi_ctu_w; cx++) {
            uint32_t *b = base + (cy * r->roi_ctu_w + cx) * 16; /* 64B/CTU */
            uint16_t *q = qp   + (cy * r->roi_ctu_w + cx) * 128; /* 256B/CTU */
            int adjust_cnt = 0;
            int all_adj = 1;

            if (!ctus[cy * r->roi_ctu_w + cx])
                continue;

            for (int c = 0; c < 16; c++) {
                int c16x = cx * 4 + (c & 3);
                int c16y = cy * 4 + (c / 4);
                const RKMPPRoiCell *cell = &cells[c16y * cu16_line + c16x];
                uint32_t z16 = roi_zscan(c & 3, c / 4, 2);
                int adj = (cell->written && (cell->force_intra || cell->qp_adj));

                adjust_cnt += adj;
                all_adj    &= adj;
                if (!cell->written)
                    continue;

                /* each cu16 splits into four cu8 entries */
                for (int c8 = 0; c8 < 4; c8++) {
                    int rx = (c & 3) * 2 + (c8 & 1);
                    int ry = (c / 4) * 2 + (c8 / 2);
                    roi_hevc_set_entry(b, q, roi_zscan(rx, ry, 3), cell);
                }
                roi_hevc_set_entry(b, q, 64 + z16, cell);
            }

            /* propagate to cu32/cu64 when the whole CTU is adjusted */
            if (adjust_cnt == 16 && all_adj) {
                const RKMPPRoiCell *cell = &cells[cy * 4 * cu16_line + cx * 4];
                for (int i = 0; i < 4; i++)
                    roi_hevc_set_entry(b, q, 80 + i, cell);
                roi_hevc_set_entry(b, q, 84, cell);
            } else if (adjust_cnt > 0) {
                /* force split down to cu16 so per-cu16 qp applies */
                roi_buf_set_bit(b, 340 + 84, 1);
                for (int i = 0; i < 4; i++)
                    roi_buf_set_bit(b, 340 + 80 + i, 1);
                for (int i = 0; i < 16; i++)
                    roi_buf_set_bit(b, 340 + 64 + i, 1);
            }
        }
    }
}

static int rkmpp_alloc_roi_type2_bufs(AVCodecContext *avctx, MPPEncFrame *enc_frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    int ret;

    if (enc_frame->mpp_roi_base_buf && enc_frame->mpp_roi_qp_buf)
        return 0;

    if (!r->roi_buf_grp) {
        /* ION + cachable, matching how MPP allocates its ROI config buffers */
        ret = mpp_buffer_group_get_internal(&r->roi_buf_grp,
                                            MPP_BUFFER_TYPE_ION |
                                            MPP_BUFFER_FLAGS_CACHABLE);
        if (ret != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get ROI buffer group: %d\n", ret);
            return AVERROR_EXTERNAL;
        }
    }

    if (!enc_frame->mpp_roi_base_buf) {
        ret = mpp_buffer_get(r->roi_buf_grp, &enc_frame->mpp_roi_base_buf,
                             r->roi_base_size);
        if (ret != MPP_OK || !enc_frame->mpp_roi_base_buf) {
            av_log(avctx, AV_LOG_ERROR, "Failed to alloc ROI base cfg buffer: %d\n", ret);
            return AVERROR_EXTERNAL;
        }
    }
    if (!enc_frame->mpp_roi_qp_buf) {
        ret = mpp_buffer_get(r->roi_buf_grp, &enc_frame->mpp_roi_qp_buf,
                             r->roi_qp_size);
        if (ret != MPP_OK || !enc_frame->mpp_roi_qp_buf) {
            av_log(avctx, AV_LOG_ERROR, "Failed to alloc ROI qp cfg buffer: %d\n", ret);
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

static int rkmpp_prepare_roi_data_type2(AVCodecContext *avctx,
                                        MPPEncFrame *mpp_enc_frame,
                                        const uint8_t *sd_data, size_t sd_size)
{
    RKMPPEncContext *r = avctx->priv_data;
    RKMPPRoiRegion regions[RKMPP_MAX_ROI_REGIONS];
    RKMPPRoiCell *cells = NULL;
    uint8_t *ctus = NULL;
    uint32_t *base = NULL;
    uint16_t *qp = NULL;
    uint32_t self_size;
    int cell_w, cell_h, nb_regions = 0, n = 0;
    int base_qp;
    int ret = 0;

    /*
     * The vepu580 ROI qp adjustment only takes effect in absolute qp mode
     * (qp_adj_mode = 1); relative mode is ignored by the hardware. Convert
     * the qoffset to an absolute target around the encoder's base QP.
     */
    if (r->qp_init >= 0)
        base_qp = r->qp_init;
    else if (r->qp_min >= 0 && r->qp_max >= 0)
        base_qp = (r->qp_min + r->qp_max) / 2;
    else
        base_qp = 26;

    self_size = ((const AVRegionOfInterest *)sd_data)->self_size;
    nb_regions = sd_size / self_size;
    if (nb_regions > RKMPP_MAX_ROI_REGIONS) {
        av_log(avctx, AV_LOG_WARNING,
               "Too many ROI regions (%d), keeping the first %d\n",
               nb_regions, RKMPP_MAX_ROI_REGIONS);
        nb_regions = RKMPP_MAX_ROI_REGIONS;
    }

    /* When regions overlap the first one applies, so iterate in reverse */
    for (int i = nb_regions - 1; i >= 0; i--) {
        const AVRegionOfInterest *roi =
            (const AVRegionOfInterest *)(sd_data + (size_t)i * self_size);
        RKMPPRoiRegion *rg = &regions[n];

        rg->x = av_clip(roi->left, 0, avctx->width);
        rg->y = av_clip(roi->top,  0, avctx->height);
        rg->w = av_clip(roi->right  - roi->left, 0, avctx->width  - rg->x);
        rg->h = av_clip(roi->bottom - roi->top,  0, avctx->height - rg->y);
        if (!rg->w || !rg->h)
            continue;

        rg->qp_adj = roi->qoffset.den ?
            av_clip(llrint(av_q2d(roi->qoffset) * 51), -51, 51) : 0;
        n++;
    }
    if (!n)
        return 0;

    if ((ret = rkmpp_alloc_roi_type2_bufs(avctx, mpp_enc_frame)) < 0)
        return ret;

    base = (uint32_t *)mpp_buffer_get_ptr(mpp_enc_frame->mpp_roi_base_buf);
    qp   = (uint16_t *)mpp_buffer_get_ptr(mpp_enc_frame->mpp_roi_qp_buf);
    if (!base || !qp)
        return AVERROR_EXTERNAL;
    memset(base, 0, r->roi_base_size);
    memset(qp, 0, r->roi_qp_size);

    if (r->roi_is_hevc) {
        cell_w = r->roi_ctu_w * 4;
        cell_h = r->roi_ctu_h * 4;
    } else {
        cell_w = r->roi_stride_h;
        cell_h = r->roi_stride_v;
    }

    cells = av_calloc(cell_w * cell_h, sizeof(*cells));
    ctus  = r->roi_is_hevc ? av_calloc(r->roi_ctu_w * r->roi_ctu_h, 1) : NULL;
    if (!cells || (r->roi_is_hevc && !ctus)) {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    /* fill the maps, cells are claimed by the first (earliest) region */
    for (int i = 0; i < n; i++) {
        const RKMPPRoiRegion *rg = &regions[i];
        int x0 = rg->x / 16, y0 = rg->y / 16;
        int x1 = (rg->x + rg->w - 1) / 16, y1 = (rg->y + rg->h - 1) / 16;

        for (int y = y0; y <= y1 && y < cell_h; y++) {
            for (int x = x0; x <= x1 && x < cell_w; x++) {
                RKMPPRoiCell *cell = &cells[y * cell_w + x];
                if (cell->written)
                    continue;
                cell->written      = 1;
                cell->qp_adj       = av_clip(base_qp + rg->qp_adj, 0, 51);
                cell->qp_mode      = 1;
                cell->force_intra  = 0;
            }
        }

        if (r->roi_is_hevc) {
            int tx0 = rg->x / 64, ty0 = rg->y / 64;
            int tx1 = (rg->x + rg->w - 1) / 64, ty1 = (rg->y + rg->h - 1) / 64;

            for (int y = ty0; y <= ty1 && y < r->roi_ctu_h; y++)
                for (int x = tx0; x <= tx1 && x < r->roi_ctu_w; x++)
                    ctus[y * r->roi_ctu_w + x] = 1;
        }
    }

    if (r->roi_is_hevc)
        rkmpp_gen_roi_type2_hevc(r, base, qp, cells, ctus);
    else
        rkmpp_gen_roi_type2_h264(r, base, qp, cells);

    mpp_buffer_sync_ro_end(mpp_enc_frame->mpp_roi_base_buf);
    mpp_buffer_sync_ro_end(mpp_enc_frame->mpp_roi_qp_buf);

    memset(&mpp_enc_frame->mpp_roi_cfg2, 0, sizeof(mpp_enc_frame->mpp_roi_cfg2));
    mpp_enc_frame->mpp_roi_cfg2.base_cfg_buf = mpp_enc_frame->mpp_roi_base_buf;
    mpp_enc_frame->mpp_roi_cfg2.qp_cfg_buf   = mpp_enc_frame->mpp_roi_qp_buf;
    mpp_enc_frame->mpp_roi_cfg2.roi_qp_en    = 1;

    {
        MppMeta mpp_meta = mpp_frame_get_meta(mpp_enc_frame->mpp_frame);
        if (!mpp_meta) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame meta\n");
            ret = AVERROR_EXTERNAL;
            goto done;
        }
        if ((ret = mpp_meta_set_ptr(mpp_meta, KEY_ROI_DATA2,
                                    &mpp_enc_frame->mpp_roi_cfg2)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set the ROI data2 ptr: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto done;
        }
    }

done:
    av_freep(&cells);
    av_freep(&ctus);
    return ret;
}

static int rkmpp_prepare_roi_data(AVCodecContext *avctx, MPPEncFrame *mpp_enc_frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    AVFrameSideData *sd = NULL;
    const AVRegionOfInterest *roi = NULL;
    MppEncROIRegion *regions = NULL;
    MppMeta mpp_meta = NULL;
    uint32_t self_size = 0;
    int i, nb_regions, n = 0;
    int ret;

    if (!mpp_enc_frame ||
        !mpp_enc_frame->frame ||
        !mpp_enc_frame->mpp_frame)
        return AVERROR(EINVAL);

    sd = av_frame_get_side_data(mpp_enc_frame->frame,
                                AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (!sd)
        return 0;

    if (sd->size < sizeof(uint32_t))
        return AVERROR_INVALIDDATA;

    self_size = ((const AVRegionOfInterest *)sd->data)->self_size;
    if (!self_size || sd->size % self_size) {
        av_log(avctx, AV_LOG_WARNING, "Invalid Regions Of Interest side data, skipping\n");
        return 0;
    }

    /* vepu580 (rk3588) only consumes the TYPE_2 format */
    if (r->roi_data_mode)
        return rkmpp_prepare_roi_data_type2(avctx, mpp_enc_frame, sd->data, sd->size);

    nb_regions = sd->size / self_size;
    if (nb_regions > RKMPP_MAX_ROI_REGIONS) {
        av_log(avctx, AV_LOG_WARNING,
               "Too many ROI regions (%d), keeping the first %d\n",
               nb_regions, RKMPP_MAX_ROI_REGIONS);
        nb_regions = RKMPP_MAX_ROI_REGIONS;
    }

    regions = av_calloc(nb_regions, sizeof(*regions));
    if (!regions)
        return AVERROR(ENOMEM);

    /* When regions overlap the first one applies, so iterate in reverse */
    for (i = nb_regions - 1; i >= 0; i--) {
        roi = (const AVRegionOfInterest *)(sd->data + (size_t)i * self_size);

        regions[n].x = av_clip(roi->left, 0, avctx->width);
        regions[n].y = av_clip(roi->top,  0, avctx->height);
        regions[n].w = av_clip(roi->right  - roi->left, 0, avctx->width  - regions[n].x);
        regions[n].h = av_clip(roi->bottom - roi->top,  0, avctx->height - regions[n].y);
        if (!regions[n].w || !regions[n].h)
            continue;

        regions[n].intra       = 0;
        regions[n].abs_qp_en   = 0; /* relative qp */
        regions[n].area_map_en = 1;
        regions[n].qp_area_idx = 0;
        regions[n].quality     = roi->qoffset.den ?
            av_clip(llrint(av_q2d(roi->qoffset) * 51), -51, 51) : 0;

        n++;
    }

    if (!n) {
        av_freep(&regions);
        return 0;
    }

    mpp_enc_frame->mpp_roi_regions  = regions;
    mpp_enc_frame->mpp_roi_cfg.number  = n;
    mpp_enc_frame->mpp_roi_cfg.regions = regions;

    mpp_meta = mpp_frame_get_meta(mpp_enc_frame->mpp_frame);
    if (!mpp_meta) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame meta\n");
        return AVERROR_EXTERNAL;
    }
    if ((ret = mpp_meta_set_ptr(mpp_meta, KEY_ROI_DATA,
                                &mpp_enc_frame->mpp_roi_cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set the ROI data ptr: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static MPPEncFrame *rkmpp_submit_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppFrame mpp_frame = NULL;
    MppBuffer mpp_buf = NULL;
    AVFrame *drm_frame = NULL;
    const AVDRMFrameDescriptor *drm_desc;
    const AVDRMLayerDescriptor *layer;
    const AVDRMPlaneDescriptor *plane0;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(r->pix_fmt);
    const int is_planar = pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR;
    const int is_rgb = pix_desc->flags & AV_PIX_FMT_FLAG_RGB;
    const int is_yuv = !is_rgb && pix_desc->nb_components >= 2;
    int hor_stride = 0, ver_stride = 0;
    MppBufferInfo buf_info = { 0 };
    MppFrameFormat mpp_fmt = r->mpp_fmt;
    int ret, is_afbc = 0;

    MPPEncFrame *mpp_enc_frame = NULL;

    clear_unused_frames(r->frame_list);

    mpp_enc_frame = get_free_frame(&r->frame_list);
    if (!mpp_enc_frame)
        return NULL;

    if ((ret = mpp_frame_init(&mpp_frame)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP frame: %d\n", ret);
        goto exit;
    }
    mpp_enc_frame->mpp_frame = mpp_frame;

    if (!frame) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream\n");
        mpp_frame_set_eos(mpp_frame, 1);
        return mpp_enc_frame;
    }

    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        drm_frame = frame;
        mpp_enc_frame->frame = av_frame_clone(drm_frame);
    } else {
        AVBufferRef *hw_frames_ctx = frame->hw_frames_ctx;

        drm_frame = av_frame_alloc();
        if (!drm_frame) {
            goto exit;
        }
        if ((ret = av_hwframe_get_buffer(r->hwframe, drm_frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot allocate an internal frame: %d\n", ret);
            goto exit;
        }
        frame->hw_frames_ctx = NULL; /* clear hwfc to avoid HW -> HW transfer */
        if ((ret = av_hwframe_transfer_data(drm_frame, frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_hwframe_transfer_data failed: %d\n", ret);
            frame->hw_frames_ctx = hw_frames_ctx;
            goto exit;
        }
        if ((ret = av_frame_copy_props(drm_frame, frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
            frame->hw_frames_ctx = hw_frames_ctx;
            goto exit;
        }
        mpp_enc_frame->frame = drm_frame;
        frame->hw_frames_ctx = hw_frames_ctx; /* restore hwfc */
    }

    drm_desc = (AVDRMFrameDescriptor *)drm_frame->data[0];
    if (drm_desc->objects[0].fd < 0)
        goto exit;

    /* planar YUV quirks */
    if ((r->pix_fmt == AV_PIX_FMT_YUV420P ||
         r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         r->pix_fmt == AV_PIX_FMT_YUV422P ||
         r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
         r->pix_fmt == AV_PIX_FMT_NV24) && (drm_frame->width % 2)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported width '%d', not 2-aligned\n",
               drm_frame->width);
        goto exit;
    }
    /* packed RGB/YUV quirks */
    if ((is_rgb || (is_yuv && !is_planar)) &&
        (drm_frame->width % 2 || drm_frame->height % 2)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported size '%dx%d', not 2-aligned\n",
               drm_frame->width, drm_frame->height);
        goto exit;
    }

    mpp_frame_set_pts(mpp_frame, PTS_TO_MPP_PTS(drm_frame->pts, avctx->time_base));
    mpp_frame_set_width(mpp_frame, drm_frame->width);
    mpp_frame_set_height(mpp_frame, drm_frame->height);

    mpp_frame_set_colorspace(mpp_frame, avctx->colorspace);
    mpp_frame_set_color_primaries(mpp_frame, avctx->color_primaries);
    mpp_frame_set_color_trc(mpp_frame, avctx->color_trc);

    mpp_frame_set_color_range(mpp_frame, avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_frame_set_color_range(mpp_frame, AVCOL_RANGE_JPEG);
    }

    layer = &drm_desc->layers[0];
    plane0 = &layer->planes[0];

    is_afbc = drm_is_afbc(drm_desc->objects[0].format_modifier);
    if (!is_afbc &&
        drm_desc->objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR) {
        av_log(avctx, AV_LOG_ERROR, "Only linear and AFBC modifiers are supported\n");
        goto exit;
    }
    if (is_afbc &&
        !(avctx->codec_id == AV_CODEC_ID_H264 ||
          avctx->codec_id == AV_CODEC_ID_HEVC)) {
        av_log(avctx, AV_LOG_ERROR, "AFBC is not supported in codec '%s'\n",
               avcodec_get_name(avctx->codec_id));
        goto exit;
    }
    if (is_afbc) {
        uint32_t drm_afbc_fmt = rkmpp_get_drm_afbc_format(mpp_fmt);
        int afbc_offset_y = 0;

        if (drm_afbc_fmt != layer->format) {
            av_log(avctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported\n",
                   av_get_pix_fmt_name(r->pix_fmt));
            goto exit;
        }
        mpp_fmt |= MPP_FRAME_FBC_AFBC_V2;

        if (drm_frame->crop_top > 0) {
            afbc_offset_y = drm_frame->crop_top;
            mpp_frame_set_offset_y(mpp_frame, afbc_offset_y);
        }
    }
    mpp_frame_set_fmt(mpp_frame, mpp_fmt);

    if (is_afbc) {
        hor_stride = plane0->pitch;
        if ((ret = get_afbc_byte_stride(pix_desc, &hor_stride, 1)) < 0)
            goto exit;

        if (hor_stride % 16)
            hor_stride = FFALIGN(avctx->width, 16);

        mpp_frame_set_fbc_hdr_stride(mpp_frame, hor_stride);
    } else {
        ret = get_byte_stride(&drm_desc->objects[0],
                              &drm_desc->layers[0],
                              (pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                              (pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                              &hor_stride, &ver_stride);
        if (ret < 0 || !hor_stride || !ver_stride) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            goto exit;
        }

        mpp_frame_set_hor_stride(mpp_frame, hor_stride);
        mpp_frame_set_ver_stride(mpp_frame, ver_stride);
    }

    buf_info.type  = MPP_BUFFER_TYPE_DRM;
    buf_info.fd    = drm_desc->objects[0].fd;
    buf_info.size  = drm_desc->objects[0].size;

    /* mark buffer as used (idx >= 0) */
    buf_info.index = buf_info.fd;

    if ((ret = mpp_buffer_import(&mpp_buf, &buf_info)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to import MPP buffer: %d\n", ret);
        goto exit;
    }
    mpp_frame_set_buffer(mpp_frame, mpp_buf);
    mpp_frame_set_buf_size(mpp_frame, drm_desc->objects[0].size);

    if (r->udu_sei &&
        (avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC)) {
        ret = rkmpp_prepare_udu_sei_data(avctx, mpp_enc_frame);
        if (ret < 0)
            goto exit;
    }

    if (r->roi &&
        (avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC)) {
        ret = rkmpp_prepare_roi_data(avctx, mpp_enc_frame);
        if (ret < 0)
            goto exit;
    }

    return mpp_enc_frame;

exit:
    if (drm_frame &&
        avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME)
        av_frame_free(&drm_frame);

    return NULL;
}

static int rkmpp_send_frame(AVCodecContext *avctx, MPPEncFrame *mpp_enc_frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    AVFrame *frame = NULL;
    MppFrame mpp_frame = NULL;
    int ret;

    if (mpp_enc_frame) {
        frame = mpp_enc_frame->frame;
        mpp_frame = mpp_enc_frame->mpp_frame;
    }

    if (frame && (ret = rkmpp_set_enc_cfg_prep(avctx, frame)) < 0)
        goto exit;

    if ((avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC) &&
         frame && frame->pict_type == AV_PICTURE_TYPE_I) {
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_IDR_FRAME, NULL)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set IDR frame: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto exit;
        }
    }

    if ((ret = r->mapi->encode_put_frame(r->mctx, mpp_frame)) != MPP_OK) {
        int log_level = (ret == MPP_NOK) ? AV_LOG_DEBUG : AV_LOG_ERROR;
        ret = (ret == MPP_NOK) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
        av_log(avctx, log_level, "Failed to put frame to encoder input queue: %d\n", ret);
        goto exit;
    } else
        av_log(avctx, AV_LOG_DEBUG, "Wrote %ld bytes to encoder\n",
               mpp_frame_get_buf_size(mpp_frame));

exit:
    return ret;
}

static int rkmpp_get_packet(AVCodecContext *avctx, AVPacket *packet, int timeout)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppPacket mpp_pkt = NULL;
    MppMeta mpp_meta = NULL;
    MppFrame mpp_frame = NULL;
    MppBuffer mpp_buf = NULL;
    int key_frame = 0;
    int avg_qp = -1;
    int ret;

    if ((ret = r->mapi->control(r->mctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = r->mapi->encode_get_packet(r->mctx, &mpp_pkt)) != MPP_OK) {
        int log_level = (ret == MPP_NOK) ? AV_LOG_DEBUG : AV_LOG_ERROR;
        ret = (ret == MPP_NOK) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
        av_log(avctx, log_level, "Failed to get packet from encoder output queue: %d\n", ret);
        return ret;
    }
    if (!mpp_pkt)
        return AVERROR(ENOMEM);

    if (mpp_packet_get_eos(mpp_pkt)) {
        av_log(avctx, AV_LOG_DEBUG, "Received an EOS packet\n");
        ret = AVERROR_EOF;
        goto exit;
    }
    av_log(avctx, AV_LOG_DEBUG, "Received a packet\n");

    /* freeing MppPacket data in buffer callbacks is not supported in async mode */
    {
        size_t mpp_pkt_length = mpp_packet_get_length(mpp_pkt);

        if ((ret = ff_get_encode_buffer(avctx, packet, mpp_pkt_length, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_get_encode_buffer failed: %d\n", ret);
            goto exit;
        }
        memcpy(packet->data, mpp_packet_get_data(mpp_pkt), mpp_pkt_length);
    }

    packet->time_base.num = avctx->time_base.num;
    packet->time_base.den = avctx->time_base.den;
    packet->pts = MPP_PTS_TO_PTS(mpp_packet_get_pts(mpp_pkt), avctx->time_base);
    packet->dts = packet->pts;

    mpp_meta = mpp_packet_get_meta(mpp_pkt);
    if (!mpp_meta || !mpp_packet_has_meta(mpp_pkt)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get packet meta\n");
        ret = AVERROR_EXTERNAL;
        goto exit;
    }

    mpp_meta_get_s32(mpp_meta, KEY_OUTPUT_INTRA, &key_frame);
    if (key_frame)
        packet->flags |= AV_PKT_FLAG_KEY;

    mpp_meta_get_s32(mpp_meta, KEY_ENC_AVERAGE_QP, &avg_qp);
    if (avg_qp >= 0)
        ff_encode_add_stats_side_data(packet, avg_qp * FF_QP2LAMBDA, NULL, 0,
                                      (packet->flags & AV_PKT_FLAG_KEY) ?
                                      AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P);

    if ((ret = mpp_meta_get_frame(mpp_meta, KEY_INPUT_FRAME, &mpp_frame)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get key input frame from packet meta: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto exit;
    }

    mpp_buf = mpp_frame_get_buffer(mpp_frame);
    if (!mpp_buf) {
        ret = AVERROR(ENOMEM);
        goto exit;
    }

    /* mark buffer as unused (idx < 0) */
    mpp_buffer_set_index(mpp_buf, -1);
    clear_unused_frames(r->frame_list);

    mpp_packet_deinit(&mpp_pkt);
    return 0;

exit:
    if (mpp_pkt)
        mpp_packet_deinit(&mpp_pkt);

    return ret;
}

static int rkmpp_encode_frame(AVCodecContext *avctx, AVPacket *packet,
                              const AVFrame *frame, int *got_packet)
{
    RKMPPEncContext *r = avctx->priv_data;
    MPPEncFrame *mpp_enc_frame = NULL;
    int ret;
    int timeout = (avctx->codec_id == AV_CODEC_ID_H264 ||
                   avctx->codec_id == AV_CODEC_ID_HEVC ||
                   avctx->codec_id == AV_CODEC_ID_MJPEG) &&
                   !(avctx->flags & AV_CODEC_FLAG_LOW_DELAY)
                   ? MPP_TIMEOUT_NON_BLOCK : MPP_TIMEOUT_BLOCK;

    if (get_used_frame_count(r->frame_list) > r->async_frames)
        goto get;

    mpp_enc_frame = rkmpp_submit_frame(avctx, (AVFrame *)frame);
    if (!mpp_enc_frame) {
        av_log(avctx, AV_LOG_ERROR, "Failed to submit frame on input\n");
        return AVERROR(ENOMEM);
    }

send:
    ret = rkmpp_send_frame(avctx, mpp_enc_frame);
    if (ret == AVERROR(EAGAIN))
        goto send;
    else if (ret)
        return ret;

get:
    ret = rkmpp_get_packet(avctx, packet, timeout);
    if (!frame && ret == AVERROR(EAGAIN))
        goto send;
    if (ret == AVERROR_EOF ||
        ret == AVERROR(EAGAIN))
        *got_packet = 0;
    else if (ret)
        return ret;
    else
        *got_packet = 1;

    return 0;
}

static av_cold int rkmpp_encode_close(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;

    r->cfg_init = 0;
    r->async_frames = 0;

    if (r->mcfg) {
        mpp_enc_cfg_deinit(r->mcfg);
        r->mcfg = NULL;
    }

    if (r->mapi) {
        r->mapi->reset(r->mctx);
        mpp_destroy(r->mctx);
        r->mctx = NULL;
    }

    clear_frame_list(&r->frame_list);

    if (r->roi_buf_grp) {
        mpp_buffer_group_put(r->roi_buf_grp);
        r->roi_buf_grp = NULL;
    }

    if (r->hwframe)
        av_buffer_unref(&r->hwframe);
    if (r->hwdevice)
        av_buffer_unref(&r->hwdevice);

    return 0;
}

static av_cold int init_hwframes_ctx(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    AVHWFramesContext *hwfc;
    int ret;

    av_buffer_unref(&r->hwframe);
    r->hwframe = av_hwframe_ctx_alloc(r->hwdevice);
    if (!r->hwframe)
        return AVERROR(ENOMEM);

    hwfc            = (AVHWFramesContext *)r->hwframe->data;
    hwfc->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc->sw_format = avctx->pix_fmt;
    hwfc->width     = avctx->width;
    hwfc->height    = avctx->height;

    ret = av_hwframe_ctx_init(r->hwframe);
    if (ret < 0) {
        av_buffer_unref(&r->hwframe);
        av_log(avctx, AV_LOG_ERROR, "Error creating internal frames_ctx: %d\n", ret);
        return ret;
    }

    return 0;
}

static av_cold int rkmpp_encode_init(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    MppFrameFormat mpp_fmt = MPP_FMT_BUTT;
    MppCodingType coding_type = MPP_VIDEO_CodingUnused;
    MppPacket mpp_pkt = NULL;
    int input_timeout = MPP_TIMEOUT_NON_BLOCK;
    int output_timeout = MPP_TIMEOUT_NON_BLOCK;
    int ret;

    r->cfg_init = 0;
    r->async_frames = 0;

    if ((coding_type = rkmpp_get_coding_type(avctx)) == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec id: %d\n", avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    pix_fmt = avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME ? avctx->sw_pix_fmt : avctx->pix_fmt;
    mpp_fmt = avctx->codec_id == AV_CODEC_ID_MJPEG
              ? rkmpp_get_mpp_fmt_mjpeg(pix_fmt) : rkmpp_get_mpp_fmt_h26x(pix_fmt);
    mpp_fmt &= MPP_FRAME_FMT_MASK;

    if (mpp_fmt == MPP_FMT_BUTT) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported input pixel format '%s'\n",
               av_get_pix_fmt_name(pix_fmt));
        return AVERROR(ENOSYS);
    }
    r->pix_fmt = pix_fmt;
    r->mpp_fmt = mpp_fmt;

    if ((ret = mpp_check_support_format(MPP_CTX_ENC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "MPP doesn't support encoding codec '%s' (%d)\n",
               avcodec_get_name(avctx->codec_id), avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    if ((ret = mpp_create(&r->mctx, &r->mapi)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = r->mapi->control(r->mctx, MPP_SET_INPUT_TIMEOUT,
                                (MppParam)&input_timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set input timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = r->mapi->control(r->mctx, MPP_SET_OUTPUT_TIMEOUT,
                                (MppParam)&output_timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = mpp_init(r->mctx, MPP_CTX_ENC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = mpp_enc_cfg_init(&r->mcfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init encoder config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = r->mapi->control(r->mctx, MPP_ENC_GET_CFG, r->mcfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get encoder config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = rkmpp_set_enc_cfg(avctx)) < 0)
        goto fail;

    if (avctx->codec_id == AV_CODEC_ID_H264 ||
        avctx->codec_id == AV_CODEC_ID_HEVC)
        r->async_frames = H26X_ASYNC_FRAMES;
    else if (avctx->codec_id == AV_CODEC_ID_MJPEG)
        r->async_frames = MJPEG_ASYNC_FRAMES;

    /*
     * ROI data format auto-selection:
     * the vepu580 encoder cores (rk3588) only consume KEY_ROI_DATA2,
     * other cores (vepu54x/51x) consume the legacy KEY_ROI_DATA.
     */
    if (r->roi &&
        (avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC)) {
        char soc_name[128] = { 0 };

        rkmpp_enc_read_soc_name(avctx, soc_name, sizeof(soc_name));
        if (strstr(soc_name, "rk3588")) {
            r->roi_data_mode = 1;
            r->roi_is_hevc   = (avctx->codec_id == AV_CODEC_ID_HEVC);

            if (r->roi_is_hevc) {
                r->roi_ctu_w = FFALIGN(avctx->width,  64) / 64;
                r->roi_ctu_h = FFALIGN(avctx->height, 64) / 64;
                r->roi_base_size = (size_t)r->roi_ctu_w * r->roi_ctu_h * 64;
                r->roi_qp_size   = (size_t)r->roi_ctu_w * r->roi_ctu_h * 256;
            } else {
                int mb16_w = FFALIGN(avctx->width,  16) / 16;
                int mb16_h = FFALIGN(avctx->height, 16) / 16;

                r->roi_mb_w     = FFALIGN(avctx->width,  64) / 16;
                r->roi_mb_h     = FFALIGN(avctx->height, 64) / 16;
                r->roi_stride_h = FFALIGN(mb16_w, 4);
                r->roi_stride_v = FFALIGN(mb16_h, 4);
                r->roi_base_size = (size_t)r->roi_mb_w * r->roi_mb_h * 8;
                r->roi_qp_size   = (size_t)r->roi_mb_w * r->roi_mb_h * 2;
            }
            av_log(avctx, AV_LOG_VERBOSE,
                   "ROI TYPE_2 (KEY_ROI_DATA2) selected for vepu580 cores\n");
        }
    }

    if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
        (avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC)) {
        RK_U8 enc_hdr_buf[H26X_HEADER_SIZE];
        size_t pkt_len = 0;
        void *pkt_pos = NULL;

        memset(enc_hdr_buf, 0, H26X_HEADER_SIZE);

        if ((ret = mpp_packet_init(&mpp_pkt,
                                   (void *)enc_hdr_buf,
                                   H26X_HEADER_SIZE)) != MPP_OK || !mpp_pkt) {
            av_log(avctx, AV_LOG_ERROR, "Failed to init extra info packet: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        mpp_packet_set_length(mpp_pkt, 0);
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_GET_HDR_SYNC, mpp_pkt)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get header sync: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        pkt_pos = mpp_packet_get_pos(mpp_pkt);
        pkt_len = mpp_packet_get_length(mpp_pkt);

        if (avctx->extradata) {
            av_free(avctx->extradata);
            avctx->extradata = NULL;
        }
        avctx->extradata = av_malloc(pkt_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        avctx->extradata_size = pkt_len + AV_INPUT_BUFFER_PADDING_SIZE;
        memcpy(avctx->extradata, pkt_pos, pkt_len);
        memset(avctx->extradata + pkt_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        mpp_packet_deinit(&mpp_pkt);
    }

    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME)
        return 0;

    if (avctx->hw_frames_ctx || avctx->hw_device_ctx) {
        AVBufferRef *device_ref = avctx->hw_device_ctx;
        AVHWDeviceContext *device_ctx = NULL;
        AVHWFramesContext *hwfc = NULL;

        if (avctx->hw_frames_ctx) {
            hwfc = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
            device_ref = hwfc->device_ref;
        }
        device_ctx = (AVHWDeviceContext *)device_ref->data;

        if (device_ctx && device_ctx->type == AV_HWDEVICE_TYPE_RKMPP) {
            r->hwdevice = av_buffer_ref(device_ref);
            if (r->hwdevice)
                av_log(avctx, AV_LOG_VERBOSE, "Picked up an existing RKMPP hardware device\n");
        }
    }
    if (!r->hwdevice) {
        if ((ret = av_hwdevice_ctx_create(&r->hwdevice,
                                          AV_HWDEVICE_TYPE_RKMPP,
                                          NULL, NULL, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to create a RKMPP hardware device: %d\n", ret);
            goto fail;
        }
        av_log(avctx, AV_LOG_VERBOSE, "Created a RKMPP hardware device\n");
    }

    ret = init_hwframes_ctx(avctx);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    if (mpp_pkt)
        mpp_packet_deinit(&mpp_pkt);

    return ret;
}

#if CONFIG_H264_RKMPP_ENCODER
DEFINE_RKMPP_ENCODER(h264, H264, h26x)
#endif
#if CONFIG_HEVC_RKMPP_ENCODER
DEFINE_RKMPP_ENCODER(hevc, HEVC, h26x)
#endif
#if CONFIG_MJPEG_RKMPP_ENCODER
DEFINE_RKMPP_ENCODER(mjpeg, MJPEG, mjpeg)
#endif
