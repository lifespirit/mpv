# Experimental DRM half-SBS frame-sequential output (no USB)

> **Update (2026-10-25):** the native DRM-stereo video path remains usable
> without any USB device or extra library. An **optional, disabled-by-default**
> NVIDIA 3D Vision USB IR bridge has now been added for calibration experiments.
> See [DRM stereo USB test and optical calibration](drm-stereo-usb.md).
> This does not establish actual shutter optical phase or glasses safety.
> Use the software-only dry-run before enabling real USB commands.

> **Current native DRM-stereo defaults (2026-10-25):**
> With `--drm-stereo-3d=yes`, `MPV_DRM_STEREO_TWO_SLOT_QUEUE`
> and `MPV_DRM_STEREO_PLAN_HOLD` now **both default to enabled**.
> The production mode needs **no environment variables** for these
> policies. Set either one to `0` to disable it for regression testing,
> or `1` to force it on. Invalid values warn and fall back to defaults.
> With normal DRM 2D output both switches are ignored. The older
> `MPV_DRM_STEREO_SBS=1 MPV_DRM_STEREO_PAIR=1` experimental
> interface continues to default both policies off, and still accepts
> explicit `=1`. This is a targeted rollout on the tested i915 path;
> other GPU drivers and normal-mode regressions still need validation.
> The document below preserves the *historical* opt-in experiments;
> older sections saying "disabled by default" describe earlier revisions.
>
> Rebuild the branch after `474c53b`, then verify the init log contains
> `Stereo scheduling policy: plan_hold=1 two_slot_queue=1 (native_3d=1`
> when neither environment switch is set. For independent opt-out use
> `MPV_DRM_STEREO_PLAN_HOLD=0` or
> `MPV_DRM_STEREO_TWO_SLOT_QUEUE=0`. The injected
> `MPV_DRM_STEREO_QUEUE_TEST_HOLD_MS` remains **test-only and off**.


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

### Fifth test: forced-flip regression and fix

The first `MPV_DRM_STEREO_SLOT=1` build unintentionally forced a
new atomic flip *and synchronously waited for its callback on every
draw*. This drained the GBM queue to zero waiting BOs regardless
of `--swapchain-depth`, serializing GPU rendering and display updates.

In actual approximately 68-second runs:

| swapchain depth | flips | shown_hz | missed_vblanks | wrong_eye | target_misses |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3 | 4320 | 64.229 | 3750 | 1472 | 2598 |
| 4 | 4440 | 65.777 | 3659 | 1577 | 2605 |
| 5 | 7080 | 105.970 | 937 | 373 | 655 |

Each run reported `pending=0` and `discards=0` in every 120-flip
window; `commit_errors=0`. A larger queue depth was not a solution
to the forced synchronous pipeline.

**Fix:** remove the forced flip and unconditional wait, and restore
the pre-existing asynchronous GBM swapchain pressure loop. The
MSC-targeting metadata, conditional stale-eye discard, and diagnostics
remain available. New reports also include `peak_pending` (maximum
number of pending BOs in the reporting window) to verify the queue
actually fills.

**The fix is not yet validated by a local build or new KMS run.**
Rebuild and test depth=3 for 90-120 seconds first. Only if that
sustains near 120 fps should depth=4 and 5 be tested. Monitor both
`peak_pending` and the L/R correctness metrics; a higher queue
depth does not itself guarantee phase correctness.

### Sixth test: asynchronous MSC-slot v2 A/B across depth 3, 4 and 5

After restoring the original pipelined GBM pressure loop (and adding
`peak_pending`), three runs used the same 23.976 fps Half-SBS film
with VA-API decoding, 2560x1440 at ~119.998 Hz, audio via PipeWire,
`MPV_DRM_STEREO_SBS=1`, `MPV_DRM_STEREO_SLOT=1`,
`--video-sync=display-resample`, `--interpolation=no`,
and USB emitter **disconnected**.

| Metric | Depth 3 | Depth 4 | Depth 5 |
| --- | ---: | ---: | ---: |
| Report windows (120 flips each) | 92 | 95 | 95 |
| Confirmed KMS flips | 11,040 | 11,400 | 11,400 |
| Cumulative shown_hz | 119.859 | 119.902 | 119.932 |
| Missed vblanks | 13 | 9 | 6 |
| Confirmed wrong-eye flips | 7 | 7 | 4 |
| Wrong-eye held physical slots | 9 | 8 | 4 |
| Adjacent parity errors | 12 | 14 | 6 |
| Predicted target MSC misses | 11 | 10 | 5 |
| Waiting GBM BOs discarded | 7 | 7 | 4 |
| Maximum observed pending BOs | 2 | 2 | 2 |
| Atomic commit errors | 0 | 0 | 0 |
| Clean 120-flip windows (all stereo diagnostics zero) | 87 / 92 | 92 / 95 | 92 / 95 |
| Longest continuous span of clean windows | 73 | 60 | 38 |

Windows with physical missed slots:

- Depth 3: windows #1 (6), #8 (2), #82 (1), #83 (3), #91 (1).
- Depth 4: windows #1 (2), #9 (3), #35 (4).
- Depth 5: windows #1 (2), #40 (3), #74 (1).

The pipeline is back to near-120 flip cadence; the previous forced-flip
regression is fixed. Between isolated missed vblanks, the MSC-slot
algorithm keeps the phase correct for long stretches. The remaining
confirmed wrong-eye flips are localized to skip bursts, and
`window_discards` rises alongside recovery, showing that the pending
queue is repairing phase without sustained inversion.

All three queue-depth settings recorded **peak_pending=2** for every
window, so raising `--swapchain-depth` beyond 3 did not demonstrably
increase the physical GBM queue depth. Depth=5 showed the fewest misses
in this sample, but is **not proven** superior: tests started at
different times and encountered different transient scheduling jitter.

In clean windows, `max_commit_callback_ms` is typically ~8.4-8.6 ms
(one 120 Hz refresh). During skip bursts, it rises to ~13-18 ms
and sometimes more; `max_draw_callback_ms` is ~25 ms in clean
windows (including pre-rendering queue latency) and rises to ~33-39 ms
during faults. The current counters combine hardware scanout wait
and callback-delivery scheduling, so these observations **do not
conclusively identify** which portion caused each missed deadline.

**Architectural recommendation:** stop doing a full GL SBS-eye draw
for every 120 Hz output refresh. Decode each new 23.976 fps source
frame, generate/cache **both** already-expanded L/R images, and
drive KMS plane FB_ID swaps using the actual (MSC, UST) vblank clock.
The scanout cycle must keep running on the previous stereo pair
when video decoding or GL rendering is delayed. The producer can
replace the eye pair only at a safe frame boundary, with no KMS
FB_ID pointing at a released BO; distinguish in-flight, scanned-out,
and update-staging buffers. A persistent MSC parity defines which
eye to commit, including when a physical slot is skipped; do not
choose the eye from render/draw count.

A rigorous next implementation should include a producer/consumer
contract, explicit BO lifetime management, a KMS-driven independent
flip loop, and per-flip logs for intended MSC, confirmed MSC and
render-pair generation ID. The monitor's `119.998 Hz` is not
`120.000 Hz` and should also be the eventual timing reference for
the USB/IR emitter. **No new independent scheduler is implemented
yet; this is a design note based on the experimental runs.**

## Seventh experiment: dual-eye framebuffer producer + independent KMS scanout

Branch: `feature/drm-stereo-dual-scanout` (the v2 branch is untouched).

This is an **experimental, unvalidated architecture**. Its source has not
yet been compiled/run on the target Arch Linux DRM system. USB emitter
support remains **disabled**.

Enable with `MPV_DRM_STEREO_SBS=1 MPV_DRM_STEREO_PAIR=1`.
The older `MPV_DRM_STEREO_SLOT=1` and software recovery are
automatically bypassed in this mode.

### Producer

- DRM/EGL creates **two** GBM/EGL window surfaces with the same dimensions
  and format, one for L and one for R, using the same rendering GL context.
- The mpv VO thread checks `frame->frame_id`: on every *new* decoded SBS
  source frame, it binds and renders L, swaps/locks its GBM BO, then binds
  and renders R, swaps/locks its GBM BO and publishes the complete pair.
- Repeated display-resample frame callbacks do **no GL draws**. The
  dedicated KMS worker keeps presenting the previous complete pair.
- If a GBM surface has no free capture buffer, the producer skips the
  update rather than blocking the KMS worker. An incomplete pair is never
  published. `pair_update_skips` counts these attempts.

### Consumer

- A dedicated pthread is the only DRM page-flip event consumer in pair
  mode. It submits `DRM_MODE_ATOMIC_NONBLOCK |
  DRM_MODE_PAGE_FLIP_EVENT` on the draw plane and waits for the true KMS
  page-flip callback. It uses `last_msc + 1` parity to choose the eye,
  anchored to the first L callback; exact actual MSC is validated by the
  original diagnostics.
- The worker uses already-created FB IDs from the captured BOs. It
  never calls EGL or GBM and never renders video.
- Source generations are switched only at an L slot, and only after
  *both* new eye BOs are ready. Older active BOs are retired **after**
  the new flip's callback, never when a submit was merely requested.
  The VO thread eventually releases retired GBM surface locks. The
  active pair is reused until the next complete generation is available.
- The worker stops and is joined before the original DRM master state
  is restored or EGL/GBM is destroyed. The DRM fd stays open through
  GBM cleanup.

### Diagnostics

The existing `DRM SBS test` counters remain authoritative for the
physical output, especially `window_missed`, `window_wrong_eye`
and `window_wrong_hold`. Extra counters:

- `pair=on`: confirms the independent worker mode;
- `pair_gen`: currently displayed L/R pair generation;
- `pair_renders`: number of complete L/R pairs published by the producer;
- `pair_reuse_flips`: flips that reused the active pair rather than
  adopting a new one;
- `pair_update_skips`: producer attempted an update but no free
  GBM capture BO was available.

A ~23.976 fps source should normally yield ~24 complete pair renders
per second and ~120 confirmed KMS flips per second. A stable movie
frame continues alternating eyes across decoder/GL stalls. Count
late or missing hardware flips from MSC, **not** from mpv's OSD
`DS` rate or the nominal 120 Hz mode.

### Build and smoke test

```sh
git fetch origin
git switch feature/drm-stereo-dual-scanout
git pull --ff-only
meson compile -C build

MPV_DRM_STEREO_SBS=1 MPV_DRM_STEREO_PAIR=1 \
./build/mpv --no-config --vo=gpu --gpu-context=drm \
    --drm-connector=DP-1 --drm-mode=2560x1440@120 \
    --drm-vrr-enabled=no --video-sync=display-resample \
    --interpolation=no --swapchain-depth=3 --hwdec=auto \
    "/path/to/half-sbs-video.mkv" \
    2>&1 | tee /tmp/mpv-drm-stereo-pair-depth3.log
```

Start with a **short 10–15 second smoke test**, exit using `q`,
inspect `pair=on`, increasing `pair_gen`, `pair_renders`,
`pair_reuse_flips`, and confirm that `flips` is approaching
119.998 Hz. Then run for 90–120 seconds and collect the full log.
Keep the USB/IR emitter disconnected.

**Known limitations**: initial L/R anchor is based on the first
confirmed flip, not measured optical shutter timing. EGL reuse
synchronizes through GBM/KMS implicit fences (not explicit output
fences). The paired producer still uses the mpv display-synced VO
thread to obtain fresh source frames; the KMS worker alone owns the
120 Hz scanout cadence. VT switching, HDR reconfiguration, seeks,
and decoder stalls require further stress tests. A page-flip callback
is not a measurement of when LCD pixels or shutter lenses respond.
No physical stereo accuracy has been demonstrated yet.


### First paired KMS smoke-test failure: no scanout

The first `MPV_DRM_STEREO_PAIR=1` test on the user's Arch Linux
hardware started VA-API video and PipeWire audio but showed a black
screen and repeatedly logged:

```
[vo/gpu/opengl] Stereo KMS atomic flip failed: Success
```

No `DRM SBS test` callbacks were logged, and mpv's A-V drift grew
while the KMS scanout worker made no successful page flips.

**Root cause found in the source:** the new worker used a short-circuit
property chain requiring `ZPOS` to be set successfully before calling
`drmModeAtomicCommit`. `drm_object_set_property` returns
`-EINVAL` for absent or immutable properties without setting `errno`.
The original mpv `queue_flip` does not require `ZPOS` to succeed.
Thus a missing/immutable `ZPOS` prevented the atomic submit entirely,
while `mp_strerror(errno)` misleadingly printed "Success".

**Fix:** require `FB_ID` and `CRTC_ID` only; attempt but ignore
failure on optional `ZPOS`. Track the return status of property
setup and a separate errno for `drmModeAtomicCommit` failures. The
new diagnostic prints `fb_prop`, `crtc_prop`, `commit_rc` and
`errno`. Log the first successfully submitted eye/FB/generation.
Stop the KMS worker after 60 consecutive failed commits to avoid
endless busy retries. This fix still needs hardware validation.


### Second paired KMS smoke test: 120 Hz scanout, runaway playback speed

After fixing the atomic-property bug, the dedicated KMS worker **did**
produce picture, with confirmed KMS flip rate 119.997 Hz, zero
missed slots, zero wrong-eye flips and zero wrong holds over at
least 1,680 flips. However video advanced roughly 7-10x faster
than audio and mpv's A-V drift reached -90 seconds.

At the first and second 120-flip reports:

| Metric | flip 120 | flip 240 |
| --- | ---: | ---: |
| KMS confirmed flips | 120 | 240 |
| shown_hz | 119.997 | 119.997 |
| wrong_eye / window_missed | 0 / 0 | 0 / 0 |
| pair_renders | 178 | 363 |
| pair_gen | 175 | 360 |
| stereo draws | 357 | 728 |

This exposed a separate regression: the paired `vo_gpu.flip_page()`
returned **immediately**, unlike the original display-synchronized
GBM swap path which blocks on vblank/backpressure. mpv's
`--video-sync=display-resample` expects each VO flip to advance
at approximately the physical monitor refresh; without blocking
it consumes the source stream at decoder speed, independent of the
correctly clocked KMS worker. The huge `pair_renders` values
therefore represent genuinely advancing video, not merely
duplicated draws.

**Fix:** `drm_egl` now has a dedicated `flip_ready` condition and
a hardware-confirmed flip sequence counter. The KMS worker signals
it after processing the real DRM event, while `vo_gpu.flip_page()`
waits until the independent consumer has confirmed a *new* physical
flip. The KMS thread never waits for the VO thread. The waiting
thread releases the stereo mutex during the condition wait;
`get_vsync` retains the existing mutex to protect the
`present_sync` feedback. There is a 100 ms maximum wait to
avoid indefinite stalls if the KMS worker stops or loses events.
The worker signals waiters on shutdown. No USB emitter involved.

**Next test:** build current `feature/drm-stereo-dual-scanout`
and run the same command for 10-15 seconds. Look for ~119.998
KMS flips/s, **~24 pair_renders/s** for 23.976 fps source, and
small/stable A-V offset. The mode still uses `frame->frame_id`
to skip video repeats. If repeat behavior is not correct in
practice, adjust it after reviewing the new test.


### Third paired test: restored real-time playback; isolated MSC misses

In the ~107-second third run with paired output and VO vblank pacing:

| Metric | Full run |
| --- | ---: |
| Confirmed flips | 12,840 |
| Cumulative shown_hz | 119.801 |
| Normal window_hz | 119.997 |
| Complete L/R pairs rendered | 2,570 (~24/s) |
| Stereo eye renders | 5,140 (~48/s) |
| Hardware flips reusing a completed pair | 10,271 |
| Skipped physical vblanks | 21 |
| Wrong-eye flips / holds | 21 / 21 |
| Atomic commit errors | 0 |
| Pair producer no-free-buffer skips | 0 |
| Final audio-video deviation | ~ -2 ms |

92/107 report windows had no missed physical slot. Misses clustered in
15 windows, including a five-slot burst near window #42. The long-term
audio-video deviation remained near zero and no AV desync warning was
logged. The `DS: ~5.0` ratio is the expected approximately five 120 Hz
refreshes per 23.976 fps source frame, **not** 5x playback speed.

The remaining failure mechanism is a rare delayed KMS submit/callback:
the worker normally predicts the next eye from `last_msc+1`. If a
physical vblank advances during a delayed worker wakeup, a flip
submitted for that predicted MSC can land on a later MSC and carry
the wrong eye for one slot. The next flip uses the confirmed MSC and
automatically restores phase.

Follow-up fixes:

1. Move retired GBM buffer releases out of the KMS mutex critical
   section to reduce scheduling interference during 120 Hz scanout.
2. Opt-in `MPV_DRM_STEREO_SEQ=1` uses
   `drmCrtcGetSequence(fd, crtc_id, ...)` immediately before the
   atomic request, predicts the actual upcoming sequence, and chooses
   the eye by physical MSC parity. The default behavior is unchanged.
   If the query fails, fall back to last confirmed page-flip MSC.
3. Log `pair_seq=on/off`, `pair_catchups`, existing
   `target_misses/window_target_misses`, and
   `max_commit_callback_ms` to distinguish late submissions from
   late scanout when comparing with the original mode.

The sequence query is **not** atomic with the subsequent commit;
a vblank racing between the query and submit can still cause an
off-by-one prediction. The additional ioctl also costs some time, so
the new mode must be tested rather than assumed superior. Future
testing may need optional SCHED_FIFO for the isolated KMS thread
(with appropriate RT scheduling permissions), but avoid doing
that before establishing the new timing histogram.

**Validation**: new commits were not locally compiled or tested on
the target KMS host. First run a short test with `MPV_DRM_STEREO_SEQ=1`
to confirm the new query succeeds and `target_misses` are populated.
Then compare 90-120 second runs with `SEQ=0` and `SEQ=1`, keeping
the exact same video and mpv configuration. Both must continue
sustaining approximately 24 `pair_renders` and 120 flips per second,
with stable A/V and zero commit errors. USB remains disabled.

### Fourth paired test: opt-in CRTC-sequence mode

The fourth run with `MPV_DRM_STEREO_SEQ=1` lasted ~108 seconds.

| Metric | Previous baseline (~107 s) | SEQ=1 (~108 s) |
| --- | ---: | ---: |
| Confirmed KMS flips | 12,840 | 12,960 |
| Missed hardware refresh slots | 21 | 10 |
| Confirmed wrong-eye flips / holds | 21 / 21 | 10 / 10 |
| Clean windows of 120 flips | 92 / 107 | 98 / 108 |
| Effective cumulative scanout Hz | 119.801 | 119.904 |
| DRM commit errors | 0 | 0 |
| Pair capture skips | 0 | 0 |
| Complete stereo pairs prepared | 2,570 | 2,591 |
| A-V at end | ~ -2 ms | ~ +12-13 ms |

The ten misses occurred in report windows #1, #10, #19, #38, #50,
#55, #69, #72, #101 and #104. Every miss coincided with
`window_target_misses=1`, and late `max_commit_callback_ms`
of 16.49-16.71 ms (two 120 Hz refreshes). Normal windows showed
~8.4-9.0 ms, with occasional up to ~11.8 ms and no physical misses.

Crucially **`pair_catchups=0`** throughout the entire run. The
sequence query never observed the current CRTC counter ahead of the
last confirmed page-flip counter. We therefore cannot attribute
the improvement to sequence-parity correction alone: removal of GBM
releases from the KMS mutex in the same build, run-to-run timing jitter,
and the additional query may all be relevant. One sample is not enough
to estimate the true miss rate precisely.

### Next experiment: worker-only SCHED_FIFO and precise turnaround telemetry

An optional `MPV_DRM_STEREO_RT=1` asks for SCHED_FIFO priority 10
**on the dedicated KMS thread only**. Other mpv decoder, GL and audio
threads retain normal priority. When the process lacks permission
(`RLIMIT_RTPRIO`/CAP_SYS_NICE), the worker logs one warning and
continues with the normal scheduler; it must never attempt to alter
privileges itself. Because realtime threads can starve the machine if
they spin, use only the tested self-blocking KMS worker, never boost
the whole mpv process.

Per-report `max_submit_after_callback_ms` now measures the maximum
CPU-side time from the prior DRM event being handled to the next
atomic submit. Every `DRM SBS gap` also logs its own
`submit_after_callback_ms` and `commit_callback_ms`.
Those fields separate a late worker wakeup / userspace mutex stall
from a commit that was submitted promptly but latched at N+2 rather
than N+1. They are not display pixel photodiode timing measurements.

Recommended controlled A/B:

1. `MPV_DRM_STEREO_SEQ=1 MPV_DRM_STEREO_RT=0` for a ~2-minute run;
   confirm baseline turnaround telemetry.
2. `MPV_DRM_STEREO_SEQ=1 MPV_DRM_STEREO_RT=1` for a similar run;
   check the startup log for successful SCHED_FIFO activation.
3. Compare `window_missed`, `target_misses`,
   `max_submit_after_callback_ms`, and the new per-gap turnaround.
   Never interpret RT=1 as active unless the startup line confirms it.

No emitter or optical shutter sync has been enabled. Compilation and
KMS behavior of these new commits still require user-side validation.


### Paired KMS A/B: RT=0 versus SCHED_FIFO(10)

Two ~1.5 minute runs with `MPV_DRM_STEREO_SEQ=1` on the target
DRM/GBM/EGL host:

| Metric | RT=0 | RT=1 |
| --- | ---: | ---: |
| Confirmed flips | 12,720 | 11,640 |
| Duration based on 120-flip windows | ~106 s | ~97 s |
| Skipped physical vblanks | 19 | 9 |
| Skips/minute | 10.75 | 5.57 |
| Wrong-eye flips | 19 | 9 |
| Largest callback-to-next-submit latency per run | 0.265 ms | 0.176 ms |
| Median of per-window max callback-to-submit | 0.185 ms | 0.061 ms |
| Atomic commit errors | 0 | 0 |
| Pair producer skips | 0 | 0 |
| Terminal A-V offset | +4 ms | +6 ms |

The RT=1 run confirmed startup log
`Stereo KMS thread scheduling: SCHED_FIFO priority 10`.
The observed skipped-refresh rate declined about 48%, but two runs
alone do not establish a true long-run percentage.

At the individual **missed** refreshes, the KMS worker actually sent
the next atomic commit only 0.012–0.105 ms after the preceding callback
and the *commit to callback* time was 16.44–16.74 ms rather than the
normal ~8.4–9 ms. Every observed miss was a one-slot miss, not a long
pause. Thus the measured **user-space turnaround is already fast**;
the remaining misses appear to happen after submit, due to the
atomic commit or buffer readiness not hitting the expected scanout
refresh.

Notably, the misses are substantially asymmetric:

| Previous eye -> incoming eye at miss | RT=0 | RT=1 |
| --- | ---: | ---: |
| L -> R | 17 | 7 |
| R -> L | 2 | 2 |

Across both experiments 24/28 misses occurred when trying to show
the right eye. The producer renders L then R on separate GBM/EGL
surfaces; an EGL swap does not necessarily wait for rendering
completion. A plausible cause is that the later-rendered right
GBM buffer is still pending an implicit GPU fence when KMS tries
to scan it out. This is **a hypothesis, not a confirmed diagnosis**.

### Opt-in producer-side GPU wait

`MPV_DRM_STEREO_GPU_WAIT=1` invokes `eglWaitClient()` **once per
completed pair after the right-eye swap, before locking the right-eye
GBM BO and publishing the pair**. Waiting on the GL producer thread,
instead of the 120 Hz KMS worker, allows scanout to continue reusing
the last complete L/R pair while the GPU completes rendering. This
feature is **off by default** so the previous working behavior
remains intact.

New `DRM SBS test` fields:
- `gpu_wait=on/off`: whether the experiment is enabled.
- `gpu_waits`: cumulative number of GPU completion waits.
- `max_gpu_wait_ms`: largest `eglWaitClient()` duration in that
  120-flip reporting window.

For the next controlled comparison, use the same source, same
`--swapchain-depth=3`, `SEQ=1`, and `RT=1`. Compare
`GPU_WAIT=0` versus `GPU_WAIT=1` for at least ~2 minutes
each, including A-V drift, pair capture skips, gaps split by L/R
direction, and `max_gpu_wait_ms`. The latter may show large GPU
waits even if the scanout rate improves, because this opt-in test
deliberately synchronizes the producer. A win on miss rate would
support the implicit-fence hypothesis, but could also result from
the change in publication timing; repeat as needed.

Compilation and runtime stability of this opt-in change have **not**
been verified on the target hardware. Keep the USB IR emitter off.


### Follow-up: GPU completion wait A/B with working RT priority

Tested the optional producer-side `MPV_DRM_STEREO_GPU_WAIT=1` with
`SEQ=1 RT=1`; compared to the previous `SEQ=1 RT=1 GPU_WAIT=0` run.

| Metric | GPU_WAIT=0 | GPU_WAIT=1 |
| --- | ---: | ---: |
| Confirmed DRM flips | 11,640 | 13,080 |
| Duration (120-flip windows) | ~97 s | ~109 s |
| Missed vblank slots | 9 | 9 |
| Misses per minute | 5.57 | 4.95 |
| Wrong-eye flips | 9 | 9 |
| Misses at L -> R | 7 | 6 |
| Misses at R -> L | 2 | 3 |
| Pair capture skips / atomic errors | 0 / 0 | 0 / 0 |
| Terminal A-V | +6 ms | ~ -1 ms |
| `gpu_waits` | n/a | 2615 |
| Largest `max_gpu_wait_ms` among windows | n/a | 0.030 ms |

The direction imbalance persists (6/9 L->R vs 7/9 in baseline)
and the nine events in each run do not provide convincing evidence
that the GPU completion wait has improved KMS reliability.
`eglWaitClient()` usually returned within single-digit microseconds.
Short wait duration is not proof that there was no GPU work at any
point; the experiment merely did not expose significant waiting.
Keep the opt-in GPU wait disabled for ordinary tests.

During each missed vblank, the measured time from completion of the
previous userspace DRM callback to initiation of the next atomic
commit was ~0.03-0.12 ms; commit-to-callback stayed ~16.5-16.7 ms.
Those observations do not distinguish time spent *inside* the atomic
ioctl itself from time before the hardware latches the FB_ID.

**New diagnostic:** measure the duration of the
`drmModeAtomicCommit()` system call. Fields added:
`max_atomic_ioctl_ms` to each 120-flip report and
`atomic_ioctl_ms` to each individual `DRM SBS gap`.
`commit_callback_ms` still measures from the ioctl entry to the
userspace callback, so the remainder (minus event-handling latency)
is driver scheduling/scanout rather than ioctl blocking.

A useful next controlled comparison is `RT=1 GPU_WAIT=0`,
first `SEQ=1` and then `SEQ=0`, holding video, KMS mode and
all mpv arguments identical. `pair_catchups=0` throughout every
SEQ=1 run so far, therefore the sequence ioctl has shown no
corrective benefit for this workload and may add unnecessary overhead.

This new instrumentation was not compiled or hardware-tested
when committed. The emitter must remain disabled.


### Fifth run: RT=1, SEQ=0, GPU_WAIT=0, atomic ioctl tracing

The latest 113-second run, using the same Half-SBS film and display:

| Metric | Result |
| --- | ---: |
| Completed KMS flips | 13,560 |
| Report windows | 113 |
| Missed physical vblanks / wrong-eye flips | 9 / 9 |
| Misses per minute | ~4.78 |
| Non-error window refresh rate | 119.991-119.992 Hz |
| Pair renders by the final report | 2,711 |
| Pair-update skips / commit errors | 0 / 0 |
| End A-V deviation | ~+7 ms |

Every one of nine missed refreshes happened on the L->R edge.
Only the **first** missed refresh had a slow synchronous atomic
ioctl (13.958 ms). The other **eight** missed refreshes had
ordinary fast ioctls (0.089-0.119 ms), and a prompt userspace
callback-to-submit delay of 0.012-0.021 ms. Despite those quick
submissions, their commit-to-callback times were 16.515-16.748 ms
rather than one ~8.33 ms vblank. Thus synchronous ioctl blocking
explains only 1 of the 9 misses. The remaining events were delayed
after the ioctl returned; potential causes include KMS plane scheduling,
fences, or driver workqueues. Their kernel-level cause is not yet
established by these userspace measurements.

The observed 9/113s rate is close to the earlier 9/97s RT=1,
SEQ=1, GPU_WAIT=0 result. There has been no evidence of a CRTC
sequence catchup in any previous SEQ=1 run.

### Optional legacy CRTC pageflip control experiment

`MPV_DRM_STEREO_LEGACY=1` selects `drmModePageFlip()` with
`DRM_MODE_PAGE_FLIP_EVENT` on the already configured CRTC.
The same prepared GBM framebuffer is used, and the independent
KMS worker, page-flip callbacks, MSC eye prediction, video pacing,
audio sync, and frame pair buffers are unchanged. Initial mode
setting and restoration still use the existing atomic implementation.

This is an experimental legacy API comparison with the atomic
`FB_ID + CRTC_ID` path. It assumes the selected draw plane is
the CRTC **primary** plane (the default); do not use it with a
manually selected overlay plane. If the legacy operation is rejected
with EINVAL, ENOTTY, EOPNOTSUPP or ENOSYS, it automatically
disables the experiment and switches to the existing atomic flip
path. Other errors follow the standard bounded retry/stop logic.
The legacy path is disabled by default.

New log fields are `legacy=on/off`, `legacy_flips`, and
`legacy_fallbacks`. The existing `atomic_ioctl_ms` and
`max_atomic_ioctl_ms` fields are reused for the active pageflip
ioctl in this experiment, despite their historical names.

