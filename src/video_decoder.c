#include "video_decoder.h"

#include "drm_fb.h"
#include "drm_props.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

#include <gst/gst.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_err.h>

#if defined(__has_include)
#  if __has_include(<rockchip/mpp_packet.h>)
#    include <rockchip/mpp_packet.h>
#  endif
#endif

#if defined(PIXELPILOT_DISABLE_NEON)
#define PIXELPILOT_NEON_AVAILABLE 0
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(PIXELPILOT_HAS_NEON)
#define PIXELPILOT_NEON_AVAILABLE 1
#else
#define PIXELPILOT_NEON_AVAILABLE 0
#endif

#if PIXELPILOT_NEON_AVAILABLE
#include <arm_neon.h>
#endif

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_PRIMARY 1
#endif
#ifndef DRM_PLANE_TYPE_OVERLAY
#define DRM_PLANE_TYPE_OVERLAY 0
#endif

#define DECODER_READ_BUF_SIZE (1024 * 1024)
#define DECODER_MAX_FRAMES 24

struct FrameSlot {
    int prime_fd;
    uint32_t fb_id;
    uint32_t handle;
};

struct VideoDecoder {
    gboolean initialized;
    gboolean running;
    gboolean frame_thread_running;
    gboolean display_thread_running;
    gboolean eos_received;
    gboolean lock_initialized;
    gboolean cond_initialized;

    int drm_fd;
    uint32_t plane_id;
    uint32_t crtc_id;
    int mode_w;
    int mode_h;
    uint32_t src_w;
    uint32_t src_h;

    uint32_t prop_fb_id;
    uint32_t prop_crtc_id;
    uint32_t prop_crtc_x;
    uint32_t prop_crtc_y;
    uint32_t prop_crtc_w;
    uint32_t prop_crtc_h;
    uint32_t prop_src_x;
    uint32_t prop_src_y;
    uint32_t prop_src_w;
    uint32_t prop_src_h;

    struct DumbFB background_fb;
    uint32_t background_plane_id;
    uint32_t background_prop_fb_id;
    uint32_t background_prop_crtc_id;
    uint32_t background_prop_crtc_x;
    uint32_t background_prop_crtc_y;
    uint32_t background_prop_crtc_w;
    uint32_t background_prop_crtc_h;
    uint32_t background_prop_src_x;
    uint32_t background_prop_src_y;
    uint32_t background_prop_src_w;
    uint32_t background_prop_src_h;
    uint32_t background_prop_zpos;
    gboolean background_has_zpos;
    gboolean background_initialized;

    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frm_grp;
    struct FrameSlot frame_map[DECODER_MAX_FRAMES];

    guint8 *packet_buf;
    size_t packet_buf_size;
    MppPacket packet;

    GMutex lock;
    GCond cond;
    uint32_t pending_fb;
    uint64_t pending_pts;

    GThread *frame_thread;
    GThread *display_thread;

};

VideoDecoder *video_decoder_new(void) {
    return g_new0(VideoDecoder, 1);
}

void video_decoder_free(VideoDecoder *vd) {
    if (vd == NULL) {
        return;
    }
    video_decoder_deinit(vd);
    g_free(vd);
}

static inline guint64 get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (guint64)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static inline RK_S64 gst_pts_to_mpp_timestamp(GstClockTime pts) {
    if (!GST_CLOCK_TIME_IS_VALID(pts)) {
        return (RK_S64)get_time_ms();
    }
    guint64 ms = gst_util_uint64_scale(pts, 1, GST_MSECOND);
    return (RK_S64)ms;
}

