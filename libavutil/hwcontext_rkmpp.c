/*
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

#include "config.h"

#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* This was introduced in version 4.6. And may not exist all without an
 * optional package. So to prevent a hard dependency on needing the Linux
 * kernel headers to compile, make this optional. */
#if HAVE_LINUX_DMA_BUF_H
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#endif

#include "avassert.h"
#include "hwcontext.h"
#include "hwcontext_rkmpp.h"
#include "hwcontext_internal.h"
#include "imgutils.h"

static const struct {
    enum AVPixelFormat pixfmt;
    uint32_t drm_format;
} supported_formats[] = {
    /* grayscale */
    { AV_PIX_FMT_GRAY8,    DRM_FORMAT_R8        },
    /* planar YUV */
    { AV_PIX_FMT_YUV420P,  DRM_FORMAT_YUV420,   },
    { AV_PIX_FMT_YUV422P,  DRM_FORMAT_YUV422,   },
    { AV_PIX_FMT_YUV444P,  DRM_FORMAT_YUV444,   },
    /* semi-planar YUV */
    { AV_PIX_FMT_NV12,     DRM_FORMAT_NV12,     },
    { AV_PIX_FMT_NV21,     DRM_FORMAT_NV21,     },
    { AV_PIX_FMT_NV16,     DRM_FORMAT_NV16,     },
    { AV_PIX_FMT_NV24,     DRM_FORMAT_NV24,     },
    /* semi-planar YUV 10-bit */
    { AV_PIX_FMT_P010,     DRM_FORMAT_P010,     },
    { AV_PIX_FMT_NV15,     DRM_FORMAT_NV15,     },
    { AV_PIX_FMT_NV20,     DRM_FORMAT_NV20,     },
    /* packed YUV */
    { AV_PIX_FMT_YUYV422,  DRM_FORMAT_YUYV,     },
    { AV_PIX_FMT_YVYU422,  DRM_FORMAT_YVYU,     },
    { AV_PIX_FMT_UYVY422,  DRM_FORMAT_UYVY,     },
    /* packed RGB */
    { AV_PIX_FMT_RGB555LE, DRM_FORMAT_XRGB1555, },
    { AV_PIX_FMT_BGR555LE, DRM_FORMAT_XBGR1555, },
    { AV_PIX_FMT_RGB565LE, DRM_FORMAT_RGB565,   },
    { AV_PIX_FMT_BGR565LE, DRM_FORMAT_BGR565,   },
    { AV_PIX_FMT_RGB24,    DRM_FORMAT_RGB888,   },
    { AV_PIX_FMT_BGR24,    DRM_FORMAT_BGR888,   },
    { AV_PIX_FMT_RGBA,     DRM_FORMAT_ABGR8888, },
    { AV_PIX_FMT_RGB0,     DRM_FORMAT_XBGR8888, },
    { AV_PIX_FMT_BGRA,     DRM_FORMAT_ARGB8888, },
    { AV_PIX_FMT_BGR0,     DRM_FORMAT_XRGB8888, },
    { AV_PIX_FMT_ARGB,     DRM_FORMAT_BGRA8888, },
    { AV_PIX_FMT_0RGB,     DRM_FORMAT_BGRX8888, },
    { AV_PIX_FMT_ABGR,     DRM_FORMAT_RGBA8888, },
    { AV_PIX_FMT_0BGR,     DRM_FORMAT_RGBX8888, },
};

static int rkmpp_device_create(AVHWDeviceContext *hwdev, const char *device,
                               AVDictionary *opts, int flags)
{
    AVRKMPPDeviceContext *hwctx = hwdev->hwctx;
    AVDictionaryEntry *opt_d = NULL;

    hwctx->flags = MPP_BUFFER_FLAGS_DMA32 | MPP_BUFFER_FLAGS_CACHABLE;

    opt_d = av_dict_get(opts, "dma32", NULL, 0);
    if (opt_d && !strtol(opt_d->value, NULL, 10))
        hwctx->flags &= ~MPP_BUFFER_FLAGS_DMA32;

    opt_d = av_dict_get(opts, "cacheable", NULL, 0);
    if (opt_d && !strtol(opt_d->value, NULL, 10))
        hwctx->flags &= ~MPP_BUFFER_FLAGS_CACHABLE;

    return 0;
}

