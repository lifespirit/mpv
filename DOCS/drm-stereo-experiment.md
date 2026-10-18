# Experimental DRM half-SBS frame-sequential output (no USB)

This is the first implementation checkpoint on branch
`feature/drm-stereo-diagnostics`. It is an **opt-in visual/DRM timing
experiment**: it does not load `libnvstusb`, does not address the
emitter, and must not be used with active 3D glasses.

## Scope

- Reuse mpv's existing `--vo=gpu --gpu-context=drm` EGL/GBM direct
  output on a **120 Hz fixed-rate** connector.
- Treat each half-SBS frame as two 960x1080 (for 1920x1080 inputs)
  eye images. Expand one half to the full output viewport.
- Pick the left or right half on *each mpv display-synchronized draw*,
  using an alternating software sequence **for this first checkpoint**.
  Reusing the same decoded movie frame is expected.
- Carry the chosen eye alongside each GBM framebuffer in
  `context_drm_egl.c`, not just alongside the draw call.
- On the actual KMS `drm_pflip_cb`, report measured physical
  presentation rate, missed vblank slots, and eye/slot parity errors.
  The DRM callback contains the KMS sequence `msc` and a timestamp.

**Important limitation:** a successful `draw_frame` is not
equivalent to a successful KMS page flip. mpv's ordinary
`--video-sync=display-resample` redraw behavior is still
responsible for issuing repeated frames. This checkpoint validates
whether that is sufficient; it does **not** enforce 120 successful
page flips per second. Left/right alternation follows render calls,
not hardware slots, and intentionally reports resulting mistakes.
The next implementation must schedule the eye by the actual next
DRM slot, independent of source-frame delivery, with safe recovery.

## Build (Arch Linux)

Use the system mpv build dependencies plus Meson/Ninja. In the fork:

```sh
git fetch origin
git switch feature/drm-stereo-diagnostics
meson setup build --buildtype=release
meson compile -C build
```

If your existing `build` directory belongs to another configuration,
choose a new one such as `build-drm-stereo`.

## First run, TTY only

Switch away from KDE to a free local TTY, as you did for the
working unmodified mpv DRM run. In the checkout:

```sh
MPV_DRM_STEREO_SBS=1 ./build/mpv --no-config \
    --vo=gpu --gpu-context=drm \
    --drm-connector=DP-1 --drm-mode=2560x1440@120 \
    --drm-vrr-enabled=no \
    --video-sync=display-resample --interpolation=no \
    --hwdec=auto \
    "/path/to/half-sbs-video.mkv" \
    2>&1 | tee /tmp/mpv-drm-stereo-no-usb.log
```

If Meson places the executable elsewhere, use its actual generated path.

Make sure `[vo/gpu] Experimental DRM SBS L/R alternation ... (NO USB)`
appears. The image should switch between left/right images, so it
will look visually wrong without shutter glasses. This is expected.

The DRM backend logs:

```
DRM SBS test: flips=120 shown_hz=... missed_vblanks=...
parity_errors=... draws=... commit_errors=... msc=... eye=...
```

Successful baseline requires sustained ~120 physical flips/s,
`missed_vblanks=0`, `parity_errors=0`, and
`commit_errors=0`. These are *diagnostic gates*, not guaranteed
real-time properties. The log includes cumulative counters. An
occasional startup anomaly can be separated from steady playback.

## Limitations and safety

- Half SBS only. Top-bottom, separate-eye streams, eye-specific
  subtitles and presentation-time eye selection are not yet
  implemented.
- Use `--vo=gpu`, not `--vo=gpu-next`. The test deliberately fails
  outside DRM/EGL and if the selected output frequency is not within
  0.25 Hz of 120.
- Pauses, seek, mode changes, or missed frame deadlines can break
  parity. The first test makes those problems measurable.
- Screen mode is fixed at 120 Hz, but the frame sequence is not
  yet guaranteed to update every vblank. No USB synchronization
  or optical shutter waveform is implemented.
