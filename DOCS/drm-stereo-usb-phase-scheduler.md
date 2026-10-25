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