Recommended comparison with identical mpv options and input:
`RT=1 SEQ=0 GPU_WAIT=0 LEGACY=0` (existing baseline) vs
`RT=1 SEQ=0 GPU_WAIT=0 LEGACY=1`.
Test LEGACY=1 briefly first, verify that the initial flip is
labelled `legacy` and `legacy=on legacy_flips>0` appears in
reports. Then run it for ~2 minutes and compare missed_vblanks,
direction distribution, A-V stability and max_atomic_ioctl_ms.
Do not interpret a fallback run as having tested legacy scanout.
The change is **not compiled or runtime verified on the user's
GPU**; retain the working atomic path and keep USB disabled.


### Sixth run: legacy page-flip versus atomic (same RT/SEQ settings)

The `MPV_DRM_STEREO_LEGACY=1` run completed ~110 seconds with
`RT=1 SEQ=0 GPU_WAIT=0`. The log confirmed the first successful
legacy flip and `legacy=on legacy_flips=13200 legacy_fallbacks=0`:
**the fallback atomic path was not used**.

| Metric | Atomic | Legacy |
| --- | ---: | ---: |
| KMS flips | 13,560 | 13,200 |
| Duration | ~113 s | ~110 s |
| Missed physical slots | 9 | 14 |
| Misses/min | 4.78 | 7.64 |
| L -> R misses | 9 | 9 |
| R -> L misses | 0 | 5 |
| Commit failures | 0 | 0 |
| Pair-update skips | 0 | 0 |
| Final A-V | +7 ms | +19 ms |

All 14 missed legacy flips had a quick ioctl: 0.043-0.121 ms,
but confirmation happened about 16.6-16.8 ms after submit,
two 120-Hz periods instead of one. The legacy mode did not
eliminate the issue; one A/B sample does not prove that it is
intrinsically less reliable. Both interface paths may ultimately
be serviced by related KMS commit / plane-update machinery.

### New phase measurement: event delivery lag vs physical vblank

A key missing value is the phase at which mpv receives its
previous page-flip callback. The KMS worker can issue the next
ioctl immediately after callback, but if that callback reaches
userspace late in the refresh period, a fast ioctl may still miss
the next vblank deadline.

DRM event UST uses `CLOCK_MONOTONIC` when
`DRM_CAP_TIMESTAMP_MONOTONIC` is true. This is checked once
on the opened DRM fd; no UST age is calculated if unsupported.
The new diagnostic measures CPU-side `CLOCK_MONOTONIC` at the
start of the DRM callback and immediately before the flip ioctl;
it does **not** mix this clock with mpv's offset raw clock.

New fields on every `DRM SBS gap`:
- `prev_callback_lag_ms`: previous DRM event's callback delivery
  time minus its physical UST, before the current flip was sent;
- `submit_after_ust_ms`: elapsed time since previous physical
  UST until the new flip ioctl, including event-delivery lag and
  userspace work;
- existing `submit_after_callback_ms`,
  `atomic_ioctl_ms`, `commit_callback_ms` remain available.

Per-120-flip reports additionally show `ust_monotonic=on/off`,
`max_callback_lag_ms`, `max_submit_after_ust_ms`.
The per-gap values are -1.000 ms if no valid monotonic phase
measurement is available.

Run the default **atomic** path:
`RT=1 SEQ=0 GPU_WAIT=0 LEGACY=0`.
If per-gap `submit_after_ust_ms` is near or above the 8.333 ms
scanout period, the previous kernel event arrived too late for
a next-vblank flip (assuming the supplied event UST corresponds to
the previous vblank). If the value is small, investigation should
move to kernel-side DRM/KMS flip scheduling, implicit fences,
and display core. Do not infer an exact presentation-deadline
threshold from 8.333 ms alone; the hardware cutoff may be earlier.

These instrumentation commits are not yet compiled or tested on
the actual Linux DRM host.


### Seventh run: signed DRM UST measurements (phase instrumentation bug)

The new 113-second atomic, `RT=1 SEQ=0 GPU_WAIT=0 LEGACY=0`
experiment again showed **9 of 13,560 physical flips** missing one
refresh slot (7 L->R, 2 R->L). KMS RT scheduling worked, frame pairing
had zero dropped updates, atomic ioctl was only 0.089-0.114 ms during
all 9 missed flips, and A-V ended near +7 ms. 113 one-second windows
were recorded; clean-window frequency was about 120.000 Hz.

Critically, **ALL nine gaps** logged
`prev_callback_lag_ms=-0.000 submit_after_ust_ms=-0.000`.
These values are **not measured zeros**: prior diagnostic logic
replaced any negative or >=1000 ms CLOCK_MONOTONIC-minus-UST
difference with the integer -1 nanosecond, which printf rounds to
`-0.000` milliseconds. On the other hand, 74/113 windows had valid
positive `max_callback_lag_ms` values up to 0.409 ms, and
`max_submit_after_ust_ms` reached 0.464 ms. Therefore the
specific timing evidence needed to distinguish stale event delivery
from an anomalous UST value was accidentally masked.

The fix keeps the **signed full-range raw difference** between
the current `CLOCK_MONOTONIC` and the DRM flip event's UST
(verified on the DRM fd via `DRM_CAP_TIMESTAMP_MONOTONIC`).
No negative samples are discarded. Instead a separate validity
flag indicates whether `clock_gettime()` ran successfully, and
invalid values appear as `nan` in the gap log. Six decimal places
display microsecond-scale phase information.

Additional gap fields:
`prev_lag_valid=0/1` and `submit_ust_valid=0/1`.

Additional window fields:
`negative_callback_lags` (cumulative),
`min_callback_lag_ms` (signed),
`min_submit_after_ust_ms` (signed), in addition to existing
positive maxima.

Run `RT=1 SEQ=0 GPU_WAIT=0 LEGACY=0` again with the rebuilt
diagnostic version. If gap timing is negative, inspect the magnitude:
negative `prev_callback_lag_ms` means the received UST is *ahead*
of the userspace CPU monotonic time; positive but large means actual
late callback, and both indicate a different path than ordinary
immediate next-vblank flip submission. Kernel DRM helpers document
that page-flip event MSC/UST are obtained from the KMS vblank
timestamping machinery; The underlying DRM driver may deliver events during vblank, and its
UST may describe the *future end* of that vblank interval.
The exact reason on this GPU and kernel remains undetermined.

This change only modifies metrics, not scanout selection or timing.
It has not been compiled or tested on the target GPU.


### Eighth run and GPU identity correction: Intel Tiger Lake Iris Xe

The user corrected the hardware identification. The **test laptop** has:
- `0000:00:02.0` Intel TigerLake-LP GT2 [Iris Xe Graphics];
- `0000:5a:00.0` NVIDIA TU117M [GeForce MX450].

Earlier entries referring to an `AMD` video card/AMDGPU were
incorrect assumptions. The DRM/KMS GPU for DP-1 is *likely* Intel,
but neither the existing `lspci` output nor an mpv `vaapi` message
proves which PCI device backs the selected DRM/KMS fd and which
device renders the EGL context. New diagnostics at startup print
`Stereo DRM/KMS: node=... driver=... version=...` via
`drmGetVersion(drm->fd)`, and
`Stereo EGL/GL: vendor=... renderer=...` via GL GetString,
after the EGL context is current. These should be checked before
attributing hardware-specific behavior to i915 or nvidia-drm.

The latest 112 s run (`RT=1 SEQ=0 GPU_WAIT=0 LEGACY=0`) had
13,440 confirmed flips, 7 one-refresh vblank misses (5 L->R,
2 R->L), 0 failed commits and 0 skipped pair updates. The active
atomic ioctl on every missed flip completed in 0.096-0.122 ms;
callback delivery followed about 16.56-16.76 ms after submission.
End A-V was ~-1 ms. So none of the seven misses requires a
slow synchronous ioctl.

The corrected UST measurements show *13,327 negative callback
lags out of 13,440 flips* (~99.2%); on seven misses, the previous
UST minus userspace callback showed -0.233 to -0.380 ms, and
the next submit also preceded that event's nominal UST by
0.214 to 0.367 ms. Negative lags are therefore not specific
to the failures. The Linux DRM specification for
`get_vblank_timestamp` explicitly states that it targets the
**end** of vblank / first active scanline and can be in the
future when queried **inside** vblank. The earlier belief that
negative UST callback age was necessarily a bug is incorrect.
It is consistent with this driver reporting the end-of-vblank
timestamp while sending its event earlier in vblank. This
explanation is plausible, not by itself proof of the exact
hardware IRQ and flip deadline.

Investigating the remaining misses should target the active
Intel `i915` KMS path (once driver logging confirms it),
especially page-flip completion/commit workers, synchronization
fences and real vblank versus event delivery, rather than
AMDGPU DCN. Kernel ftrace DRM/i915 events and dynamic-debug
are more promising next than repeatedly altering the L/R
rendering timing. Preserve the known working atomic path and
`SCHED_FIFO(10)`.

The new startup GPU identification logs have not been compiled or
verified on the user's system yet.


### Ninth run: active i915 KMS + Intel Mesa renderer confirmed

The user resolved `/sys/class/drm/card1-DP-1`:
`status=connected`, PCI device `0000:00:02.0`,
driver `/sys/bus/pci/drivers/i915`. The new mpv startup messages
independently confirmed `node=/dev/dri/card1 driver=i915 version=1.6.0`
and `vendor=Intel renderer=Mesa Intel(R) Iris(R) Xe Graphics (TGL GT2)`.
The NVIDIA MX450 is not used for the current KMS or GL rendering.

This uploaded log covers only ~5 seconds / 600 confirmed flips
(not a full 2-minute sample). It contains two one-vblank misses,
both **L -> R** with quick atomic ioctls:
- MSC 3285450, previous 3285448: callback->submit 0.013 ms,
  submit-after-UST -0.389267 ms, ioctl 0.090 ms,
  submit->callback 16.762 ms;
- MSC 3285956, previous 3285954: callback->submit 0.015 ms,
  submit-after-UST -0.395587 ms, ioctl 0.099 ms,
  submit->callback 16.730 ms.

Both phase samples have explicit validity flags set. The KMS thread
was already at SCHED_FIFO(10); no atomic commit errors or dropped
stereo pair updates were reported through the 600-flip report.
Thus quick application submission does not consistently guarantee
a following-vblank callback on this Intel i915 host.

Do not read the negative monotonic-vs-UST phase as evidence of
late userspace events: DRM timestamps target the end of vblank,
which may still be in the future when the flip event is delivered
inside vblank. Next diagnostic step should capture available
`drm:drm_vblank_event*`, i915 `intel_pipe_update_*`,
and any `dma_fence` tracepoints during a 90-120 second run.
Check which tracepoints exist on the **installed kernel** before
selecting them; do not assume old i915 tracepoint names exist.

See Linux DRM `get_vblank_timestamp` contract and i915 tracepoint
descriptions for the kernel used during investigation.


### Tenth run: 114-second i915 trace correlated by MSC

The user captured `mpv-i915-trace.txt` using `trace-cmd` alongside
`mpv-drm-stereo-pair-depth3-rt-1(7).log`. Both refer to
Intel i915 on Tiger Lake / Mesa Intel Iris Xe (TGL GT2) and
`RT=1 SEQ=0 GPU_WAIT=0 LEGACY=0` at 119.998 Hz.

The player logged 13,680 confirmed flips over ~114s, 11 missed
one-refresh slots, zero atomic errors, zero pair-update skips,
and end A-V ~+2ms. The **11 misses were exclusively L->R**.
For missed flips, callback->submit took 0.011-0.025ms, the atomic
ioctl 0.098-0.128ms, and commit->callback 16.57-16.74ms.
`submit_after_ust_ms` was -0.393 to -0.180ms, so each
request was submitted shortly before the previous DRM event's
nominal end-of-vblank timestamp.

The DRM trace contains 14,570
`drm_vblank_event_delivered` events (plus 793
`drm_vblank_event` and 9 sets of `intel_pipe_update_*`
events, the latter chiefly near setup/teardown). Every mpv
gap corresponds *exactly* to a kernel delivered-event MSC jump:
3393801->3393803; 3395187->3395189;
3396773->3396775; 3397829->3397831;
3400855->3400857; 3401041->3401043;
3401427->3401429; 3402481->3402483;
3403827->3403829; 3404423->3404425;
3404439->3404441.
The kernel-delivered event intervals for those jumps are
16.627-16.733ms, consistent with the userspace per-gap logs.
Normal consecutive events for the movie are ~8.334ms apart
(median for 13,701 consecutive event pairs). An earlier
3391767->3391777 gap is around transition into mpv and
is **not** a playback miss.

The trace therefore rules out losing these 11 events
between kernel DRM delivery and `drmHandleEvent`. It does
**not** establish why the flip failed to happen by the
immediately following physical refresh: completed flips have
events, while a scanout retained for an extra refresh need
not have its own page-flip completion event. Only a small
subset of frames emitted the available `drm_vblank_event`
tracepoint, and `intel_pipe_update_*` events don't cover
ordinary per-frame flips; this capture does not reveal
kernel commit-worker or implicit fence scheduling.

A clear new pattern is that every missed request tried
to display the RIGHT eye. That may relate to right-eye
buffer readiness (it is the second eye rendered) or to
first display of a new pair generation; it may also be
a coincidence/other i915 internal scheduling behavior.
The new diagnostic reports `first_eye`, `pair_gen`,
and `pair_age_ms` on gaps, plus cumulative
`first_eye_L/R` and `first_eye_L/R_misses` counters.
Here `pair_age_ms` is CPU time from publication of a
fully prepared pair to the submission attempt: **not**
a GPU fence-completion timestamp.

Next recommended *no-code* A/B experiment:
1. Rebuild the branch with first-eye diagnostics, play
   normally for 120s (baseline) and save its log.
2. Start again with the identical options; after a few
   seconds press `space` to pause mpv and leave the
   independent KMS worker running for ~120s. Verify
   `DRM SBS test` continues incrementing `flips`
   while paused. If KMS activity stops on pause, the
   trial is invalid.
3. Compare `missed_vblanks` **delta while paused** vs
   delta while playing, not startup misses. No new
   frames or stereo pairs should be produced when paused,
   but the KMS worker will keep alternating the static
   active pair. If misses remain, rendering concurrency
   and fresh framebuffer readiness are unlikely to be
   necessary for the issue. If they disappear, first-eye
   statistics and actual fence completion should be
   examined before any broad driver changes.

The added diagnostic source has not been compiled or
runtime tested on the target host.


### Eleventh run: pause isolates refresh scheduling from pair rendering

The user arranged an IPC pause after five seconds; the log
`mpv-drm-stereo-pair-depth3-rt-1(8).log` confirms that pausing
worked around video timestamp `00:00:04`, with the independent
KMS thread continuing to flip at 119.997 Hz. This capture covers
just **5,160 confirmed flips (~43 s)**, despite the originally
planned 120-s pause; it is not a two-minute recording.

Two one-vblank misses occurred **before pause**, both L->R and both
`first_eye=1` on the first right-eye submission for a pair:
- MSC 3721877, generation 4, pair_age 15.188 ms,
  ioctl 0.110 ms, callback->submit 0.015 ms,
  submit-after-UST -0.374620 ms;
- MSC 3721897, generation 7, pair_age 15.357 ms,
  ioctl 0.090 ms, callback->submit 0.014 ms,
  submit-after-UST -0.224298 ms.

By the first paused KMS report, generation and render counts
stabilized at `pair_gen=112 pair_renders=112 draws=224`,
while `flips` rose from 600 to 5,160, i.e. **4,560 additional
flips / ~38s with ZERO additional misses**, and all `window_hz`
were near 119.997Hz; `commit_errors=0`.

This strongly suggests a dependency on the creation/publication/
first scanout of new stereo pairs **or** on producer-side GPU/CPU
activity; a pause disables both. It does **not** conclusively prove
a GPU fence issue, because `eglWaitClient` in older A/B runs
failed to eliminate all misses, and a zero-miss 38-s interval
can occur statistically with the earlier observed low failure
rate. Explicitly waiting for rendering to finish is not a
proven fix.

New optional control `MPV_DRM_STEREO_FREEZE_PAIR=1` separates
those causes: the video continues decoding and rendering at its
usual rate, but KMS shows only the **first** GBM stereo pair.
The producer releases each later completed pair immediately,
instead of publishing it to the KMS pending slot. This is not a
pause and no per-frame rendering work is skipped. The resulting
image stays static by design.

Expected `FREEZE_PAIR=1` logs:
- Startup: `Stereo freeze-pair A/B: rendering continues but only generation 1 will be submitted to KMS`.
- `pair_gen=1`, `first_eye_L=1 first_eye_R=1` remain fixed;
- `pair_renders` and `draws` **continue increasing**;
- `flips` increases at 120 Hz.
A truly valid control must preserve `pair_renders` activity
without any new KMS generation/BO being handed to the scanout
worker. Compare 120s of this mode to 120s of the normal
`FREEZE_PAIR=0` mode with the same video.

If misses persist despite static KMS BOs, investigate generic
rendering load/CPU scheduling vs KMS. If they disappear,
investigate the readiness of newly published GBM BOs, explicit
fences and the first-right-eye commit path. These outcomes
are suggestive, not proofs; repeat long runs for statistical
confidence.

The new optional A/B control has not been compiled or tested
on the user's Intel i915 host.


### Twelfth run: continuous rendering / first stereo pair frozen (103 s)

The user ran `MPV_DRM_STEREO_FREEZE_PAIR=1` and stopped
manually with Ctrl+C. The video starts on black frames, so
the scanout picture was black: this does **not** invalidate the
A/B result, since KMS holds the **first** pair by design.
The reported counters independently verify active production:
`draws=4942`, `pair_renders=2471`, while the core
advanced to movie PTS ~00:01:43 and did not pause.

The last report covered 12,360 KMS confirmations (~103s) at
119.996 Hz, and showed `pair_gen=1`,
`first_eye_L=1 first_eye_R=1`, zero missed vblanks,
zero parity/eye/hold errors, zero pair update skips and zero
atomic commit errors. Thus the same Intel i915/GL renderer
can process/submit roughly 24 new pairs per second while the
KMS worker alternates a fixed pair at ~120Hz with no misses.

This is substantially stronger evidence than the earlier
38-second pause experiment that fresh KMS BO presentation
and/or associated handoff is implicated, rather than generic
steady producer-side GPU load. It does not prove the implicit
fence is the culprit: unlike the normal mode, the fixed-pair
case never references newly finished buffers from KMS.
Careful longer A/B comparisons are still warranted because
the normal failure process is rare and possibly clustered.

For visual verification of this mode, start playback from a
nonblack timestamp e.g. `--start=00:01:00`. The first pair
is still frozen; rendering and decoding continue beyond it.

New targeted diagnostic: `MPV_DRM_STEREO_MIN_PAIR_AGE_MS=N`,
valid `0..32`, default `0`. With `FREEZE_PAIR=0`, the
independent KMS worker may promote a new pending stereo
pair only at an L boundary and after the pair has been
published for at least `N` milliseconds. If the pending
pair is too young, KMS continues alternating the old pair
until a subsequent L boundary; the producer may replace the
pending pair in the meantime. This deliberately increases
frame-to-scanout latency while leaving actual 120Hz
L/R alternation and regular producer-side rendering active.
`MIN_PAIR_AGE_MS=25` is an initial comparison point:
the two missed right-eye first scanouts in run 11 involved
pairs aged only ~15.2-15.4ms at R submission. If ageing
reduces errors, buffer readiness / first-use scheduling
becomes more plausible; if not, look for i915 atomic
commit/fence timing. Do not regard this as a final solution
without an A/B run and video-sync analysis.

No kernel, EGL or GL synchronization guarantees derive
from simply holding a BO for 25ms; age gating is an
experimental diagnostic, **not** a synchronization primitive.
The new source has not been compiled on the target laptop.


### Thirteenth run: 25ms minimum pair age, no missed vblanks in ~116s

The uploaded `mpv-drm-stereo-pair-depth3-rt-1(10).log`
used `MPV_DRM_STEREO_MIN_PAIR_AGE_MS=25` and normal,
non-frozen L/R pair promotion at 119.998 Hz on Intel i915 /
Mesa Iris Xe. It covered 2,047 lines and 116 full 120-flip
reporting windows, reaching 13,920 confirmed DRM page-flips
(~116.00s of hardware scanout), with **zero**
`DRM SBS gap` records.

The last report:
- `shown_hz=119.998 window_hz=119.998`,
  `missed_vblanks=0 parity_errors=0 wrong_eye=0 wrong_hold=0`,
  `max_gap=1`;
- `pair_gen=2782 pair_renders=2783 draws=5566`
  (normal continuous replacement of stereo buffers at ~24fps),
  `first_eye_L=2781 first_eye_R=2781`;
- `pair_update_skips=0`, `commit_errors=0`,
  `first_eye_L_misses=0 first_eye_R_misses=0`;
- `A-V` in the final movie-time lines (~00:01:56)
  oscillates around 0ms, but this does NOT measure the
  added real presentation latency of age gating.

Prior similar-run comparison:
- normal `MIN_PAIR_AGE_MS=0`: 11 misses in ~114s,
  all L->R as recorded in mpv and correlated with kernel events.
  The *first presentation* flag was not available in that run;
  two later baseline misses were both confirmed `first_eye=1`;
- `FREEZE_PAIR=1`: 0 misses over ~103s with 2,471
  video pairs rendered but unchanged scanned-out generation;
- `MIN_PAIR_AGE_MS=25`, `FREEZE_PAIR=0`: 0 misses
  over ~116s and 2,782 KMS pair generations.

This is strong evidence that holding a prepared pair for
longer before first KMS scanout eliminates the observed
sporadic delays on this tested Intel setup, but it is still
an empirical workaround, not GPU-fence synchronization or
a proof of which i915 scheduling dependency is responsible.

Next A/B probe: `FREEZE_PAIR=0 MIN_PAIR_AGE_MS=16`,
with the same ~120s sample and all other variables unchanged.
If any misses occur, try 20ms; if still zero, repeat
16ms or try 12ms to bracket the latency tolerance.
Retain 25ms as a working fallback until the threshold
and frame-to-physical-scanout latency are characterized.
`MIN_PAIR_AGE_MS=16` gates *L promotion*, so first R
scanout will occur one vblank (~8.33ms) later, at least
~24.3ms from pair CPU publication if KMS flips are on
adjacent refreshes. This does not guarantee GPU rendering
has completed, nor does it set a constant 16ms delay.

Do not enable `FREEZE_PAIR=1` in the threshold tests.
No code changes to the underlying algorithm were made
following this log analysis.


### Fourteenth run: 16ms age gate, zero skips over 110s

`mpv-drm-stereo-pair-depth3-rt-1(20261018-210642).log`
was captured with `MIN_PAIR_AGE_MS=16` and normal
`FREEZE_PAIR=0` playback on Intel i915 / Mesa Iris Xe
at 119.998Hz. The capture has 110 complete 120-flip
reporting windows: 13,200 confirmed KMS page-flip events,
approximately 110.0s, **zero DRM SBS gap records**,
and cumulative `missed_vblanks=0`, `parity_errors=0`,
`wrong_eye=0`, `wrong_hold=0`, `commit_errors=0`,
`pair_update_skips=0`. The final `shown_hz` and
`window_hz` were 119.997Hz, with `max_gap=1`.

The normal video producer kept running at ~23.976fps:
`pair_gen=2638`, `pair_renders=2639`,
`draws=5278`, `first_eye_L=2638`,
`first_eye_R=2638`; the recorded movie position reached
approximately `00:01:50` with A/V error around zero.
The log ends there without a clean mpv shutdown message,
so results apply to the last complete reporting window.
No indications that the pair was frozen or playback paused.

Side-by-side short run evidence:

| Pair min age | Confirmed KMS flips | Approx runtime | Skips |
| --- | ---: | ---: | ---: |
| 0ms (baseline run 7) | 13,680 | 114s | 11 |
| 16ms (this run) | 13,200 | 110s | **0** |
| 25ms (run 10) | 13,920 | 116s | **0** |

This is now two substantial runs with regularly changing
L/R buffers and no misses when an age gate is enabled.
This does not establish a hard real-time guarantee or GPU
fence completion, and the MPV A/V offset does not measure
added presentation latency.

Next characterize the lower limit: try
`MIN_PAIR_AGE_MS=12`, then `8` if still zero,
holding all other variables and test length (~120s) fixed.
A 16ms L-boundary age requirement means the first R
submission is roughly >=24.3ms from CPU publication
assuming adjacent 120Hz flips; this need not be the
actual rendering-completion-to-photon delay. If
failures return at 12ms, repeat 16ms and consider 14ms
to bound the useful range. Keep 16ms as tested fallback.


### Fifteenth run: 12ms age gate fails in four consecutive right-eye first flips

The log `mpv-drm-stereo-pair-depth3-rt-1(20261018-211629).log`
used `MIN_PAIR_AGE_MS=12` and `FREEZE_PAIR=0` on Intel
i915 / Mesa TGL GT2 at 119.998 Hz. There were 100 full
120-flip report windows: 12,000 confirmed KMS flips (~100.03s),
four single-refresh gaps, and no DRM commit errors or
pair-update skips. Final `window_hz=119.996`,
`shown_hz=119.956`, `missed_vblanks=4`,
`parity_errors=8`, `wrong_eye=4`, `wrong_hold=4`.
Continuous rendering/promotion continued:
`pair_gen=2398`, `pair_renders=2399`,
`draws=4798`, `first_eye_L=2398`,
`first_eye_R=2398`; `first_eye_R_misses=4`
and `first_eye_L_misses=0`.

The FOUR gaps all occurred as a compact burst:
- MSC 4276916->4276918, generation 882, first R use,
  pair_age=23.395ms, callback->submit=0.015ms,
  ioctl=0.108ms, commit->callback=16.843ms;
- MSC 4276922->4276924, generation 883, first R use,
  pair_age=23.563ms, callback->submit=0.011ms,
  ioctl=0.103ms, commit->callback=16.566ms;
- MSC 4276928->4276930, generation 884, first R use,
  pair_age=23.566ms, callback->submit=0.025ms,
  ioctl=0.128ms, commit->callback=16.533ms;
- MSC 4276934->4276936, generation 885, first R use,
  pair_age=23.594ms, callback->submit=0.016ms,
  ioctl=0.102ms, commit->callback=16.582ms.

The reported MSCs differ by 6 each (~50ms per successive
miss), with the four misses spanning ~150ms from the first
to last; the burst occurs ~36s into scanout. The rest of
the capture (~63s) shows no additional misses. The flip
events were *late*, not rejected. No clear user-thread
commit/submit slowdown preceded the burst.

The age of the pair at right-eye submit was ~23.4–23.6ms,
not merely the 12ms left-eye promotion gate. By contrast,
the 16ms and 25ms gated tests both had zero misses for
~110–116s. This empirically suggests a narrow useful
threshold between 12 and 16ms under this workload; it does
NOT demonstrate that 16ms is a real-time guarantee or
settle GPU fence root cause.

Next check `MIN_PAIR_AGE_MS=14` for ~120s with otherwise
identical settings, then perform longer repeated trials
at 16ms (e.g. >=10 minutes, ideally with varied scenes
and system load) before considering 16ms a reliable
operational setting. Leave the KMS worker and atomic code
unchanged until threshold/reproducibility is characterized.


### Sixteenth run: 14ms age gate still misses two first-R scans

The user tested `MIN_PAIR_AGE_MS=14` on Intel i915 / Mesa
TGL GT2 at ~119.998Hz with regular video playback and
non-frozen pair promotion. The log
`mpv-drm-stereo-pair-depth3-rt-1(20261018-212144).log`
has 104 complete 120-flip report windows, **12,480
confirmed page flips (~104s)**, with precisely **two**
one-vblank missed flip events. The final report has
`shown_hz=119.978 window_hz=119.997`,
`missed_vblanks=2 parity_errors=4 wrong_eye=2 wrong_hold=2`,
`first_eye_R_misses=2 first_eye_L_misses=0`,
`pair_gen=2495 pair_renders=2495 draws=4990`,
`pair_update_skips=0`, and `commit_errors=0`.
The reported movie PTS reached ~00:01:44.

The two missed first-right-eye page flips:
- `previous_msc=4334674 msc=4334676` (approx 5s),
  `pair_gen=119`, `first_eye=1`,
  `pair_age_ms=23.502`, `submit_after_callback_ms=0.015`,
  `atomic_ioctl_ms=0.104`, `commit_callback_ms=16.638`;
- `previous_msc=4344910 msc=4344912` (approx 90s),
  `pair_gen=2166`, `first_eye=1`,
  `pair_age_ms=22.301`, `submit_after_callback_ms=0.013`,
  `atomic_ioctl_ms=0.096`, `commit_callback_ms=16.660`.

Both are L->R; neither is explained by a slow userspace
atomic ioctl or user-thread callback handling. New stereo
pairs continued to be scanned out at ~24fps. At 12ms
the previously missed first-R pairs had ages ~23.4–23.6ms;
at 14ms they had ages ~22.3–23.5ms. Thus R-submit age
alone does not establish a hard threshold; the phase/
actual GPU fence completion may matter.

The empirical runs so far:

| MIN_PAIR_AGE_MS | Seconds | Missed refreshes |
| ---: | ---: | ---: |
| 0 | ~114 | 11 |
| 12 | 100 | 4 |
| 14 | 104 | 2 |
| 16 | 110 | 0 |
| 25 | 116 | 0 |