static inline void copy_packet_data(guint8 *dst, const guint8 *src, size_t size) {
#if PIXELPILOT_NEON_AVAILABLE
    /*
     * Use NEON vector loads/stores to move packets in 64-byte bursts when
     * running on ARM targets with NEON support. For remaining tail bytes we
     * fall back to memcpy to avoid reimplementing scalar copies.
     */
    if (G_UNLIKELY(size == 0)) {
        return;
    }

    size_t offset = 0;

    while (offset + 64 <= size) {
        vst1q_u8(dst + offset, vld1q_u8(src + offset));
        vst1q_u8(dst + offset + 16, vld1q_u8(src + offset + 16));
        vst1q_u8(dst + offset + 32, vld1q_u8(src + offset + 32));
        vst1q_u8(dst + offset + 48, vld1q_u8(src + offset + 48));
        offset += 64;
    }

    while (offset + 16 <= size) {
        vst1q_u8(dst + offset, vld1q_u8(src + offset));
        offset += 16;
    }

    if (offset < size) {
        memcpy(dst + offset, src + offset, size - offset);
    }
#else
    /* Fallback to memcpy on non-NEON platforms. */
    if (size > 0) {
        memcpy(dst, src, size);
    }
#endif
}

typedef void (*MppPacketSetErrinfoFunc)(MppPacket packet, RK_U32 errinfo);

static inline MppPacketSetErrinfoFunc resolve_mpp_packet_set_errinfo(void) {
    static gsize once_init = 0;
    static MppPacketSetErrinfoFunc func = NULL;

    if (g_once_init_enter(&once_init)) {
        void *symbol = NULL;
#ifdef RTLD_DEFAULT
        symbol = dlsym(RTLD_DEFAULT, "mpp_packet_set_errinfo");
#else
        void *handle = dlopen(NULL, RTLD_LAZY);
        if (handle != NULL) {
            symbol = dlsym(handle, "mpp_packet_set_errinfo");
        }
#endif
        func = (MppPacketSetErrinfoFunc)symbol;
        g_once_init_leave(&once_init, 1);
    }

    return func;
}

static inline void set_packet_errinfo_safe(MppPacket packet, gboolean corrupted) {
    MppPacketSetErrinfoFunc func = resolve_mpp_packet_set_errinfo();
    if (func != NULL) {
        func(packet, corrupted ? 1 : 0);
    }
}

static void log_decoder_neon_status_once(void) {
    static gsize once_init = 0;
    if (g_once_init_enter(&once_init)) {
#if defined(PIXELPILOT_DISABLE_NEON)
        LOGI("Video decoder: NEON packet acceleration disabled by build flag");
#elif PIXELPILOT_NEON_AVAILABLE
        LOGI("Video decoder: NEON packet acceleration enabled");
#else
        LOGI("Video decoder: NEON packet acceleration unavailable on this build");
#endif
        g_once_init_leave(&once_init, 1);
    }
}

static void reset_frame_map(VideoDecoder *vd) {
    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
        vd->frame_map[i].prime_fd = -1;
        vd->frame_map[i].fb_id = 0;
        vd->frame_map[i].handle = 0;
    }
}

static void release_frame_group(VideoDecoder *vd) {
    if (vd->frm_grp == NULL) {
        return;
    }
    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
        if (vd->frame_map[i].fb_id) {
            drmModeRmFB(vd->drm_fd, vd->frame_map[i].fb_id);
            vd->frame_map[i].fb_id = 0;
        }
        if (vd->frame_map[i].prime_fd >= 0) {
            close(vd->frame_map[i].prime_fd);
            vd->frame_map[i].prime_fd = -1;
        }
        if (vd->frame_map[i].handle) {
            struct drm_mode_destroy_dumb dmd = {.handle = vd->frame_map[i].handle};
            ioctl(vd->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
            vd->frame_map[i].handle = 0;
        }
    }
    mpp_buffer_group_clear(vd->frm_grp);
    mpp_buffer_group_put(vd->frm_grp);
    vd->frm_grp = NULL;
    vd->src_w = 0;
    vd->src_h = 0;
}

static void set_control_verbose(MppApi *mpi, MppCtx ctx, MpiCmd control, RK_U32 enable) {
    RK_U32 res = mpi->control(ctx, control, &enable);
    if (res) {
        LOGW("MPP: control %d -> %u failed (%u)", control, enable, res);
    }
}