static int rkmpp_frames_get_constraints(AVHWDeviceContext *hwdev,
                                        const void *hwconfig,
                                        AVHWFramesConstraints *constraints)
{
    int i;

    constraints->min_width  = 16;
    constraints->min_height = 16;

    constraints->valid_hw_formats =
        av_malloc_array(2, sizeof(enum AVPixelFormat));
    if (!constraints->valid_hw_formats)
        return AVERROR(ENOMEM);
    constraints->valid_hw_formats[0] = AV_PIX_FMT_DRM_PRIME;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    constraints->valid_sw_formats =
        av_malloc_array(FF_ARRAY_ELEMS(supported_formats) + 1,
                        sizeof(enum AVPixelFormat));
    if (!constraints->valid_sw_formats)
        return AVERROR(ENOMEM);
    for(i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        constraints->valid_sw_formats[i] = supported_formats[i].pixfmt;
    constraints->valid_sw_formats[i] = AV_PIX_FMT_NONE;

    return 0;
}

static void rkmpp_free_drm_frame_descriptor(AVRKMPPDeviceContext *hwctx,
                                            AVDRMFrameDescriptor *desc)
{
    int i, ret;

    if (!desc)
        return;

    for (i = 0; i < desc->nb_objects; i++) {
        AVDRMObjectDescriptor *object = &desc->objects[i];
        MppBuffer mpp_buf = (MppBuffer)object->opaque;

        if (mpp_buf) {
            ret = mpp_buffer_put(mpp_buf);
            if (ret != MPP_OK)
                av_log(NULL, AV_LOG_WARNING,
                       "Failed to put MPP buffer: %d\n", ret);
        }
    }

    memset(desc, 0, sizeof(*desc));
    av_free(desc);
}

static void rkmpp_buffer_free(void *opaque, uint8_t *data)
{
    AVHWFramesContext *hwfc = opaque;
    AVRKMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;

    rkmpp_free_drm_frame_descriptor(hwctx, desc);
}

static int rkmpp_get_aligned_linesize(enum AVPixelFormat pix_fmt, int width, int plane)
{
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(pix_fmt);
    const int is_rgb = pixdesc->flags & AV_PIX_FMT_FLAG_RGB;
    const int is_planar = pixdesc->flags & AV_PIX_FMT_FLAG_PLANAR;
    const int is_packed_fmt = is_rgb || (!is_rgb && !is_planar);
    int linesize;

    if (pix_fmt == AV_PIX_FMT_NV15 ||
        pix_fmt == AV_PIX_FMT_NV20) {
        const int log2_chroma_w = plane == 1 ? 1 : 0;
        const int width_align_256_odds = FFALIGN(width << log2_chroma_w, 256) | 256;
        return FFALIGN(width_align_256_odds * 10 / 8, 64);
    }

    linesize = av_image_get_linesize(pix_fmt, width, plane);

    if (is_packed_fmt) {
        const int pixel_width = av_get_padded_bits_per_pixel(pixdesc) / 8;
        linesize = FFALIGN(linesize / pixel_width, 8) * pixel_width;
    } else
        linesize = FFALIGN(linesize, 64);

    return linesize;
}

static AVBufferRef *rkmpp_drm_pool_alloc(void *opaque, size_t size)
{
    int ret;
    AVHWFramesContext *hwfc = opaque;
    AVRKMPPFramesContext *avfc = hwfc->hwctx;
    AVRKMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    AVDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor *layer;
    AVBufferRef *ref;

    int i;
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(hwfc->sw_format);
    const int bits_pp = av_get_padded_bits_per_pixel(pixdesc);
    const int aligned_w = FFALIGN(hwfc->width * 6 / 5,  64);
    const int aligned_h = FFALIGN(hwfc->height * 6 / 5, 64);

    MppBuffer mpp_buf = NULL;
    size_t mpp_buf_size = aligned_w * aligned_h * bits_pp / 8;

    if (hwfc->initial_pool_size > 0 &&
        avfc->nb_frames >= hwfc->initial_pool_size)
        return NULL;

    desc = av_mallocz(sizeof(*desc));
    if (!desc)
        return NULL;

    desc->nb_objects = 1;
    desc->nb_layers  = 1;

    ret = mpp_buffer_get(avfc->buf_group, &mpp_buf, mpp_buf_size);
    if (ret != MPP_OK || !mpp_buf) {
        av_log(hwctx, AV_LOG_ERROR, "Failed to get MPP buffer: %d\n", ret);
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    desc->objects[0].opaque = mpp_buf;

    desc->objects[0].fd   = mpp_buffer_get_fd(mpp_buf);
    desc->objects[0].ptr  = mpp_buffer_get_ptr(mpp_buf);
    desc->objects[0].size = mpp_buffer_get_size(mpp_buf);

    layer = &desc->layers[0];
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        if (supported_formats[i].pixfmt == hwfc->sw_format) {
            layer->format = supported_formats[i].drm_format;
            break;
        }
    }
    layer->nb_planes = av_pix_fmt_count_planes(hwfc->sw_format);
    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch =
        rkmpp_get_aligned_linesize(hwfc->sw_format, hwfc->width, 0);

    for (i = 1; i < layer->nb_planes; i++) {
        layer->planes[i].object_index = 0;
        layer->planes[i].offset =
            layer->planes[i-1].offset +
            layer->planes[i-1].pitch * (hwfc->height >> (i > 1 ? pixdesc->log2_chroma_h : 0));
        layer->planes[i].pitch =
            rkmpp_get_aligned_linesize(hwfc->sw_format, hwfc->width, i);
    }

    ref = av_buffer_create((uint8_t*)desc, sizeof(*desc), rkmpp_buffer_free,
                           opaque, 0);
    if (!ref) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to create RKMPP buffer.\n");
        goto fail;
    }

    if (hwfc->initial_pool_size > 0) {
        av_assert0(avfc->nb_frames < hwfc->initial_pool_size);
        memcpy(&avfc->frames[avfc->nb_frames], desc, sizeof(*desc));
        ++avfc->nb_frames;
    }

    return ref;

fail:
    rkmpp_free_drm_frame_descriptor(hwctx, desc);
    return NULL;
}