Do not declare 16ms a proven guarantee from one 110s
successful run. Next run a sustained **>=10-minute**
regular playback `MIN_PAIR_AGE_MS=16`,
`FREEZE_PAIR=0` to evaluate rare misses and clustering,
without changing other conditions. Do not use a frozen
frame or pause for this verification. If a miss recurs
under 16ms, use 25ms as an interim fallback and improve
GPU buffer readiness synchronization rather than lowering
the tolerance further. Even a zero-miss long run cannot
guarantee safe L/R alternation for shutter glasses.


### Seventeenth run: long 16ms age test fails three times across 11m01s

The user supplied `mpv-drm-stereo-pair-depth3-rt-1(20261018-213606).log`,
an extended normal playback run on Intel i915 (Mesa TGL GT2),
`MIN_PAIR_AGE_MS=16`, `FREEZE_PAIR=0`, 119.998Hz.
The log has 11,185 lines and 661 complete one-second
(`flips % 120 == 0`) reports, reaching **79,320**
confirmed KMS page-flip events (~661s = 11m01s).
There are **3 missed vblanks** (three single-refresh MSC
jumps), at movie position ~03:55, 07:47 and 10:34.
Each occurred during the **first scanout of a new
right-eye GBM framebuffer**, always L->R:
- `previous_msc=4417576 msc=4417578`,
  `pair_gen=5643 first_eye=1 pair_age_ms=24.361`,
  `submit_after_callback_ms=0.006`,
  `atomic_ioctl_ms=0.058 commit_callback_ms=16.676`,
  `submit_after_ust_ms=-0.429221`;
- `previous_msc=4445412 msc=4445414`,
  `pair_gen=11210 first_eye=1 pair_age_ms=24.362`,
  `submit_after_callback_ms=0.005`,
  `atomic_ioctl_ms=0.045 commit_callback_ms=16.644`,
  `submit_after_ust_ms=-0.374995`;
- `previous_msc=4465398 msc=4465400`,
  `pair_gen=15207 first_eye=1 pair_age_ms=24.517`,
  `submit_after_callback_ms=0.013`,
  `atomic_ioctl_ms=0.079 commit_callback_ms=16.551`,
  `submit_after_ust_ms=-0.278198`.

Counters at the last full report:
`flips=79320 shown_hz=119.992 window_hz=119.996
missed_vblanks=3 parity_errors=6 wrong_eye=3
wrong_hold=3 pair_gen=15862 pair_renders=15863
draws=31726 first_eye_L=15862 first_eye_R=15862
first_eye_L_misses=0 first_eye_R_misses=3
pair_update_skips=0 commit_errors=0`.
No code or DRM commit errors were logged. Normal
rendering and pair promotion continued monotonically;
movie time advanced to ~11m02s.

**Critical correction to prior conclusions:** 16ms is
*not* a verified stable setting; its earlier ~110s
zero-miss run was not representative of its longer-run
rare failures. The 14ms and 12ms tests also missed,
and the single 25ms result only covers ~116s.
Do not describe 25ms as proven stable, either.

The observed first-right submit pair ages at the three
16ms failures are tightly grouped at ~24.36–24.52ms,
compared with ~22.30–23.50ms at 14ms and
~23.40–23.59ms at 12ms. These samples do not prove
that a specific age (or GPU fence) is the sole cause.
The first-right flip's callback is late by one hardware
refresh (~16.6ms), despite immediate userspace submit
and sub-0.1ms atomic ioctl on all three misses. This
is consistent with a kernel/GPU scanout readiness or
scheduling dependency on newly used BOs, not CPU
thread late submission.

Next controlled test: keep `FREEZE_PAIR=0`, set
`MIN_PAIR_AGE_MS=25`, run **at least 10–15 minutes**
of normal video, and inspect KMS/first-eye/VO counters.
The 25ms L-promotion gate provides a larger margin
before R is first scanned out, but this is still a
timed heuristic without synchronization guarantees;
added image latency / actual A/V presentation delay
have not been measured. If misses persist, first
investigate proper EGL/DRM fence readiness and scheduling,
not a claim that an arbitrary age solves the issue.
Do not proceed to USB shutter timing based solely on
short-run zero-miss logs.


### Eighteenth run: 25ms pair age has zero misses across fifteen minutes

The user supplied `mpv-drm-stereo-pair-depth3-rt-1(20261018-215521).log`,
normal real-time movie playback with Intel i915, Mesa
Iris Xe (TGL GT2), `MPV_DRM_STEREO_MIN_PAIR_AGE_MS=25`,
normal non-frozen stereo-pair promotion and
`SCHED_FIFO` priority 10 for the independent 120 Hz KMS
worker. This is the requested long-run A/B comparison
against the 16ms gate's three misses in 11m01s.

The full log contains **899** one-second (`120 flip`)
KMS report windows; the last completed report contains
**107,880 confirmed page-flip events**, corresponding to
~899s = **14m59s** at a display mode of 119.998Hz.
The mpv movie time advanced to `00:15:00`; no clean
player shutdown is present in the capture.

All 899 KMS diagnostic windows were checked. Every
`window_missed=0`, `missed_vblanks=0`,
`parity_errors=0`, `wrong_eye=0`, `wrong_hold=0`,
`pair_update_skips=0`, and `commit_errors=0`;
there were no `DRM SBS gap` lines. All windows had
`window_hz` between 119.996 and 119.998Hz.
Maximum *per-window* `max_commit_callback_ms` was
9.161ms, well short of the double-vblank (~16.6ms)
callbacks seen in failed 12/14/16ms tests. The worst
observed per-window `max_atomic_ioctl_ms` was 5.281ms
during startup, not associated with any missed vblank.

At the final complete report:
`flips=107880 shown_hz=119.997 window_hz=119.997
msc=4639714 pair_gen=21574 pair_renders=21575
draws=43150 first_eye_L=21573 first_eye_R=21573
first_eye_L_misses=0 first_eye_R_misses=0
missed_vblanks=0 parity_errors=0 wrong_eye=0
wrong_hold=0 max_gap=1 pair_update_skips=0
commit_errors=0`.

The producer continued rendering ~24 stereo pairs/s, and
KMS continually promoted fresh pairs. This is a real
full-motion test, not a freeze or a pause.

Long-run comparison:
- 16ms gate: 3 first-right missed refreshes / ~661s;
- 25ms gate: **0 missed refreshes / ~899s**.

This is the strongest stability evidence for an age
gate yet on this system. It does **not** mean that
25ms guarantees scanout readiness under all load,
resolutions or driver versions. It is still an
empirically supported timing workaround, not an EGL
fence or a measured GPU completion event; its actual
video-to-photon/A-V latency is not established by the
mpv `A-V` counter alone.

Recommended: preserve `MIN_PAIR_AGE_MS=25` as the
current provisional configuration. Prioritize:
(1) explicit verification of GPU rendering completion
before handing a fresh right BO to KMS using a
fence-backed diagnostic, without blocking the KMS
real-time thread;
(2) detailed presentation latency measurement;
(3) parity / missed-refresh recovery behavior before
driving active shutter glasses by USB. There is no
evidence from this run that increasing the age gate
beyond 25ms is necessary.


### Nineteenth experiment: opt-in EGL native-fence readiness gate

After the 15-minute zero-miss 25ms age-gated run,
`video/out/opengl/context_drm_egl.c` now has an
experimental **`MPV_DRM_STEREO_NATIVE_FENCE=1`** mode.
It does **not** change behavior unless explicitly enabled.
It requires both `EGL_KHR_fence_sync` and
`EGL_ANDROID_native_fence_sync` and their KHR/ANDROID
entry points; when these are missing, mpv **fails
initialization** with an explanatory log, rather than
silently testing unsynchronized buffers.

After each eye's `eglSwapBuffers`, the EGL producer:
- creates an `EGL_SYNC_NATIVE_FENCE_ANDROID` EGL sync;
- issues a zero-timeout `eglClientWaitSyncKHR` with
  `EGL_SYNC_FLUSH_COMMANDS_BIT_KHR` to submit work
  without waiting for completion;
- obtains a `sync_file` descriptor with
  `eglDupNativeFenceFDANDROID`; destroys the EGL
  object but retains the independent duplicated FD;
- publishes the two eye BOs **alongside** both FDs.

The RT KMS worker does not call EGL/GL or wait for GPU
execution. For the first pair and at eligible left-eye
promotion boundaries, it calls `poll(fd, ..., 0)` for
**both** native fences. If either is unsignaled it
keeps reusing the previous already-presented pair and
retries at the next left boundary. This is a
**nonblocking fence readiness gate**, not a pass-through
`IN_FENCE_FD` atomic property. FDs are closed when
the owning pair is superseded, retired, or destroyed.

Both gating methods can be used together: the configured
`MIN_PAIR_AGE_MS=N` remains an independent lower bound.
The existing proven 25ms age gate is deliberately still
available as a safety fallback; `NATIVE_FENCE=0` leaves
prior behavior unchanged.

Six cumulative counters and a mode flag were added to each
`DRM SBS test` report:
`native_fence=on/off`, `fence_checks`,
`fence_promotions`, `fence_deferrals`,
`fence_unready_L`, `fence_unready_R`,
`fence_errors`.
A nonzero error counter invalidates a zero-miss result.

**Controlled A/B sequence:**
1. On the test host, `git pull --ff-only` and
   `meson compile -C build` (these changes have not
   been compiled/tested on the Intel laptop yet).
   Baseline: `FREEZE_PAIR=0`, `GPU_WAIT=0`,
   `NATIVE_FENCE=1`, `MIN_PAIR_AGE_MS=25` for
   ~2 minutes. Confirm extension detection and
   `fence_errors=0`; the age gate stays intact.
2. If native fences are available and clean, run
   `NATIVE_FENCE=1`, `MIN_PAIR_AGE_MS=0`,
   `FREEZE_PAIR=0` for **10-15 minutes**.
   Observe `fence_deferrals`, the individual
   unready-eye counters, frequency, misses and
   whether frame cadence/PTS changes. This isolates
   GPU fence completion from arbitrary time delay.
3. Compare with existing no-fence 0ms and age-gated
   16ms/25ms runs. If zero-time polling alone still
   misses first-right scanouts, that suggests GPU
   command-stream fence completion is not identical
   to i915's first-use FB readiness. Potential next
   probes: atomic `IN_FENCE_FD`, plane constraints,
   explicit sync for video decode/render dependencies,
   or kernel traces of the first right-eye flip.

This first mode verifies EGL command completion before
allowing an entire *pair* to scan out. It does **not**
guarantee that the i915 plane can latch a newly
rendered BO by the next vblank: the kernel has its
own implicit dependencies and memory/display setup,
and zero-time FD polling does not serialize them.
No added video presentation latency has yet been
measured against wall-clock optical output.

**Do not attach/drive shutter glasses** based solely
on a successful two-minute experiment: test longer
runs and recover phase on missed hardware refreshes.


### Twentieth run: EGL native-fence plus 25ms age, successful 118s smoke test

User log: `mpv-drm-stereo-pair-depth3-rt-1(20261018-221311).log`.
It confirms that the new EGL extension probe succeeds on
Intel i915/Mesa Iris Xe (TGL GT2) and the runtime prints
`Stereo native fences enabled`. Test conditions:
`MPV_DRM_STEREO_NATIVE_FENCE=1`,
`MIN_PAIR_AGE_MS=25`, `FREEZE_PAIR=0`,
`GPU_WAIT=0`, `SCHED_FIFO(10)`, normal video playback.

All 118 completed 120-flip report windows were checked:
**14,160 KMS events (~118s), no DRM SBS gap**, zero
`missed_vblanks`, `parity_errors`, `wrong_eye`,
`wrong_hold`, `pair_update_skips`, or
`commit_errors`. The reported cumulative
`shown_hz=119.997`; the per-window refresh readings
ranged from 119.995 to 119.998 Hz. Maximum observed
per-window `max_commit_callback_ms` was 9.137ms,
and `max_atomic_ioctl_ms` was 1.182ms at startup.
The movie reached ~00:01:57.

At the final complete report:
`flips=14160 pair_gen=2830 pair_renders=2831
draws=5662 first_eye_L=2827 first_eye_R=2827
native_fence=on fence_checks=2841
fence_promotions=2826 fence_deferrals=14
fence_unready_L=5 fence_unready_R=14
fence_errors=0`.

The fence gate does **actually defer** promotion.
Twelve of the fourteen failed nonblocking fence checks
happened before the first one-second report, including
five unsignaled L and twelve unsignaled R results.
The other two appeared in the ~111s and ~113s
report windows and affected the **right fence only**.
All other reports confirmed continued replacement at
~24fps and no resulting KMS timing miss.

This is a successful smoke test for EGL native-fence
creation, export, cleanup, asynchronous poll and normal
producer/worker interaction. It is **not** evidence
that native fences alone stop first-R misses, because
the previously successful 25ms CPU age gate remained
enabled. The 14 deferrals are a count of *checks*,
not necessarily fourteen distinct dropped video frames;
the initial 12 may repeatedly concern generation 1.

Recommended next controlled A/B: leave
`NATIVE_FENCE=1` and all the same mpv options,
but set `MIN_PAIR_AGE_MS=0`; play for >=10 minutes
(preferably 15) with `FREEZE_PAIR=0`.
Check `fence_errors=0`, cumulative
`fence_deferrals`, `first_eye_R_misses`,
`pair_update_skips`, and `pair_gen`.
The previous 0ms no-fence baseline had 11 missed
vblanks over ~114s. Native fence polling measures
the GL fence signaling state, **not** atomic plane's
ability to schedule a newly introduced FB by the
next scanout. If misses recur with fences signaled,
investigate DRM atomic `IN_FENCE_FD` semantics
and/or the kernel scheduling path rather than simply
declaring synchronization solved.


### Twenty-first run: native fences ALONE, 0ms age, zero KMS misses over 11m34s

The file `mpv-drm-stereo-pair-depth3-rt-1(20261018-222825).log`
was recorded on Intel i915 / Mesa TGL GT2, 119.998Hz,
normal Half-SBS 23.976fps playback, with
`MPV_DRM_STEREO_NATIVE_FENCE=1`,
`MPV_DRM_STEREO_MIN_PAIR_AGE_MS=0`,
`MPV_DRM_STEREO_FREEZE_PAIR=0`, and `GPU_WAIT=0`.
This is the first long A/B run with **no fixed
age gate** and actual nonblocking EGL native-fence
checks on both eyes.

The uploaded log contains 11,733 lines and exactly
694 complete 120-flip diagnostic windows: **83,280
confirmed DRM page-flip events** (~694s = 11m34s).
The video position reached ~00:11:34. The capture
ends without a clean player shutdown message.

All 694 reporting windows were checked:
- `missed_vblanks=0`, `window_missed=0`,
  `parity_errors=0`, `wrong_eye=0`,
  `wrong_hold=0`, `max_gap=1`, **no**
  `DRM SBS gap` entries;
- `commit_errors=0`, `pair_update_skips=0`,
  `first_eye_L_misses=0`,
  `first_eye_R_misses=0`;
- `shown_hz=119.997` throughout;
  `window_hz` ranged only 119.996–119.997Hz;
- worst per-report `max_commit_callback_ms=9.136`
  (no double-refresh ~16.6ms callback);
  `max_atomic_ioctl_ms` up to 5.551ms in one
  isolated window, with no observed KMS miss.

Final diagnostic line (flips=83,280):
`pair_gen=16654 pair_renders=16655 draws=33310
first_eye_L=16644 first_eye_R=16644
native_fence=on fence_checks=31965
fence_promotions=16643 fence_deferrals=15321
fence_unready_L=8150 fence_unready_R=15321
fence_errors=0`.

The native fences clearly **do real work**: 15,321
readiness checks at a candidate left-eye promotion
found the pair unready. In every deferred check the
right fence was unready (15,321); the left was also
unready in 8,150 of those checks. This is not the
number of unique late frames: repeated fence polls
and pending overwrites make deferral counts distinct
from video-frame drop counts. The approximately
16.6k completed new-pair promotions are consistent
with approximately 24fps video production, but
`pair_gen=16654` vs `fence_promotions=16643`
indicates some prepared pair generations may have
been superseded before first L scanout. In the present
log there is no explicit **count** of pairs superseded
before display; `pair_update_skips=0` measures a
different condition.

Run-to-run comparison:

| Mode | Normal movie duration | Missed KMS refreshes |
| --- | ---: | ---: |
| No fence; min age 0ms | ~114s | 11 |
| No fence; min age 16ms | ~661s | 3 |
| No fence; min age 25ms | ~899s | 0 |
| Native fence on; min age 25ms | ~118s | 0 |
| **Native fence on; min age 0ms** | **~694s** | **0** |

This is **strong evidence that real GPU-completion
signals can replace the fixed 25ms age threshold for
the previously observed first-R KMS miss** on this
specific i915 setup. It does not prove that a signaled
EGL fence guarantees a successful next-vblank page-flip
under every workload; additional extended or stressed
runs are needed, as well as physical optical L/R
timing verification before enabling shutter glasses.

Next: instrument the count of unpresented staging
pairs superseded before KMS promotion, together with
per-pair first-L and first-R CPU publication age
histograms, to distinguish presentation cadence
and end-to-end latency from KMS flip regularity.
Then run >=15 minutes with `NATIVE_FENCE=1`,
`MIN_PAIR_AGE_MS=0` under mixed scene/GPU load,
watching both flip misses and actual pair generations
shown. Retain the empirically supported 25ms fallback
until the fence-only mode is validated more broadly.


### Next instrumentation: count pending-pair discards and first-eye ages

After the zero-miss 694s native-fence-only run, added
lightweight diagnostic counters to distinguish perfect
120Hz *eye alternation* from possible missing 24fps
*source video frames*. This requires a new build;
the 694s user log predates these counters.

At every one-second `DRM SBS test` report:
- `pair_unpresented_discards`: counts complete pending
  stereo generations explicitly discarded on the producer
  before any KMS first-L promotion (either superseded by
  the next generation or released to relieve GBM pressure).
  This is a cumulative *KMS staging-pair loss* count, not
  necessarily an mpv decoder drop or a visible duplicate;
- `first_age_L_min_ms/avg_ms/max_ms` and
  `first_age_R_min_ms/avg_ms/max_ms`: cumulative elapsed
  CPU time between publication of a full L/R pair and
  submission of the first L or R atomic page flip using
  that pair. **Not optical presentation time**, GPU
  rendering time or calibrated video/audio latency.
  Collected only once per eye per promoted pair.

The metrics are independent of the native-fence setting
and do not change KMS flipping/pacing. The existing
`fence_deferrals` counter is a count of unsuccessful
poll checks, which is *not* a dropped-frame count.

The source edits are in `video/out/drm_common.h`,
`video/out/opengl/context_drm_egl.c`, and
`video/out/drm_common.c`. Their consistency and braces
were checked; a compile on the user's Arch Linux host
remains necessary.

Next A/B: `NATIVE_FENCE=1` and
`MIN_PAIR_AGE_MS=0` with normal playback for
~15min; examine `pair_unpresented_discards`,
first-eye age distributions, `fence_errors`,
`missed_vblanks` and `pair_gen`. Under full-rate
24fps playback, expect around 24 `pair_renders`
per second; some staging-pair drops may occur with
a nonblocking fence gate, even with zero KMS misses.
Do not confuse those events with missed 120Hz eye
alternation.


### Twenty-second run: 14m42s native-fence-only with pair-loss/age diagnostics

Uploaded `mpv-drm-stereo-pair-depth3-rt-1(20261018-225132).log`
contains **882 complete 120-flip report windows** (14m42s of
confirmed output), on i915/Mesa Tiger Lake Iris Xe.
Configuration: `NATIVE_FENCE=1`, `MIN_PAIR_AGE_MS=0`
(the startup contains no minimum-age activation message),
`FREEZE_PAIR=0`, `GPU_WAIT=0`, 119.998Hz display,
VAAPI 23.976fps Half-SBS film and the independent
SCHED_FIFO(10) KMS worker. Last movie time ~00:14:43;
log ends without an explicit clean shutdown.

**Scanout correctness:** every one of the 882 windows
reports `window_missed=0`, `missed_vblanks=0`,
`parity_errors=0`, `wrong_eye=0`,
`wrong_hold=0`, `max_gap=1`,
`first_eye_L_misses=0`,
`first_eye_R_misses=0`,
`pair_update_skips=0`,
`commit_errors=0`, and `fence_errors=0`.
There are **no** `DRM SBS gap` or error/warning
lines. Total **105,840 confirmed flips**, average
`shown_hz=119.997`, window readings between
119.996 and 119.997Hz. Largest per-window
`max_commit_callback_ms=9.148` (no ~16.6ms missed
page-flip response).

**New pair-loss accounting:** final diagnostic reports
`pair_renders=21167`, `pair_gen=21166`,
`first_eye_L=21141`, `first_eye_R=21141`,
`fence_promotions=21140`, and
`pair_unpresented_discards=25`. Thus exactly
**25 completed prepared stereo generations never
received a first L/R KMS submission** under this
measurement; one is at startup, the other 24 are
distributed over playback (several short clusters,
e.g. near 10min and 12m30s). Approximately 0.118%
of all 21,167 producer pair renders were discarded
before display, around one prepared pair every 35s
on average. This is not 25 missed vblanks, nor proof
of 25 distinct decoded PTS drops, but it does mean
there are 25 intentional pending-pair replacements/
drops at the producer-to-KMS interface. The ratio
`pair_gen - first_eye_L = 25` corroborates the
cumulative discard counter at the last full report.

**Fence polls:** `fence_checks=40531`,
`fence_promotions=21140`,
`fence_deferrals=19390`,
`fence_unready_L=10401`,
`fence_unready_R=19390`,
`fence_errors=0`. Every failed poll included the
right fence; roughly half of these also found the
left fence unready. Fence deferrals are repeated
attempts, **not** missed video frames.

**New publication-to-submit age measurements** from
the last report, cumulative over first appearances:
- L: min **2.333ms**, avg **26.271ms**, max **41.099ms**;
- R: min **8.770ms**, avg **34.601ms**, max **49.488ms**.

The avg R-L offset of ~8.33ms is consistent with one
120Hz refresh. Critically, removing the *fixed*
25ms promotion threshold has **not** shown a 25ms
reduction in CPU pair-publication-to-first-scanout
submission latency: the measured mean first-L age
in the native-fence-only run is already **26.27ms**.
However, there is no identically instrumented 25ms
baseline for a true latency delta. These metrics are
not render-start/PTS-to-photon or calibrated AV sync.

Overall this reproduces the prior 694s zero-miss
native-fence-only experiment over 882s with new
accounting; `NATIVE_FENCE=1` and no age gate is the
best *verified scanout-phase* configuration tested.
The remaining investigation is no longer primarily
the 120Hz L/R alternation (zero errors) but the
low-rate 24fps staging-pair loss and the ~26ms
first-L publication age. A one-pair pending staging
slot can be overwritten when a new rendered pair
arrives before the old pair qualifies for L-promotion;
avoid assuming a deep FIFO can be added within a
3-BO-per-eye GBM surface without checking buffer
retirement, scanout pinning and producer scheduling.

Next experiment should focus on *why* the 25
unpresented pairs were superseded: add
event-level, rate-limited diagnostics at pending
overwrite/GBM-pressure release with old pair
generation and age, both fence states, active
generation and target MSC. Add time-to-fence-signal
histograms on the producer/worker without blocking
the KMS RT thread. Use this to determine whether a
small timing adjustment in pair publication or a
safe additional ready/pending queue would improve
frame completeness and average latency; do not
enable USB shutters based solely on user-space
phase correctness.


### Native-fence pending-drop event log (following run 22)

A subsequent opt-in diagnostic change to
`context_drm_egl.c` logs each of the first 40 cases where
a previously prepared complete `pending` stereo pair
is discarded without reaching first-L KMS submission,
then every 100th subsequent event to bound logging.
Each `Stereo unpresented pair discarded:` line
contains:
- `reason=newer_generation` when the producer
  replaced the unpublished pair with another fully
  prepared generation, or `reason=gbm_pressure` when
  it freed pending BOs to permit further rendering;
- `pending_gen`, `active_gen`, and the
  `pending_age_ms` CPU time since both eyes were
  published;
- the nonblocking `fence_L` and `fence_R` state
  at the actual discard (1=signaled, 0=unsignaled,
  -1=unavailable or poll error);
- `left_boundary_fence_deferrals`, how many L
  promotion opportunities found this specific
  pending generation not ready.

These probes are performed only at uncommon
producer-side pair discard points while already
holding the pair mutex, and do not modify the
real-time KMS submit path or spin-wait on GPU.
The existing `pair_unpresented_discards` cumulative
counter remains incremented exactly once per
discard. A signaled fence *at discard time* does
not prove the fence was ready at the earlier
promotion attempt.

Next run with native fences enabled and age gate
0ms for ~10–15min. Analyze whether discards
mostly occur because of still-unready right fences
or because a now-ready pending frame missed its
L-boundary window. This determines whether to
improve producer phase/queue capacity vs scheduling
new KMS atomic FBs with explicit plane fences.
The patch was source-reviewed but not compiled
on the target user's Intel host.


### Twenty-third run: 18 pending generations dropped, all ready at discard (14m27s)

Log: `mpv-drm-stereo-pair-depth3-rt-1(20261018-231530).log`,
14,552 lines, **867** full 120-flip KMS reports, on Intel
i915/TGL GT2, native EGL fences on, age gate off,
SCHED_FIFO(10), full-rate stereo movie playback.
Last reported KMS flips **104,040** (~867s = 14m27s),
`shown_hz=119.997`, `window_hz` 119.996–119.998Hz.
Zero `DRM SBS gap`, missed vblank, parity/eye mistakes,
`pair_update_skips`, `fence_errors`, or commit failures.
Worst per-second max DRM commit-to-callback duration
was **9.136ms**, with no ~16.6ms late flip.

Last counters:
`pair_gen=20806 pair_renders=20807
first_eye_L=20788 first_eye_R=20788
pair_unpresented_discards=18
fence_checks=39792 fence_promotions=20787
fence_deferrals=19004 fence_unready_L=10240
fence_unready_R=19004 fence_errors=0
first_age_L_min_ms=3.253 first_age_L_avg_ms=26.236
first_age_L_max_ms=41.189
first_age_R_min_ms=8.114 first_age_R_avg_ms=34.562
first_age_R_max_ms=49.500`.

**Crucial new finding from event-level drop logs:**
Exactly **18** complete pending generations were discarded
(18/20807 = 0.0865% of producer pair renders).
**All 18** were discarded with reason
`newer_generation`; none via `gbm_pressure`.
Every drop event showed `fence_L=1 fence_R=1`
at the time the *next* generation overwrote pending.
Seventeen discarded pairs had exactly
`left_boundary_fence_deferrals=2`; one (generation
10668) had **3** unsuccessful earlier left-boundary
promotion checks. The discarded pairs' publication-to-
discard ages ranged from **33.258ms** to **42.035ms**
(median **41.733ms**, mean **39.782ms**).

Representative log: `pending_gen=5667 active_gen=5666
pending_age_ms=41.764 fence_L=1 fence_R=1
left_boundary_fence_deferrals=2`.
There are several short clusters, notably around
5m, 7m25s, and ~12m of movie playback; counts remain
small and KMS alternation remains flawless.

Interpretation: the producer overwrites a previous
pending frame that had missed earlier *L-only*
eligibility opportunities because its fences were
unsignaled then. It is **incorrect** to conclude that
the GPU was ready at those earlier L boundaries just
because both fences had signaled by the time of
replacement. Most drops occur near the film-frame
period (~41.71ms), suggesting interaction between
GPU readiness, 16.67ms eligible L promotion intervals,
and one-slot pending generation storage. This remains
a hypothesis until the age at each deferral is
measured.

Following this log, a new per-pair diagnostic collects
`first_deferral_age_ms`, `last_deferral_age_ms`,
and `last_deferral_target_msc` and prints them in
the existing `Stereo unpresented pair discarded:`
log line. These fields record actual unsuccessful
L-boundary checks on the KMS worker; they are separate
from `pending_age_ms` and final fence states at
producer discard. This does not add waits or alter
promotion scheduling. It requires recompiling mpv.

Next controlled test: continue `NATIVE_FENCE=1`,
`MIN_PAIR_AGE_MS=0` normal playback (~10–15min),
and compare first/last deferral ages with the ~34–42ms
overwrite ages. Do not attempt to promote new stereo
pairs at right-eye boundaries: that could display
L and R from different movie frames. Do not
prematurely add a FIFO without accounting for only
three BOs per eye on these Mesa GBM surfaces and
the active/pending/building/retired frame lifetimes.


### Twenty-fourth run: 939s native fences, 17 pending drops, exact ages of L deferrals

User log `mpv-drm-stereo-pair-depth3-rt-1(20261018-233639).log`
(15,763 lines), Intel i915/Mesa Iris Xe TGL GT2,
119.998Hz display, 23.976fps normal VAAPI Half-SBS
movie playback, `NATIVE_FENCE=1`,
`MIN_PAIR_AGE_MS=0`, `GPU_WAIT=0`,
`FREEZE_PAIR=0`, `SCHED_FIFO=10`. The log
contains 939 complete 120-flip reports (15m39s
confirmed KMS output), **112,680 confirmed page
flips**, and no clean exit message.

