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
    case AV_PIX_FMT_NV12:      return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_YUYV422:   return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_UYVY422:   return MPP_FMT_YUV422_UYVY;
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

    mpp_enc_cfg_set_s32(cfg, "prep:colorspace", avctx->colorspace);
    mpp_enc_cfg_set_s32(cfg, "prep:colorprim", avctx->color_primaries);
    mpp_enc_cfg_set_s32(cfg, "prep:colortrc", avctx->color_trc);

    mpp_enc_cfg_set_s32(cfg, "prep:colorrange", avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_enc_cfg_set_s32(cfg, "prep:colorrange", AVCOL_RANGE_JPEG);
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

    RK_U32 rc_mode, fps_num, fps_den;
    MppEncHeaderMode header_mode;
    MppEncSeiMode sei_mode;
    int max_bps, min_bps;
    int qp_init, qp_max, qp_min, qp_max_i, qp_min_i;
    int ret;

    mpp_enc_cfg_set_s32(cfg, "prep:width", avctx->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", avctx->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", FFALIGN(avctx->width, 64));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", FFALIGN(avctx->height, 64));
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:mirroring", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:rotation", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:flip", 0);

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
        else if (avctx->rc_max_rate > 0)
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
        max_bps = (avctx->rc_max_rate > 0 && avctx->rc_max_rate >= avctx->bit_rate)
                  ? avctx->rc_max_rate : (avctx->bit_rate * 17 / 16);
        min_bps = (avctx->rc_min_rate > 0 && avctx->rc_min_rate <= avctx->bit_rate)
                  ? avctx->rc_min_rate : (avctx->bit_rate * 1 / 16);
        break;
    case MPP_ENC_RC_MODE_CBR:
    default:
        /* CBR mode has narrow bound */
        max_bps = avctx->bit_rate * 17 / 16;
        min_bps = avctx->bit_rate * 15 / 16;
        break;
    }
    if (rc_mode == MPP_ENC_RC_MODE_CBR ||
        rc_mode == MPP_ENC_RC_MODE_VBR ||
        rc_mode == MPP_ENC_RC_MODE_AVBR) {
        mpp_enc_cfg_set_u32(cfg, "rc:bps_target", avctx->bit_rate);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", max_bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", min_bps);
        av_log(avctx, AV_LOG_VERBOSE, "Bitrate Target/Min/Max is set to %ld/%d/%d\n",
               avctx->bit_rate, min_bps, max_bps);
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
                qp_max = qp_min = qp_max_i = qp_min_i = qp_init;
                mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 0);
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
                                (r->dct8x8 && avctx->profile == FF_PROFILE_H264_HIGH));

            switch (avctx->profile) {
            case FF_PROFILE_H264_BASELINE:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to BASELINE\n"); break;
            case FF_PROFILE_H264_MAIN:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to MAIN\n"); break;
            case FF_PROFILE_H264_HIGH:
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
                ? FF_PROFILE_HEVC_REXT : FF_PROFILE_HEVC_MAIN;
            avctx->level = r->level;
            mpp_enc_cfg_set_s32(cfg, "h265:profile", avctx->profile);
            mpp_enc_cfg_set_s32(cfg, "h265:level", avctx->level);
            if (avctx->level >= 120) {
                mpp_enc_cfg_set_s32(cfg, "h265:tier", r->tier);
                av_log(avctx, AV_LOG_VERBOSE, "Tier is set to %d\n", r->tier);
            }

            switch (avctx->profile) {
            case FF_PROFILE_HEVC_MAIN:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to MAIN\n"); break;
            case FF_PROFILE_HEVC_REXT:
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
        sei_mode = MPP_ENC_SEI_MODE_DISABLE;
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
        drm_frame = av_frame_alloc();
        if (!drm_frame) {
            goto exit;
        }
        if ((ret = av_hwframe_get_buffer(r->hwframe, drm_frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot allocate an internal frame: %d\n", ret);
            goto exit;
        }
        if ((ret = av_hwframe_transfer_data(drm_frame, frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_hwframe_transfer_data failed: %d\n", ret);
            goto exit;
        }
        if ((ret = av_frame_copy_props(drm_frame, frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
            goto exit;
        }
        mpp_enc_frame->frame = drm_frame;
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
    int ret, key_frame = 0;

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

    if ((ret = mpp_meta_get_frame(mpp_meta, KEY_INPUT_FRAME, &mpp_frame)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get key input frame from packet meta: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto exit;
    }

    mpp_buf = mpp_frame_get_buffer(mpp_frame);
    if (!mpp_buf)
        return AVERROR(ENOMEM);

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

static int rkmpp_encode_close(AVCodecContext *avctx)
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

static int rkmpp_encode_init(AVCodecContext *avctx)
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

    if (avctx->codec_id == AV_CODEC_ID_H264 ||
        avctx->codec_id == AV_CODEC_ID_HEVC) {
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

    rkmpp_encode_close(avctx);
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
