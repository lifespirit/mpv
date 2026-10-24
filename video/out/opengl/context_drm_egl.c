/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>

#include "mpv/render_gl.h"
#include "common/common.h"
#include "osdep/timer.h"
#include "video/out/drm_atomic.h"
#include "video/out/drm_common.h"
#include "video/out/present_sync.h"

#include "egl_helpers.h"
#include "common.h"
#include "context.h"

#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x31D7
#endif

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

struct gbm_frame {
    struct gbm_bo *bo;
    int stereo_eye; // -1 for ordinary frames
    bool stereo_target_valid;
    uint32_t stereo_target_msc;
    int64_t stereo_draw_time_ns;
};

struct gbm {
    struct gbm_surface *surface;
    struct gbm_device *device;
    struct gbm_frame **bo_queue;
    unsigned int num_bos;
};

struct egl {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
};

struct stereo_pair {
    struct gbm_bo *bo[2];
    // Exported EGL_ANDROID_native_fence_sync descriptors for the two eyes.
    // Zero means absent: exported fds are moved above stderr if necessary.
    int fence_fd[2];
    // Number and ages of L-boundary attempts with unsignaled GPU work.
    unsigned fence_deferrals;
    int64_t first_fence_deferral_age_ns;
    int64_t last_fence_deferral_age_ns;
    uint32_t last_fence_deferral_msc;
    // Diagnostic R-vblank probe after two unsuccessful L promotions.
    // Does not affect pair selection. 0 age means no sample taken.
    int64_t intermediate_r_probe_age_ns;
    uint32_t intermediate_r_probe_msc;
    int intermediate_r_fence_L;
    int intermediate_r_fence_R;
    uint64_t generation;
    // Source movie identity carried from mpv's VO to the first confirmed L.
    // PTS is in MEDIA seconds (not an mp_time_ns realtime target).
    uint64_t frame_id;
    double media_pts;
    bool pts_valid;
    int planned_vsyncs;
    bool display_synced;
    // Set only on an actually confirmed FIRST LEFT of this generation.
    bool first_left_confirmed;
    uint32_t first_left_msc;
    // Producer-side render/submit timings; mp_time_ns is CPU monotonic.
    int64_t render_ns[2], egl_swap_ns[2], fence_export_ns[2];
    // CPU time when BOTH eye GBM front buffers were published as a pair.
    int64_t ready_ns;
};

// Opt-in NVIDIA 3D Vision diagnostic. The USB thread never owns GBM/DRM
// objects and no synchronous libusb calls run on the 120Hz KMS worker.
// A one-entry, versioned atomic mailbox keeps only the latest confirmed flip.
struct stereo_usb {
    struct ra_ctx *ctx;
    pthread_t thread;
    int event_fd;
    int control_fd;
    char *control_path;
    uint64_t control_updates, control_errors;
    int final_eye_timer_us;
    bool started;
    bool invert_eyes;
    bool dry_run;
    bool timestamp_monotonic;
    atomic_int init_status; // 1=ready; -1=dlopen; -2=symbols; -3=device; -4=rate; -5=timer mismatch
    char *firmware;
    char *library;
    float refresh_hz;
    atomic_bool stop;
    atomic_uint_fast64_t mailbox_seq;
    atomic_uint_fast64_t mailbox_event;
    atomic_uint_fast64_t mailbox_ust_us;
    atomic_uint_fast64_t confirmed;
    atomic_uint_fast64_t first_confirmed_msc_plus_one;
    atomic_uint_fast64_t wake_errors;
    // The sender alone updates these; read after pthread_join only.
    // Startup frames lost while firmware/device initialization was still
    // running are intentionally separate from steady-state mailbox skips.
    uint64_t sent, startup_skipped, skipped, late_2ms, late_period, usb_errors;
    uint64_t signed_lag_samples, signed_lag_negative;
    int usb_error_code;
    double min_signed_scanout_to_send_ms, max_signed_scanout_to_send_ms;
    double max_scanout_to_send_ms, max_usb_transfer_ms;
};

struct stereo_kms {
    bool enabled;
    bool sequence_mode;
    bool rt_mode;
    bool gpu_wait;
    bool legacy_mode;
    // Native --drm-stereo-3d defaults to planned MSC hold; independent
    // explicit 0/1 environment override for regressions.
    bool plan_hold;
    // Independent mpv VO wake edge; presentation feedback stays attached
    // to the configured --drm-stereo-pacing-eye. Default is the same eye.
    int render_wake_eye;
    // Experimental one-slot priority rule: keep a ready pending pair
    // rather than discarding it when a newer movie frame is rendered.
    bool keep_ready_pending;
    bool two_slot_queue; // Bounded FIFO: native stereo default, opt-out via env.
    struct stereo_usb usb; // Optional IR bridge (disabled by default).
    uint64_t queue_second_enqueues, queue_second_promotions;
    uint64_t queue_full_skips, queue_gbm_skips, queue_unexpected_incoming;
    uint64_t queue_seek_resets, queue_seek_pending_flushes;
    uint64_t queue_seek_queued_flushes, queue_seek_building_flushes;
    // One-shot, explicit stress-test-only KMS promotion gate. It never
    // stops physical L/R flips: only holds pending while both slots are
    // occupied, giving the IPC test time to issue a seek.
    int queue_test_hold_ms;
    int64_t queue_test_hold_until_ns;
    bool queue_test_triggered;
    uint64_t queue_test_hold_blocks;
    // eglSwapInterval is per EGL draw surface, not per display/context.
    // The legacy stereo init sets interval 0 on the left surface only.
    // Opt-in experiment: explicitly set the right surface to 0 too.
    bool right_swap_interval_zero;
    bool right_swap_interval_configured;
    // Diagnostic control: keep rendering new frames, but use only the
    // first complete pair for all KMS scanout flips.
    bool freeze_pair;
    // Experimental nonblocking native-fence readiness gate. The EGL
    // producer exports sync_file fds; the RT KMS worker only polls them.
    bool native_fence;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLCLIENTWAITSYNCKHRPROC client_wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;
    // Wait for a completed pair to age before first KMS promotion.
    // A controlled experiment to separate fresh-BO readiness from
    // recurring display-timing problems; disabled by default.
    int64_t min_pair_age_ns;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t flip_ready;
    uint64_t confirmed_flips;
    uint64_t vo_seen_flips;
    bool initialized, started, stop, first_flip_done;
    pthread_t thread;

    struct gbm_surface *right_gbm;
    EGLSurface right_egl;
    int capture_eye;
    struct stereo_pair building;
    struct stereo_pair active, pending, queued;
    struct stereo_pair retired[8];
    unsigned num_retired;
    uint64_t next_generation;
};

struct priv {
    GL gl;

    struct egl egl;
    struct gbm gbm;
    struct stereo_kms stereo;

    uint32_t gbm_format;
    uint64_t *gbm_modifiers;
    unsigned int num_gbm_modifiers;

    struct mpv_opengl_drm_params_v2 drm_params;
    struct mpv_opengl_drm_draw_surface_size draw_surface_size;

    bool allow_modeset;
};

// Not general. Limited to only the formats being used in this module
static const char *gbm_format_to_string(uint32_t format)
{
    switch (format) {
    case GBM_FORMAT_XRGB8888:
        return "GBM_FORMAT_XRGB8888";
    case GBM_FORMAT_ARGB8888:
        return "GBM_FORMAT_ARGB8888";
    case GBM_FORMAT_XBGR8888:
        return "GBM_FORMAT_XBGR8888";
    case GBM_FORMAT_ABGR8888:
        return "GBM_FORMAT_ABGR8888";
    case GBM_FORMAT_XRGB2101010:
        return "GBM_FORMAT_XRGB2101010";
    case GBM_FORMAT_ARGB2101010:
        return "GBM_FORMAT_ARGB2101010";
    case GBM_FORMAT_XBGR2101010:
        return "GBM_FORMAT_XBGR2101010";
    case GBM_FORMAT_ABGR2101010:
        return "GBM_FORMAT_ABGR2101010";
    default:
        return "UNKNOWN";
    }
}

// Allow falling back to an ARGB EGLConfig when we have an XRGB framebuffer.
// Also allow falling back to an XRGB EGLConfig for ARGB framebuffers, since
// this seems necessary to work with broken Mali drivers that don't report
// their EGLConfigs as supporting alpha properly.
static uint32_t fallback_format_for(uint32_t format)
{
    switch (format) {
    case GBM_FORMAT_XRGB8888:
        return GBM_FORMAT_ARGB8888;
    case GBM_FORMAT_ARGB8888:
        return GBM_FORMAT_XRGB8888;
    case GBM_FORMAT_XBGR8888:
        return GBM_FORMAT_ABGR8888;
    case GBM_FORMAT_ABGR8888:
        return GBM_FORMAT_XBGR8888;
    case GBM_FORMAT_XRGB2101010:
        return GBM_FORMAT_ARGB2101010;
    case GBM_FORMAT_ARGB2101010:
        return GBM_FORMAT_XRGB2101010;
    case GBM_FORMAT_XBGR2101010:
        return GBM_FORMAT_ABGR2101010;
    case GBM_FORMAT_ABGR2101010:
        return GBM_FORMAT_XBGR2101010;
    default:
        return 0;
    }
}

static int match_config_to_visual(void *user_data, EGLConfig *configs, int num_configs)
{
    struct ra_ctx *ctx = (struct ra_ctx*)user_data;
    struct priv *p = ctx->priv;
    const EGLint visual_id[] = {
        (EGLint)p->gbm_format,
        (EGLint)fallback_format_for(p->gbm_format),
        0
    };

    for (unsigned int i = 0; visual_id[i] != 0; ++i) {
        MP_VERBOSE(ctx, "Attempting to find EGLConfig matching %s\n",
                   gbm_format_to_string(visual_id[i]));
        for (unsigned int j = 0; j < num_configs; ++j) {
            EGLint id;

            if (!eglGetConfigAttrib(p->egl.display, configs[j], EGL_NATIVE_VISUAL_ID, &id))
                continue;

            if (visual_id[i] == id) {
                MP_VERBOSE(ctx, "Found matching EGLConfig for %s\n",
                           gbm_format_to_string(visual_id[i]));
                return j;
            }
        }
        MP_VERBOSE(ctx, "No matching EGLConfig for %s\n", gbm_format_to_string(visual_id[i]));
    }

    MP_ERR(ctx, "Could not find EGLConfig matching the GBM visual (%s).\n",
           gbm_format_to_string(p->gbm_format));
    return -1;
}

static EGLDisplay egl_get_display(struct gbm_device *gbm_device)
{
    EGLDisplay ret;

    ret = mpegl_get_display(EGL_PLATFORM_GBM_MESA, "EGL_MESA_platform_gbm", gbm_device);
    if (ret != EGL_NO_DISPLAY)
        return ret;

    ret = mpegl_get_display(EGL_PLATFORM_GBM_KHR, "EGL_KHR_platform_gbm", gbm_device);
    if (ret != EGL_NO_DISPLAY)
        return ret;

    return eglGetDisplay(gbm_device);
}

static bool init_egl(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    MP_VERBOSE(ctx, "Initializing EGL\n");
    p->egl.display = egl_get_display(p->gbm.device);

    if (p->egl.display == EGL_NO_DISPLAY) {
        MP_ERR(ctx, "Failed to get EGL display.\n");
        return false;
    }
    if (!eglInitialize(p->egl.display, NULL, NULL)) {
        MP_ERR(ctx, "Failed to initialize EGL.\n");
        return false;
    }
    EGLConfig config;
    if (!mpegl_create_context_cb(ctx,
                                 p->egl.display,
                                 (struct mpegl_cb){match_config_to_visual, ctx},
                                 &p->egl.context,
                                 &config))
        return false;

    p->egl.config = config;
    MP_VERBOSE(ctx, "Initializing EGL surface\n");
    p->egl.surface = mpegl_create_window_surface(
        p->egl.display, config, p->gbm.surface);
    if (p->egl.surface == EGL_NO_SURFACE) {
        p->egl.surface = eglCreateWindowSurface(
            p->egl.display, config, p->gbm.surface, NULL);
    }
    if (p->egl.surface == EGL_NO_SURFACE) {
        MP_ERR(ctx, "Failed to create EGL surface.\n");
        return false;
    }
    return true;
}

static bool init_gbm(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;
    MP_VERBOSE(ctx->vo, "Creating GBM device\n");
    p->gbm.device = gbm_create_device(drm->fd);
    if (!p->gbm.device) {
        MP_ERR(ctx->vo, "Failed to create GBM device.\n");
        return false;
    }

    MP_VERBOSE(ctx->vo, "Initializing GBM surface (%d x %d)\n",
        p->draw_surface_size.width, p->draw_surface_size.height);
    if (p->num_gbm_modifiers == 0) {
        p->gbm.surface = gbm_surface_create(
            p->gbm.device,
            p->draw_surface_size.width,
            p->draw_surface_size.height,
            p->gbm_format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    } else {
        p->gbm.surface = gbm_surface_create_with_modifiers(
            p->gbm.device,
            p->draw_surface_size.width,
            p->draw_surface_size.height,
            p->gbm_format,
            p->gbm_modifiers,
            p->num_gbm_modifiers);
    }
    if (!p->gbm.surface) {
        MP_ERR(ctx->vo, "Failed to create GBM surface.\n");
        return false;
    }
    return true;
}


// Pair mode uses two separate EGL/GBM surfaces so the source renderer can
// prepare an eye independently of the BO currently used by KMS. With a
// single GBM surface, 2 pinned eye BOs + 2 staging BOs would exhaust Mesa's
// 3-buffer surface. Each per-eye surface needs at most 3 BOs.
static bool init_pair_surfaces(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled)
        return true;

    if (p->num_gbm_modifiers) {
        s->right_gbm = gbm_surface_create_with_modifiers(
            p->gbm.device, p->draw_surface_size.width,
            p->draw_surface_size.height, p->gbm_format,
            p->gbm_modifiers, p->num_gbm_modifiers);
    } else {
        s->right_gbm = gbm_surface_create(
            p->gbm.device, p->draw_surface_size.width,
            p->draw_surface_size.height, p->gbm_format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }
    if (!s->right_gbm)
        return false;

    s->right_egl = mpegl_create_window_surface(
        p->egl.display, p->egl.config, s->right_gbm);
    if (s->right_egl == EGL_NO_SURFACE) {
        s->right_egl = eglCreateWindowSurface(
            p->egl.display, p->egl.config, s->right_gbm, NULL);
    }
    return s->right_egl != EGL_NO_SURFACE;
}

static void framebuffer_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct framebuffer *fb = data;
    if (fb) {
        drmModeRmFB(fb->fd, fb->id);
    }
}

static void update_framebuffer_from_bo(struct ra_ctx *ctx, struct gbm_bo *bo)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;
    struct framebuffer *fb = gbm_bo_get_user_data(bo);
    if (fb) {
        drm->fb = fb;
        return;
    }

    fb = talloc_zero(ctx, struct framebuffer);
    fb->fd     = drm->fd;
    fb->width  = gbm_bo_get_width(bo);
    fb->height = gbm_bo_get_height(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    uint32_t handles[4] = {0};
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};
    uint32_t flags = 0;

    const int num_planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < num_planes; ++i) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifier;
    }

    if (modifier && modifier != DRM_FORMAT_MOD_INVALID) {
        MP_VERBOSE(ctx, "GBM surface using modifier 0x%"PRIX64"\n", modifier);
        flags = DRM_MODE_FB_MODIFIERS;
    }

    int ret = drmModeAddFB2WithModifiers(fb->fd, fb->width, fb->height,
                                         p->gbm_format,
                                         handles, strides, offsets, modifiers,
                                         &fb->id, flags);
    if (ret) {
        ret = drmModeAddFB2(fb->fd, fb->width, fb->height, p->gbm_format,
                            handles, strides, offsets, &fb->id, 0);
    }
    if (ret) {
        MP_ERR(ctx->vo, "Failed to create framebuffer: %s\n", mp_strerror(errno));
    }
    gbm_bo_set_user_data(bo, fb, framebuffer_destroy_callback);
    drm->fb = fb;
}

