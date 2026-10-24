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

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/sync_file.h>
#endif
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <drm_fourcc.h>

#include "config.h"

#if HAVE_CONSIO_H
#include <sys/consio.h>
#elif HAVE_VT_H
#include <sys/vt.h>
#elif HAVE_WSDISPLAY_USL_IO_H
#include <dev/wscons/wsdisplay_usl_io.h>
#endif

#include "drm_atomic.h"
#include "drm_common.h"

#include "common/common.h"
#include "common/msg.h"
#include "misc/ctype.h"
#include "options/m_config.h"
#include "options/path.h"
#include "osdep/io.h"
#include "osdep/poll_wrapper.h"
#include "osdep/timer.h"
#include "present_sync.h"
#include "video/out/vo.h"

#define EVT_RELEASE 1
#define EVT_ACQUIRE 2
#define HANDLER_ACQUIRE 0
#define HANDLER_RELEASE 1
#define RELEASE_SIGNAL SIGUSR1
#define ACQUIRE_SIGNAL SIGUSR2
#define MAX_CONNECTOR_NAME_LEN 20

#define DRM_PRIM_FACTOR 50000
#define DRM_MIN_LUMA_FACTOR 10000


/* Timing-critical stereo logging: publish immutable records with trylock.
 * Slow TTY/file sinks can only block the normal-priority logger, never KMS. */
#define DRM_STEREO_ASYNC_CAP 16

struct drm_stereo_stats_extra {
    double shown_hz, window_hz;
    uint64_t window_missed, window_parity, window_absolute;
    uint64_t window_hold, window_target_misses, window_discards;
    uint32_t msc;
};

struct drm_stereo_async_gap {
    unsigned skipped, msc, previous_msc;
    int old_eye, incoming_eye;
    double submit_callback_ms, prev_lag_ms, submit_ust_ms;
    int prev_lag_valid, submit_ust_valid;
    uint64_t pair_generation;
    int first_eye;
    double pair_age_ms, atomic_ms, commit_callback_ms;
};

enum drm_stereo_async_kind {
    DRM_STEREO_ASYNC_STATS,
    DRM_STEREO_ASYNC_GAP,
    DRM_STEREO_ASYNC_DROP,
    DRM_STEREO_ASYNC_GPU_PROBE,
};

struct drm_stereo_async_event {
    enum drm_stereo_async_kind kind;
    union {
        struct {
            // Copied only once per 120 confirmed flips, never dereferenced
            // as live DRM state. Its mp_log remains alive until thread join.
            struct vo_drm_state state;
            struct drm_stereo_stats_extra extra;
        } stats;
        struct drm_stereo_async_gap gap;
        struct drm_stereo_async_drop drop;
        struct drm_stereo_gpu_probe gpu_probe;
    };
};

struct drm_stereo_async_log {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t thread;
    bool stop;
    size_t head, tail, count;
    struct vo_drm_state *owner;
    atomic_uint_fast64_t dropped;
    struct drm_stereo_async_event events[DRM_STEREO_ASYNC_CAP];
};

// Success returns an event with the queue mutex locked; publish unlocks it.
static struct drm_stereo_async_event *stereo_async_reserve(struct vo_drm_state *drm)
{
    struct drm_stereo_async_log *q = drm->stereo_async_log;
    if (!q)
        return NULL;
    if (pthread_mutex_trylock(&q->mutex)) {
        atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
        return NULL;
    }
    if (q->stop || q->count == DRM_STEREO_ASYNC_CAP) {
        atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    return &q->events[q->tail];
}

static void stereo_async_publish(struct vo_drm_state *drm)
{
    struct drm_stereo_async_log *q = drm->stereo_async_log;
    q->tail = (q->tail + 1) % DRM_STEREO_ASYNC_CAP;
    ++q->count;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->mutex);
}

static void stereo_async_log_stats(struct vo_drm_state *drm,
                                   const struct drm_stereo_stats_extra *extra)
{
    struct drm_stereo_async_event *e = stereo_async_reserve(drm);
    if (!e)
        return;
    e->kind = DRM_STEREO_ASYNC_STATS;
    // Copy scalars and pointers only, not any live references. No formatting.
    e->stats.state = *drm;
    e->stats.extra = *extra;
    stereo_async_publish(drm);
}

static void stereo_async_log_gap(struct vo_drm_state *drm,
                                 const struct drm_stereo_async_gap *gap)
{
    struct drm_stereo_async_event *e = stereo_async_reserve(drm);
    if (!e)
        return;
    e->kind = DRM_STEREO_ASYNC_GAP;
    e->gap = *gap;
    stereo_async_publish(drm);
}

void vo_drm_stereo_log_drop(struct vo_drm_state *drm,
                             const struct drm_stereo_async_drop *drop)
{
    struct drm_stereo_async_event *e = stereo_async_reserve(drm);
    if (!e)
        return;
    e->kind = DRM_STEREO_ASYNC_DROP;
    e->drop = *drop;
    stereo_async_publish(drm);
}

void vo_drm_stereo_log_gpu_probe(struct vo_drm_state *drm,
                                  const struct drm_stereo_gpu_probe *probe)
{
    if (!drm->opts || !drm->opts->stereo_stats ||
        probe->fence_fd[0] <= 0 || probe->fence_fd[1] <= 0)
        return;
    struct drm_stereo_async_event *e = stereo_async_reserve(drm);
    if (!e)
        return;
    // The real KMS worker continues polling the original descriptors.
    // Only the normal-priority logger owns these dup'd descriptors.
    const int left = fcntl(probe->fence_fd[0], F_DUPFD_CLOEXEC, 3);
    const int right = fcntl(probe->fence_fd[1], F_DUPFD_CLOEXEC, 3);
    if (left < 0 || right < 0) {
        if (left >= 0) close(left);
        if (right >= 0) close(right);
        atomic_fetch_add_explicit(&drm->stereo_async_log->dropped,
                                  1, memory_order_relaxed);
        pthread_mutex_unlock(&drm->stereo_async_log->mutex);
        return;
    }
    e->kind = DRM_STEREO_ASYNC_GPU_PROBE;
    e->gpu_probe = *probe;
    e->gpu_probe.fence_fd[0] = left;
    e->gpu_probe.fence_fd[1] = right;
    stereo_async_publish(drm);
}