static void set_mpp_decoding_parameters(VideoDecoder *vd) {
    MppDecCfg cfg = NULL;
    if (mpp_dec_cfg_init(&cfg) != MPP_OK) {
        LOGW("MPP: mpp_dec_cfg_init failed");
        return;
    }

    if (vd->mpi->control(vd->ctx, MPP_DEC_GET_CFG, cfg) != MPP_OK) {
        LOGW("MPP: GET_CFG failed");
        mpp_dec_cfg_deinit(cfg);
        return;
    }

    if (mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1) != MPP_OK) {
        LOGW("MPP: failed to enable split_parse");
    }

    if (vd->mpi->control(vd->ctx, MPP_DEC_SET_CFG, cfg) != MPP_OK) {
        LOGW("MPP: SET_CFG failed");
    }

    mpp_dec_cfg_deinit(cfg);

    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_DISABLE_ERROR, 1);
    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_IMMEDIATE_OUT, 1);
    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_ENABLE_FAST_PLAY, 1);
}

static int find_crtc_index(int fd, uint32_t crtc_id) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }
    int index = -1;
    for (int i = 0; i < res->count_crtcs; ++i) {
        if ((uint32_t)res->crtcs[i] == crtc_id) {
            index = i;
            break;
        }
    }
    drmModeFreeResources(res);
    return index;
}

static gboolean setup_black_background(VideoDecoder *vd) {
    if (vd->background_initialized) {
        return TRUE;
    }

    int crtc_index = find_crtc_index(vd->drm_fd, vd->crtc_id);
    if (crtc_index < 0) {
        LOGW("Video decoder: failed to locate CRTC index for background plane");
        return FALSE;
    }

    drmModePlaneRes *pres = drmModeGetPlaneResources(vd->drm_fd);
    if (!pres) {
        LOGW("Video decoder: drmModeGetPlaneResources failed");
        return FALSE;
    }

    uint32_t primary_plane = 0;
    for (uint32_t i = 0; i < pres->count_planes; ++i) {
        drmModePlane *plane = drmModeGetPlane(vd->drm_fd, pres->planes[i]);
        if (!plane) {
            continue;
        }
        gboolean matches_crtc = (plane->possible_crtcs & (1u << crtc_index)) != 0;
        if (!matches_crtc) {
            drmModeFreePlane(plane);
            continue;
        }

        drmModeObjectProperties *props = drmModeObjectGetProperties(vd->drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            drmModeFreePlane(plane);
            continue;
        }

        gboolean is_primary = FALSE;
        for (uint32_t p = 0; p < props->count_props; ++p) {
            drmModePropertyRes *prop = drmModeGetProperty(vd->drm_fd, props->props[p]);
            if (!prop) {
                continue;
            }
            if (strcmp(prop->name, "type") == 0) {
                if (props->prop_values[p] == DRM_PLANE_TYPE_PRIMARY) {
                    is_primary = TRUE;
                }
                drmModeFreeProperty(prop);
                break;
            }
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);

        if (is_primary) {
            primary_plane = plane->plane_id;
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(pres);

    if (primary_plane == 0) {
        LOGW("Video decoder: no primary plane available for background fill");
        return FALSE;
    }

    if (primary_plane == vd->plane_id) {
        LOGW("Video decoder: configured video plane is primary; skipping background fill");
        return FALSE;
    }

    if (create_argb_fb(vd->drm_fd, vd->mode_w, vd->mode_h, 0xFF000000u, &vd->background_fb) != 0) {
        LOGW("Video decoder: failed to allocate background FB: %s", g_strerror(errno));
        memset(&vd->background_fb, 0, sizeof(vd->background_fb));
        return FALSE;
    }

    if (drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", &vd->background_prop_fb_id) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &vd->background_prop_crtc_id) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", &vd->background_prop_crtc_x) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &vd->background_prop_crtc_y) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", &vd->background_prop_crtc_w) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", &vd->background_prop_crtc_h) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_X", &vd->background_prop_src_x) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", &vd->background_prop_src_y) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_W", &vd->background_prop_src_w) != 0 ||
        drm_get_prop_id(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_H", &vd->background_prop_src_h) != 0) {
        LOGW("Video decoder: failed to query background plane properties");
        destroy_dumb_fb(vd->drm_fd, &vd->background_fb);
        memset(&vd->background_fb, 0, sizeof(vd->background_fb));
        return FALSE;
    }

    uint64_t zmin = 0, zmax = 0;
    vd->background_has_zpos = (drm_get_prop_id_and_range_ci(vd->drm_fd, primary_plane, DRM_MODE_OBJECT_PLANE, "ZPOS",
                                                            &vd->background_prop_zpos, &zmin, &zmax, "zpos") == 0);

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        LOGW("Video decoder: drmModeAtomicAlloc failed for background plane");
        destroy_dumb_fb(vd->drm_fd, &vd->background_fb);
        memset(&vd->background_fb, 0, sizeof(vd->background_fb));
        vd->background_has_zpos = FALSE;
        return FALSE;
    }

    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_fb_id, vd->background_fb.fb_id);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_crtc_id, vd->crtc_id);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_crtc_x, 0);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_crtc_y, 0);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_crtc_w, vd->mode_w);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_crtc_h, vd->mode_h);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_src_x, 0);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_src_y, 0);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_src_w, (uint64_t)vd->mode_w << 16);
    drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_src_h, (uint64_t)vd->mode_h << 16);
    if (vd->background_has_zpos) {
        drmModeAtomicAddProperty(req, primary_plane, vd->background_prop_zpos, zmin);
    }

    int ret = drmModeAtomicCommit(vd->drm_fd, req, 0, NULL);
    if (ret != 0 && errno == EBUSY) {
        ret = drmModeAtomicCommit(vd->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    }
    if (ret != 0) {
        LOGW("Video decoder: failed to enable background plane: %s", g_strerror(errno));
        drmModeAtomicFree(req);
        destroy_dumb_fb(vd->drm_fd, &vd->background_fb);
        memset(&vd->background_fb, 0, sizeof(vd->background_fb));
        vd->background_has_zpos = FALSE;
        return FALSE;
    }

    drmModeAtomicFree(req);

    vd->background_plane_id = primary_plane;
    vd->background_initialized = TRUE;
    return TRUE;
}