static void queue_flip(struct ra_ctx *ctx, struct gbm_frame *frame)
{
    struct vo_drm_state *drm = ctx->vo->drm;
    struct priv *p = ctx->priv;

    update_framebuffer_from_bo(ctx, frame->bo);

    struct drm_atomic_context *atomic_ctx = drm->atomic_context;
    int flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    drm_object_set_property(atomic_ctx->request, atomic_ctx->draw_plane, "FB_ID", drm->fb->id);
    drm_object_set_property(atomic_ctx->request, atomic_ctx->draw_plane, "CRTC_ID", atomic_ctx->crtc->id);
    drm_object_set_property(atomic_ctx->request, atomic_ctx->draw_plane, "ZPOS", 1);

    if (p->allow_modeset)
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

    const int64_t commit_time_ns = mp_time_ns();
    int ret = drmModeAtomicCommit(drm->fd, atomic_ctx->request, flags, drm);

    if (ret)
        MP_WARN(ctx->vo, "Failed to commit atomic request: %s\n", mp_strerror(ret));
    if (drm->stereo_test_enabled) {
        if (ret)
            drm->stereo_commit_errors++;
        else {
            drm->stereo_flip_eye = frame->stereo_eye;
            drm->stereo_commit_time_ns = commit_time_ns;
            drm->stereo_commit_draw_time_ns = frame->stereo_draw_time_ns;
            drm->stereo_commit_target_valid = frame->stereo_target_valid;
            drm->stereo_commit_target_msc = frame->stereo_target_msc;
        }
    }
    drm->waiting_for_flip = !ret;
    p->allow_modeset = !ret;

    drmModeAtomicFree(atomic_ctx->request);
    atomic_ctx->request = drmModeAtomicAlloc();
}

static void enqueue_bo(struct ra_ctx *ctx, struct gbm_bo *bo)
{
    struct priv *p = ctx->priv;

    struct vo_drm_state *drm = ctx->vo->drm;
    struct gbm_frame *new_frame = talloc_zero(p, struct gbm_frame);
    new_frame->bo = bo;
    new_frame->stereo_eye = drm->stereo_test_enabled ? drm->stereo_draw_eye : -1;
    if (drm->stereo_test_enabled) {
        new_frame->stereo_target_valid = drm->stereo_draw_target_valid;
        new_frame->stereo_target_msc = drm->stereo_draw_target_msc;
        new_frame->stereo_draw_time_ns = drm->stereo_draw_time_ns;
    }
    MP_TARRAY_APPEND(p, p->gbm.bo_queue, p->gbm.num_bos, new_frame);
}

static void dequeue_bo(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    talloc_free(p->gbm.bo_queue[0]);
    MP_TARRAY_REMOVE_AT(p->gbm.bo_queue, p->gbm.num_bos, 0);
}

static void swapchain_step(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    if (!(p->gbm.num_bos > 0))
        return;

    if (p->gbm.bo_queue[0]->bo)
        gbm_surface_release_buffer(p->gbm.surface, p->gbm.bo_queue[0]->bo);
    dequeue_bo(ctx);
}

// This is a recovery fast path, not a guarantee that the next atomic
// commit will reach the *next* vblank. Only discard buffers which are
// neither scanned-out (index 0) nor in flight (waiting_for_flip is false).
// Never reorder a BO behind one currently owned by KMS.
static void stereo_align_pending(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;
    if (!drm->stereo_slot_mode || !drm->stereo_have_flip ||
        drm->waiting_for_flip)
        return;

    const uint32_t expected_msc = drm->stereo_last_msc + 1u;
    const int expected_eye = drm->stereo_base_eye ^
                       ((expected_msc - drm->stereo_base_msc) & 1u);
    while (p->gbm.num_bos >= 3 &&
           p->gbm.bo_queue[1] && p->gbm.bo_queue[1]->bo &&
           p->gbm.bo_queue[2] && p->gbm.bo_queue[2]->bo &&
           p->gbm.bo_queue[1]->stereo_eye != expected_eye &&
           p->gbm.bo_queue[2]->stereo_eye == expected_eye) {
        // Drop the stale waiting frame, but retain the current scanout.
        struct gbm_frame *obsolete = p->gbm.bo_queue[1];
        gbm_surface_release_buffer(p->gbm.surface, obsolete->bo);
        talloc_free(obsolete);
        MP_TARRAY_REMOVE_AT(p->gbm.bo_queue, p->gbm.num_bos, 1);
        drm->stereo_queue_discards++;
    }
}


// A retired pair is owned exclusively by the rendering thread after a
// confirmed flip has removed the final reference to its old scanout BO.
// The worker never calls into EGL or GBM, avoiding cross-thread EGL use.
static void stereo_release_pair(struct priv *p, struct stereo_pair *pair)
{
    // sync_file FDs are independent of GBM and safe to close on the
    // original producer thread, after the worker has retired the pair.
    for (int eye = 0; eye < 2; eye++)
        if (pair->fence_fd[eye] > 0)
            close(pair->fence_fd[eye]);
    if (pair->bo[0])
        gbm_surface_release_buffer(p->gbm.surface, pair->bo[0]);
    if (pair->bo[1])
        gbm_surface_release_buffer(p->stereo.right_gbm, pair->bo[1]);
    *pair = (struct stereo_pair){0};
}

static void stereo_drain_retired(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    struct stereo_pair retired[MP_ARRAY_SIZE(s->retired)];
    unsigned num_retired;
    bool first_flip_done;

    // GBM release/unref may take locks or invoke DRM framebuffer removal.
    // Do not make the 120 Hz KMS worker wait on that potentially slow work.
    pthread_mutex_lock(&s->mutex);
    num_retired = s->num_retired;
    for (unsigned i = 0; i < num_retired; i++)
        retired[i] = s->retired[i];
    s->num_retired = 0;
    first_flip_done = s->first_flip_done;
    pthread_mutex_unlock(&s->mutex);

    for (unsigned i = 0; i < num_retired; i++)
        stereo_release_pair(p, &retired[i]);
    // The initial EGL BO is no longer scanned out after the first flip.
    // The startup queue is used by the producer thread only.
    if (first_flip_done && p->gbm.num_bos)
        swapchain_step(ctx);
}

// Export a real fence for the GL commands preceding this call. This runs
// solely on the EGL/render thread, after eglSwapBuffers of the current eye.
// A zero-timeout client wait with FLUSH_COMMANDS queues the fence without
// waiting for the GPU. Polling and closing the returned fd needs no EGL
// context on the KMS worker.
static int stereo_export_native_fence(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    EGLSyncKHR sync = s->create_sync(p->egl.display,
                                    EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) {
        MP_ERR(ctx, "Stereo native fence create failed (EGL error %#x)\n",
               eglGetError());
        return 0;
    }
    EGLint status = s->client_wait_sync(p->egl.display, sync,
                                       EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0);
    int fd = status != EGL_FALSE
        ? s->dup_native_fence_fd(p->egl.display, sync) : -1;
    EGLint egl_error = fd < 0 ? eglGetError() : EGL_SUCCESS;
    s->destroy_sync(p->egl.display, sync);
    if (fd < 0) {
        MP_ERR(ctx, "Stereo native fence export failed (EGL error %#x)\n",
               egl_error);
        return 0;
    }

    // Pair structs are zero-initialized. Never treat stdin=0 as an
    // absent descriptor or accidentally close another standard stream.
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        fd = moved;
    } else {
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0)
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    if (fd < 0) {
        MP_ERR(ctx, "Stereo native fence FD move failed: %s\n",
               mp_strerror(errno));
        return 0;
    }
    return fd;
}

// One-shot FD status probe for rare producer-side pending-discard logs.
static int stereo_debug_fence_ready(int fd)
{
    if (fd <= 0)
        return -1;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int rc = poll(&pfd, 1, 0);
    if (rc < 0 || (rc == 1 && (pfd.revents & (POLLERR | POLLNVAL))))
        return -1;
    return rc == 1 && (pfd.revents & POLLIN) ? 1 : 0;
}

// Called on the GL producer thread with stereo.mutex held, immediately
// before a pending pair that never reached KMS is discarded.
// First 40 events are queued; thereafter one in 100 to avoid log storms.
static void stereo_note_unpresented_pair(struct ra_ctx *ctx,
                                         const char *reason)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    struct vo_drm_state *drm = ctx->vo->drm;
    const struct stereo_pair *pair = &s->pending;
    uint64_t count = ++drm->stereo_pair_unpresented_discards;

    // Inexpensive counters for both normal/quiet modes: no fence polls,
    // allocations, printing, or other blocking operations. Capture the
    // readiness-history bucket and age before the pending pair is freed.
    if (strcmp(reason, "newer_generation") == 0)
        drm->stereo_pair_drop_newer++;
    else if (strcmp(reason, "gbm_pressure") == 0)
        drm->stereo_pair_drop_gbm_pressure++;
    drm->stereo_pair_drop_deferrals[MPMIN(pair->fence_deferrals, 3u)]++;
    if (pair->ready_ns > 0) {
        int64_t age_ns = mp_time_ns() - pair->ready_ns;
        if (age_ns >= 0) {
            drm->stereo_pair_drop_age_sum_ns += age_ns;
            if (!drm->stereo_pair_drop_age_samples ||
                age_ns < drm->stereo_pair_drop_age_min_ns)
                drm->stereo_pair_drop_age_min_ns = age_ns;
            drm->stereo_pair_drop_age_max_ns =
                MPMAX(drm->stereo_pair_drop_age_max_ns, age_ns);
            drm->stereo_pair_drop_age_samples++;
        }
    }
    // This function runs on the producer while stereo.mutex is held.
    // Never print from here: only publish through the trylock-only ring.
    // In quiet mode keep the exact discard count (and later frame_id_gaps)
    // but do not poll fences, format, or log individual drop records.
    // The final snapshot is emitted after the worker has been joined.
    if ((drm->opts && !drm->opts->stereo_stats) ||
        (count > 40 && count % 100 != 0))
        return;

    int64_t now_ns = mp_time_ns();
    double age_ms = pair->ready_ns > 0
        ? (now_ns - pair->ready_ns) / 1000000.0 : -1.0;
    int left = s->native_fence
        ? stereo_debug_fence_ready(pair->fence_fd[0]) : -1;
    int right = s->native_fence
        ? stereo_debug_fence_ready(pair->fence_fd[1]) : -1;
    vo_drm_stereo_log_drop(drm, &(struct drm_stereo_async_drop) {
        .reason = reason, .count = count,
        .pending_gen = pair->generation,
        .active_gen = s->active.generation,
        .frame_id = pair->frame_id,
        .media_pts = pair->media_pts,
        .pts_valid = pair->pts_valid,
        .age_ms = age_ms,
        .fence_left = left, .fence_right = right,
        .deferrals = pair->fence_deferrals,
        .first_deferral_ms = pair->fence_deferrals
            ? pair->first_fence_deferral_age_ns / 1000000.0 : -1.0,
        .last_deferral_ms = pair->fence_deferrals
            ? pair->last_fence_deferral_age_ns / 1000000.0 : -1.0,
        .last_deferral_msc = pair->last_fence_deferral_msc,
        .r_probe_age_ms = pair->intermediate_r_probe_age_ns > 0
            ? pair->intermediate_r_probe_age_ns / 1000000.0 : -1.0,
        .r_probe_msc = pair->intermediate_r_probe_msc,
        .r_probe_left = pair->intermediate_r_probe_age_ns > 0
            ? pair->intermediate_r_fence_L : -1,
        .r_probe_right = pair->intermediate_r_probe_age_ns > 0
            ? pair->intermediate_r_fence_R : -1,
    });
    // Probe every *actual* lost candidate (first 40, then 1/100).
    // The normal-priority async logger duplicates these sync_file FDs
    // before the producer releases the pair; it obtains the historical
    // GPU timestamps even when the fences are ALREADY signaled.
    // Compare right fence signal time to the last failed left-boundary
    // decision; the two ages use the same mpv monotonic timebase.
    // The RT worker never calls ioctl, EGL, blocks, or logs.
    if (s->native_fence && pair->fence_fd[0] > 0 &&
        pair->fence_fd[1] > 0) {
        vo_drm_stereo_log_gpu_probe(drm, &(struct drm_stereo_gpu_probe) {
            .dropped_pending = true,
            .generation = pair->generation,
            .frame_id = pair->frame_id,
            .fence_fd = {pair->fence_fd[0], pair->fence_fd[1]},
            .pair_published_ns = pair->ready_ns,
            .render_ns = {pair->render_ns[0], pair->render_ns[1]},
            .egl_swap_ns = {pair->egl_swap_ns[0], pair->egl_swap_ns[1]},
            .fence_export_ns = {pair->fence_export_ns[0],
                                pair->fence_export_ns[1]},
            .last_l_deferral_age_ns = pair->last_fence_deferral_age_ns,
            .last_l_deferral_target_msc = pair->last_fence_deferral_msc,
        });
    }
}

// Zero-timeout poll of both eyes on the RT worker. No EGL/GL operations
// and no waits on GPU completion are ever performed on this thread.
static bool stereo_pair_fences_signaled(struct vo_drm_state *drm,
                                        const struct stereo_pair *pair)
{
    bool ready = true;
    for (int eye = 0; eye < 2; eye++) {
        if (pair->fence_fd[eye] <= 0) {
            drm->stereo_pair_fence_errors++;
            ready = false;
            continue;
        }
        struct pollfd fd = {.fd = pair->fence_fd[eye], .events = POLLIN};
        int rc = poll(&fd, 1, 0);
        if (rc == 1 && (fd.revents & POLLIN)) {
            continue;
        } else if (rc < 0 || (rc == 1 && (fd.revents & (POLLERR | POLLNVAL)))) {
            drm->stereo_pair_fence_errors++;
        } else {
            drm->stereo_pair_fence_unready_eye[eye]++;
        }
        ready = false;
    }
    return ready;
}

// Capture the submission phase relative to the previous physical vblank.
 // The DRM timestamp clock must be validated once at setup; CLOCK_MONOTONIC
 // here is intentionally NOT mp_time_ns() (which uses a different origin).
static void stereo_stamp_submit_phase(struct vo_drm_state *drm)
{
    drm->stereo_prev_callback_lag_ns = drm->stereo_last_callback_lag_ns;
    drm->stereo_prev_callback_lag_valid = drm->stereo_callback_lag_valid;
    drm->stereo_submit_after_ust_valid = false;
    if (!drm->stereo_ust_monotonic || !drm->stereo_have_flip)
        return;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;

    int64_t cpu_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    int64_t submit_after_ust_ns =
        cpu_ns - (int64_t)drm->stereo_last_ust_us * 1000LL;
    // Preserve signed elapsed time, including cases where DRM reports
    // a timestamp *ahead of* the userspace clock. Previous code replaced
    // all negative samples with -1ns, obscuring the gap root cause.
    drm->stereo_submit_after_ust_ns = submit_after_ust_ns;
    drm->stereo_submit_after_ust_valid = true;
    drm->stereo_min_submit_after_ust_ns =
        MPMIN(drm->stereo_min_submit_after_ust_ns, submit_after_ust_ns);
    drm->stereo_max_submit_after_ust_ns =
        MPMAX(drm->stereo_max_submit_after_ust_ns, submit_after_ust_ns);
}

