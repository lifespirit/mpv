/*
 * Based on vo_gl.c by Reimar Doeffinger.
 *
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>

#include <libavutil/common.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/bstr.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "osdep/timer.h"
#include "vo.h"
#include "video/mp_image.h"
#include "sub/osd.h"

#include "gpu/context.h"
#include "gpu/hwdec.h"
#include "gpu/video.h"

#include "config.h"
#if HAVE_DRM
#include "drm_common.h"
#endif

struct gpu_priv {
    struct mp_log *log;
    struct ra_ctx *ctx;

    char *context_name;
    char *context_type;
    struct gl_video *renderer;

    int events;

    // Experimental --vo=gpu --gpu-context=drm SBS alternation, no USB.
    bool stereo_test;
    uint64_t stereo_draw_count;
    bool stereo_pair;
    bool stereo_pair_valid;
    uint64_t stereo_pair_last_frame_id;
    struct mp_rect stereo_full_src;
    int stereo_calibration_pattern; // -1=movie, 0/1=BW pair, 2=stereo depth ladder
    bool stereo_calibration_drawn;
};
static void resize(struct vo *vo)
{
    struct gpu_priv *p = vo->priv;
    struct ra_swapchain *sw = p->ctx->swapchain;

    MP_VERBOSE(vo, "Resize: %dx%d\n", vo->dwidth, vo->dheight);

    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    p->stereo_full_src = src;

    // The first eye will be selected by the first draw_frame() call.
    // Do not half the viewport in the global aspect calculation; each
    // half-SBS eye is expanded into the entire destination rectangle.
    gl_video_resize(p->renderer, &src, &dst, &osd);

    int fb_depth = sw->fns->color_depth ? sw->fns->color_depth(sw) : 0;
    if (fb_depth)
        MP_VERBOSE(p, "Reported display depth: %d\n", fb_depth);
    gl_video_set_fb_depth(p->renderer, fb_depth);

    vo->want_redraw = true;
}

#if HAVE_DRM
// The calibration pair is rendered only once. The ruler is complementary:
// the white test eye has a black scale with white ticks, and the black test
// eye a white scale with black ticks. This makes the scanout leakage
// boundary visible in the scale as well as in the adjacent test field.
// It is not OSD and does not run on the KMS or USB scheduling threads.
//
// All positions below use top-left image coordinates. The ra_gl clear
// scissor uses bottom-left coordinates, so invert Y explicitly.
static void stereo_ruler_rect(struct ra *ra, struct ra_tex *tex,
                             float color[4], int x0, int y0, int x1, int y1)
{
    const int w = tex->params.w, h = tex->params.h;
    x0 = MPMAX(0, MPMIN(w, x0));
    x1 = MPMAX(0, MPMIN(w, x1));
    y0 = MPMAX(0, MPMIN(h, y0));
    y1 = MPMAX(0, MPMIN(h, y1));
    if (x1 <= x0 || y1 <= y0)
        return;
    struct mp_rect scissor = {
        .x0 = x0, .y0 = h - y1, .x1 = x1, .y1 = h - y0,
    };
    ra->fns->clear(ra, tex, color, &scissor);
}

// 3x5 pixel glyphs: 0..9 and %. Only those glyphs are drawn, without font
// loading, subtitles, GL shaders or video timing dependencies.
static const unsigned char stereo_ruler_glyphs[11][5] = {
    {7, 5, 5, 5, 7}, // 0
    {2, 6, 2, 2, 7}, // 1
    {7, 1, 7, 4, 7}, // 2
    {7, 1, 7, 1, 7}, // 3
    {5, 5, 7, 1, 1}, // 4
    {7, 4, 7, 1, 7}, // 5
    {7, 4, 7, 5, 7}, // 6
    {7, 1, 2, 2, 2}, // 7
    {7, 5, 7, 5, 7}, // 8
    {7, 5, 7, 1, 7}, // 9
    {5, 1, 2, 4, 5}, // %
};

static void stereo_ruler_text(struct ra *ra, struct ra_tex *tex,
                              int x, int top, int scale,
                              float color[4], const char *label)
{
    for (int i = 0; label[i]; i++) {
        int glyph = label[i] == '%' ? 10 :
                    label[i] >= '0' && label[i] <= '9' ?
                    label[i] - '0' : -1;
        if (glyph < 0)
            continue;
        for (int y = 0; y < 5; y++) {
            unsigned int bits = stereo_ruler_glyphs[glyph][y];
            for (int col = 0; col < 3; col++) {
                if (bits & (4u >> col)) {
                    stereo_ruler_rect(ra, tex, color,
                                     x + col * scale, top + y * scale,
                                     x + (col + 1) * scale,
                                     top + (y + 1) * scale);
                }
            }
        }
        x += 4 * scale;
    }
}

static void stereo_calibration_draw_ruler(struct ra_ctx *ctx,
                                          struct ra_tex *tex,
                                          bool white_eye)
{
    const int w = tex->params.w, h = tex->params.h;
    // Ruler is on for synthetic calibration by default. Allow an A/B
    // comparison with the exact legacy blank calibration buffers.
    const char *ruler = getenv("MPV_DRM_STEREO_CALIBRATION_RULER");
    if ((ruler && strcmp(ruler, "0") == 0) || w < 320 || h < 240)
        return;

    // On 2560x1440: a 150 px strip at the right, every 1% tick,
    // every 5% medium tick, large ticks and labels every 10%.
    const int scale = h >= 1000 ? 4 : 2;
    const int strip = MPMIN(scale == 4 ? 150 : 96, w / 3);
    const int left = w - strip;
    const int tick_x = left + 14;
    const int text_x = left + (scale == 4 ? 57 : 43);
    // Complementary high-contrast ruler for the two eye buffers.
    // White test eye: black background / white marks.
    // Black test eye: white background / black marks.
    const float bg_value = white_eye ? 0.0f : 1.0f;
    const float edge_value = white_eye ? 0.20f : 0.80f;
    const float faint_value = white_eye ? 0.55f : 0.45f;
    const float medium_value = white_eye ? 0.78f : 0.22f;
    const float bright_value = white_eye ? 1.0f : 0.0f;
    float bg[4] = {bg_value, bg_value, bg_value, 1.0f};
    float edge[4] = {edge_value, edge_value, edge_value, 1.0f};
    float faint[4] = {faint_value, faint_value, faint_value, 1.0f};
    float medium[4] = {medium_value, medium_value, medium_value, 1.0f};
    float bright[4] = {bright_value, bright_value, bright_value, 1.0f};

    struct ra *ra = ctx->ra;
    stereo_ruler_rect(ra, tex, bg, left, 0, w, h);
    stereo_ruler_rect(ra, tex, edge, left, 0, left + 2, h);
    stereo_ruler_rect(ra, tex, edge, tick_x, 0, tick_x + 2, h);
    for (int percent = 0; percent <= 100; percent++) {
        int top = (percent * (h - 1) + 50) / 100;
        bool major = percent % 10 == 0;
        bool mid = percent % 5 == 0;
        int length = major ? 34 : mid ? 22 : 11;
        float *ink = major ? bright : mid ? medium : faint;
        stereo_ruler_rect(ra, tex, ink, tick_x, top - 1,
                          tick_x + length, top + (major ? 2 : 1));
        if (major) {
            char label[8];
            snprintf(label, sizeof(label), "%d%%", percent);
            int label_top = MPMAX(3, MPMIN(h - 3 - scale * 5,
                                          top - scale * 5 / 2));
            stereo_ruler_text(ra, tex, text_x, label_top, scale,
                              bright, label);
        }
    }
}

// Seven static stereo-disparity steps: top three recede behind the
// screen, the middle is at the screen plane, and bottom three project
// in front of it. Eye shifts are equal and opposite. No rendering occurs
// on the real-time KMS or USB threads, only on pattern (re)capture.
//
// Positive disparity means crossed parallax (front of the screen):
// left-eye x is to the RIGHT of right-eye x. Negative means behind.
// On 2560px displays the per-level disparity is 16px: -48..+48px,
// i.e. <= 1.9% of display width. Labels move with their own steps.
static void stereo_calibration_draw_depth_ladder(struct ra_ctx *ctx,
                                                 struct ra_tex *tex, int eye)
{
    const int w = tex->params.w, h = tex->params.h;
    if (w < 320 || h < 240)
        return;
    struct ra *ra = ctx->ra;

    // Draw common reference geometry at zero disparity in BOTH buffers.
    // This helps distinguish true stereoscopic separation from ghosting.
    float guide[4] = {0.20f, 0.24f, 0.28f, 1.0f};
    float guide_mid[4] = {0.38f, 0.43f, 0.48f, 1.0f};
    float outline[4] = {0.96f, 0.97f, 0.98f, 1.0f};
    float inset[4] = {0.15f, 0.18f, 0.21f, 1.0f};
    float ink[4] = {0.05f, 0.06f, 0.08f, 1.0f};

    const int cx = w / 2;
    const int width = MPMAX(100, MPMIN(w * 2 / 5, w - w / 4));
    const int height = MPMAX(12, h / 23);
    const int pitch = MPMAX(height + 5, h / 9);
    const int first_y = (h - (6 * pitch + height)) / 2;
    // Keep disparities even so each eye moves by an identical integer
    // offset. All values remain safely below ~2% of screen width.
    const int one_depth = 2 * MPMAX(2, MPMIN(12, w / 320));
    const int stroke = MPMAX(2, h / 720);
    const int scale = h >= 1000 ? 3 : 2;

    stereo_ruler_rect(ra, tex, guide, cx - stroke / 2,
                      h / 12, cx + (stroke + 1) / 2, h - h / 12);
    stereo_ruler_rect(ra, tex, guide, w / 6, h / 2 - 1,
                      w - w / 6, h / 2 + 1);
    // A fixed screen-plane frame in both eyes, behind the steps.
    stereo_ruler_rect(ra, tex, guide_mid, w / 8, h / 9,
                      w / 8 + stroke, h - h / 9);
    stereo_ruler_rect(ra, tex, guide_mid, w - w / 8 - stroke,
                      h / 9, w - w / 8, h - h / 9);

    for (int i = 0; i < 7; i++) {
        // i=0: farthest back (uncrossed); i=3: screen; i=6:
        // nearest (crossed). i=3 has EXACT zero binocular shift.
        int depth = i - 3;
        int disparity = depth * one_depth;
        int eye_shift = eye == 0 ? disparity / 2 : -disparity / 2;
        // A modest common X offset makes the set look like a staircase
        // without changing the independently measurable parallax.
        int x = cx - width / 2 + (i - 3) * w / 120 + eye_shift;
        int y = first_y + i * pitch;
        float face[4] = {
            0.71f + i * 0.025f,
            0.77f + i * 0.023f,
            0.83f + i * 0.018f, 1.0f
        };
        if (i == 3) { // distinguish the screen-plane step
            face[0] = 0.98f;
            face[1] = 0.85f;
            face[2] = 0.49f;
        }

        // Thick contrasting edges expose crosstalk and local shutter
        // inversion; the inner mark also tests fine stereoscopic detail.
        stereo_ruler_rect(ra, tex, outline, x - stroke, y - stroke,
                          x + width + stroke, y + height + stroke);
        stereo_ruler_rect(ra, tex, face, x, y, x + width, y + height);
        stereo_ruler_rect(ra, tex, inset, x + 7 * stroke,
                          y + height / 2 - stroke, x + width - 7 * stroke,
                          y + height / 2 + stroke);
        // Distinct digits 0..6 are part of the binocular object,
        // not stationary OSD. They must move with its disparity.
        char digit[2] = {(char)('0' + i), 0};
        stereo_ruler_text(ra, tex, x + 3 * stroke,
                          y + (height - scale * 5) / 2,
                          scale, ink, digit);
    }
}

#endif

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct gpu_priv *p = vo->priv;
    struct ra_swapchain *sw = p->ctx->swapchain;

#if HAVE_DRM
    if (p->stereo_pair) {
        // The calibration pair follows the SAME EGL/native-fence/GBM/KMS
        // pipeline as a movie pair. Once published, the existing KMS worker
        // scans it out forever, alternating eyes at each physical MSC.
        if (p->stereo_calibration_pattern >= 0) {
            if (p->stereo_calibration_drawn)
                return VO_TRUE;
            if (!vo_drm_stereo_pair_can_capture(p->ctx))
                return VO_TRUE;
            int pat = p->stereo_calibration_pattern;
            uint64_t before = vo->drm->stereo_pair_rendered;
            // Do not attribute calibration frames to movie frame-id/PTS plans.
            vo_drm_stereo_pair_set_frame_info(p->ctx, 0, 0, false, 0, false);
            for (int eye = 0; eye < 2; eye++) {
                if (!vo_drm_stereo_pair_begin_eye(p->ctx, eye))
                    return VO_FALSE;
                struct ra_fbo fbo;
                if (!sw->fns->start_frame(sw, &fbo))
                    return VO_FALSE;
                bool ladder = pat == 2;
                bool white = (pat == 0 && eye == 0) ||
                             (pat == 1 && eye == 1);
                // The ladder has a neutral dark background in BOTH eyes;
                // the bars (and their labels) carry per-eye disparity.
                float level = ladder ? 0.07f : (white ? 1.0f : 0.0f);
                float color[4] = {level, level, level, 1.0f};
                // ra_gl.gl_clear() unconditionally dereferences scissor.
                // NULL here crashed mpv with SIGSEGV on the first pattern.
                struct mp_rect full = {
                    .x0 = 0, .y0 = 0,
                    .x1 = fbo.tex->params.w,
                    .y1 = fbo.tex->params.h,
                };
                p->ctx->ra->fns->clear(p->ctx->ra, fbo.tex, color, &full);
                if (ladder) {
                    stereo_calibration_draw_depth_ladder(p->ctx, fbo.tex, eye);
                    // For the depth test the ruler is common to both eyes
                    // (zero disparity), unlike the complementary BW scale.
                    stereo_calibration_draw_ruler(p->ctx, fbo.tex, true);
                } else {
                    stereo_calibration_draw_ruler(p->ctx, fbo.tex, white);
                }
                // The pair remains fixed for repeated KMS scanouts.
                vo->drm->stereo_draw_eye = eye;
                ++vo->drm->stereo_draws;
                vo_drm_stereo_pair_note_eye_render(p->ctx, eye, 0);
                if (!sw->fns->submit_frame(sw, frame))
                    return VO_FALSE;
                sw->fns->swap_buffers(sw);
            }
            vo_drm_stereo_pair_begin_eye(p->ctx, -1);
            if (vo->drm->stereo_pair_rendered == before)
                return VO_FALSE;
            p->stereo_calibration_drawn = true;
            return VO_TRUE;
        }
        // The VO thread remains mpv's decoder-side producer. An independent
        // KMS thread repeatedly scans out the last completed L/R pair.
        // Don't draw twice at 120 Hz when the source only changes at ~24 Hz.
        if (!frame->current || (p->stereo_pair_valid &&
            p->stereo_pair_last_frame_id == frame->frame_id))
            return VO_TRUE;
        if (!vo_drm_stereo_pair_can_capture(p->ctx))
            return VO_TRUE; // Keep the last good pair rather than blocking KMS.

        // vo_frame.pts is a REALTIME scheduling target, and is zeroed for
        // display-synced frames. Use the source mp_image PTS in seconds.
        // frame_id is mpv's stable ID for the current image, including
        // redraws (which are already suppressed above).
        const double media_pts = frame->current->pts;
        // vo.c passes a copy of the first draw with the original requested
        // num_vsyncs, before it decrements its own repeat counter. Do not
        // use a later redraw/repeat's remaining num_vsyncs as the plan.
        vo_drm_stereo_pair_set_frame_info(p->ctx, frame->frame_id,
                                          media_pts,
                                          isfinite(media_pts) &&
                                          media_pts != MP_NOPTS_VALUE,
                                          frame->num_vsyncs,
                                          frame->display_synced);

        const uint64_t previous_pairs = vo->drm->stereo_pair_rendered;
        for (int eye = 0; eye < 2; eye++) {
            if (!vo_drm_stereo_pair_begin_eye(p->ctx, eye))
                return VO_FALSE;
            struct ra_fbo eye_fbo;
            if (!sw->fns->start_frame(sw, &eye_fbo))
                return VO_FALSE;
            gl_video_set_sbs_eye(p->renderer, p->stereo_full_src, eye);
            vo->drm->stereo_draw_eye = eye;
            ++vo->drm->stereo_draws;
            int64_t render_start_ns = mp_time_ns();
            gl_video_render_frame(p->renderer, frame, &eye_fbo, RENDER_FRAME_DEF);
            vo_drm_stereo_pair_note_eye_render(p->ctx, eye,
                                                 mp_time_ns() - render_start_ns);
            if (!sw->fns->submit_frame(sw, frame))
                return VO_FALSE;
            // Captures the locked GBM BO, but does NOT queue a KMS flip.
            sw->fns->swap_buffers(sw);
        }
        vo_drm_stereo_pair_begin_eye(p->ctx, -1); // stop capturing
        if (vo->drm->stereo_pair_rendered == previous_pairs)
            return VO_FALSE; // Do not silently accept an incomplete eye pair.
        p->stereo_pair_valid = true;
        p->stereo_pair_last_frame_id = frame->frame_id;

        struct mp_image_params *params =
            gl_video_get_target_params_ptr(p->renderer);
        mp_mutex_lock(&vo->params_mutex);
        vo->target_params = params;
        mp_mutex_unlock(&vo->params_mutex);
        return VO_TRUE;
    }
#endif

    struct ra_fbo fbo;
    if (!sw->fns->start_frame(sw, &fbo))
        return VO_FALSE;

    if (p->stereo_test && frame->current) {
        // The experimental mode accepts half SBS without 90/270-degree
        // rotation. Keep mpv's regular full-image viewport and select
        // the eye via a lightweight source crop. Critically, do NOT call
        // gl_video_resize() every 8.33ms: that resets the GPU caches.
        int eye = p->stereo_draw_count & 1;
#if HAVE_DRM
        struct vo_drm_state *drm = vo->drm;
        drm->stereo_draw_target_valid = false;
        if (drm->stereo_slot_mode && drm->stereo_have_flip) {
            // The GBM queue contains the current scanout followed by
            // already-rendered buffers. Predict the MSC of this *new*
            // buffer, not the MSC of the current draw callback.
            const uint32_t target = drm->stereo_last_msc +
                                    drm->stereo_queue_ahead + 1u;
            eye = drm->stereo_base_eye ^
                  ((target - drm->stereo_base_msc) & 1u);
            drm->stereo_draw_target_msc = target;
            drm->stereo_draw_target_valid = true;
        } else if (drm->stereo_recover && !drm->stereo_slot_mode) {
            eye ^= drm->stereo_phase_correction;
        }
        drm->stereo_draw_time_ns = mp_time_ns();
#endif
        gl_video_set_sbs_eye(p->renderer, p->stereo_full_src, eye);
#if HAVE_DRM
        vo->drm->stereo_draw_eye = eye;
        ++vo->drm->stereo_draws;
#endif
        ++p->stereo_draw_count;
    }

    gl_video_render_frame(p->renderer, frame, &fbo, RENDER_FRAME_DEF);
    if (!sw->fns->submit_frame(sw, frame)) {
        MP_ERR(vo, "Failed presenting frame!\n");
        return VO_FALSE;
    }

    struct mp_image_params *params = gl_video_get_target_params_ptr(p->renderer);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = params;
    mp_mutex_unlock(&vo->params_mutex);

    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct gpu_priv *p = vo->priv;
    struct ra_swapchain *sw = p->ctx->swapchain;
#if HAVE_DRM
    if (p->stereo_pair) {
        // The independently running KMS worker advances each physical
        // vblank. mpv's display-resample scheduler still needs blocking
        // presentation feedback once per VO flip to maintain AV speed.
        vo_drm_stereo_pair_wait_vsync(p->ctx);
        return;
    }
#endif
    sw->fns->swap_buffers(sw);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct gpu_priv *p = vo->priv;
    struct ra_swapchain *sw = p->ctx->swapchain;
    if (sw->fns->get_vsync)
        sw->fns->get_vsync(sw, info);
}

static int query_format(struct vo *vo, int format)
{
    struct gpu_priv *p = vo->priv;
    if (!gl_video_check_format(p->renderer, format))
        return 0;
    return 1;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct gpu_priv *p = vo->priv;

    if (!p->ctx->fns->reconfig(p->ctx))
        return -1;

    resize(vo);
    p->stereo_pair_valid = false;
    if (p->stereo_test && (params->w < 2 || (params->w & 1)
                            || params->rotate % 180)) {
        MP_ERR(vo, "DRM stereo test requires even-width, unrotated SBS frames\n");
        return -1;
    }
    gl_video_config(p->renderer, params);

    return 0;
}

static void request_hwdec_api(struct vo *vo, void *data)
{
    struct gpu_priv *p = vo->priv;
    gl_video_load_hwdecs_for_img_fmt(p->renderer, vo->hwdec_devs, data);
}

static void call_request_hwdec_api(void *ctx,
                                   struct hwdec_imgfmt_request *params)
{
    // Roundabout way to run hwdec loading on the VO thread.
    // Redirects to request_hwdec_api().
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, params);
}

static void get_and_update_icc_profile(struct gpu_priv *p)
{
    if (gl_video_icc_auto_enabled(p->renderer)) {
        MP_VERBOSE(p, "Querying ICC profile...\n");
        bstr icc = bstr0(NULL);
        int r = p->ctx->fns->control(p->ctx, &p->events, VOCTRL_GET_ICC_PROFILE, &icc);

        if (r != VO_NOTAVAIL) {
            if (r == VO_FALSE) {
                MP_WARN(p, "Could not retrieve an ICC profile.\n");
            } else if (r == VO_NOTIMPL) {
                MP_ERR(p, "icc-profile-auto not implemented on this platform.\n");
            }

            gl_video_set_icc_profile(p->renderer, icc);
        }
    }
}

static void get_and_update_ambient_lighting(struct gpu_priv *p)
{
    double lux;
    int r = p->ctx->fns->control(p->ctx, &p->events, VOCTRL_GET_AMBIENT_LUX, &lux);
    if (r == VO_TRUE) {
        gl_video_set_ambient_lux(p->renderer, lux);
    }
    if (r != VO_TRUE && gl_video_gamma_auto_enabled(p->renderer)) {
        MP_ERR(p, "gamma_auto option provided, but querying for ambient"
                  " lighting is not supported on this platform\n");
    }
}

static void update_ra_ctx_options(struct vo *vo, struct ra_ctx_opts *ctx_opts)
{
    struct gpu_priv *p = vo->priv;
    struct gl_video_opts *gl_opts = mp_get_config_group(p->ctx, vo->global, &gl_video_conf);
    ctx_opts->want_alpha = (gl_opts->background == BACKGROUND_COLOR &&
                            gl_opts->background_color.a != 255) ||
                            gl_opts->background == BACKGROUND_NONE;
    talloc_free(gl_opts);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct gpu_priv *p = vo->priv;

    switch (request) {
    case VOCTRL_STEREO_CALIBRATION: {
#if HAVE_DRM
        if (!p->stereo_pair || !data)
            return VO_FALSE;
        int pattern = *(int *)data;
        if (pattern < -1 || pattern > 2)
            return VO_FALSE;
        if (pattern != p->stereo_calibration_pattern) {
            // Detach pending/queued but retain active and in-flight pairs.
            // Never reset the physical MSC counter or USB synchronization.
            vo_drm_stereo_pair_reset(p->ctx);
            p->stereo_calibration_pattern = pattern;
            p->stereo_calibration_drawn = false;
            p->stereo_pair_valid = false;
            vo->want_redraw = true;
        }
        return VO_TRUE;
#else
        return VO_FALSE;
#endif
    }
    case VOCTRL_STEREO_USB_TIMER_ADJUST:
#if HAVE_DRM
        return p->stereo_pair && data &&
               vo_drm_stereo_usb_timer_adjust(p->ctx, *(int *)data);
#else
        return VO_FALSE;
#endif
    case VOCTRL_SET_PANSCAN:
        resize(vo);
        return VO_TRUE;
    case VOCTRL_SCREENSHOT: {
        struct vo_frame *frame = vo_get_current_vo_frame(vo);
        if (frame)
            gl_video_screenshot(p->renderer, frame, data);
        talloc_free(frame);
        return true;
    }
    case VOCTRL_LOAD_HWDEC_API:
        request_hwdec_api(vo, data);
        return true;
    case VOCTRL_UPDATE_RENDER_OPTS: {
        struct ra_ctx_opts *ctx_opts = mp_get_config_group(vo, vo->global, &ra_ctx_conf);
        update_ra_ctx_options(vo, ctx_opts);
        gl_video_configure_queue(p->renderer, vo);
        get_and_update_icc_profile(p);
        if (p->ctx->fns->update_render_opts)
            p->ctx->fns->update_render_opts(p->ctx);
        vo->want_redraw = true;
        talloc_free(ctx_opts);
        return true;
    }
    case VOCTRL_RESET:
        gl_video_reset(p->renderer);
#if HAVE_DRM
        if (p->stereo_pair) {
            vo_drm_stereo_pair_reset(p->ctx);
            p->stereo_pair_valid = false;
            p->stereo_pair_last_frame_id = 0;
            p->stereo_calibration_drawn = false;
        }
#endif
        return true;
    case VOCTRL_PAUSE:
        if (gl_video_showing_interpolated_frame(p->renderer))
            vo->want_redraw = true;
        return true;
    case VOCTRL_PERFORMANCE_DATA:
        gl_video_perfdata(p->renderer, (struct voctrl_performance_data *)data);
        return true;
    case VOCTRL_EXTERNAL_RESIZE:
        p->ctx->fns->reconfig(p->ctx);
        resize(vo);
        return true;
    }

    int events = 0;
    int r = p->ctx->fns->control(p->ctx, &events, request, data);
    if (events & VO_EVENT_ICC_PROFILE_CHANGED) {
        get_and_update_icc_profile(p);
        vo->want_redraw = true;
    }
    if (events & VO_EVENT_AMBIENT_LIGHTING_CHANGED) {
        get_and_update_ambient_lighting(p);
        vo->want_redraw = true;
    }
    events |= p->events;
    p->events = 0;
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return r;
}

static void wakeup(struct vo *vo)
{
    struct gpu_priv *p = vo->priv;
    if (p->ctx && p->ctx->fns->wakeup)
        p->ctx->fns->wakeup(p->ctx);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct gpu_priv *p = vo->priv;
    if (p->ctx && p->ctx->fns->wait_events) {
        p->ctx->fns->wait_events(p->ctx, until_time_ns);
    } else {
        vo_wait_default(vo, until_time_ns);
    }
}

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align, int flags)
{
    struct gpu_priv *p = vo->priv;

    return gl_video_get_image(p->renderer, imgfmt, w, h, stride_align, flags);
}

static void uninit(struct vo *vo)
{
    struct gpu_priv *p = vo->priv;

    gl_video_uninit(p->renderer);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = NULL;
    mp_mutex_unlock(&vo->params_mutex);

    if (vo->hwdec_devs) {
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
    }
    ra_ctx_destroy(&p->ctx);
}

static int preinit(struct vo *vo)
{
    struct gpu_priv *p = vo->priv;
    p->log = vo->log;

    struct ra_ctx_opts *ctx_opts = mp_get_config_group(vo, vo->global, &ra_ctx_conf);
    update_ra_ctx_options(vo, ctx_opts);
    p->ctx = ra_ctx_create(vo, *ctx_opts);
    talloc_free(ctx_opts);
    if (!p->ctx)
        goto err_out;
    mp_assert(p->ctx->ra);
    mp_assert(p->ctx->swapchain);

    p->renderer = gl_video_init(p->ctx->ra, vo->log, vo->global);
    gl_video_set_osd_source(p->renderer, vo->osd);
    gl_video_configure_queue(p->renderer, vo);

    get_and_update_icc_profile(p);

    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_set_loader(vo->hwdec_devs, call_request_hwdec_api, vo);

    gl_video_init_hwdecs(p->renderer, p->ctx, vo->hwdec_devs, false);

    // Public mpv option enables BOTH Half-SBS eye rendering and paired
    // KMS scanout; retain the old experiment variables for A/B comparison.
    // Explicitly reject non-DRM contexts instead of silently ignoring 3D.
    bool native_3d = false;
    int native_calibration = 0;
#if HAVE_DRM
    struct drm_opts *drm_opts = mp_get_config_group(vo, vo->global, &drm_conf);
    native_3d = drm_opts->stereo_3d;
    native_calibration = drm_opts->stereo_calibration;
    talloc_free(drm_opts);
    if (native_3d && (!vo->drm || strcmp(p->ctx->fns->name, "drm") != 0)) {
        MP_ERR(vo, "--drm-stereo-3d=yes requires --gpu-context=drm\n");
        goto err_out;
    }
#endif
    const char *stereo_env = getenv("MPV_DRM_STEREO_SBS");
    p->stereo_test = native_3d ||
        (stereo_env && strcmp(stereo_env, "1") == 0);
    const char *pair_env = getenv("MPV_DRM_STEREO_PAIR");
    p->stereo_pair = native_3d ||
        (pair_env && strcmp(pair_env, "1") == 0);
    const char *calib = getenv("MPV_DRM_STEREO_CALIBRATION");
    p->stereo_calibration_pattern =
        native_3d && (native_calibration == 1 ||
                       (calib && strcmp(calib, "1") == 0)) ? 0 : -1;
    if (p->stereo_pair && !p->stereo_test) {
        MP_ERR(vo, "MPV_DRM_STEREO_PAIR requires MPV_DRM_STEREO_SBS=1\n");
        goto err_out;
    }
    if (p->stereo_test) {
#if HAVE_DRM
        if (strcmp(p->ctx->fns->name, "drm") != 0 || !vo->drm) {
            MP_ERR(vo, "MPV_DRM_STEREO_SBS=1 requires --gpu-context=drm\n");
            goto err_out;
        }
        double hz = vo_drm_get_display_fps(vo->drm);
        if (fabs(hz - 120.0) > 0.25) {
            MP_ERR(vo, "DRM stereo test requires a 120 Hz output (got %.3f)\n", hz);
            goto err_out;
        }
        vo->drm->stereo_test_enabled = true;
        vo->drm->stereo_draw_eye = -1;
        vo->drm->stereo_flip_eye = -1;
        vo->drm->stereo_last_eye = -1;
        const char *recover = getenv("MPV_DRM_STEREO_RECOVER");
        vo->drm->stereo_recover = !p->stereo_pair && recover &&
                                  strcmp(recover, "1") == 0;
        const char *slot = getenv("MPV_DRM_STEREO_SLOT");
        vo->drm->stereo_slot_mode = !p->stereo_pair &&
                                      slot && strcmp(slot, "1") == 0;
        vo->drm->stereo_pair_mode = p->stereo_pair;
        MP_INFO(vo, "Experimental DRM SBS at %.3f Hz "
                "(NO USB, independent paired KMS scanout %s, slot=%s)\n",
                hz, p->stereo_pair ? "on" : "off",
                vo->drm->stereo_slot_mode ? "on" : "off");
        if (native_3d)
            MP_INFO(vo, "DRM stereo 3D: physical %.3f Hz L/R, "
                    "logical %.3f Hz mpv pacing on %s callback "
                    "(presentation timestamps remain tied to L; "
                    "native GPU fences on)\n",
                    hz, hz / 2.0,
                    vo->drm->opts->stereo_pacing_eye ? "R" : "L");
#else
        MP_ERR(vo, "MPV_DRM_STEREO_SBS=1 was built without DRM support\n");
        goto err_out;
#endif
    }

    return 0;

err_out:
    uninit(vo);
    return -1;
}

const struct vo_driver video_out_gpu = {
    .description = "Shader-based GPU Renderer",
    .name = "gpu",
    .caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct gpu_priv),
};
