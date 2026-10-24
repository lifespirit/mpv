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

#ifndef MP_VT_SWITCHER_H
#define MP_VT_SWITCHER_H

#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libdisplay-info/cta.h>
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>

#include "video/mp_image.h"
#include "vo.h"

enum {
    DRM_OPTS_FORMAT_XRGB8888,
    DRM_OPTS_FORMAT_XRGB2101010,
    DRM_OPTS_FORMAT_XBGR8888,
    DRM_OPTS_FORMAT_XBGR2101010,
    DRM_OPTS_FORMAT_RGB565,
    DRM_OPTS_FORMAT_YUYV,
};

// Enum values based on include/linux/hdmi.h
// and https://docs.kernel.org/gpu/drm-uapi.html
// for interoperability with drm API.
enum drm_metadata_type {
    HDMI_STATIC_METADATA_TYPE1 = 0,
};

// Enum values based on https://docs.kernel.org/gpu/drm-kms.html
// for interoperability with drm API.
enum drm_colorspace {
    DRM_MODE_COLORIMETRY_DEFAULT = 0,
    DRM_MODE_COLORIMETRY_NO_DATA = 0,
    DRM_MODE_COLORIMETRY_SMPTE_170M_YCC = 1,
    DRM_MODE_COLORIMETRY_BT709_YCC = 2,
    DRM_MODE_COLORIMETRY_XVYCC_601 = 3,
    DRM_MODE_COLORIMETRY_XVYCC_709 = 4,
    DRM_MODE_COLORIMETRY_SYCC_601 = 5,
    DRM_MODE_COLORIMETRY_OPYCC_601 = 6,
    DRM_MODE_COLORIMETRY_OPRGB = 7,
    DRM_MODE_COLORIMETRY_BT2020_CYCC = 8,
    DRM_MODE_COLORIMETRY_BT2020_RGB = 9,
    DRM_MODE_COLORIMETRY_BT2020_YCC = 10,
    DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65 = 11,
    DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER = 12,
    DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED = 13,
    DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT = 14,
    DRM_MODE_COLORIMETRY_BT601_YCC = 15,
    DRM_MODE_COLORIMETRY_COUNT,
};

// Enum values based on include/linux/hdmi.h
// and https://docs.kernel.org/gpu/drm-uapi.html
// for interoperability with drm API.
enum drm_eotf {
    HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
    HDMI_EOTF_TRADITIONAL_GAMMA_HDR,
    HDMI_EOTF_SMPTE_ST2084,
    HDMI_EOTF_BT_2100_HLG,
};

struct framebuffer {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t size;
    uint32_t handle;
    uint8_t *map;
    uint32_t id;
};

struct drm_hdr {
    struct hdr_output_metadata metadata;
    uint32_t blob_id;
};

struct drm_mode {
    drmModeModeInfo mode;
    uint32_t blob_id;
};

struct drm_opts {
    char *device_path;
    char *connector_spec;
    char *mode_spec;
    int drm_atomic;
    int draw_plane;
    int drmprime_video_plane;
    int drm_format;
    struct m_geometry draw_surface_size;
    int vrr_enabled;
    // Half-SBS active-shutter experiment: mpv sees one logical refresh
    // per completed L/R cycle while DRM keeps the physical mode unchanged.
    bool stereo_3d;
    // Which physical eye callback drives the logical 60Hz mpv clock.
    // 0 = L (normal), 1 = R (experimental half-cycle phase shift).
    int stereo_pacing_eye;
    // Enable 120-flip verbose timing reports from DRM callback.
    // Set to false for proper RT logging overhead A/B tests.
    bool stereo_stats;
};

// Shared option group, also queried by vo_gpu's DRM stereo startup.
extern const struct m_sub_options drm_conf;

struct vt_switcher {
    int tty_fd;
    struct mp_log *log;
    void (*handlers[2])(void*);
    void *handler_data[2];
};

// A bounded diagnostic snapshot of a confirmed FIRST LEFT stereo scanout.
// These are sampled in the DRM callback and printed only after the RT thread
// exits. UST is a DRM CLOCK_MONOTONIC vblank timestamp, NOT mp_time_ns().
#define DRM_STEREO_PTS_TRACE_CAP 256
struct drm_stereo_pts_sample {
    uint64_t frame_id;
    uint64_t generation;
    uint32_t msc;
    uint64_t ust_us;
    double media_pts;
    double relative_drift_ms; // versus first confirmed L in this PTS segment
    uint32_t cycles_since_previous; // physical MSC delta / 2, 0 for anchor
    // mpv's ORIGINAL schedule: num_vsyncs from first VO draw, before repeats
    // decrement it. Applies to this frame's intended duration.
    int planned_vsyncs;
    bool display_synced;
    // Interval ending at this frame: compare prior movie frame's plan only
    // when previous and current confirmed frame IDs are consecutive.
    int previous_planned_vsyncs; // -1 for uncomparable interval
    uint64_t frame_id_gap;       // count skipped IDs since previous first L
    bool plan_comparable;
};