static void *stereo_kms_thread(void *arg)
{
    struct ra_ctx *ctx = arg;
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    struct vo_drm_state *drm = ctx->vo->drm;
    uint64_t errors = 0;
    unsigned consecutive_errors = 0;
    bool first_commit_logged = false;

    if (s->rt_mode) {
        // Only the self-blocking DRM event worker receives RT priority.
        // The decoder, renderer and audio threads keep normal scheduling.
        // No privilege escalation is attempted; permission failure falls
        // back to the normal scheduler.
        struct sched_param sp = {.sched_priority = 10};
        int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (err) {
            MP_WARN(ctx, "Stereo KMS SCHED_FIFO(10) unavailable: %s; "
                    "continuing with normal scheduling\n", mp_strerror(err));
        } else {
            MP_INFO(ctx, "Stereo KMS thread scheduling: SCHED_FIFO priority 10\n");
        }
    }

    while (true) {
        pthread_mutex_lock(&s->mutex);
        while (!s->stop && !s->active.bo[0])
            pthread_cond_wait(&s->ready, &s->mutex);
        if (s->stop) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }
        // A confirmed flip gives the last actual MSC, but the kernel's
        // vblank counter may already have advanced while this thread was
        // descheduled. The optional sequence probe allows recovery before
        // submitting the next FB_ID, instead of assuming last_msc + 1.
        uint32_t target_msc = drm->stereo_last_msc + 1u;
        bool sequence_target_valid = false;
        if (drm->stereo_have_flip && s->sequence_mode) {
            uint64_t current_seq = 0;
            uint64_t current_ns = 0;
            int rc = drmCrtcGetSequence(drm->fd, drm->crtc_id,
                                        &current_seq, &current_ns);
            if (!rc) {
                uint32_t current = (uint32_t)current_seq;
                // Do not regress if a driver returns stale sequence data.
                if ((int32_t)(current - drm->stereo_last_msc) >= 0) {
                    target_msc = current + 1u;
                    sequence_target_valid = true;
                    if (current != drm->stereo_last_msc)
                        drm->stereo_pair_sequence_catchups++;
                }
            } else {
                MP_WARN(ctx, "drmCrtcGetSequence unavailable (%d); "
                        "falling back to page-flip MSC feedback\n", rc);
                s->sequence_mode = false;
                drm->stereo_pair_sequence_mode = false;
            }
        }
        // The first successful page flip anchors L. Subsequent eyes are
        // chosen from the target physical MSC, not the mpv draw count.
        int eye = drm->stereo_have_flip
            ? drm->stereo_base_eye ^
              ((target_msc - drm->stereo_base_msc) & 1u)
            : 0;

        // The initial generation also has to be fully rendered. Normally
        // only executed once, before the first confirmed page flip.
        if (s->native_fence && !drm->stereo_have_flip) {
            drm->stereo_pair_fence_checks++;
            if (!stereo_pair_fences_signaled(drm, &s->active)) {
                drm->stereo_pair_fence_deferrals++;
                pthread_mutex_unlock(&s->mutex);
                usleep(1000);
                continue;
            }
        }
        // Diagnostic only: sample twice-deferred pending pairs at the
        // intermediate R refresh, immediately after their second failed
        // L-boundary poll. Never promote here: L/R must share a generation.
        // Only rare candidate drops incur two extra zero-timeout polls.
        if (s->native_fence && eye == 1 && s->pending.bo[0] &&
            s->pending.fence_deferrals >= 2 &&
            s->pending.intermediate_r_probe_age_ns == 0 &&
            target_msc - s->pending.last_fence_deferral_msc == 1u) {
            int64_t age_ns = s->pending.ready_ns > 0
                ? mp_time_ns() - s->pending.ready_ns : -1;
            s->pending.intermediate_r_probe_age_ns = age_ns;
            s->pending.intermediate_r_probe_msc = target_msc;
            s->pending.intermediate_r_fence_L =
                stereo_debug_fence_ready(s->pending.fence_fd[0]);
            s->pending.intermediate_r_fence_R =
                stereo_debug_fence_ready(s->pending.fence_fd[1]);
        }
        // A pair generation may be replaced only on an L boundary. Do
        // not free the old generation until the new L flip is confirmed.
        bool age_ok = s->min_pair_age_ns == 0 ||
                      (s->pending.ready_ns > 0 &&
                       mp_time_ns() - s->pending.ready_ns >=
                           s->min_pair_age_ns);
        bool eligible = eye == 0 && s->pending.bo[0] && age_ok &&
                        s->num_retired < MP_ARRAY_SIZE(s->retired);
        // mpv's requested num_vsyncs is the hold of the PREVIOUS movie
        // generation. Both eye scans for one movie frame are one logical
        // cycle. Do not advance the movie frame before the previous image
        // completed its requested number of L/R cycles. Use actual MSC of
        // its FIRST confirmed L; submitted target MSC can be inaccurate.
        // This is an opt-in diagnostic gating experiment. A late GPU frame
        // can increase A/V latency; a future media-clock anchored scheduler
        // may elect to skip it instead of accumulating such delay.
        bool plan_late = false;
        if (eligible && s->plan_hold && s->active.first_left_confirmed &&
            s->active.display_synced && s->active.planned_vsyncs > 0) {
            const uint32_t due = s->active.first_left_msc +
                2u * MPCLAMP(s->active.planned_vsyncs, 1, 4);
            if ((int32_t)(target_msc - due) < 0) {
                eligible = false;
                drm->stereo_pair_plan_early_blocks++;
            } else {
                plan_late = (int32_t)(target_msc - due) > 0;
            }
        } else if (eligible && s->plan_hold && !s->active.first_left_confirmed) {
            drm->stereo_pair_plan_anchor_missing++;
        }
        if (eligible && s->queue_test_hold_ms &&
            s->queued.bo[0] &&
            mp_time_ns() < s->queue_test_hold_until_ns) {
            eligible = false;
            s->queue_test_hold_blocks++;
        }
        bool fences_ready = true;
        if (eligible && s->native_fence) {
            drm->stereo_pair_fence_checks++;
            fences_ready = stereo_pair_fences_signaled(drm, &s->pending);
            if (!fences_ready) {
                drm->stereo_pair_fence_deferrals++;
                // Record the ages at the *actual* left-boundary misses,
                // not just the fence state at later producer replacement.
                // Only simple scalar bookkeeping runs on the RT worker.
                int64_t age_ns = s->pending.ready_ns > 0
                    ? mp_time_ns() - s->pending.ready_ns : -1;
                if (s->pending.fence_deferrals++ == 0)
                    s->pending.first_fence_deferral_age_ns = age_ns;
                s->pending.last_fence_deferral_age_ns = age_ns;
                s->pending.last_fence_deferral_msc = target_msc;
            }
        }
        bool promote = eligible && fences_ready;
        struct stereo_pair *selected = promote ? &s->pending : &s->active;
        struct framebuffer *fb = gbm_bo_get_user_data(selected->bo[eye]);
        if (!drm->active || !fb || !fb->id) {
            pthread_mutex_unlock(&s->mutex);
            usleep(3000);
            continue;
        }

        int ret = -1;
        int failed_errno = ENOMEM;
        int fb_property = -1;
        int crtc_property = -1;
        int64_t submit_ns = 0;
        bool legacy_submitted = false;

        if (s->legacy_mode) {
            // Control experiment: legacy page-flip on the CRTC's primary
            // plane, using exactly the same fully-rendered GBM framebuffer.
            // Mode setup and restoration remain atomic. The regular
            // page-flip DRM event is consumed by the existing worker.
            stereo_stamp_submit_phase(drm);
            submit_ns = mp_time_ns();
            ret = drmModePageFlip(drm->fd, drm->crtc_id, fb->id,
                                  DRM_MODE_PAGE_FLIP_EVENT, drm);
            int flip_errno = errno;
            int64_t ioctl_ns = mp_time_ns() - submit_ns;
            drm->stereo_commit_ioctl_ns = ioctl_ns;
            drm->stereo_max_commit_ioctl_ns =
                MPMAX(drm->stereo_max_commit_ioctl_ns, ioctl_ns);
            failed_errno = ret ? flip_errno : 0;
            if (!ret) {
                legacy_submitted = true;
                drm->stereo_pair_legacy_flips++;
            } else if (flip_errno == EINVAL || flip_errno == ENOTTY ||
                       flip_errno == EOPNOTSUPP || flip_errno == ENOSYS) {
                // An unsupported legacy interface should never strand a
                // working atomic scanout. Rejected flips queue no event.
                MP_WARN(ctx, "Stereo legacy CRTC flip unsupported (%s), "
                        "falling back to atomic scanout\n",
                        mp_strerror(flip_errno));
                s->legacy_mode = false;
                drm->stereo_pair_legacy_mode = false;
                drm->stereo_pair_legacy_fallbacks++;
            }
        }

        // Default path, or automatic fallback when the legacy interface
        // is unavailable. Do not resubmit after a successful page-flip.
        if (!s->legacy_mode && !legacy_submitted) {
            drmModeAtomicReqPtr req = drmModeAtomicAlloc();
            ret = -1;
            failed_errno = ENOMEM;
            if (req) {
                struct drm_atomic_context *atomic = drm->atomic_context;
                fb_property = drm_object_set_property(req, atomic->draw_plane,
                                                      "FB_ID", fb->id);
                crtc_property = drm_object_set_property(req, atomic->draw_plane,
                                                        "CRTC_ID", atomic->crtc->id);
                // ZPOS is optional, and is often immutable or missing on AMD.
                if (fb_property >= 0 && crtc_property >= 0) {
                    drm_object_set_property(req, atomic->draw_plane, "ZPOS", 1);
                    stereo_stamp_submit_phase(drm);
                    submit_ns = mp_time_ns();
                    ret = drmModeAtomicCommit(drm->fd, req,
                                DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                                drm);
                    int commit_errno = errno;
                    int64_t ioctl_ns = mp_time_ns() - submit_ns;
                    drm->stereo_commit_ioctl_ns = ioctl_ns;
                    drm->stereo_max_commit_ioctl_ns =
                        MPMAX(drm->stereo_max_commit_ioctl_ns, ioctl_ns);
                    failed_errno = ret ? commit_errno : 0;
                } else {
                    failed_errno = EINVAL;
                }
                drmModeAtomicFree(req);
            }
        }
        struct stereo_pair old = {0};
        if (!ret) {
            consecutive_errors = 0;
            if (!first_commit_logged) {
                MP_INFO(ctx, "Stereo KMS first %s flip submitted: "
                        "eye=%c fb_id=%u generation=%llu\n",
                        legacy_submitted ? "legacy" : "atomic",
                        eye ? 'R' : 'L', fb->id,
                        (unsigned long long)selected->generation);
                first_commit_logged = true;
            }
            if (promote) {
                if (s->native_fence)
                    drm->stereo_pair_fence_promotions++;
                if (s->plan_hold) {
                    drm->stereo_pair_plan_due_promotions++;
                    if (plan_late)
                        drm->stereo_pair_plan_late_promotions++;
                }
                old = s->active;
                s->active = s->pending;
                // Dequeue only after a successful L flip submission;
                // old active BO is released after its KMS confirmation.
                if (s->two_slot_queue && s->queued.bo[0]) {
                    s->pending = s->queued;
                    s->queued = (struct stereo_pair){0};
                    s->queue_second_promotions++;
                } else {
                    s->pending = (struct stereo_pair){0};
                }
            }
            drm->stereo_flip_eye = eye;
            drm->stereo_commit_target_valid = sequence_target_valid;
            drm->stereo_commit_target_msc = target_msc;
            drm->stereo_commit_time_ns = submit_ns;
            drm->stereo_commit_after_callback_ns =
                drm->stereo_last_callback_cpu_ns > 0
                    ? MPMAX(submit_ns - drm->stereo_last_callback_cpu_ns, 0)
                    : 0;
            drm->stereo_max_submit_after_callback_ns =
                MPMAX(drm->stereo_max_submit_after_callback_ns,
                      drm->stereo_commit_after_callback_ns);
            drm->stereo_commit_draw_time_ns = 0;
            drm->stereo_pair_display_generation = s->active.generation;
            drm->stereo_submit_frame_id = s->active.frame_id;
            drm->stereo_submit_media_pts = s->active.media_pts;
            drm->stereo_submit_pts_valid = s->active.pts_valid;
            drm->stereo_submit_planned_vsyncs = s->active.planned_vsyncs;
            drm->stereo_submit_display_synced = s->active.display_synced;
            drm->stereo_pair_first_eye_in_generation =
                drm->stereo_pair_eye_last_submitted_generation[eye] !=
                s->active.generation;
            if (drm->stereo_pair_first_eye_in_generation)
                drm->stereo_pair_eye_first_flips[eye]++;
            drm->stereo_pair_eye_last_submitted_generation[eye] =
                s->active.generation;
            drm->stereo_pair_age_at_submit_ns =
                s->active.ready_ns > 0 && submit_ns >= s->active.ready_ns
                    ? submit_ns - s->active.ready_ns
                    : -1;
            if (drm->stereo_pair_first_eye_in_generation &&
                drm->stereo_pair_age_at_submit_ns >= 0) {
                int64_t age_ns = drm->stereo_pair_age_at_submit_ns;
                uint64_t count = drm->stereo_pair_eye_first_flips[eye];
                drm->stereo_pair_first_age_sum_ns[eye] += age_ns;
                if (count == 1 || age_ns < drm->stereo_pair_first_age_min_ns[eye])
                    drm->stereo_pair_first_age_min_ns[eye] = age_ns;
                drm->stereo_pair_first_age_max_ns[eye] =
                    MPMAX(drm->stereo_pair_first_age_max_ns[eye], age_ns);
            }
            if (!promote)
                drm->stereo_pair_repeated_flips++;
            drm->waiting_for_flip = true;
        } else {
            drm->stereo_commit_errors++;
            errors++;
            consecutive_errors++;
            if (errors <= 5 || errors % 120 == 0)
                MP_WARN(ctx, "Stereo KMS flip failed: fb_prop=%d "
                        "crtc_prop=%d commit_rc=%d errno=%d (%s)\n",
                        fb_property, crtc_property, ret,
                        failed_errno, mp_strerror(failed_errno));
            if (consecutive_errors >= 60) {
                MP_ERR(ctx, "Stereo KMS stopped after 60 consecutive "
                        "flip failures (video scanout unavailable)\n");
                s->stop = true;
            }
        }
        pthread_mutex_unlock(&s->mutex);

        if (ret) {
            if (consecutive_errors >= 60)
                break;
            usleep(3000);
            continue;
        }

        // Only this thread drains DRM page-flip events in paired mode.
        // The callback records the true MSC and updates the next phase.
        // During shutdown, give an in-flight page flip a bounded chance
        // to finish before the caller restores the previous CRTC mode.
        bool complete = false;
        unsigned shutdown_polls = 0;
        while (!complete) {
            struct pollfd fd = {.fd = drm->fd, .events = POLLIN};
            int n = poll(&fd, 1, 50);
            pthread_mutex_lock(&s->mutex);
            if (n > 0 && (fd.revents & POLLIN)) {
                if (drmHandleEvent(drm->fd, &drm->ev) < 0) {
                    MP_ERR(ctx, "Stereo DRM event processing failed\n");
                    s->stop = true;
                } else if (!drm->waiting_for_flip) {
                    // The scanout callback has confirmed the hardware MSC.
                    // Never anchor a requested hold to drmModeAtomicCommit.
                    if (drm->stereo_flip_eye == 0 &&
                        drm->stereo_pair_first_eye_in_generation &&
                        drm->stereo_pair_display_generation ==
                            s->active.generation) {
                        s->active.first_left_confirmed = true;
                        s->active.first_left_msc = drm->stereo_last_msc;
                    }
                    // Physical KMS scanout remains independent at 120Hz.
                    // The VO advances once per full L/R cycle. By default
                    // pace it on L, but allow an opt-in R-edge experiment to
                    // shift the producer phase without changing eye parity.
                    // VO wakeup may be an EARLIER edge than mpv's
                    // presentation feedback, which remains unchanged in
                    // drm_pflip_cb. Do not modify physical eye parity.
                    if (!drm->opts || !drm->opts->stereo_3d ||
                        drm->stereo_flip_eye == s->render_wake_eye) {
                        s->confirmed_flips = drm->stereo_flips;
                        drm->stereo_pair_render_wakes++;
                        pthread_cond_broadcast(&s->flip_ready);
                    }
                }
            } else if (n < 0 && errno != EINTR) {
                MP_ERR(ctx, "Stereo DRM poll failed: %s\n",
                       mp_strerror(errno));
                s->stop = true;
            }
            if (s->stop && drm->waiting_for_flip && ++shutdown_polls >= 20) {
                MP_WARN(ctx, "Timed out waiting for final stereo KMS flip on shutdown\n");
                complete = true;
            } else {
                complete = !drm->waiting_for_flip;
            }
            if (complete && old.bo[0]) {
                // If stopping on a timeout, uninit will first restore the
                // old CRTC state before any retired BO is released.
                if (s->num_retired < MP_ARRAY_SIZE(s->retired))
                    s->retired[s->num_retired++] = old;
                old = (struct stereo_pair){0};
            }
            if (complete && !s->stop)
                s->first_flip_done = true;
            pthread_mutex_unlock(&s->mutex);
        }
    }
    return NULL;
}

// Called once by the mpv VO thread for each display-synced flip_page.
// A standalone KMS worker continues alternating L/R every vblank, but
// mpv's own frame scheduler MUST still be paced by those hardware events.
// Otherwise the core assumes implicit blocking was performed by the VO
// and advances the 23.976 fps movie as fast as decoding allows.
void vo_drm_stereo_pair_wait_vsync(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled || !s->initialized)
        return;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100000000; // 100 ms: never hang mpv on a lost KMS event
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
        deadline.tv_sec++;
    }

    pthread_mutex_lock(&s->mutex);
    while (!s->stop && s->confirmed_flips <= s->vo_seen_flips) {
        int ret = pthread_cond_timedwait(&s->flip_ready, &s->mutex, &deadline);
        if (ret == ETIMEDOUT)
            break;
        if (ret != 0) {
            MP_ERR(ctx, "Stereo VO vblank wait failed: %d\n", ret);
            break;
        }
    }
    if (s->confirmed_flips > s->vo_seen_flips)
        s->vo_seen_flips = s->confirmed_flips;
    pthread_mutex_unlock(&s->mutex);
}