static void stereo_async_print_gpu_probe(struct vo_drm_state *drm,
                                         struct drm_stereo_gpu_probe *sample)
{
    struct pollfd pfd[2] = {
        {.fd = sample->fence_fd[0], .events = POLLIN},
        {.fd = sample->fence_fd[1], .events = POLLIN},
    };
    int64_t observed_ns[2] = {0, 0};
    int64_t kernel_ns[2] = {0, 0};
    // Bounded 100ms; never delay scanout. poll timestamps are upper
    // bounds on GPU completion, subject to logger scheduling latency.
    for (int attempt = 0; attempt < 10; attempt++) {
        if (pfd[0].fd < 0 && pfd[1].fd < 0)
            break;
        int rc = poll(pfd, 2, 10);
        if (rc <= 0)
            continue;
        int64_t now_ns = mp_time_ns();
        for (int eye = 0; eye < 2; eye++) {
            if (pfd[eye].fd < 0 ||
                !(pfd[eye].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
                continue;
            if (pfd[eye].revents & POLLIN) {
                observed_ns[eye] = now_ns;
#if defined(__linux__)
                // The kernel may expose the actual dma_fence signal
                // timestamp. Convert raw CLOCK_MONOTONIC time to mpv's
                // offset monotonic timeline before comparing to ready_ns.
                struct sync_file_info info = {0};
                if (!ioctl(pfd[eye].fd, SYNC_IOC_FILE_INFO, &info) &&
                    info.num_fences > 0 && info.num_fences <= 16) {
                    struct sync_fence_info fences[16] = {0};
                    info.sync_fence_info = (uintptr_t)fences;
                    if (!ioctl(pfd[eye].fd, SYNC_IOC_FILE_INFO, &info)) {
                        uint64_t latest_ns = 0;
                        for (unsigned n = 0; n < info.num_fences; n++)
                            if (fences[n].status == 1 &&
                                fences[n].timestamp_ns > latest_ns)
                                latest_ns = fences[n].timestamp_ns;
                        if (latest_ns) {
                            // dma_fence timestamps use CLOCK_MONOTONIC,
                            // whereas mpv prefers CLOCK_MONOTONIC_RAW.
                            // Subtracting mpv's RAW startup offset from
                            // a MONOTONIC timestamp introduces seconds of
                            // error on hosts disciplined by NTP. Bridge
                            // both time bases using a near-simultaneous
                            // sample and only then subtract ready_ns.
                            struct timespec mono = {0};
                            if (!clock_gettime(CLOCK_MONOTONIC, &mono)) {
                                int64_t mp_now_ns = mp_time_ns();
                                int64_t mono_now_ns =
                                    (int64_t)mono.tv_sec * 1000000000LL +
                                    mono.tv_nsec;
                                kernel_ns[eye] =
                                    (int64_t)latest_ns - mono_now_ns +
                                    mp_now_ns;
                            }
                        }
                    }
                }
#endif
            }
            pfd[eye].fd = -1;
        }
    }
    // Close even on timeout/error, exactly once, never the KMS-owned fd.
    close(sample->fence_fd[0]);
    close(sample->fence_fd[1]);
    double gpu_after_failed_L_ms = -1.0;
    if (sample->dropped_pending && sample->last_l_deferral_age_ns > 0 &&
        kernel_ns[1]) {
        gpu_after_failed_L_ms =
            ((kernel_ns[1] - sample->pair_published_ns) -
             sample->last_l_deferral_age_ns) / 1000000.0;
    }
    MP_INFO(drm, "Stereo GPU probe: generation=%llu frame_id=%llu "
            "kind=%s last_failed_L_target_msc=%u "
            "last_failed_L_age_ms=%.3f "
            "gpu_R_after_failed_L_ms=%.3f "
            "cpu_render_L_ms=%.3f cpu_render_R_ms=%.3f "
            "egl_swap_L_ms=%.3f egl_swap_R_ms=%.3f "
            "fence_export_L_ms=%.3f fence_export_R_ms=%.3f "
            "fence_observed_L_age_ms=%.3f fence_observed_R_age_ms=%.3f "
            "fence_kernel_L_age_ms=%.3f fence_kernel_R_age_ms=%.3f "
            "(age relative to published pair; negative kernel age is possible; "
            "-1 means unavailable)\n",
            (unsigned long long)sample->generation,
            (unsigned long long)sample->frame_id,
            sample->dropped_pending ? "discard" : "sample",
            sample->last_l_deferral_target_msc,
            sample->last_l_deferral_age_ns > 0
                ? sample->last_l_deferral_age_ns / 1000000.0 : -1.0,
            gpu_after_failed_L_ms,
            sample->render_ns[0] / 1000000.0,
            sample->render_ns[1] / 1000000.0,
            sample->egl_swap_ns[0] / 1000000.0,
            sample->egl_swap_ns[1] / 1000000.0,
            sample->fence_export_ns[0] / 1000000.0,
            sample->fence_export_ns[1] / 1000000.0,
            observed_ns[0] ? (observed_ns[0] - sample->pair_published_ns) / 1000000.0 : -1.0,
            observed_ns[1] ? (observed_ns[1] - sample->pair_published_ns) / 1000000.0 : -1.0,
            kernel_ns[0] ? (kernel_ns[0] - sample->pair_published_ns) / 1000000.0 : -1.0,
            kernel_ns[1] ? (kernel_ns[1] - sample->pair_published_ns) / 1000000.0 : -1.0);
}

static void stereo_async_print_stats(const struct drm_stereo_async_event *event)
{
    const struct vo_drm_state *drm = &event->stats.state;
    const struct drm_stereo_stats_extra *extra = &event->stats.extra;
    const double shown_hz = extra->shown_hz;
    const double window_hz = extra->window_hz;
    const uint64_t window_missed = extra->window_missed;
    const uint64_t window_parity = extra->window_parity;
    const uint64_t window_absolute = extra->window_absolute;
    const uint64_t window_hold = extra->window_hold;
    const uint64_t window_target_misses = extra->window_target_misses;
    const uint64_t window_discards = extra->window_discards;
    const unsigned msc = extra->msc;
    MP_INFO(drm, "DRM SBS test: flips=%llu shown_hz=%.3f "
            "window_hz=%.3f window_missed=%llu window_parity=%llu "
            "missed_vblanks=%llu parity_errors=%llu max_gap=%u "
            "wrong_eye=%llu window_wrong_eye=%llu "
            "wrong_hold=%llu window_wrong_hold=%llu "
            "recovery=%s corrections=%llu "
            "slot=%s pending=%u peak_pending=%u discards=%llu window_discards=%llu "
            "target_misses=%llu window_target_misses=%llu "
            "max_commit_callback_ms=%.3f max_draw_callback_ms=%.3f "
            "max_submit_after_callback_ms=%.3f "
            "max_atomic_ioctl_ms=%.3f "
            "ust_monotonic=%s negative_callback_lags=%llu "
            "min_callback_lag_ms=%.6f max_callback_lag_ms=%.3f "
            "min_submit_after_ust_ms=%.6f max_submit_after_ust_ms=%.3f "
            "pair=%s pair_gen=%llu pair_renders=%llu "
            "pair_reuse_flips=%llu pair_update_skips=%llu "
            "pair_unpresented_discards=%llu "
            "first_eye_L=%llu first_eye_R=%llu "
            "first_eye_L_misses=%llu first_eye_R_misses=%llu "
            "first_age_L_min_ms=%.3f first_age_L_avg_ms=%.3f "
            "first_age_L_max_ms=%.3f "
            "first_age_R_min_ms=%.3f first_age_R_avg_ms=%.3f "
            "first_age_R_max_ms=%.3f "
            "legacy=%s legacy_flips=%llu legacy_fallbacks=%llu "
            "pair_seq=%s pair_catchups=%llu "
            "gpu_wait=%s gpu_waits=%llu max_gpu_wait_ms=%.3f "
            "native_fence=%s fence_checks=%llu fence_promotions=%llu "
            "fence_deferrals=%llu fence_unready_L=%llu "
            "fence_unready_R=%llu fence_errors=%llu "
            "draws=%llu commit_errors=%llu msc=%u eye=%s "
            "(video output only; USB disabled)\n",
            (unsigned long long)drm->stereo_flips, shown_hz,
            window_hz,
            (unsigned long long)window_missed,
            (unsigned long long)window_parity,
            (unsigned long long)drm->stereo_missed_slots,
            (unsigned long long)drm->stereo_parity_errors,
            drm->stereo_max_gap,
            (unsigned long long)drm->stereo_absolute_eye_errors,
            (unsigned long long)window_absolute,
            (unsigned long long)drm->stereo_wrong_hold_slots,
            (unsigned long long)window_hold,
            drm->stereo_recover && !drm->stereo_slot_mode ? "on" : "off",
            (unsigned long long)drm->stereo_corrections,
            drm->stereo_slot_mode ? "on" : "off",
            drm->stereo_queue_ahead,
            drm->stereo_queue_peak,
            (unsigned long long)drm->stereo_queue_discards,
            (unsigned long long)window_discards,
            (unsigned long long)drm->stereo_target_misses,
            (unsigned long long)window_target_misses,
            drm->stereo_max_commit_callback_ns / 1000000.0,
            drm->stereo_max_draw_callback_ns / 1000000.0,
            drm->stereo_max_submit_after_callback_ns / 1000000.0,
            drm->stereo_max_commit_ioctl_ns / 1000000.0,
            drm->stereo_ust_monotonic ? "on" : "off",
            (unsigned long long)drm->stereo_negative_callback_lags,
            drm->stereo_min_callback_lag_ns / 1000000.0,
            drm->stereo_max_callback_lag_ns / 1000000.0,
            drm->stereo_min_submit_after_ust_ns / 1000000.0,
            drm->stereo_max_submit_after_ust_ns / 1000000.0,
            drm->stereo_pair_mode ? "on" : "off",
            (unsigned long long)drm->stereo_pair_display_generation,
            (unsigned long long)drm->stereo_pair_rendered,
            (unsigned long long)drm->stereo_pair_repeated_flips,
            (unsigned long long)drm->stereo_pair_update_skips,
            (unsigned long long)drm->stereo_pair_unpresented_discards,
            (unsigned long long)drm->stereo_pair_eye_first_flips[0],
            (unsigned long long)drm->stereo_pair_eye_first_flips[1],
            (unsigned long long)drm->stereo_pair_eye_first_misses[0],
            (unsigned long long)drm->stereo_pair_eye_first_misses[1],
            drm->stereo_pair_first_age_min_ns[0] / 1000000.0,
            drm->stereo_pair_eye_first_flips[0]
                ? drm->stereo_pair_first_age_sum_ns[0] /
                  (1000000.0 * drm->stereo_pair_eye_first_flips[0])
                : 0.0,
            drm->stereo_pair_first_age_max_ns[0] / 1000000.0,
            drm->stereo_pair_first_age_min_ns[1] / 1000000.0,
            drm->stereo_pair_eye_first_flips[1]
                ? drm->stereo_pair_first_age_sum_ns[1] /
                  (1000000.0 * drm->stereo_pair_eye_first_flips[1])
                : 0.0,
            drm->stereo_pair_first_age_max_ns[1] / 1000000.0,
            drm->stereo_pair_legacy_mode ? "on" : "off",
            (unsigned long long)drm->stereo_pair_legacy_flips,
            (unsigned long long)drm->stereo_pair_legacy_fallbacks,
            drm->stereo_pair_sequence_mode ? "on" : "off",
            (unsigned long long)drm->stereo_pair_sequence_catchups,
            drm->stereo_pair_gpu_wait ? "on" : "off",
            (unsigned long long)drm->stereo_pair_gpu_waits,
            drm->stereo_max_gpu_wait_ns / 1000000.0,
            drm->stereo_pair_native_fence ? "on" : "off",
            (unsigned long long)drm->stereo_pair_fence_checks,
            (unsigned long long)drm->stereo_pair_fence_promotions,
            (unsigned long long)drm->stereo_pair_fence_deferrals,
            (unsigned long long)drm->stereo_pair_fence_unready_eye[0],
            (unsigned long long)drm->stereo_pair_fence_unready_eye[1],
            (unsigned long long)drm->stereo_pair_fence_errors,
            (unsigned long long)drm->stereo_draws,
            (unsigned long long)drm->stereo_commit_errors, msc,
            drm->stereo_flip_eye ? "R" : "L");
}

static void stereo_async_print_gap(struct vo_drm_state *drm,
                                   const struct drm_stereo_async_gap *g)
{
    MP_WARN(drm, "DRM SBS gap: skipped=%u refreshes msc=%u "
            "previous_msc=%u old_eye=%s incoming_eye=%s "
            "submit_after_callback_ms=%.3f "
            "prev_callback_lag_ms=%.6f submit_after_ust_ms=%.6f "
            "prev_lag_valid=%d submit_ust_valid=%d "
            "pair_gen=%llu first_eye=%d pair_age_ms=%.3f "
            "atomic_ioctl_ms=%.3f commit_callback_ms=%.3f\n",
            g->skipped, g->msc, g->previous_msc,
            g->old_eye ? "R" : "L", g->incoming_eye ? "R" : "L",
            g->submit_callback_ms, g->prev_lag_ms, g->submit_ust_ms,
            g->prev_lag_valid, g->submit_ust_valid,
            (unsigned long long)g->pair_generation, g->first_eye,
            g->pair_age_ms, g->atomic_ms, g->commit_callback_ms);
}

static void stereo_async_print_drop(struct vo_drm_state *drm,
                                    const struct drm_stereo_async_drop *d)
{
    MP_INFO(drm, "Stereo unpresented pair discarded: "
            "reason=%s count=%llu pending_gen=%llu active_gen=%llu "
            "frame_id=%llu movie_pts_s=%.6f pts_valid=%d "
            "pending_age_ms=%.3f fence_L=%d fence_R=%d "
            "left_boundary_fence_deferrals=%u "
            "first_deferral_age_ms=%.3f last_deferral_age_ms=%.3f "
            "last_deferral_target_msc=%u "
            "intermediate_R_probe_age_ms=%.3f intermediate_R_probe_msc=%u "
            "intermediate_R_fence_L=%d intermediate_R_fence_R=%d "
            "(fence: 1=signaled 0=unsignaled -1=unavailable)\n",
            d->reason, (unsigned long long)d->count,
            (unsigned long long)d->pending_gen,
            (unsigned long long)d->active_gen,
            (unsigned long long)d->frame_id, d->media_pts, d->pts_valid,
            d->age_ms, d->fence_left, d->fence_right, d->deferrals,
            d->first_deferral_ms, d->last_deferral_ms,
            d->last_deferral_msc, d->r_probe_age_ms, d->r_probe_msc,
            d->r_probe_left, d->r_probe_right);
}

static void *stereo_async_log_worker(void *opaque)
{
    struct drm_stereo_async_log *q = opaque; // normal priority, never SCHED_FIFO
    for (;;) {
        pthread_mutex_lock(&q->mutex);
        while (!q->count && !q->stop)
            pthread_cond_wait(&q->ready, &q->mutex);
        if (!q->count && q->stop) {
            pthread_mutex_unlock(&q->mutex);
            break;
        }
        // Never hold this mutex during formatting, mpv logging, or I/O.
        struct drm_stereo_async_event event = q->events[q->head];
        q->head = (q->head + 1) % DRM_STEREO_ASYNC_CAP;
        --q->count;
        pthread_mutex_unlock(&q->mutex);

        switch (event.kind) {
        case DRM_STEREO_ASYNC_STATS: stereo_async_print_stats(&event); break;
        case DRM_STEREO_ASYNC_GAP: stereo_async_print_gap(q->owner, &event.gap); break;
        case DRM_STEREO_ASYNC_DROP: stereo_async_print_drop(q->owner, &event.drop); break;
        case DRM_STEREO_ASYNC_GPU_PROBE:
            stereo_async_print_gpu_probe(q->owner, &event.gpu_probe);
            break;
        }
    }
    return NULL;
}

static bool stereo_async_log_init(struct vo_drm_state *drm)
{
    struct drm_stereo_async_log *q = calloc(1, sizeof(*q));
    if (!q)
        return false;
    q->owner = drm;
    atomic_init(&q->dropped, 0);
    if (pthread_mutex_init(&q->mutex, NULL)) {
        free(q);
        return false;
    }
    if (pthread_cond_init(&q->ready, NULL)) {
        pthread_mutex_destroy(&q->mutex);
        free(q);
        return false;
    }
    if (pthread_create(&q->thread, NULL, stereo_async_log_worker, q)) {
        pthread_cond_destroy(&q->ready);
        pthread_mutex_destroy(&q->mutex);
        free(q);
        return false;
    }
    drm->stereo_async_log = q;
    return true;
}

static void stereo_async_log_uninit(struct vo_drm_state *drm)
{
    struct drm_stereo_async_log *q = drm->stereo_async_log;
    if (!q)
        return;
    // Called after the KMS worker has been joined; drain remaining events.
    pthread_mutex_lock(&q->mutex);
    q->stop = true;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->mutex);
    pthread_join(q->thread, NULL);
    uint64_t lost = atomic_load_explicit(&q->dropped, memory_order_relaxed);
    if (lost)
        MP_WARN(drm, "Stereo async logger: lost %llu diagnostic events "
                "(bounded queue or contention, scanout unaffected)\n",
                (unsigned long long)lost);
    drm->stereo_async_log = NULL;
    pthread_cond_destroy(&q->ready);
    pthread_mutex_destroy(&q->mutex);
    free(q);
}

static int vt_switcher_pipe[2];

static int drm_connector_opt_help(struct mp_log *log, const struct m_option *opt,
                                  struct bstr name);

static int drm_mode_opt_help(struct mp_log *log, const struct m_option *opt,
                             struct bstr name);

static OPT_STRING_VALIDATE_FUNC(drm_validate_mode_opt);

static void drm_show_available_modes(struct mp_log *log, const drmModeConnector *connector);

static void drm_show_available_connectors(struct mp_log *log, int card_no,
                                          const char *card_path);
static double mode_get_Hz(const drmModeModeInfo *mode);

#define OPT_BASE_STRUCT struct drm_opts
const struct m_sub_options drm_conf = {
    .opts = (const struct m_option[]) {
        {"drm-device", OPT_STRING(device_path), .flags = M_OPT_FILE},
        {"drm-connector", OPT_STRING(connector_spec),
            .help = drm_connector_opt_help},
        {"drm-mode", OPT_STRING_VALIDATE(mode_spec, drm_validate_mode_opt),
            .help = drm_mode_opt_help},
        {"drm-draw-plane", OPT_CHOICE(draw_plane,
            {"primary", DRM_OPTS_PRIMARY_PLANE},
            {"overlay", DRM_OPTS_OVERLAY_PLANE}),
            M_RANGE(0, INT_MAX)},
        {"drm-drmprime-video-plane", OPT_CHOICE(drmprime_video_plane,
            {"primary", DRM_OPTS_PRIMARY_PLANE},
            {"overlay", DRM_OPTS_OVERLAY_PLANE}),
            M_RANGE(0, INT_MAX)},
        {"drm-format", OPT_CHOICE(drm_format,
            {"xrgb8888",    DRM_OPTS_FORMAT_XRGB8888},
            {"xrgb2101010", DRM_OPTS_FORMAT_XRGB2101010},
            {"xbgr8888",    DRM_OPTS_FORMAT_XBGR8888},
            {"xbgr2101010", DRM_OPTS_FORMAT_XBGR2101010},
            {"rgb565",      DRM_OPTS_FORMAT_RGB565},
            {"yuyv",        DRM_OPTS_FORMAT_YUYV})},
        {"drm-draw-surface-size", OPT_SIZE_BOX(draw_surface_size)},
        {"drm-vrr-enabled", OPT_CHOICE(vrr_enabled,
            {"no", 0}, {"yes", 1}, {"auto", -1})},
        {"drm-stereo-3d", OPT_BOOL(stereo_3d)},
        {"drm-stereo-pacing-eye", OPT_CHOICE(stereo_pacing_eye,
            {"left", 0}, {"right", 1})},
        {"drm-stereo-stats", OPT_BOOL(stereo_stats)},
        {0},
    },
    .defaults = &(const struct drm_opts) {
        .mode_spec = "preferred",
        .draw_plane = DRM_OPTS_PRIMARY_PLANE,
        .drmprime_video_plane = DRM_OPTS_OVERLAY_PLANE,
        .drm_format = DRM_OPTS_FORMAT_XRGB8888,
        .stereo_stats = true,
    },
    .size = sizeof(struct drm_opts),
};

static const char *connector_names[] = {
    "Unknown",   // DRM_MODE_CONNECTOR_Unknown
    "VGA",       // DRM_MODE_CONNECTOR_VGA
    "DVI-I",     // DRM_MODE_CONNECTOR_DVII
    "DVI-D",     // DRM_MODE_CONNECTOR_DVID
    "DVI-A",     // DRM_MODE_CONNECTOR_DVIA
    "Composite", // DRM_MODE_CONNECTOR_Composite
    "SVIDEO",    // DRM_MODE_CONNECTOR_SVIDEO
    "LVDS",      // DRM_MODE_CONNECTOR_LVDS
    "Component", // DRM_MODE_CONNECTOR_Component
    "DIN",       // DRM_MODE_CONNECTOR_9PinDIN
    "DP",        // DRM_MODE_CONNECTOR_DisplayPort
    "HDMI-A",    // DRM_MODE_CONNECTOR_HDMIA
    "HDMI-B",    // DRM_MODE_CONNECTOR_HDMIB
    "TV",        // DRM_MODE_CONNECTOR_TV
    "eDP",       // DRM_MODE_CONNECTOR_eDP
    "Virtual",   // DRM_MODE_CONNECTOR_VIRTUAL
    "DSI",       // DRM_MODE_CONNECTOR_DSI
    "DPI",       // DRM_MODE_CONNECTOR_DPI
    "Writeback", // DRM_MODE_CONNECTOR_WRITEBACK
    "SPI",       // DRM_MODE_CONNECTOR_SPI
    "USB",       // DRM_MODE_CONNECTOR_USB
};

static int eotf_map[PL_COLOR_TRC_COUNT] = {
    [PL_COLOR_TRC_BT_1886] = HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
    [PL_COLOR_TRC_PQ] = HDMI_EOTF_SMPTE_ST2084,
    [PL_COLOR_TRC_HLG] = HDMI_EOTF_BT_2100_HLG,
};

struct drm_mode_spec {
    enum {
        DRM_MODE_SPEC_BY_IDX,     // Specified by idx
        DRM_MODE_SPEC_BY_NUMBERS, // Specified by width, height and opt. refresh
        DRM_MODE_SPEC_PREFERRED,  // Select the preferred mode of the display
        DRM_MODE_SPEC_HIGHEST,    // Select the mode with the highest resolution
    } type;
    unsigned int idx;
    unsigned int width;
    unsigned int height;
    double refresh;
};

/* VT Switcher */
static void vt_switcher_sighandler(int sig)
{
    int saved_errno = errno;
    unsigned char event = sig == RELEASE_SIGNAL ? EVT_RELEASE : EVT_ACQUIRE;
    (void)write(vt_switcher_pipe[1], &event, sizeof(event));
    errno = saved_errno;
}

static bool has_signal_installed(int signo)
{
    struct sigaction act = { 0 };
    sigaction(signo, 0, &act);
    return act.sa_handler != 0;
}

static int install_signal(int signo, void (*handler)(int))
{
    struct sigaction act = { 0 };
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    return sigaction(signo, &act, NULL);
}

static void release_vt(void *data)
{
    struct vo_drm_state *drm = data;
    MP_VERBOSE(drm, "Releasing VT\n");
    vo_drm_release_crtc(drm);
}

static void acquire_vt(void *data)
{
    struct vo_drm_state *drm = data;
    MP_VERBOSE(drm, "Acquiring VT\n");
    vo_drm_acquire_crtc(drm);
}

static void vt_switcher_acquire(struct vt_switcher *s,
                         void (*handler)(void*), void *user_data)
{
    s->handlers[HANDLER_ACQUIRE] = handler;
    s->handler_data[HANDLER_ACQUIRE] = user_data;
}

static void vt_switcher_release(struct vt_switcher *s,
                         void (*handler)(void*), void *user_data)
{
    s->handlers[HANDLER_RELEASE] = handler;
    s->handler_data[HANDLER_RELEASE] = user_data;
}

static bool vt_switcher_init(struct vt_switcher *s, struct mp_log *log)
{
    s->tty_fd = -1;
    s->log = log;
    vt_switcher_pipe[0] = -1;
    vt_switcher_pipe[1] = -1;

    if (mp_make_cloexec_pipe(vt_switcher_pipe)) {
        mp_err(log, "Creating pipe failed: %s\n", mp_strerror(errno));
        return false;
    }

    s->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (s->tty_fd < 0) {
        mp_err(log, "Can't open TTY for VT control: %s\n", mp_strerror(errno));
        return false;
    }

    if (has_signal_installed(RELEASE_SIGNAL)) {
        mp_err(log, "Can't handle VT release - signal already used\n");
        return false;
    }
    if (has_signal_installed(ACQUIRE_SIGNAL)) {
        mp_err(log, "Can't handle VT acquire - signal already used\n");
        return false;
    }

    if (install_signal(RELEASE_SIGNAL, vt_switcher_sighandler)) {
        mp_err(log, "Failed to install release signal: %s\n", mp_strerror(errno));
        return false;
    }
    if (install_signal(ACQUIRE_SIGNAL, vt_switcher_sighandler)) {
        mp_err(log, "Failed to install acquire signal: %s\n", mp_strerror(errno));
        return false;
    }

    struct vt_mode vt_mode = { 0 };
    if (ioctl(s->tty_fd, VT_GETMODE, &vt_mode) < 0) {
        mp_err(log, "VT_GETMODE failed: %s\n", mp_strerror(errno));
        return false;
    }

    vt_mode.mode = VT_PROCESS;
    vt_mode.relsig = RELEASE_SIGNAL;
    vt_mode.acqsig = ACQUIRE_SIGNAL;
    // frsig is a signal for forced release. Not implemented on Linux,
    // Solaris, BSDs but must be set to a valid signal on some of those.
    vt_mode.frsig = SIGIO; // unused
    if (ioctl(s->tty_fd, VT_SETMODE, &vt_mode) < 0) {
        mp_err(log, "VT_SETMODE failed: %s\n", mp_strerror(errno));
        return false;
    }

    // Block the VT switching signals from interrupting the VO thread (they will
    // still be picked up by other threads, which will fill vt_switcher_pipe for us)
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, RELEASE_SIGNAL);
    sigaddset(&set, ACQUIRE_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    return true;
}

static void vt_switcher_destroy(struct vt_switcher *s)
{
    struct vt_mode vt_mode = {0};
    vt_mode.mode = VT_AUTO;
    if (ioctl(s->tty_fd, VT_SETMODE, &vt_mode) < 0) {
        MP_ERR(s, "VT_SETMODE failed: %s\n", mp_strerror(errno));
        return;
    }

    install_signal(RELEASE_SIGNAL, SIG_DFL);
    install_signal(ACQUIRE_SIGNAL, SIG_DFL);
    close(s->tty_fd);
    close(vt_switcher_pipe[0]);
    close(vt_switcher_pipe[1]);
}

static void vt_switcher_handle_event(struct vt_switcher *s)
{
    unsigned char event;
    if (read(vt_switcher_pipe[0], &event, sizeof(event)) != sizeof(event))
        return;

    switch (event) {
    case EVT_RELEASE:
        s->handlers[HANDLER_RELEASE](s->handler_data[HANDLER_RELEASE]);
        if (ioctl(s->tty_fd, VT_RELDISP, 1) < 0) {
            MP_ERR(s, "Failed to release virtual terminal\n");
        }
        break;
    case EVT_ACQUIRE:
        s->handlers[HANDLER_ACQUIRE](s->handler_data[HANDLER_ACQUIRE]);
        if (ioctl(s->tty_fd, VT_RELDISP, VT_ACKACQ) < 0) {
            MP_ERR(s, "Failed to acquire virtual terminal\n");
        }
        break;
    }
}

bool vo_drm_acquire_crtc(struct vo_drm_state *drm)
{
    if (drm->active)
        return true;
    drm->active = true;

    if (drmSetMaster(drm->fd)) {
        MP_WARN(drm, "Failed to acquire DRM master: %s\n",
                mp_strerror(errno));
    }

    struct drm_atomic_context *atomic_ctx = drm->atomic_context;

    if (!drm_atomic_save_old_state(atomic_ctx))
        MP_WARN(drm, "Failed to save old DRM atomic state\n");

    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (!request) {
        MP_ERR(drm, "Failed to allocate drm atomic request\n");
        goto err;
    }

    if (drm_object_set_property(request, atomic_ctx->connector, "CRTC_ID", drm->crtc_id) < 0) {
        MP_ERR(drm, "Could not set CRTC_ID on connector\n");
        goto err;
    }

    if (!drm_mode_ensure_blob(drm->fd, &drm->mode)) {
        MP_ERR(drm, "Failed to create DRM mode blob\n");
        goto err;
    }
    if (drm_object_set_property(request, atomic_ctx->crtc, "MODE_ID", drm->mode.blob_id) < 0) {
        MP_ERR(drm, "Could not set MODE_ID on crtc\n");
        goto err;
    }
    if (drm_object_set_property(request, atomic_ctx->crtc, "ACTIVE", 1) < 0) {
        MP_ERR(drm, "Could not set ACTIVE on crtc\n");
        goto err;
    }

    /*
     * VRR related properties were added in kernel 5.0. We will not fail if we
     * cannot query or set the value, but we will log as appropriate.
     */
    uint64_t vrr_capable = 0;
    drm_object_get_property(atomic_ctx->connector, "VRR_CAPABLE", &vrr_capable);
    MP_VERBOSE(drm, "crtc is%s VRR capable\n", vrr_capable ? "" : " not");

    uint64_t vrr_requested = drm->opts->vrr_enabled;
    if (vrr_requested == 1 || (vrr_capable && vrr_requested == -1)) {
        if (drm_object_set_property(request, atomic_ctx->crtc, "VRR_ENABLED", 1) < 0) {
            MP_WARN(drm, "Could not enable VRR on crtc\n");
        } else {
            MP_VERBOSE(drm, "Enabled VRR on crtc\n");
        }
    }

    drm_object_set_property(request, atomic_ctx->draw_plane, "FB_ID",   drm->fb->id);
    drm_object_set_property(request, atomic_ctx->draw_plane, "CRTC_ID", drm->crtc_id);
    drm_object_set_property(request, atomic_ctx->draw_plane, "SRC_X",   0);
    drm_object_set_property(request, atomic_ctx->draw_plane, "SRC_Y",   0);
    drm_object_set_property(request, atomic_ctx->draw_plane, "SRC_W",   drm->width << 16);
    drm_object_set_property(request, atomic_ctx->draw_plane, "SRC_H",   drm->height << 16);
    drm_object_set_property(request, atomic_ctx->draw_plane, "CRTC_X",  0);
    drm_object_set_property(request, atomic_ctx->draw_plane, "CRTC_Y",  0);
    drm_object_set_property(request, atomic_ctx->draw_plane, "CRTC_W",  drm->mode.mode.hdisplay);
    drm_object_set_property(request, atomic_ctx->draw_plane, "CRTC_H",  drm->mode.mode.vdisplay);

    if (drmModeAtomicCommit(drm->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
        MP_ERR(drm, "Failed to commit ModeSetting atomic request: %s\n", mp_strerror(errno));
        goto err;
    }

    drmModeAtomicFree(request);
    return true;

err:
    drmModeAtomicFree(request);
    return false;
}


void vo_drm_release_crtc(struct vo_drm_state *drm)
{
    if (!drm->active)
        return;
    drm->active = false;

    if (!drm->atomic_context->old_state.saved)
        return;

    bool success = true;
    struct drm_atomic_context *atomic_ctx = drm->atomic_context;
    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (!request) {
        MP_ERR(drm, "Failed to allocate drm atomic request\n");
        success = false;
    }

    if (request && !drm_atomic_restore_old_state(request, atomic_ctx)) {
        MP_WARN(drm, "Got error while restoring old state\n");
        success = false;
    }

    if (request) {
        if (drmModeAtomicCommit(drm->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
            MP_WARN(drm, "Failed to commit ModeSetting atomic request: %s\n",
                    mp_strerror(errno));
            success = false;
        }
    }

    if (request)
        drmModeAtomicFree(request);

    if (!success)
        MP_ERR(drm, "Failed to restore previous mode\n");

    if (drmDropMaster(drm->fd)) {
        MP_WARN(drm, "Failed to drop DRM master: %s\n",
                mp_strerror(errno));
    }
}

/* libdrm */
static void destroy_hdr_blob(struct vo_drm_state *drm)
{
    if (drm->hdr.blob_id) {
        drmModeDestroyPropertyBlob(drm->fd, drm->hdr.blob_id);
        drm->hdr.blob_id = 0;
    }
}

static void get_connector_name(const drmModeConnector *connector,
                               char ret[MAX_CONNECTOR_NAME_LEN])
{
    const char *type_name;

    if (connector->connector_type < MP_ARRAY_SIZE(connector_names)) {
        type_name = connector_names[connector->connector_type];
    } else {
        type_name = "UNKNOWN";
    }

    snprintf(ret, MAX_CONNECTOR_NAME_LEN, "%s-%d", type_name,
             connector->connector_type_id);
}

// Gets the first connector whose name matches the input parameter.
// The returned connector may be disconnected.
// Result must be freed with drmModeFreeConnector.
static drmModeConnector *get_connector_by_name(const drmModeRes *res,
                                               const char *connector_name,
                                               int fd)
{
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *connector
            = drmModeGetConnector(fd, res->connectors[i]);
        if (!connector)
            continue;
        char other_connector_name[MAX_CONNECTOR_NAME_LEN];
        get_connector_name(connector, other_connector_name);
        if (!strcmp(connector_name, other_connector_name))
            return connector;
        drmModeFreeConnector(connector);
    }
    return NULL;
}

// Gets the first connected connector.
// Result must be freed with drmModeFreeConnector.
static drmModeConnector *get_first_connected_connector(const drmModeRes *res,
                                                       int fd)
{
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, res->connectors[i]);
        if (!connector)
            continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            return connector;
        }
        drmModeFreeConnector(connector);
    }
    return NULL;
}