All 939 diagnostic windows are healthy:
`shown_hz=119.997` (window frequencies
119.996–119.997Hz), zero `DRM SBS gap`
events, zero `missed_vblanks`,
`parity_errors`, `wrong_eye`, `wrong_hold`,
`first_eye_L_misses`, `first_eye_R_misses`,
`pair_update_skips`, `commit_errors`, or
`fence_errors`; `max_gap=1`. The highest
reported `max_commit_callback_ms=9.158`ms,
with no 16.6ms callback outlier.

At final report: `pair_gen=22534`,
`pair_renders=22535`, `draws=45070`,
`first_eye_L=22517`, `first_eye_R=22517`,
`pair_unpresented_discards=17` (17/22535
= **0.0754%** of generated pairs),
`fence_checks=43138`,
`fence_promotions=22516`,
`fence_deferrals=20621`,
`fence_unready_L=11110`,
`fence_unready_R=20621`,
`fence_errors=0`.
The first-eye submission age from pair publication:
L min/avg/max=**3.223/26.267/41.030ms**;
R min/avg/max=**11.244/34.591/49.272ms**.
These are CPU publication-to-atomic-submit ages,
**not optical/image or calibrated A-V delay**.

The 17 event-level records all show
`reason=newer_generation`, and **all 17**
have `fence_L=1 fence_R=1` at the time of
producer-side replacement. **Every one** has
exactly two earlier unsuccessful eligible left-eye
fence checks (`left_boundary_fence_deferrals=2`).
New timestamps locate these failures very tightly:

- first L-boundary deferral: pair age
  **7.570–15.313ms** (median 15.185ms);
- second L-boundary deferral: pair age
  **24.239–31.978ms** (median 31.857ms);
- pending overwritten: age
  **34.205–42.014ms** (median 41.729ms);
- age difference from *last* failed check to
  overwrite **9.690–10.218ms** (mean 9.912ms).
- Typical failed pair 369: first deferral
  **15.304ms**, second **31.890ms**, both
  GPU fences signal by producer-side overwrite
  at **41.861ms**; its last failed target MSC
  was `5563681`.

Thus the troublesome window is now well localized:
the pair is not fully fence-ready at two successive
left-eye promotion opportunities, 16.67ms apart,
but becomes fence-ready **sometime within the
following ~10ms**, before the source-frame cadence
(~41.71ms) replaces it. Another right-eye vblank
at +8.33ms lies within this gap, but the code
deliberately must not promote a fresh pair on
right-eye boundaries (would mismatch generations
between eyes). This pinpoints a *fence-vs-L-boundary*
phase issue combined with one-slot pending
replacement; it does not prove an exact GPU
completion timestamp, nor show that increasing
GBM buffer count is currently safe.

**Next suggested instrumentation:** in the independent
KMS worker, on right-eye boundaries ONLY for
already twice-deferred pending pairs, perform
a diagnostic, nonblocking poll of both fences.
Record whether the pair became ready at that
intermediate right-eye event and the corresponding
CPU age / target MSC, without promoting it.
This distinguishes a fence completion before
the R event (~8.33ms after the last L failure)
from completion later, and adds no GPU wait.
Such a diagnostic is preferable to blindly adding
a deeper pair queue to the current GBM surfaces.
Additional out-of-band GPU fence timestamps could
narrow this interval further if needed.

Comparative zero-miss native-fence-only runs:
694s/0 missed vblanks;
882s/0 with 25 pending drops;
867s/0 with 18 drops;
939s/0 with 17 drops. More important than the
KMS success alone is separately quantifying the
~0.08–0.12% staging-pair losses before planning
active shutter glasses.


### Diagnostic extension after run 24: intermediate right-eye fence probe

Commit `f7eda9d018ad9f13d948e59f8004261196f5d57e`
adds a **nonblocking, diagnostic-only** fence
probe on the right-eye KMS refresh **immediately
following the second failed left-eye fence poll**
for a pending stereo pair. It is only attempted
when all of the following hold:
- native EGL fences enabled, pending complete;
- current hardware target is **R**;
- that pending generation has already been
  deferred on **at least two** L attempts;
- the target MSC equals the second failed
  L target MSC + 1, and no R sample is stored.

The additional `poll(fd, timeout=0)` operations
never block the KMS worker and never promote a
new pair on an R boundary. Only rare two-deferral
pairs incur this extra diagnostic overhead.

The existing discard event log now includes
`intermediate_R_probe_age_ms`,
`intermediate_R_probe_msc`,
`intermediate_R_fence_L`, and
`intermediate_R_fence_R` (both 1=ready,
0=not ready, -1=poll error/unavailable/no sample).
An age of -1 and MSC 0 mean no intermediate
R sample was obtained.

At 120Hz this R probe occurs approximately
**8.33ms** after the last failed L check and,
based on run 24, approximately 1.4–1.9ms before
the newest pair overwrites the old one. Thus
it should usually tell whether the pair had
become ready by that **last intermediate R**
slot, even though promoting at that point would
produce mismatched L/R film generations and is
prohibited.

This adds logging/telemetry only and **does not
address staging pair discards**. Follow-up run:
rebuild mpv and play 10-15min with
`NATIVE_FENCE=1` and `MIN_PAIR_AGE_MS=0`,
then compare the new R-probe fields in each
discard record with the existing first/last
L deferral ages. If ready at R, stage more
safely or change producer timing instead of
allowing an unsafe mid-pair replacement.
If still not ready at R, optimize GPU fence
completion / resource pressure before
considering deeper pair queues. 


### Twenty-fifth run: 517s intermediate-R probe confirms fences ready before overwrite

User log: `mpv-drm-stereo-pair-depth3-rt-1(20261018-235143).log`
(8,789 lines). On the same Intel i915 / Mesa TGL GT2
platform, the native-fence-only `MIN_PAIR_AGE_MS=0`
path confirms **62,040 KMS flips in 517 complete
120-flip windows (8m37s)**. The file ends at movie
time ~00:08:38, without a clean shutdown line.
This is **not** a 15-minute run; only the available
8m37s can be evaluated.

All 517 intervals have `missed_vblanks=0`,
`window_missed=0`, `wrong_eye=0`,
`wrong_hold=0`, `parity_errors=0`,
`first_eye_L_misses=0`,
`first_eye_R_misses=0`, `commit_errors=0`,
`pair_update_skips=0`, `fence_errors=0`,
with no `DRM SBS gap` events and
`shown_hz=119.997`. Maximum observed
per-window `max_commit_callback_ms=9.115`ms.

Last complete report:
`pair_gen=12406 pair_renders=12407 draws=24814
first_eye_L=12401 first_eye_R=12401
pair_unpresented_discards=5
fence_checks=23770 fence_promotions=12400
fence_deferrals=11369 fence_unready_L=6047
fence_unready_R=11369 fence_errors=0
first_age_L_min_ms=2.282 first_age_L_avg_ms=26.291
first_age_L_max_ms=41.146
first_age_R_min_ms=5.825 first_age_R_avg_ms=34.613
first_age_R_max_ms=49.411`.
Five superseded pending pairs out of 12,407 producer
pair renders, i.e. **0.0403%** of generated pairs.

The critical extra diagnostic from the
**intermediate R-vblank probe** is now confirmed
on all five logged discarded generations:

| pending_gen | Last failed L age (ms) | R probe age (ms) | R fence L/R | Overwrite age (ms) |
| ---: | ---: | ---: | :---: | ---: |
| 1528 | 30.518 | 38.736 | 1/1 | 40.310 |
| 7844 | 31.875 | 40.141 | 1/1 | 41.668 |
| 8474 | 31.949 | 40.171 | 1/1 | 41.759 |
| 11770 | 31.921 | 40.239 | 1/1 | 41.748 |
| 11866 | 29.554 | 37.807 | 1/1 | 39.363 |

Every discarded pair:
- was deferred twice at an eligible L refresh
  (`left_boundary_fence_deferrals=2`);
- was **already signaled on both fences by the
  intermediate R refresh**, ~8.22–8.32ms
  after the second failed L;
- was discarded just **1.51–1.59ms** after
  the R probe, due to `newer_generation`;
- had both fences signaled at discard, too;
  no `gbm_pressure` discards occurred.

This locates the readiness transition only to the
interval **after the last failed L** but **no later
than the subsequent R probe**. It does *not*
prove GPU work completed at R or supply exact
GPU fence signal timestamps. On the R event
the KMS worker must keep the previous generation:
promoting a new pair on R would violate the
guarantee that both eyes see the same movie frame.

At 120Hz the next permissible L is around
8.33ms after the diagnostic R probe, while the
24fps producer overwrites the one-slot
`pending` approximately 1.5ms after R.
Thus the old pending pair is ready in time for
a **future L**, but gets overwritten before
that opportunity. This is now primarily a
**single-slot staging / scheduling collision**
rather than a missed 120Hz KMS refresh.

**Design direction:** do not keep adding fence
telemetry just to rediscover this collision.
Preserve chronological generation order and
only promote on L, while safely retaining up
to one older ready pair alongside a newer
queued generation. Must FIRST account for GBM
surface locked-buffer capacity, active/in-flight
BO retirement, and the producer's pending
release path: an unbounded FIFO or naive
extra `stereo_pair` field could starve
`gbm_surface_lock_front_buffer`.
An alternative controlled experiment is
bounded producer-side backpressure until
the next L, with explicit measurements of VO
stalls and video drops. Either strategy
should be **opt-in** and keep the present
fence-only mode as an unchanged baseline.

Before selecting queue vs bounded backpressure,
inventory exact per-eye BO residency at moments of
discard, retirement and swap under the current
GBM surfaces, then instrument total discarded
video generations versus KMS flips and
publication-to-submit latency. Avoid
synchronizing USB shutters until parity
recovery and optical shutter timing are tested.


### Twenty-sixth implementation: public --drm-stereo-3d=yes and logical 60 Hz VO

A new **native mpv command-line option** is registered in
`drm_conf`: `--drm-stereo-3d=<yes|no>` (default **no**).
This is deliberately scoped to `--vo=gpu --gpu-context=drm`,
120 Hz physical DRM/KMS and the existing **Half-SBS** input
layout. It is **not** yet a generic stereo-format parser, an
OSD stereo renderer or USB shutter-glasses driver.

Example initial test (requires recompiling the fork):

```sh
mpv --no-config \
  --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode='2560x1440@120' \
  --drm-vrr-enabled=no \
  --drm-stereo-3d=yes \
  --video-sync=display-resample \
  --swapchain-depth=3 --hwdec=auto \
  --log-file=/tmp/mpv-stereo-logical60.log \
  /path/to/half-sbs.mkv
```

No `MPV_DRM_STEREO_SBS`, `MPV_DRM_STEREO_PAIR`,
`MPV_DRM_STEREO_RT`, or
`MPV_DRM_STEREO_NATIVE_FENCE` variables are
needed for this new mode. It enables **paired L/R GBM
scanout**, **the RT KMS worker**, and **EGL native GPU
fence checks** by default. Lack of required native EGL
fence extensions fails initialization rather than
degrading to the formerly unstable non-fence path.
The preexisting environmental settings remain as
legacy experimental controls **when the new option is
off**; they still report the physical 120 Hz cadence.

**All three timing paths were modified together:**

1. `VOCTRL_GET_DISPLAY_FPS` returns **half** the
   actual DRM connector mode's refresh rate only when
   `--drm-stereo-3d=yes`. The hardware mode selected
   and `vo_drm_get_display_fps()` remain unchanged.
   At measured 119.998Hz KMS refresh mpv sees
   approximately **59.999Hz**.
2. `drm_pflip_cb` continues counting and validating
   **every physical L/R page-flip event** at 120 Hz.
   But in this mode, the `present_sync` interface
   receives **only completed L events** and a logical
   MSC = physical MSC / 2 (integer division). The
   latter is essential: merely feeding alternate
   physical MSC values would make `present_sync`
   divide 16.67ms by a two-count difference and
   mistakenly estimate an **8.33ms** vsync period.
   Logical consecutive L events now yield both a
   ~16.67ms UST separation **and** a one-count MSC
   difference for reliable mpv `get_vsync` pacing.
3. The VO's `flip_page` uses the existing blocking
   helper `vo_drm_stereo_pair_wait_vsync` but is
   awakened **only after an L scanout callback**,
   rather than every physical R callback too.
   Meanwhile the standalone KMS worker keeps
   alternating L/R at 120Hz, independent of the VO.

Thus `player/video.c`'s existing
`handle_display_sync_frame()` computes repeat counts
and long-term A/V drift correction on logical **full
L/R cycles** instead of individual eye exposures.
For 23.976fps content on ~60Hz logical output the
expected nominal frame cadence is approximately
**2/3 full stereo cycles**. Eye-selection parity
is still derived from **physical MSC**; no source
film frame may begin with R.

Startup diagnostics include:
`DRM stereo 3D: physical 119.998 Hz L/R, logical
59.999 Hz L-only player pacing and presentation
feedback (native GPU fences on)`. At verbose log
level, mpv VO should also announce
`Assuming 59.999 FPS for display sync`.

**Important limits of this first implementation:**
- This changes VO cadence and presentation feedback,
  not yet PTS-to-actual-GBM-generation accounting.
  The KMS worker may still supersede a pending pair
  before showing it. A zero mpv A/V error does NOT
  prove exact audio-to-photon delay or zero staging
  drops. Keep collecting `pair_unpresented_discards`,
  eye first ages, `fence_errors`, physical flip
  counters and `DRM SBS gap` events.
- Do **not** set `--display-fps-override=120`: this
  forces mpv's display-sync scheduler back to 120Hz
  despite the new logical mode. Let auto-detection
  use the DRM-reported half-rate value.
- `--drm-stereo-3d=no` is the default for normal
  2D playback. The old env-var experimental mode
  remains intact for A/B comparison.
- Physical monitor refresh remains 120Hz. The USB
  glasses controller is **not enabled** and must
  eventually follow confirmed physical eye phase,
  never the half-rate film frame schedule.
- This source update has **not been compiled on the
  user's Arch / Intel test host**. Check option
  parsing and playback logs first; only then perform
  10-15min comparisons. Do not infer correctness
  solely from code inspection.

**A/B checklist:** with clean environment variables,
compare 3D option **on** (logical ~60Hz, actual
120Hz L/R KMS, expected 2/3 repeat scheduling)
against old env-based paired mode with option **off**.
The first test should verify exact physical
`shown_hz`, `missed_vblanks`, `wrong_eye`,
`fence_errors`, `pair_unpresented_discards` and
film/audio times. A sufficiently long second test
should compare total dropped pending pairs at equal
content/GPU load. Further planned work: associate
`vo_frame.frame_id` / PTS with physical
`stereo_pair.generation` and report actual accepted
versus superseded video frames to the player.


### Twenty-seventh run: public 3D CLI + 60Hz logical VO increases staging drops

User log: `mpv-stereo-60hz.log` on Oct 19, 2026
(mpv `v0.41.0-dev-g49e649fcb`, i915 Mesa TGL GT2).
`--drm-stereo-3d=yes --video-sync=display-resample`
with 2560x1440 physical 120Hz mode and VRR off,
swapchain depth 3, VAAPI decoding of 23.976fps
Half-SBS film. This confirms successful compilation,
option registration, and runtime operation of the
new half-frequency VO scheduler.

Startup:
`DRM stereo 3D: physical 119.998 Hz L/R, logical
59.999 Hz L-only player pacing and presentation
feedback (native GPU fences on)`;
`Assuming 59.998795 FPS for display sync.`;
`Video sync mode enabled.`. At end the player
received an explicit quit command; video played
~115.98s, not an unbounded stress run.

All **115 complete** 120-physical-flip windows
(13,800 scanouts in 115s) had zero physical
`missed_vblanks`, `wrong_eye`,
`parity_errors`, `first_eye_L_misses`,
`first_eye_R_misses`, `commit_errors`,
`pair_update_skips`, or `fence_errors`,
`max_gap=1`, `shown_hz=119.997`.
The new logical 60Hz signaling did NOT disrupt the
independent 120Hz KMS L/R alternation.

**But logical VO pacing dramatically worsened
movie-frame generation retention:** at last complete
report `pair_renders=2758`,
`pair_gen=2757`,
`first_eye_L=2716 first_eye_R=2716`,
`pair_unpresented_discards=41` (**1.49%** of
prepared stereo generations), vs the previous
517s/5 drop baseline (**0.040%**) and earlier
939s/17 drop run (**0.075%**). These runs have
different duration and are not precise matched
workload benchmarks; nevertheless the increased
loss is substantial. The code currently does
not propagate staging losses to mpv's main
`Dropped` video-frame count.

Forty event-level discarded generations were
logged, each `reason=newer_generation` and
`left_boundary_fence_deferrals=2`, both fences
signaled **at discard**. Their median publication
ages: first failed eligible L **6.858ms**, last
failed L **23.517ms**, intermediate R check
**31.885ms**, overwritten **33.504ms**.
At the intermediate R checkpoint, **18/40** had
both GPU fences signaled and **22/40** still had
an *unsignaled right fence*. No `gbm_pressure`
discards. Cumulative ages of successfully first
submitted pairs: L avg **23.726ms**, R avg
**32.065ms**; all have an approximately one
physical-vblank L-to-R difference.

This contrasts with previous 120Hz-VO callback
scheduling when failed-pair last-L checks
clustered around **31.9ms**, intermediate R
around **40ms**, overwrite around **41.7ms**.
The almost **8.3ms** phase change is a strong
hint that 60Hz VO pacing is placing new pair
publications disadvantageously with respect to
eligible L boundaries. The correct 60Hz
*frequency* does not determine the correct
*phase*. No mpv DS:2/3 status is present in
the captured file; do not pretend per-frame
display cadence is explicitly verified.

**Follow-up experiment (opt-in, following the log):**
Added `--drm-stereo-pacing-eye=<left|right>` with
default left preserving this tested behavior.
`right` moves only mpv's logical wakeup signal
from confirmed L scanouts to confirmed R scanouts
(~8.33ms later). Hardware KMS still commits
L/R at 120Hz and promotes new movie pairs only
on L. The `present_sync` UST in R-pacing mode
is sourced from the most recent confirmed L
event, with a logical MSC advancing by one per
full L/R cycle, to avoid reporting the R callback
as the film-frame presentation timestamp.

Test on the user's machine with the **same**
`--drm-stereo-3d=yes` options, and add
`--drm-stereo-pacing-eye=right`. First validate
startup, absence of KMS misses and roughly normal
A/V speed; then compare `pair_unpresented_discards`
and L/R first-age statistics over matched 2-minute
segments (ideally same movie scenes). If it
does not substantially improve pair losses
without A/V/DRM regressions, revert to
`--drm-stereo-pacing-eye=left` and examine
explicit PTS/VO frame_id-to-GBM generation mapping,
rather than piling up a FIFO.

**Limitation:** R-pacing has only been source
reviewed, not compiled or tested on the user's
hardware. It is a controlled scheduling-phase
experiment, *not* a finalized fix or proof of
audio-to-photon correctness.


### Twenty-eighth run: R-pacing causes severe periodic 120Hz KMS gaps

User log `mpv-stereo-60hz(1).log`, Oct 19, 2026,
mpv `v0.41.0-dev-g58c06c0a1`,
`--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`,
`--video-sync=display-resample`,
120Hz physical i915/TGL DP-1 and 23.976fps Half-SBS
film. Runtime ~114s, explicitly quit. Output log
contains 1,326 lines and **113 complete 120-flip
DRM SBS reports** (13,560 physical page-flip callbacks
at the last full report).

**Severe R-phase regression:**
- Last cumulative `missed_vblanks=108`,
  `parity_errors=216`,
  `wrong_eye=108`,
  `wrong_hold=108`,
  `max_gap=2` (window misses start after the fifth
  window). There is **one additional gap warning
  after** the final complete report, making
  **109 logged `DRM SBS gap` warnings** overall.
- Last `shown_hz=119.048`; trailing windows
  `window_hz=119.005`, not the expected 119.997Hz.
  Almost every one-second diagnostics window
  contains precisely one missed physical vblank.
- `pair_renders=2731`,
  `pair_gen=2730`,
  `first_eye_L=2369`,
  `first_eye_R=2369`,
  `pair_unpresented_discards=361` — **13.2%** of
  2731 rendered pairs, vs 41/2758 (**1.49%**) in
  the roughly equivalent 115s original L-paced
  experiment. `first_eye_L_misses=37`,
  `first_eye_R_misses=18`. Mean first L/R pair
  ages 26.960 / 35.431 ms, respectively.
- `fence_checks=4791`,
  `fence_promotions=2368`,
  `fence_deferrals=2422`,
  `fence_unready_L=502`,
  `fence_unready_R=2422`,
  `fence_errors=0`,
  `commit_errors=0`,
  `pair_update_skips=0`.
  Thus the regression occurs despite valid fence
  polling and atomic commits.

**An exceptionally strong periodicity clue:**
All **109** gap warnings appear **immediately
after** a `DRM SBS test:` report at every 120th
completed flip, starting with the report at
5.303s (flips=600), then 6.311s (flips=720), etc.
First gap 5.319s, then 6.327s, 7.336s ...
114.222s; consecutive gap warnings are separated
by only **1.008–1.009 seconds** (mean 1.00836s),
equivalent to 121 physical refreshes per
120 confirmed flips. The first five report windows
had no misses.

Critically, at the fifth report the already
measured `max_callback_lag_ms=6.331` and
`max_commit_callback_ms=14.896` reveal a large
callback timing outlier, and subsequent gap warnings
show `submit_after_callback_ms` only 0.005–0.014ms
while `submit_after_ust_ms` often reaches
14–15ms. **Hypothesis, not proof:** expensive
`MP_INFO` printing of the long per-second
`DRM SBS test` status **inside the real-time
KMS callback and while holding the pair mutex**
is delaying the next atomic commit under the
R-phase producer scheduling. The exact
120-flip cadence is much too systematic to
ascribe to random GPU workload. A first diagnosis
should suppress only the INFO output of
`vo/gpu/drm` and retain its WARN diagnostics:

```sh
# Repeat the same right-eye command, adding:
--msg-level=vo/gpu/drm=warn
--log-file=/tmp/mpv-stereo-60hz-right-nostats.log
```

This mpv option is officially supported
(`DOCS/man/options.rst`, `--msg-level`), and
the module name matches the log prefix. It
suppresses per-second INFO lines but leaves
`DRM SBS gap` warnings visible. A 20–30s test
can determine whether the periodic KMS gaps
are caused by synchronous logging: if the
per-second warnings disappear, move telemetry
outside the RT critical path; if they persist,
investigate R-phase scheduling itself.

**Do not deploy R-pacing** or synchronize shutter
glasses with this build: every skipped physical
vblank can invalidate eye phase. The proven
baseline is `--drm-stereo-pacing-eye=left` /
the original default. Regardless of the logging
hypothesis, R-pacing also suffers an unacceptable
~13% unpublished-pair loss rate and therefore
is not presently a candidate for production.
Investigate L-mode frame pacing and honest
PTS/VO frame_id-to-KMS promotion feedback
rather than simply pushing renders onto R.

The trailing message
`Failed to commit atomic request: Error number 22
occurred` appears **after** the quit/shutdown
sequence, not as an in-play stereo atomic-commit
error (the in-play counter remains zero). Treat
this as a separate teardown issue until
independently reproduced.


### Twenty-ninth run: right logical pacing, 60s, no KMS gaps; msg-level does NOT suppress logfile INFO

User control log `mpv-stereo-right-nostats.log`,
built from `v0.41.0-dev-g58c06c0a1` on i915
Tiger Lake + Mesa 26.2.4, Oct 19 2026. Launch
used `--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`,
`--msg-level=vo/gpu/drm=warn`,
`--log-file=/tmp/mpv-stereo-right-nostats.log`,
physical 2560x1440@120, VAAPI decode of the
same 23.976fps Half-SBS movie.
Playback stopped with an explicit quit around
61.106s, after **60** complete 120-flip
report windows (**7,200 confirmed physical
KMS flips**, 60s sampled).

Results at last complete window:
`shown_hz=119.997 window_hz=119.997`,
`missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 max_gap=1
first_eye_L_misses=0 first_eye_R_misses=0
pair_update_skips=0 commit_errors=0
fence_errors=0`. Worst per-report
`max_commit_callback_ms=9.058`ms.
`pair_renders=1437`, `pair_gen=1437`,
`first_eye_L=1433`, `first_eye_R=1433`,
`pair_unpresented_discards=4`, **0.278%**;
`fence_checks=2038 fence_promotions=1432
fence_deferrals=605 fence_unready_L=20
fence_unready_R=605`.
First-submission age: L mean **22.121ms**, R
mean **30.467ms**. There are exactly four
`Stereo unpresented pair discarded:` event
records (all newer-generation replacement).
One occurred at 0.393s startup; the other three
at ~13.34s, 42.257s and 42.339s. Both GPU
fences signaled by the discard in all cases.
The event-level `intermediate_R_probe` was
absent on all four, which is expected if the
twice-failed L was already followed by producer
replacement before another R probe.

**This run directly contradicts the notion that
R pacing necessarily causes the earlier severe
109 periodic KMS gaps or ~13% frame-generation
discard rate.** It ran reliably for 60s. But it
does NOT isolate the previous suspected cause:

Despite the `--msg-level=vo/gpu/drm=warn` setting,
the uploaded log still contains **all 60 verbose
`[i][vo/gpu/drm] DRM SBS test:` lines**. Review
of `common/msg.c` `update_loglevel()` and
`write_msg_to_buffers()` shows that mpv's
`--log-file` requests DEBUG-level messages
even when terminal module level is WARN, and
`MP_INFO` is still executed inside the KMS callback.
Thus this run is a **successful right-phase repeat
with status logging still active**, not proof that
disabling INFO fixes the old periodic gaps. The
prior 113s bad run and this 60s good run are
non-identical trials with unexplained variability,
and the single-second correlation between
`DRM SBS test` and misses from the bad run
is not enough to prove causation.

After this run, a *real, explicit* opt-out has
been added: `--drm-stereo-stats=no`
(default yes, preserving all existing behavior).
It guards only the expensive formatting and
`MP_INFO` call for the per-120-flip report in
`drm_pflip_cb`, still updating the telemetry
counters and leaving `DRM SBS gap` warning
messages visible. The first correct overhead
isolation experiment should run R pacing with
**`--drm-stereo-stats=no`**, `--log-file` kept
enabled for error recording. Check that
`DRM SBS test:` lines are genuinely absent;
continue at least ~2min to cover the duration
that previously showed systematic misses.
To evaluate video staging drops, temporarily
repeat with stats=yes at comparable scene/time
windows since without periodic status logs we
no longer get the final cumulative discard
counter (individual first 40 drop event records
are still available).

This stats opt-out has only been source reviewed,
not compiled or run on the target hardware.
Log suffix still reports a shutdown-time DRM
atomic EINVAL *after* playback termination;
not an in-play atomic commit failure.


### Thirtieth run: R-paced stereo, actual stats=no control, ~91.95s

Uploaded log: `mpv-stereo-right-quiet.log`, mpv
`v0.41.0-dev-g3e807a641` (Oct 19, 2026),
physical 2560x1440@120Hz Intel i915 / Mesa TGL GT2,
23.976fps Half-SBS film, `--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`,
`--drm-stereo-stats=no`,
`--video-sync=display-resample`, `--swapchain-depth=3`,
`--log-file` still enabled. Player quit command
at ~91.954s; shutdown ended ~92.096s.

The new no-stats feature **is actually working**:
**0** `DRM SBS test:` periodic INFO messages
in the 1,085-line log. There are **0**
`DRM SBS gap` warnings and **0**
`Stereo KMS flip failed` messages, with
`SCHED_FIFO priority 10` startup confirmed and
the mpv scheduler using `59.998795 FPS`.
The last log line is a known shutdown-only
`Failed to commit atomic request: Error number 22`,
after the explicit quit and resource teardown,
not evidence of an in-play KMS failure.

**Do not interpret absence of the warnings as a
measured zero `missed_vblanks` cumulative total.**
With periodic stats disabled, this revision does
not print its final KMS counters; the lack of
warnings is strong evidence but not a full
aggregate. There is also no direct calibrated
PTS-to-photon/A-V offset measurement.

The complete log has exactly **36**
`Stereo unpresented pair discarded:` records,
last `count=36`, all
`reason=newer_generation`,
`fence_L=1 fence_R=1` **at discard**.
The first drop is an early startup
generation (`gen=2` at 0.458s; only one
L-boundary deferral and 16.853ms age);
all other **35** pairs have exactly
two earlier failed eligible L-promotion
fence polls. For these 35:
- first failed L-check age median
  **15.147ms** (range 13.851–15.549ms);
- second failed L-check age median
  **31.851ms** (range 30.618–32.222ms);
- producer replacement age median
  **34.445ms** (range 33.248–37.820ms);
- time from second failed L to replacement
  median **2.491ms**, mean **2.918ms**
  (range 1.370–7.202ms);
- all `intermediate_R_probe_age_ms=-1`:
  the producer replaced the pending pair
  **before** the next R checkpoint, so
  there was no opportunity for that probe.
  Do not misinterpret unavailable probes
  as GPU fence failure.

The last logged discarded pair was at ~87.407s.
Since the final cumulative
`pair_renders` figure is unavailable,
report **36 actual superseded generations**,
not a precisely measured loss percentage.
Using ~23.976fps for ~91.5s of active video
would imply approximately 2,200 source-frame
opportunities and a rough ~1.6% ratio, but
this is an **estimate**, not a captured
diagnostic denominator.