- Do not wear glasses until the real emitter timing and optical
  latency have been validated separately. Previously observed
  right-lens flicker can cause substantial eye strain.

## First real-world no-USB run (2026-10-18)

The tested direct KMS mode was **119.998 Hz**, VAAPI hwdec active.
The initial experimental build still called `gl_video_resize()` in
every 120-Hz eye draw, which invalidated the GPU renderer's temporal
surfaces/cache on every eye transition. This is an unnecessary and
potentially severe rendering overhead, not a DRM requirement.

Hardware confirmation from the user's first 31-second log:

- At 720 KMS flips: 2 missed physical refreshes, 0 eye parity errors.
- At 3120 flips (~26 s): 12 missed refreshes, 10 parity errors,
  and 0 atomic commit errors.
- Late in the run, mpv's audio/video DS dropped-frame counter rose
  rapidly while draw attempts (4351) diverged from confirmed KMS
  flips (3240); the cumulative hardware counter then reached
  854 missed refreshes. This is *not* an optical observation.
  That one late presentation gap heavily biases the final
  cumulative `shown_hz=105.4`; earlier steady-rate windows
  were close to 120.

### Follow-up change

The render path now calls `gl_video_set_sbs_eye()` instead of
`gl_video_resize()` on each eye. The helper changes only the
video's horizontal source crop and invalidates the cached final-eye
texture, **without resetting the entire GPU surface history**.
The full-image geometry is computed once on resize instead of
on every eye flip.

The KMS report also includes `window_hz`, `window_missed`,
`window_parity`, and `max_gap`, independent from the
long-running cumulative `shown_hz`. Individual missed
flip intervals are logged with both MSC values and eye IDs.

The original behavior still has major limitations:
- The eye remains selected by the *render attempt count*, rather than
  actual CRTC MSC parity.
- mpv's default playback pacing is still responsible for issuing
  draw calls, and can miss cycles under decoding or other load.
- This is **NOT** yet a trustworthy real-time 3D shutter engine.

Rebuild and repeat the exact same 30-second no-USB dry test on the
`feature/drm-stereo-diagnostics` branch. If the new `window_hz`
is consistently near 119.998 with no missed cycles, proceed
to hardware-slot-based eye selection. If it still misses, instrument
buffer-queue latency and decouple the stereoscopic compositor refresh
from mpv's movie-frame schedule.

## Second real-world no-USB test: fast crop and hardware phase recovery

The second no-USB test ran for approximately **93 seconds**, with
**11,160** KMS-confirmed page flips, **27** missed physical refresh
slots (including 2 at startup), **23** incremental adjacent-eye
parity anomalies, and **zero** atomic commit errors. In **83 of the
92 one-second windows after startup**, no refresh or adjacent-parity
anomaly occurred; the measured per-window presentation rate was
virtually exactly 120.000Hz. Misses came in bursts around 33, 46-50,
56-57 and 87-88 seconds. The prolonged >1000-buffers-without-KMS
flips failure from v1 was not reproduced.

The remaining bug is that our eye alternation still counts draw
attempts, rather than absolute CRTC scanout slots. An odd-length
physical slip can place the wrong eye on all later slots, even if
subsequent DRM page flips are perfectly 120Hz.

New v3 diagnostics:

- `wrong_eye` = confirmed page flips whose tagged buffer eye does
  NOT match the *absolute* CRTC MSC parity relative to first
  confirmed flip. Unlike the original adjacent `parity_errors`,
  this detects persistent inverted eye phase after one slip.
- `wrong_hold` = skipped physical vblank slots where the previous
  buffer remained shown but its eye did not match that physical
  slot. Such errors cannot be undone by later recovery.
- `window_wrong_eye` and `window_wrong_hold` = per-report
  increments of the two counters.