static void restore_sdr(struct vo_drm_state *drm)
{
    struct drm_atomic_context *atomic_ctx = drm->atomic_context;
    if (!atomic_ctx)
        return;

    struct pl_color_space sdr = pl_color_space_srgb;
    vo_drm_set_color(drm->vo, &sdr);
    int ret = drmModeAtomicCommit(drm->fd, atomic_ctx->request, DRM_MODE_ATOMIC_ALLOW_MODESET, drm);
    if (ret)
        MP_VERBOSE(drm, "Failed to commit atomic request: %s\n", mp_strerror(ret));
}

static bool setup_connector(struct vo_drm_state *drm, const drmModeRes *res,
                            const char *connector_name)
{
    drmModeConnector *connector;

    if (connector_name && strcmp(connector_name, "") && strcmp(connector_name, "auto")) {
        connector = get_connector_by_name(res, connector_name, drm->fd);
        if (!connector) {
            MP_ERR(drm, "No connector with name %s found\n", connector_name);
            drm_show_available_connectors(drm->log, drm->card_no, drm->card_path);
            return false;
        }
    } else {
        connector = get_first_connected_connector(res, drm->fd);
        if (!connector) {
            MP_ERR(drm, "No connected connectors found\n");
            return false;
        }
    }

    if (connector->connection != DRM_MODE_CONNECTED) {
        drmModeFreeConnector(connector);
        MP_ERR(drm, "Chosen connector is disconnected\n");
        return false;
    }

    if (connector->count_modes == 0) {
        drmModeFreeConnector(connector);
        MP_ERR(drm, "Chosen connector has no valid modes\n");
        return false;
    }

    drm->connector = connector;
    return true;
}

