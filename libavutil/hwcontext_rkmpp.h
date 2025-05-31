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
#ifndef DRM_FORMAT_P210
#define DRM_FORMAT_P210         fourcc_code('P', '2', '1', '0')
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
#ifndef DRM_FORMAT_VUY888
#define DRM_FORMAT_VUY888       fourcc_code('V', 'U', '2', '4')
#endif

/* ARM AFBC (16x16) */
#ifndef DRM_FORMAT_MOD_VENDOR_ARM
#define DRM_FORMAT_MOD_VENDOR_ARM          0x08
#endif
#ifndef DRM_FORMAT_MOD_ARM_TYPE_AFBC
#define DRM_FORMAT_MOD_ARM_TYPE_AFBC       0x00
#endif
#ifndef AFBC_FORMAT_MOD_BLOCK_SIZE_16x16
#define AFBC_FORMAT_MOD_BLOCK_SIZE_16x16   (1ULL)
#endif
#ifndef AFBC_FORMAT_MOD_SPARSE
#define AFBC_FORMAT_MOD_SPARSE             (1ULL << 6)
#endif

#define drm_is_afbc(mod) \
        ((mod >> 52) == (DRM_FORMAT_MOD_ARM_TYPE_AFBC | \
                (DRM_FORMAT_MOD_VENDOR_ARM << 4)))

/* Rockchip RFBC (64x4) */
#undef  DRM_FORMAT_MOD_VENDOR_ROCKCHIP
#define DRM_FORMAT_MOD_VENDOR_ROCKCHIP     0x0b
#undef  DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT
#define DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT 52
#undef  DRM_FORMAT_MOD_ROCKCHIP_TYPE_MASK
#define DRM_FORMAT_MOD_ROCKCHIP_TYPE_MASK  0xf
#undef  DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC
#define DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC  0x1
#undef  ROCKCHIP_RFBC_BLOCK_SIZE_64x4
#define ROCKCHIP_RFBC_BLOCK_SIZE_64x4      (1ULL)

#undef  fourcc_mod_code
#define fourcc_mod_code(vendor, val) \
        ((((__u64)DRM_FORMAT_MOD_VENDOR_## vendor) << 56) | ((val) & 0x00ffffffffffffffULL))

#undef  DRM_FORMAT_MOD_ROCKCHIP_CODE
#define DRM_FORMAT_MOD_ROCKCHIP_CODE(__type, __val) \
	fourcc_mod_code(ROCKCHIP, ((__u64)(__type) << DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT) | \
			((__val) & 0x000fffffffffffffULL))

#undef  DRM_FORMAT_MOD_ROCKCHIP_RFBC
#define DRM_FORMAT_MOD_ROCKCHIP_RFBC(mode) \
	DRM_FORMAT_MOD_ROCKCHIP_CODE(DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC, mode)

#define drm_is_rfbc(mod) \
        (((mod >> 56) & 0xff) == DRM_FORMAT_MOD_VENDOR_ROCKCHIP) && \
        (((mod >> 52) & DRM_FORMAT_MOD_ROCKCHIP_TYPE_MASK) == DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC)

/**
 * DRM Prime Frame descriptor for RKMPP HWDevice.
 */
typedef struct AVRKMPPDRMFrameDescriptor {
    /**
     * Backwards compatibility with AVDRMFrameDescriptor.
     */
    AVDRMFrameDescriptor drm_desc;

    /**
     * References to MppBuffer instances which are used
     * on each drm frame index.
     */
    MppBuffer buffers[AV_DRM_MAX_PLANES];
} AVRKMPPDRMFrameDescriptor;

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
     * MPP buffer allocation flags at frames context level.
     */
    int flags;

    /**
     * The descriptors of all frames in the pool after creation.
     * Only valid if AVHWFramesContext.initial_pool_size was positive.
     * These are intended to be used as the buffer of RKMPP decoder.
     */
    AVRKMPPDRMFrameDescriptor *frames;
    int                     nb_frames;
} AVRKMPPFramesContext;

/**
 * RKMPP device details.
 *
 * Allocated as AVHWDeviceContext.hwctx
 */
typedef struct AVRKMPPDeviceContext {
    /**
     * MPP buffer allocation flags at device context level.
     */
    int flags;
} AVRKMPPDeviceContext;

#endif /* AVUTIL_HWCONTEXT_RKMPP_H */