- `MPV_DRM_STEREO_RECOVER=1` = **opt-in**, no-USB, phase recovery
  experiment. The page-flip callback detects an odd number of
  skipped scanout slots and adjusts the software L/R draw parity
  for following submissions. A frame already submitted cannot
  be changed, so errors remain possible for one or more flips
  following a missed deadline.
- `corrections` = number of observed odd-slot recovery actions;
  `recovery=on|off` identifies the selected mode.

Run a 90-120 second no-USB test from TTY:

```sh
MPV_DRM_STEREO_SBS=1 MPV_DRM_STEREO_RECOVER=1 ./build/mpv \
    --no-config --vo=gpu --gpu-context=drm \
    --drm-connector=DP-1 --drm-mode=2560x1440@120 \
    --drm-vrr-enabled=no --video-sync=display-resample \
    --interpolation=no --hwdec=auto \
    "/path/to/half-sbs-video.mkv" \
    2>&1 | tee /tmp/mpv-drm-stereo-recover.log
```

Compare with `MPV_DRM_STEREO_RECOVER=0` if needed. This only
corrects the eye of **future rendered buffers**, not the hardware
refresh rate or KMS submission/presentation jitter. There may be
in-flight buffered frames: success is judged by the measured
`wrong_eye`, not by assuming all mismatches have been eliminated.
A production player ultimately requires an independent
scanout-driven stereo timing loop and USB phase control. Keep
the 3D Vision emitter disconnected and do not wear the glasses.

## Third real-world no-USB test: 63 seconds of phase recovery

The first opt-in `MPV_DRM_STEREO_RECOVER=1` test confirmed:
- **7,560** hardware KMS page-flip events in approximately 63s,
  with 119.998 Hz mode and 0 DRM atomic commit errors.
- **8** missed display refresh slots, all in the first ~13 seconds:
  report windows ending at 1, 3, 11 and 13 seconds had respectively
  2, 2, 3 and 1 missed cycles.
- **11** confirmed flips displayed an eye opposite the initial
  absolute MSC parity; **5** skipped slots held the wrong eye.
  These are separate from **14** adjacent-flip parity anomalies.
- `corrections=8`, matching the 8 missed individual slots in
  this particular run, but that does **not** prove causation,
  since the recovery logic does not prevent missed page flips.
- From around second 14 to second 63, 50 consecutive
  one-second windows had `window_missed=0`,
  `window_wrong_eye=0`, `window_wrong_hold=0`, and
  `window_hz=119.997-119.998`.

Interpretation: the phase correction successfully avoided persistent
reversal of the left/right cadence **after** initial disturbances,
but does not make individual missed vblanks disappear. Do not
enable the physical emitter yet.

### Next unmodified-code A/B: GBM/DRM swapchain depth

mpv's documented default `--swapchain-depth=2` allows multiple
in-flight frames. Its DRM EGL backend holds GBM buffers in a queue
and waits for KMS page-flip events. The queue may allow extra frames
rendered ahead with stale eye phase before a missed flip is detected.

Measure the same 90s sample, with unchanged options and audio,
first at `--swapchain-depth=1` and then at
`--swapchain-depth=3`. Log each run separately.
A depth of 1 may reduce in-flight stale-eye errors but can increase
missed deadlines; a depth of 3 may reduce deadline misses but increase
the reaction time for parity corrections. Neither outcome should be
assumed before measurements. If neither is sufficient, change
the renderer to select the eye by *intended actual DRM MSC slot* and
separate its 120Hz scanout rhythm from mpv's video-frame delivery.

## Fourth opt-in experiment: MSC-targeted queue alignment (NO USB)

The old `feature/drm-stereo-diagnostics` branch remains unchanged. This
experiment lives on `feature/drm-stereo-msc-queue`.

**Previous measurements:** a one-buffer swapchain produced approximately
66.77 confirmed flips/s and thousands of missed physical slots; depth=3
reached 119.914 average flips/s over 72 windows (6 skipped slots, 10
wrong-eye presentations), versus 119.870 average over 63 windows at depth=2
(8 skipped slots, 11 wrong-eye presentations). These runs differ in
duration and startup conditions, so the small difference is not proof of
an optimal queue depth.