static bool setup_crtc(struct vo_drm_state *drm, const drmModeRes *res)
{
    // First try to find currently connected encoder and its current CRTC
    for (unsigned int i = 0; i < res->count_encoders; i++) {
        drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, res->encoders[i]);
        if (!encoder) {
            MP_WARN(drm, "Cannot retrieve encoder %u:%u: %s\n",
                    i, res->encoders[i], mp_strerror(errno));
            continue;
        }

        if (encoder->encoder_id == drm->connector->encoder_id && encoder->crtc_id != 0) {
            MP_VERBOSE(drm, "Connector %u currently connected to encoder %u\n",
                       drm->connector->connector_id, drm->connector->encoder_id);
            drm->encoder = encoder;
            drm->crtc_id = encoder->crtc_id;
            goto success;
        }

        drmModeFreeEncoder(encoder);
    }

    // Otherwise pick first legal encoder and CRTC combo for the connector
    for (unsigned int i = 0; i < drm->connector->count_encoders; ++i) {
        drmModeEncoder *encoder
            = drmModeGetEncoder(drm->fd, drm->connector->encoders[i]);
        if (!encoder) {
            MP_WARN(drm, "Cannot retrieve encoder %u:%u: %s\n",
                    i, drm->connector->encoders[i], mp_strerror(errno));
            continue;
        }

        // iterate all global CRTCs
        for (unsigned int j = 0; j < res->count_crtcs; ++j) {
            // check whether this CRTC works with the encoder
            if (!(encoder->possible_crtcs & (1 << j)))
                continue;

            drm->encoder = encoder;
            drm->crtc_id = res->crtcs[j];
            goto success;
        }

        drmModeFreeEncoder(encoder);
    }

    MP_ERR(drm, "Connector %u has no suitable CRTC\n",
           drm->connector->connector_id);
    return false;

success:
    MP_VERBOSE(drm, "Selected Encoder %u with CRTC %u\n",
               drm->encoder->encoder_id, drm->crtc_id);
    return true;
}

static void setup_edid(struct vo_drm_state *drm)
{
    drmModePropertyBlobRes *blob = NULL;
    for (int i = 0; i < drm->connector->count_props; ++i) {
        drmModePropertyRes *prop = drmModeGetProperty(drm->fd, drm->connector->props[i]);
        if (prop && strcmp(prop->name, "EDID") == 0)
            blob = drmModeGetPropertyBlob(drm->fd, drm->connector->prop_values[i]);
        drmModeFreeProperty(prop);
        if (blob)
            break;
    }
    if (!blob) {
        MP_VERBOSE(drm, "Unable to get EDID blob from connector.\n");
        return;
    }

    drm->info = di_info_parse_edid(blob->data, blob->length);
    if (!drm->info) {
        MP_VERBOSE(drm, "Failed to parse EDID info: %s\n", mp_strerror(errno));
        goto done;
    }

    const struct di_edid *edid = di_info_get_edid(drm->info);
    if (!edid) {
        MP_VERBOSE(drm, "Failed to get EDID info: %s\n", mp_strerror(errno));
        goto done;
    }

    drm->chromaticity = di_edid_get_chromaticity_coords(edid);

    const struct di_edid_cta *cta = NULL;
    const struct di_edid_ext *const *exts = di_edid_get_extensions(edid);
    while (*exts && !(cta = di_edid_ext_get_cta(*exts++)));

    if (!cta) {
        MP_VERBOSE(drm, "Unable to find CTA-861 extension block.\n");
        goto done;
    }

    const struct di_cta_data_block *const *blocks = di_edid_cta_get_data_blocks(cta);
    for (int i = 0; *blocks && blocks[i]; ++i) {
        if (!drm->colorimetry)
            drm->colorimetry = di_cta_data_block_get_colorimetry(blocks[i]);
        if (!drm->hdr_static_metadata)
            drm->hdr_static_metadata = di_cta_data_block_get_hdr_static_metadata(blocks[i]);
        if (drm->colorimetry && drm->hdr_static_metadata)
            break;
    }

done:
    if (blob)
        drmModeFreePropertyBlob(blob);
}