// Invoked by mpv on the producer/VO thread when its video queue is reset
// after seek (and on startup/reconfiguration). Both unpublished slots must
// be flushed before the KMS worker can consider another L promotion.
// Do not touch active or in-flight scanout BOs. GBM release must run
// outside stereo.mutex so the 120Hz worker cannot be delayed by Mesa.
void vo_drm_stereo_pair_reset(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled || !s->initialized)
        return;

    struct stereo_pair pending = {0}, queued = {0}, building = {0};
    pthread_mutex_lock(&s->mutex);
    s->queue_seek_resets++;
    pending = s->pending;
    queued = s->queued;
    building = s->building;
    s->pending = (struct stereo_pair){0};
    s->queued = (struct stereo_pair){0};
    s->building = (struct stereo_pair){0};
    // The optional occupied-FIFO test must not stall the next movie
    // generation after a seek. Keep queue_test_triggered one-shot.
    s->queue_test_hold_until_ns = 0;
    s->queue_seek_pending_flushes += !!pending.bo[0];
    s->queue_seek_queued_flushes += !!queued.bo[0];
    s->queue_seek_building_flushes += !!building.bo[0];
    pthread_mutex_unlock(&s->mutex);

    if (pending.bo[0] || queued.bo[0] || building.bo[0])
        MP_INFO(ctx, "Stereo queue seek reset: pending_frame_id=%llu "
                "queued_frame_id=%llu building_frame_id=%llu "
                "(active and in-flight scanout preserved)\n",
                (unsigned long long)pending.frame_id,
                (unsigned long long)queued.frame_id,
                (unsigned long long)building.frame_id);

    stereo_release_pair(p, &pending);
    stereo_release_pair(p, &queued);
    stereo_release_pair(p, &building);
    stereo_drain_retired(ctx);
}

// These helpers are only invoked on the original VO/EGL thread.
bool vo_drm_stereo_pair_can_capture(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled || !s->initialized)
        return false;
    stereo_drain_retired(ctx);
    // Bounded two-slot FIFO: do not destroy an accepted frame to
    // create GBM room for a later picture. Rendering skips admission
    // if both slots are taken, rather than stalling the 120 Hz worker.
    if (s->two_slot_queue) {
        pthread_mutex_lock(&s->mutex);
        bool full = s->pending.bo[0] && s->queued.bo[0];
        if (full) {
            s->queue_full_skips++;
            ctx->vo->drm->stereo_pair_update_skips++;
        }
        pthread_mutex_unlock(&s->mutex);
        if (full)
            return false;
    }
    bool available = gbm_surface_has_free_buffers(p->gbm.surface) &&
                     gbm_surface_has_free_buffers(s->right_gbm);
    if (!available) {
        pthread_mutex_lock(&s->mutex);
        if (s->two_slot_queue) {
            // Skipped incoming frame, without evicting pending or queued.
            s->queue_gbm_skips++;
            ctx->vo->drm->stereo_pair_update_skips++;
            pthread_mutex_unlock(&s->mutex);
            return false;
        }
        // A prior partial capture is never eligible for scanout.
        if (s->building.bo[0] || s->building.bo[1])
            stereo_release_pair(p, &s->building);
        // If necessary, drop an unpublished pending generation to free
        // buffers. Its BOs are not owned by KMS until the worker moves
        // the pair to active (under this same mutex).
        if ((!gbm_surface_has_free_buffers(p->gbm.surface) ||
             !gbm_surface_has_free_buffers(s->right_gbm)) &&
            s->pending.bo[0]) {
            stereo_note_unpresented_pair(ctx, "gbm_pressure");
            stereo_release_pair(p, &s->pending);
        }
        available = gbm_surface_has_free_buffers(p->gbm.surface) &&
                    gbm_surface_has_free_buffers(s->right_gbm);
        if (!available)
            ctx->vo->drm->stereo_pair_update_skips++;
        pthread_mutex_unlock(&s->mutex);
    }
    return available;
}

// Called from vo_gpu once per NEW movie frame, on the GL producer thread
// BEFORE either eye is rendered. The metadata travels in building -> pending
// -> active alongside the BOs; no extra release ordering or PTS queue exists.
void vo_drm_stereo_pair_set_frame_info(struct ra_ctx *ctx,
                                       uint64_t frame_id, double media_pts,
                                       bool pts_valid, int planned_vsyncs,
                                       bool display_synced)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled || !s->initialized)
        return;
    pthread_mutex_lock(&s->mutex);
    s->building.frame_id = frame_id;
    s->building.media_pts = media_pts;
    s->building.pts_valid = pts_valid;
    s->building.planned_vsyncs = planned_vsyncs;
    s->building.display_synced = display_synced;
    pthread_mutex_unlock(&s->mutex);
}

void vo_drm_stereo_pair_note_eye_render(struct ra_ctx *ctx, int eye,
                                         int64_t render_ns)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled || !s->initialized || eye < 0 || eye > 1)
        return;
    // The producer owns building, but keep metadata ordering explicit.
    pthread_mutex_lock(&s->mutex);
    s->building.render_ns[eye] = render_ns;
    pthread_mutex_unlock(&s->mutex);
}

bool vo_drm_stereo_pair_begin_eye(struct ra_ctx *ctx, int eye)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled)
        return false;
    if (eye == -1) {
        s->capture_eye = -1;
        return true;
    }
    if (eye != 0 && eye != 1)
        return false;
    EGLSurface surface = eye ? s->right_egl : p->egl.surface;
    if (!eglMakeCurrent(p->egl.display, surface, surface, p->egl.context)) {
        MP_ERR(ctx, "Cannot bind stereo eye %d EGL surface\n", eye);
        return false;
    }
    if (eye == 1 && s->right_swap_interval_zero &&
        !s->right_swap_interval_configured) {
        // EGL 1.5: eglSwapInterval affects only the CURRENT draw surface.
        // Our existing eglSwapInterval(display, 0) runs with LEFT current;
        // the newly created RIGHT surface otherwise retains default 1.
        // This is GL producer-only, performed once before the first R draw.
        EGLint min_interval = -1, max_interval = -1;
        bool min_known = eglGetConfigAttrib(p->egl.display, p->egl.config,
                                             EGL_MIN_SWAP_INTERVAL, &min_interval);
        bool max_known = eglGetConfigAttrib(p->egl.display, p->egl.config,
                                             EGL_MAX_SWAP_INTERVAL, &max_interval);
        if (!eglSwapInterval(p->egl.display, 0)) {
            MP_ERR(ctx, "Stereo right EGL swap interval 0 failed: "
                   "EGL error %#x\n", eglGetError());
            return false;
        }
        s->right_swap_interval_configured = true;
        if (min_known && min_interval > 0) {
            MP_WARN(ctx, "Stereo right eglSwapInterval(0) accepted but "
                    "CLAMPED by EGLConfig min=%d max=%d; "
                    "not an effective zero-interval test\n",
                    min_interval, max_known ? max_interval : -1);
        } else {
            MP_INFO(ctx, "Stereo right EGLSurface swap interval 0 requested; "
                    "EGLConfig min=%d max=%d\n",
                    min_known ? min_interval : -1,
                    max_known ? max_interval : -1);
        }
    }
    s->capture_eye = eye;
    return true;
}

static void drm_egl_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;

    if (p->stereo.enabled) {
        struct stereo_kms *s = &p->stereo;
        const int eye = s->capture_eye;
        if (eye != 0 && eye != 1)
            return;
        const int64_t egl_swap_start_ns = mp_time_ns();
        struct gbm_surface *surface = eye ? s->right_gbm : p->gbm.surface;
        EGLSurface egl_surface = eye ? s->right_egl : p->egl.surface;
        if (!eglSwapBuffers(p->egl.display, egl_surface)) {
            MP_ERR(ctx, "EGL stereo eye %d swap failed\n", eye);
            return;
        }
        const int64_t egl_swap_duration_ns = mp_time_ns() - egl_swap_start_ns;
        int fence_fd = 0;
        const int64_t fence_export_start_ns = mp_time_ns();
        if (s->native_fence) {
            fence_fd = stereo_export_native_fence(ctx);
            if (!fence_fd) {
                // Never hand an unverified new buffer to KMS in this mode.
                pthread_mutex_lock(&s->mutex);
                drm->stereo_pair_fence_errors++;
                pthread_mutex_unlock(&s->mutex);
                return;
            }
        }
        const int64_t fence_export_duration_ns =
            mp_time_ns() - fence_export_start_ns;
        if (eye == 1 && s->gpu_wait) {
            // EGL swap queues GPU work, but the BO can still be busy when
            // atomic KMS tries to scan it out. Experimentally wait for
            // both eyes to finish before making the complete pair public.
            // This executes on the 24 fps GL producer thread, never the
            // 120 Hz independent KMS worker.
            int64_t begin_ns = mp_time_ns();
            if (!eglWaitClient()) {
                MP_ERR(ctx, "Stereo GPU completion wait failed: EGL error %#x\n",
                       eglGetError());
                return;
            }
            int64_t waited_ns = mp_time_ns() - begin_ns;
            pthread_mutex_lock(&s->mutex);
            drm->stereo_pair_gpu_waits++;
            drm->stereo_max_gpu_wait_ns =
                MPMAX(drm->stereo_max_gpu_wait_ns, waited_ns);
            pthread_mutex_unlock(&s->mutex);
        }
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(surface);
        if (!bo) {
            if (fence_fd)
                close(fence_fd);
            MP_ERR(ctx, "Could not lock stereo eye %d GBM front buffer\n", eye);
            return;
        }
        // Create the FB on the EGL thread, so the KMS worker only needs
        // immutable framebuffer IDs and does not call GBM or EGL.
        update_framebuffer_from_bo(ctx, bo);
        struct framebuffer *fb = gbm_bo_get_user_data(bo);
        if (!fb || !fb->id) {
            if (fence_fd)
                close(fence_fd);
            gbm_surface_release_buffer(surface, bo);
            return;
        }

        // The old unpublished staging pair, if superseded, is detached
        // under mutex but its GBM buffers are released on the producer
        // AFTER unlocking. Never hold up 120Hz KMS on GBM teardown.
        struct stereo_pair overwritten = {0};
        bool queue_test_occupied = false;
        uint64_t queue_test_pending_id = 0, queue_test_queued_id = 0;
        pthread_mutex_lock(&s->mutex);
        if (s->building.bo[eye]) {
            if (s->building.fence_fd[eye] > 0)
                close(s->building.fence_fd[eye]);
            gbm_surface_release_buffer(surface, s->building.bo[eye]);
        }
        s->building.bo[eye] = bo;
        s->building.fence_fd[eye] = fence_fd;
        s->building.egl_swap_ns[eye] = egl_swap_duration_ns;
        s->building.fence_export_ns[eye] =
            fence_export_duration_ns;
        if (eye == 1 && s->building.bo[0] && s->building.bo[1]) {
            s->building.generation = ++s->next_generation;
            s->building.ready_ns = mp_time_ns();
            // Sample every 16 full pairs. The queue owns duplicate fence
            // descriptors; their completion is measured away from the
            // 120Hz SCHED_FIFO KMS thread. No sampling in quiet mode.
            if (s->native_fence && drm->opts && drm->opts->stereo_stats &&
                (s->building.generation % 16 == 0)) {
                vo_drm_stereo_log_gpu_probe(drm, &(struct drm_stereo_gpu_probe) {
                    .generation = s->building.generation,
                    .frame_id = s->building.frame_id,
                    .fence_fd = {s->building.fence_fd[0],
                                 s->building.fence_fd[1]},
                    .pair_published_ns = s->building.ready_ns,
                    .render_ns = {s->building.render_ns[0],
                                   s->building.render_ns[1]},
                    .egl_swap_ns = {s->building.egl_swap_ns[0],
                                    s->building.egl_swap_ns[1]},
                    .fence_export_ns = {s->building.fence_export_ns[0],
                                        s->building.fence_export_ns[1]},
                });
            }
            if (s->freeze_pair && s->active.bo[0]) {
                // A/B isolation test: preserve producer-side GPU load
                // (decode + rendering of BOTH stereo eyes), but never
                // give a newly rendered GBM BO to KMS. Only the initial
                // pair is scanned out, exactly as during pause. This
                // control distinguishes first BO presentation/fences
                // from generic contention caused by video rendering.
                stereo_release_pair(p, &s->building);
                s->building = (struct stereo_pair){0};
                drm->stereo_pair_rendered++;
                pthread_mutex_unlock(&s->mutex);
                return;
            }
            if (!s->active.bo[0]) {
                s->active = s->building;
            } else if (s->two_slot_queue) {
                if (!s->pending.bo[0]) {
                    s->pending = s->building;
                } else if (!s->queued.bo[0]) {
                    // The old pending pair becomes GPU ready soon after a
                    // missed L boundary; retain it until the next L edge.
                    s->queued = s->building;
                    s->queue_second_enqueues++;
                    if (s->queue_test_hold_ms && !s->queue_test_triggered) {
                        // Armed exactly once per mpv lifetime. This state
                        // transition and the deadline are committed under
                        // the SAME mutex the KMS thread uses.
                        s->queue_test_triggered = true;
                        s->queue_test_hold_until_ns = mp_time_ns() +
                            (int64_t)s->queue_test_hold_ms * 1000000;
                        queue_test_pending_id = s->pending.frame_id;
                        queue_test_queued_id = s->queued.frame_id;
                        queue_test_occupied = true;
                    }
                } else {
                    // Should be unreachable: can_capture() refuses a
                    // full queue. Never overwrite accepted generations.
                    s->queue_unexpected_incoming++;
                    overwritten = s->building;
                }
            } else {
                // Superseded staging pairs were never submitted to KMS.
                // Do not release active or in-flight BOs here.
                if (s->pending.bo[0]) {
                    // Optional isolation test for the single pending slot:
                    // retain an already-GPU-ready older generation until
                    // the next eligible L rather than discard it merely
                    // because the producer rendered a newer movie frame.
                    // This trades one dropped incoming movie picture for
                    // avoiding replacement of the older ready picture;
                    // it MUST be evaluated against A/V latency and frame
                    // pacing before becoming the default.
                    bool protect = s->keep_ready_pending && s->native_fence &&
                        stereo_debug_fence_ready(s->pending.fence_fd[0]) == 1 &&
                        stereo_debug_fence_ready(s->pending.fence_fd[1]) == 1;
                    if (protect) {
                        drm->stereo_pair_kept_ready_pending++;
                        drm->stereo_pair_dropped_newer_incoming++;
                        overwritten = s->building;
                        s->building = (struct stereo_pair){0};
                        drm->stereo_pair_rendered++;
                        pthread_mutex_unlock(&s->mutex);
                        stereo_release_pair(p, &overwritten);
                        return;
                    }
                    stereo_note_unpresented_pair(ctx, "newer_generation");
                    overwritten = s->pending;
                    s->pending = (struct stereo_pair){0};
                }
                s->pending = s->building;
            }
            s->building = (struct stereo_pair){0};
            drm->stereo_pair_rendered++;
            if (!s->started) {
                s->started = pthread_create(&s->thread, NULL,
                                             stereo_kms_thread, ctx) == 0;
                if (!s->started)
                    MP_ERR(ctx, "Failed to start independent stereo KMS thread\n");
            }
            pthread_cond_signal(&s->ready);
        }
        pthread_mutex_unlock(&s->mutex);
        if (queue_test_occupied) {
            // This one producer-thread log (never the RT KMS thread)
            // allows an external JSON IPC stress harness to seek while
            // both unpublished generations are deliberately retained.
            MP_INFO(ctx, "Stereo queue test occupied: pending_frame_id=%llu "
                    "queued_frame_id=%llu hold_ms=%d\n",
                    (unsigned long long)queue_test_pending_id,
                    (unsigned long long)queue_test_queued_id,
                    s->queue_test_hold_ms);
        }
        if (overwritten.bo[0])
            stereo_release_pair(p, &overwritten);
        return;
    }

    if (!drm->atomic_context->request) {
        drm->atomic_context->request = drmModeAtomicAlloc();
        p->drm_params.atomic_request_ptr = &drm->atomic_context->request;
    }

    if (!drm->active)
        return;

    eglSwapBuffers(p->egl.display, p->egl.surface);

    struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(p->gbm.surface);
    if (!new_bo) {
        MP_ERR(ctx->vo, "Couldn't lock front buffer\n");
        return;
    }
    enqueue_bo(ctx, new_bo);

    // Keep the original GBM pipelined queue pressure rules. In
    // particular, do NOT force a flip and immediately wait for its
    // callback on every draw: that serializes GPU rendering with KMS,
    // defeating --swapchain-depth and cutting throughput to ~60 Hz.
    while (drm->redraw ||
           p->gbm.num_bos > ctx->vo->opts->swapchain_depth ||
           !gbm_surface_has_free_buffers(p->gbm.surface)) {
        if (drm->waiting_for_flip) {
            vo_drm_wait_on_flip(drm);
            swapchain_step(ctx);
        }
        if (p->gbm.num_bos <= 1)
            break;
        if (!p->gbm.bo_queue[1] || !p->gbm.bo_queue[1]->bo) {
            MP_ERR(ctx->vo, "Hole in swapchain?\n");
            swapchain_step(ctx);
            continue;
        }
        stereo_align_pending(ctx);
        if (p->gbm.num_bos > 1)
            queue_flip(ctx, p->gbm.bo_queue[1]);
    }
    // Keep the real GBM queue length (including any in-flight BO) as
    // input to the future-MSC estimator; consuming the current flip
    // here would collapse the render/scanout pipeline.
    if (drm->stereo_test_enabled) {
        drm->stereo_queue_ahead = p->gbm.num_bos > 0 ? p->gbm.num_bos - 1 : 0;
        drm->stereo_queue_peak = MPMAX(drm->stereo_queue_peak,
                                       drm->stereo_queue_ahead);
    }
    drm->redraw = false;
}

