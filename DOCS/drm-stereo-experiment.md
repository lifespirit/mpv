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