static bool all_digits(const char *str)
{
    if (str == NULL || str[0] == '\0') {
        return false;
    }

    for (const char *c = str; *c != '\0'; ++c) {
        if (!mp_isdigit(*c))
            return false;
    }
    return true;
}

static bool parse_mode_spec(const char *spec, struct drm_mode_spec *parse_result)
{
    if (spec == NULL || spec[0] == '\0' || strcmp(spec, "preferred") == 0) {
        if (parse_result) {
            *parse_result =
                (struct drm_mode_spec) { .type = DRM_MODE_SPEC_PREFERRED };
        }
        return true;
    }

    if (strcmp(spec, "highest") == 0) {
        if (parse_result) {
            *parse_result =
                (struct drm_mode_spec) { .type = DRM_MODE_SPEC_HIGHEST };
        }
        return true;
    }

    // If the string is made up of only digits, it means that it is an index number
    if (all_digits(spec)) {
        if (parse_result) {
            *parse_result = (struct drm_mode_spec) {
                .type = DRM_MODE_SPEC_BY_IDX,
                .idx = strtoul(spec, NULL, 10),
            };
        }
        return true;
    }

    if (!mp_isdigit(spec[0]))
        return false;
    char *height_part, *refresh_part;
    const unsigned int width = strtoul(spec, &height_part, 10);
    if (spec == height_part || height_part[0] == '\0' || height_part[0] != 'x')
        return false;

    height_part += 1;
    if (!mp_isdigit(height_part[0]))
        return false;
    const unsigned int height = strtoul(height_part, &refresh_part, 10);
    if (height_part == refresh_part)
        return false;

    char *rest = NULL;
    double refresh;
    switch (refresh_part[0]) {
    case '\0':
        refresh = nan("");
        break;
    case '@':
        refresh_part += 1;
        if (!(mp_isdigit(refresh_part[0]) || refresh_part[0] == '.'))
            return false;
        refresh = strtod(refresh_part, &rest);
        if (refresh_part == rest || rest[0] != '\0' || refresh < 0.0)
            return false;
        break;
    default:
        return false;
    }

    if (parse_result) {
        *parse_result = (struct drm_mode_spec) {
            .type = DRM_MODE_SPEC_BY_NUMBERS,
            .width = width,
            .height = height,
            .refresh = refresh,
        };
    }
    return true;
}

static bool setup_mode_by_idx(struct vo_drm_state *drm, unsigned int mode_idx)
{
    if (mode_idx >= drm->connector->count_modes) {
        MP_ERR(drm, "Bad mode index (max = %d).\n",
               drm->connector->count_modes - 1);
        return false;
    }

    drm->mode.mode = drm->connector->modes[mode_idx];
    return true;
}

static bool mode_match(const drmModeModeInfo *mode,
                       unsigned int width,
                       unsigned int height,
                       double refresh)
{
    if (isnan(refresh)) {
        return
            (mode->hdisplay == width) &&
            (mode->vdisplay == height);
    } else {
        const double mode_refresh = mode_get_Hz(mode);
        return
            (mode->hdisplay == width) &&
            (mode->vdisplay == height) &&
            ((int)round(refresh*100) == (int)round(mode_refresh*100));
    }
}

static bool setup_mode_by_numbers(struct vo_drm_state *drm,
                                  unsigned int width,
                                  unsigned int height,
                                  double refresh)
{
    for (unsigned int i = 0; i < drm->connector->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm->connector->modes[i];
        if (mode_match(current_mode, width, height, refresh)) {
            drm->mode.mode = *current_mode;
            return true;
        }
    }

    MP_ERR(drm, "Could not find mode matching %s\n", drm->opts->mode_spec);
    return false;
}

static bool setup_mode_preferred(struct vo_drm_state *drm)
{
    for (unsigned int i = 0; i < drm->connector->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm->connector->modes[i];
        if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
            drm->mode.mode = *current_mode;
            return true;
        }
    }

    // Fall back to first mode
    MP_WARN(drm, "Could not find any preferred mode. Picking the first mode.\n");
    drm->mode.mode = drm->connector->modes[0];
    return true;
}

static bool setup_mode_highest(struct vo_drm_state *drm)
{
    unsigned int area = 0;
    drmModeModeInfo *highest_resolution_mode = &drm->connector->modes[0];
    for (unsigned int i = 0; i < drm->connector->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm->connector->modes[i];

        const unsigned int current_area =
            current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            highest_resolution_mode = current_mode;
            area = current_area;
        }
    }

    drm->mode.mode = *highest_resolution_mode;
    return true;
}

static bool setup_mode(struct vo_drm_state *drm)
{
    if (drm->connector->count_modes <= 0) {
        MP_ERR(drm, "No available modes\n");
        return false;
    }

    struct drm_mode_spec parsed;
    if (!parse_mode_spec(drm->opts->mode_spec, &parsed)) {
        MP_ERR(drm, "Parse error\n");
        goto err;
    }

    switch (parsed.type) {
    case DRM_MODE_SPEC_BY_IDX:
        if (!setup_mode_by_idx(drm, parsed.idx))
            goto err;
        break;
    case DRM_MODE_SPEC_BY_NUMBERS:
        if (!setup_mode_by_numbers(drm, parsed.width, parsed.height, parsed.refresh))
            goto err;
        break;
    case DRM_MODE_SPEC_PREFERRED:
        if (!setup_mode_preferred(drm))
            goto err;
        break;
    case DRM_MODE_SPEC_HIGHEST:
        if (!setup_mode_highest(drm))
            goto err;
        break;
    default:
        MP_ERR(drm, "setup_mode: Internal error\n");
        goto err;
    }

    drmModeModeInfo *mode = &drm->mode.mode;
    MP_VERBOSE(drm, "Selected mode: %s (%dx%d@%.2fHz)\n",
        mode->name, mode->hdisplay, mode->vdisplay, mode_get_Hz(mode));

    return true;

err:
    MP_INFO(drm, "Available modes:\n");
    drm_show_available_modes(drm->log, drm->connector);
    return false;
}

static int open_card_path(const char *path)
{
    return open(path, O_RDWR | O_CLOEXEC);
}

static bool card_supports_kms(const char *path)
{
    int fd = open_card_path(path);
    bool ret = fd != -1 && drmIsKMS(fd);
    if (fd != -1)
        close(fd);
    return ret;
}

static bool card_has_connection(const char *path)
{
    int fd = open_card_path(path);
    bool ret = false;
    if (fd != -1) {
        drmModeRes *res = drmModeGetResources(fd);
        if (res) {
            drmModeConnector *connector = get_first_connected_connector(res, fd);
            if (connector)
                ret = true;
            drmModeFreeConnector(connector);
            drmModeFreeResources(res);
        }
        close(fd);
    }
    return ret;
}

static void get_primary_device_path(struct vo_drm_state *drm)
{
    if (drm->opts->device_path) {
        drm->card_path = mp_get_user_path(drm, drm->vo->global, drm->opts->device_path);
        return;
    }

    drmDevice *devices[DRM_MAX_MINOR] = { 0 };
    int card_count = drmGetDevices2(0, devices, MP_ARRAY_SIZE(devices));
    bool card_no_given = drm->card_no >= 0;

    if (card_count < 0) {
        MP_ERR(drm, "Listing DRM devices with drmGetDevices failed! (%s)\n",
               mp_strerror(errno));
        goto err;
    }

    if (card_no_given && drm->card_no > (card_count - 1)) {
        MP_ERR(drm, "Card number %d given too high! %d devices located.\n",
               drm->card_no, card_count);
        goto err;
    }

    for (int i = card_no_given ? drm->card_no : 0; i < card_count; i++) {
        drmDevice *dev = devices[i];

        if (!(dev->available_nodes & (1 << DRM_NODE_PRIMARY))) {
            if (card_no_given) {
                MP_ERR(drm, "DRM card number %d given, but it does not have "
                       "a primary node!\n", i);
                break;
            }

            continue;
        }

        const char *card_path = dev->nodes[DRM_NODE_PRIMARY];

        if (!card_supports_kms(card_path)) {
            if (card_no_given) {
                MP_ERR(drm,
                       "DRM card number %d given, but it does not support "
                       "KMS!\n", i);
                break;
            }

            continue;
        }

        if (!card_has_connection(card_path)) {
            if (card_no_given) {
                MP_ERR(drm,
                        "DRM card number %d given, but it does not have any "
                        "connected outputs.\n", i);
                break;
            }

            continue;
        }

        MP_VERBOSE(drm, "Picked DRM card %d, primary node %s%s.\n",
                   i, card_path,
                   card_no_given ? "" : " as the default");

        drm->card_path = talloc_strdup(drm, card_path);
        drm->card_no = i;
        break;
    }

    if (!drm->card_path)
        MP_ERR(drm, "No primary DRM device could be picked!\n");

err:
    drmFreeDevices(devices, card_count);
}

