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

#ifndef AVUTIL_HWCONTEXT_RKMPP_H
#define AVUTIL_HWCONTEXT_RKMPP_H

#include <stddef.h>
#include <stdint.h>
#include <drm_fourcc.h>
#include <rockchip/rk_mpi.h>

#include "hwcontext_drm.h"

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010         fourcc_code('P', '0', '1', '0')
#endif
#ifndef DRM_FORMAT_NV15
#define DRM_FORMAT_NV15         fourcc_code('N', 'V', '1', '5')
#endif
#ifndef DRM_FORMAT_NV20
#define DRM_FORMAT_NV20         fourcc_code('N', 'V', '2', '0')
#endif
#ifndef DRM_FORMAT_YUV420_8BIT
#define DRM_FORMAT_YUV420_8BIT  fourcc_code('Y', 'U', '0', '8')
#endif
#ifndef DRM_FORMAT_YUV420_10BIT
#define DRM_FORMAT_YUV420_10BIT fourcc_code('Y', 'U', '1', '0')
#endif
#ifndef DRM_FORMAT_Y210
#define DRM_FORMAT_Y210         fourcc_code('Y', '2', '1', '0')
#endif

#ifndef DRM_FORMAT_MOD_VENDOR_ARM
#define DRM_FORMAT_MOD_VENDOR_ARM 0x08
#endif
#ifndef DRM_FORMAT_MOD_ARM_TYPE_AFBC
#define DRM_FORMAT_MOD_ARM_TYPE_AFBC 0x00
#endif

#define drm_is_afbc(mod) \
        ((mod >> 52) == (DRM_FORMAT_MOD_ARM_TYPE_AFBC | \
                (DRM_FORMAT_MOD_VENDOR_ARM << 4)))

/**
 * RKMPP-specific data associated with a frame pool.
 *
 * Allocated as AVHWFramesContext.hwctx.
 */
typedef struct AVRKMPPFramesContext {
    /**
     * MPP buffer group.
     */
    MppBufferGroup buf_group;

    /**
     * The descriptors of all frames in the pool after creation.
     * Only valid if AVHWFramesContext.initial_pool_size was positive.
     * These are intended to be used as the buffer of RKMPP decoder.
     */
    AVDRMFrameDescriptor *frames;
    int                nb_frames;
} AVRKMPPFramesContext;

/**
 * RKMPP device details.
 *
 * Allocated as AVHWDeviceContext.hwctx
 */
typedef struct AVRKMPPDeviceContext {
    /**
     * MPP buffer allocation flags.
     */
    int flags;
} AVRKMPPDeviceContext;

#endif /* AVUTIL_HWCONTEXT_RKMPP_H */