static void teardown_background(VideoDecoder *vd) {
    if (!vd->background_initialized && vd->background_fb.fb_id == 0) {
        return;
    }

    if (vd->background_initialized) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            drmModeAtomicAddProperty(req, vd->background_plane_id, vd->background_prop_fb_id, 0);
            drmModeAtomicAddProperty(req, vd->background_plane_id, vd->background_prop_crtc_id, 0);
            int ret = drmModeAtomicCommit(vd->drm_fd, req, 0, NULL);
            if (ret != 0) {
                LOGW("Video decoder: failed to disable background plane: %s", g_strerror(errno));
            }
            drmModeAtomicFree(req);
        }
    }

    if (vd->background_fb.fb_id != 0) {
        destroy_dumb_fb(vd->drm_fd, &vd->background_fb);
        memset(&vd->background_fb, 0, sizeof(vd->background_fb));
    }

    vd->background_plane_id = 0;
    vd->background_initialized = FALSE;
    vd->background_has_zpos = FALSE;
}

static int commit_plane(VideoDecoder *vd, uint32_t fb_id, uint32_t src_w, uint32_t src_h) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -1;
    }

    if (src_w == 0) {
        src_w = vd->src_w ? vd->src_w : (uint32_t)vd->mode_w;
    } else {
        vd->src_w = src_w;
    }
    if (src_h == 0) {
        src_h = vd->src_h ? vd->src_h : (uint32_t)vd->mode_h;
    } else {
        vd->src_h = src_h;
    }

    uint32_t dst_w = vd->mode_w;
    uint32_t dst_h = vd->mode_h;
    uint32_t dst_x = 0;
    uint32_t dst_y = 0;

    if (src_w != 0 && src_h != 0) {
        double mode_aspect = (double)vd->mode_w / (double)vd->mode_h;
        double src_aspect = (double)src_w / (double)src_h;

        if (mode_aspect > 0.0 && src_aspect > 0.0) {
            if (src_aspect > mode_aspect) {
                dst_w = vd->mode_w;
                dst_h = (uint32_t)((double)vd->mode_w / src_aspect + 0.5);
                if (dst_h > vd->mode_h) {
                    dst_h = vd->mode_h;
                }
            } else {
                dst_h = vd->mode_h;
                dst_w = (uint32_t)((double)vd->mode_h * src_aspect + 0.5);
                if (dst_w > vd->mode_w) {
                    dst_w = vd->mode_w;
                }
            }
        }
    }

    if (dst_w == 0) {
        dst_w = 1;
    }
    if (dst_h == 0) {
        dst_h = 1;
    }

    if (dst_w < vd->mode_w) {
        dst_x = (vd->mode_w - dst_w) / 2;
    }
    if (dst_h < vd->mode_h) {
        dst_y = (vd->mode_h - dst_h) / 2;
    }

    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_fb_id, fb_id);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_id, vd->crtc_id);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_x, dst_x);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_y, dst_y);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_w, dst_w);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_crtc_h, dst_h);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_x, 0);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_y, 0);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_w, (uint64_t)src_w << 16);
    drmModeAtomicAddProperty(req, vd->plane_id, vd->prop_src_h, (uint64_t)src_h << 16);

    int ret = drmModeAtomicCommit(vd->drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    if (ret != 0 && errno == EBUSY) {
        ret = drmModeAtomicCommit(vd->drm_fd, req, 0, NULL);
    }
    if (ret != 0) {
        LOGW("Atomic commit failed: %s", g_strerror(errno));
    }
    drmModeAtomicFree(req);
    return ret;
}