Compared with the previous `right` A/B run
(with INFO logs still enabled), 60s/4 discarded,
this new 92s/36 discarded shows substantial
run-to-run variability. Compared with earlier
113s `right` 108 missed vblanks and 361
pending discards, it is a clear improvement
in observed KMS regularity, but not a
reliable demonstration that verbose log output
*caused* all the previous failures: the
previous 60s run also succeeded **with INFO
logging still active**. More controlled
matched-length, matched-film-time A/B repeats
are required for causation.

**Next source improvement after this run:**
A single `Stereo DRM final:` snapshot is now
printed from `drm_egl_uninit` **after joining
the realtime KMS worker**, but before tearing
down the DRM state. It includes physical flips,
missed vblanks, parity/wrong-eye/hold errors,
pair render/generation counts, unpresented pair
discards, first L/R submissions and their
misses, fence checks/promotions/deferrals/errors,
pair update skips and atomic commit errors.
This adds **no per-refresh or periodic logging**
while `--drm-stereo-stats=no`, and future
quiet-mode runs can calculate actual drop
percentages without guessing source cadence.
Needs rebuilding; no target-host compilation yet.

The right-pacing phase remains experimental;
avoid driving active shutter glasses while
its physical reliability under lengthy runs
has not been independently established.


### Thirty-first experiment: matched quiet-mode LEFT / RIGHT pacing A/B

User uploaded two complete logs on Oct 19, 2026:
- `mpv-stereo-left-quiet.log`
- `mpv-stereo-right-quiet(1).log`

Both run **the exact same built mpv commit**
`v0.41.0-dev-gdf3cf3d9b`, same Intel i915 /
Mesa Tiger Lake hardware and 2560x1440@120 DP,
23.976 fps Half-SBS *Venom* source,
`--drm-stereo-3d=yes`,
`--video-sync=display-resample`,
`--swapchain-depth=3`,
`--hwdec=auto`,
`--drm-stereo-stats=no`.
Only `--drm-stereo-pacing-eye` changed:
`left` versus `right`.
Both logged `Assuming 59.998795 FPS for
display sync`, `Video sync mode enabled`,
and `Stereo KMS thread scheduling:
SCHED_FIFO priority 10`.
Both shutdown cleanly after explicit quit
except for a **post-playback** DRM teardown
atomic EINVAL message. There were no
`DRM SBS test` periodic reports in either log.
The new `Stereo DRM final:` after-RT-thread
snapshot worked.

| Final counters | left | right |
|---|---:|---:|
| Last physical KMS flips | 13,983 | 13,649 |
| Approx. KMS time at 119.997 Hz | 116.5s | 113.7s |
| `missed_vblanks` | **0** | **0** |
| `wrong_eye` / `wrong_hold` | **0 / 0** | **0 / 0** |
| `parity_errors` | **0** | **0** |
| `first_eye_L_misses` / R misses | 0 / 0 | 0 / 0 |
| `pair_renders` | 2,794 | 2,727 |
| `pair_gen` | 2,794 | 2,727 |
| `pair_unpresented_discards` | **48** | **19** |
| Drop fraction per render | **1.718%** | **0.697%** |
| `first_eye_L` | 2,746 | 2,708 |
| `first_eye_R` | 2,746 | 2,707 |
| `fence_checks` | 5,604 | 4,296 |
| `fence_promotions` | 2,745 | 2,707 |
| `fence_deferrals` | **2,858** | **1,588** |
| `fence_errors` | 0 | 0 |
| `pair_update_skips` | 0 | 0 |
| `commit_errors` | 0 | 0 |

In this A/B run R-pacing cut the fraction
of prepared pairs overwritten without being
shown to **40.6%** of the L result (about
**2.47x fewer**). This is not a strict
repeatable performance guarantee; the scene
windows differ by roughly 3 seconds and
previous R-pacing runs varied dramatically.

Exact event diagnostics:
- Left: **40 individual overwrite messages**
  are logged due to the first-40 diagnostic cap;
  final cumulative total is **48**. Every logged
  loss had `left_boundary_fence_deferrals=2`,
  with both `fence_L=1` and `fence_R=1`
  *at producer replacement*.
  Failed L-boundary ages median **6.735ms**
  and **23.389ms**, intermediate R probe
  age median ~31.8ms, producer replacement
  median **33.515ms**. At intermediate R,
  the right fence was still unsignaled in
  **26/40** events and both fences were
  ready in **14/40**.
- Right: all **19** discard messages are logged
  and each has exactly two failed L promotions.
  First failed L age median **15.159ms**,
  second **31.851ms**, replacement median
  **33.848ms**. All 19 had both fences ready
  *at replacement*. The intermediate R probe
  was absent in **18/19** because they were
  overwritten before it; in the one probed
  case R fence remained unsignaled at the R
  event (`intermediate_R_fence_L=1`,
  `intermediate_R_fence_R=0`).
- Thus R-pacing shifted the first two eligible
  L-promotions later by approximately **8.4ms**
  in publication-relative pair age, while
  overwrites still occurred around
  **33–35ms**. This gives GPU work more time
  before the first two allowed promotions and
  plausibly explains fewer unpresented pairs.
  This is a causal hypothesis consistent with
  the observed fence timing, not a direct GPU
  completion timestamp.

Important distinction: **`fence_deferrals`
count missed individual readiness checks, not
dropped video frames**. The diagnostic
`pair_unpresented_discards` counts producer
staging generations that never reached KMS.
They may not coincide with mpv's own
`Dropped` counter or directly measure A/V sync.
Confirmed physical L/R parity is **not** an
optical glasses phase measurement.

Recommendation after this test:
**retain selectable left and right modes**
without changing defaults or adding latency
buffers. Right currently has the stronger
A/B result, but the earlier 113-second
right-pacing test showed 108 missed vblanks
and severe staging losses, while other runs
showed zero misses; investigate sources of
this run-to-run variability. A further
10–15-minute R-pacing run with
`--drm-stereo-stats=no` is a useful
stability gate. Next development milestone
should be an explicit, low-overhead
association of **mpv vo_frame frame_id/PTS**
with the stereo `generation` and its
confirmed L-presentation time. That enables
validation of A/V scheduling and *actual*
movie-frame drops, instead of optimizing only
buffer retention. Avoid assuming the
R-pacing improvement alone establishes
correct PTS-to-photon latency.


### Thirty-second implementation: vo_frame ID + media PTS to confirmed first L scanout

This change is ready for compilation and a 15-minute
native-stereo quiet-mode trial on the user's Intel DRM
device. It adds **observability only**; the KMS
worker's L/R selection, native-fence checks,
pair promotion conditions and mpv 60Hz logical
frame scheduling are not changed.

**Three distinct time/identity domains:**
- `vo_frame.frame_id` is mpv's nonzero stable
  identity for the current `vo_frame` image
  (unchanged on repeat/redraw). It comes from
  `video/out/vo.c` queueing and is NOT
  `stereo_pair.generation`.
- `vo_frame.pts` is the mpv **realtime**
  scheduling target, and mpv sets it to **0**
  in display-synced rendering. It is NOT a
  movie PTS. The implementation intentionally
  takes `vo_frame.current->pts`, i.e.
  `mp_image.pts`, expressed as source movie
  seconds; negative sentinel `MP_NOPTS_VALUE`
  or nonfinite values count as unknown.
- The KMS callback's `msc` and `sec/usec`
  UST represent the **confirmed physical
  page flip**. UST is verified as
  `CLOCK_MONOTONIC` on this i915 system;
  it is NOT comparable directly with mpv
  `mp_time_ns` and is not an audio clock.

**Data flow implemented:**

```text
player/video.c -> vo_queue_frame() allocates vo_frame.frame_id
  -> video/out/vo_gpu.c: draw_frame()
     -> vo_drm_stereo_pair_set_frame_info(frame_id,
          frame->current->pts, valid)
  -> context_drm_egl.c: building[frame_id,pts]
  -> atomically publish complete building -> pending/active
  -> KMS RT worker: promote pending only at L,
     copy active.frame_id/pts into in-flight drm->stereo_submit_*
     ONLY after successful atomic/page-flip submission
  -> drm_common.c: drm_pflip_cb() confirms FIRST L in generation,
     records (generation, frame_id, movie_pts, physical_msc, physical_ust)
  -> drm_egl_uninit(): pthread_join(RT worker), then print final
     aggregates and bounded trace OUTSIDE realtime operation
```

The `Stereo unpresented pair discarded:` log
now also includes `frame_id`,
`movie_pts_s` and `pts_valid` of
superseded pending pairs. With this information
we can distinguish source movie pictures that
did receive their first confirmed L from
generations discarded before any KMS presentation.

At all confirmed FIRST L events with valid PTS
(approximately 24/s), the RT callback updates
**fixed-size scalars only**:
- `first_L_confirmed`, `valid_pts`,
  `missing_pts`;
- counts of 1, 2, 3, >=4 complete **L/R cycles**
  between first L callbacks of consecutive
  movie generations. At 23.976 fps / logical
  ~59.999Hz, the typical intended pattern is
  alternation between 2 and 3 cycles.
- `frame_id_gaps` between generations shown,
  which indicates skipped *queued VO IDs*
  (not necessarily decoded source pictures);
- `segments`, `discontinuities` if the
  PTS jumps backwards by >100ms, advances by
  >5s, or frame ID becomes nonmonotonic.
- bounded relative timing statistics:
  `relative_drift_ms = (confirmed_first_L_UST
  - anchor_L_UST) - (media_pts - anchor_media_pts)`
  with unit conversion to milliseconds.
  The anchor is reset after a detected timeline
  discontinuity.

A fixed `DRM_STEREO_PTS_TRACE_CAP=256`
ring stores snapshots of first 8 valid L
presentations, every 120th valid source
picture (~5s), and every new segment.
Approximately 188 samples for a 15-minute
23.976fps run will be printed as
`Stereo PTS sample: frame_id=... generation=...
movie_pts_s=... confirmed_L_msc=...
confirmed_L_ust_us=... cycles_from_prev=...
relative_drift_ms=...`.
This is **NOT a full per-frame trace**; in
between samples, exact aggregation continues
for every confirmed first L with valid PTS.
There are no extra per-frame MP_INFO messages
on the 120Hz RT thread.

After the RT thread has stopped, the log includes
`Stereo PTS final:` with first L count,
missing PTS, discontinuities, frame-ID gaps,
display-cycle cadence (1/2/3/4plus), monotonic
scanout span, media-PTS span, relative drift
min/avg/max/last, total trace records and the
`ust_monotonic` flag. It also retains the
existing `Stereo DRM final:` aggregate.

**Interpretation warning:** `relative_drift_ms`
is a RELATIVE video-media-time versus
physical-wall-time measurement; it is NOT
absolute audio-to-photon latency or guaranteed
audio/video synchronization. In particular
`--video-sync=display-resample` may deliberately
adjust the playback/audio rate to make 23.976
fit the physical output, so a long-run slope
can be expected. When interpreting 15-minute
results correlate the observed media/display
rates with mpv speed and audio status.

Recommended 15-minute stability launch:
```sh
git pull --ff-only
meson compile -C build
./build/mpv --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode=2560x1440@120 \
  --drm-vrr-enabled=no --drm-stereo-3d=yes \
  --drm-stereo-pacing-eye=right \
  --drm-stereo-stats=no \
  --video-sync=display-resample --swapchain-depth=3 \
  --hwdec=auto --log-file=/tmp/mpv-stereo-pts-15m.log \
  '/path/to/23.976fps-Half-SBS-movie.mkv'
```

Allow at least 15 minutes of normal playback
and **quit normally** to get both final summaries
and the bounded per-generation samples. Do not
use `--display-fps-override=120`.
Initial validation: confirmed `valid_pts` close
to `first_eye_L`, nonzero `trace_samples_total`,
samples contain plausible 0.0417s PTS increments,
and exactly one first-L callback per accepted
generation. **Do not attach shutters until
physical L/R parity and missed-vblank counters
are verified**. Compiling on the user's Arch
system is still pending.


### Thirty-third run: 15m33s R pacing with PTS/confirmed-L frame attribution

User-uploaded full `mpv-stereo-pts-15m.log`
(203 KiB, 1,959 lines), Oct 19, 2026,
mpv `v0.41.0-dev-g64c80a4b2` on Intel i915,
Mesa TGL GT2, same Half-SBS 23.976fps film,
2560x1440 physical 120Hz DP1, native GPU
fences, `--video-sync=display-resample`,
`--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`,
`--drm-stereo-stats=no`. RT worker
`SCHED_FIFO priority 10`. Explicit quit
after ~933.6 seconds; stats emitted after
stopping KMS worker and before DRM teardown.

**Confirmed KMS final totals:**

| Counter | Total |
|---|---:|
| physical KMS flips | **111,996** (~15m33s) |
| missed physical vblanks | **6** |
| parity_errors | **12** |
| wrong_eye / wrong_hold | **6 / 6** |
| pair_renders / pair_gen | **22,397 / 22,397** |
| pair_unpresented_discards | **1,393** (**6.2196%**) |
| first_eye_L / first_eye_R | **21,004 / 21,004** |
| first_eye_L_misses / R_misses | **0 / 0** |
| fence_checks / fence_promotions | 39,461 / 21,003 |
| fence_deferrals | 18,457 (not equivalent to drops) |
| fence_errors / commit_errors / pair_update_skips | **0 / 0 / 0** |

The identity `22,397 rendered − 1,393
superseded = 21,004 first L` holds exactly,
as does `first_eye_R = first_eye_L`.
The new PTS-to-L instrumentation is **valid
and effective**:

```text
Stereo PTS final:
first_L_confirmed=21004 valid_pts=21004 missing_pts=0
segments=1 discontinuities=0 frame_id_gaps=1393
cycles_1=1576 cycles_2=8315 cycles_3=7954 cycles_4plus=3158
scanout_span_s=933.358733 movie_pts_span_s=934.100000
relative_drift_last_ms=-741.267 relative_drift_avg_ms=-290.015
relative_drift_min_ms=-757.938 relative_drift_max_ms=161.159
trace_samples_total=183 ust_monotonic=1
```

All **21,004** first-L confirmed records have
valid movie PTS; precisely **1,393 skipped VO
frame IDs**, matching the producer's discarded
pending generations. This establishes that
staging discards correspond to **real absent
source/movie-frame presentations**, not merely
internal GBM churn. No seeking or reset detected.

Movie-frame pacing between consecutive first-L
presentations is not ideal 2/3-cycle pulldown:
**1 cycle: 1,576 (7.5%); 2 cycles: 8,315;
3 cycles: 7,954; >=4 cycles: 3,158 (15.0%)**.
Only **77.46%** of intervals are 2/3 cycles.
The 1-cycle and >=4-cycle intervals are relevant
to visible judder/stutter; a correct clock
alone does not guarantee smooth pacing.

The trace contains 183 bounded snapshots.
Raw `relative_drift_last_ms=-741.267` is
**NOT 741ms of A/V desynchronization**:
its formula subtracts *unadjusted source
movie-PTS time* from KMS physical elapsed
time. For a 23.976023976fps film on
59.998795Hz logical display, normal
`display-resample` speed adjustment toward
2.5 logical L/R cycles/frame is
`59.998795/(23.976023976*2.5)
= 1.000979897` (~+0.0980%).
Expected relative trend approximately
**-0.979 ms per second of source PTS**.
A linear fit of trace points after the
initial 20 seconds has slope **-0.941
ms/s**, intercept ~142ms and detrended
residual SD ~**9.2ms** (10th–90th
percentiles about -11.4 to +12.1ms).
This largely matches the expected speed
correction, with some phase jitter;
it does NOT directly prove synchronization
to audio hardware's clock or measure
absolute audio-to-photon latency.

**High-confidence instrumentation interference
clue from complete timestamp correlation:**
All **six** physical `DRM SBS gap` warnings
(at 145.352, 353.190, 386.024, 573.453,
706.797 and 793.132 seconds) appear exactly
**17–20ms after** an individual
`Stereo unpresented pair discarded:` log
(at 145.335, 353.171, 386.005, 573.433,
706.778, 793.115s). Five are at the
per-100 drop-report milestones (counts 200,
300, 600, 900, 1100); the first at logged
count 37. The corresponding callback's
`prev_callback_lag_ms` reached
**8.18–12.01ms** and
`submit_after_callback_ms` stayed
**0.008–0.021ms**. This strongly
implicates potentially blocking synchronous
`MP_INFO` logging from
`stereo_note_unpresented_pair()`, executed
under `stereo.mutex` on the producer thread.
Not yet causal proof: comparable stress,
logging disabled with all other settings
unchanged, is needed. The prior
`--drm-stereo-stats=no` suppressed only the
per-120-flip `DRM SBS test` report;
it still printed the first 40 discarded
pairs then one per 100, exactly 53 events
in this run.

**Follow-up code fix after the run:**
`stereo_note_unpresented_pair()` now
returns immediately in
`--drm-stereo-stats=no` after incrementing
the **exact cumulative count**. In
quiet mode it no longer performs fence-poll
diagnostics, formats or logs drop events
under the pair mutex; the final
`Stereo DRM final` and `Stereo PTS final`
still report all aggregate losses and
PTS/frame_id gaps after RT teardown.
`DRM SBS gap` warning messages are
retained. This is a controlled change
to telemetry overhead, **not** to how
GBM buffers are promoted or KMS flips.
Rebuild and repeat a long quiet R-phase
run to test whether the six missed
physical slots disappear. Important:
the severe **1,393 frame drops (6.22%)**
are a separate scheduling problem and
may persist even if KMS real-time
scheduling becomes perfectly stable.

Status: Source-only update, brace/anchor
checked, target compilation still pending.
Do not connect active USB shutters with
any observed missed-vblank episodes.


### Thirty-fourth run: quiet-RT logging regression test, 400s, zero physical misses

Complete upload `mpv-stereo-pts-15m(1).log`,
Oct 19 2026, **mpv `v0.41.0-dev-gaab50c009`**.
Despite filename, the movie was manually stopped by
`Run command: quit` at 399.862s; playback sampled
**~399.6s (6m40s), not 15m**.
Same Intel i915 TGL GT2/Mesa 26.2.4, physical
DP1 2560x1440@120Hz, 23.976fps Half-SBS
*Venom*, logical ~59.999Hz
`--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`,
`--drm-stereo-stats=no`,
`--video-sync=display-resample`.
Native GPU fences and SCHED_FIFO 10 enabled.

This is the first run after source change
`e43ac5c8`, which prevents synchronous
producer-side `Stereo unpresented pair discarded`
logging while holding the shared mutex
when stats=no. **No periodic DRM SBS test and
no individual discard INFO messages** were emitted.

```text
Stereo DRM final:
 flips=47949 missed_vblanks=0 parity_errors=0
 wrong_eye=0 wrong_hold=0
 pair_renders=9587 pair_gen=9587
 pair_unpresented_discards=267
 first_eye_L=9320 first_eye_R=9319
 first_eye_L_misses=0 first_eye_R_misses=0
 fence_checks=15744 fence_promotions=9319
 fence_deferrals=6424 fence_errors=0
 pair_update_skips=0 commit_errors=0

Stereo PTS final:
 first_L_confirmed=9320 valid_pts=9320 missing_pts=0
 segments=1 discontinuities=0 frame_id_gaps=267
 cycles_1=835 cycles_2=3705 cycles_3=3611
 cycles_4plus=1168
 scanout_span_s=399.577599 movie_pts_span_s=399.816000
 relative_drift_last_ms=-238.401
 relative_drift_avg_ms=-43.951
 relative_drift_min_ms=-254.432
 relative_drift_max_ms=157.697
 trace_samples_total=85 ust_monotonic=1
```

The crucial result: **zero missed physical
vblanks, wrong-eye flips or parity errors over
47,949 confirmed flips**, versus the old
15m33s run's **6** gaps / wrong eyes. In the
old run *all six* gap warnings were 17–20ms
after verbose producer-side discard logging.
The new quiet condition appears effective for
the first 400 seconds and **supports**, but does
not conclusively prove, RT mutex contention
caused the previous KMS misses: trial durations
differ. The teardown-only
`Failed to commit atomic request: Error 22`
still occurs after user quit, not in-play.

**Video frame loss remains significant:**
`267 / 9587 = 2.785%` of prepared pairs
were superseded before a confirmed first-L;
exactly **267 frame_id_gaps** confirm those
movie pictures never reached scanout.
All 9320 first-L records had valid media PTS.
A one-count `first_eye_L` vs `first_eye_R`
difference at shutdown is consistent with
termination after an L but before its matching
R, not proof of a playback eye mismatch.

Important matched-content comparison:
the prior **full** 933s run's final 6.22%
cannot fairly be compared directly to this
400s run's 2.79% because movie complexity
varies. Using the first ~395 seconds of each
run and the identical sampled VO frame IDs,
the old trace had approximately **322**
cumulative unseen frames by movie PTS=395s,
versus **254** for the new trace.
Sampled metrics at each 120th successful first L
are precise, but counts interpolated at time
boundaries are **estimates**, not final counters.
Thus the improvement on matched early content
is real but more modest than headline 6.22%
versus 2.79% would suggest.

Strong scene-time dependence:
- First ~5min: approximately **1.43%** old
  versus **1.23%** new cumulative drop rates.
- Movie PTS=300–360s: interval drop fraction
  approximately **9.58%** old versus
  **6.98%** new.
- 360–395s: approximately **9.69%** old
  versus **7.80%** new.
This spike is near the movie's chapter boundary
at PTS 295.128s, but chapter boundaries alone
do **not** prove a causal change in GPU load.
A 15-min follow-up would be required to confirm
physical KMS stability in the same high-loss
middle and late source sections.

First-L movie pacing remains variable:
835 one-cycle intervals (**8.96%**),
3705 two-cycle (**39.76%**),
3611 three-cycle (**38.75%**),
1168 four-plus (**12.53%**);
only **78.51%** are 2–3 complete L/R cycles.
No extra permanent queue was added.

The bounded first-L PTS/UST samples show a
**relative drift slope** of approximately
**-0.942ms per movie-second** when excluding
first 20sec startup, with ~**8.2ms**
standard deviation about its trend (74
samples). The nominal
`23.976 -> 59.998795Hz / 2.5`
display-resample speed adjustment predicts
~**-0.979ms/s**. Thus the sampled relative
timeline remains consistent with mpv's
intended speed matching; the final -238ms is
**not** an absolute audio/video offset.

Engineering note: checking the actual
`vo_is_ready_for_frame` implementation in
`video/out/vo.c` confirms that
`--video-timing-offset` only affects the
`next_pts>=0` timed path. During the active
`display-resample` mode,
`player/video.c` passes `-1`, meaning
this ordinary option cannot tune rendering
lead time in our selected mode.
Focus subsequent experiments on **explicit,
bounded scheduling ahead/phase** and GBM
buffer readiness with confirmed frame PTS,
while strictly avoiding unbounded buffering,
corruption of eye phase or frame-PTS accumulation.

**Recommendation:** keep the new quiet mode
(`--drm-stereo-stats=no`) and right pacing
for short staging studies, but do not yet
attach shutter glasses; the test ended too
early for a 15-minute safety proof, and
**2.8% omitted film frames** is still too
high for smooth movie playback. Next design
should target on-time readiness at allowed L
boundaries rather than simply FIFO-preserving
every source frame.


### Thirty-fifth diagnostic patch: aggregate loss reasons in quiet mode

Following the 400s zero-KMS-gap run, add cheap
producer-side counters for the next 5–7 minute
high-motion-stress observation, **without
reintroducing per-drop synchronous logging
or fence polls while holding stereo.mutex**.

The `Stereo DRM drops final:` line, printed
after joining the realtime KMS thread, now
reports:
- `newer_generation` / `gbm_pressure`:
  which cause discarded the one-slot
  `pending` (these sum to the previous
  unpresented count for currently known reasons);
- `deferrals_0`, `deferrals_1`,
  `deferrals_2`, `deferrals_3plus`:
  how many eligible L promotions already
  tried and failed GPU fence readiness
  before that pending generation was
  superseded (these sum to all discarded
  generations);
- `age_samples`, `age_min_ms`,
  `age_avg_ms`, `age_max_ms`:
  CPU publication-to-discard age for
  valid, nonnegative pair age samples.

This is scalar-only instrumentation; the
hot producer path checks an existing integer,
compares the known reason string, and reads a
monotonic timestamp. **It does not poll GPU
fences or print in the hot path**, and it
does not change pair selection, output,
queue depth or clock. Physical misses still
require the separate `Stereo DRM final`
confirmation. This code has not yet been
compiled or tested on the target i915 machine.

Suggested efficient stress test: same parameters,
`--start=295 --length=120` to concentrate
on the movie segment where both previous
tests saw a large jump in omitted pictures.
The seek/warm-up means it is *not* a perfectly
matched full-run benchmark, but it is useful
for isolating producer scheduling pressure in
2 minutes. Stop normally to flush the final
summary. Longer uninterrupted R-pacing testing
is still required before shutter-controller
development.


### Thirty-sixth run: 120s targeted seek stress, all 28 drops after 2 failed L promotions

User log `mpv-stereo-stress.log`, Oct 19, 2026;
**built commit `v0.41.0-dev-g21b39b5d9`**.
Full file 1,160 lines. `--start=295 --length=120`
restarts the same 23.976fps Half-SBS *Venom*
at chapter 2 (~04:55) and exits at EOF
for this requested length, around wall
120.4s. Other options unchanged:
`--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`,
`--drm-stereo-stats=no`,
`--video-sync=display-resample`,
`--swapchain-depth=3`, native GPU fences,
Intel i915 at DP1 2560x1440@120 Hz,
SCHED_FIFO priority 10. mpv reports
logical 59.998795Hz display sync.

```text
Stereo DRM final:
flips=14399 missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0
pair_renders=2876 pair_gen=2876
pair_unpresented_discards=28
first_eye_L=2848 first_eye_R=2848
first_eye_L_misses=0 first_eye_R_misses=0
fence_checks=4376 fence_promotions=2847
fence_deferrals=1528 fence_errors=0
pair_update_skips=0 commit_errors=0

Stereo DRM drops final:
newer_generation=28 gbm_pressure=0
deferrals_0=0 deferrals_1=0
deferrals_2=28 deferrals_3plus=0
age_samples=28 age_min_ms=33.010
age_avg_ms=34.471 age_max_ms=38.010

Stereo PTS final:
first_L_confirmed=2848 valid_pts=2848 missing_pts=0
segments=1 discontinuities=0 frame_id_gaps=29
cycles_1=281 cycles_2=1142 cycles_3=1092
cycles_4plus=332
scanout_span_s=119.936575
movie_pts_span_s=119.953000
relative_drift_last_ms=-16.425
relative_drift_avg_ms=54.185
relative_drift_min_ms=-16.764
relative_drift_max_ms=133.340
trace_samples_total=31 ust_monotonic=1
```

**Physical timing: 14,399 flips, 0 missed
vblanks, 0 eye/parity errors, 0 fence or
atomic commit errors.** An atomic
`EINVAL` appears only *after* the normal
end-of-file shutdown. The quiet mode
produces no periodic or per-pair hot-path
INFO records; final counters are printed
after joining the KMS thread.

**Unshown GBM picture generations:
28/2876 = 0.9736%.** All 28 losses are
`newer_generation`, none from
`gbm_pressure`. All 28 had exactly
**2** failed L-boundary GPU-ready
promotion attempts before their next
producer picture superseded `pending`.
Producer publication-to-discard age
33.01–38.01ms, mean 34.471ms. Therefore
the single-slot pending replacement policy
and the phase of readiness relative to
physical L opportunities remain the
concrete source of in-VO generation losses.
The experiment does *not* give individual
GPU-fence completion timestamps (the quiet
mode suppresses polls at discard).

**Nuance: 29 `frame_id_gaps` versus 28
unpresented generations.** On the initial
source-PTS records,
`frame_id=37 gen=4 pts=295.128` is
followed by `frame_id=39 gen=5
pts=295.212`. Thus `frame_id=38`
(~PTS 295.170) is absent from first-L
output **without any skipped rendered
generation** at that point (`gen`
increases by one). This one gap was
introduced upstream of the completed
stereo-pair publishing path, probably by
mpv's own VO/video scheduling on startup/
seek; it is NOT a KMS pair discard.
The other 28 frame-ID gaps match exactly
the 28 unseen paired GBM generations.
Do not use `frame_id_gaps` as an exact
synonym for `pair_unpresented_discards`
in all situations.

Frame interval distribution over 2,847
successive first-L transitions:
- 1 full L/R cycle: 281 = 9.87%;
- 2 cycles: 1142 = 40.11%;
- 3 cycles: 1092 = 38.36%;
- 4+ cycles: 332 = 11.66%.
Only 78.47% have the normal 2/3-cycle
cadence. The 21.5% of anomalous 1 or
>=4-cycle intervals indicate persistent
*frame pacing* irregularity, not just
omitted frames.

The 31 PTS samples show all 2,848 first L
generations are assigned valid source
PTS. The first few snapshots show startup
timing discontinuity (e.g. first to second
presentation 7 full cycles / 116.7ms).
A line fit for samples at movie
PTS >=300s yields approximate
**-0.948ms of relative drift per movie
second** and **8.58ms standard deviation
after detrending**, consistent with the
expected ~-0.979ms/s display-resample
speed adjustment. The final relative
drift -16.425ms is NOT absolute A/V error.