static void drm_pflip_cb(int fd, unsigned int msc, unsigned int sec,
                         unsigned int usec, void *data)
{
    struct vo_drm_state *drm = data;

    int64_t ust = MP_TIME_S_TO_NS(sec) + MP_TIME_US_TO_NS(usec);
    if (drm->stereo_test_enabled && drm->stereo_flip_eye >= 0) {
        // For DRM devices reporting MONOTONIC vblank UST, measure how
        // far after the *physical scanout timestamp* this callback runs.
        // Do not compare UST to mp_time_ns(), which uses an offset raw
        // monotonic clock in mpv and has a different time origin.
        drm->stereo_callback_lag_valid = false;
        if (drm->stereo_pair_mode && drm->stereo_ust_monotonic) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                int64_t cpu_ns = (int64_t)now.tv_sec * 1000000000LL +
                                 now.tv_nsec;
                int64_t lag_ns = cpu_ns - ust;
                // Preserve signed differences. The old -1ns sentinel
                // hid the previous callback's timing on every gap.
                drm->stereo_last_callback_lag_ns = lag_ns;
                drm->stereo_callback_lag_valid = true;
                if (lag_ns < 0)
                    drm->stereo_negative_callback_lags++;
                drm->stereo_min_callback_lag_ns =
                    MPMIN(drm->stereo_min_callback_lag_ns, lag_ns);
                drm->stereo_max_callback_lag_ns =
                    MPMAX(drm->stereo_max_callback_lag_ns, lag_ns);
            }
        }
        // This callback confirms a real KMS flip. The timestamp is NOT
        // the time when our draw_frame() callback was invoked.
        const uint64_t when_us = (uint64_t)sec * 1000000u + usec;
        if (drm->stereo_have_flip) {
            const uint32_t steps = msc - drm->stereo_last_msc;
            if (steps > drm->stereo_max_gap)
                drm->stereo_max_gap = steps;
            if (steps > 1 && steps < 1000) {
                drm->stereo_missed_slots += steps - 1;
                if (drm->stereo_pair_mode && drm->stereo_flip_eye >= 0 &&
                    drm->stereo_flip_eye < 2 &&
                    drm->stereo_pair_first_eye_in_generation)
                    drm->stereo_pair_eye_first_misses[drm->stereo_flip_eye] +=
                        steps - 1;
                // Scanout retained the previous eye for every skipped
                // physical slot. Count mismatches against the absolute
                // alternating-eye timeline.
                for (uint32_t n = 1; n < steps; ++n) {
                    const int expected = drm->stereo_base_eye ^
                            ((msc - steps + n - drm->stereo_base_msc) & 1u);
                    if (drm->stereo_last_eye != expected)
                        ++drm->stereo_wrong_hold_slots;
                }
                // Optional and explicitly experimental: future software
                // renders compensate an odd number of skipped scanouts.
                // The already-queued flip cannot be corrected here.
                if (drm->stereo_recover && !drm->stereo_slot_mode &&
                    ((steps - 1) & 1u)) {
                    drm->stereo_phase_correction ^= 1u;
                    ++drm->stereo_corrections;
                }
                int64_t gap_callback_ns = mp_time_ns();
                const double commit_callback_ms =
                    drm->stereo_commit_time_ns > 0
                        ? (gap_callback_ns - drm->stereo_commit_time_ns) / 1000000.0
                        : -1.0;
                stereo_async_log_gap(drm, &(struct drm_stereo_async_gap) {
                    .skipped = steps - 1, .msc = msc,
                    .previous_msc = drm->stereo_last_msc,
                    .old_eye = drm->stereo_last_eye,
                    .incoming_eye = drm->stereo_flip_eye,
                    .submit_callback_ms = drm->stereo_commit_after_callback_ns / 1000000.0,
                    .prev_lag_ms = drm->stereo_prev_callback_lag_valid
                        ? drm->stereo_prev_callback_lag_ns / 1000000.0 : NAN,
                    .submit_ust_ms = drm->stereo_submit_after_ust_valid
                        ? drm->stereo_submit_after_ust_ns / 1000000.0 : NAN,
                    .prev_lag_valid = drm->stereo_prev_callback_lag_valid,
                    .submit_ust_valid = drm->stereo_submit_after_ust_valid,
                    .pair_generation = drm->stereo_pair_display_generation,
                    .first_eye = drm->stereo_pair_first_eye_in_generation,
                    .pair_age_ms = drm->stereo_pair_age_at_submit_ns / 1000000.0,
                    .atomic_ms = drm->stereo_commit_ioctl_ns / 1000000.0,
                    .commit_callback_ms = commit_callback_ms,
                });
            }
            if (steps == 0 || steps >= 1000 ||
                    (drm->stereo_flip_eye ^ drm->stereo_last_eye) != (steps & 1))
                ++drm->stereo_parity_errors;
        } else {
            drm->stereo_window_first_ust_us = when_us;
            drm->stereo_base_msc = msc;
            drm->stereo_base_eye = drm->stereo_flip_eye;
        }
        const int expected_eye = drm->stereo_base_eye ^
                                ((msc - drm->stereo_base_msc) & 1u);
        if (drm->stereo_flip_eye != expected_eye)
            ++drm->stereo_absolute_eye_errors;

        if (drm->stereo_slot_mode || drm->stereo_pair_mode) {
            if (drm->stereo_commit_target_valid &&
                drm->stereo_commit_target_msc != msc)
                ++drm->stereo_target_misses;
            // mp_time_ns is sampled when the callback is delivered, not
            // at scanout. Keep these CPU-side delays distinct from DRM UST.
            int64_t delivered_ns = mp_time_ns();
            if (drm->stereo_commit_time_ns > 0 &&
                delivered_ns > drm->stereo_commit_time_ns) {
                int64_t delay = delivered_ns - drm->stereo_commit_time_ns;
                drm->stereo_max_commit_callback_ns =
                    MPMAX(drm->stereo_max_commit_callback_ns, delay);
            }
            if (drm->stereo_commit_draw_time_ns > 0 &&
                delivered_ns > drm->stereo_commit_draw_time_ns) {
                int64_t delay = delivered_ns - drm->stereo_commit_draw_time_ns;
                drm->stereo_max_draw_callback_ns =
                    MPMAX(drm->stereo_max_draw_callback_ns, delay);
            }
        }
        drm->stereo_have_flip = true;
        drm->stereo_last_eye = drm->stereo_flip_eye;
        drm->stereo_last_msc = msc;
        drm->stereo_last_ust_us = when_us;
        if (drm->stereo_pair_mode && drm->stereo_flip_eye == 0)
            drm->stereo_last_left_ust_us = when_us;
        ++drm->stereo_flips;

        // Map the *actual confirmed* first LEFT scanout of each pair back
        // to the mpv vo_frame ID and movie PTS copied on atomic submit.
        // The DRM callback runs serially inside the 120Hz KMS worker, with
        // the stereo mutex held. No per-frame logging or allocations here.
        // Keep media PTS seconds and DRM CLOCK_MONOTONIC UST microseconds
        // separate: they are different clocks and cannot be subtracted
        // directly without anchoring to a first confirmed observation.
        if (drm->stereo_pair_mode && drm->stereo_flip_eye == 0 &&
            drm->stereo_pair_first_eye_in_generation) {
            drm->stereo_pts_first_L++;
            if (!drm->stereo_submit_pts_valid ||
                !drm->stereo_submit_frame_id ||
                !isfinite(drm->stereo_submit_media_pts)) {
                drm->stereo_pts_missing_L++;
            } else {
                const double pts = drm->stereo_submit_media_pts;
                const uint64_t id = drm->stereo_submit_frame_id;
                const int plan = drm->stereo_submit_planned_vsyncs;
                const bool synced = drm->stereo_submit_display_synced;

                // The schedule attached to this frame describes the frame's
                // future hold. Its accuracy can only be assessed when the
                // NEXT source frame reaches its first confirmed L. Compare
                // the last presented frame's original plan with this
                // first-L-to-first-L delta, never this frame's new plan.
                if (synced) {
                    if (plan >= 0)
                        drm->stereo_plan_requested_hist[MPMIN(plan, 4)]++;
                    else
                        drm->stereo_plan_invalid++;
                } else {
                    drm->stereo_plan_nondisplay++;
                }

                uint32_t cycles = 0;
                uint64_t frame_id_gap = 0;
                int previous_plan = -1;
                bool plan_comparable = false;
                bool reset = !drm->stereo_pts_valid_L;
                if (!reset) {
                    // Seek/reconfig or source PTS discontinuity: start a
                    // new relative anchor, never accumulate stale drift or
                    // compare a prior picture's hold across that boundary.
                    if (id <= drm->stereo_pts_last_frame_id ||
                        pts < drm->stereo_pts_last_media_pts - 0.100 ||
                        pts > drm->stereo_pts_last_media_pts + 5.0) {
                        reset = true;
                    } else {
                        uint32_t delta_msc = msc - drm->stereo_pts_last_msc;
                        bool valid_msc = delta_msc > 0 && delta_msc < 1000 &&
                                         (delta_msc & 1u) == 0;
                        if (valid_msc) {
                            cycles = delta_msc / 2u;
                            if (cycles == 1)
                                drm->stereo_pts_cycles_1++;
                            else if (cycles == 2)
                                drm->stereo_pts_cycles_2++;
                            else if (cycles == 3)
                                drm->stereo_pts_cycles_3++;
                            else if (cycles >= 4)
                                drm->stereo_pts_cycles_4plus++;
                        } else {
                            drm->stereo_plan_invalid_msc++;
                        }
                        if (id > drm->stereo_pts_last_frame_id + 1) {
                            frame_id_gap = id - drm->stereo_pts_last_frame_id - 1;
                            drm->stereo_pts_id_gaps += frame_id_gap;
                            drm->stereo_plan_skipped_intervals++;
                            drm->stereo_plan_skipped_ids += frame_id_gap;
                        }

                        // A multi-frame interval is NOT an accurate hold
                        // measurement of the preceding frame, as the missing
                        // pictures' requested durations are unknown here.
                        // This conditional comparison intentionally excludes
                        // those intervals and any invalid schedule metadata.
                        plan_comparable = valid_msc && !frame_id_gap &&
                            synced && drm->stereo_pts_last_display_synced &&
                            drm->stereo_pts_last_planned_vsyncs >= 0;
                        if (plan_comparable) {
                            previous_plan = drm->stereo_pts_last_planned_vsyncs;
                            drm->stereo_plan_comparisons++;
                            if (cycles == previous_plan)
                                drm->stereo_plan_exact++;
                            int64_t difference = (int64_t)cycles -
                                                 previous_plan;
                            drm->stereo_plan_signed_error_cycles += difference;
                            drm->stereo_plan_abs_error_cycles +=
                                difference < 0 ? -difference : difference;
                            drm->stereo_plan_matrix[MPMIN(previous_plan, 4)]
                                                   [MPMIN(cycles, 4u)]++;
                        }
                    }
                }
                if (reset) {
                    if (drm->stereo_pts_valid_L)
                        drm->stereo_pts_discontinuities++;
                    drm->stereo_pts_segments++;
                    drm->stereo_pts_segment_anchor_ust_us = when_us;
                    drm->stereo_pts_segment_anchor_media_pts = pts;
                    cycles = 0;
                }

                // This is relative display-time MINUS media-PTS elapsed,
                // not a measurement of absolute audio-to-photon latency.
                // --video-sync=display-resample may intentionally change
                // playback speed, so long-run drift must be interpreted
                // together with the player speed and A/V statistics.
                const double relative_ms =
                    ((double)((int64_t)when_us -
                        (int64_t)drm->stereo_pts_segment_anchor_ust_us) /
                        1000.0) -
                    (pts - drm->stereo_pts_segment_anchor_media_pts) * 1000.0;

                if (!drm->stereo_pts_valid_L) {
                    drm->stereo_pts_first_ust_us = when_us;
                    drm->stereo_pts_first_media_pts = pts;
                    drm->stereo_pts_drift_min_ms = relative_ms;
                    drm->stereo_pts_drift_max_ms = relative_ms;
                } else {
                    drm->stereo_pts_drift_min_ms =
                        MPMIN(drm->stereo_pts_drift_min_ms, relative_ms);
                    drm->stereo_pts_drift_max_ms =
                        MPMAX(drm->stereo_pts_drift_max_ms, relative_ms);
                }
                drm->stereo_pts_valid_L++;
                drm->stereo_pts_drift_sum_ms += relative_ms;
                drm->stereo_pts_last_drift_ms = relative_ms;
                drm->stereo_pts_last_ust_us = when_us;
                drm->stereo_pts_last_media_pts = pts;
                drm->stereo_pts_last_frame_id = id;
                drm->stereo_pts_last_msc = msc;
                drm->stereo_pts_last_planned_vsyncs = plan;
                drm->stereo_pts_last_display_synced = synced;

                // Bounded history: first 8 frames and one per 120 movie
                // pictures (~5 seconds at 23.976 FPS), plus a segment
                // restart. For 15 minutes this yields ~190 samples.
                // Store scalars only; print them after pthread_join().
                if (drm->stereo_pts_valid_L <= 8 ||
                    drm->stereo_pts_valid_L % 120 == 0 || reset) {
                    uint64_t pos = drm->stereo_pts_trace_total++ %
                                   DRM_STEREO_PTS_TRACE_CAP;
                    drm->stereo_pts_trace[pos] =
                        (struct drm_stereo_pts_sample) {
                            .frame_id = id,
                            .generation = drm->stereo_pair_display_generation,
                            .msc = msc,
                            .ust_us = when_us,
                            .media_pts = pts,
                            .relative_drift_ms = relative_ms,
                            .cycles_since_previous = cycles,
                            .planned_vsyncs = plan,
                            .display_synced = synced,
                            .previous_planned_vsyncs = previous_plan,
                            .frame_id_gap = frame_id_gap,
                            .plan_comparable = plan_comparable,
                        };
                }
            }
        }
        if (drm->stereo_flips % 120 == 0) {
            const uint64_t elapsed_us = when_us - drm->stereo_window_first_ust_us;
            const double shown_hz = elapsed_us > 0
                ? (drm->stereo_flips - 1) * 1000000.0 / elapsed_us : 0.0;
            const uint64_t since_us = when_us - (drm->stereo_last_report_ust_us
                         ? drm->stereo_last_report_ust_us
                         : drm->stereo_window_first_ust_us);
            const unsigned window_flips = drm->stereo_last_report_ust_us ? 120 : 119;
            const double window_hz = since_us > 0
                ? window_flips * 1000000.0 / since_us : 0.0;
            const uint64_t window_missed = drm->stereo_missed_slots -
                                          drm->stereo_last_report_missed;
            const uint64_t window_parity = drm->stereo_parity_errors -
                                          drm->stereo_last_report_parity;
            const uint64_t window_absolute = drm->stereo_absolute_eye_errors -
                                             drm->stereo_last_report_absolute;
            const uint64_t window_hold = drm->stereo_wrong_hold_slots -
                                         drm->stereo_last_report_hold;
            const uint64_t window_target_misses = drm->stereo_target_misses -
                                                  drm->stereo_last_report_target_misses;
            const uint64_t window_discards = drm->stereo_queue_discards -
                                             drm->stereo_last_report_queue_discards;
            if (drm->opts && drm->opts->stereo_stats) {
                stereo_async_log_stats(drm, &(struct drm_stereo_stats_extra) {
                    .shown_hz = shown_hz,
                    .window_hz = window_hz,
                    .window_missed = window_missed,
                    .window_parity = window_parity,
                    .window_absolute = window_absolute,
                    .window_hold = window_hold,
                    .window_target_misses = window_target_misses,
                    .window_discards = window_discards,
                    .msc = msc,
                });
            }
            drm->stereo_last_report_ust_us = when_us;
            drm->stereo_last_report_missed = drm->stereo_missed_slots;
            drm->stereo_last_report_parity = drm->stereo_parity_errors;
            drm->stereo_last_report_absolute = drm->stereo_absolute_eye_errors;
            drm->stereo_last_report_hold = drm->stereo_wrong_hold_slots;
            drm->stereo_last_report_target_misses = drm->stereo_target_misses;
            drm->stereo_last_report_queue_discards = drm->stereo_queue_discards;
            drm->stereo_max_commit_callback_ns = 0;
            drm->stereo_max_draw_callback_ns = 0;
            drm->stereo_max_submit_after_callback_ns = 0;
            drm->stereo_max_commit_ioctl_ns = 0;
            drm->stereo_min_callback_lag_ns = 0;
            drm->stereo_max_callback_lag_ns = 0;
            drm->stereo_min_submit_after_ust_ns = 0;
            drm->stereo_max_submit_after_ust_ns = 0;
            drm->stereo_max_gpu_wait_ns = 0;
            drm->stereo_queue_peak = 0;
            drm->stereo_max_gap = 0;
        }
    }
    // The physical KMS clock always runs at 120Hz, but mpv sees one
    // complete L/R cycle as a single 60Hz logical display refresh.
    // Normally VO wakeup/feedback occurs at L; experimental R pacing
    // shifts only the producer's wakeup phase by one physical refresh.
    // Always feed the *preceding L* UST to mpv, even when the callback
    // that wakes the VO is R, because new film generations begin on L.
    //
    // Use an incrementing logical MSC for the presentation feedback:
    // passing raw every-second physical MSC would incorrectly estimate
    // 120Hz based on the 16.67ms L/L (or R/R) interval.
    // Physical MSC, eye parity, and error diagnostics remain unchanged.
    bool logical_stereo = drm->opts && drm->opts->stereo_3d &&
                          drm->stereo_pair_mode;
    int pacing_eye = logical_stereo ? drm->opts->stereo_pacing_eye : 0;
    if (!logical_stereo || drm->stereo_flip_eye == pacing_eye) {
        int64_t presentation_msc = logical_stereo ? ((int64_t)msc / 2) : msc;
        int64_t presentation_ust = ust;
        if (logical_stereo && pacing_eye == 1 && drm->stereo_last_left_ust_us)
            presentation_ust = MP_TIME_US_TO_NS(drm->stereo_last_left_ust_us);
        present_sync_update_values(drm->present, presentation_ust,
                                   presentation_msc);
        present_sync_swap(drm->present);
    }
    // End-of-callback CPU clock, distinct from KMS's hardware UST.
    drm->stereo_last_callback_cpu_ns = mp_time_ns();
    drm->waiting_for_flip = false;
}