static int setup_external_buffers(VideoDecoder *vd, MppFrame frame) {
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);

    if (fmt != MPP_FMT_YUV420SP && fmt != MPP_FMT_YUV420SP_10BIT) {
        LOGE("MPP: unexpected format %d", fmt);
        return -1;
    }

    release_frame_group(vd);

    if (mpp_buffer_group_get_external(&vd->frm_grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        LOGE("MPP: failed to get external buffer group");
        return -1;
    }

    reset_frame_map(vd);

    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
        struct drm_mode_create_dumb dmcd;
        memset(&dmcd, 0, sizeof(dmcd));
        dmcd.bpp = (fmt == MPP_FMT_YUV420SP) ? 8 : 10;
        dmcd.width = hor_stride;
        dmcd.height = ver_stride * 2;

        int ret;
        do {
            ret = ioctl(vd->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        if (ret != 0) {
            LOGW("DRM_IOCTL_MODE_CREATE_DUMB failed: %s", g_strerror(errno));
            continue;
        }
        vd->frame_map[i].handle = dmcd.handle;

        struct drm_prime_handle dph;
        memset(&dph, 0, sizeof(dph));
        dph.handle = dmcd.handle;
        dph.fd = -1;
        do {
            ret = ioctl(vd->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
        if (ret != 0) {
            LOGW("PRIME_HANDLE_TO_FD failed: %s", g_strerror(errno));
            continue;
        }

        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DRM;
        info.size = dmcd.size;
        info.fd = dph.fd;
        ret = mpp_buffer_commit(vd->frm_grp, &info);
        if (ret != MPP_OK) {
            LOGW("MPP: buffer_commit failed (%d)", ret);
            close(dph.fd);
            continue;
        }
        vd->frame_map[i].prime_fd = info.fd;
        if (dph.fd != info.fd) {
            close(dph.fd);
        }

        uint32_t handles[4] = {0};
        uint32_t pitches[4] = {0};
        uint32_t offsets[4] = {0};
        handles[0] = vd->frame_map[i].handle;
        handles[1] = vd->frame_map[i].handle;
        pitches[0] = dmcd.pitch;
        pitches[1] = dmcd.pitch;
        offsets[0] = 0;
        offsets[1] = dmcd.pitch * ver_stride;

        ret = drmModeAddFB2(vd->drm_fd, width, height, DRM_FORMAT_NV12, handles, pitches, offsets,
                            &vd->frame_map[i].fb_id, 0);
        if (ret != 0) {
            LOGW("drmModeAddFB2 failed: %s", g_strerror(errno));
            continue;
        }
    }

    vd->mpi->control(vd->ctx, MPP_DEC_SET_EXT_BUF_GROUP, vd->frm_grp);
    vd->mpi->control(vd->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

    vd->src_w = width;
    vd->src_h = height;
    commit_plane(vd, vd->frame_map[0].fb_id, width, height);
    return 0;
}

static gpointer frame_thread_func(gpointer data) {
    VideoDecoder *vd = (VideoDecoder *)data;
    vd->frame_thread_running = TRUE;
    while (TRUE) {
        if (!vd->running) {
            break;
        }
        MppFrame frame = NULL;
        MPP_RET ret = vd->mpi->decode_get_frame(vd->ctx, &frame);
        if (ret != MPP_OK || frame == NULL) {
            g_usleep(1000);
            continue;
        }

        if (mpp_frame_get_info_change(frame)) {
            setup_external_buffers(vd, frame);
        } else {
            RK_U32 errinfo = mpp_frame_get_errinfo(frame);
            RK_U32 discard = mpp_frame_get_discard(frame);
            if (G_UNLIKELY(errinfo || discard)) {
                LOGW("MPP: dropping frame errinfo=%u discard=%u", errinfo, discard);
                vd->eos_received = mpp_frame_get_eos(frame) ? TRUE : FALSE;
                mpp_frame_deinit(&frame);
                if (vd->eos_received) {
                    break;
                }
                continue;
            }

            MppBuffer buffer = mpp_frame_get_buffer(frame);
            if (buffer != NULL) {
                MppBufferInfo info;
                memset(&info, 0, sizeof(info));
                if (mpp_buffer_info_get(buffer, &info) == MPP_OK) {
                    for (int i = 0; i < DECODER_MAX_FRAMES; ++i) {
                        if (vd->frame_map[i].prime_fd == info.fd) {
                            g_mutex_lock(&vd->lock);
                            vd->pending_fb = vd->frame_map[i].fb_id;
                            vd->pending_pts = mpp_frame_get_pts(frame);
                            g_cond_signal(&vd->cond);
                            g_mutex_unlock(&vd->lock);
                            break;
                        }
                    }
                }
            }
        }

        vd->eos_received = mpp_frame_get_eos(frame) ? TRUE : FALSE;
        mpp_frame_deinit(&frame);
        if (vd->eos_received) {
            break;
        }
    }

    g_mutex_lock(&vd->lock);
    vd->running = FALSE;
    g_cond_broadcast(&vd->cond);
    g_mutex_unlock(&vd->lock);
    vd->frame_thread_running = FALSE;
    return NULL;
}

static gpointer display_thread_func(gpointer data) {
    VideoDecoder *vd = (VideoDecoder *)data;
    vd->display_thread_running = TRUE;
    while (TRUE) {
        g_mutex_lock(&vd->lock);
        while (vd->running && vd->pending_fb == 0) {
            g_cond_wait(&vd->cond, &vd->lock);
        }
        uint32_t fb = vd->pending_fb;
        vd->pending_fb = 0;
        gboolean still_running = vd->running;
        g_mutex_unlock(&vd->lock);

        if (!still_running && fb == 0) {
            break;
        }
        if (fb != 0) {
            commit_plane(vd, fb, 0, 0);
        }
        if (!still_running) {
            break;
        }
    }
    vd->display_thread_running = FALSE;
    return NULL;
}

size_t video_decoder_max_packet_size(const VideoDecoder *vd) {
    if (vd == NULL || vd->packet_buf_size == 0) {
        return DECODER_READ_BUF_SIZE;
    }
    return vd->packet_buf_size;
}

int video_decoder_init(VideoDecoder *vd, const AppCfg *cfg, const ModesetResult *ms, int drm_fd) {
    if (vd == NULL || cfg == NULL || ms == NULL) {
        return -1;
    }

    log_decoder_neon_status_once();

    memset(vd, 0, sizeof(*vd));
    vd->drm_fd = -1;
    vd->plane_id = (uint32_t)cfg->plane_id;
    vd->crtc_id = ms->crtc_id;
    vd->mode_w = ms->mode_w;
    vd->mode_h = ms->mode_h;
    vd->packet_buf_size = DECODER_READ_BUF_SIZE;
    vd->packet_buf = g_malloc0(vd->packet_buf_size);
    if (vd->packet_buf == NULL) {
        LOGE("Video decoder: failed to allocate packet buffer");
        return -1;
    }

    int dup_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        LOGE("Video decoder: failed to dup DRM fd: %s", g_strerror(errno));
        g_free(vd->packet_buf);
        vd->packet_buf = NULL;
        return -1;
    }
    vd->drm_fd = dup_fd;

    if (drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &vd->prop_fb_id) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &vd->prop_crtc_id) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &vd->prop_crtc_x) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &vd->prop_crtc_y) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &vd->prop_crtc_w) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &vd->prop_crtc_h) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &vd->prop_src_x) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &vd->prop_src_y) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &vd->prop_src_w) != 0 ||
        drm_get_prop_id(vd->drm_fd, vd->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &vd->prop_src_h) != 0) {
        LOGE("Video decoder: failed to query plane properties");
        video_decoder_deinit(vd);
        return -1;
    }

    if (!setup_black_background(vd)) {
        LOGW("Video decoder: continuing without background plane");
    }

    if (mpp_create(&vd->ctx, &vd->mpi) != MPP_OK) {
        LOGE("Video decoder: mpp_create failed");
        video_decoder_deinit(vd);
        return -1;
    }

    if (mpp_packet_init(&vd->packet, vd->packet_buf, vd->packet_buf_size) != MPP_OK) {
        LOGE("Video decoder: mpp_packet_init failed");
        video_decoder_deinit(vd);
        return -1;
    }

    const RK_U32 need_split = 1;
    set_control_verbose(vd->mpi, vd->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, need_split);

    MppCodingType coding = MPP_VIDEO_CodingHEVC;
    if (mpp_init(vd->ctx, MPP_CTX_DEC, coding) != MPP_OK) {
        LOGE("Video decoder: mpp_init failed");
        video_decoder_deinit(vd);
        return -1;
    }

    set_mpp_decoding_parameters(vd);