struct drm_stereo_async_log;

// Immutable scalar diagnostic event. Never store pointers to GBM/DRM buffers.
struct drm_stereo_gpu_probe {
    uint64_t frame_id;
    uint64_t generation;
    int fence_fd[2]; // existing KMS-owned sync_file fds; duplicate on enqueue
    int64_t pair_published_ns; // mpv CPU monotonic clock
    int64_t render_ns[2];
    int64_t egl_swap_ns[2];
    int64_t fence_export_ns[2];
};

struct drm_stereo_async_drop {
    const char *reason; // Fixed string literals owned by the caller.
    uint64_t count, pending_gen, active_gen, frame_id;
    double media_pts, age_ms;
    bool pts_valid;
    int fence_left, fence_right;
    unsigned deferrals;
    double first_deferral_ms, last_deferral_ms;
    uint32_t last_deferral_msc;
    double r_probe_age_ms;
    uint32_t r_probe_msc;
    int r_probe_left, r_probe_right;
};

struct vo_drm_state {
    drmModeConnector *connector;
    drmModeEncoder *encoder;
    drmEventContext ev;

    struct drm_atomic_context *atomic_context;
    struct drm_hdr hdr;
    struct drm_mode mode;
    struct drm_opts *opts;
    struct framebuffer *fb;
    struct pl_color_space colorspace;
    struct mp_log *log;
    struct mp_present *present;
    struct vo *vo;
    struct vt_switcher vt_switcher;

    // libdisplay-info edid stuff
    struct di_info *info;
    const struct di_edid_chromaticity_coords *chromaticity;
    const struct di_cta_hdr_static_metadata_block *hdr_static_metadata;
    const struct di_cta_colorimetry_block *colorimetry;

    bool active;
    bool redraw;
    bool vt_switcher_active;
    bool waiting_for_flip;
    struct drm_stereo_async_log *stereo_async_log;

    // Experimental opt-in SBS shutter-stereo diagnostics (no USB).
    // A submitted eye is tagged with the GBM buffer it belongs to.
    bool stereo_test_enabled;
    int stereo_draw_eye;
    int stereo_flip_eye;
    int stereo_last_eye;
    bool stereo_have_flip;
    uint32_t stereo_last_msc;
    uint64_t stereo_last_ust_us;
    // Physical L callback timestamp retained for R-phase logical feedback.
    uint64_t stereo_last_left_ust_us;
    uint64_t stereo_window_first_ust_us;
    uint64_t stereo_last_report_ust_us;
    uint64_t stereo_last_report_missed, stereo_last_report_parity;
    uint64_t stereo_flips, stereo_missed_slots, stereo_parity_errors;
    uint32_t stereo_max_gap;
    uint64_t stereo_draws, stereo_commit_errors;

    // Shadow diagnosis: physical-slot identity anchored to first page flip.
    // Optional recovery changes ONLY the next software eye selection.
    bool stereo_recover;
    uint32_t stereo_base_msc;
    int stereo_base_eye;
    unsigned stereo_phase_correction;
    uint64_t stereo_absolute_eye_errors, stereo_wrong_hold_slots;
    uint64_t stereo_last_report_absolute, stereo_last_report_hold;
    uint64_t stereo_corrections;

    // Opt-in MSC/queue-aware scheduling experiment (still NO USB).
    // Count frames queued *after* the currently scanned-out GBM buffer.
    bool stereo_slot_mode;
    unsigned stereo_queue_ahead;
    unsigned stereo_queue_peak;
    uint32_t stereo_draw_target_msc;
    bool stereo_draw_target_valid;
    int64_t stereo_draw_time_ns;

    // Commit-to-callback diagnostics. Delivery time is not scanout time.
    uint32_t stereo_commit_target_msc;
    bool stereo_commit_target_valid;
    int64_t stereo_commit_time_ns;
    int64_t stereo_commit_draw_time_ns;
    uint64_t stereo_target_misses;
    uint64_t stereo_queue_discards;
    uint64_t stereo_last_report_target_misses;
    uint64_t stereo_last_report_queue_discards;
    int64_t stereo_max_commit_callback_ns;
    int64_t stereo_max_draw_callback_ns;
    // CPU-side turnaround between confirmed page-flip callback and the
    // next atomic submit. This is not the kernel's hardware vblank time.
    int64_t stereo_last_callback_cpu_ns;
    int64_t stereo_commit_after_callback_ns;
    int64_t stereo_max_submit_after_callback_ns;
    // Duration spent inside drmModeAtomicCommit() (including kernel blocking).
    int64_t stereo_commit_ioctl_ns;
    int64_t stereo_max_commit_ioctl_ns;