**Comparison caveat / new hypothesis:**
The full-file 400s run ending near movie
PTS 400s recorded 267/9587 (2.79%)
unseen stereo pairs, concentrated in the
portion after PTS ~295s. The targeted
**fresh-seek** run of the overlapping
295–415s source section records only
28/2876 (0.97%). These are *not*
controlled, equal-boundary trials: starting
at PTS 295 **resets** display-sync phase,
GPU warm-up and VO history, whereas the
continuous run reaches that material after
five minutes of playback. Therefore it
would be premature to conclude that
particular movie action scenes are
inherently too demanding for the GPU.
The large improvement on the same source
content suggests producer-vblank phase
and/or warm-up state is material.
The code change between runs was only
quiet-mode scalar loss instrumentation;
it did not alter eye selection or timing.

Recommendations:
1. Keep the proven RT-safe `stats=no`
   baseline; avoid putting logging back
   under `stereo.mutex`.
2. Measure lead time of native GPU
   fences relative to the *eligible next
   physical L boundary* and the nominal
   mpv output schedule. The objective
   must be correct PTS and 2/3-cycle
   cadence, not hiding all losses by
   forcing FIFO delays.
3. Run a controlled same-seek 295–415s
   A/B with `--drm-stereo-pacing-eye=left`
   vs `right` on the same build,
   before treating the right phase as
   universal.
4. Consider experimental depth/pipeline
   knobs only if their effects are
   measurable without changing A/V
   offset and 120Hz eye-parity integrity.

The exact GBM KMS implementation remains
a single `pending` slot, with L-only
promotion and nonblocking native fence
readiness checks. Resource budget requires
care before adding an older-pair queue;
a queue must not accumulate video A/V lag.


### Thirty-seventh run: same-build, same-seek LEFT vs RIGHT 120s A/B exposes frame-pacing tradeoff

User upload `mpv-stereo-stress-left.log` compared against
`mpv-stereo-stress.log` (right phase), Oct 19 2026.
**Both exact commit `v0.41.0-dev-g21b39b5d9`**
with the same `--start=295 --length=120`
same source file, VRR off, physical DP1
2560x1440@120, Intel i915/TGL GT2,
`--video-sync=display-resample`,
`--drm-stereo-3d=yes`, native GPU fences,
SCHED_FIFO 10, `--drm-stereo-stats=no`,
`--swapchain-depth=3`. The **only**
intended mode difference is
`--drm-stereo-pacing-eye=left|right`.

| Reported final counter | LEFT | RIGHT |
|---|---:|---:|
| physical KMS flips | 14,400 | 14,399 |
| missed_vblanks, wrong_eye, wrong_hold | **0, 0, 0** | **0, 0, 0** |
| parity_errors, commit_errors, fence_errors | **0, 0, 0** | **0, 0, 0** |
| pair_renders / pair_gen | 2,877 / 2,877 | 2,876 / 2,876 |
| first_eye_L / first_eye_R | 2,813 / 2,813 | 2,848 / 2,848 |
| pair_unpresented_discards | **64 (2.225%)** | **28 (0.974%)** |
| frame_id_gaps | 64 | 29 |
| fence_checks | 5,788 | 4,376 |
| fence_promotions | 2,812 | 2,847 |
| fence_deferrals | 2,975 | 1,528 |
| gbm_pressure discards | 0 | 0 |
| newer_generation discards | 64 | 28 |
| drop deferrals_0/1/2/3plus | 0 / 1 / 63 / 0 | 0 / 0 / 28 / 0 |
| discard age min / mean / max (ms) | 16.616 / 33.583 / 38.804 | 33.010 / 34.471 / 38.010 |
| valid first L source PTS | 2,813 | 2,848 |
| source PTS segments/discontinuities | 1 / 0 | 1 / 0 |
| PTS relative last drift | +0.375ms | -16.425ms |

**Critical tradeoff hidden by drop totals:**
The actual completed L/R cycle intervals
between confirmed FIRST L movie generations
were:

| physical full stereo cycles | LEFT count | RIGHT count |
|---|---:|---:|
| 1 cycle (~16.67ms) | **0** | **281 (9.87%)** |
| 2 cycles (~33.33ms) | 1,367 | 1,142 |
| 3 cycles (~50ms) | 1,380 | 1,092 |
| 4 or more cycles (>=66.67ms) | **65** | **332 (11.66%)** |
| 2+3-cycle fraction | **97.69%** | **78.47%** |

Thus LEFT preserved the desired 23.976fps
2/3-cycle cadence between shown frames MUCH
better, with **no 1-cycle intervals at all**,
despite omitting 64 entire source frames
rather than RIGHT's 28. RIGHT is 2.28x
better by the pending-drop count but exhibits
frequent 1-cycle catch-up and >=4-cycle
stalls, and may look visibly jerkier.

Using the 31 recorded timestamp/PTS samples
per run, fitting relative drift against source
movie PTS for the 23 samples with
`movie_pts_s>=300` gives:
- LEFT slope **-0.9901ms/source-second**,
  detrended residual SD **3.84ms**.
- RIGHT slope **-0.9482ms/source-second**,
  detrended residual SD **8.77ms**.
Both slopes are close to the predicted
~**-0.979ms/second** from display-resample
speed matching and do not independently
establish actual A/V latency; the lower
LEFT sample residual corroborates its
more regular frame timing.

The LEFT final loss histogram
`deferrals_1=1 deferrals_2=63`
(and RIGHT `deferrals_2=28`) confirms that
almost all missing generations had two
failed eligible L fence checks before the
newer film generation replaced pending.
No GBM-pressure discards and no in-play
commit/fence errors were seen.
A nonzero `fence_deferrals` counts
readiness checks, NOT source frames dropped.

Both complete logs ended normally via
`--length=120` and then showed the existing
post-playback DRM atomic EINVAL during
teardown. This is not counted as an
in-play KMS failure and remains a separate
cleanup issue.

**Engineering inference:** Avoid declaring
right pacing a clear winner from its
0.974% drop fraction alone. There are
two objectives: source-frame retention and
near-ideal 2/3 movie-picture hold cadence.
The correlated change in cadence vs wakeup
phase suggests the mpv logical video scheduler
and KMS picture-promotion timing are currently
misaligned under right pacing. Do not attempt
an unbounded FIFO to hide dropped generations:
that can turn frame omission into accumulating
audio/video delay.

**Next experiment should distinguish player
frame cadence requests from actual KMS cadence:**
carry `vo_frame.num_vsyncs` (requested logical
L/R cycles), `display_synced`, and the
movie PTS with the same `stereo_pair` metadata
already traced, then compare the planned
per-picture 2/3 cycles with first-L-to-first-L
MSC deltas. If the player requested 2/3 but
KMS produced 1/4, focus on the VO staging and
promotion/feedback phase rather than tweaking
display-resample. Prefer bounded aggregate
instrumentation and teardown snapshots, not
per-refresh synchronous logs under the KMS
mutex. Only then consider a selectable hybrid
producer-wakeup/presentation-feedback phase
experiment. Physical L/R parity must remain
independent and no frame queue should be
allowed to accumulate A/V lag.

No runtime code modified in this documentation
update; results are grounded in the user logs.


### Thirty-eighth change: compare mpv requested num_vsyncs with confirmed KMS first-L cadence

Implemented after the matched 120-second LEFT/RIGHT experiment at
`--start=295 --length=120`. There are not yet any runtime logs from
this instrumentation version.

**Important original-plan semantics:** `player/video.c` computes
`vo_frame.num_vsyncs` from `--video-sync=display-resample`, including
intentional A/V-driven repeats/drops. `video/out/vo.c` consumes this
value (decreasing its private `current_frame->num_vsyncs` after
preparing the copy passed to `draw_frame`). The first `draw_frame`
for a new `frame_id` receives the original requested count. Subsequent
repeats/redraws of the same `frame_id` are filtered out by the stereo
producer. Thus the new `stereo_pair.planned_vsyncs` is the **initial
requested logical L/R cycles**, NOT the remaining counter at a later
display callback. A boolean `display_synced` accompanies it.

Metadata path:
```text
vo_gpu draw_frame: (frame_id, current->pts,
                    original num_vsyncs, display_synced)
  -> EGL producer stereo_pair.building
  -> pending -> active
  -> successful KMS commit: drm.stereo_submit_*
  -> confirmed FIRST L event: sample + aggregate stats
  -> after KMS pthread_join(): final summary and trace
```

The key timing rule: **the planned `num_vsyncs` for frame N
describes how many logical display cycles it should be held
BEFORE frame N+1 appears**. Hence at the confirmed first L
of frame N+1, compare its physical MSC interval against the
**prior confirmed frame's** `num_vsyncs`, not N+1's own
request. Using N+1's count would systematically misattribute
2/3 cadence and misdiagnose phase mismatch.

A trustworthy plan-to-actual comparison is possible only
when all conditions hold:
- Both confirmed first-L records have valid media PTS, are
  in the same monotonic timeline segment, and have
  `frame_id(N+1) == frame_id(N)+1`.
- Both frames are display-synced and the *prior* initial
  `num_vsyncs` is nonnegative.
- The physical MSC delta is positive, even, and small
  enough to count full left/right cycles.
A skipped frame ID may represent an upstream mpv drop
or an EGL pending generation that did not reach the screen.
The desired hold durations of those absent frames are
not recoverable from presented-frame metadata, so such
intervals are **excluded** from the comparison matrix
and separately counted as skipped.

Per-presentation hot-path bookkeeping is fixed-size
integer/scalar updates only; **no per-frame logging,
no allocation, no GPU fence wait**, unchanged KMS
promotion timing. After the RT worker stops, mpv prints
`Stereo plan final:`:
- `planned_0/1/2/3/4plus` histogram over confirmed
  valid-PTS, display-synced first-L frames.
- `nondisplay` / `invalid_plan`: inappropriate
  scheduling metadata.
- `adjacent_compared`, `exact`, `mismatched`,
  `signed_error_cycles`, `abs_error_cycles`.
- `skipped_intervals` / `skipped_frame_ids`
  for comparisons unavailable across missing pictures.
- `invalid_msc` for bad physical cycle deltas.

Five `Stereo plan row:` lines form a **5x5 contingency table**
with rows = *previous picture's requested* 0,1,2,3,>=4
logical cycles; columns = *actual first-L-to-first-L*
0,1,2,3,>=4 cycles. Comparing the diagonal to off-diagonal
reveals whether KMS is respecting the player's requested
picture holds; the `exact` counter preserves exact numerical
matches even inside the collapsed >=4 category.

Existing `Stereo PTS sample:` teardown entries now contain:
`mpv_planned_vsyncs` (current picture),
`display_synced`,
`previous_planned_vsyncs` (the previous confirmed
picture, or -1 if not comparable),
`skipped_frame_ids` (relative to the prior confirmed
first-L, not relative to the previous sampled record),
and `plan_comparable` (0/1).
`cycles_from_prev` is still the observed first-L-to-first-L
physical MSC delta / 2.

Interpreting results:
- A largely 2/3 planned histogram but a high off-diagonal
  `Stereo plan row` mass under RIGHT means staging,
  promotion or presentation feedback deviated from the
  *requested* frame schedule.
- Large off-diagonal errors under BOTH LEFT and RIGHT
  mean the problem is not simply picking a producer
  wakeup edge.
- Large `skipped_intervals` with high `pair_unpresented_discards`
  point to late GPU readiness + single-slot pending supersession.
- If the requested distribution itself has many 1 or 4+
  values, investigate mpv A/V corrections and video
  scheduling before changing KMS promotion.
- A large fraction of exact adjacent holds does **not**
  mean no movie frames were dropped; inspect both counters.

Recommended matched test commands (run separately, same binary):
```sh
git pull --ff-only && meson compile -C build
for eye in left right; do
    ./build/mpv --vo=gpu --gpu-context=drm \
      --drm-connector=DP-1 --drm-mode=2560x1440@120 \
      --drm-vrr-enabled=no --drm-stereo-3d=yes \
      --drm-stereo-pacing-eye="$eye" --drm-stereo-stats=no \
      --video-sync=display-resample --swapchain-depth=3 --hwdec=auto \
      --start=295 --length=120 \
      --log-file="/tmp/mpv-stereo-plan-${eye}.log" \
      '/path/to/Half-SBS-film.mkv'
done
```
Do not run both simultaneously. Normal end-of-length shutdown
is required for final counters. The code was reviewed against
the existing mpv frame lifecycle and cross-file metadata
transfer, but **has not been compiled or run on the target Arch
machine yet**. A quick launch check of `Stereo plan final`
and `Stereo plan row` lines is worthwhile before full A/B.


### Async stereo diagnostic ring (experimental, no USB)

Branch: `feature/drm-stereo-async-logging` based on the current
`feature/drm-stereo-dual-scanout` implementation. Only diagnostics change;
the independent 120Hz KMS scanout, pre-rendered L/R pair management and GPU
fences are unchanged.

The periodic `DRM SBS test` reports, `DRM SBS gap` warnings and sampled
`Stereo unpresented pair discarded` messages are now immutable events
queued with `pthread_mutex_trylock` into a bounded 16-entry ring. Formatting,
mpv logging and TTY/file writes happen in a normal-priority consumer thread.
There are no blocking logger locks, allocations or writes on the KMS callback
path. A full/contended ring discards diagnostic events, **never frames**.
The final shutdown message reports the count of discarded log events.

`--drm-stereo-stats=no` continues to suppress periodic reports and sampled
producer drops; KMS gap warnings remain enabled, but asynchronous. Final
`Stereo DRM final`, `Stereo PTS final` and plan reports remain synchronous
after joining the KMS thread. Infrequent non-stereo log messages elsewhere in
mpv are not changed by this patch.

For A/B, replay the same 120-second Half-SBS segment with
`--drm-stereo-stats=yes` and `--drm-stereo-stats=no`, the same 120Hz mode,
the same pacing eye and GPU-fence settings. Compare physical
`missed_vblanks`, confirmed wrong-eye events, source frame-ID gaps and
aggregate final statistics. Do not mistake a dropped *diagnostic event*
for a dropped display refresh. This source patch requires a local build
and hardware validation.



### Experimental confirmed-MSC planned-hold gate (source-only)

Branch: `feature/drm-stereo-msc-presentation`, based on
`feature/drm-stereo-async-logging`.

`MPV_DRM_STEREO_PLAN_HOLD=1` enables a strictly opt-in lower-bound
hold gate. The current movie generation's ORIGINAL `num_vsyncs` describes
the number of complete L/R display cycles before the next image may
begin. The actual first confirmed LEFT MSC is recorded by the KMS worker
AFTER successful page-flip event dispatch, never from an atomic-submit
guess. The next generation can be promoted on L only when
`target_msc >= first_left_msc + 2 * clamp(planned_vsyncs, 1, 4)`.
Existing native-fence checks and GBM ownership rules still apply.
The default remains OFF for a scientifically comparable baseline.

CAUTION: A lower-bound gate is not an A/V-synchronized scheduler by itself:
late frames may extend display duration. The initial 120s experiment must
report `Stereo plan hold final` counters, movie frame-ID gaps, and
`Stereo PTS final` relative drift. If drift grows, disable the gate and
build media-clock deadline skipping before enabling it as a default.
CPU measurements of renderer / EGL swap / fence-export calls describe CPU
command-submission work, not actual GPU completion.



### Sampled GPU fence completion diagnostics

With `--drm-stereo-stats=yes`, one of every 16 newly rendered pairs
produces a `Stereo GPU probe:` diagnostic. While the completed pair is
still safely owned by the producer, the logger duplicates both native
sync_file FDs; the ordinary-priority logger, NOT the SCHED_FIFO KMS
thread, polls them (max 100ms), optionally obtains dma_fence timestamp_ns
via SYNC_IOC_FILE_INFO (Linux), then closes its duplicate FDs.
The probe reports CPU draw, EGL swap and fence export durations separately
for L/R and both *observed* and (if supported) kernel-recorded fence
completion age relative to pair publication. Note `mp_time_ns` is based
on raw monotonic time with a process offset; kernel timestamp_ns is
converted to that timebase. Negative kernel age means a fence was already
signaled before both eyes were published. -1 indicates unavailable.
The observed poll time is a completion **upper bound**, not a precise GPU
completion time.

The worker's normal-priority logging can be arbitrarily delayed without
holding the KMS scanout mutex or GBM buffers. Duplicate sync_file FDs
are always closed on completion, failure or timeout. Default quiet mode
records no probes. The existing async logger counts lost diagnostic
events separately from video frames and page flips.



### Optional early producer wake and pending preservation controls

The baseline `--drm-stereo-pacing-eye` continues to determine mpv's
logical presentation feedback. Experimental variable
`MPV_DRM_STEREO_RENDER_WAKE_EYE=left|right` independently chooses
which physical flip wakes the producer's `flip_page` wait. With
`--drm-stereo-pacing-eye=right`, setting render wake to `left`
offers the VO ~8.33ms additional lead time. This is **not** guaranteed
to help: renderer may see the previous feedback cycle. Evaluate with
matched tests and disable if audio/video drift or cadence worsens.
Default render wake matches the configured pacing eye.

`MPV_DRM_STEREO_KEEP_READY_PENDING=1` is a second, independent
experiment that retains the previously published pending generation if
both native GPU fences are signaled when a new generation arrives.
It discards the newer incoming generation rather than the older ready
one. This preserves display order and L/R safety, but does NOT guarantee
fewer missing source frames or lower A/V latency. The shutdown
`Stereo staging final` line distinguishes protected older pairs from
discarded newer frames. Default is 0 (prior newest-pair-wins logic).
Only one staging slot remains, and no GPU/KMS BO is freed while scanout
owns it.

Do not enable both experiments simultaneously for first A/B; isolate
render wake phase and candidate retention effects. Neither changes
physical KMS 120Hz or stereo eye parity.



### Correction of kernel GPU probe clock (after four 120-second runs)

The original GPU probe kernel values were systematically wrong:
`fence_kernel_L_age_ms` was around +1215ms and
`fence_kernel_R_age_ms` around +1223ms; the matching
`fence_observed_*` measurements were ~8ms/~16ms.
This discrepancy was caused by converting Linux dma_fence
CLOCK_MONOTONIC timestamps with mpv's raw-time startup
offset, despite mpv selecting CLOCK_MONOTONIC_RAW on Linux.

The corrected converter takes paired CLOCK_MONOTONIC and
`mp_time_ns()` samples, maps each reported kernel fence timestamp
to mpv's CPU timebase and **then** subtracts the pair-published
timestamp. The R-minus-L fence completion delta in the old
logs was unaffected by this constant clock-epoch mismatch,
but absolute kernel ages MUST be remeasured with the fixed code.
This fix changes diagnostics only.


### Four-way 120-second MSC presentation A/B (Oct 24 2026)

Source: matched user test logs `mpv-stereo-{baseline,hold,wake,keep}.log`,
all exact build `v0.41.0-dev-gdb237dc84` (before the kernel probe
timebase correction), native `--drm-stereo-3d=yes`,
`--drm-stereo-pacing-eye=right`, `--drm-stereo-stats=yes`,
`--video-sync=display-resample`, DP1 2560x1440@120, VRR off,
`--start=295 --length=120`. All ended normally. i915 card1,
SCHED_FIFO priority 10 confirmed.

| Metric | baseline | PLAN_HOLD=1 | RENDER_WAKE_EYE=left | KEEP_READY_PENDING=1 |
|---|---:|---:|---:|---:|
| physical flips | 14403 | 14405 | 14398 | 14399 |
| missed_vblanks / wrong_eye / commit_errors | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| pair_renders | 2877 | 2877 | 2877 | 2876 |
| confirmed first L | 2862 | 2846 | 2834 | 2866 |
| frame_id_gaps | 15 | 31 | 43 | **11** |
| unpresented pending discards | 15 | 31 | 43 | **0** |
| newly rendered incoming pairs discarded by keep | 0 | 0 | 0 | 10 |
| fence_deferrals | 1466 | 68 | 2953 | 310 |
| actual hold: 1 / 2 / 3 / >=4 cycles | 294 / 1146 / 1087 / 334 | **0 / 1394 / 1412 / 39** | **0 / 1389 / 1400 / 44** | 89 / 1348 / 1315 / 113 |
| requested-vs-actual exact / compared | 1517 / 2846 | **2794 / 2814** | 2734 / 2790 | 2416 / 2853 |
| exact rate (comparable adjacent pairs only) | 53.3% | **99.3%** | 98.0% | 84.7% |
| PTS relative drift last (ms) | +33.633 | +50.298 | -16.366 | -16.365 |

`hold` almost eliminates incorrect hold lengths, but costs 31 source
frames (vs 15 baseline). Its small `fence_deferrals=68` partly
reflects deferring the *fence check itself* until the due MSC, not
faster GPU completion: `early_blocks=2830`, `late_promotions=48`.
`wake` also markedly regularizes 2/3-cycle cadence without a hold
gate, but has the worst frame loss (43) and highest deferrals (2953).
`keep` reduces source frame gaps to 11, but it explicitly throws away
10 NEW incoming frames to preserve ready older pending pairs;
zero `unpresented_discards` is **not** equivalent to zero frame loss.
Its 84.7% exact rate is intermediate. `keep` also shows lower sampled
CPU render times than the other sequential runs, indicating possible
CPU/GPU frequency or scheduling confounding: repeat A/B before
attributing the whole improvement to the policy.

For each run, 179 asynchronous GPU probe events (every 16th pair)
were recorded; none reported async queue loss. Typical per-pair
CPU draw L/R means (ms): baseline 0.495/0.215; hold 0.524/0.217;
wake 0.519/0.210; keep 0.286/0.102. Observed fence completion after
pair publication averaged L about 8.0ms, R about 15.7ms. The reliable
WITHIN-PAIR kernel timestamp difference R minus L has median about
7.7ms in all modes; R is later despite quicker CPU draw and EGL swap.
The original absolute `fence_kernel_{L,R}_age_ms` fields (~+1.2s)
are INVALID because MONOTONIC kernel timestamps were compared directly
to mpv's MONOTONIC_RAW-derived offset. Corrected in next commit
`fd64ba3a`, but must be remeasured. A close-to-one-refresh R-minus-L
delay suggests investigating GPU command completion cadence / EGL
synchronization; the existing code already calls
`eglSwapInterval(display, 0)` in stereo mode.

All four normal shutdowns still yield a post-playback DRM atomic
EINVAL; no in-play KMS commit errors. Relative PTS drift is anchored
to each run's first first-L, **not** an absolute audio-to-photon delay.
The exact-rate matrix excludes intervals containing missing source IDs:
improved exactness can come with WORSE frame retention, as here.

Suggested next path: fix and repeat native-fence timestamp diagnostics,
investigate the ~7.7ms L-to-R GPU completion lag and the eligible
left-boundary phase, and design a bounded **media-clock deadline**
policy with an explicit late-drop decision. Do not enable PLAN_HOLD as
a default without managing A/V delay, or grow a FIFO without accounting
for per-eye GBM BO scarcity. Run more than one A/B trial to control
thermal/frequency variance. Prefer to retain the independent KMS
worker, which has achieved 0 physical misses across all four modes.


### Experiment: per-surface EGL swap interval (after fixed-clock 120s baseline)

The corrected native dma_fence clock test
(`mpv-stereo-clockfix.log`, build `v0.41.0-dev-gf2d859ffd`)
contained 179 samples. Mean kernel completion ages relative to producer
pair publication: L=7.939ms and R=15.739ms; medians L=7.455ms and
R=15.183ms. R-minus-L GPU completion delta mean 7.800ms,
median 7.739ms, max 13.932ms. The ordinary-priority poll-thread
detection lag over kernel signal time averaged 0.078ms (L) and
0.091ms (R): these are genuine driver-recorded fence completion
differences, not delayed userspace detection. Both CPU render and
eglSwapBuffers for R are faster than L.

In this baseline: 14,399 flips, zero physical misses, zero eye errors,
zero in-play commit failures, 22 first-L frame_id gaps and 22 pending
discards. 1,528 / 2,832 comparable planned-vs-actual hold intervals
matched (54.0%). 1,335 native fence deferrals, zero fence errors.
The usual post-shutdown DRM atomic EINVAL remains, separate from
in-play errors.

An initialization oversight is visible in the source:
`eglSwapInterval(display, 0)` is only called with LEFT draw surface
current. EGL 1.5 defines swap interval **per current draw surface**;
the default for each new EGLSurface is 1, and the second RIGHT surface
was created after the first one. An R completion ~one vblank later is
compatible with implicit right-surface EGL swap throttling, but could
also be driver-specific GPU queueing. No causal claim is made yet.

`MPV_DRM_STEREO_RIGHT_SWAP_INTERVAL_ZERO=1` now requests interval=0
on the RIGHT EGLSurface as soon as it becomes current for the first
R draw. The existing LEFT setting remains unchanged, and the KMS
worker and presentation pipeline are untouched. The producer logs
whether EGL accepted the call and the EGLConfig advertised minimum
and maximum swap interval. An EGL driver may silently clamp the
requested interval. Default is OFF to retain direct A/B.

Run baseline with this variable unset and an otherwise identical run
with only this variable set, ideally using the same 120-second movie
window. Compare `Stereo GPU probe` kernel L/R completion ages and
R-L delta, L-boundary `fence_deferrals`, `frame_id_gaps`, MSC holds,
and ensure `missed_vblanks`/parity/commit errors remain zero.
Do not simultaneously enable `PLAN_HOLD`, `RENDER_WAKE_EYE`, or
`KEEP_READY_PENDING`: their effects must remain independently
identifiable.



### Right swap-interval experiment result and event-focused GPU probes

Test: `mpv-stereo-right-swap0.log` (Oct 24, 2026),
build `v0.41.0-dev-g06b04c3cb`, 120s, only
`MPV_DRM_STEREO_RIGHT_SWAP_INTERVAL_ZERO=1`.

The EGL call returned success but EGLConfig reported
`EGL_MIN_SWAP_INTERVAL=1` and `EGL_MAX_SWAP_INTERVAL=1`.
By the EGL API contract, 0 is silently clamped to the allowed range;
the new call could not actually set interval 0. More importantly,
Mesa's DRM/GBM swap implementation
(`src/egl/drivers/dri2/platform_drm.c`, `dri2_drm_swap_buffers`)
does not queue a vblank wait for the surface; an EGLConfig minimum of
1 does **not** establish that vblank throttling caused the observed
GPU delta.

Mean native kernel fence signal age in clockfix baseline vs swap0:
L=7.939 vs 7.865ms, R=15.739 vs 15.523ms,
R-L=7.800 vs 7.658ms; identical R median 15.183ms.
Physical KMS misses/parity/commit errors remained zero. Movie
`frame_id_gaps` 22 vs 18, `fence_deferrals` 1335 vs 1491,
exact planned holds 1528/2832 vs 1511/2839. This is not evidence of
a GPU-latency improvement from the swap-interval change.
Recast the swap-interval knob as diagnostic only; when min>0 it now
prints an explicit clamping warning.

To pinpoint the real cause of candidate loss, each logged
`Stereo unpresented pair discarded` with native fences now ALSO
enqueues a nonblocking `Stereo GPU probe` (`kind=discard`) for
the exact pending generation being discarded. The async logger uses
duplicate sync_file FDs to retrieve historical signal timestamps,
including fences signaled long before the producer dropped the pair.
`last_failed_L_age_ms` captures that pair's KMS age at its last
unsuccessful L-boundary check; `gpu_R_after_failed_L_ms` computes the
right fence signal's offset from that *CPU check* in the same mpv
timebase. Positive means the right GPU fence signaled too late for
the last left decision; negative means it was already signaled, in
which case the other eye, an error, or a race must be examined.
This is an observable signal-vs-check comparison, NOT proof of a
possible earlier scanout: committing FB_ID also requires a future
L boundary and correct BO ownership.

The diagnostic stays OFF with `--drm-stereo-stats=no`.
No GPU waits, ioctls or allocations were added to SCHED_FIFO KMS.
The existing bounded asynchronous logger may drop diagnostic events
under load, so always check its loss count and avoid interpreting
missing events as missing movie frames.


### Lost-pair probe outcome: late R signal versus next producer overwrite

Baseline test `mpv-stereo-right-swap0(1).log`, despite the reused
filename, **did not enable** the right swap interval experiment: build
`v0.41.0-dev-gf102d6c65`, no `Stereo right EGLSurface` enable log;
`PLAN_HOLD`, `RENDER_WAKE_EYE` and `KEEP_READY_PENDING` disabled.
The test command did not explicitly unset
`MPV_DRM_STEREO_RIGHT_SWAP_INTERVAL_ZERO`, so future reproducible
baseline commands should unset it too.

Summary: 14,403 confirmed physical flips, 0 missed physical vblanks,
0 wrong eye/hold/parity, 0 in-play commit errors. 2,877 pair renders,
2,857 confirmed first L generations, 20 unpresented pending discards,
20 `frame_id_gaps`. 1,452 native fence deferrals, 0 fence errors.
Planned-vs-actual exact 1,476 / 2,836 comparable intervals (52.0%).
There were 179 periodic `kind=sample` GPU probes and, importantly,
exactly 20 `kind=discard` probes for all 20 lost generations.

Every one of these 20 losses shows:
- Two failed L-boundary fence checks, with most recent L check at pair
  age 28.332–32.725ms (median 31.747ms).
- The LEFT kernel fence had *already* signaled before that last L
  check (mean L signal age 20.228ms vs 31.650ms for the check).
- The RIGHT kernel fence signaled **AFTER** the last L check,
  by min 0.314ms / median 1.466ms / mean 1.770ms / max 5.226ms.
  R signal age relative to pair publication averaged 33.420ms.
- By the time the next producer publication discarded the old pending
  generation, the RIGHT fence had already signaled. The R-signal to
  discard lead was 0.062–1.290ms, median 0.164ms (mean 0.433ms).
  Every one of the 20 producer-side `fence_L` and `fence_R`
  snapshot values was 1 (both signaled).