static void stereo_usb_stop(struct ra_ctx *ctx);

static void drm_egl_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (p && p->stereo.initialized) {
        struct stereo_kms *s = &p->stereo;
        pthread_mutex_lock(&s->mutex);
        s->stop = true;
        pthread_cond_broadcast(&s->ready);
        pthread_cond_broadcast(&s->flip_ready);
        pthread_mutex_unlock(&s->mutex);
        if (s->started)
            pthread_join(s->thread, NULL);
        stereo_usb_stop(ctx);

        // Print one final snapshot after the KMS worker has stopped, never
        // in the 120Hz page-flip callback. In particular, this preserves
        // accurate totals with --drm-stereo-stats=no and does not perturb
        // the realtime scanout schedule. Keep the snapshot before DRM is
        // torn down so all counters are still accessible.
        struct vo_drm_state *drm = ctx->vo->drm;
        if (drm && drm->stereo_test_enabled) {
            MP_INFO(ctx, "Stereo DRM final: flips=%llu "
                    "missed_vblanks=%llu parity_errors=%llu "
                    "wrong_eye=%llu wrong_hold=%llu "
                    "pair_renders=%llu pair_gen=%llu "
                    "pair_unpresented_discards=%llu "
                    "first_eye_L=%llu first_eye_R=%llu "
                    "first_eye_L_misses=%llu first_eye_R_misses=%llu "
                    "fence_checks=%llu fence_promotions=%llu "
                    "fence_deferrals=%llu fence_errors=%llu "
                    "pair_update_skips=%llu commit_errors=%llu "
                    "(scanout telemetry; USB reported separately when enabled)\n",
                    (unsigned long long)drm->stereo_flips,
                    (unsigned long long)drm->stereo_missed_slots,
                    (unsigned long long)drm->stereo_parity_errors,
                    (unsigned long long)drm->stereo_absolute_eye_errors,
                    (unsigned long long)drm->stereo_wrong_hold_slots,
                    (unsigned long long)drm->stereo_pair_rendered,
                    (unsigned long long)drm->stereo_pair_display_generation,
                    (unsigned long long)drm->stereo_pair_unpresented_discards,
                    (unsigned long long)drm->stereo_pair_eye_first_flips[0],
                    (unsigned long long)drm->stereo_pair_eye_first_flips[1],
                    (unsigned long long)drm->stereo_pair_eye_first_misses[0],
                    (unsigned long long)drm->stereo_pair_eye_first_misses[1],
                    (unsigned long long)drm->stereo_pair_fence_checks,
                    (unsigned long long)drm->stereo_pair_fence_promotions,
                    (unsigned long long)drm->stereo_pair_fence_deferrals,
                    (unsigned long long)drm->stereo_pair_fence_errors,
                    (unsigned long long)drm->stereo_pair_update_skips,
                    (unsigned long long)drm->stereo_commit_errors);

            if (drm->stereo_pair_mode) {
                MP_INFO(ctx, "Stereo staging final: "
                        "wake_eye=%c render_wakes=%llu "
                        "protect_ready_pending=%d protected=%llu "
                        "dropped_incoming=%llu\n",
                        s->render_wake_eye ? 'R' : 'L',
                        (unsigned long long)drm->stereo_pair_render_wakes,
                        s->keep_ready_pending,
                        (unsigned long long)drm->stereo_pair_kept_ready_pending,
                        (unsigned long long)drm->stereo_pair_dropped_newer_incoming);
                MP_INFO(ctx, "Stereo two-slot queue final: "
                        "enabled=%d second_enqueues=%llu "
                        "second_promotions=%llu full_skips=%llu "
                        "gbm_skips=%llu unexpected_incoming=%llu "
                        "remaining_pending=%d remaining_queued=%d\n",
                        s->two_slot_queue,
                        (unsigned long long)s->queue_second_enqueues,
                        (unsigned long long)s->queue_second_promotions,
                        (unsigned long long)s->queue_full_skips,
                        (unsigned long long)s->queue_gbm_skips,
                        (unsigned long long)s->queue_unexpected_incoming,
                        s->pending.bo[0] != NULL, s->queued.bo[0] != NULL);
                MP_INFO(ctx, "Stereo queue seek reset final: "
                        "resets=%llu pending_flushes=%llu "
                        "queued_flushes=%llu building_flushes=%llu\n",
                        (unsigned long long)s->queue_seek_resets,
                        (unsigned long long)s->queue_seek_pending_flushes,
                        (unsigned long long)s->queue_seek_queued_flushes,
                        (unsigned long long)s->queue_seek_building_flushes);
                MP_INFO(ctx, "Stereo queue test final: hold_ms=%d "
                        "triggered=%d hold_blocks=%llu\n",
                        s->queue_test_hold_ms, s->queue_test_triggered,
                        (unsigned long long)s->queue_test_hold_blocks);
                MP_INFO(ctx, "Stereo plan hold final: enabled=%d "
                        "early_blocks=%llu due_promotions=%llu "
                        "late_promotions=%llu anchor_missing=%llu\n",
                        drm->stereo_pair_plan_hold,
                        (unsigned long long)drm->stereo_pair_plan_early_blocks,
                        (unsigned long long)drm->stereo_pair_plan_due_promotions,
                        (unsigned long long)drm->stereo_pair_plan_late_promotions,
                        (unsigned long long)drm->stereo_pair_plan_anchor_missing);
                double drop_age_avg_ms = drm->stereo_pair_drop_age_samples
                    ? drm->stereo_pair_drop_age_sum_ns /
                      (1000000.0 * drm->stereo_pair_drop_age_samples) : 0.0;
                MP_INFO(ctx, "Stereo DRM drops final: "
                        "newer_generation=%llu gbm_pressure=%llu "
                        "deferrals_0=%llu deferrals_1=%llu "
                        "deferrals_2=%llu deferrals_3plus=%llu "
                        "age_samples=%llu age_min_ms=%.3f "
                        "age_avg_ms=%.3f age_max_ms=%.3f\n",
                        (unsigned long long)drm->stereo_pair_drop_newer,
                        (unsigned long long)drm->stereo_pair_drop_gbm_pressure,
                        (unsigned long long)drm->stereo_pair_drop_deferrals[0],
                        (unsigned long long)drm->stereo_pair_drop_deferrals[1],
                        (unsigned long long)drm->stereo_pair_drop_deferrals[2],
                        (unsigned long long)drm->stereo_pair_drop_deferrals[3],
                        (unsigned long long)drm->stereo_pair_drop_age_samples,
                        drm->stereo_pair_drop_age_min_ns / 1000000.0,
                        drop_age_avg_ms,
                        drm->stereo_pair_drop_age_max_ns / 1000000.0);

                // Observations are recorded only on DRM's confirmed first
                // L flip, never on producer publication or atomic submit.
                // Relative drift is between different clock timelines, and
                // is NOT the absolute A/V offset. With display-resample the
                // movie rate can intentionally differ from the wall clock.
                double scanout_span_s = drm->stereo_pts_valid_L > 1
                    ? (drm->stereo_pts_last_ust_us -
                       drm->stereo_pts_first_ust_us) / 1000000.0 : 0.0;
                double movie_span_s = drm->stereo_pts_valid_L > 1
                    ? drm->stereo_pts_last_media_pts -
                      drm->stereo_pts_first_media_pts : 0.0;
                double avg_drift_ms = drm->stereo_pts_valid_L
                    ? drm->stereo_pts_drift_sum_ms /
                      drm->stereo_pts_valid_L : 0.0;
                MP_INFO(ctx, "Stereo PTS final: "
                        "first_L_confirmed=%llu valid_pts=%llu missing_pts=%llu "
                        "segments=%llu discontinuities=%llu "
                        "long_gap_resets=%llu long_gap_ust_s=%.3f "
                        "frame_id_gaps=%llu "
                        "cycles_1=%llu cycles_2=%llu cycles_3=%llu cycles_4plus=%llu "
                        "scanout_span_s=%.6f movie_pts_span_s=%.6f "
                        "relative_drift_last_ms=%.3f relative_drift_avg_ms=%.3f "
                        "relative_drift_min_ms=%.3f relative_drift_max_ms=%.3f "
                        "trace_samples_total=%llu ust_monotonic=%d "
                        "(relative to segment anchor, NOT audio-to-photon)\n",
                        (unsigned long long)drm->stereo_pts_first_L,
                        (unsigned long long)drm->stereo_pts_valid_L,
                        (unsigned long long)drm->stereo_pts_missing_L,
                        (unsigned long long)drm->stereo_pts_segments,
                        (unsigned long long)drm->stereo_pts_discontinuities,
                        (unsigned long long)drm->stereo_pts_long_gap_resets,
                        drm->stereo_pts_long_gap_ust_us / 1000000.0,
                        (unsigned long long)drm->stereo_pts_id_gaps,
                        (unsigned long long)drm->stereo_pts_cycles_1,
                        (unsigned long long)drm->stereo_pts_cycles_2,
                        (unsigned long long)drm->stereo_pts_cycles_3,
                        (unsigned long long)drm->stereo_pts_cycles_4plus,
                        scanout_span_s, movie_span_s,
                        drm->stereo_pts_last_drift_ms, avg_drift_ms,
                        drm->stereo_pts_drift_min_ms,
                        drm->stereo_pts_drift_max_ms,
                        (unsigned long long)drm->stereo_pts_trace_total,
                        drm->stereo_ust_monotonic);

                // The requested vsync count belongs to the PREVIOUS
                // pictured movie frame. Only intervals between consecutive
                // frame IDs can be compared unambiguously. Missing pictures
                // and seeks are explicitly excluded from the plan matrix.
                MP_INFO(ctx, "Stereo plan final: "
                        "planned_0=%llu planned_1=%llu planned_2=%llu "
                        "planned_3=%llu planned_4plus=%llu "
                        "nondisplay=%llu invalid_plan=%llu "
                        "adjacent_compared=%llu exact=%llu mismatched=%llu "
                        "skipped_intervals=%llu skipped_frame_ids=%llu "
                        "invalid_msc=%llu signed_error_cycles=%lld "
                        "abs_error_cycles=%llu "
                        "(previous frame plan vs next confirmed first L)\n",
                        (unsigned long long)drm->stereo_plan_requested_hist[0],
                        (unsigned long long)drm->stereo_plan_requested_hist[1],
                        (unsigned long long)drm->stereo_plan_requested_hist[2],
                        (unsigned long long)drm->stereo_plan_requested_hist[3],
                        (unsigned long long)drm->stereo_plan_requested_hist[4],
                        (unsigned long long)drm->stereo_plan_nondisplay,
                        (unsigned long long)drm->stereo_plan_invalid,
                        (unsigned long long)drm->stereo_plan_comparisons,
                        (unsigned long long)drm->stereo_plan_exact,
                        (unsigned long long)(drm->stereo_plan_comparisons -
                                             drm->stereo_plan_exact),
                        (unsigned long long)drm->stereo_plan_skipped_intervals,
                        (unsigned long long)drm->stereo_plan_skipped_ids,
                        (unsigned long long)drm->stereo_plan_invalid_msc,
                        (long long)drm->stereo_plan_signed_error_cycles,
                        (unsigned long long)drm->stereo_plan_abs_error_cycles);
                static const char *const plan_labels[5] =
                    {"0", "1", "2", "3", "4plus"};
                for (int requested = 0; requested < 5; requested++) {
                    MP_INFO(ctx, "Stereo plan row: requested=%s "
                            "actual_0=%llu actual_1=%llu actual_2=%llu "
                            "actual_3=%llu actual_4plus=%llu\n",
                            plan_labels[requested],
                            (unsigned long long)drm->stereo_plan_matrix[requested][0],
                            (unsigned long long)drm->stereo_plan_matrix[requested][1],
                            (unsigned long long)drm->stereo_plan_matrix[requested][2],
                            (unsigned long long)drm->stereo_plan_matrix[requested][3],
                            (unsigned long long)drm->stereo_plan_matrix[requested][4]);
                }

                // Bounded 256-entry scalar trace: read after RT thread
                // exit, so no synchronization or memory allocation occurs
                // in the page-flip callback.
                uint64_t n = MPMIN(drm->stereo_pts_trace_total,
                                   DRM_STEREO_PTS_TRACE_CAP);
                uint64_t first = drm->stereo_pts_trace_total - n;
                for (uint64_t i = first; i < drm->stereo_pts_trace_total; i++) {
                    const struct drm_stereo_pts_sample *sample =
                        &drm->stereo_pts_trace[i % DRM_STEREO_PTS_TRACE_CAP];
                    MP_INFO(ctx, "Stereo PTS sample: frame_id=%llu "
                            "generation=%llu movie_pts_s=%.6f "
                            "confirmed_L_msc=%u confirmed_L_ust_us=%llu "
                            "cycles_from_prev=%u relative_drift_ms=%.3f "
                            "mpv_planned_vsyncs=%d display_synced=%d "
                            "previous_planned_vsyncs=%d skipped_frame_ids=%llu "
                            "plan_comparable=%d\n",
                            (unsigned long long)sample->frame_id,
                            (unsigned long long)sample->generation,
                            sample->media_pts, sample->msc,
                            (unsigned long long)sample->ust_us,
                            sample->cycles_since_previous,
                            sample->relative_drift_ms,
                            sample->planned_vsyncs, sample->display_synced,
                            sample->previous_planned_vsyncs,
                            (unsigned long long)sample->frame_id_gap,
                            sample->plan_comparable);
                }
            }
        }
    }

    // Stop referencing the BOs on the display before releasing the GBM
    // surface locks or destroying the DRM framebuffers.
    if (p && p->stereo.enabled && ctx->vo->drm)
        vo_drm_release_crtc(ctx->vo->drm);

    ra_gl_ctx_uninit(ctx);

    if (p) {
        struct stereo_kms *s = &p->stereo;
        if (s->initialized) {
            stereo_release_pair(p, &s->building);
            stereo_release_pair(p, &s->pending);
            stereo_release_pair(p, &s->queued);
            stereo_release_pair(p, &s->active);
            for (unsigned i = 0; i < s->num_retired; i++)
                stereo_release_pair(p, &s->retired[i]);
            s->num_retired = 0;
            pthread_cond_destroy(&s->flip_ready);
            pthread_cond_destroy(&s->ready);
            pthread_mutex_destroy(&s->mutex);
        }
        // All locked front buffers must be released before destruction.
        while (p->gbm.num_bos)
            swapchain_step(ctx);

        if (p->egl.display != EGL_NO_DISPLAY) {
            eglMakeCurrent(p->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
            if (s->right_egl != EGL_NO_SURFACE)
                eglDestroySurface(p->egl.display, s->right_egl);
            if (p->egl.surface != EGL_NO_SURFACE)
                eglDestroySurface(p->egl.display, p->egl.surface);
            if (p->egl.context != EGL_NO_CONTEXT)
                eglDestroyContext(p->egl.display, p->egl.context);
        }
        if (s->right_gbm)
            gbm_surface_destroy(s->right_gbm);
        if (p->gbm.surface)
            gbm_surface_destroy(p->gbm.surface);
        if (p->egl.display != EGL_NO_DISPLAY)
            eglTerminate(p->egl.display);
        if (p->gbm.device)
            gbm_device_destroy(p->gbm.device);
        if (p->drm_params.render_fd != -1)
            close(p->drm_params.render_fd);
    }

    // The DRM fd must outlive all gbm_bo framebuffer destroy callbacks.
    vo_drm_uninit(ctx->vo);
}

