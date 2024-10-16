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

#include "config.h"
#include "config_components.h"

#include "rkmppdec.h"

#include <fcntl.h>
#include <unistd.h>
#if CONFIG_RKRGA
#include <rga/im2d.h>
#endif

static MppCodingType rkmpp_get_coding_type(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H263:          return MPP_VIDEO_CodingH263;
    case AV_CODEC_ID_H264:          return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:          return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_AV1:           return MPP_VIDEO_CodingAV1;
    case AV_CODEC_ID_VP8:           return MPP_VIDEO_CodingVP8;
    case AV_CODEC_ID_VP9:           return MPP_VIDEO_CodingVP9;
    case AV_CODEC_ID_MPEG1VIDEO:    /* fallthrough */
    case AV_CODEC_ID_MPEG2VIDEO:    return MPP_VIDEO_CodingMPEG2;
    case AV_CODEC_ID_MPEG4:         return MPP_VIDEO_CodingMPEG4;
    default:                        return MPP_VIDEO_CodingUnused;
    }
}

static uint32_t rkmpp_get_drm_format(MppFrameFormat mpp_fmt)
{
    switch (mpp_fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:          return DRM_FORMAT_NV12;
    case MPP_FMT_YUV420SP_10BIT:    return DRM_FORMAT_NV15;
    case MPP_FMT_YUV422SP:          return DRM_FORMAT_NV16;
    case MPP_FMT_YUV422SP_10BIT:    return DRM_FORMAT_NV20;
    case MPP_FMT_YUV444SP:          return DRM_FORMAT_NV24;
    default:                        return DRM_FORMAT_INVALID;
    }
}

static uint32_t rkmpp_get_drm_afbc_format(MppFrameFormat mpp_fmt)
{
    switch (mpp_fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:          return DRM_FORMAT_YUV420_8BIT;
    case MPP_FMT_YUV420SP_10BIT:    return DRM_FORMAT_YUV420_10BIT;
    case MPP_FMT_YUV422SP:          return DRM_FORMAT_YUYV;
    case MPP_FMT_YUV422SP_10BIT:    return DRM_FORMAT_Y210;
    case MPP_FMT_YUV444SP:          return DRM_FORMAT_VUY888;
    default:                        return DRM_FORMAT_INVALID;
    }
}