- The producer replaced the pending pair 1.034–5.352ms after the
  last failed L check, median 1.785ms. The next permissible L boundary
  would occur approximately 16.667ms after the failed check (120Hz
  physical L/R) because the next physical refresh is R, not L.

This is **not** KMS or GL throughput failure. It is a deterministic
*late-to-L / ready-before-next-frame / overwritten-before-next-L*
queue policy failure. It cannot be fixed by promoting at the immediate
R refresh without mixing two source generations between eyes. Waiting
for R on the SCHED_FIFO KMS thread would also break the hard physical
refresh pacing. Since producer publication precedes the next eligible
L edge, preservation of the old ready pending generation requires
retaining that generation across new frame arrival.

A one-slot KEEP_READY_PENDING trial protected old ready pending frames
and reduced displayed source `frame_id_gaps` from 15 to 11 in an
earlier run, but explicitly discarded 10 newly arriving generations:
it exchanges which frame is lost, rather than delivering both. The
next design should define media-PTS anchored deadlines, bounded
generation queueing and GBM BO ownership/backpressure before aiming
to eliminate source-frame losses. With two pending generations plus
one currently scanned-out pair, typical GBM surface triple buffering
already consumes all three BOs per eye; a producer cannot blindly
render a fourth generation. Require bounded buffering, explicit
late-frame selection and measured impact on audio/video latency,
rather than silently introducing unbounded presentation delay.


### Experimental two-slot FIFO for late RIGHT-fence pairs

After the 20 loss samples in the `f102d6c65` test, the opt-in
`MPV_DRM_STEREO_TWO_SLOT_QUEUE=1` prevents a newer producer frame
from automatically replacing an older complete unpublished pair.

States: `active` (KMS scanout), `pending` (oldest unpublished),
`queued` (one additional unpublished pair). A completed new pair
enters pending if empty, otherwise queued if empty. On successful
LEFT atomic commit only, KMS selects pending as the next active
generation and advances queued to pending. The previous active BOs
remain held until the flip callback confirms the transition and the
VO producer drains retired buffers. No eye-specific promotion or
pair mixing is introduced.

Bounded admission: `vo_drm_stereo_pair_can_capture` returns false
when both pending slots are full, OR when either GBM eye surface
has no free buffers. Under this mode it **never evicts** pending to
make room; the upstream producer skips the new draw without waiting
for a 120Hz event or blocking the real-time KMS thread. All accepted
pairs retain their original sync_file fences and source PTS metadata.
Cleanup also releases the new queued BOs/FDs.

`Stereo two-slot queue final` adds `second_enqueues`,
`second_promotions`, `full_skips`, `gbm_skips`,
`unexpected_incoming`, `remaining_pending` and
`remaining_queued` counters. Cross-compare these with
`frame_id_gaps`, `pair_update_skips`, the PTS drift, planned/actual
hold matrix, and missed_vblanks / wrong_eye / KMS commit_errors.
`full_skips` and `gbm_skips` can count repeated attempts to draw
the same movie picture; they are NOT directly unique source-frame
drop counts.

This is the first bounded queue experiment, OFF by default.
It intentionally separates pair-retention correctness from a later
media-clock-anchored deadline scheduler. The FIFO could retain
frames longer than is useful for A/V sync. Do not enable it together
with `PLAN_HOLD`, `RENDER_WAKE_EYE` or
`KEEP_READY_PENDING` during the initial A/B. In the unlikely event
of enabling both KEEP_READY_PENDING and TWO_SLOT_QUEUE, two-slot FIFO
takes precedence.

Test with the previous clockfix 120s baseline (same mpv options,
source offset and DP 120Hz mode), while unsetting all other
experimental variables; on the new run set ONLY
`MPV_DRM_STEREO_TWO_SLOT_QUEUE=1`.

Safety: this code has been reviewed by inspection, not compiled on
the target i915 Arch system; treat it as experimental. Stop and
report if there are compile errors, GBM front-buffer failures,
KMS commit errors, visible A/V latency or playback freeze.


 
### First two-slot FIFO A/B: zero missing source frames in 120 seconds
 
Test log: `mpv-stereo-queue2.log`; build `v0.41.0-dev-gc51dfaddc`.
Settings: DP-1 2560x1440@120, 23.976fps Half-SBS, movie offset
295s, duration 120s, right-eye VO pacing, display-resample, 3-deep
mpv swapchain, stereo stats ON. Only
`MPV_DRM_STEREO_TWO_SLOT_QUEUE=1` enabled among queue/pacing experiments.
 
Result: 14,405 physical flips and zero missed_vblanks, parity_errors,
wrong_eye, wrong_hold or in-play atomic commit errors. The full
2,877/2,877 producer pairs were confirmed at first L, with
`frame_id_gaps=0` and `pair_unpresented_discards=0`.
The second slot received 17 pairs and all 17 were eventually promoted:
`second_enqueues=17 second_promotions=17`.
`full_skips=0 gbm_skips=0 unexpected_incoming=0
pair_update_skips=0`; queue empty at shutdown. `fence_deferrals=1553`
versus 1452 in the prior f102d6c single-pending baseline.
 
Same 120s single-pending baseline (`mpv-stereo-right-swap0(1).log`,
`f102d6c`) produced all 2,877 pairs but confirmed only 2,857
first-L source frames: 20 pending discards and 20 `frame_id_gaps`.
Physical KMS remained error-free in both cases. This strongly
supports overwriting the single pending pair as the cause of those
20 *particular* playback losses; it does not prove no losses under
other loads or scenes.
 
Native GPU fence signal ages are little changed: 179 periodic samples
per run, mean L=7.919ms, R=15.617ms, R-L=7.697ms for two-slot,
versus clockfix baseline L=7.939ms, R=15.739ms, R-L=7.800ms.
The FIFO changes the retention policy rather than rendering speed.
 
Pacing is **not fixed**: exact previous `num_vsyncs` hold interval
matches 1484/2875 = 51.62%. First-L inter-generation intervals:
1 cycle=328, 2=1096, 3=1138, 4+=314. Movie PTS span=119.953s,
KMS scanout span=119.986621s; anchor-relative end drift=+33.621ms,
average=+104.744ms, max=+185.835ms. This *anchor-relative* drift
does NOT measure absolute audio-to-photon latency. Existing PLAN_HOLD
alone achieved 99.3% hold agreement in an older 120s test, but it
discarded 31 movie frames with the single pending slot.
 
Next controlled A/B should enable both
`MPV_DRM_STEREO_TWO_SLOT_QUEUE=1` and
`MPV_DRM_STEREO_PLAN_HOLD=1`, leaving KEEP_READY_PENDING,
RENDER_WAKE_EYE and RIGHT_SWAP_INTERVAL_ZERO unset. This is the
first time intentionally combining two knobs after the isolated
baseline. Acceptance: no physical KMS errors, no high volume of
full/GBM update skips, no source ID gaps, >98% exact planned hold
intervals, no long-term accumulation of relative PTS drift. An
apparent success must still be validated by perceptual A/V timing
and longer-run tests. Media-clock deadlines remain a separate
future step, not implicitly solved by PLAN_HOLD.


### Two-slot FIFO + planned MSC hold: 120-second successful A/B

Test: `mpv-stereo-queue2-hold.log`, same binary
`v0.41.0-dev-gc51dfaddc` as isolated QUEUE2, with
`MPV_DRM_STEREO_TWO_SLOT_QUEUE=1` AND
`MPV_DRM_STEREO_PLAN_HOLD=1`. Other opt-in stereo controls
unset. Display DP-1 2560x1440@120 Hz, source 23.976 fps
half-SBS, start=295 length=120, display-resample,
right-eye VO pacing and native GPU sync_file fences.

**Integrity:** 14,401 physical flips, zero missed_vblanks,
parity_errors, wrong_eye, wrong_hold or in-play commit errors.
2,877 source stereo pairs rendered and 2,877 first-L and 2,877
first-R generations confirmed. `frame_id_gaps=0`,
`pair_unpresented_discards=0`, `pair_update_skips=0`;
0 GBM pressure and 0 dropped incoming sources.

**Pacing:** `Stereo plan final` adjacent_compared=2875,
exact=2872, mismatched=3, or **99.8957%** correct holds.
No skipped intervals or source IDs in this matrix.
Confirmed L durations: 1 cycle=1, 2 cycles=1436,
3 cycles=1435, 4+ cycles=4 (the exceptional holds are
concentrated around playback startup). The requested vs
actual histogram has 1436 exact 2-cycle holds, 1434 exact
3-cycle holds, one 2->3 and one 3->4+ (the remaining
mismatch is an exact count inside the 4+ bucket).
KMS plan hold: early_blocks=4280, due_promotions=2876,
late_promotions=4, anchor_missing=0. Native-fence checks=2883,
promotions=2876, deferrals=6, errors=0 (compared with 1,553
deferrals on QUEUE2 alone).

**Queue behavior:** second_enqueues=1410,
second_promotions=1410, full_skips=0, gbm_skips=0,
unexpected_incoming=0; pending and queued empty at shutdown.
This is real prebuffering under the 2/3-cycle gate,
not dropped producer draws. No progressive GBM backpressure
appeared during the 120-second sample.

**GPU work:** 179 periodic `kind=sample` probes;
kernel fence age L mean=7.770ms and R mean=15.430ms
after pair publication, R-L mean=7.659ms. The new
policy does not remove sequential GPU completion latency,
but it is no longer a visible source-generation loss.

**Timing interpretation:** raw media-PTS vs physical scanout
comparison has scanout span=119.986626s, movie PTS span=
119.953000s; anchored last delta=+33.626ms, average=+95.381ms,
max=+156.404ms. The 31-sample trace is ~154ms at 5 seconds
after startup and ~47ms at ~115 seconds; the falling
delta is largely the 23.976 fps media timeline compared
against an almost exact 24.000 fps display-resample
2/3-cycle cadence. Core logs explicitly confirm
`Video sync mode enabled` and
`swresample: 48056Hz ... -> 48000Hz`. Audio is
being rate-corrected by mpv; **do not interpret this
raw-PTS difference as measured A/V latency**, and do
not feed uncorrected source media PTS into a new
wallclock/MSC deadline gate. Absolute audio-to-photon
delay was not observed in these logs.

**Next:** before any media-PTS-aware gating, instrument or
reuse the mpv core's *effective video/audio speed* and
presentation clock anchor so a scheduler can compare
corrected presentation deadlines with actual DRM UST,
without fighting `--video-sync=display-resample`.
Maintain current QUEUE2+HOLD as a validated opt-in
reference. Perform a longer video run and pause/seek
stress with audio to check non-steady-state behavior
and perceived synchronization. Treat EOF-only atomic
EINVAL on CRTC restoration separately from in-play
`commit_errors=0`.


### 15-minute continuous hold + two-slot stability run (2026-10-24)

Input: `mpv-stereo-queue2-hold15.log`,
`v0.41.0-dev-gc51dfaddc`, native Intel i915/Mesa 26.2.5,
DP-1 2560x1440@120, 23.976 Half-SBS H.264, from media PTS 0
(`--length=900`, no `--start`), with
`--video-sync=display-resample`, right-eye VO pacing and
`MPV_DRM_STEREO_TWO_SLOT_QUEUE=1` +
`MPV_DRM_STEREO_PLAN_HOLD=1`.
This was continuous playback; no pause or seek was exercised.

**Preservation and physical correctness:** 21,578 render pairs and
21,578 confirmed FIRST-L source generations, 21,577 FIRST-R
generations at test cutoff (one last L without an R before stop),
`frame_id_gaps=0`, `pair_unpresented_discards=0`,
`pair_update_skips=0`, `missed_vblanks=0`, `parity_errors=0`,
`wrong_eye=0`, `wrong_hold=0`, `commit_errors=0`.
107,907 physical flips, all `max_gap=1` in the 899 1-second
physical scanout reports. The single final `Failed to commit
atomic request: Error number 22` is after the end-of-run
stats at 899.624s, during shutdown restoration; it is not
an in-play KMS flip error.

**Queue and scheduling:** all 10,781 second-slot enqueues became
second-slot promotions; `full_skips=0 gbm_skips=0
unexpected_incoming=0 remaining_pending=0 remaining_queued=0`.
Only SIX GPU fence deferrals over ~21.6k generations.
`PLAN_HOLD`: `early_blocks=32354 due_promotions=21577
late_promotions=4`.
Exact previous requested hold vs actual physical L-cycle interval
was 21,572/21,576 = **99.98146%**; 4 mismatches,
`abs_error_cycles=5`, `signed_error_cycles=1`;
no skipped IDs or invalid MSC. Actual duration histogram
for adjacent first-L generation changes:
`cycles_1=0, cycles_2=10787, cycles_3=10785, cycles_4plus=5`.
The source rate of 23.976 frames/sec is locked to
alternating 2/3 stereo cycles at a physical 120Hz display
(~24 frames/sec after display-resample).

**GPU signal latency stable:** 1,348 `kind=sample` GPU probes
(once per 16 pairs), no lost-pair probe events.
Mean L kernel fence age=8.057ms, R=15.857ms,
R-L=7.800ms; the medians are 7.572ms, 15.244ms,
and 7.734ms, respectively. Splitting into three 5-minute
blocks gives R-L mean 7.881 / 7.709 / 7.811 ms, indicating
no progressive GPU latency issue.

**Important diagnostic clock-domain and resampling insight:**
`Stereo PTS final` reports scanout_span=899.241387s,
movie_pts_span=899.941000s, last raw drift=-699.613ms,
min drift=-707.951ms and max=+174.693ms.
This compares *unadjusted media PTS* against physical UST.
It is NOT absolute A/V sync or an accumulating rendering lag.
After initial ~5sec startup transient, sparse PTS/UST samples
follow the precise fitted line
`raw_relative_drift_ms = 175.466 - 0.972512 * elapsed_ust_s`;
maximum absolute residual on these regular snapshots after 10sec
is only **0.024ms**. Independently, the hardware MSC-to-UST
measurements give actual refresh **119.996702Hz**; five refreshes
per frame yield 23.999340 delivered pictures/s.
Compared with source `24000/1001` frames/s, the expected
raw drift slope is about **-0.972ms/s**, matching the trace
slope almost exactly. mpv logged `Video sync mode enabled`
and `swresample 48056Hz -> 48000Hz`, confirming that
audio time stretching/resampling is active. DO NOT
interpret -700ms raw drift as -700ms A/V desync, nor
apply a naive source-media-PTS correction in KMS.
Actual audio-to-photon offset is not observable in these
records; measuring it would require an audio clock anchor,
timestamped samples or independent perceptual/physical test.

Consequence: QUEUE2 + PLAN_HOLD passed 15-minute continuous
stress without missing source frames, incorrect eye order,
physical vblank loss or GBM pressure, at 99.98% duration
agreement. Preserve this as the reference implementation
and do not replace mpv's existing display-resample clock
with raw media PTS. Next independent validation should
exercise pause/resume, seeks, speed changes, file ending,
and real audio/video synchronization before enabling the
two options by default. No evidence here warrants a
new PTS/MSC correction feedback loop.


### IPC pause/resume and seek stress: pass, with diagnostic caveats

Test artifacts: `mpv-stereo-ipc-stress-20261024-224807-330991.log`
and `.jsonl`, using `v0.41.0-dev-gc51dfaddc` built from the
two-slot + PLAN_HOLD experiment. Duration ~140.07 s wall clock,
starting media offset 295s. JSON IPC script ran 18 requested
actions: 5 pause/resume pairs and 8 exact seeks (relative/absolute,
including an exact seek *while paused*). Every action succeeded;
all 8 application seeks emitted `seek` and `playback-restart`;
application exited with `code=0`.

Pause correctness: media `time-pos` stayed exactly 306.681s
over the 6s pause, and after a paused seek to 348.723s the
`pause` property remained true until explicit resume.
Continuous 120Hz physical L/R scanning did not pause or lose
a vblank (KMS continues to repeat the active pair).
Relative seek from 388.722s to 348.723s while paused was
correct; later startup/resume audio-video
`avsync` snapshots can temporarily be ~-0.03 to -0.05s,
but final sample `avsync=-0.000123s` (= -0.123ms)
and `vo-delayed-frame-count=0`.

Final KMS: `flips=16747 missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 commit_errors=0`; 2887 pairs
rendered and 2887 confirmed new left and right generations.
`pair_unpresented_discards=0 pair_update_skips=0`.
FIFO `second_enqueues=1127 second_promotions=1127
full_skips=0 gbm_skips=0 unexpected_incoming=0` and
both queue slots were empty at exit; 26 native-fence
deferrals and 0 native-fence errors. After exit, the
known DRM atomic EINVAL during CRTC restoration is not
a running flip error.

**Important: final summary is not a steady-state cadence test.**
`Stereo PTS final` has 9 segments and 8 media PTS
discontinuities, matching the 8 commanded playback seeks.
`frame_id_gaps=1` and `skipped_frame_ids=1` come from
frame_id 37 -> 39 during initial startup (the first
new-generation trace), NOT a mid-test pending queue discard.
The source frame 38 was never an accepted published pair;
cause of this upstream startup omission is not uniquely
established by the existing telemetry. No loss in either
pending slot during seek operations.

`Stereo plan final`: 2850/2876 = **99.096%** exact
previous-plan intervals with 26 mismatches,
`signed_error_cycles=900 abs_error_cycles=910`. Comparing
these global counters to the 99.981% steady-state test is
invalid: KMS must keep flipping even while mpv is paused.
Fifteen seconds of explicit paused time fall within
otherwise continuous movie PTS segments
(6s first pause + 3s before paused seek + 3 * 2s burst
pauses); at 60 stereo cycles/s this accounts for ~900
surplus physical cycles. Startup/seek intervals also
contaminate the remaining hold comparisons.

`relative_drift_last_ms=6046.769` is similarly not
audio/video delay: the current PTS vs hardware UST statistic
does not reset its time anchor across pause/resume.
Three final 2-second pauses alone contribute about 6s
to its final segment, and the IPC `avsync` settled to
~0 by the end. Do not feed this unsegmented pause-included
drift into a new KMS PTS controller.

Conclusions: 2-slot retention, paired-eye scanout and
planned-hold gating survive every tested pause/seek/resume
transition without a KMS error, pending discard or GBM
pressure. The existing diagnostic counters for PTS drift
and aggregate cadence need *pause-aware segmenting* if
one wants trustworthy global summary percentages under
interactive playback. That observability fix should not
alter KMS timing or promote from old queued generations.
A further functional test could seek repeatedly *while
the 2-slot queue is intentionally occupied* (rather than
relying on incidental timing), and cover playback speed
and end-of-file state changes.


### Pause-aware PTS diagnostics and forced occupied-queue seek

Starting at commit `c07b262`, the confirmed-FIRST-LEFT diagnostic
starts a fresh PTS/MSC segment whenever the interval since the
previous confirmed FIRST-LEFT presentation exceeds **500ms**, even if
the source PTS is monotonic and frame IDs are adjacent. This
distinguishes *wall-clock time with no new movie frame* (pause,
possible stall or other idle) from genuine source-frame hold.
A qualifying gap does not increment the 2/3-cycle histogram or
the planned-hold comparison matrix and does not add to the current
segment's raw PTS-relative drift. `Stereo PTS final` now reports
`long_gap_resets` and `long_gap_ust_s`, alongside the old total
`segments` and `discontinuities`. The threshold is deliberately
conservative; the event is called a long idle gap, not proof of
an explicit user pause. Global scanout and raw-media PTS spans
remain global for compatibility. The KMS flip path, PTS plan and
BO ownership are unchanged by this telemetry-only change.

A second, independently opt-in mode, starting at `15d97c9`,
makes an otherwise timing-dependent seek reproducible. Set
`MPV_DRM_STEREO_QUEUE_TEST_HOLD_MS=750` (valid 100..2000)
with `TWO_SLOT_QUEUE=1` and `PLAN_HOLD=1`.
After the **first** new pair enters `queued` while another
remains in `pending`, a one-shot deadline postpones
future **promotions** of pending for 750ms.
The physical 120Hz L/R worker continues to flip `active`
without blocking or sleeping on GPU, GBM or IPC. On the
producer thread, a single diagnostic marker is printed:
`Stereo queue test occupied: pending_frame_id=... queued_frame_id=... hold_ms=750`.
The test harness watches for that marker and sends JSON IPC
`seek ... absolute+exact` immediately, while this special
promotion gate is active. At exit,
`Stereo queue test final: hold_ms=750 triggered=1 hold_blocks=...`
confirms arming and counts suppressed eligible L promotions.

**Important:** the hold injection intentionally simulates a
brief video backlog. It may cause `queue_full_skips`,
PTS discontinuities, additional held frames or visible A/V
stall, even on a correct driver. It is NOT a jitter, dropped-frame
or playback-quality benchmark. Run it only in a dedicated
reproducibility test, NEVER in production. Explicitly unset
`MPV_DRM_STEREO_QUEUE_TEST_HOLD_MS` for every regular A/B test.

The accompanying updated `mpv_stereo_ipc_stress.py` can be run
as `./mpv_stereo_ipc_stress.py --occupied-seek`.
It starts mpv with 750ms hold enabled, waits for the occupancy
marker, sends an absolute seek to media position 900s before doing
slow property reads, verifies that the requested position settles,
plays for 10 seconds, seeks back to the initial position and plays
another 10 seconds, then gracefully quits to capture final
stereo statistics. `--occupied-hold-ms` and
`--occupied-target` customize the stress case.
The original pause/resume/seeking suite remains the default
when `--occupied-seek` is omitted.

Look for outdated pre-seek pending/queued generations being
promoted after the seek boundary, queue pressure or unbounded
resumption delay; correlate the IPC `.jsonl` action timestamp
against the two-slot marker and `Stereo PTS sample` segment
boundaries. This targeted test is not yet validation that all
seek paths always flush stale pending BOs; if it reveals such a
race, fix producer seek/reset handling while retaining KMS
framebuffer lifetime safety.


### Forced occupied-queue seek exposed real stale movie frames (2026-10-24)

Repro logs: `mpv-stereo-ipc-stress-20261024-230948-334210.log`
and `.jsonl`, build `df0c700c`, `QUEUE2=1 PLAN_HOLD=1
QUEUE_TEST_HOLD_MS=750`. At mpv time 2.112s the producer logged
`Stereo queue test occupied: pending_frame_id=69
queued_frame_id=70 hold_ms=750`. JSON IPC issued
`seek 900 absolute+exact` at 2.118s, with mpv reporting
`playback restart complete @900.024000` at 2.149s.
Yet the confirmed-L PTS trace reports generation 35
frame_id=69 movie_pts=296.463 at MSC 6178296, followed by
generation 37 frame_id=99 movie_pts=900.733 at MSC 6178306.
Mapping physical MSC to the report at 1.511s for MSC 6178131
puts frame 69's *first physical L scanout* at ~2.886s:
approximately 0.768s after the seek command and
0.737s after playback restart. The intervening gen 36
is not in the bounded sample trace and should not be
asserted as a particular frame without further telemetry.

This is a **real seek correctness failure**: the old
unpublished pre-seek FIFO outlived the decoder seek and
was promoted at the end of the forced 750ms gate.
An mpv IPC seek being accepted or playback-restart
emitted does NOT mean stale GBM pending/queued BOs were
flushed. All L/R KMS counters were nevertheless error-free:
flips=2659, missed_vblanks=0, wrong_eye=0,
pair_unpresented_discards=0 and in-play commit_errors=0.
The intentionally full queue recorded 46 full_skips and
45 gate hold_blocks; queue drained afterwards.
The new long-gap PTS diagnostic found exactly one
0.800s reset and produced 495 exact / 500 comparable
plans: raw test-induced mismatch is not a steady-state
pacing regression. One source frame_id gap 37->39
was during startup, before the deliberate seek.

**Fix in commit `c3cf121`:** mpv's
`reset_playback_state -> reset_video_state ->
vo_seek_reset -> VOCTRL_RESET` sequence already
delivers a seek/reconfigure reset to the VO thread.
The stereo GPU VO previously called only
`gl_video_reset`, leaving stereo `pending` and
`queued` owned by the KMS worker.
The new `vo_drm_stereo_pair_reset(ctx)` detaches
`building`, `pending` and `queued` under the
same mutex protecting KMS selection, cancels
the test hold deadline (without rearming the one-shot),
and releases the unpublished sync_file FDs + GBM BOs
on the original VO/EGL thread *after unlocking*.
`active` and any in-flight or retired KMS buffers
remain untouched until their ordinary confirmed-flip
lifetime transition, so eye pairing is preserved.
VO resets the last-drawn-frame tracking as well.

The fix adds a producer-thread event:
`Stereo queue seek reset: pending_frame_id=...
queued_frame_id=... building_frame_id=...`
only when a nonempty unpublished state was flushed.
Shutdown diagnostics gain `Stereo queue seek reset final:
resets=... pending_flushes=... queued_flushes=...
building_flushes=...`.

**Immediate re-test:** `git pull --ff-only &&
meson compile -C build` then run
`mpv_stereo_ipc_stress.py --occupied-seek` with the
existing 750ms test gate. Verify this time there
is a producer-side queue-reset log reporting pending
69 / queued 70 shortly after the IPC seek,
with nonzero pending and queued flush counters
and no reappearance of a confirmed-L movie PTS
from the pre-seek segment after the flush.
The KMS must continue 120Hz alternating L/R
with zero missing vblanks. Previously the first
post-seek confirmed L was old frame 69; after
the fix the **next newly promoted** pair should
belong to the new 900s segment, while displaying
the last pre-seek active pair until then is
expected/necessary for continuous shutter cadence.

The fix is not yet built or tested on the target i915
machine. Repeat ordinary pause/seek and steady-state
test without `QUEUE_TEST_HOLD_MS` after targeted
revalidation. Observe that intentional seek flush
counters are not movie-frame loss due to poor GPU
performance.


### Forced occupied-queue seek re-test PASSED after VOCTRL_RESET flush

Test log: `mpv-stereo-ipc-stress-20261024-231757-335731.log`,
JSON IPC trace `.jsonl`, binary `v0.41.0-dev-g67b4f064d`
(with implementation commit `c3cf121`). Same i915/Mesa
DP-1@120Hz, `TWO_SLOT_QUEUE=1`, `PLAN_HOLD=1`,
and stress-only `QUEUE_TEST_HOLD_MS=750` as preceding
failed run. Test completed successfully; mpv exit code 0.

Timeline: producer recorded `Stereo queue test occupied`
with pending frame **77**, queued frame **78** at t=2.404s.
The test's IPC `seek 900 absolute+exact` arrived at
t=2.411s. On the VO thread, `VOCTRL_RESET` emitted
`Stereo queue seek reset: pending_frame_id=77
queued_frame_id=78 building_frame_id=0` at t=2.419s.
mpv completed playback restart for media position
**900.024s** at t=2.457s. The first logged first-L
confirmed PTS sample after the seek is generation **45**,
frame ID **90**, PTS **900.024s**, MSC **6292582**.
The old accepted unpublished frames 77/78 were
removed before they could be promoted; continued scanout
of the last pre-seek *active* image until a new L is ready
is expected and protects eye parity.

Final: `flips=2699 missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 commit_errors=0
pair_unpresented_discards=0 fence_errors=0
pair_update_skips=0`.
`Stereo queue seek reset final: resets=5
pending_flushes=2 queued_flushes=1 building_flushes=0`.
`Stereo two-slot queue final: second_enqueues=204
second_promotions=203 full_skips=0 gbm_skips=0
unexpected_incoming=0 remaining_pending=1
remaining_queued=0`. The 1 second-slot enqueue
not promoted is an intentional seek flush (not a GPU
discard); 1 unpublished pending pair at normal shutdown
is later released by uninit, not a KMS leak.
One-shot stress gate `triggered=1 hold_blocks=1`;
seek promptly disarmed its active hold deadline, rather
than incurring the previous run's 45 blocked L promotions.
Playback avsync from IPC after the 10s recovery at
media 910.118 was -1.273ms, and after the return
seek and another 10s at media 305.138 was -2.149ms
(mpvs internal status, not a physical AV measurement).

`Stereo plan final: adjacent_compared=522 exact=514
mismatched=8`; `frame_id_gaps=1` is the preexisting
startup 37->39 gap, not a post-seek queue eviction.
`segments=3 discontinuities=2 long_gap_resets=0`;
the explicit seek canceling the injected hold made
the interval too short for the 500ms idle-gap threshold.
At shutdown, as in previous tests, a CRTC restoration
atomic EINVAL occurs *after* the `commit_errors=0`
in-play final report; handle separately.

Conclusion: the reproducible occupied-FIFO seek regression
from the prior run is fixed by flushing unpublished
pairs on `VOCTRL_RESET`. Do NOT conflate its intentional
seek flush counts with GPU pressure or missed source
frames. This stress input now passes, but it has not
tested every possible seek timing (e.g. an already
submitted pending first-L commit concurrently with a
VO reset). The next validation is ordinary pause/seek
with the stress-only test hold variable unset, followed
by long-running end-of-file and reconfiguration checks.


### Post-seek-reset ordinary IPC stress with pause-aware PTS metrics: PASS

Artifacts: `mpv-stereo-ipc-stress-20261024-232235-336840.log`
and `.jsonl`, binary `v0.41.0-dev-g67b4f064d`;
run with `QUEUE2=1 PLAN_HOLD=1`, *without* the artificial
`QUEUE_TEST_HOLD_MS` injection. Script started the film
at 295s, ran 140s wall-clock including 18 IPC actions:
5 pause/resume pairs and 8 seeks. Every IPC action
succeeded, all 8 seeks settled; player exited normally
(`code=0`).