// If the draw plane supports ARGB we want to use that, but if it doesn't we
// fall back on XRGB. If we do not have atomic there is no particular reason to
// be using ARGB (drmprime hwdec will not work without atomic, anyway), so we
// fall back to XRGB (another reason is that we do not have the convenient
// atomic_ctx and its convenient plane fields).
static bool probe_gbm_format(struct ra_ctx *ctx, uint32_t argb_format, uint32_t xrgb_format)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;

    drmModePlane *drmplane = drmModeGetPlane(drm->fd, drm->atomic_context->draw_plane->id);
    bool have_argb = false;
    bool have_xrgb = false;
    bool result = false;
    for (unsigned int i = 0; i < drmplane->count_formats; ++i) {
        if (drmplane->formats[i] == argb_format) {
            have_argb = true;
        } else if (drmplane->formats[i] == xrgb_format) {
            have_xrgb = true;
        }
    }

    if (have_argb) {
        p->gbm_format = argb_format;
        MP_VERBOSE(ctx->vo, "%s supported by draw plane.\n", gbm_format_to_string(argb_format));
        result = true;
    } else if (have_xrgb) {
        p->gbm_format = xrgb_format;
        MP_VERBOSE(ctx->vo, "%s not supported by draw plane: Falling back to %s.\n",
                   gbm_format_to_string(argb_format), gbm_format_to_string(xrgb_format));
        result = true;
    }

    drmModeFreePlane(drmplane);
    return result;
}

static bool probe_gbm_modifiers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;

    drmModePropertyBlobPtr blob =  drm_object_get_property_blob(drm->atomic_context->draw_plane,
                                                                "IN_FORMATS");
    if (!blob) {
        MP_VERBOSE(ctx->vo, "Failed to find IN_FORMATS property\n");
        return false;
    }

    struct drm_format_modifier_blob *data = blob->data;
    uint32_t *fmts = (uint32_t *)((char *)data + data->formats_offset);
    struct drm_format_modifier *mods =
        (struct drm_format_modifier *)((char *)data + data->modifiers_offset);

    for (unsigned int j = 0; j < data->count_modifiers; ++j) {
        struct drm_format_modifier *mod = &mods[j];
        for (uint64_t k = 0; k < 64; ++k) {
            if (mod->formats & (1ull << k)) {
                uint32_t fmt = fmts[k + mod->offset];
                if (fmt == p->gbm_format) {
                    MP_TARRAY_APPEND(p, p->gbm_modifiers,
                                        p->num_gbm_modifiers, mod->modifier);
                    MP_VERBOSE(ctx->vo, "Supported modifier: 0x%"PRIX64"\n",
                                (uint64_t)mod->modifier);
                    break;
                }
            }
        }
    }
    drmModeFreePropertyBlob(blob);

    if (p->num_gbm_modifiers == 0) {
        MP_VERBOSE(ctx->vo, "No supported DRM modifiers found.\n");
    }
    return true;
}

static void drm_egl_get_vsync(struct ra_ctx *ctx, struct vo_vsync_info *info)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;
    // The paired KMS worker calls drm_pflip_cb on another thread.
    // Protect present_sync_get_info against concurrent updates.
    if (p->stereo.initialized)
        pthread_mutex_lock(&p->stereo.mutex);
    present_sync_get_info(drm->present, info);
    if (p->stereo.initialized)
        pthread_mutex_unlock(&p->stereo.mutex);
}

static pl_color_space_t drm_egl_preferred_csp(struct ra_ctx *ctx)
{
    struct vo_drm_state *drm = ctx->vo->drm;
    pl_color_space_t bt2020 = {0};

    // If the display supports either one of these colorspaces, return it as
    // preferred.
    if (vo_drm_color_supported_by_display(drm, pl_color_space_hdr10)) {
        bt2020 = pl_color_space_hdr10;
    } else if (vo_drm_color_supported_by_display(drm, pl_color_space_bt2020_hlg)) {
        bt2020 = pl_color_space_bt2020_hlg;
    }

    if (bt2020.primaries != PL_COLOR_PRIM_UNKNOWN) {
        bt2020.hdr.prim.red.x = drm->chromaticity->red_x;
        bt2020.hdr.prim.red.y = drm->chromaticity->red_y;
        bt2020.hdr.prim.green.x = drm->chromaticity->green_x;
        bt2020.hdr.prim.green.y = drm->chromaticity->green_y;
        bt2020.hdr.prim.blue.x = drm->chromaticity->blue_x;
        bt2020.hdr.prim.blue.y = drm->chromaticity->blue_y;
        bt2020.hdr.prim.white.x = drm->chromaticity->white_x;
        bt2020.hdr.prim.white.y = drm->chromaticity->white_y;
        bt2020.hdr.min_luma = drm->hdr_static_metadata->desired_content_min_luminance;
        bt2020.hdr.max_luma = drm->hdr_static_metadata->desired_content_max_luminance;
        return bt2020;
    }

    return pl_color_space_srgb;
}

static bool drm_egl_set_color(struct ra_ctx *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    struct pl_color_space sdr = pl_color_space_srgb;
    p->allow_modeset |= vo_drm_set_color(ctx->vo, params ? &params->color : &sdr);
    return true;
}

// Do not alter ordinary DRM/2D output or legacy SBS/PAIR experiment defaults.
// The production --drm-stereo-3d path enables these two policies by default.
// Strict 0/1 overrides permit independently bisecting queue and hold behavior.
// Confirmed-flip callback: fixed cost, nonblocking, no libusb or allocation.
// The callback runs with stereo.mutex held by the KMS event thread.
static void stereo_usb_confirm_flip(void *opaque, int eye, uint32_t msc,
                                    uint64_t ust_us)
{
    struct stereo_usb *u = opaque;
    if (atomic_load_explicit(&u->stop, memory_order_relaxed))
        return;

    if (!atomic_load_explicit(&u->confirmed, memory_order_relaxed))
        atomic_store_explicit(&u->first_confirmed_msc_plus_one,
                              (uint64_t)msc + 1, memory_order_relaxed);

    // seq is even while the payload is complete; odd during an update.
    // This avoids pairing an eye from one MSC with a UST from another.
    // The first acq_rel RMW prevents publishing payload before marking
    // the sequence odd; the final release RMW publishes both payload fields.
    atomic_fetch_add_explicit(&u->mailbox_seq, 1, memory_order_acq_rel);
    atomic_store_explicit(&u->mailbox_ust_us, ust_us, memory_order_relaxed);
    atomic_store_explicit(&u->mailbox_event,
                          (((uint64_t)msc << 1) | (unsigned)(eye & 1)) + 1,
                          memory_order_relaxed);
    atomic_fetch_add_explicit(&u->mailbox_seq, 1, memory_order_release);
    atomic_fetch_add_explicit(&u->confirmed, 1, memory_order_relaxed);

    uint64_t one = 1;
    if (write(u->event_fd, &one, sizeof(one)) != sizeof(one))
        atomic_fetch_add_explicit(&u->wake_errors, 1, memory_order_relaxed);
}

static void *stereo_usb_sender_thread(void *opaque)
{
    struct stereo_usb *u = opaque;
    struct ra_ctx *ctx = u->ctx;
    void *lib = NULL, *dev = NULL;

    // Deliberately avoid a hard link dependency: in the default mode this
    // code does not load libnvstusb or claim any USB device at all.
    struct nvstusb_api {
        void *(*init)(const char *);
        void (*deinit)(void *);
        void (*set_rate)(void *, float);
        int (*effective_timer_us)(void *); // Optional diagnostic API.
        int (*set_eye_timer_us)(void *, int); // Optional runtime calibration.
        int (*last_error)(void *);
        void (*invert)(void *);
        int (*send_eye)(void *, int);
    } api = {0};
    int error = 0;
    if (!u->dry_run) {
        lib = dlopen(u->library ? u->library : "libnvstusb.so.0",
                     RTLD_NOW | RTLD_LOCAL);
        if (!lib && !u->library)
            lib = dlopen("libnvstusb.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            atomic_store_explicit(&u->init_status, -1, memory_order_release);
            MP_WARN(ctx, "Stereo USB: cannot load libnvstusb (%s); "
                    "video scanout continues without IR\n", dlerror());
            return NULL;
        }
        api.init = dlsym(lib, "nvstusb_init");
        api.deinit = dlsym(lib, "nvstusb_deinit");
        api.set_rate = dlsym(lib, "nvstusb_set_rate");
        api.effective_timer_us = dlsym(lib, "nvstusb_effective_eye_timer_us");
        api.set_eye_timer_us = dlsym(lib, "nvstusb_set_eye_timer_us");
        api.last_error = dlsym(lib, "nvstusb_last_error");
        api.invert = dlsym(lib, "nvstusb_invert_eyes");
        api.send_eye = dlsym(lib, "nvstusb_send_eye");
        if (!api.init || !api.deinit || !api.set_rate || !api.last_error ||
            !api.invert || !api.send_eye) {
            atomic_store_explicit(&u->init_status, -2, memory_order_release);
            MP_WARN(ctx, "Stereo USB: incompatible libnvstusb; "
                    "nvstusb_send_eye and its support API are required\n");
            goto shutdown;
        }
        // Firmware upload/re-enumeration and all other blocking USB traffic
        // execute only in this normal-priority thread.
        dev = api.init(u->firmware);
        if (!dev) {
            atomic_store_explicit(&u->init_status, -3, memory_order_release);
            MP_WARN(ctx, "Stereo USB: emitter unavailable or firmware failed; "
                    "video scanout continues\n");
            goto shutdown;
        }
        api.set_rate(dev, u->refresh_hz);
        error = api.last_error(dev);
        if (error) {
            atomic_store_explicit(&u->init_status, -4, memory_order_release);
            u->usb_errors++;
            u->usb_error_code = error;
            MP_WARN(ctx, "Stereo USB: emitter timing setup failed: %d\n", error);
            goto shutdown;
        }
        // A request for a nonstandard firmware timer is only meaningful if
        // we can prove that the loaded .so actually applied it. This prevents
        // misleading optical tests using an older libnvstusb from LD paths.
        const char *timer_env = getenv("NVSTUSB_EYE_TIMER_US");
        if (timer_env && *timer_env) {
            char *end = NULL;
            errno = 0;
            long requested = strtol(timer_env, &end, 10);
            if (errno || end == timer_env || *end != '\0' ||
                requested < 500 || requested > 10000 ||
                !api.effective_timer_us) {
                atomic_store_explicit(&u->init_status, -5,
                                      memory_order_release);
                MP_ERR(ctx, "Stereo USB: cannot verify requested firmware "
                       "eye timer '%s' (need libnvstusb calibration "
                       "API); aborting optical test\n", timer_env);
                goto shutdown;
            }
            int effective = api.effective_timer_us(dev);
            if (effective != requested) {
                atomic_store_explicit(&u->init_status, -5,
                                      memory_order_release);
                MP_ERR(ctx, "Stereo USB: requested eye timer=%ld us but "
                       "loaded library reports %d us; aborting test\n",
                       requested, effective);
                goto shutdown;
            }
            MP_INFO(ctx, "Stereo USB: verified firmware eye timer=%d us "
                    "(requested=%ld, phase NOT optically calibrated)\n",
                    effective, requested);
        } else if (api.effective_timer_us) {
            MP_INFO(ctx, "Stereo USB: default firmware eye timer=%d us "
                    "(effective library setting)\n",
                    api.effective_timer_us(dev));
        }
        if (u->control_fd >= 0 &&
            (!api.effective_timer_us || !api.set_eye_timer_us)) {
            atomic_store_explicit(&u->init_status, -6,
                                  memory_order_release);
            MP_ERR(ctx, "Stereo USB: live calibration requires a rebuilt "
                   "libnvstusb with effective_eye_timer_us and "
                   "set_eye_timer_us; refusing VO startup\n");
            goto shutdown;
        }
        if (api.effective_timer_us)
            u->final_eye_timer_us = api.effective_timer_us(dev);
        if (u->invert_eyes)
            api.invert(dev);
    }
    MP_INFO(ctx, "Stereo USB: sender initialized at %.3fHz "
            "(dry_run=%d inverted=%d, confirmed KMS flip trigger, "
            "optical phase UNCALIBRATED)\n",
            u->refresh_hz, u->dry_run, u->invert_eyes);
    atomic_store_explicit(&u->init_status, 1, memory_order_release);

    uint64_t last_word = 0;
    uint32_t last_msc = 0;
    bool have_msc = false;
    while (!atomic_load_explicit(&u->stop, memory_order_acquire)) {
        struct pollfd fd[2] = {
            {.fd = u->event_fd, .events = POLLIN},
            {.fd = u->control_fd, .events = POLLIN},
        };
        int n = poll(fd, 2, 100);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n > 0 &&
            ((fd[0].revents | fd[1].revents) & (POLLERR | POLLNVAL))))
            break;
        if (n == 0)
            continue;
        if (atomic_load_explicit(&u->stop, memory_order_acquire))
            break;

        // The same USB thread owns firmware timing and USB transfers.
        // User commands never touch the KMS worker, nor race send_eye().
        // At most one short FIFO read per wake; no blocking I/O here.
        if (u->control_fd >= 0 && (fd[1].revents & POLLIN)) {
            char buffer[256];
            ssize_t nr = read(u->control_fd, buffer, sizeof(buffer) - 1);
            if (nr > 0) {
                buffer[nr] = '\0';
                char *save = NULL;
                for (char *line = strtok_r(buffer, "\n", &save);
                     line; line = strtok_r(NULL, "\n", &save)) {
                    int current = api.effective_timer_us(dev);
                    long target = current;
                    bool update = false;
                    bool valid = false;
                    if (strcmp(line, "get") == 0) {
                        valid = true;
                    } else if (line[0] == '+' || line[0] == '-' ||
                               strncmp(line, "set ", 4) == 0) {
                        const char *val = strncmp(line, "set ", 4) == 0
                                          ? line + 4 : line;
                        char *end = NULL;
                        errno = 0;
                        long number = strtol(val, &end, 10);
                        if (!errno && end != val && *end == '\0') {
                            target = val == line ? current + number : number;
                            valid = true;
                            update = true;
                        }
                    }
                    int max_timer = (int)(1000000.0 / u->refresh_hz) - 500;
                    if (valid && target >= 500 && target <= max_timer) {
                        int rc = update
                               ? api.set_eye_timer_us(dev, (int)target) : 0;
                        if (rc == 0) {
                            u->final_eye_timer_us = api.effective_timer_us(dev);
                            if (update)
                                u->control_updates++;
                            MP_INFO(ctx, "Stereo USB phase control: "
                                    "eye_timer_us=%d previous=%d "
                                    "updates=%llu optical_verified=0\n",
                                    u->final_eye_timer_us, current,
                                    (unsigned long long)u->control_updates);
                        } else {
                            u->control_errors++;
                            MP_WARN(ctx, "Stereo USB phase control: "
                                    "libnvstusb rejected %ld us: %d\n",
                                    target, rc);
                        }
                    } else {
                        u->control_errors++;
                        MP_WARN(ctx, "Stereo USB phase control: invalid "
                                "command '%s' (get, +N, -N, set N; "
                                "range 500..%d us)\n", line, max_timer);
                    }
                }
            }
        }
        if (!(fd[0].revents & POLLIN))
            continue;
        uint64_t wake_count;
        if (read(u->event_fd, &wake_count, sizeof(wake_count)) !=
            sizeof(wake_count))
            continue;
        if (atomic_load_explicit(&u->stop, memory_order_acquire))
            break;

        // Read a consistent snapshot from the single KMS callback writer.
        uint64_t word = 0, ust_us = 0, seq1 = 0, seq2 = 0;
        do {
            seq1 = atomic_load_explicit(&u->mailbox_seq, memory_order_acquire);
            if (seq1 & 1)
                continue;
            word = atomic_load_explicit(&u->mailbox_event,
                                         memory_order_relaxed);
            ust_us = atomic_load_explicit(&u->mailbox_ust_us,
                                          memory_order_relaxed);
            seq2 = atomic_load_explicit(&u->mailbox_seq, memory_order_acquire);
        } while ((seq1 & 1) || seq1 != seq2);
        if (!word || word == last_word)
            continue;
        last_word = word;
        uint64_t payload = word - 1;
        uint32_t msc = (uint32_t)(payload >> 1);
        int eye = (int)(payload & 1u);
        if (have_msc) {
            uint32_t diff = msc - last_msc;
            if (diff > 1 && diff < 1000)
                u->skipped += diff - 1;
        } else {
            uint64_t first_plus_one =
                atomic_load_explicit(&u->first_confirmed_msc_plus_one,
                                     memory_order_acquire);
            if (first_plus_one) {
                uint32_t initial_msc = (uint32_t)(first_plus_one - 1);
                uint32_t startup_diff = msc - initial_msc;
                // The initial confirmed flips happened while the USB
                // worker was loading firmware/configuring the emitter.
                // They were never sent, even though skipped_msc stays 0.
                if (startup_diff < 100000)
                    u->startup_skipped = startup_diff;
            }
        }
        last_msc = msc;
        have_msc = true;

        if (u->timestamp_monotonic) {
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
                int64_t start_us = (int64_t)ts.tv_sec * 1000000LL +
                                    ts.tv_nsec / 1000;
                double ms = (start_us - (int64_t)ust_us) / 1000.0;
                if (!u->signed_lag_samples ||
                    ms < u->min_signed_scanout_to_send_ms)
                    u->min_signed_scanout_to_send_ms = ms;
                if (!u->signed_lag_samples ||
                    ms > u->max_signed_scanout_to_send_ms)
                    u->max_signed_scanout_to_send_ms = ms;
                u->signed_lag_samples++;
                if (ms < 0)
                    u->signed_lag_negative++;
                // Keep the old nonnegative-only maximum for compatibility,
                // but use the SIGNED extrema when diagnosing vblank phase.
                if (ms > u->max_scanout_to_send_ms)
                    u->max_scanout_to_send_ms = ms;
                if (ms > 2.0)
                    u->late_2ms++;
                if (ms > 1000.0 / u->refresh_hz)
                    u->late_period++;
            }
        }
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        if (!u->dry_run)
            error = api.send_eye(dev, eye);
        clock_gettime(CLOCK_MONOTONIC, &end);
        double usb_ms = (end.tv_sec - begin.tv_sec) * 1000.0 +
                        (end.tv_nsec - begin.tv_nsec) / 1e6;
        if (usb_ms > u->max_usb_transfer_ms)
            u->max_usb_transfer_ms = usb_ms;
        if (error) {
            u->usb_errors++;
            u->usb_error_code = error;
            MP_WARN(ctx, "Stereo USB: eye command failed at MSC %u: %d; "
                    "disabling USB, DRM/KMS playback unaffected\n",
                    msc, error);
            break;
        }
        u->sent++;
    }

