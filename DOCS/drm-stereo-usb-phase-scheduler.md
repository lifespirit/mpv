# Predicted DRM/MSC USB shutter phase (experimental)

Branch: `feature/drm-stereo-usb-phase-scheduler`.
The validated `feature/drm-stereo-native-calibration` branch is **unchanged**.

## Design

The movie and calibration patterns still use the existing independently paced
120 Hz DRM/KMS physical L/R worker with QUEUE2 + PLAN_HOLD. The KMS page-flip
callback reports a confirmed eye, MSC and DRM vblank UST. It only publishes
that timestamp and signals the existing eventfd; **no timerfd or libusb work
occurs in the real-time KMS thread**.

In the USB sender thread, predict the *next* one or two physical MSC edges
from this confirmed MSC/UST and the measured display refresh rate:

```text
target_ust_ns = confirmed_ust_us * 1000
              + (target_msc - confirmed_msc) * round(1e9 / physical_refresh_hz)
              + usb_command_phase_us * 1000
```

Use `timerfd_create(CLOCK_MONOTONIC)` and
`timerfd_settime(TFD_TIMER_ABSTIME)` to schedule the **start** of
`nvstusb_send_eye(next_eye)`. A negative phase sends before the corresponding
vblank; a positive phase sends after it. Current KMS MSC/UST is the only
clock anchor: if confirmations stop, the scheduler stops generating commands.