#if defined(MPP_SET_OUTPUT_TIMEOUT)
    int64_t timeout = 5000; // allow decode_get_frame() to wake at ~5 ms intervals
    if (vd->mpi->control(vd->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK) {
        LOGW("Video decoder: failed to set output timeout");
    }
#else
    int block = 5; // poll with a small timeout so shutdown does not block indefinitely
    if (vd->mpi->control(vd->ctx, MPP_SET_OUTPUT_BLOCK, &block) != MPP_OK) {
        LOGW("Video decoder: failed to set output block timeout");
    }
#endif

    g_mutex_init(&vd->lock);
    g_cond_init(&vd->cond);
    vd->lock_initialized = TRUE;
    vd->cond_initialized = TRUE;
    vd->pending_fb = 0;
    vd->pending_pts = 0;
    vd->frm_grp = NULL;
    reset_frame_map(vd);

    vd->initialized = TRUE;
    return 0;
}

void video_decoder_deinit(VideoDecoder *vd) {
    if (vd == NULL) {
        return;
    }

    video_decoder_stop(vd);

    if (vd->packet) {
        mpp_packet_deinit(&vd->packet);
        vd->packet = NULL;
    }

    if (vd->mpi && vd->ctx) {
        vd->mpi->reset(vd->ctx);
        mpp_destroy(vd->ctx);
    }
    vd->ctx = NULL;
    vd->mpi = NULL;

    release_frame_group(vd);

    teardown_background(vd);

    if (vd->packet_buf) {
        g_free(vd->packet_buf);
        vd->packet_buf = NULL;
    }

    if (vd->drm_fd >= 0) {
        close(vd->drm_fd);
        vd->drm_fd = -1;
    }

    if (vd->lock_initialized) {
        g_mutex_clear(&vd->lock);
        vd->lock_initialized = FALSE;
    }
    if (vd->cond_initialized) {
        g_cond_clear(&vd->cond);
        vd->cond_initialized = FALSE;
    }
    vd->initialized = FALSE;
}