shutdown:
    if (dev)
        api.deinit(dev);
    if (lib)
        dlclose(lib);
    return NULL;
}

// Called from drm_egl_init(), BEFORE any movie frame can start the KMS
// worker. Synchronously gate the initial stereo display on emitter readiness,
// but never do USB I/O or wait in the 120Hz KMS page-flip thread.
static bool stereo_usb_start(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;
    const char *enabled = getenv("MPV_DRM_STEREO_USB");
    if (!enabled || strcmp(enabled, "0") == 0)
        return true;
    if (strcmp(enabled, "1") != 0) {
        MP_WARN(ctx, "Invalid MPV_DRM_STEREO_USB='%s' (expected 0 or 1)\n",
                enabled);
        return true;
    }
    if (!drm->opts || !drm->opts->stereo_3d || !p->stereo.enabled) {
        MP_WARN(ctx, "Stereo USB: requires native --drm-stereo-3d=yes; "
                "ignoring request\n");
        return true;
    }
    struct stereo_usb *u = &p->stereo.usb;
    u->ctx = ctx;
    u->control_fd = -1;
    u->timestamp_monotonic = drm->stereo_ust_monotonic;
    u->refresh_hz = vo_drm_get_display_fps(drm); // PHYSICAL, not logical 60 Hz.
    u->firmware = talloc_strdup(p, getenv("MPV_DRM_STEREO_USB_FIRMWARE")
                                   ? getenv("MPV_DRM_STEREO_USB_FIRMWARE")
                                   : "nvstusb.fw");
    const char *library = getenv("MPV_DRM_STEREO_USB_LIBRARY");
    if (library && *library)
        u->library = talloc_strdup(p, library);
    const char *invert = getenv("MPV_DRM_STEREO_USB_INVERT_EYES");
    u->invert_eyes = invert && strcmp(invert, "1") == 0;
    const char *dry_run = getenv("MPV_DRM_STEREO_USB_DRY_RUN");
    u->dry_run = dry_run && strcmp(dry_run, "1") == 0;
    const char *eye_timer = getenv("NVSTUSB_EYE_TIMER_US");
    if (eye_timer && *eye_timer && !u->dry_run)
        MP_WARN(ctx, "Stereo USB: experimental libnvstusb firmware "
                "eye timer override requested: %s us; "
                "NOT measured optical phase. The library must be built "
                "from feature/usb-eye-timer-calibration.\n", eye_timer);
    atomic_init(&u->stop, false);
    atomic_init(&u->init_status, 0);
    atomic_init(&u->mailbox_seq, 0);
    atomic_init(&u->mailbox_event, 0);
    atomic_init(&u->mailbox_ust_us, 0);
    atomic_init(&u->confirmed, 0);
    atomic_init(&u->first_confirmed_msc_plus_one, 0);
    atomic_init(&u->wake_errors, 0);

    const char *control = getenv("MPV_DRM_STEREO_USB_CONTROL");
    if (control && strcmp(control, "1") == 0) {
        if (u->dry_run) {
            MP_ERR(ctx, "Stereo USB: runtime eye-timer control requires "
                   "live hardware (dry-run has no firmware timing)\n");
            return false;
        }
        const char *dir = getenv("XDG_RUNTIME_DIR");
        struct stat st;
        if (!dir || !*dir || stat(dir, &st) != 0 ||
            !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0077) != 0) {
            MP_ERR(ctx, "Stereo USB: runtime calibration requires a "
                   "private XDG_RUNTIME_DIR owned by this user\n");
            return false;
        }
        u->control_path = talloc_asprintf(p, "%s/mpv-stereo-usb-%ld.fifo",
                                           dir, (long)getpid());
        if (strlen(u->control_path) >= 240) {
            MP_ERR(ctx, "Stereo USB: runtime FIFO path too long\n");
            return false;
        }
        // Never delete or replace an existing FIFO on start.
        if (mkfifo(u->control_path, 0600) != 0) {
            MP_ERR(ctx, "Stereo USB: failed to create calibration FIFO "
                   "'%s': %s\n", u->control_path, mp_strerror(errno));
            return false;
        }
        u->control_fd = open(u->control_path,
                             O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (u->control_fd < 0) {
            MP_ERR(ctx, "Stereo USB: failed to open calibration FIFO: %s\n",
                   mp_strerror(errno));
            unlink(u->control_path);
            return false;
        }
        MP_INFO(ctx, "Stereo USB: live eye-timer control FIFO: %s "
                "(get, +N, -N, set N; timer microseconds)\n",
                u->control_path);
    } else if (control && strcmp(control, "0") != 0) {
        MP_WARN(ctx, "Invalid MPV_DRM_STEREO_USB_CONTROL='%s' "
                "(expected 0 or 1; control disabled)\n", control);
    }
    u->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (u->event_fd < 0) {
        MP_ERR(ctx, "Stereo USB: eventfd failed: %s\n", mp_strerror(errno));
        if (u->control_fd >= 0) {
            close(u->control_fd);
            unlink(u->control_path);
        }
        return false;
    }
    int err = pthread_create(&u->thread, NULL, stereo_usb_sender_thread, u);
    if (err) {
        MP_ERR(ctx, "Stereo USB: cannot start sender thread: %s\n",
               mp_strerror(err));
        close(u->event_fd);
        if (u->control_fd >= 0) {
            close(u->control_fd);
            unlink(u->control_path);
        }
        return false;
    }
    u->started = true;

    // A real emitter may need firmware upload and USB re-enumeration.
    // This bounded VO-init wait prevents the ~100 initial shutter commands
    // previously lost while independent KMS already displayed L/R.
    // Timeout affects readiness only: the uninit join must still wait for
    // the USB worker to finish so its ctx/device cannot be freed in use.
    int timeout_ms = 3000;
    const char *timeout_env = getenv("MPV_DRM_STEREO_USB_INIT_TIMEOUT_MS");
    if (timeout_env && *timeout_env) {
        char *end = NULL;
        errno = 0;
        long value = strtol(timeout_env, &end, 10);
        if (!errno && end != timeout_env && *end == '\0' &&
            value >= 100 && value <= 10000) {
            timeout_ms = value;
        } else {
            MP_WARN(ctx, "Invalid MPV_DRM_STEREO_USB_INIT_TIMEOUT_MS='%s' "
                    "(valid range 100..10000; using 3000)\n", timeout_env);
        }
    }
    int64_t started_ns = mp_time_ns();
    const int64_t deadline_ns = started_ns + (int64_t)timeout_ms * 1000000LL;
    int status;
    while ((status = atomic_load_explicit(&u->init_status,
                                           memory_order_acquire)) == 0 &&
           mp_time_ns() < deadline_ns) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    }
    status = atomic_load_explicit(&u->init_status, memory_order_acquire);
    if (status != 1) {
        MP_ERR(ctx, "Stereo USB: emitter NOT ready before first stereo "
               "scanout (status=%d timeout_ms=%d); aborting VO init "
               "rather than displaying unpaired shutter stereo\n",
               status, timeout_ms);
        // drm_egl_uninit() will stop and join the sender before freeing
        // the context. No KMS worker or movie frame has started yet.
        return false;
    }
    MP_INFO(ctx, "Stereo USB: startup gate READY, took %.3fms "
            "(timeout=%dms; physical KMS stereo can now begin)\n",
            (mp_time_ns() - started_ns) / 1000000.0, timeout_ms);

    drm->stereo_usb_opaque = u;
    drm->stereo_usb_on_confirmed_flip = stereo_usb_confirm_flip;
    MP_WARN(ctx, "Stereo USB: EXPERIMENTAL %s bridge enabled; "
            "not phase calibrated. Do not wear glasses until "
            "optically validated\n", u->dry_run ? "DRY-RUN" : "IR");
    return true;
}

static void stereo_usb_stop(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (!p || !p->stereo.usb.started)
        return;
    struct stereo_usb *u = &p->stereo.usb;
    // Caller must first join the KMS worker; no further flip hooks can run.
    struct vo_drm_state *drm = ctx->vo->drm;
    drm->stereo_usb_on_confirmed_flip = NULL;
    drm->stereo_usb_opaque = NULL;
    atomic_store_explicit(&u->stop, true, memory_order_release);
    uint64_t one = 1;
    (void)write(u->event_fd, &one, sizeof(one));
    pthread_join(u->thread, NULL);
    close(u->event_fd);
    if (u->control_fd >= 0) {
        close(u->control_fd);
        unlink(u->control_path);
    }
    MP_INFO(ctx, "Stereo USB phase control final: enabled=%d "
            "updates=%llu errors=%llu eye_timer_us=%d "
            "(firmware command parameter; lens timing NOT calibrated)\n",
            u->control_fd >= 0,
            (unsigned long long)u->control_updates,
            (unsigned long long)u->control_errors,
            u->final_eye_timer_us);
    uint64_t confirmed = atomic_load(&u->confirmed);
    uint64_t explained = u->sent + u->startup_skipped + u->skipped;
    uint64_t unaccounted = confirmed > explained ? confirmed - explained : 0;
    MP_INFO(ctx, "Stereo USB final: dry_run=%d init_status=%d "
            "confirmed=%llu sent=%llu startup_skipped=%llu "
            "skipped_msc=%llu unaccounted=%llu "
            "wake_errors=%llu usb_errors=%llu "
            "last_usb_error=%d late_gt_2ms=%llu late_gt_period=%llu "
            "max_scanout_to_send_ms=%.3f max_usb_transfer_ms=%.3f "
            "signed_lag_samples=%llu signed_lag_negative=%llu "
            "min_signed_scanout_to_send_ms=%.3f "
            "max_signed_scanout_to_send_ms=%.3f "
            "(USB calls only, NOT optical sync)\n",
            u->dry_run,
            atomic_load_explicit(&u->init_status, memory_order_acquire),
            (unsigned long long)confirmed,
            (unsigned long long)u->sent,
            (unsigned long long)u->startup_skipped,
            (unsigned long long)u->skipped,
            (unsigned long long)unaccounted,
            (unsigned long long)atomic_load(&u->wake_errors),
            (unsigned long long)u->usb_errors, u->usb_error_code,
            (unsigned long long)u->late_2ms,
            (unsigned long long)u->late_period,
            u->max_scanout_to_send_ms, u->max_usb_transfer_ms,
            (unsigned long long)u->signed_lag_samples,
            (unsigned long long)u->signed_lag_negative,
            u->min_signed_scanout_to_send_ms,
            u->max_signed_scanout_to_send_ms);
    u->started = false;
}

static bool stereo_env_policy(struct ra_ctx *ctx, const char *name,
                              bool enabled, bool default_on)
{
    if (!enabled)
        return false;
    const char *value = getenv(name);
    if (!value)
        return default_on;
    if (strcmp(value, "0") == 0)
        return false;
    if (strcmp(value, "1") == 0)
        return true;
    MP_WARN(ctx, "Invalid %s='%s' (expected 0 or 1; using default %d)\n",
            name, value, default_on);
    return default_on;
}