static void vo_drm_poll_events(struct vo_drm_state *drm, int64_t timeout_ns,
                               bool wakeups)
{
    struct pollfd fds[2] = {
        { .events = POLLIN, .fd = drm->vt_switcher_active ? vt_switcher_pipe[0] : -1 },
        { .events = POLLIN, .fd = drm->wakeup_pipe[0] },
    };
    mp_poll(fds, wakeups ? 2 : 1, timeout_ns);
    if (fds[0].revents & POLLIN)
        vt_switcher_handle_event(&drm->vt_switcher);
    if (fds[1].revents & POLLIN)
        mp_flush_wakeup_pipe(drm->wakeup_pipe[0]);
}

int vo_drm_control(struct vo *vo, int *events, int request, void *arg)
{
    struct vo_drm_state *drm = vo->drm;
    switch (request) {
    case VOCTRL_CHECK_EVENTS:
        vo_drm_poll_events(drm, 0, false);
        return VO_TRUE;
    case VOCTRL_GET_DISPLAY_FPS: {
        double fps = vo_drm_get_display_fps(drm);
        if (fps <= 0)
            break;
        // --drm-stereo-3d changes mpv's *logical* presentation cadence,
        // never the hardware mode or the physical L/R KMS flip cadence.
        if (drm->opts && drm->opts->stereo_3d && drm->stereo_pair_mode)
            fps /= 2.0;
        *(double*)arg = fps;
        return VO_TRUE;
    }
    case VOCTRL_GET_DISPLAY_RES: {
        ((int *)arg)[0] = drm->mode.mode.hdisplay;
        ((int *)arg)[1] = drm->mode.mode.vdisplay;
        return VO_TRUE;
    }
    case VOCTRL_PAUSE:
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_REDRAW:
        drm->redraw = true;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

bool vo_drm_init(struct vo *vo)
{
    vo->drm = talloc_zero(NULL, struct vo_drm_state);
    struct vo_drm_state *drm = vo->drm;

    *drm = (struct vo_drm_state) {
        .vo = vo,
        .log = mp_log_new(drm, vo->log, "drm"),
        .mode = {{0}},
        .crtc_id = -1,
        .card_no = -1,
        .fd = -1,
        .wakeup_pipe = {-1, -1},
    };

    drm->vt_switcher_active = vt_switcher_init(&drm->vt_switcher, drm->log);
    if (drm->vt_switcher_active) {
        vt_switcher_acquire(&drm->vt_switcher, acquire_vt, drm);
        vt_switcher_release(&drm->vt_switcher, release_vt, drm);
    } else {
        MP_WARN(drm, "Failed to set up VT switcher. Terminal switching will be unavailable.\n");
    }

    drm->opts = mp_get_config_group(drm, drm->vo->global, &drm_conf);

    drmModeRes *res = NULL;
    if (mp_make_wakeup_pipe(drm->wakeup_pipe) < 0) {
        MP_ERR(drm, "Creating wakeup pipe failed: %s\n", mp_strerror(errno));
        goto err;
    }

    get_primary_device_path(drm);

    if (!drm->card_path) {
        MP_ERR(drm, "Failed to find a usable DRM primary node!\n");
        goto err;
    }

    drm->fd = open_card_path(drm->card_path);
    if (drm->fd < 0) {
        MP_ERR(drm, "Cannot open card \"%d\": %s.\n", drm->card_no, mp_strerror(errno));
        goto err;
    }

    drmVersionPtr ver = drmGetVersion(drm->fd);
    if (ver) {
        MP_VERBOSE(drm, "Driver: %s %d.%d.%d (%s)\n", ver->name, ver->version_major,
                   ver->version_minor, ver->version_patchlevel, ver->date);
        drmFreeVersion(ver);
    }

    res = drmModeGetResources(drm->fd);
    if (!res) {
        MP_ERR(drm, "Cannot retrieve DRM resources: %s\n", mp_strerror(errno));
        goto err;
    }

    if (!setup_connector(drm, res, drm->opts->connector_spec))
        goto err;
    if (!setup_crtc(drm, res))
        goto err;
    if (!setup_mode(drm))
        goto err;

    setup_edid(drm);

    // Universal planes allows accessing all the planes (including primary)
    if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        MP_ERR(drm, "Failed to set Universal planes capability\n");
    }

    if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        MP_ERR(drm, "Failed to create DRM atomic context, no DRM Atomic support\n");
        goto err;
    } else {
        MP_VERBOSE(drm, "DRM Atomic support found\n");
        drm->atomic_context = drm_atomic_create_context(drm->log, drm->fd, drm->crtc_id,
                                                        drm->connector->connector_id,
                                                        drm->opts->draw_plane,
                                                        drm->opts->drmprime_video_plane);
        if (!drm->atomic_context) {
            MP_ERR(drm, "Failed to create DRM atomic context\n");
            goto err;
        }
    }

    drmModeFreeResources(res);

    drm->ev.version = DRM_EVENT_CONTEXT_VERSION;
    drm->ev.page_flip_handler = &drm_pflip_cb;
    drm->present = mp_present_initialize(drm, drm->vo->opts, VO_MAX_SWAPCHAIN_DEPTH);

    const char *stereo_env = getenv("MPV_DRM_STEREO_SBS");
    const char *pair_env = getenv("MPV_DRM_STEREO_PAIR");
    if ((drm->opts->stereo_3d ||
         (stereo_env && strcmp(stereo_env, "1") == 0) ||
         (pair_env && strcmp(pair_env, "1") == 0)) &&
        !stereo_async_log_init(drm)) {
        // Optional logging failure is not a reason to block scanout.
        MP_WARN(drm, "Stereo async logger unavailable; "
                "per-flip diagnostic messages disabled\n");
    }

    return true;

err:
    if (res)
        drmModeFreeResources(res);

    vo_drm_uninit(vo);
    return false;
}