The firmware eye timer stays at the **rate-derived default** (roughly 4629
microseconds at 120 Hz under libnvstusb's historical /1.8 calculation).
The eye duration/timer is not user-tunable in this mode; only the host
command-send phase is adjusted. This is NOT a direct optical opening-delay
measurement, nor a USB controller microframe reservation.

The sender discards stale deadlines instead of blindly transmitting an old
eye command. It also checks confirmed MSC and eye parity where possible.
The sender is still normal-priority, and libusb delivery time remains
nondeterministic. Missed deadlines and command-start timing errors are logged.

## Native run

Build this branch, using the existing rebuilt
`lifespirit/libnvstusb` branch `feature/usb-eye-timer-calibration`.
Importantly, **unset `NVSTUSB_EYE_TIMER_US`**. A leftover timer override
will cause the optical run to be rejected, deliberately.

```bash
unset NVSTUSB_EYE_TIMER_US

MPV_DRM_STEREO_USB=1 \
MPV_DRM_STEREO_USB_PHASE_SCHED=1 \
MPV_DRM_STEREO_USB_PHASE_US=-1500 \
MPV_DRM_STEREO_USB_LIBRARY=/absolute/path/to/libnvstusb.so \
MPV_DRM_STEREO_USB_FIRMWARE=/absolute/path/to/nvstusb.fw \
./build/mpv --no-config \
  --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode=2560x1440@120 \
  --drm-vrr-enabled=no --drm-stereo-3d=yes \
  --drm-stereo-pacing-eye=right \
  --drm-stereo-calibration=before-play \
  --video-sync=display-resample \
  --log-file=/tmp/mpv-stereo-phase.log \
  /absolute/path/to/movie.mkv
```

Do not redirect stdout with `> /tmp/log` if you intend to operate
the mpv terminal keyboard: upstream mpv disables terminal key reading
when stdout is not a TTY. `--log-file` preserves keyboard input.

`MPV_DRM_STEREO_USB_CONTROL=1` is **not required** in the new mode. If
present, the old firmware-timer FIFO is disabled, while mpv's native hotkeys
control the command phase directly.

## Calibration hotkeys

In before-play mode, the generated L-white/R-black image is held while
the **physical** KMS/USB eye alternation keeps running.

### Optical drift measurement ruler

This experimental branch adds a persistent **right-edge 0–100% height
ruler** to the synthetic calibration screen (0% at the top, 100% at the
bottom). Small ticks are 1% apart, medium ticks 5% apart, and bright
numbered ticks are 10% apart. The sidebar is 150 pixels wide at
2560x1440.

The ruler is deliberately **complementary between the eye buffers**:
the white calibration eye gets a black ruler with white tick marks and
labels; the black calibration eye gets a white ruler with black tick
marks and labels. The ruler follows the actual calibration pattern,
including the 1/2 eye-swap controls and the both-white/both-black
patterns. Its opposite-contrast areas can make optical cross-talk
and phase boundary motion easier to compare.

The left/right buffers are drawn once per pattern, then KMS repeats
the same pair. No scale is present in movie frames, and no per-vblank
ruler rendering occurs. Because the ruler now alternates between
opposite-contrast fields, it is itself sensitive to eye cross-talk,
rather than acting as a common binocular reference.

To compare against a completely unchanged all-white/all-black
calibration screen, use `MPV_DRM_STEREO_CALIBRATION_RULER=0`.
The scale is enabled by default in this branch.

To measure drift, hold the white/black image for 3-5 minutes without
adjusting the phase arrows. Note where the **moving boundary in the
main image** crosses the numbered scale after 0, 60, 120, 180 and 240
seconds. The complementary marks can help locate the transition, but
also compare against the adjacent flat calibration field to avoid
mistaking LCD response / shutter cross-talk for a timing change.


- LEFT: -100 us command phase (earlier).
- RIGHT: +100 us command phase (later).
- UP: +25 us command phase (later).
- DOWN: -25 us command phase (earlier).
- 1/2: L-white/R-black or L-black/R-white.
- 3/4: both white / both black.
- Enter / Esc: leave the test screen and play the film.
- `c` during film: enable/disable live phase adjustment.
- `C` during film: enter/exit calibration screen while retaining KMS cadence.

Initial `MPV_DRM_STEREO_USB_PHASE_US` defaults to -1500. Accepted phase
range is -4000 to +4000 us. Adjustments take effect in the USB scheduler,
without changing firmware timer, KMS parity, film clock or QUEUE2/PLAN_HOLD.

## Experimental linear drift compensation (separate branch)

On `feature/drm-stereo-usb-phase-ramp`, retain the complementary
calibration ruler from `feature/drm-stereo-calibration-ruler`.
An **optional** signed host-only phase ramp lets us test whether
gradually shifting `nvstusb_send_eye()` relative to physical KMS MSC
cancels the observed ~5% screen-height/minute optical drift.

```text
effective_command_phase(t) = manual_phase_us + drift_us_per_s * elapsed_seconds
```

`elapsed_seconds` starts at the **first confirmed physical KMS flip**;
we integrate in CLOCK_MONOTONIC using that flip's UST and retain
nanosecond precision in the calculated `timerfd` deadline. No firmware
timer, KMS flip, eye parity, movie PTS or screen frame rate is altered.
Manual arrow controls adjust only the base phase and do **not** reset
the drift integration clock.

Set `MPV_DRM_STEREO_USB_DRIFT_US_PER_S` to a finite decimal number in
`-20..+20` **with**
`MPV_DRM_STEREO_USB=1 MPV_DRM_STEREO_USB_PHASE_SCHED=1`.
Positive values gradually send USB commands later; negative values
send earlier. Omit it or set `0` for exactly the previous behavior.
The example +7.0us/s corresponds to +420us/min (about 5% of a
120Hz scanout), **but the optical sign has not been established**.

For the first controlled A/B study, keep everything else unchanged,
including the same firmware, USB port, display mode, `--no-config`,
`--drm-stereo-calibration=before-play`, the `L-white/R-black` pattern,
`MPV_DRM_STEREO_USB_PHASE_US=-1500`, and an **unset**
`NVSTUSB_EYE_TIMER_US`. In particular, reset the earlier Timer 2
experiment to `NVSTUSB_FRAME_TIMER_EXACT=1` and
`NVSTUSB_FRAME_TIMER_TICK_BIAS=0` for **all three runs**. Use three
independent launches of the same 3-minute stationary calibration pattern:

- A: `MPV_DRM_STEREO_USB_DRIFT_US_PER_S=0`
- B: `MPV_DRM_STEREO_USB_DRIFT_US_PER_S=+7`
- C: `MPV_DRM_STEREO_USB_DRIFT_US_PER_S=-7`

At 0, 60, 120, 180s, note the white/black transition height in
percent, separately for each eye. Do not adjust arrows *during each
measurement*; you can tune a common starting phase before starting a
fresh run. If one sign produces clearly less drift, refine its slope
(e.g. 5, 6, 8 or 9us/s) in another experiment.

**Safety / limiting behavior:** The effective host phase stays between
-4000 and +4000us relative to its TARGET vblank. If the requested
manual plus drift offset exceeds either bound, the software **clamps**
the phase and logs an explicit warning instead of silently wrapping
the deadline to another MSC (which could invert the eye mapping).
The ramp is therefore a SHORT DIAGNOSTIC, not yet a fix for a full movie.
For example, starting at -1500us with -7us/s reaches -4000us after
~357s. If a limit warning appears, STOP and restart; do not conclude
that drift compensation has stopped working beyond its valid range.

Every 30s the USB worker logs:

```text
Stereo USB drift sample: elapsed_s=... rate_us_per_s=... manual_phase_us=...
correction_us=... effective_phase_us=... clamped=0
```

On exit it reports `Stereo USB drift final` with cumulative correction
and `limit_entries`. These are **HOST command scheduling times**,
not measured IR transmission or lens opening. The existing
`Stereo USB scheduler final` remains unchanged for comparison.

Hardware-free arithmetic check:

```sh
cc -std=c11 -Wall -Wextra -Werror -o /tmp/stereo-phase-math \
  TOOLS/stereo_usb_phase_ramp_math_test.c
/tmp/stereo-phase-math
```

With DRM hardware but *without* the IR emitter, exercise the updated
USB-dry-run smoke path:

```sh
python3 TOOLS/drm_stereo_native_calibration_smoke.py \
    --phase-scheduler --drift-us-per-s=7 \
    --mpv ./build/mpv '/absolute/path/to/movie.mkv'
```

Neither test can prove optical correction. They test only integration
and deadline arithmetic; observing the height ruler through the real
shutters (ideally with a camera) remains necessary.

## Timestamped exceptional USB events

On `feature/drm-stereo-usb-phase-event-trace` (built on the phase-ramp
branch), enable diagnostic logging for exceptional USB-worker events:

```sh
export MPV_DRM_STEREO_USB_EVENT_TRACE=1
```

This emits `Stereo USB event:` lines with `elapsed_s`, a reason, target
and last observed MSC, intended target eye, and
`host_now_minus_deadline_us`. Event types:

- `mailbox_msc_skip` — sender did not consume every confirmed MSC
  (may represent a slow USB worker, not necessarily a DRM missed flip).
- `late_dispatch`, `obsolete_target` — armed timer expired too late
  or after its target slot passed.
- `stale_target`, `target_eye_mismatch` — event mailbox invalidated
  an armed target.
- `target_too_late_to_arm`, `manual_rearm_too_late` — target deadline
  already too close to or past the usable arm window.
- `slow_send_eye` — a single synchronous USB invocation lasted at
  least 2ms; this could disturb subsequent sends.

Only the **USB sender thread** logs these rare exceptional events;
the KMS flip callback is unchanged. The logging itself can slightly
increase rare-event latency, so compare results against the
`MPV_DRM_STEREO_USB_EVENT_TRACE=0` baseline. It is disabled by
default, and neither the algorithm nor eye parity is changed.

Compare event `elapsed_s` values with wallclock observations such
as a sudden optical inversion at 459 seconds. The underlying optics
are not timestamped by the host, so matching timestamps are
correlation rather than proof that an event caused a shutter
resynchronization.

```sh
grep 'Stereo USB event:' /tmp/mpv-stereo-phase.log
```

If no event appears near the inversion, it strengthens the case for
a transmitter/IR/glasses phase mechanism beyond the host's
observable KMS/USB dispatch failures.

## Diagnostics and first test (do not use glasses for dry-run)

```bash
python3 TOOLS/drm_stereo_native_calibration_smoke.py \
  --phase-scheduler --mpv ./build/mpv \
  '/absolute/path/to/movie.mkv'
```

The script uses dry-run and checks all four arrow keys, pause/restart,
KMS accounting, phase adjustment count, predicted arm/send/reanchor
activity. This **does not** demonstrate optical timing.

On native exit, check `Stereo USB scheduler final`:
`phase_us`, `fixed_eye_timer_us`, `phase_adjustments`, `phase_armed`,
`phase_sent`, `deadline_misses`, `stale_drops`,
`parity_drops`, `too_late_to_arm`, `reanchors`,
`min/avg/max_dispatch_error_us`.

If optical alignment still changes with random restarts despite stable
predicted deadlines, hardware USB delivery / firmware IR response may be
the source; host-side timerfd cannot force an exact on-device transition.

## Regression / rollback

To use the original confirmed-flip immediate sender, omit
`MPV_DRM_STEREO_USB_PHASE_SCHED=1`. For exactly the last optically tested
software, switch back to `feature/drm-stereo-native-calibration` at
`1389e238a8e3894906abc83b90adec4f43854ef2`.

**Build not verified here:** cross-check with `meson compile -C build` on
the target Arch/DRM system before attempting optical experiments.