    // Phase diagnostics: DRM UST timestamps must use CLOCK_MONOTONIC.
    // This is verified using DRM_CAP_TIMESTAMP_MONOTONIC on the CRTC fd.
    bool stereo_ust_monotonic;
    bool stereo_callback_lag_valid;
    bool stereo_prev_callback_lag_valid;
    bool stereo_submit_after_ust_valid;
    // Signed measurements are preserved even if kernel UST is ahead of
    // clock_gettime(CLOCK_MONOTONIC). Never use -1 ns as an error sentinel.
    int64_t stereo_last_callback_lag_ns;
    int64_t stereo_prev_callback_lag_ns;
    int64_t stereo_submit_after_ust_ns;
    int64_t stereo_min_callback_lag_ns;
    int64_t stereo_max_callback_lag_ns;
    int64_t stereo_min_submit_after_ust_ns;
    int64_t stereo_max_submit_after_ust_ns;
    uint64_t stereo_negative_callback_lags;

    // Opt-in dual-surface producer / independent DRM vblank scanout.
    // Stereo pair operations are implemented by the drm EGL context.
    bool stereo_pair_mode;
    // Optional legacy CRTC page-flip experiment; atomic remains default.
    bool stereo_pair_legacy_mode;
    uint64_t stereo_pair_legacy_flips;
    uint64_t stereo_pair_legacy_fallbacks;
    bool stereo_pair_sequence_mode;
    uint64_t stereo_pair_sequence_catchups;
    bool stereo_pair_gpu_wait;
    bool stereo_pair_plan_hold;
    uint64_t stereo_pair_plan_early_blocks;
    uint64_t stereo_pair_plan_due_promotions;
    uint64_t stereo_pair_plan_late_promotions;
    uint64_t stereo_pair_plan_anchor_missing;
    uint64_t stereo_pair_gpu_waits;
    int64_t stereo_max_gpu_wait_ns;
    // Optional EGL native sync_file FD readiness checks before new pairs
    // are promoted at left-eye vblank. KMS does not block on the GPU.
    bool stereo_pair_native_fence;
    uint64_t stereo_pair_fence_checks;
    uint64_t stereo_pair_fence_promotions;
    uint64_t stereo_pair_fence_deferrals;
    uint64_t stereo_pair_fence_unready_eye[2];
    uint64_t stereo_pair_fence_errors;
    int stereo_capture_eye;
    uint64_t stereo_capture_generation;
    uint64_t stereo_pair_rendered;
    uint64_t stereo_pair_display_generation;
    // Diagnostic of initial scanout for each eye in a prepared pair.
    uint64_t stereo_pair_eye_last_submitted_generation[2];
    uint64_t stereo_pair_eye_first_flips[2];
    uint64_t stereo_pair_eye_first_misses[2];
    bool stereo_pair_first_eye_in_generation;
    int64_t stereo_pair_age_at_submit_ns;
    uint64_t stereo_pair_repeated_flips;
    uint64_t stereo_pair_update_skips;
    // Pairs replaced/dropped while pending, before their first KMS flip.
    uint64_t stereo_pair_unpresented_discards;
    // Cheap producer-side aggregate diagnostics: no fence poll or log call
    // while holding stereo.mutex in quiet mode.
    uint64_t stereo_pair_drop_newer, stereo_pair_drop_gbm_pressure;
    uint64_t stereo_pair_drop_deferrals[4]; // 0, 1, 2, >=3 L attempts
    uint64_t stereo_pair_drop_age_samples;
    int64_t stereo_pair_drop_age_sum_ns;
    int64_t stereo_pair_drop_age_min_ns, stereo_pair_drop_age_max_ns;

