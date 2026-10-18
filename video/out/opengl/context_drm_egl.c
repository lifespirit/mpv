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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
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
    uint64_t generation;
    // CPU time when BOTH eye GBM front buffers were published as a pair.
    int64_t ready_ns;
};

struct stereo_kms {
    bool enabled;
    bool sequence_mode;
    bool rt_mode;
    bool gpu_wait;
    bool legacy_mode;
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
    struct stereo_pair active, pending;
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

        // A pair generation may be replaced only on an L boundary. Do
        // not free the old generation until the new L flip is confirmed.
        bool promote = eye == 0 && s->pending.bo[0] &&
                       s->num_retired < MP_ARRAY_SIZE(s->retired);
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
                old = s->active;
                s->active = s->pending;
                s->pending = (struct stereo_pair){0};
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
                    // Callback confirmed a physical page flip. Inform the
                    // mpv VO pacing loop without making scanout wait on it.
                    s->confirmed_flips = drm->stereo_flips;
                    pthread_cond_broadcast(&s->flip_ready);
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

// These helpers are only invoked on the original VO/EGL thread.
bool vo_drm_stereo_pair_can_capture(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct stereo_kms *s = &p->stereo;
    if (!s->enabled || !s->initialized)
        return false;
    stereo_drain_retired(ctx);
    bool available = gbm_surface_has_free_buffers(p->gbm.surface) &&
                     gbm_surface_has_free_buffers(s->right_gbm);
    if (!available) {
        pthread_mutex_lock(&s->mutex);
        // A prior partial capture is never eligible for scanout.
        if (s->building.bo[0] || s->building.bo[1])
            stereo_release_pair(p, &s->building);
        // If necessary, drop an unpublished pending generation to free
        // buffers. Its BOs are not owned by KMS until the worker moves
        // the pair to active (under this same mutex).
        if ((!gbm_surface_has_free_buffers(p->gbm.surface) ||
             !gbm_surface_has_free_buffers(s->right_gbm)) &&
            s->pending.bo[0])
            stereo_release_pair(p, &s->pending);
        available = gbm_surface_has_free_buffers(p->gbm.surface) &&
                    gbm_surface_has_free_buffers(s->right_gbm);
        if (!available)
            ctx->vo->drm->stereo_pair_update_skips++;
        pthread_mutex_unlock(&s->mutex);
    }
    return available;
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
        struct gbm_surface *surface = eye ? s->right_gbm : p->gbm.surface;
        EGLSurface egl_surface = eye ? s->right_egl : p->egl.surface;
        if (!eglSwapBuffers(p->egl.display, egl_surface)) {
            MP_ERR(ctx, "EGL stereo eye %d swap failed\n", eye);
            return;
        }
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
            MP_ERR(ctx, "Could not lock stereo eye %d GBM front buffer\n", eye);
            return;
        }
        // Create the FB on the EGL thread, so the KMS worker only needs
        // immutable framebuffer IDs and does not call GBM or EGL.
        update_framebuffer_from_bo(ctx, bo);
        struct framebuffer *fb = gbm_bo_get_user_data(bo);
        if (!fb || !fb->id) {
            gbm_surface_release_buffer(surface, bo);
            return;
        }

        pthread_mutex_lock(&s->mutex);
        if (s->building.bo[eye]) {
            gbm_surface_release_buffer(surface, s->building.bo[eye]);
        }
        s->building.bo[eye] = bo;
        if (eye == 1 && s->building.bo[0] && s->building.bo[1]) {
            s->building.generation = ++s->next_generation;
            s->building.ready_ns = mp_time_ns();
            if (!s->active.bo[0]) {
                s->active = s->building;
            } else {
                // Superseded staging pairs were never submitted to KMS.
                // Do not release active or in-flight BOs here.
                if (s->pending.bo[0])
                    stereo_release_pair(p, &s->pending);
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

static bool drm_egl_init(struct ra_ctx *ctx)
{
    if (!vo_drm_init(ctx->vo))
        goto err;

    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    struct vo_drm_state *drm = ctx->vo->drm;
    const char *sbs = getenv("MPV_DRM_STEREO_SBS");
    const char *pair = getenv("MPV_DRM_STEREO_PAIR");
    p->stereo.enabled = sbs && pair &&
        strcmp(sbs, "1") == 0 && strcmp(pair, "1") == 0;
    const char *seq = getenv("MPV_DRM_STEREO_SEQ");
    p->stereo.sequence_mode = p->stereo.enabled && seq &&
                              strcmp(seq, "1") == 0;
    const char *rt = getenv("MPV_DRM_STEREO_RT");
    p->stereo.rt_mode = p->stereo.enabled && rt &&
                        strcmp(rt, "1") == 0;
    const char *gpu_wait = getenv("MPV_DRM_STEREO_GPU_WAIT");
    p->stereo.gpu_wait = p->stereo.enabled && gpu_wait &&
                         strcmp(gpu_wait, "1") == 0;
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