static int rkmpp_frames_init(AVHWFramesContext *hwfc)
{
    AVRKMPPFramesContext *avfc = hwfc->hwctx;
    AVRKMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    int i, ret;

    if (hwfc->pool)
        return 0;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i].pixfmt == hwfc->sw_format)
            break;
    if (i >= FF_ARRAY_ELEMS(supported_formats)) {
        av_log(hwfc, AV_LOG_ERROR, "Unsupported format: %s.\n",
               av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(EINVAL);
    }

    avfc->nb_frames = 0;
    avfc->frames    = NULL;
    if (hwfc->initial_pool_size > 0) {
        avfc->frames = av_malloc(hwfc->initial_pool_size *
                                 sizeof(*avfc->frames));
        if (!avfc->frames)
            return AVERROR(ENOMEM);
    }

    ret = mpp_buffer_group_get_internal(&avfc->buf_group, MPP_BUFFER_TYPE_DRM | hwctx->flags);
    if (ret != MPP_OK) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to get MPP internal buffer group: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    hwfc->internal->pool_internal =
        av_buffer_pool_init2(sizeof(AVDRMFrameDescriptor), hwfc,
                             rkmpp_drm_pool_alloc, NULL);
    if (!hwfc->internal->pool_internal) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to create RKMPP buffer pool.\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}

static void rkmpp_frames_uninit(AVHWFramesContext *hwfc)
{
    AVRKMPPFramesContext *avfc = hwfc->hwctx;

    av_freep(&avfc->frames);

    if (avfc->buf_group) {
        mpp_buffer_group_put(avfc->buf_group);
        avfc->buf_group = NULL;
    }
}