    // Submitted generation metadata, transferred atomically from stereo_pair
    // while KMS submits the FB. The callback runs only for the in-flight flip.
    uint64_t stereo_submit_frame_id;
    double stereo_submit_media_pts;
    bool stereo_submit_pts_valid;
    int stereo_submit_planned_vsyncs;
    bool stereo_submit_display_synced;
    // First LEFT presentation telemetry (no per-frame KMS logging).
    uint64_t stereo_pts_first_L, stereo_pts_valid_L, stereo_pts_missing_L;
    uint64_t stereo_pts_segments, stereo_pts_discontinuities;
    uint64_t stereo_pts_id_gaps;
    uint64_t stereo_pts_cycles_1, stereo_pts_cycles_2;
    uint64_t stereo_pts_cycles_3, stereo_pts_cycles_4plus;
    uint32_t stereo_pts_last_msc;
    uint64_t stereo_pts_first_ust_us, stereo_pts_last_ust_us;
    uint64_t stereo_pts_segment_anchor_ust_us;
    double stereo_pts_segment_anchor_media_pts;
    double stereo_pts_first_media_pts;
    double stereo_pts_last_media_pts;
    uint64_t stereo_pts_last_frame_id;
    int stereo_pts_last_planned_vsyncs;
    bool stereo_pts_last_display_synced;

    // Planned logical refreshes from mpv vs actual first-L-to-first-L
    // physical MSC deltas / 2. Compare only adjacent confirmed frame IDs:
    // intervals spanning dropped movie frames are not single-frame holds.
    uint64_t stereo_plan_requested_hist[5]; // 0, 1, 2, 3, >=4
    uint64_t stereo_plan_nondisplay, stereo_plan_invalid;
    uint64_t stereo_plan_comparisons, stereo_plan_exact;
    uint64_t stereo_plan_skipped_intervals, stereo_plan_skipped_ids;
    uint64_t stereo_plan_invalid_msc;
    uint64_t stereo_plan_matrix[5][5]; // [planned 0/1/2/3/4+][actual]
    int64_t stereo_plan_signed_error_cycles;
    uint64_t stereo_plan_abs_error_cycles;

    double stereo_pts_last_drift_ms, stereo_pts_drift_min_ms;
    double stereo_pts_drift_max_ms, stereo_pts_drift_sum_ms;
    struct drm_stereo_pts_sample stereo_pts_trace[DRM_STEREO_PTS_TRACE_CAP];
    uint64_t stereo_pts_trace_total;

    // CPU publication-to-first-submission ages. These do NOT measure
    // optical scanout or actual audio-video latency.
    int64_t stereo_pair_first_age_sum_ns[2];
    int64_t stereo_pair_first_age_min_ns[2];
    int64_t stereo_pair_first_age_max_ns[2];

    char *card_path;
    int card_no;
    int fd;
    int wakeup_pipe[2];

    uint32_t crtc_id;
    uint32_t height;
    uint32_t width;
};

bool vo_drm_init(struct vo *vo);
int vo_drm_control(struct vo *vo, int *events, int request, void *arg);

bool vo_drm_color_supported_by_display(struct vo_drm_state *drm, struct pl_color_space color);
double vo_drm_get_display_fps(struct vo_drm_state *drm);
bool vo_drm_set_color(struct vo *vo, struct pl_color_space *color);
void vo_drm_set_monitor_par(struct vo *vo);
void vo_drm_uninit(struct vo *vo);
void vo_drm_wait_events(struct vo *vo, int64_t until_time_ns);
void vo_drm_wait_on_flip(struct vo_drm_state *drm);
void vo_drm_wakeup(struct vo *vo);

// Nonblocking diagnostics: safe while stereo.mutex is held.
void vo_drm_stereo_log_drop(struct vo_drm_state *drm,
                             const struct drm_stereo_async_drop *drop);
// Sampled only. The logger duplicates both sync_file descriptors and
// waits in its normal-priority worker, never on the KMS/GL threads.
void vo_drm_stereo_log_gpu_probe(struct vo_drm_state *drm,
                                  const struct drm_stereo_gpu_probe *probe);

bool vo_drm_acquire_crtc(struct vo_drm_state *drm);
void vo_drm_release_crtc(struct vo_drm_state *drm);

// Only valid for --gpu-context=drm and MPV_DRM_STEREO_PAIR=1.
// The EGL context remains owned by the VO/rendering thread.
struct ra_ctx;
bool vo_drm_stereo_pair_begin_eye(struct ra_ctx *ctx, int eye);
void vo_drm_stereo_pair_note_eye_render(struct ra_ctx *ctx, int eye,
                                         int64_t render_ns);
bool vo_drm_stereo_pair_can_capture(struct ra_ctx *ctx);
void vo_drm_stereo_pair_set_frame_info(struct ra_ctx *ctx,
                                       uint64_t frame_id, double media_pts,
                                       bool pts_valid, int planned_vsyncs,
                                       bool display_synced);
// Pace the mpv display-synced VO independently of the KMS scanout worker.
void vo_drm_stereo_pair_wait_vsync(struct ra_ctx *ctx);

#endif