static uint32_t rkmpp_get_av_format(MppFrameFormat mpp_fmt)
{
    switch (mpp_fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:          return AV_PIX_FMT_NV12;
    case MPP_FMT_YUV420SP_10BIT:    return AV_PIX_FMT_NV15;
    case MPP_FMT_YUV422SP:          return AV_PIX_FMT_NV16;
    case MPP_FMT_YUV422SP_10BIT:    return AV_PIX_FMT_NV20;
    case MPP_FMT_YUV444SP:          return AV_PIX_FMT_NV24;
    default:                        return AV_PIX_FMT_NONE;
    }
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

static void read_soc_name(AVCodecContext *avctx, char *name, int size)
{
    const char *dt_path = "/proc/device-tree/compatible";
    int fd = open(dt_path, O_RDONLY);

    if (fd < 0) {
        av_log(avctx, AV_LOG_VERBOSE, "Unable to open '%s' for reading SoC name\n", dt_path);
    } else {
        ssize_t soc_name_len = 0;

        snprintf(name, size - 1, "unknown");
        soc_name_len = read(fd, name, size - 1);
        if (soc_name_len > 0) {
            name[soc_name_len] = '\0';
            /* replacing the termination character to space */
            for (char *ptr = name;; ptr = name) {
                ptr += av_strnlen(name, size);
                if (ptr >= name + soc_name_len - 1)
                    break;
                *ptr = ' ';
            }

            av_log(avctx, AV_LOG_VERBOSE, "Found SoC name from device-tree: '%s'\n", name);
        }

        close(fd);
    }
}

static av_cold int rkmpp_decode_close(AVCodecContext *avctx)
{
    RKMPPDecContext *r = avctx->priv_data;

    r->eof = 0;
    r->draining = 0;
    r->info_change = 0;
    r->errinfo_cnt = 0;
    r->got_frame = 0;
    r->use_rfbc = 0;

    if (r->mapi) {
        r->mapi->reset(r->mctx);
        mpp_destroy(r->mctx);
        r->mctx = NULL;
    }
    if (r->buf_group &&
        r->buf_mode == RKMPP_DEC_PURE_EXTERNAL) {
        mpp_buffer_group_put(r->buf_group);
        r->buf_group = NULL;
    }

    if (r->hwframe)
        av_buffer_unref(&r->hwframe);
    if (r->hwdevice)
        av_buffer_unref(&r->hwdevice);

    return 0;
}

static av_cold int rkmpp_decode_init(AVCodecContext *avctx)
{
    RKMPPDecContext *r = avctx->priv_data;
    MppCodingType coding_type = MPP_VIDEO_CodingUnused;
    const char *opts_env = NULL;
    char soc_name[MAX_SOC_NAME_LENGTH];
    int ret, is_fmt_supported = 0;
    enum AVPixelFormat pix_fmts[3] = { AV_PIX_FMT_DRM_PRIME,
                                       AV_PIX_FMT_NV12,
                                       AV_PIX_FMT_NONE };

    opts_env = getenv("FFMPEG_RKMPP_DEC_OPT");
    if (opts_env && av_set_options_string(r, opts_env, "=", " ") <= 0)
        av_log(avctx, AV_LOG_WARNING, "Unable to set decoder options from env\n");

    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        pix_fmts[1] = AV_PIX_FMT_NV12;
        is_fmt_supported = 1;
        break;
    case AV_PIX_FMT_YUV420P10:
        pix_fmts[1] = AV_PIX_FMT_NV15;
        is_fmt_supported =
            avctx->codec_id == AV_CODEC_ID_H264 ||
            avctx->codec_id == AV_CODEC_ID_HEVC ||
            avctx->codec_id == AV_CODEC_ID_VP9 ||
            avctx->codec_id == AV_CODEC_ID_AV1;
        break;
    case AV_PIX_FMT_YUV422P:
        pix_fmts[1] = AV_PIX_FMT_NV16;
        is_fmt_supported =
            avctx->codec_id == AV_CODEC_ID_H264;
        break;
    case AV_PIX_FMT_YUV422P10:
        pix_fmts[1] = AV_PIX_FMT_NV20;
        is_fmt_supported =
            avctx->codec_id == AV_CODEC_ID_H264;
        break;
    case AV_PIX_FMT_YUV444P:
        pix_fmts[1] = AV_PIX_FMT_NV24;
        is_fmt_supported =
            avctx->codec_id == AV_CODEC_ID_HEVC;
        break;
    case AV_PIX_FMT_NONE: /* fallback to drm_prime */
        is_fmt_supported = 1;
        avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
        break;
    default:
        is_fmt_supported = 0;
        break;
    }

    if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
        if (!is_fmt_supported) {
            av_log(avctx, AV_LOG_ERROR, "MPP doesn't support codec '%s' with pix_fmt '%s'\n",
                   avcodec_get_name(avctx->codec_id), av_get_pix_fmt_name(avctx->pix_fmt));
            return AVERROR(ENOSYS);
        }

        if ((ret = ff_get_format(avctx, pix_fmts)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_get_format failed: %d\n", ret);
            return ret;
        }
        avctx->pix_fmt = ret;
    }

    if ((coding_type = rkmpp_get_coding_type(avctx)) == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec id: %d\n", avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    if ((ret = mpp_check_support_format(MPP_CTX_DEC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "MPP doesn't support codec '%s' (%d)\n",
               avcodec_get_name(avctx->codec_id), avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    if ((ret = mpp_create(&r->mctx, &r->mapi)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = mpp_init(r->mctx, MPP_CTX_DEC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (avctx->skip_frame == AVDISCARD_NONKEY)
        r->deint = 0;

    if ((ret = r->mapi->control(r->mctx, MPP_DEC_SET_ENABLE_DEINTERLACE, &r->deint)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set enable deinterlace: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME)
        r->afbc = 0;

    if (r->afbc > RKMPP_DEC_AFBC_OFF) {
        read_soc_name(avctx, soc_name, sizeof(soc_name));
        r->use_rfbc = !!strstr(soc_name, "rk3576");
    }

    if (r->afbc == RKMPP_DEC_AFBC_ON_RGA) {
#if CONFIG_RKRGA
        const char *rga_ver = querystring(RGA_VERSION);
        int has_rga2p = !!strstr(rga_ver, "RGA_2_PRO");
        int has_rga3  = !!strstr(rga_ver, "RGA_3");
        int is_rga2p_compat = avctx->width >= 2 &&
                              avctx->width <= 8192 &&
                              avctx->height >= 2 &&
                              avctx->height <= 8192;
        int is_rga3_compat  = avctx->width >= 68 &&
                              avctx->width <= 8176 &&
                              avctx->height >= 2 &&
                              avctx->height <= 8176;

        r->use_rfbc = r->use_rfbc || has_rga2p;
        if (!((has_rga2p && is_rga2p_compat) || (has_rga3 && is_rga3_compat))) {
#endif
            av_log(avctx, AV_LOG_VERBOSE, "AFBC is requested without capable RGA, ignoring\n");
            r->afbc = RKMPP_DEC_AFBC_OFF;
#if CONFIG_RKRGA
        }
#endif
    }

    if (r->afbc) {
        MppFrameFormat afbc_fmt = MPP_FRAME_FBC_AFBC_V2;

        if (avctx->codec_id == AV_CODEC_ID_H264 ||
            avctx->codec_id == AV_CODEC_ID_HEVC ||
            avctx->codec_id == AV_CODEC_ID_VP9 ||
            avctx->codec_id == AV_CODEC_ID_AV1) {
            if ((ret = r->mapi->control(r->mctx, MPP_DEC_SET_OUTPUT_FORMAT, &afbc_fmt)) != MPP_OK) {
                av_log(avctx, AV_LOG_ERROR, "Failed to set AFBC mode: %d\n", ret);
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
        } else {
            av_log(avctx, AV_LOG_VERBOSE, "AFBC is not supported in codec '%s', ignoring\n",
                   avcodec_get_name(avctx->codec_id));
            r->afbc = 0;
        }
    }

    if (avctx->hw_device_ctx) {
        r->hwdevice = av_buffer_ref(avctx->hw_device_ctx);
        if (!r->hwdevice) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_log(avctx, AV_LOG_VERBOSE, "Picked up an existing RKMPP hardware device\n");
    } else {
        if ((ret = av_hwdevice_ctx_create(&r->hwdevice, AV_HWDEVICE_TYPE_RKMPP, NULL, NULL, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to create a RKMPP hardware device: %d\n", ret);
            goto fail;
        }
        av_log(avctx, AV_LOG_VERBOSE, "Created a RKMPP hardware device\n");
    }

    return 0;

fail:
    rkmpp_decode_close(avctx);
    return ret;
}

static int rkmpp_set_buffer_group(AVCodecContext *avctx,
                                  enum AVPixelFormat pix_fmt,
                                  int width, int height)
{
    RKMPPDecContext *r = avctx->priv_data;
    AVHWFramesContext *hwfc = NULL;
    AVRKMPPFramesContext *rkmpp_fc = NULL;
    int i, ret, decoder_pool_size;

    if (!r->hwdevice)
        return AVERROR(ENOMEM);

    av_buffer_unref(&r->hwframe);

    r->hwframe = av_hwframe_ctx_alloc(r->hwdevice);
    if (!r->hwframe)
        return AVERROR(ENOMEM);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
        decoder_pool_size = 20;
        break;
    default:
        decoder_pool_size = 10;
        break;
    }

    hwfc = (AVHWFramesContext *)r->hwframe->data;
    hwfc->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc->sw_format = pix_fmt;
    hwfc->width     = FFALIGN(width,  16);
    hwfc->height    = FFALIGN(height, 16);

    rkmpp_fc = hwfc->hwctx;
    rkmpp_fc->flags |= MPP_BUFFER_FLAGS_CACHABLE;

    if (r->buf_mode == RKMPP_DEC_HALF_INTERNAL) {
        if ((ret = av_hwframe_ctx_init(r->hwframe)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to init RKMPP frame pool\n");
            goto fail;
        }

        r->buf_group = rkmpp_fc->buf_group;
        goto attach;
    } else if (r->buf_mode != RKMPP_DEC_PURE_EXTERNAL) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    hwfc->initial_pool_size = decoder_pool_size + 10;
    if (avctx->extra_hw_frames > 0)
        hwfc->initial_pool_size += avctx->extra_hw_frames;

    if ((ret = av_hwframe_ctx_init(r->hwframe)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init RKMPP frame pool\n");
        goto fail;
    }

    if (r->buf_group) {
        if ((ret = mpp_buffer_group_clear(r->buf_group)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to clear external buffer group: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        if ((ret = mpp_buffer_group_get_external(&r->buf_group, MPP_BUFFER_TYPE_DRM)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get external buffer group: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    for (i = 0; i < hwfc->initial_pool_size; i++) {
        AVRKMPPFramesContext *rkmpp_fc = hwfc->hwctx;
        MppBufferInfo buf_info = {
            .index = i,
            .type  = MPP_BUFFER_TYPE_DRM,
            .ptr   = mpp_buffer_get_ptr(rkmpp_fc->frames[i].buffers[0]),
            .fd    = rkmpp_fc->frames[i].drm_desc.objects[0].fd,
            .size  = rkmpp_fc->frames[i].drm_desc.objects[0].size,
        };

        if ((ret = mpp_buffer_commit(r->buf_group, &buf_info)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to commit external buffer group: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

attach:
    if ((ret = r->mapi->control(r->mctx, MPP_DEC_SET_EXT_BUF_GROUP, r->buf_group)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to attach external buffer group: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (r->buf_mode == RKMPP_DEC_HALF_INTERNAL) {
        int group_limit = decoder_pool_size + ((width * height > (3840 * 2160 * 3)) ? 2 : 10);
        if (avctx->extra_hw_frames > 0)
            group_limit += avctx->extra_hw_frames;
        if ((ret = mpp_buffer_group_limit_config(r->buf_group, 0, group_limit)) != MPP_OK)
            av_log(avctx, AV_LOG_WARNING, "Failed to set buffer group limit: %d\n", ret);
    }

    return 0;

fail:
    if (r->buf_group &&
        r->buf_mode == RKMPP_DEC_HALF_INTERNAL) {
        mpp_buffer_group_put(r->buf_group);
        r->buf_group = NULL;
    }
    av_buffer_unref(&r->hwframe);
    return ret;
}

static int rkmpp_export_mastering_display(AVCodecContext *avctx, AVFrame *frame,
                                          MppFrameMasteringDisplayMetadata mpp_mastering)
{
    AVMasteringDisplayMetadata *mastering = NULL;
    AVFrameSideData *sd = NULL;
    int mapping[3] = { 0, 1, 2 };
    int chroma_den = 0;
    int max_luma_den = 0;
    int min_luma_den = 0;
    int i;

    switch (avctx->codec_id) {
        case AV_CODEC_ID_HEVC:
            // HEVC uses a g,b,r ordering, which we convert to a more natural r,g,b
            mapping[0] = 2;
            mapping[1] = 0;
            mapping[2] = 1;
            chroma_den = 50000;
            max_luma_den = 10000;
            min_luma_den = 10000;
            break;
        case AV_CODEC_ID_AV1:
            chroma_den = 1 << 16;
            max_luma_den = 1 << 8;
            min_luma_den = 1 << 14;
            break;
        default:
            return 0;
    }

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd)
        mastering = (AVMasteringDisplayMetadata *)sd->data;
    else
        mastering = av_mastering_display_metadata_create_side_data(frame);
    if (!mastering)
        return AVERROR(ENOMEM);

    for (i = 0; i < 3; i++) {
        const int j = mapping[i];
        mastering->display_primaries[i][0] = av_make_q(mpp_mastering.display_primaries[j][0], chroma_den);
        mastering->display_primaries[i][1] = av_make_q(mpp_mastering.display_primaries[j][1], chroma_den);
    }
    mastering->white_point[0] = av_make_q(mpp_mastering.white_point[0], chroma_den);
    mastering->white_point[1] = av_make_q(mpp_mastering.white_point[1], chroma_den);

    mastering->max_luminance = av_make_q(mpp_mastering.max_luminance, max_luma_den);
    mastering->min_luminance = av_make_q(mpp_mastering.min_luminance, min_luma_den);

    mastering->has_luminance = 1;
    mastering->has_primaries = 1;

    return 0;
}

static int rkmpp_export_content_light(AVFrame *frame,
                                      MppFrameContentLightMetadata mpp_light)
{
    AVContentLightMetadata *light = NULL;

    AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd)
        light = (AVContentLightMetadata *)sd->data;
    else
        light = av_content_light_metadata_create_side_data(frame);
    if (!light)
        return AVERROR(ENOMEM);

    light->MaxCLL  = mpp_light.MaxCLL;
    light->MaxFALL = mpp_light.MaxFALL;

    return 0;
}

static void rkmpp_free_mpp_frame(void *opaque, uint8_t *data)
{
    MppFrame mpp_frame = (MppFrame)opaque;
    mpp_frame_deinit(&mpp_frame);
}

static void rkmpp_free_drm_desc(void *opaque, uint8_t *data)
{
    AVRKMPPDRMFrameDescriptor *drm_desc = (AVRKMPPDRMFrameDescriptor *)opaque;
    av_free(drm_desc);
}

static int frame_create_buf(AVFrame *frame,
                            uint8_t* data, int size,
                            void (*free)(void *opaque, uint8_t *data),
                            void *opaque, int flags)
{
    int i;

    for (i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (!frame->buf[i]) {
            frame->buf[i] = av_buffer_create(data, size, free, opaque, flags);
            return frame->buf[i] ? 0 : AVERROR(ENOMEM);
        }
    }
    return AVERROR(EINVAL);
}

static int rkmpp_export_frame(AVCodecContext *avctx, AVFrame *frame, MppFrame mpp_frame)
{
    RKMPPDecContext *r = avctx->priv_data;
    AVRKMPPDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    const AVPixFmtDescriptor *pix_desc;
    MppBuffer mpp_buf = NULL;
    MppFrameFormat mpp_fmt = MPP_FMT_BUTT;
    int mpp_frame_mode = 0;
    int ret, is_afbc = 0;

    if (!frame || !mpp_frame)
        return AVERROR(ENOMEM);

    mpp_buf = mpp_frame_get_buffer(mpp_frame);
    if (!mpp_buf)
        return AVERROR(EAGAIN);

    desc = av_mallocz(sizeof(*desc));
    if (!desc)
        return AVERROR(ENOMEM);

    desc->drm_desc.nb_objects = 1;
    desc->buffers[0] = mpp_buf;

    desc->drm_desc.objects[0].fd   = mpp_buffer_get_fd(mpp_buf);
    desc->drm_desc.objects[0].size = mpp_buffer_get_size(mpp_buf);

    mpp_fmt = mpp_frame_get_fmt(mpp_frame);
    is_afbc = mpp_fmt & MPP_FRAME_FBC_MASK;

    desc->drm_desc.nb_layers = 1;
    layer = &desc->drm_desc.layers[0];
    layer->planes[0].object_index = 0;

    if (is_afbc) {
        desc->drm_desc.objects[0].format_modifier =
            r->use_rfbc ? DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4)
                        : DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);

        layer->format = rkmpp_get_drm_afbc_format(mpp_fmt);
        layer->nb_planes = 1;
        layer->planes[0].offset = 0;
        layer->planes[0].pitch  = mpp_frame_get_hor_stride(mpp_frame);

        pix_desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
        if ((ret = get_afbc_byte_stride(pix_desc, (int *)&layer->planes[0].pitch, 0)) < 0)
            return ret;

        /* MPP specific AFBC src_y offset, not memory address offset */
        frame->crop_top = r->use_rfbc ? 0 : mpp_frame_get_offset_y(mpp_frame);
    } else {
        layer->format = rkmpp_get_drm_format(mpp_fmt);
        layer->nb_planes = 2;
        layer->planes[0].offset = 0;
        layer->planes[0].pitch  = mpp_frame_get_hor_stride(mpp_frame);

        layer->planes[1].object_index = 0;
        layer->planes[1].offset = layer->planes[0].pitch * mpp_frame_get_ver_stride(mpp_frame);
        layer->planes[1].pitch  = layer->planes[0].pitch;

        if (avctx->sw_pix_fmt == AV_PIX_FMT_NV24)
            layer->planes[1].pitch *= 2;
    }

    if ((ret = frame_create_buf(frame, mpp_frame, mpp_frame_get_buf_size(mpp_frame),
                                rkmpp_free_mpp_frame, mpp_frame, AV_BUFFER_FLAG_READONLY)) < 0)
        return ret;

    if ((ret = frame_create_buf(frame, (uint8_t *)desc, sizeof(*desc),
                                rkmpp_free_drm_desc, desc, AV_BUFFER_FLAG_READONLY)) < 0)
        return ret;

    frame->data[0] = (uint8_t *)desc;

    frame->hw_frames_ctx = av_buffer_ref(r->hwframe);
    if (!frame->hw_frames_ctx)
        return AVERROR(ENOMEM);

    if ((ret = ff_decode_frame_props(avctx, frame)) < 0)
        return ret;

    frame->width  = avctx->width;
    frame->height = avctx->height;
    frame->pts    = MPP_PTS_TO_PTS(mpp_frame_get_pts(mpp_frame), avctx->pkt_timebase);

    mpp_frame_mode = mpp_frame_get_mode(mpp_frame);
    if ((mpp_frame_mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED)
        frame->flags |= AV_FRAME_FLAG_INTERLACED;
    if ((mpp_frame_mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST)
        frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;

    if (avctx->codec_id == AV_CODEC_ID_MPEG1VIDEO ||
        avctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        MppFrameRational sar = mpp_frame_get_sar(mpp_frame);
        frame->sample_aspect_ratio = av_div_q((AVRational) { sar.num, sar.den },
                                              (AVRational) { frame->width, frame->height });
    }

    if (avctx->codec_id == AV_CODEC_ID_HEVC &&
        (frame->color_trc == AVCOL_TRC_SMPTE2084 ||
         frame->color_trc == AVCOL_TRC_ARIB_STD_B67)) {
        ret = rkmpp_export_mastering_display(avctx, frame, mpp_frame_get_mastering_display(mpp_frame));
        if (ret < 0)
            return ret;
        ret = rkmpp_export_content_light(frame, mpp_frame_get_content_light(mpp_frame));
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void rkmpp_export_avctx_color_props(AVCodecContext *avctx, MppFrame mpp_frame)
{
    int val;

    if (!avctx || !mpp_frame)
        return;

    if (avctx->color_primaries == AVCOL_PRI_RESERVED0)
        avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
    if ((val = mpp_frame_get_color_primaries(mpp_frame)) &&
        val != MPP_FRAME_PRI_RESERVED0 &&
        val != MPP_FRAME_PRI_UNSPECIFIED)
        avctx->color_primaries = val;

    if (avctx->color_trc == AVCOL_TRC_RESERVED0)
        avctx->color_trc = AVCOL_TRC_UNSPECIFIED;
    if ((val = mpp_frame_get_color_trc(mpp_frame)) &&
        val != MPP_FRAME_TRC_RESERVED0 &&
        val != MPP_FRAME_TRC_UNSPECIFIED)
        avctx->color_trc = val;

    if (avctx->colorspace == AVCOL_SPC_RESERVED)
        avctx->colorspace = AVCOL_SPC_UNSPECIFIED;
    if ((val = mpp_frame_get_colorspace(mpp_frame)) &&
        val != MPP_FRAME_SPC_RESERVED &&
        val != MPP_FRAME_SPC_UNSPECIFIED)
        avctx->colorspace = val;

    if ((val = mpp_frame_get_color_range(mpp_frame)) > MPP_FRAME_RANGE_UNSPECIFIED)
        avctx->color_range = val;

    if ((val = mpp_frame_get_chroma_location(mpp_frame)) > MPP_CHROMA_LOC_UNSPECIFIED)
        avctx->chroma_sample_location = val;
}

static int rkmpp_get_frame(AVCodecContext *avctx, AVFrame *frame, int timeout)
{
    RKMPPDecContext *r = avctx->priv_data;
    MppFrame mpp_frame = NULL;
    int ret;

    /* should not provide any frame after EOS */
    if (r->eof)
        return AVERROR_EOF;

    if ((ret = r->mapi->control(r->mctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    ret = r->mapi->decode_get_frame(r->mctx, &mpp_frame);
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    if (!mpp_frame) {
        if (timeout != MPP_TIMEOUT_NON_BLOCK)
            av_log(avctx, AV_LOG_DEBUG, "Timeout getting decoded frame\n");
        return AVERROR(EAGAIN);
    }
    if (mpp_frame_get_eos(mpp_frame)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a 'EOS' frame\n");
        /* EOS frame may contain valid data */
        if (!mpp_frame_get_buffer(mpp_frame)) {
            r->eof = 1;
            ret = AVERROR_EOF;
            goto exit;
        }
    }
    if (mpp_frame_get_discard(mpp_frame)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a 'discard' frame\n");
        ret = AVERROR(EAGAIN);
        goto exit;
    }
    if (mpp_frame_get_errinfo(mpp_frame)) {
        av_log(avctx, AV_LOG_DEBUG, "Received a 'errinfo' frame\n");
        ret = (r->errinfo_cnt++ > MAX_ERRINFO_COUNT) ? AVERROR_EXTERNAL : AVERROR(EAGAIN);
        goto exit;
    }

    if (r->info_change = mpp_frame_get_info_change(mpp_frame)) {
        char *opts = NULL;
        int fast_parse = r->fast_parse;
        int mpp_frame_mode = mpp_frame_get_mode(mpp_frame);
        const MppFrameFormat mpp_fmt = mpp_frame_get_fmt(mpp_frame);
        enum AVPixelFormat pix_fmts[3] = { AV_PIX_FMT_DRM_PRIME,
                                           AV_PIX_FMT_NONE,
                                           AV_PIX_FMT_NONE };

        av_log(avctx, AV_LOG_VERBOSE, "Noticed an info change\n");

        if (r->afbc && !(mpp_fmt & MPP_FRAME_FBC_MASK)) {
            av_log(avctx, AV_LOG_VERBOSE, "AFBC is requested but not supported\n");
            r->afbc = 0;
        }

        pix_fmts[1] = rkmpp_get_av_format(mpp_fmt & MPP_FRAME_FMT_MASK);

        if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME)
            avctx->sw_pix_fmt = pix_fmts[1];
        else {
            if ((ret = ff_get_format(avctx, pix_fmts)) < 0)
                goto exit;
            avctx->pix_fmt = ret;
        }

        avctx->width        = mpp_frame_get_width(mpp_frame);
        avctx->height       = mpp_frame_get_height(mpp_frame);
        avctx->coded_width  = FFALIGN(avctx->width,  64);
        avctx->coded_height = FFALIGN(avctx->height, 64);
        rkmpp_export_avctx_color_props(avctx, mpp_frame);

        if (av_opt_serialize(r, 0, 0, &opts, '=', ' ') >= 0)
            av_log(avctx, AV_LOG_VERBOSE, "Decoder options: %s\n", opts);

        av_log(avctx, AV_LOG_VERBOSE, "Configured with size: %dx%d | pix_fmt: %s | sw_pix_fmt: %s\n",
               avctx->width, avctx->height,
               av_get_pix_fmt_name(avctx->pix_fmt),
               av_get_pix_fmt_name(avctx->sw_pix_fmt));

        if ((ret = rkmpp_set_buffer_group(avctx, pix_fmts[1], avctx->width, avctx->height)) < 0)
            goto exit;

        /* Disable fast parsing for the interlaced video */
        if (((mpp_frame_mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_DEINTERLACED ||
             (mpp_frame_mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) == MPP_FRAME_FLAG_TOP_FIRST) && fast_parse) {
            av_log(avctx, AV_LOG_VERBOSE, "Fast parsing is disabled for the interlaced video\n");
            fast_parse = 0;
        }
        if ((ret = r->mapi->control(r->mctx, MPP_DEC_SET_PARSER_FAST_MODE, &fast_parse)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set parser fast mode: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto exit;
        }

        if ((ret = r->mapi->control(r->mctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set info change ready: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto exit;
        }
        goto exit;
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Received a frame\n");
        r->errinfo_cnt = 0;
        r->got_frame = 1;

        switch (avctx->pix_fmt) {
        case AV_PIX_FMT_DRM_PRIME:
            {
                if ((ret = rkmpp_export_frame(avctx, frame, mpp_frame)) < 0)
                    goto exit;
                return 0;
            }
            break;
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV16:
        case AV_PIX_FMT_NV24:
        case AV_PIX_FMT_NV15:
        case AV_PIX_FMT_NV20:
            {
                AVFrame *tmp_frame = av_frame_alloc();
                if (!tmp_frame) {
                    ret = AVERROR(ENOMEM);
                    goto exit;
                }
                if ((ret = rkmpp_export_frame(avctx, tmp_frame, mpp_frame)) < 0)
                    goto exit;

                if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed: %d\n", ret);
                    av_frame_free(&tmp_frame);
                    goto exit;
                }
                if ((ret = av_hwframe_transfer_data(frame, tmp_frame, 0)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "av_hwframe_transfer_data failed: %d\n", ret);
                    av_frame_free(&tmp_frame);
                    goto exit;
                }
                if ((ret = av_frame_copy_props(frame, tmp_frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
                    av_frame_free(&tmp_frame);
                    goto exit;
                }
                av_frame_free(&tmp_frame);
                return 0;
            }
            break;
        default:
            {
                ret = AVERROR_BUG;
                goto exit;
            }
            break;
        }
    }

exit:
    if (mpp_frame)
        mpp_frame_deinit(&mpp_frame);
    return ret;
}

static int rkmpp_send_eos(AVCodecContext *avctx)
{
    RKMPPDecContext *r = avctx->priv_data;
    MppPacket mpp_pkt = NULL;
    int ret;

    if ((ret = mpp_packet_init(&mpp_pkt, NULL, 0)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init 'EOS' packet: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    mpp_packet_set_eos(mpp_pkt);

    do {
        ret = r->mapi->decode_put_packet(r->mctx, mpp_pkt);
    } while (ret != MPP_OK);

    r->draining = 1;

    mpp_packet_deinit(&mpp_pkt);
    return 0;
}

static int rkmpp_send_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    RKMPPDecContext *r = avctx->priv_data;
    MppPacket mpp_pkt = NULL;
    int64_t pts = PTS_TO_MPP_PTS(pkt->pts, avctx->pkt_timebase);
    int ret;

    /* avoid sending new data after EOS */
    if (r->draining)
        return AVERROR(EOF);

    /* do not skip non-key pkt until got any frame */
    if (r->got_frame &&
        avctx->skip_frame == AVDISCARD_NONKEY &&
        !(pkt->flags & AV_PKT_FLAG_KEY)) {
        av_log(avctx, AV_LOG_TRACE, "Skip packet without key flag "
               "at pts %"PRId64"\n", pkt->pts);
        return 0;
    }

    if ((ret = mpp_packet_init(&mpp_pkt, pkt->data, pkt->size)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init packet: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    mpp_packet_set_pts(mpp_pkt, pts);

    if ((ret = r->mapi->decode_put_packet(r->mctx, mpp_pkt)) != MPP_OK) {
        av_log(avctx, AV_LOG_TRACE, "Decoder buffer is full\n");
        mpp_packet_deinit(&mpp_pkt);
        return AVERROR(EAGAIN);
    }
    av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", pkt->size);

    mpp_packet_deinit(&mpp_pkt);
    return 0;
}

static int rkmpp_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecContext *r = avctx->priv_data;
    AVPacket *pkt = &r->last_pkt;
    int ret;

    if (r->info_change && !r->buf_group)
        return AVERROR_EOF;

    /* no more frames after EOS */
    if (r->eof)
        return AVERROR_EOF;

    /* drain remain frames */
    if (r->draining) {
        ret = rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);
        goto exit;
    }

    while (1) {
        if (!pkt->size) {
            ret = ff_decode_get_packet(avctx, pkt);
            if (ret == AVERROR_EOF) {
                av_log(avctx, AV_LOG_DEBUG, "Decoder is at EOF\n");
                /* send EOS and start draining */
                rkmpp_send_eos(avctx);
                ret = rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);
                goto exit;
            } else if (ret == AVERROR(EAGAIN)) {
                /* not blocking so that we can feed data ASAP */
                ret = rkmpp_get_frame(avctx, frame, MPP_TIMEOUT_NON_BLOCK);
                goto exit;
            } else if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Decoder failed to get packet: %d\n", ret);
                goto exit;
            }
        } else {
            /* send pending data to decoder */
            ret = rkmpp_send_packet(avctx, pkt);
            if (ret == AVERROR(EAGAIN)) {
                /* some streams might need more packets to start returning frames */
                ret = rkmpp_get_frame(avctx, frame, 100);
                if (ret != AVERROR(EAGAIN))
                    goto exit;
            } else if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Decoder failed to send packet: %d\n", ret);
                goto exit;
            } else {
                av_packet_unref(pkt);
                pkt->size = 0;
            }
        }
    }

exit:
    if (r->draining &&
        ret == AVERROR(EAGAIN))
        ret = AVERROR_EOF;
    return ret;
}

static void rkmpp_decode_flush(AVCodecContext *avctx)
{
    RKMPPDecContext *r = avctx->priv_data;
    int ret;

    av_log(avctx, AV_LOG_DEBUG, "Decoder flushing\n");

    if ((ret = r->mapi->reset(r->mctx)) == MPP_OK) {
        r->eof = 0;
        r->draining = 0;
        r->info_change = 0;
        r->errinfo_cnt = 0;
        r->got_frame = 0;

        av_packet_unref(&r->last_pkt);
    } else
        av_log(avctx, AV_LOG_ERROR, "Failed to reset MPP context: %d\n", ret);
}

#if CONFIG_H263_RKMPP_DECODER
DEFINE_RKMPP_DECODER(h263, H263, NULL)
#endif
#if CONFIG_H264_RKMPP_DECODER
DEFINE_RKMPP_DECODER(h264, H264, "h264_mp4toannexb,dump_extra")
#endif
#if CONFIG_HEVC_RKMPP_DECODER
DEFINE_RKMPP_DECODER(hevc, HEVC, "hevc_mp4toannexb,dump_extra")
#endif
#if CONFIG_VP8_RKMPP_DECODER
DEFINE_RKMPP_DECODER(vp8, VP8, NULL)
#endif
#if CONFIG_VP9_RKMPP_DECODER
DEFINE_RKMPP_DECODER(vp9, VP9, NULL)
#endif
#if CONFIG_AV1_RKMPP_DECODER
DEFINE_RKMPP_DECODER(av1, AV1, NULL)
#endif
#if CONFIG_MPEG1_RKMPP_DECODER
DEFINE_RKMPP_DECODER(mpeg1, MPEG1VIDEO, NULL)
#endif
#if CONFIG_MPEG2_RKMPP_DECODER
DEFINE_RKMPP_DECODER(mpeg2, MPEG2VIDEO, NULL)
#endif
#if CONFIG_MPEG4_RKMPP_DECODER
DEFINE_RKMPP_DECODER(mpeg4, MPEG4, "dump_extra,mpeg4_unpack_bframes")
#endif