void vo_drm_uninit(struct vo *vo)
{
    struct vo_drm_state *drm = vo->drm;
    if (!drm)
        return;

    // The context has already joined the KMS worker.
    stereo_async_log_uninit(drm);
    restore_sdr(drm);
    destroy_hdr_blob(drm);

    if (drm->info)
        di_info_destroy(drm->info);

    vo_drm_release_crtc(drm);
    if (drm->vt_switcher_active)
        vt_switcher_destroy(&drm->vt_switcher);

    drm_mode_destroy_blob(drm->fd, &drm->mode);

    if (drm->connector) {
        drmModeFreeConnector(drm->connector);
        drm->connector = NULL;
    }
    if (drm->encoder) {
        drmModeFreeEncoder(drm->encoder);
        drm->encoder = NULL;
    }
    if (drm->atomic_context)
        drm_atomic_destroy_context(drm->atomic_context);

    for (int n = 0; n < 2; n++) {
        if (drm->wakeup_pipe[n] >= 0)
            close(drm->wakeup_pipe[n]);
        drm->wakeup_pipe[n] = -1;
    }
    if (drm->fd >= 0)
        close(drm->fd);
    talloc_free(drm);
    vo->drm = NULL;
}

static double mode_get_Hz(const drmModeModeInfo *mode)
{
    double rate = mode->clock * 1000.0 / mode->htotal / mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE)
        rate *= 2.0;
    return rate;
}

static void drm_show_available_modes(struct mp_log *log,
                                     const drmModeConnector *connector)
{
    for (unsigned int i = 0; i < connector->count_modes; i++) {
        mp_info(log, "  Mode %d: %s (%dx%d@%.2fHz)\n", i,
                connector->modes[i].name,
                connector->modes[i].hdisplay,
                connector->modes[i].vdisplay,
                mode_get_Hz(&connector->modes[i]));
    }
}

static void drm_show_foreach_connector(struct mp_log *log, int card_no,
                                       const char *card_path,
                                       void (*show_fn)(struct mp_log*, int,
                                                       const drmModeConnector*))
{
    int fd = open_card_path(card_path);
    if (fd < 0) {
        mp_err(log, "Failed to open card %d (%s)\n", card_no, card_path);
        return;
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        mp_err(log, "Cannot retrieve DRM resources: %s\n", mp_strerror(errno));
        goto err;
    }

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, res->connectors[i]);
        if (!connector)
            continue;
        show_fn(log, card_no, connector);
        drmModeFreeConnector(connector);
    }

err:
    if (fd >= 0)
        close(fd);
    if (res)
        drmModeFreeResources(res);
}

static void drm_show_connector_name_and_state_callback(struct mp_log *log, int card_no,
                                                       const drmModeConnector *connector)
{
    char other_connector_name[MAX_CONNECTOR_NAME_LEN];
    get_connector_name(connector, other_connector_name);
    const char *connection_str = (connector->connection == DRM_MODE_CONNECTED) ?
                                 "connected" : "disconnected";
    mp_info(log, "  %s (%s)\n", other_connector_name, connection_str);
}

static void drm_show_available_connectors(struct mp_log *log, int card_no,
                                          const char *card_path)
{
    if (card_no)
        mp_info(log, "\n");
    mp_info(log, "Available connectors for card %d (%s):\n", card_no,
            card_path);
    drm_show_foreach_connector(log, card_no, card_path,
                               drm_show_connector_name_and_state_callback);
}

static void drm_show_connector_modes_callback(struct mp_log *log, int card_no,
                                              const drmModeConnector *connector)
{
    if (connector->connection != DRM_MODE_CONNECTED)
        return;

    char other_connector_name[MAX_CONNECTOR_NAME_LEN];
    get_connector_name(connector, other_connector_name);
    if (card_no)
        mp_info(log, "\n");
    mp_info(log, "Available modes for drm-connector=%d.%s\n",
            card_no, other_connector_name);
    drm_show_available_modes(log, connector);
}

static void drm_show_available_connectors_and_modes(struct mp_log *log,
                                                    int card_no,
                                                    const char *card_path)
{
    drm_show_foreach_connector(log, card_no, card_path,
                               drm_show_connector_modes_callback);
}

static void drm_show_foreach_card(struct mp_log *log,
                                  void (*show_fn)(struct mp_log *, int,
                                                  const char *))
{
    drmDevice *devices[DRM_MAX_MINOR] = { 0 };
    int card_count = drmGetDevices2(0, devices, MP_ARRAY_SIZE(devices));
    if (card_count < 0) {
        mp_err(log, "Listing DRM devices with drmGetDevices failed! (%s)\n",
               mp_strerror(errno));
        return;
    }

    for (int i = 0; i < card_count; i++) {
        drmDevice *dev = devices[i];

        if (!(dev->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;

        const char *card_path = dev->nodes[DRM_NODE_PRIMARY];

        int fd = open_card_path(card_path);
        if (fd < 0) {
            mp_err(log, "Failed to open primary DRM node path %s!\n",
                   card_path);
            continue;
        }

        close(fd);
        show_fn(log, i, card_path);
    }

    drmFreeDevices(devices, card_count);
}

static void drm_show_available_cards_and_connectors(struct mp_log *log)
{
    drm_show_foreach_card(log, drm_show_available_connectors);
}

static void drm_show_available_cards_connectors_and_modes(struct mp_log *log)
{
    drm_show_foreach_card(log, drm_show_available_connectors_and_modes);
}

static int drm_connector_opt_help(struct mp_log *log, const struct m_option *opt,
                                  struct bstr name)
{
    drm_show_available_cards_and_connectors(log);
    return M_OPT_EXIT;
}

static int drm_mode_opt_help(struct mp_log *log, const struct m_option *opt,
                             struct bstr name)
{
    drm_show_available_cards_connectors_and_modes(log);
    return M_OPT_EXIT;
}

static int drm_validate_mode_opt(struct mp_log *log, const struct m_option *opt,
                                 struct bstr name, const char **value)
{
    const char *param = *value;
    if (!parse_mode_spec(param, NULL)) {
        mp_fatal(log, "Invalid value for option drm-mode. Must be a positive number, a string of the format WxH[@R] or 'help'\n");
        return M_OPT_INVALID;
    }

    return 1;
}

/* Helpers */
bool vo_drm_color_supported_by_display(struct vo_drm_state *drm, struct pl_color_space color)
{
    const struct di_cta_hdr_static_metadata_block *hdr_static_metadata = drm->hdr_static_metadata;
    enum pl_color_transfer trc = color.transfer;

    if (!pl_color_transfer_is_hdr(trc) && hdr_static_metadata && !hdr_static_metadata->eotfs->traditional_sdr)
        return false;

    if (pl_color_transfer_is_hdr(trc)) {
        if (!drm->chromaticity || !drm->colorimetry || !hdr_static_metadata || !drm->colorimetry->bt2020_rgb)
            return false;

        if (trc == PL_COLOR_TRC_PQ && !hdr_static_metadata->eotfs->pq)
            return false;

        if (trc == PL_COLOR_TRC_HLG && !hdr_static_metadata->eotfs->hlg)
            return false;
    }

    return true;
}

double vo_drm_get_display_fps(struct vo_drm_state *drm)
{
    return mode_get_Hz(&drm->mode.mode);
}

bool vo_drm_set_color(struct vo *vo, struct pl_color_space *color)
{
    struct vo_drm_state *drm = vo->drm;
    if (pl_color_space_equal(&drm->colorspace, color))
        return false;

    // Set to sdr if the display doesn't support the colorspace.
    if (!vo_drm_color_supported_by_display(drm, *color)) {
        *color = pl_color_space_srgb;
        pl_color_space_infer(color);
    }

    destroy_hdr_blob(drm);
    drm->colorspace = *color;

    // For any HDR, the BT2020 drm colorspace is the only one that works in practice.
    struct drm_atomic_context *atomic_ctx = drm->atomic_context;
    int colorspace = pl_color_space_is_hdr(color) || pl_color_primaries_is_wide_gamut(color->primaries) ?
                     DRM_MODE_COLORIMETRY_BT2020_RGB : DRM_MODE_COLORIMETRY_DEFAULT;
    drm_object_set_property(atomic_ctx->request, atomic_ctx->connector, "Colorspace", colorspace);

    const struct pl_hdr_metadata *hdr = &color->hdr;
    struct hdr_output_metadata metadata = {
        .metadata_type = HDMI_STATIC_METADATA_TYPE1,
        .hdmi_metadata_type1.metadata_type = HDMI_STATIC_METADATA_TYPE1,

        .hdmi_metadata_type1.eotf = eotf_map[color->transfer],

        .hdmi_metadata_type1.display_primaries[0].x = lrintf(hdr->prim.red.x * DRM_PRIM_FACTOR),
        .hdmi_metadata_type1.display_primaries[0].y = lrintf(hdr->prim.red.y * DRM_PRIM_FACTOR),
        .hdmi_metadata_type1.display_primaries[1].x = lrintf(hdr->prim.green.x * DRM_PRIM_FACTOR),
        .hdmi_metadata_type1.display_primaries[1].y = lrintf(hdr->prim.green.y * DRM_PRIM_FACTOR),
        .hdmi_metadata_type1.display_primaries[2].x = lrintf(hdr->prim.blue.x * DRM_PRIM_FACTOR),
        .hdmi_metadata_type1.display_primaries[2].y = lrintf(hdr->prim.blue.y * DRM_PRIM_FACTOR),

        .hdmi_metadata_type1.white_point.x = lrintf(hdr->prim.white.x * DRM_PRIM_FACTOR),
        .hdmi_metadata_type1.white_point.y = lrintf(hdr->prim.white.y * DRM_PRIM_FACTOR),

        .hdmi_metadata_type1.min_display_mastering_luminance = lrintf(hdr->min_luma * DRM_MIN_LUMA_FACTOR),
        .hdmi_metadata_type1.max_display_mastering_luminance = hdr->max_luma,

        .hdmi_metadata_type1.max_cll = hdr->max_cll,
        .hdmi_metadata_type1.max_fall = hdr->max_fall,
    };
    drmModeCreatePropertyBlob(drm->fd, &metadata, sizeof(metadata), &drm->hdr.blob_id);
    drm_object_set_property(atomic_ctx->request, atomic_ctx->connector, "HDR_OUTPUT_METADATA", drm->hdr.blob_id);
    return true;
}

void vo_drm_set_monitor_par(struct vo *vo)
{
    struct vo_drm_state *drm = vo->drm;
    if (vo->opts->force_monitor_aspect != 0.0) {
        vo->monitor_par = drm->fb->width / (double) drm->fb->height /
                          vo->opts->force_monitor_aspect;
    } else {
        vo->monitor_par = 1 / vo->opts->monitor_pixel_aspect;
    }
    MP_VERBOSE(drm, "Monitor pixel aspect: %g\n", vo->monitor_par);
}

void vo_drm_wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct vo_drm_state *drm = vo->drm;
    int64_t wait_ns = until_time_ns - mp_time_ns();
    int64_t timeout_ns = MPCLAMP(wait_ns, 0, MP_TIME_S_TO_NS(10));
    vo_drm_poll_events(drm, timeout_ns, true);
}

void vo_drm_wait_on_flip(struct vo_drm_state *drm)
{
    // poll page flip finish event
    while (drm->waiting_for_flip) {
        struct pollfd fds[1] = { { .events = POLLIN, .fd = drm->fd } };
        mp_poll(fds, 1, MP_TIME_S_TO_NS(3));
        if (fds[0].revents & POLLIN) {
            const int ret = drmHandleEvent(drm->fd, &drm->ev);
            if (ret != 0) {
                MP_ERR(drm, "drmHandleEvent failed: %i\n", ret);
                return;
            }
        }
    }
}

void vo_drm_wakeup(struct vo *vo)
{
    struct vo_drm_state *drm = vo->drm;
    (void)write(drm->wakeup_pipe[1], &(char){0}, 1);
}