During the first 6s pause, `time-pos` remained fixed
at 306.681s; an exact relative -40s seek *while still paused*
changed time-pos 388.763->348.765s, preserving `pause=true`
until explicitly resumed. Final mpv internal
`avsync=-0.002434s` (~-2.43ms), `vo-delayed-frame-count=0`.
This does not measure absolute audio-to-photon sync.

**Physical correctness:** 16,751 KMS flips,
`missed_vblanks=0 parity_errors=0 wrong_eye=0
wrong_hold=0 commit_errors=0 fence_errors=0`.
2,890 rendered pairs, 2,885 first-L and first-R
confirmations. `pair_unpresented_discards=0
pair_update_skips=0`, no dropped incoming
generations. This is an interactive test; the difference
in counts is consistent with intentional queue flushes
and the final pending pair, not a claim that all
decoded frames were physically presented.

**Queue correctness:** 974 second-slot enqueues and
974 promotions, `full_skips=0 gbm_skips=0
unexpected_incoming=0`; final `remaining_pending=1`,
`remaining_queued=0` at the pre-release shutdown snapshot.
The seek/reset diagnostic confirms 11 VO resets,
four flushing an occupied `pending`, none needing a
`queued` flush in this non-forced timing run;
`building_flushes=0`. Unlike the preceding forced
case, the empty second slot at the instant of seek
is ordinary and expected.

**Pause-aware diagnostic fix validated:** 5 physical
idle gaps over 500ms, totaling **15.050s**,
became new PTS segments (`long_gap_resets=5`,
`long_gap_ust_s=15.050`); combined summary
`segments=14 discontinuities=13` reflects the
eight commanded seeks plus five long gaps.
`relative_drift_last_ms=+24.498`, instead of
the previous ordinary IPC test's ~+6,047ms
pause-contaminated endpoint. This is a raw
UST-versus-source-PTS *relative* statistic after
segmenting, not absolute A/V latency. Initial and
last media-PTS and UST spans in the summary still
span all segments and **must not** be directly
compared for time drift under seeks.

**Pacing:** `adjacent_compared=2869 exact=2846
mismatched=23` (**99.198%** exact), signed-error
+13 cycles and absolute-error 23 cycles.
The previous pre-pause-aware ordinary IPC test
showed 2850/2876 (~99.10%), 26 mismatches and
+900 signed-error cycles. Now 1/2/3/4+ actual
cycles histogram=0/1412/1433/26.
Of the sparse first-L samples, confirmed startup
frame IDs 36, 37, and 40 already account for three
mismatches; the remaining mismatches cannot be
uniquely located to resume/seek boundaries from
the bounded once-per-120-frame diagnostic history.
The one `frame_id_gaps=1` is again startup
frame 37->39, not a FIFO discard during seek.
The 99.2% aggregate describes a dynamic stress
test, not the separate 15-minute steady-state
99.981% pacing baseline.

All 8 seek-settled IPC events succeeded and the
last internal AV sync was -2.43ms. The only
`Failed to commit atomic request: Error number 22`
occurs after final KMS metrics during CRTC mode
restoration, not during playback. Nothing in this
test calls for new changes to the RT KMS scheduler
or the published two-slot queue implementation.

Next optional tests before making these flags
the default: exact seek while an in-flight FIRST-L
commit is pending; rate change and reverse trick
play; EOF/next-file reconfiguration; repeat
runs without the forced hold and with stats off.


### Playlist EOF -> next file -> idle -> loadfile lifecycle: PASS (2026-10-24)

Logs: `mpv-stereo-playlist-lifecycle-20261024-234112-340233.log`
and `.jsonl`, binary `v0.41.0-dev-g67b4f064d`,
with `TWO_SLOT_QUEUE=1 PLAN_HOLD=1` and no artificial
queue hold. The test used an ordinary copy of the same
23.976fps Half-SBS source at `/home/life/Веном2.mkv`
as the second playlist entry; this is intentionally a
video-output lifecycle test, not a different-codec test.
One mpv PID (340234) stayed alive across the complete run.

JSON IPC:
- Initial file warmed up from media 295s, then the next
  playlist entry was appended with `loadfile ... append`.
- First file was exact-seeked to 6732.768s (~8s before end).
  `end-file reason=eof` for playlist entry 1 at wall elapsed
  **13.430s**; `start-file` entry 2 at **13.445s**;
  `file-loaded` entry 2 at **13.473s**. The new path was
  `/home/life/Веном2.mkv`, `playlist-pos=1`, and normal
  playing progress 295.000->302.803s was verified over 8s.
- Second file was exact-seeked to 6732.768s and reached
  genuine `end-file reason=eof` entry 2 at **29.530s**.
  `core-idle=true`, `playlist-pos=-1`, `path=null`
  were verified at **29.715s**, without terminating mpv.
- `loadfile ... replace` at **31.717s** opened the
  original as playlist entry 3; `start-file` at 31.720s,
  `file-loaded` at 31.915s; playback progressed from
  media 295.000->302.677s within 8s and ended the test
  normally with mpv return code **0** at 40.095s.
  Final `avsync=-0.003961s`, `vo-delayed-frame-count=0`
  (mpv internal sync, NOT audio-to-photon measurement).

**VO lifecycle:** There were exactly TWO DRM-stereo context
initializations and TWO final summaries. mpv **reused the same
VO/DRM stereo context across the consecutive first and second
playlist files**, including a producer-side
`Stereo queue seek reset: pending_frame_id=348 queued_frame_id=0`
at the first EOF transition. It destroyed the first context
only upon the **second** EOF (final line at 29.567s), when
the player entered `idle`; it then created a second
context on the subsequent explicit `loadfile replace`
(at ~31.759s). So an automatic playlist transition
must not be misdescribed as destroying/rebuilding KMS.

**First context, spanning both files:**
`flips=3489 missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 commit_errors=0`;
`pair_renders=684 pair_gen=684 first_eye_L=681
first_eye_R=681 pair_unpresented_discards=0
pair_update_skips=0 fence_errors=0`.
Two-slot FIFO: `second_enqueues=181
second_promotions=181 full_skips=0 gbm_skips=0
unexpected_incoming=0 remaining_pending=0
remaining_queued=0`. Seek/reset telemetry:
`resets=8 pending_flushes=3 queued_flushes=0
building_flushes=0`. Planned holds:
`adjacent_compared=673 exact=665 mismatched=8`;
interactive startup/EOF/seek transitions are
included, so this is NOT steady-state pacing.
The first-L PTS trace explicitly crosses from
first file PTS 6740.651 to second file PTS 295.003
at MSC 6608691 -> 6608699, with a reset at the
source PTS discontinuity, not a stale-generation
promotion. `frame_id_gaps=1` is the known initial
37->39 startup gap.

**Second context, original file reloaded from idle:**
`flips=943 missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 commit_errors=0`;
`pair_renders=186 pair_gen=185 first_eye_L=185
first_eye_R=185 pair_unpresented_discards=0
pair_update_skips=0 fence_errors=0`.
Two-slot queue `second_enqueues=0 second_promotions=0
full_skips=0 gbm_skips=0 remaining_pending=1`
at the pre-cleanup snapshot, a normal in-flight
pending frame during quit; GBM release follows.
`adjacent_compared=183 exact=181 mismatched=2
frame_id_gaps=0`.

**Resource lifetime:** JSONL `/proc/<pid>/fd` and
`/proc/<pid>/status` samples during initial playback
reported `open_fds=63 drm_fds=6 sync_file_fds=6
dmabuf_fds=1 Threads=32 VmRSS=382416kB`.
In `idle` following second EOF, both immediately
and after 2 seconds, resources stabilized at
`open_fds=22 drm_fds=0 sync_file_fds=0
dmabuf_fds=0 Threads=17 VmRSS=348900kB`.
After 8 seconds of new playback, resources were
`open_fds=63 drm_fds=6 sync_file_fds=6
dmabuf_fds=1 Threads=32 VmRSS=396224kB`.
This establishes **no lingering DRM, sync_file
or DMA-BUF FDs in idle** and confirms teardown and
reinitialization. It does NOT prove lack of a slow
GBM/GL process-memory leak: comparing a single live
RSS from two phases (382 vs 396 MB) is insufficient.
Repeat e.g. 20 EOF/idle/loadfile cycles to quantify
any residual memory trend.

**Known separate outstanding problem:** Both DRM
context final reports show in-play `commit_errors=0`,
but afterwards `vo/gpu/drm` logs
`Failed to commit atomic request: Error number 22`
at **29.658s** and **40.030s**, during CRTC restoration
when closing a DRM context. This is *not* an in-play
stereo flip failure and does not stop subsequent VO
initialization, but the restore failure should be
separately investigated and must not be described
as fully successful restoration.

Verdict: automatic consecutive-file playback, last-file
EOF -> idle teardown and loadfile-replace reinit all
work in a single mpv process with physical L/R
integrity intact and no lingering special-purpose
FDs after idle. No changes to the RT KMS frame scheduler
are warranted from this log. Remaining independent
checks: explicit repeated idle/reload cycles for
leak detection; and the DRM atomic CRTC restoration
EINVAL on every teardown.


### CRTC teardown EINVAL investigation and repeated EOF/idle soak test

Investigation of the two verbose `Failed to commit atomic request:
Error number 22 occurred` lines at 29.658s/40.030s in the
2026-10-24 playlist lifecycle log traced them to
`restore_sdr()` in `video/out/drm_common.c`, **not**
to `vo_drm_release_crtc()` and **not** to the realtime
stereo KMS worker. During teardown `restore_sdr()`
unconditionally invoked `drmModeAtomicCommit()`,
even when `vo_drm_set_color(..., sRGB)` reported
that the display colorspace was already SDR.
The independent stereo worker uses its own atomic
requests, leaving the generic atomic request potentially
empty. An empty atomic request commonly returns
`EINVAL` on i915. The error logger also incorrectly
passed the libdrm return value `-1` to `mp_strerror`,
instead of logging `errno`.

**Fix in `49ea1e7`:** skip the SDR restore atomic
commit if `vo_drm_set_color()` returns false
(no actual colorspace change). When a colorspace
change IS requested, retain the commit and log
failures with `errno`, with a specific warning
`Failed to restore SDR colorspace: ...`. The
normal `vo_drm_release_crtc()` old-state modeset
restore remains separate and untouched. Neither
the stereo KMS worker nor the buffer queue's
L/R synchronization logic changed. Based on the
source and previous logs, this addresses the
spurious teardown EINVAL; a build and target i915
retest is required to confirm.

A companion standalone standard-library Python
harness, `mpv_stereo_eof_idle_cycles.py`, repeats
**25 cycles** by default using only the original
Half-SBS video file. Each cycle:
(1) JSON IPC `loadfile replace` from
`--idle=yes` in the same mpv PID;
(2) verify the source media time advances;
(3) exact-seek to the last four seconds;
(4) wait for genuine `end-file reason=eof`;
(5) check `core-idle=true`, `path=null`,
`playlist-pos=-1`; the playlist entry count may
remain >0 in idle;
(6) check dedicated `Stereo DRM final` and
`Stereo two-slot queue final` exist for this
cycle and contain zero missed vblank, wrong-eye,
parity, KMS commit, GPU fence and queue-pressure
failures; and
(7) record the number of all FDs, DRM device FDs,
sync_file FDs, DMA-BUF FDs, thread count and
`VmRSS` in `/proc/<pid>` while playing and idle.
The harness treats nonzero specialized DRM-related
FDs in idle or any new restore error as a test
failure. The `VmRSS` history is also exported as
CSV with 25 data rows: gradual RSS change is a
signal for investigation, not sufficient proof
of a GBM memory leak because caching is involved.

After `git pull --ff-only && meson compile -C build`
run `./mpv_stereo_eof_idle_cycles.py --cycles 25`
(or `--cycles 2` as a quick validation).
No second movie file is needed. Main artifacts:
`/tmp/mpv-stereo-eof-idle-cycles-*.log`,
`.jsonl`, and `.csv`.
Script Python syntax and fake-IPC two-cycle state
machine have been tested; only the user machine
can perform actual DRM/KMS verification.

If the same EINVAL remains on the new build,
inspect whether the output actually changes from
HDR to SDR during teardown; the new warning will
separate genuine SDR restore failures from failed
old-mode restoration. Compare the new restore
warning and DRM master/VT handling before altering
the old CRTC state restoration sequence.


### First 2-cycle and 25-cycle EOF/idle soak attempts aborted by harness race

Attached artifacts: `mpv-stereo-eof-idle-cycles-20261024-235606-342765.{log,jsonl,csv}`
(requested 2 cycles) and
`mpv-stereo-eof-idle-cycles-20261024-235616-342856.{log,jsonl,csv}`
(requested 25 cycles). Both used binary `v0.41.0-dev-gbbcb04a9c`
(including the first SDR commit guard) and aborted
**before the first playback**, at 0.305s / 0.302s.
Each `loadfile replace` returned `success`, mpv
emitted `start-file` and `file-loaded`, and
`time-pos` returned 295.0. The harness then queried
`core-idle` ONCE, receiving `true`, and raised
`cycle 1: still idle after file-loaded`.
In mpv this is normal: `file-loaded` may precede
the first decoder/VO playback transition, especially
with `--idle=yes` and an implicit initial `--start`
seek. Both scripted runs sent `quit` with return code
0, 0 EOFs, no physical KMS flips, zero completed
cycles, and CSV containing headers but **no data rows**.
These runs provide NO soak-test or memory-leak result.

Additional diagnostic discovered by these early shutdowns:
the original generic DRM SDR EINVAL diagnostic no longer
appears, but the new warning
`Cannot restore SDR colorspace: no atomic request`
does appear once per run. Source-order review explains
the warning: the stereo EGL context already invokes
`vo_drm_release_crtc` before generic `vo_drm_uninit`;
a very early shutdown can leave `atomic_ctx->request`
unallocated, and generic `restore_sdr` then tries
to change SDR color state even though the saved CRTC
has ALREADY been restored / DRM active flag is false.
This is a redundant teardown operation, not evidence
of a live KMS pageflip failure.

**Follow-up commit `6c30ded`:** `restore_sdr()`
now returns early if `!drm->active`, before touching
color state or submitting another atomic request.
The first guard against an unnecessary SDR commit and
correct use of `errno` remain. Non-stereo active-CRTC
HDR->SDR restoration remains in place. Target i915
runtime confirmation remains pending.

**Corrected standalone Python harness**
`mpv_stereo_eof_idle_cycles.py`: replace the immediate
`if ipc.get("core-idle") is True: raise RuntimeError`
after `file-loaded` with a bounded
`wait_property(..., "core-idle", lambda x: x is False,
timeout=30)`. Also treat
`Cannot restore SDR colorspace` as a restore error.
The revised script passed a two-cycle fake-IPC
integration test with a deliberately delayed
`core-idle=false` transition (500ms after
`file-loaded`); this is a mock validation only.

Repeat `git pull --ff-only && meson compile -C build`,
**replace the old local script with the corrected
download**, first run `--cycles 2`, then `--cycles 25`.
Only then interpret the 25-row CSV idle RSS and
FD trends, final KMS summaries, and whether
`restore_sdr` teardown warnings really disappeared.


### 2026-10-25: repeated EOF/idle cycles pass 2/2 and 25/25

Revised `mpv_stereo_eof_idle_cycles.py` (wait for
`core-idle=false` after `file-loaded`) and binary
`v0.41.0-dev-g9bb593e32` (both SDR teardown fixes).
Two independent runs:
`mpv-stereo-eof-idle-cycles-20261025-000530-345048`
(two cycles, mpv PID 345049) and
`mpv-stereo-eof-idle-cycles-20261025-000555-345167`
(25 cycles, mpv PID 345168), each with full
`.log`, `.jsonl` and `.csv` artifacts.

**Results:** 2/2 and 25/25 cycles passed, both
processes exited 0 after explicit IPC `quit`.
The 25-cycle run lasted about 184 seconds, with 25
distinct `end-file reason=eof` events, 25 separate
DRM-stereo context initializations and 25 final
DRM reports. No `Failed to commit atomic request`,
`Failed to restore previous mode` or
`Cannot restore SDR colorspace` in either run.
This confirms that skipping redundant SDR restoration
after `vo_drm_release_crtc` fixed the observed
teardown `EINVAL` and premature no-request warning
on target i915 under the tested SDR workload.

**Physical KMS across all 25 contexts:**
`flips=19012`, confirmed first-eye-L/R both
3617, `missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 commit_errors=0
fence_errors=0 pair_unpresented_discards=0
pair_update_skips=0`. There were 168 expected
`fence_deferrals` (nonblocking readiness checks,
not errors). Two-slot FIFO counters totaled
`second_enqueues=491 second_promotions=491`;
`full_skips=0 gbm_skips=0 unexpected_incoming=0`,
`remaining_pending=0 remaining_queued=0` at
every EOF. `VOCTRL_RESET` count 100 across cycles;
18 pending flushes and zero queued/building flushes
are intentional reset behavior under these seeks,
not pressure-induced discards. PTS traces record
one startup frame ID gap in the first context
(37->39, familiar startup artifact); none after.
`planned_exact=3403 / adjacent_compared=3516`
(96.79%) includes a major seek at every cycle
and all EOF boundaries, so it does NOT measure
steady-state smoothness; compare the earlier
15-minute steady-state 99.981% baseline instead.

**Resource quiescence:** after each of the 25 EOFs,
`/proc/<pid>/fd` yielded 22 total FDs,
0 DRM device FDs, 0 sync_file FDs,
0 DMA-BUF FDs and 17 threads. Both full and
two-cycle runs have nonempty CSV rows for each
completed cycle; there is no FD or thread
accumulation. Every cycle left mpv in
`core-idle=true` with no active source path.
The 25-cycle run remained in the same mpv PID,
and each reload re-created the VO / DRM context.

**Process RSS, 25-cycle run:** idle after first
EOF=346512 KiB (338.39 MiB); after last EOF=
360472 KiB (352.02 MiB), +13.63 MiB across
24 intervals. Runtime memory warms up in the
first ~10 cycles and then plateaus: for idle
cycles 11-25 values remain ~347.25-352.02 MiB
(range <4.8 MiB), with fitted linear slope
~+0.058 MiB/cycle. The playing RSS on cycles
11-25 stays ~386.75-389 MiB. This is not
evidence of ongoing linear GBM/FD growth,
but process RSS includes allocator and codec
caches, so cannot strictly disprove a slow leak.
If necessary, a longer soak with device-specific
GPU memory accounting (e.g. fdinfo/DRM memory)
could increase confidence.

**Decision:** this completes the targeted QA
for the optional `QUEUE2 + PLAN_HOLD` combination
on i915 / Mesa 26.2.5, including steady-state,
pause/resume/seek, forced occupied-FIFO seek,
playlist transition, EOF idle cleanup and repeat
VO reinitialization. The experimental knobs can
be considered for *stereo-mode-only* defaults,
but normal-mode regression and non-i915 driver
behavior have not been established by these
tests. Keep opt-out facilities during first
default-on rollout, and do not enable test
queue hold injection by default.


### 2026-10-25: native-stereo default policy verified by physical IPC run

Test artifacts: `mpv-stereo-ipc-stress-20261025-002437-348760.{log,jsonl}`;
binary `v0.41.0-dev-g52f962978` on i915 / Intel Iris Xe /
Mesa 26.2.5, DP-1 2560x1440@120Hz. The independent Python
policy test used `--policy default --scale 0.25`. Its JSONL
`launch` event confirms `env_overrides={}` and the
test removed both environment variables rather than setting
`=1`; the VO initialization reports exactly
`Stereo scheduling policy: plan_hold=1 two_slot_queue=1
(native_3d=1; ...)`, and the JSONL has
`policy_verified` with both enabled. This is
the FIRST on-hardware confirmation that the defaults
introduced at `474c53b` are active without env opt-ins.

The scaled-down 36.5s run executed 18 IPC actions
(5 pause/resume pairs and 8 exact seeks). All 8
seeks settled and reported `playback-restart`;
a 1.5s pause held `time-pos=297.672` constant,
and a seek while paused went from 363.321 to
323.323 with `pause=true` until explicit resume.
`test_complete` succeeded; mpv exited with code 0.
Final `time-pos=345.971`, `pause=false`,
`seeking=false`, internal
`avsync=-0.001734s`, `vo-delayed-frame-count=0`.

DRM summary: `flips=4308 missed_vblanks=0
parity_errors=0 wrong_eye=0 wrong_hold=0
pair_renders=731 pair_gen=730 first_eye_L=726
first_eye_R=726 pair_unpresented_discards=0
fence_checks=749 fence_promotions=725
fence_deferrals=23 fence_errors=0
pair_update_skips=0 commit_errors=0`.
Two-slot queue: `second_enqueues=179
second_promotions=179 full_skips=0 gbm_skips=0
unexpected_incoming=0 remaining_pending=1
remaining_queued=0` at the pre-release shutdown
snapshot. The one remaining pending BO is
normally released during teardown. Queue seek reset:
`resets=11 pending_flushes=4 queued_flushes=0
building_flushes=0`, consistent with seek flush
semantics. Test-only injected queue hold was
disabled (`hold_ms=0 triggered=0 hold_blocks=0`).
No warnings or errors, including no atomic
CRTC/SDR restoration EINVAL.

The MSC plan comparison was
`adjacent_compared=711 exact=698 mismatched=13`
(98.17% exact), with
`signed_error_cycles=33 abs_error_cycles=39`.
`long_gap_resets=4 long_gap_ust_s=3.433`,
`segments=13 discontinuities=12`
(8 seek discontinuities + 4 recognized idle gaps).
`frame_id_gaps=1` is the familiar startup
frame 37->39. The source-vs-UST
`relative_drift_last_ms=552.818` in the
final segment is **not** real AV desync:
the final scripted ~500ms pause burst can
fall near/below the conservative >500ms long-gap
cutoff and contaminate the wall-clock PTS metric.
The final IPC `avsync` was -1.734ms. Distinguish
this measurement artifact from physical KMS
wrong-eye or frame-timing errors. This test
deliberately includes startup, short pause bursts,
seeks and resume transitions; do not directly
compare its 98.17% plan rate with the 15-minute
steady-state 99.981% baseline.

**Disposition:** no change to the working realtime
KMS scheduler or two-slot FIFO is justified by
the default-on validation. Run `--policy no-plan`,
`--policy no-queue`, `--policy both-off` separately
to validate the opt-outs if desired. Also test
ordinary DRM 2D mode and non-i915 GPUs before
asserting broader cross-driver compatibility.
A possible future diagnostic-only refinement
would use explicit mpv pause-state transitions,
rather than a wall-clock >500ms heuristic,
to segment short pause bursts.


### 2026-10-25: default-on policy opt-outs verified on i915 (all three pass)

Tested binary `v0.41.0-dev-g52f962978`, Intel Iris Xe
i915/Mesa 26.2.5 at DP-1 2560x1440@120Hz. IPC test
`mpv_stereo_ipc_policy_test.py --scale 0.25` ran
three policy modes independently. All 3 runs contain
explicit `policy_verified` and `test_complete` events;
each completed 18 IPC actions, including 5 pause/resume
pairs and 8 exact seeks, with all seek settlements
observed and mpv exit code 0. Every run confirmed
`native_3d=1` and the *expected* actual runtime
policy, not just its environment setting:

| Policy | Requested env overrides | Reported hold/FIFO | KMS flips | Exact / compared MSC intervals | Unpresented pair discards |
| --- | --- | --- | ---: | ---: | ---: |
| default (previous run, 00:24:37) | none | 1/1 | 4308 | 698/711 = 98.17% | 0 |
| no-plan (00:30:45; PID 349947) | `PLAN_HOLD=0` | 0/1 | 4289 | 377/709 = 53.17% | 0 |
| no-queue (00:31:39; PID 350045) | `TWO_SLOT_QUEUE=0` | 1/0 | 4287 | 655/679 = 96.47% | 16 |
| both-off (00:32:25; PID 350138) | both `=0` | 0/0 | 4273 | 510/701 = 72.75% | 5 |

Full filenames: `mpv-stereo-ipc-stress-20261025-003045-349947.{log,jsonl}`,
`mpv-stereo-ipc-stress-20261025-003139-350045.{log,jsonl}`, and
`mpv-stereo-ipc-stress-20261025-003225-350138.{log,jsonl}`.

**Hardware integrity on every opt-out run:** `missed_vblanks=0
parity_errors=0 wrong_eye=0 wrong_hold=0 commit_errors=0
fence_errors=0 pair_update_skips=0 full_skips=0 gbm_skips=0`;
none of these tests has a CRTC/SDR restore failure.
Native fences and independent KMS flips remain functional
with either presentation policy disabled. `vo-delayed-frame-count=0`
on all runs; final mpv internal `avsync` values are
-9.894ms (no-plan), -9.549ms (no-queue), and +1.668ms
(both-off). These are not audio-to-photon measurements
and are influenced by the deliberate seek/pause workload.

**Pacing sensitivity:** with `PLAN_HOLD=0`, some film
frames advance after only ONE complete physical L/R
cycle despite the previous movie frame's 2/3-cycle
`planned_vsyncs`: 59 actual `cycles_1` on the
no-plan run, and 50 on both-off, versus 0 with the
gate enabled on the no-queue run. No-plan's
`signed_error_cycles=-10 abs_error_cycles=338`,
`mismatched=332` reflect the unlocked timing, not
lost KMS parity. Both-off had `abs_error_cycles=196`
and 191 mismatches. Each final plan comparison includes
startup, 5 pauses and 8 seeks, so the rates describe
dynamic stress behavior, not a controlled steady-state
throughput benchmark.

**FIFO admission sensitivity:** no-queue reported
`pair_unpresented_discards=16`,
`Stereo DRM drops final: newer_generation=16`,
and `frame_id_gaps=16` with
`skipped_frame_ids=16` (only 709 confirmed
first-L compared with 725 for no-plan). Under
both-off, 5 accepted but as-yet-unpublished frames
were superseded by newer generations;
`frame_id_gaps=6` includes startup plus these
5. With FIFO enabled in the no-plan run,
`second_enqueues=6 second_promotions=6
pair_unpresented_discards=0` (one startup ID gap).
Differences in 16 vs 5 drops across independently
timed IPC sessions must not be read as proof
that removing PLAN_HOLD improves retention:
queue residency and the race with L deadlines
also change. The FIFO specifically avoids
superseding accepted generations when both
slots have capacity.

**Final summary:** the explicit overrides behave as
designed, all three remain *physically eye-correct*, and
the default-on **PLAN_HOLD=1 + TWO_SLOT_QUEUE=1**
continues to offer the strongest measured combination
on this machine. No modification to scheduling logic
is warranted. Verify ordinary DRM 2D output separately;
it has not yet been tested as part of this default-on
rollout. Testing across other KMS drivers is outstanding.


### 2026-10-25: ordinary 2D DRM regression smoke test passes on i915

Target log: `mpv-drm-2d-regression.log`; build
`v0.41.0-dev-g52f962978`, Intel Iris Xe (i915 1.6.0),
Mesa 26.2.5, DP-1 2560x1440 @ 120Hz. Invoked:

```sh
env -u MPV_DRM_STEREO_PLAN_HOLD \
    -u MPV_DRM_STEREO_TWO_SLOT_QUEUE \
    ./build/mpv --no-config --vo=gpu --gpu-context=drm \
    --drm-connector=DP-1 --drm-mode=2560x1440@120 \
    --drm-stereo-3d=no --start=295 --length=30 \
    --log-file=/tmp/mpv-drm-2d-regression.log \
    "/home/life/Веном_Venom (2018) BDRip.Half-SBS.1080p.NNMClub.mkv"
```

**PASS for ordinary 2D path.** At startup the CLI
explicitly applied `drm-stereo-3d=no`; DRM atomic
support was detected, native display mode was
2560x1440@120, normal VO estimated
`119.997589 FPS for display sync` (physical
display refresh, unlike native stereo's logical
~60Hz). Standard `VO: [gpu] 1920x1080 yuv420p`
configured with software H.264 decoding and
PipeWire audio. `playback restart complete @
295.003000, audio=playing, video=playing`
at 0.597s, then `audio EOF reached` at
30.400s, `finished playback, success (reason 0)`
at 30.565s and `Exiting... (End of file)`
at 30.569s. The log continues through 30.616s
shader-cache flushing, but does not independently
establish the process exit code.

The log contains **no**
`Stereo scheduling policy`, `Stereo KMS first`,
`Stereo DRM final`, stereo staging/queue/fence
initialization or stereo worker reports, supporting
that normal DRM/EGL page-flip handling was used
and that the new queue/plan default-on policy
does not activate for `--drm-stereo-3d=no`.
The source MKV's `StereoMode=sbs2l` metadata
does not itself activate native output stereo.
There are also no `Failed to commit atomic request`,
`Failed to restore previous mode` or SDR restore
warnings in this run.

**Scope of evidence:** this is a 30-second 2D
functional regression smoke test, NOT 2D per-vblank
timing validation. Normal 2D does not expose the
custom stereo KMS missed-vblank/parity counters;
therefore zero *logged errors* does not prove zero
missed 2D vblanks or perfect real-time pacing.
No other driver or resolution is covered. With
this 2D smoke test, the proposed stereo-only
`QUEUE2 + PLAN_HOLD` default-on rollout has
passed the requested 2D compatibility checkpoint
on the tested i915/Mesa system. No new C code
change is required.