### Changes

Enable with `MPV_DRM_STEREO_SBS=1 MPV_DRM_STEREO_SLOT=1`:

- After the first confirmed KMS flip, every subsequent draw selects L/R
  from the **estimated target** MSC:
  `last_confirmed_msc + pending_gbm_buffers + 1`.
  During initial queue priming, use the old alternating draw sequence
  until a hardware MSC and initial eye have been established.
- A GBM frame retains its chosen eye, estimated target MSC, and CPU-side
  draw timestamp. Successful atomic commits also retain their metadata.
- Immediately before committing the next pending BO, check the expected
  left/right phase for `last_confirmed_msc + 1`. If the next waiting
  BO has the wrong eye and its immediate successor has the correct eye,
  discard the stale **waiting** BO. Never discard the scanned-out BO
  or a BO with an in-flight atomic commit. Recovery can consume
  extra queued buffers and cannot repair an already-shown wrong eye.
- Once primed, attempt at least one pending flip per render call even when
  a recovery discard has temporarily reduced the queue below
  `--swapchain-depth`. Wait for its page-flip callback before
  computing the queue depth for the next prediction.
- Record `slot`, `pending`, `discards`, `window_discards`,
  `target_misses`, `window_target_misses`,
  `max_commit_callback_ms` and `max_draw_callback_ms`.
  The latter two measure CPU callback delivery delays, **not** the
  panel's optical response or exact physical scanout latency.

The old `MPV_DRM_STEREO_RECOVER=1` parity-toggle mechanism is bypassed
in slot mode; it is still available on the older branch or with
`MPV_DRM_STEREO_SLOT=0`. There is deliberately **no USB or emitter
integration**.

### Build and A/B test

```sh
git fetch origin
git switch feature/drm-stereo-msc-queue
meson compile -C build

# Switch to a free TTY (no KWin holding DRM master).
# Use the same half-SBS movie and unchanged DRM mode/audio.
for depth in 3 4 5; do
  MPV_DRM_STEREO_SBS=1 MPV_DRM_STEREO_SLOT=1 \
    ./build/mpv --no-config --vo=gpu --gpu-context=drm \
    --drm-connector=DP-1 --drm-mode=2560x1440@120 \
    --drm-vrr-enabled=no --video-sync=display-resample \
    --interpolation=no --hwdec=auto --swapchain-depth="$depth" \
    "/path/to/half-sbs-video.mkv" \
    >"/tmp/mpv-drm-stereo-slot-depth${depth}.log" 2>&1
done
```

Each run should play for 90-120 seconds; press `q` to move to the
next depth. For a clean baseline, also test
`MPV_DRM_STEREO_SLOT=0 MPV_DRM_STEREO_RECOVER=1` with depth=3.

Compare per-window `window_missed`, `window_wrong_eye`,
`window_wrong_hold`, `window_target_misses`, and queue discard bursts,
rather than relying on average refresh rate. Zero `target_misses` alone
is not enough: stale page flips or missed hardware slots still make the
stereo cadence unsafe for shutter glasses.

### Known non-goals of this patch

- It does not create a dedicated independent 120 Hz rendering thread.
  Frame draws still come from mpv's display-resample scheduling, and
  a stalled decoder or VO can still miss vblank deadlines.
- It does not reserve a future hardware MSC atomically; a commit can
  complete later than the predicted slot. Queue repair cannot guarantee
  flawless L/R cadence.
- Increasing swapchain depth may improve rendering slack but increases
  pending-frame latency and can make phase recovery more expensive.
  Keep it an explicit benchmark variable rather than assuming more
  buffers are always better.
- No physical 3D glasses/IR emitter should be enabled until measured
  KMS cadence and optical shutter timing are both validated.