static bool drm_egl_init(struct ra_ctx *ctx)
{
    if (!vo_drm_init(ctx->vo))
        goto err;

    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    struct vo_drm_state *drm = ctx->vo->drm;
    // Native option is the production-facing entry point. The older
    // experiment variables remain supported for regression comparisons;
    // they retain their original physical-120Hz VO pacing.
    bool native_3d = drm->opts && drm->opts->stereo_3d;
    const char *sbs = getenv("MPV_DRM_STEREO_SBS");
    const char *pair = getenv("MPV_DRM_STEREO_PAIR");
    p->stereo.enabled = native_3d ||
        (sbs && pair && strcmp(sbs, "1") == 0 &&
         strcmp(pair, "1") == 0);
    const char *seq = getenv("MPV_DRM_STEREO_SEQ");
    p->stereo.sequence_mode = p->stereo.enabled && seq &&
                              strcmp(seq, "1") == 0;
    const char *rt = getenv("MPV_DRM_STEREO_RT");
    p->stereo.rt_mode = p->stereo.enabled &&
                        (native_3d || (rt && strcmp(rt, "1") == 0));
    const char *gpu_wait = getenv("MPV_DRM_STEREO_GPU_WAIT");
    p->stereo.gpu_wait = p->stereo.enabled && gpu_wait &&
                         strcmp(gpu_wait, "1") == 0;
    p->stereo.plan_hold = stereo_env_policy(ctx, "MPV_DRM_STEREO_PLAN_HOLD",
                                            p->stereo.enabled, native_3d);
    drm->stereo_pair_plan_hold = p->stereo.plan_hold;
    if (p->stereo.plan_hold)
        MP_INFO(ctx, "Stereo planned-hold MSC gate enabled\n");
    else if (native_3d)
        MP_INFO(ctx, "Stereo planned-hold MSC gate disabled (explicit opt-out)\n");
    p->stereo.render_wake_eye =
        drm->opts ? drm->opts->stereo_pacing_eye : 0;
    const char *render_wake = getenv("MPV_DRM_STEREO_RENDER_WAKE_EYE");
    if (p->stereo.enabled && render_wake) {
        if (strcmp(render_wake, "left") == 0)
            p->stereo.render_wake_eye = 0;
        else if (strcmp(render_wake, "right") == 0)
            p->stereo.render_wake_eye = 1;
        else
            MP_WARN(ctx, "Invalid MPV_DRM_STEREO_RENDER_WAKE_EYE='%s' "
                    "(expected left or right; unchanged)\n", render_wake);
    }
    if (p->stereo.enabled && drm->opts && drm->opts->stereo_3d &&
        p->stereo.render_wake_eye != drm->opts->stereo_pacing_eye)
        MP_INFO(ctx, "Stereo experimental early VO wake on %c; "
                "mpv presentation feedback remains on %c\n",
                p->stereo.render_wake_eye ? 'R' : 'L',
                drm->opts->stereo_pacing_eye ? 'R' : 'L');
    const char *protect_pending = getenv("MPV_DRM_STEREO_KEEP_READY_PENDING");
    p->stereo.keep_ready_pending = p->stereo.enabled && protect_pending &&
                                   strcmp(protect_pending, "1") == 0;
    p->stereo.two_slot_queue = stereo_env_policy(ctx,
                                                 "MPV_DRM_STEREO_TWO_SLOT_QUEUE",
                                                 p->stereo.enabled, native_3d);
    if (p->stereo.two_slot_queue) {
        MP_INFO(ctx, "Stereo two-slot pending FIFO enabled\n");
        if (p->stereo.keep_ready_pending)
            MP_WARN(ctx, "TWO_SLOT_QUEUE supersedes KEEP_READY_PENDING\n");
    } else if (native_3d) {
        MP_INFO(ctx, "Stereo two-slot pending FIFO disabled (explicit opt-out)\n");
    }
    if (p->stereo.enabled)
        MP_INFO(ctx, "Stereo scheduling policy: plan_hold=%d two_slot_queue=%d "
                "(native_3d=%d; overrides: PLAN_HOLD=0|1 TWO_SLOT_QUEUE=0|1)\n",
                p->stereo.plan_hold, p->stereo.two_slot_queue, native_3d);
    const char *queue_test_hold = getenv("MPV_DRM_STEREO_QUEUE_TEST_HOLD_MS");
    if (queue_test_hold && p->stereo.two_slot_queue) {
        char *end = NULL;
        long ms = strtol(queue_test_hold, &end, 10);
        if (end != queue_test_hold && *end == '\0' &&
            ms >= 100 && ms <= 2000) {
            p->stereo.queue_test_hold_ms = ms;
            MP_WARN(ctx, "Stereo stress test: gate first occupied "
                    "two-slot FIFO for %ldms (physical L/R continues; "
                    "intentional A/V stall)\n", ms);
        } else {
            MP_WARN(ctx, "Invalid QUEUE_TEST_HOLD_MS='%s' (100..2000)\n",
                    queue_test_hold);
        }
    }
    const char *right_swap_zero = getenv("MPV_DRM_STEREO_RIGHT_SWAP_INTERVAL_ZERO");
    p->stereo.right_swap_interval_zero = p->stereo.enabled &&
        right_swap_zero && strcmp(right_swap_zero, "1") == 0;
    if (p->stereo.right_swap_interval_zero)
        MP_INFO(ctx, "Stereo right EGLSurface swap interval 0 requested "
                "(experimental; independent of KMS refresh timing)\n");
    const char *freeze_pair = getenv("MPV_DRM_STEREO_FREEZE_PAIR");
    p->stereo.freeze_pair = p->stereo.enabled && freeze_pair &&
                             strcmp(freeze_pair, "1") == 0;
    const char *native_fence = getenv("MPV_DRM_STEREO_NATIVE_FENCE");
    // The stable new mode requires genuine GPU-completion fences and a
    // realtime KMS worker by default. Unsupported EGL extensions cause
    // initialization to fail rather than silently returning to unsafe flips.
    p->stereo.native_fence = p->stereo.enabled &&
        (native_3d || (native_fence && strcmp(native_fence, "1") == 0));
    drm->stereo_pair_native_fence = p->stereo.native_fence;
    const char *min_age = getenv("MPV_DRM_STEREO_MIN_PAIR_AGE_MS");
    if (p->stereo.enabled && min_age && *min_age) {
        char *end = NULL;
        errno = 0;
        long ms = strtol(min_age, &end, 10);
        if (!errno && end != min_age && *end == '\0' &&
            ms >= 0 && ms <= 32) {
            p->stereo.min_pair_age_ns = (int64_t)ms * 1000000LL;
        } else {
            MP_WARN(ctx, "Invalid MPV_DRM_STEREO_MIN_PAIR_AGE_MS='%s' "
                    "(valid range 0..32; using 0)\n", min_age);
        }
    }
    drm->stereo_pair_sequence_mode = p->stereo.sequence_mode;
    drm->stereo_pair_gpu_wait = p->stereo.gpu_wait;
    if (p->stereo.enabled) {
        // DRM/KMS may select a different GPU than the machine's first
        // lspci VGA controller. Identify the *opened* KMS fd explicitly.
        drmVersionPtr v = drmGetVersion(drm->fd);
        MP_INFO(ctx, "Stereo DRM/KMS: node=%s driver=%s version=%d.%d.%d\n",
                drm->card_path ? drm->card_path : "(unknown)",
                v && v->name ? v->name : "(unknown)",
                v ? v->version_major : 0,
                v ? v->version_minor : 0,
                v ? v->version_patchlevel : 0);
        if (v)
            drmFreeVersion(v);
    }
    const char *legacy = getenv("MPV_DRM_STEREO_LEGACY");
    p->stereo.legacy_mode = p->stereo.enabled && legacy &&
                            strcmp(legacy, "1") == 0;
    drm->stereo_pair_legacy_mode = p->stereo.legacy_mode;
    drm->stereo_callback_lag_valid = false;
    drm->stereo_prev_callback_lag_valid = false;
    drm->stereo_submit_after_ust_valid = false;
    if (p->stereo.enabled) {
        uint64_t timestamp_monotonic = 0;
        int rc = drmGetCap(drm->fd, DRM_CAP_TIMESTAMP_MONOTONIC,
                           &timestamp_monotonic);
        drm->stereo_ust_monotonic = rc == 0 && timestamp_monotonic != 0;
        MP_INFO(ctx, "Stereo DRM vblank UST clock: %s "
                "(callback delivery / submit phase diagnostics %s)\n",
                drm->stereo_ust_monotonic ? "CLOCK_MONOTONIC" : "unverified",
                drm->stereo_ust_monotonic ? "enabled" : "disabled");
    }
    if (p->stereo.legacy_mode)
        MP_INFO(ctx, "Stereo experimental legacy CRTC page-flip enabled "
                "(requires CRTC primary plane; automatic atomic fallback)\n");
    if (p->stereo.gpu_wait)
        MP_INFO(ctx, "Stereo paired GPU completion wait enabled "
                "(eglWaitClient after right-eye EGL swap)\n");
    if (p->stereo.freeze_pair)
        MP_INFO(ctx, "Stereo freeze-pair A/B: rendering continues but "
                "only generation 1 will be submitted to KMS\n");
    if (p->stereo.min_pair_age_ns)
        MP_INFO(ctx, "Stereo pair minimum age before KMS promotion: "
                "%lld ms (A/B diagnostic; additional video latency)\n",
                (long long)(p->stereo.min_pair_age_ns / 1000000LL));
    p->stereo.capture_eye = -1;
    if (p->stereo.enabled) {
        int err = pthread_mutex_init(&p->stereo.mutex, NULL);
        if (err) {
            MP_ERR(ctx, "Cannot initialize stereo KMS mutex: %d\n", err);
            goto err;
        }
        err = pthread_cond_init(&p->stereo.ready, NULL);
        if (err) {
            MP_ERR(ctx, "Cannot initialize stereo KMS ready event: %d\n", err);
            pthread_mutex_destroy(&p->stereo.mutex);
            goto err;
        }
        err = pthread_cond_init(&p->stereo.flip_ready, NULL);
        if (err) {
            MP_ERR(ctx, "Cannot initialize stereo KMS flip event: %d\n", err);
            pthread_cond_destroy(&p->stereo.ready);
            pthread_mutex_destroy(&p->stereo.mutex);
            goto err;
        }
        p->stereo.initialized = true;
    }
    p->drm_params.fd = -1;
    p->drm_params.render_fd = -1;

    if (ctx->vo->drm->opts->draw_surface_size.wh_valid) {
        p->draw_surface_size.width = ctx->vo->drm->opts->draw_surface_size.w;
        p->draw_surface_size.height = ctx->vo->drm->opts->draw_surface_size.h;
    } else {
        p->draw_surface_size.width = drm->mode.mode.hdisplay;
        p->draw_surface_size.height = drm->mode.mode.vdisplay;
    }

    drm->width = p->draw_surface_size.width;
    drm->height = p->draw_surface_size.height;

    uint32_t argb_format;
    uint32_t xrgb_format;
    switch (ctx->vo->drm->opts->drm_format) {
    case DRM_OPTS_FORMAT_XRGB2101010:
        argb_format = GBM_FORMAT_ARGB2101010;
        xrgb_format = GBM_FORMAT_XRGB2101010;
        break;
    case DRM_OPTS_FORMAT_XBGR2101010:
        argb_format = GBM_FORMAT_ABGR2101010;
        xrgb_format = GBM_FORMAT_XBGR2101010;
        break;
    case DRM_OPTS_FORMAT_XBGR8888:
        argb_format = GBM_FORMAT_ABGR8888;
        xrgb_format = GBM_FORMAT_XBGR8888;
        break;
    case DRM_OPTS_FORMAT_RGB565:
        argb_format = GBM_FORMAT_RGB565;
        xrgb_format = GBM_FORMAT_RGB565;
        break;
    default:
        if (drm->opts->drm_format != DRM_OPTS_FORMAT_XRGB8888) {
            MP_VERBOSE(ctx->vo, "Requested format not supported by context, "
                       "falling back to xrgb8888\n");
        }
        argb_format = GBM_FORMAT_ARGB8888;
        xrgb_format = GBM_FORMAT_XRGB8888;
        break;
    }

    if (!probe_gbm_format(ctx, argb_format, xrgb_format)) {
        MP_ERR(ctx->vo, "No suitable format found on draw plane (tried: %s and %s).\n",
               gbm_format_to_string(argb_format), gbm_format_to_string(xrgb_format));
        goto err;
    }

    // It is not fatal if this fails. We'll just try without modifiers.
    probe_gbm_modifiers(ctx);

    if (!init_gbm(ctx)) {
        MP_ERR(ctx->vo, "Failed to setup GBM.\n");
        goto err;
    }

    if (!init_egl(ctx)) {
        MP_ERR(ctx->vo, "Failed to setup EGL.\n");
        goto err;
    }
    if (!init_pair_surfaces(ctx)) {
        MP_ERR(ctx->vo, "Failed creating second stereo EGL/GBM surface\n");
        goto err;
    }

    if (!eglMakeCurrent(p->egl.display, p->egl.surface, p->egl.surface,
                        p->egl.context)) {
        MP_ERR(ctx->vo, "Failed to make context current.\n");
        goto err;
    }

    mpegl_load_functions(&p->gl, ctx->vo->log);
    if (p->stereo.native_fence) {
        // Require actual EGL extension support; do not silently fall back
        // to unprotected scanout when the user requested a fence test.
        const char *ext = eglQueryString(p->egl.display, EGL_EXTENSIONS);
        struct stereo_kms *s = &p->stereo;
        if (!ext || !strstr(ext, "EGL_KHR_fence_sync") ||
            !strstr(ext, "EGL_ANDROID_native_fence_sync")) {
            MP_ERR(ctx, "Stereo native fences require EGL_KHR_fence_sync "
                   "and EGL_ANDROID_native_fence_sync (got: %s)\n",
                   ext ? ext : "(unavailable)");
            goto err;
        }
        s->create_sync = (PFNEGLCREATESYNCKHRPROC)
            eglGetProcAddress("eglCreateSyncKHR");
        s->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)
            eglGetProcAddress("eglDestroySyncKHR");
        s->client_wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)
            eglGetProcAddress("eglClientWaitSyncKHR");
        s->dup_native_fence_fd = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
        if (!s->create_sync || !s->destroy_sync || !s->client_wait_sync ||
            !s->dup_native_fence_fd) {
            MP_ERR(ctx, "Stereo native fence EGL entry points unavailable\n");
            goto err;
        }
        MP_INFO(ctx, "Stereo native fences enabled: EGL exports two sync_file "
                "FDs per pair; KMS performs only nonblocking poll at "
                "left-eye promotion; min pair age remains independent\n");
    }
    if (p->stereo.enabled && p->gl.GetString) {
        // The GL context is now current: this reports the actual GPU
        // renderer, which need not be the card inferred from lspci.
        const char *vendor = (const char *)p->gl.GetString(GL_VENDOR);
        const char *renderer = (const char *)p->gl.GetString(GL_RENDERER);
        MP_INFO(ctx, "Stereo EGL/GL: vendor=%s renderer=%s\n",
                vendor ? vendor : "(unknown)",
                renderer ? renderer : "(unknown)");
    }
    // required by gbm_surface_lock_front_buffer
    eglSwapBuffers(p->egl.display, p->egl.surface);

    MP_VERBOSE(ctx, "Preparing framebuffer\n");
    struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(p->gbm.surface);
    if (!new_bo) {
        MP_ERR(ctx, "Failed to lock GBM surface.\n");
        goto err;
    }

    enqueue_bo(ctx, new_bo);
    update_framebuffer_from_bo(ctx, new_bo);
    if (!drm->fb || !drm->fb->id) {
        MP_ERR(ctx, "Failed to create framebuffer.\n");
        goto err;
    }

    if (!vo_drm_acquire_crtc(ctx->vo->drm)) {
        MP_ERR(ctx, "Failed to set CRTC for connector %u: %s\n",
               drm->connector->connector_id, mp_strerror(errno));
        goto err;
    }

    vo_drm_set_monitor_par(ctx->vo);

    p->drm_params.fd = drm->fd;
    p->drm_params.crtc_id = drm->crtc_id;
    p->drm_params.connector_id = drm->connector->connector_id;
    p->drm_params.atomic_request_ptr = &drm->atomic_context->request;
    char *rendernode_path = drmGetRenderDeviceNameFromFd(drm->fd);
    if (rendernode_path) {
        MP_VERBOSE(ctx, "Opening render node \"%s\"\n", rendernode_path);
        p->drm_params.render_fd = open(rendernode_path, O_RDWR | O_CLOEXEC);
        if (p->drm_params.render_fd == -1) {
            MP_WARN(ctx, "Cannot open render node: %s\n", mp_strerror(errno));
        }
        free(rendernode_path);
    } else {
        p->drm_params.render_fd = -1;
        MP_VERBOSE(ctx, "Could not find path to render node.\n");
    }

    struct ra_ctx_params params = {
        .get_vsync          = drm_egl_get_vsync,
        .preferred_csp      = drm_egl_preferred_csp,
        .set_color          = drm_egl_set_color,
        .swap_buffers       = drm_egl_swap_buffers,
    };
    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto err;
    if (p->stereo.enabled)
        eglSwapInterval(p->egl.display, 0);

    ra_add_native_resource(ctx->ra, "drm_params_v2", &p->drm_params);
    ra_add_native_resource(ctx->ra, "drm_draw_surface_size", &p->draw_surface_size);

    if (!stereo_usb_start(ctx))
        goto err;
    return true;

err:
    drm_egl_uninit(ctx);
    return false;
}

static bool drm_egl_reconfig(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_drm_state *drm = ctx->vo->drm;
    // The paired producer locks/recycles BOs. Never depend on a cached
    // drm->fb pointer whose owning GBM surface may already have recycled it.
    int width = p->stereo.enabled ? p->draw_surface_size.width : drm->fb->width;
    int height = p->stereo.enabled ? p->draw_surface_size.height : drm->fb->height;
    ctx->vo->dwidth = width;
    ctx->vo->dheight = height;
    ra_gl_ctx_resize(ctx->swapchain, width, height, 0);
    return true;
}

static int drm_egl_control(struct ra_ctx *ctx, int *events, int request,
                           void *arg)
{
    int ret = vo_drm_control(ctx->vo, events, request, arg);
    return ret;
}

static void drm_egl_wait_events(struct ra_ctx *ctx, int64_t until_time_ns)
{
    vo_drm_wait_events(ctx->vo, until_time_ns);
}

static void drm_egl_wakeup(struct ra_ctx *ctx)
{
    vo_drm_wakeup(ctx->vo);
}

const struct ra_ctx_fns ra_ctx_drm_egl = {
    .type            = "opengl",
    .name            = "drm",
    .description     = "DRM/EGL",
    .reconfig        = drm_egl_reconfig,
    .control         = drm_egl_control,
    .init            = drm_egl_init,
    .uninit          = drm_egl_uninit,
    .wait_events     = drm_egl_wait_events,
    .wakeup          = drm_egl_wakeup,
};