static int rkmpp_get_buffer(AVHWFramesContext *hwfc, AVFrame *frame)
{
    frame->buf[0] = av_buffer_pool_get(hwfc->pool);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->data[0] = (uint8_t*)frame->buf[0]->data;

    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width  = hwfc->width;
    frame->height = hwfc->height;

    return 0;
}

typedef struct RKMPPDRMMapping {
    // Address and length of each mmap()ed region.
    int nb_regions;
    int sync_flags;
    int object[AV_DRM_MAX_PLANES];
    void *address[AV_DRM_MAX_PLANES];
    size_t length[AV_DRM_MAX_PLANES];
    int unmap[AV_DRM_MAX_PLANES];
} RKMPPDRMMapping;

static void rkmpp_unmap_frame(AVHWFramesContext *hwfc,
                              HWMapDescriptor *hwmap)
{
    AVRKMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    RKMPPDRMMapping *map = hwmap->priv;

    for (int i = 0; i < map->nb_regions; i++) {
#if HAVE_LINUX_DMA_BUF_H
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_END | map->sync_flags };
        if (hwctx->flags & MPP_BUFFER_FLAGS_CACHABLE)
            ioctl(map->object[i], DMA_BUF_IOCTL_SYNC, &sync);
#endif
        if (map->address[i] && map->unmap[i])
            munmap(map->address[i], map->length[i]);
    }

    av_free(map);
}

static int rkmpp_map_frame(AVHWFramesContext *hwfc,
                           AVFrame *dst, const AVFrame *src, int flags)
{
    AVRKMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)src->data[0];
#if HAVE_LINUX_DMA_BUF_H
    struct dma_buf_sync sync_start = { 0 };
#endif
    RKMPPDRMMapping *map;
    int err, i, p, plane;
    int mmap_prot;
    void *addr;

    map = av_mallocz(sizeof(*map));
    if (!map)
        return AVERROR(ENOMEM);

    mmap_prot = 0;
    if (flags & AV_HWFRAME_MAP_READ)
        mmap_prot |= PROT_READ;
    if (flags & AV_HWFRAME_MAP_WRITE)
        mmap_prot |= PROT_WRITE;

#if HAVE_LINUX_DMA_BUF_H
    if (flags & AV_HWFRAME_MAP_READ)
        map->sync_flags |= DMA_BUF_SYNC_READ;
    if (flags & AV_HWFRAME_MAP_WRITE)
        map->sync_flags |= DMA_BUF_SYNC_WRITE;
    sync_start.flags = DMA_BUF_SYNC_START | map->sync_flags;
#endif

    if (desc->objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR) {
        av_log(hwfc, AV_LOG_ERROR, "Transfer non-linear DRM_PRIME frame is not supported!\n");
        return AVERROR(ENOSYS);
    }

    av_assert0(desc->nb_objects <= AV_DRM_MAX_PLANES);
    for (i = 0; i < desc->nb_objects; i++) {
        if (desc->objects[i].ptr) {
            addr = desc->objects[i].ptr;
            map->unmap[i] = 0;
        } else {
            addr = mmap(NULL, desc->objects[i].size, mmap_prot, MAP_SHARED,
                        desc->objects[i].fd, 0);
            if (addr == MAP_FAILED) {
                err = AVERROR(errno);
                av_log(hwfc, AV_LOG_ERROR, "Failed to map RKMPP object %d to "
                       "memory: %d.\n", desc->objects[i].fd, errno);
                goto fail;
            }
            map->unmap[i] = 1;
        }

        map->address[i] = addr;
        map->length[i]  = desc->objects[i].size;
        map->object[i] = desc->objects[i].fd;

#if HAVE_LINUX_DMA_BUF_H
        /* We're not checking for errors here because the kernel may not
         * support the ioctl, in which case its okay to carry on */
        if (hwctx->flags & MPP_BUFFER_FLAGS_CACHABLE)
            ioctl(desc->objects[i].fd, DMA_BUF_IOCTL_SYNC, &sync_start);
#endif
    }
    map->nb_regions = i;

    plane = 0;
    for (i = 0; i < desc->nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &desc->layers[i];
        for (p = 0; p < layer->nb_planes; p++) {
            dst->data[plane] =
                (uint8_t*)map->address[layer->planes[p].object_index] +
                                       layer->planes[p].offset;
            dst->linesize[plane] =     layer->planes[p].pitch;
            ++plane;
        }
    }
    av_assert0(plane <= AV_DRM_MAX_PLANES);

    dst->width  = src->width;
    dst->height = src->height;

    err = ff_hwframe_map_create(src->hw_frames_ctx, dst, src,
                                &rkmpp_unmap_frame, map);
    if (err < 0)
        goto fail;

    return 0;