int video_decoder_start(VideoDecoder *vd) {
    if (vd == NULL || !vd->initialized) {
        return -1;
    }
    if (vd->running) {
        return 0;
    }

    vd->running = TRUE;
    vd->eos_received = FALSE;

    vd->frame_thread = g_thread_new("mpp-frame", frame_thread_func, vd);
    if (vd->frame_thread == NULL) {
        vd->running = FALSE;
        return -1;
    }
    vd->display_thread = g_thread_new("mpp-display", display_thread_func, vd);
    if (vd->display_thread == NULL) {
        vd->running = FALSE;
        g_thread_join(vd->frame_thread);
        vd->frame_thread = NULL;
        return -1;
    }
    return 0;
}

void video_decoder_stop(VideoDecoder *vd) {
    if (vd == NULL) {
        return;
    }

    gboolean was_running = FALSE;

    if (vd->lock_initialized) {
        g_mutex_lock(&vd->lock);
        was_running = vd->running;
        vd->running = FALSE;
        if (vd->cond_initialized) {
            g_cond_broadcast(&vd->cond);
        }
        g_mutex_unlock(&vd->lock);
    } else {
        was_running = vd->running;
        vd->running = FALSE;
        if (vd->cond_initialized) {
            g_cond_broadcast(&vd->cond);
        }
    }

    if (was_running && vd->mpi && vd->ctx) {
        video_decoder_send_eos(vd);

        MPP_RET ret = vd->mpi->reset(vd->ctx);
        if (ret != MPP_OK) {
            LOGW("Video decoder: reset during stop failed (%d)", ret);
        }
    }

    if (vd->frame_thread) {
        g_thread_join(vd->frame_thread);
        vd->frame_thread = NULL;
    }
    if (vd->display_thread) {
        g_thread_join(vd->display_thread);
        vd->display_thread = NULL;
    }
}

int video_decoder_feed(VideoDecoder *vd,
                       const guint8 *data,
                       size_t size,
                       GstClockTime pts,
                       gboolean corrupted,
                       gboolean discontinuity) {
    if (vd == NULL || !vd->running) {
        return -1;
    }
    if (size == 0 || size > vd->packet_buf_size) {
        return -1;
    }

    copy_packet_data(vd->packet_buf, data, size);
    mpp_packet_set_length(vd->packet, 0);
    mpp_packet_set_size(vd->packet, vd->packet_buf_size);
    mpp_packet_set_data(vd->packet, vd->packet_buf);
    mpp_packet_set_pos(vd->packet, vd->packet_buf);
    mpp_packet_set_length(vd->packet, size);
    RK_S64 packet_pts = gst_pts_to_mpp_timestamp(pts);
    mpp_packet_set_pts(vd->packet, packet_pts);
    mpp_packet_set_dts(vd->packet, packet_pts);

    set_packet_errinfo_safe(vd->packet, corrupted);

    (void)discontinuity;

    while (vd->running) {
        MPP_RET ret = vd->mpi->decode_put_packet(vd->ctx, vd->packet);
        if (ret == MPP_OK) {
            return 0;
        }
        g_usleep(2000);
    }
    return -1;
}

void video_decoder_send_eos(VideoDecoder *vd) {
    if (vd == NULL || vd->packet == NULL) {
        return;
    }

    mpp_packet_set_length(vd->packet, 0);
    mpp_packet_set_size(vd->packet, vd->packet_buf_size);
    mpp_packet_set_data(vd->packet, vd->packet_buf);
    mpp_packet_set_pos(vd->packet, vd->packet_buf);
    mpp_packet_set_eos(vd->packet);

    int attempts = 0;
    while (vd->mpi && vd->ctx) {
        MPP_RET ret = vd->mpi->decode_put_packet(vd->ctx, vd->packet);
        if (ret == MPP_OK) {
            break;
        }
        if (ret != MPP_ERR_BUFFER_FULL) {
            LOGW("Video decoder: decode_put_packet(EOS) failed (%d)", ret);
        }
        if (++attempts > 50) {
            LOGW("Video decoder: giving up on EOS after %d attempts", attempts);
            break;
        }
        g_usleep(2000);
    }
}