fail:
    for (i = 0; i < desc->nb_objects; i++) {
        if (map->address[i] && map->unmap[i])
            munmap(map->address[i], map->length[i]);
    }
    av_free(map);
    return err;
}

static int rkmpp_transfer_get_formats(AVHWFramesContext *ctx,
                                      enum AVHWFrameTransferDirection dir,
                                      enum AVPixelFormat **formats)
{
    enum AVPixelFormat *pix_fmts;

    pix_fmts = av_malloc_array(2, sizeof(*pix_fmts));
    if (!pix_fmts)
        return AVERROR(ENOMEM);

    pix_fmts[0] = ctx->sw_format;
    pix_fmts[1] = AV_PIX_FMT_NONE;

    *formats = pix_fmts;
    return 0;
}

static int rkmpp_transfer_data_from(AVHWFramesContext *hwfc,
                                    AVFrame *dst, const AVFrame *src)
{
    AVFrame *map;
    int err;

    if (dst->width > hwfc->width || dst->height > hwfc->height)
        return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map)
        return AVERROR(ENOMEM);
    map->format = dst->format;

    err = rkmpp_map_frame(hwfc, map, src, AV_HWFRAME_MAP_READ);
    if (err)
        goto fail;

    map->width  = dst->width;
    map->height = dst->height;

    err = av_frame_copy(dst, map);
    if (err)
        goto fail;

    err = 0;
fail:
    av_frame_free(&map);
    return err;
}

static int rkmpp_transfer_data_to(AVHWFramesContext *hwfc,
                                  AVFrame *dst, const AVFrame *src)
{
    AVFrame *map;
    int err;

    if (src->width > hwfc->width || src->height > hwfc->height)
        return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map)
        return AVERROR(ENOMEM);
    map->format = src->format;

    err = rkmpp_map_frame(hwfc, map, dst, AV_HWFRAME_MAP_WRITE |
                                          AV_HWFRAME_MAP_OVERWRITE);
    if (err)
        goto fail;

    map->width  = src->width;
    map->height = src->height;

    err = av_frame_copy(map, src);
    if (err)
        goto fail;

    err = 0;
fail:
    av_frame_free(&map);
    return err;
}

static int rkmpp_map_from(AVHWFramesContext *hwfc, AVFrame *dst,
                          const AVFrame *src, int flags)
{
    int err;

    if (hwfc->sw_format != dst->format)
        return AVERROR(ENOSYS);

    err = rkmpp_map_frame(hwfc, dst, src, flags);
    if (err)
        return err;

    err = av_frame_copy_props(dst, src);
    if (err)
        return err;

    return 0;
}

const HWContextType ff_hwcontext_type_rkmpp = {
    .type                   = AV_HWDEVICE_TYPE_RKMPP,
    .name                   = "RKMPP",

    .device_hwctx_size      = sizeof(AVRKMPPDeviceContext),
    .frames_hwctx_size      = sizeof(AVRKMPPFramesContext),

    .device_create          = &rkmpp_device_create,

    .frames_get_constraints = &rkmpp_frames_get_constraints,

    .frames_get_buffer      = &rkmpp_get_buffer,
    .frames_init            = &rkmpp_frames_init,
    .frames_uninit          = &rkmpp_frames_uninit,
    .transfer_get_formats   = &rkmpp_transfer_get_formats,
    .transfer_data_to       = &rkmpp_transfer_data_to,
    .transfer_data_from     = &rkmpp_transfer_data_from,
    .map_from               = &rkmpp_map_from,

    .pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE
    },
};
