# Experimental NVIDIA 3D Vision USB IR bridge for DRM stereo

This feature connects the native `--drm-stereo-3d=yes` KMS frame-sequential
output to the standalone `lifespirit/libnvstusb` NVIDIA 3D Vision emitter
interface. It is **disabled by default** and intentionally separate from the
validated `QUEUE2 + PLAN_HOLD` stereo scanout.

## What this does / does not prove

The DRM KMS thread receives the physical page-flip callback with the confirmed
eye (L/R), MSC and DRM UST timestamp. Inside that callback the bridge only
updates a single-entry atomic mailbox and writes to a nonblocking Linux
`eventfd`. A second, **normal-priority** worker sends `nvstusb_send_eye()`
through `libnvstusb.so.0` on the latest confirmed eye. The next KMS flip
does **not** wait for USB. If the emitter blocks, is disconnected or stops
responding, the scanout must continue unaffected. Old events are discarded
rather than sent as catch-up bursts.

**IMPORTANT:** USB packets are sent *after* confirmation of scanout, with
variable callback, thread scheduling, USB and IR delays. 120Hz alternating
bulk OUT packets and zero KMS parity errors DO NOT guarantee that the lenses
open at the correct optical phase. The NVIDIA 3D Vision firmware waveform is
reverse-engineered and its shutter timing has not yet been optically measured.
Do not wear the glasses for the initial synchronization experiments. Use
a camera or optical sensor until phase and repeatability are established;
stop if the behavior is unstable or uncomfortable.

A diagnostic USB summary is printed after each VO shutdown:

```text
Stereo USB final: dry_run=1 init_status=1 confirmed=... sent=...
    skipped_msc=... wake_errors=... usb_errors=...
    last_usb_error=... late_gt_2ms=... late_gt_period=...
    max_scanout_to_send_ms=... max_usb_transfer_ms=...
```

- `confirmed`: physically confirmed KMS page flips delivered to the bridge
- `sent`: USB submissions (or simulated commands in dry-run)
- `skipped_msc`: skipped physical MSC events in the sender mailbox during
  backlog; not the KMS missed-vblank counter
- `late_gt_2ms` and `late_gt_period`: sender woke more than 2ms or one
  physical refresh after scanout UST; only meaningful if the driver reports
  monotonic vblank UST
- `max_scanout_to_send_ms`: maximum **software begin-send delay** since UST
- `max_usb_transfer_ms`: maximum synchronous library call duration,
  NOT the IR emission or lens opening delay
- `init_status`: 1 ready; -1 dynamic library unavailable; -2 API mismatch;
  -3 USB device/firmware initialization failed; -4 emitter set-rate failed.
  In dry-run, 1 means the simulated sender is ready
- `usb_errors`: failures returned by the library; the bridge disables its
  sender on an error without stopping the video

## Step 1: software-only test (NO USB, recommended first)

From a free local TTY with DRM master access:

```sh
git pull --ff-only
meson compile -C build

MPV_DRM_STEREO_USB=1 MPV_DRM_STEREO_USB_DRY_RUN=1 \
  ./build/mpv --no-config --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode=2560x1440@120 \
  --drm-vrr-enabled=no --drm-stereo-3d=yes \
  --drm-stereo-pacing-eye=right --drm-stereo-stats=yes \
  --video-sync=display-resample --start=295 --length=30 \
  --log-file=/tmp/mpv-stereo-usb-dry.log \
  "/path/to/half-sbs-movie.mkv"
```

Expected: `Stereo USB: sender initialized ... dry_run=1`,
`Stereo USB final: ... init_status=1`, `confirmed` and `sent` of
similar magnitude, small `skipped_msc`, `wake_errors=0`,
`usb_errors=0`; KMS counters
`missed_vblanks=parity_errors=wrong_eye=wrong_hold=commit_errors=0`.
`confirmed` may differ from `sent` at startup and shutdown, since the
USB worker is asynchronous and the mailbox deliberately drops old events.
Do not interpret `late_gt_2ms` as a hardware error by itself.

**2D and the old opt-in SBS/PAIR path never start the USB bridge.**
Without `MPV_DRM_STEREO_USB=1`, there is no USB worker, eventfd,
dlopen or emitter initialization.

## Step 2: native USB device (only after dry-run passes)

Repository: https://github.com/lifespirit/libnvstusb

The independent `libnvstusb` test has previously succeeded with the NVIDIA
3D Vision USB IR emitter `0955:0007` when directly connected to a laptop USB
port; some hub connections produced `LIBUSB_ERROR_IO` and USB re-enumeration
failures. Follow `WAYLAND-USB-TEST.md` there for build, firmware and udev.
The firmware `nvstusb.fw` is proprietary and is not provided by mpv.

Build/install that library or point `MPV_DRM_STEREO_USB_LIBRARY` to its
locally built `src/.libs/libnvstusb.so`; ensure the firmware path is valid.
**Do not run `test-usb` and mpv simultaneously**, as they claim the same
device. Before using mpv, verify the device is functional independently:

```sh
cd /path/to/libnvstusb
./test-usb nvstusb.fw --rate=120 --seconds=15 --eye=alternate
```

Then from a free local TTY use the same mpv command as in Step 1, with:

```sh
MPV_DRM_STEREO_USB=1 \
MPV_DRM_STEREO_USB_LIBRARY=/path/to/libnvstusb/src/.libs/libnvstusb.so \
MPV_DRM_STEREO_USB_FIRMWARE=/path/to/libnvstusb/nvstusb.fw \
  ./build/mpv ...   # same native --drm-stereo-3d flags as Step 1
```

The firmware rate is set **once** to the physical display refresh (about
119.998 Hz), not logical ~60 Hz. The library's `nvstusb_send_eye` is used
directly without GLX swap/vblank APIs. The old `nvstusb_swap()` and its
internal stereo thread are **not used**. The option
`MPV_DRM_STEREO_USB_INVERT_EYES=1` swaps USB left and right selection
for diagnostics; it does not change KMS parity.

The library uses synchronous libusb writes with up to a 1-second timeout.
Even one such stall can make shutter phase unusable; therefore the emitter
is intentionally a second worker and will not compromise the video KMS
thread. If USB initialization fails, video continues without IR. Failures
are logged and `init_status` is reported on teardown.

## Step 3: optical verification (not yet performed)

Use an external photodiode / phototransistor and oscilloscope (preferred)
or a suitable high-speed recording with a known timestamp reference.
Record separately:

1. A bright full-screen marker changing *with confirmed left/right scanout*,
   ideally left-white/right-black and its inverse.
2. The actual light transmission of each lens through that marker, **not**
   just whether the USB emitter LED is lit.
3. Scanout-to-IR and IR-to-lens delays over many minutes, start/pause/resume,
   seek, and after a missed USB deadline.

Quantify constant phase offset, jitter, L/R inversion, drift, shutter
crosstalk, and whether firmware keeps independently timed shutters. A
software offset or timed-ahead command should only be added **after**
these measurements. Sending after a confirmed flip may intrinsically be too
late for a useful optical interval; if so the bridge needs a predicted-next-
MSC scheduler and separately validated latency compensation, never blocking
the actual page-flip callback.

The successful 15-minute KMS test and 25-cycle EOF/idle test predate this
opt-in USB bridge. They establish the video foundation, not glasses safety
or optical sync.


## Target i915 dry-run result: PASS (2026-10-25)

Artifacts: `mpv-stereo-usb-1792878911-354350.log` and
`mpv-stereo-usb-1792878911-354350.json`, produced using
`TOOLS/drm_stereo_usb_smoke.py` in **dry-run**.
Binary `v0.41.0-dev-g33be748f8`; i915 / Intel Iris Xe / Mesa 26.2.5,
DP-1 2560x1440@119.998Hz; Half-SBS 23.976fps film
starting at media time 295s. The process ran 15 seconds and
exited 0 after successful EOF of the deliberately short `--length=15`
playback. `MPV_DRM_STEREO_USB_DRY_RUN=1` meant no real
`libnvstusb` library was loaded and no IR emitter or glasses were
driven in this run.

The dynamically selected native stereo policy was unchanged:
`plan_hold=1 two_slot_queue=1 native_3d=1`.
Sender initialization: `dry_run=1 init_status=1`, physical
display refresh `119.998Hz`, `invert_eyes=0`.
The driver reported monotonic DRM UST and KMS callback events.
**KMS physical flips = 1812; USB mailbox confirmed = 1812;
dry-run sender consumed = 1812 (100%)**. There was no
mailbox-induced drop or scheduling stall:
`skipped_msc=0 wake_errors=0 usb_errors=0
late_gt_2ms=0 late_gt_period=0`.
The maximum clock-monotonic interval from physical
flip UST to **beginning** of simulated USB delivery was
**0.480ms**; simulated transfer itself took `0.000ms`.
Real USB bulk OUT time, firmware propagation, IR transmission
and optical shutter phase **remain completely unmeasured**.
The sub-0.5ms result only validates the software KMS-to-sender
handoff on this host with no USB contention.

Full stereo scanout KMS counters:
`flips=1812 missed_vblanks=0 parity_errors=0
wrong_eye=0 wrong_hold=0 commit_errors=0
fence_errors=0 pair_unpresented_discards=0
pair_update_skips=0 first_eye_L=358 first_eye_R=358`.
`pair_renders=359 pair_gen=358`; 7 ordinary
fence deferrals. `TWO_SLOT_QUEUE`: 158 second enqueues
and 158 promotions, no full/GBM skips or unexpected incoming
pairs; one pending pair at the pre-release shutdown
snapshot is expected. Plan measured
`adjacent_compared=355 exact=353 mismatched=2`
(99.44%, including startup), one familiar initial
frame-ID gap (37->39), no missed vblanks. The relative
PTS `drift_last_ms=135.081` is a segment-relative
video scanout/PTS metric influenced by startup delay,
not audio-to-photon sync nor an optical phase measurement.

**Disposition:** first target i915 software-only USB bridge
integration smoke test PASS. No scheduler or video code
change warranted by this log. Next controlled step:
standalone `libnvstusb` test of the directly attached
0955:0007 emitter; then one short mpv
`--live-usb` run with measured USB bulk OUT latency,
carefully inspect `sent/confirmed`, `skipped_msc`,
`late_gt_period`, and `max_usb_transfer_ms`
alongside unchanged KMS counters. Optical measurement
is required before relying on the shutter image.


## First live USB emitter smoke test: USB transfer PASS, startup gap observed (2026-10-25)

Artifacts: `mpv-stereo-usb-1792880121-358533.log` /
`mpv-stereo-usb-1792880121-358533.json`. Binary
`v0.41.0-dev-gc9812ac33`, Intel Iris Xe / i915 /
Mesa 26.2.5, DP-1 2560x1440@120Hz, `--length=10`,
real USB IR mode `dry_run=0`. mpv completed 10s playback
successfully and exited 0. Emitter `init_status=1`,
`usb_errors=0 wake_errors=0 last_usb_error=0`. No
USB transfer failed; measured maximum `nvstusb_send_eye`
duration was **0.350 ms**. One software wake began >2ms
after KMS UST, max **7.415ms**, but
`late_gt_period=0` (all measured software wakes within
one physical 8.33ms period).

**CRITICAL startup observation:** The KMS worker submitted
its initial left-eye flip at log t=0.503s, but the
USB emitter only became ready at t=1.371s: approximately
0.868s of early playback with no shutter command. The
final counters were `confirmed=1206 sent=1103`,
a difference of **103 early flips**. The old
`skipped_msc=0` did NOT count events overwritten in the
single-entry mailbox before the FIRST command, and thus
was misleading without inspecting logs. The time difference
predicts ~104 physical 120Hz flips, consistent with 103
missing submissions. There is no evidence of a sustained
103-command loss after the sender became active, although
the old telemetry cannot conclusively attribute every
single missing command without a distinct startup counter.

**Target KMS still clean:** `flips=1206
missed_vblanks=0 parity_errors=0 wrong_eye=0 wrong_hold=0
commit_errors=0 fence_errors=0
pair_unpresented_discards=0 first_eye_L=238 first_eye_R=238`;
two-slot `second_enqueues=108 second_promotions=108`,
no skips. Presentation plan `exact=233/235`
(99.15%, including startup).

**Follow-up telemetry change** (commits after this run):
add `startup_skipped` computed from the physical MSC of
the first confirmed KMS callback vs the first USB-sender
event; leave `skipped_msc` exclusively for skips
BETWEEN successive sender commands. Also print
`unaccounted = max(confirmed - sent - startup_skipped -
skipped_msc, 0)` to expose shutdown races, init failures
or otherwise unexplained differences. These fields are
diagnostic: they do not change when USB or KMS commands
are sent, and they do not fix initial readiness. The
Python smoke test displays warnings for nonzero startup,
steady-state or unaccounted drops. It also excludes
`PLAN_HOLD=0|1` and `TWO_SLOT_QUEUE=0|1` textual
override examples from the parsed policy, which previously
polluted the JSON policy object with spurious
uppercase `0` values.

**Disposition:** standalone real USB bulk OUT and 10s
nonblocking KMS coexistence PASS. This is NOT an
end-to-end synchronized glasses PASS: the firmware ready
time happens after video already starts, optical phase
was not measured, and the trigger remains *after*
confirmed scanout rather than predicting the next
vblank. Before using the glasses, measure actual
optical phase and decide whether the video start should
be explicitly gated on USB ready with a bounded timeout
or whether the sender should prime the firmware earlier.
Never silently block the realtime KMS worker on USB
initialization or synchronous OUT transfers.


## USB readiness gate (introduced after live test #2)

The second 10-second live test, files
`mpv-stereo-usb-1792880610-360260.{log,json}`, on build
`v0.41.0-dev-gae80234aa` confirmed the startup hypothesis
precisely: `confirmed=1214 sent=1109 startup_skipped=104
skipped_msc=0 unaccounted=1`; `init_status=1
usb_errors=0 wake_errors=0 late_gt_2ms=0
late_gt_period=0`. Maximum scanout-UST-to-start-of-send
was **0.361ms** and maximum real USB OUT was **0.314ms**.
Physical KMS remained clean: zero missed_vblanks,
parity_errors, wrong_eye, wrong_hold, commit_errors,
fence_errors or discarded pairs; plan
`exact=235/237` (99.16%). The USB firmware setup
completed at log t=1.359s whereas the first KMS
flip was submitted at t=0.491s. Thus **104 of 105**
unmatched commands arose at startup; the remaining 1
is plausibly an unsent final event at shutdown (not
proven individually). This was a USB transport test,
not optical phase validation.

**New strict startup behavior when
`MPV_DRM_STEREO_USB=1`:** in `drm_egl_init`, after
creating the USB sender thread and **before**
allowing mpv's frame producer to start the independent
KMS worker, the non-realtime VO initialization thread
waits for successful USB initialization and rate setup.
The default readiness timeout is 3000ms, adjustable
with `MPV_DRM_STEREO_USB_INIT_TIMEOUT_MS` (100..10000).
`init_status` is atomic; `1` explicitly means library
loaded, emitter opened, timing configured and no latched
USB error. Dry-run uses the same readiness gate but no
USB I/O. The log now prints:

```text
Stereo USB: sender initialized at 119.998Hz (dry_run=0 ...)
Stereo USB: startup gate READY, took 900.000ms (timeout=3000ms; physical KMS stereo can now begin)
Stereo KMS first atomic flip submitted: eye=L ...
```

(The 900ms is illustrative, not a target.) This
changes no frame scheduler, eye selection or USB
phase compensation. It merely moves the first
movie L/R flip to **after** the firmware becomes ready.
At init failure or readiness timeout, the requested
DRM-stereo VO initialization aborts instead of
displaying video with uninitialized shutter IR.
The cleanup stops and joins the USB sender before
releasing the VO; USB library calls still never block
the realtime KMS worker. Note that the *readiness wait*
is bounded, but on an extremely unresponsive
`libnvstusb` library cleanup `pthread_join`
may itself take longer. This is not a hard real-time
timeout guarantee for teardown.

The updated smoke script actively asserts a
`Stereo USB: startup gate READY` log and
`startup_skipped=0`, and accepts
`--usb-init-timeout-ms N` for controlling the VO
readiness deadline. A nonzero `unaccounted` from
the final asynchronous shutdown remains a warning,
not a test failure; KMS and optical counters are
checked separately. Any nonzero `startup_skipped`
on the new build means the gate failed and should
be investigated before adding phase compensation.

**Re-run two 10-second tests after recompilation:**
first with dry-run (no USB):
```sh
python3 TOOLS/drm_stereo_usb_smoke.py \
  "/path/to/half-sbs-movie.mkv" --length 10
```
then with the physical emitter attached (optical phase
still UNCALIBRATED):
```sh
python3 TOOLS/drm_stereo_usb_smoke.py \
  "/path/to/half-sbs-movie.mkv" --length 10 --live-usb \
  --libnvstusb "/path/to/libnvstusb/src/.libs/libnvstusb.so" \
  --firmware "/path/to/libnvstusb/nvstusb.fw"
```

**Required results:** 0 startup_skipped, 0 steady
skipped_msc, 0 USB/KMS errors, nonzero send count,
correct default `QUEUE2 + PLAN_HOLD`; first KMS
flip logged *after* `startup gate READY`.
There is still no evidence of actual optical eye
transmission timing, so do not equate a software
PASS with a usable glasses configuration.


## Target i915 readiness-gated live USB re-test: PASS (2026-10-25)

Artifacts: `mpv-stereo-usb-1792881036-361692.{log,json}`.
Test binary `v0.41.0-dev-g2064ac389`, real NVIDIA IR emitter,
Intel Iris Xe / i915 1.6.0 / Mesa 26.2.5,
DP-1 2560x1440@120Hz, film segment `--length=10`,
`--usb-init-timeout-ms=3000`; mpv exited 0
after successful 10-second segment.

**Startup gate verified by log ordering:** emitter initialisation
completed at log t=**1.352s**, the VO reported
`Stereo USB: startup gate READY, took 1114.579ms
(timeout=3000ms)` at t=**1.353s**, and the first
`Stereo KMS first atomic flip submitted: eye=L`
only appeared at t=**1.622s**. Startup waiting was
entirely before independent movie-frame KMS scanout;
USB operations did not stall the active 120Hz worker.

**Exact event accounting:** `confirmed=1212 sent=1212
startup_skipped=0 skipped_msc=0 unaccounted=0
wake_errors=0 usb_errors=0 last_usb_error=0`.
No software wake exceeded 2ms or a refresh interval
(`late_gt_2ms=0 late_gt_period=0`). Maximum
physical-KMS-UST-to-start-send delay **0.438ms**;
maximum measured synchronous USB-call duration
**0.287ms**. These are software timing upper maxima
for this run, NOT LCD-to-lens timings.

**KMS unchanged:** `flips=1212 missed_vblanks=0
parity_errors=0 wrong_eye=0 wrong_hold=0
commit_errors=0 fence_errors=0
pair_unpresented_discards=0`, first eye L/R=238/238;
6 expected GPU fence deferrals, no unpresented frame loss.
The queue staged 62 second enqueues and 62 promotions,
with no pressure skips. The presentation plan exact
rate was **233/235 = 99.15%** (2 adjacent mismatches,
abs_error_cycles=2, includes startup). No SDR/CRTC
teardown errors.

**Disposition:** readiness-gated optical emitter **USB
transport validation PASS** for this host and workload.
The formerly observed ~104 unsent startup commands are
now eliminated. Do not adjust the KMS scheduler or
introduce arbitrary phase offsets based only on this
test. `optical_phase_verified=false`: lens transmission,
IR waveform phase, crosstalk, LCD response and display
scanout position remain unknown. Next milestone is an
optical measurement with a controlled Half-SBS test
pattern and sensor/high-speed camera, with both eye
channels characterized independently.


## Optical phase measurement fixture (added after zero-loss transport PASS)

The standalone generator
[`TOOLS/drm_stereo_optical_fixture.py`](../TOOLS/drm_stereo_optical_fixture.py)
creates a constant-per-frame, deterministic Half-SBS H.264 Matroska
clip with four equal phases:

| Interval (default) | Left movie eye | Right movie eye | Measurement purpose |
| --- | --- | --- | --- |
| 0–5 s | WHITE | BLACK | Left/right lens opening versus white eye scanout |
| 5–10 s | BLACK | WHITE | Verify inverted content and swapped shutters |
| 10–15 s | WHITE | WHITE | Independently measure the shutter transmission waveform |
| 15–20 s | BLACK | BLACK | Background light/dark leakage baseline |

The test clip is generated from a 1920x1080 @ 24000/1001 Hz
source; each of its two 960x1080 Half-SBS halves is pure
white or black (no picture motion). The script writes a
`stereo_mode=left_right` stream tag, but native stereo
requires `--drm-stereo-3d=yes` explicitly regardless of tags.

**Generate from the checked-out mpv repository** (needs
FFmpeg with `libx264`):

```sh
python3 TOOLS/drm_stereo_optical_fixture.py /tmp/drm-optical-sbs.mkv
ffprobe -v error -select_streams v:0 \
  -show_entries stream=width,height,r_frame_rate:stream_tags=stereo_mode \
  -of default=noprint_wrappers=1 /tmp/drm-optical-sbs.mkv
```

The generator was tested on a 640x360 four-second
`--phase-seconds 1` sample by decoding frames at
0.5, 1.5, 2.5 and 3.5 seconds: sampled left/right center
RGB values were respectively
`(255,0)`, `(0,255)`, `(255,255)`, `(0,0)`
(in terms of per-half 8-bit brightness); the video
codec and `stereo_mode` metadata were verified with
`ffprobe`. It has **not yet been played on target KMS
with lenses/sensors**. Re-running the generator with
an existing output requires `--overwrite`.

**Run in native DRM stereo** after a successful hardware
startup-gated transport smoke test (free TTY and direct
DRM master access). Set paths to your actual local files:

```sh
MPV_DRM_STEREO_USB=1 \
MPV_DRM_STEREO_USB_LIBRARY=/path/to/libnvstusb/src/.libs/libnvstusb.so \
MPV_DRM_STEREO_USB_FIRMWARE=/path/to/libnvstusb/nvstusb.fw \
  ./build/mpv --no-config --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode=2560x1440@120 \
  --drm-vrr-enabled=no --drm-stereo-3d=yes \
  --video-sync=display-resample --drm-stereo-pacing-eye=right \
  --drm-stereo-stats=no --no-audio --no-sub \
  --log-file=/tmp/mpv-drm-optical-phase.log \
  /tmp/drm-optical-sbs.mkv
```

**Measurement procedure:** Preferred acquisition uses
three independently sampled photodiodes/phototransistors
and a multi-channel oscilloscope or logic analyzer with
appropriate analogue acquisition: a direct monitor
white/black reference, light transmission through the
left lens, and light transmission through the right lens.
Place sensing areas at essentially the same vertical
position on the screen. A rolling-scanned LCD panel adds
its own scanout location and pixel response delay.
Use the first 10s phases to establish which shutter is
open during the bright LEFT vs RIGHT physical scanout.
The BOTH-WHITE section isolates each shutter's open/closed
duty wave from the content brightness; the BOTH-BLACK
section establishes ambient/dark leakage. Quantify per
eye: time of rising/falling light edge versus reference,
duty cycle, L/R inversion, jitter, optical crosstalk and
stability. Keep the recorder and sample index consistent
across phase transitions. A high-speed camera can provide
a qualitative first look, but typical 240fps or
rolling-shutter recordings do **not** reliably establish
sub-millisecond optical phase; photodiodes/scope are
strongly preferred for calibration.

Interpretation caveats:

- KMS UST describes GPU/display page flip, not
  illumination at a fixed scan line; first control for
  panel scanout and pixel response.
- A 0.3ms USB-call duration is NOT equivalent to
  0.3ms IR or lens delay. The firmware's own
  shutter waveform may be independent of individual
  `nvstusb_send_eye` packet submission edges.
- If the shutters open systematically late, do not
  insert an arbitrary phase offset or blocking sleep
  in the KMS callback. Instead first quantify
  panel-to-lens delay and jitter, then design a
  nonblocking predicted-next-MSC USB scheduler.
- Optical checks are separate from the already-passing
  transport assertions; `optical_phase_verified=false`
  remains correct until physical lens transmission is
  measured and documented.


## Observed variable optical phase across independent runs (2026-10-25)

Source: `mpv-drm-optical-phase.log` from the
four-phase 20s `drm-optical-sbs.mkv` fixture on
`v0.41.0-dev-gecd430816` / i915 / Intel Iris Xe /
Mesa 26.2.5 at 2560x1440@120Hz. The user visually
reported three distinct *between-run* outcomes through
the shutter glasses: apparently well-synchronized;
mostly correct black/white with a small contaminated
patch at the TOP; and a misaligned horizontal boundary
covering roughly HALF the test square.

**This is a real functional optical defect despite fully
successful software/USB transport.** In the uploaded last
run, mpv reported `confirmed=2399 sent=2399
startup_skipped=0 skipped_msc=0 unaccounted=0
usb_errors=0 wake_errors=0 late_gt_2ms=0
late_gt_period=0`, with `max_usb_transfer_ms=1.130`.
The KMS worker recorded `flips=2399
missed_vblanks=0 parity_errors=0 wrong_eye=0
wrong_hold=0 commit_errors=0 fence_errors=0
pair_unpresented_discards=0`, and the plan was
`exact=477/477` (100%). The emitter was ready
at t=0.450s and first KMS atomic flip submitted
at t=0.568s. The initial readiness gate worked.

Thus the observations cannot be attributed to
a logged missed KMS vblank, KMS eye swap, or a dropped
USB bulk OUT. They are consistent with a relative
optical shutter phase/raster scanout mismatch;
at 120Hz, half the 8.33ms period is ~4.17ms, so
a roughly half-height horizontal boundary is a
plausible *order-of-magnitude* indicator. It is
NOT a calibrated measurement: LCD rolling scanout,
pixel transitions, emitter timing and the shutter
receiver's acquisition and latency are all unknown.
The exact position and variation between launches
cannot be determined from this log alone.

**Important instrumentation correction:** the old
`max_scanout_to_send_ms=0.000` only updated with
positive `CLOCK_MONOTONIC - DRM UST` values, and
the per-second DRM debug log showed many negative
callback lags. Zero was therefore not proof of zero
real delay; it can result from all signed samples
being negative. Subsequent code records
`signed_lag_samples`, `signed_lag_negative`, and
`min_signed_scanout_to_send_ms` /
`max_signed_scanout_to_send_ms` without changing
sender pacing. The old periodic KMS string
`(USB disabled)` was also misleading and has
been clarified as separate KMS-vs-USB accounting.

**One-variable optical A/B experiment:** an
*independent, optional* calibration-only branch of
`lifespirit/libnvstusb`,
`feature/usb-eye-timer-calibration`, exposes
`NVSTUSB_EYE_TIMER_US`. The lib originally
sends `r = NVSTUSB_T2_COUNT((1e6/rate)/1.8)`
with each shutter eye command; at 120Hz its
numeric Timer-2 argument is about 4630µs.
What this timer does to real IR/lens phase is
NOT proven. The new variable overrides this
firmware command parameter only, preserving
the original value when unset; the separate
firmware timing/profile parameters remain
unchanged. This does NOT change KMS parity,
MSC scheduling or the USB callback. No
optical test has yet been performed with
the override; it is purely a diagnostic.

Build locally, in the `libnvstusb` checkout:

```sh
git fetch origin feature/usb-eye-timer-calibration
git switch --track origin/feature/usb-eye-timer-calibration
make -j"$(nproc)"
```

Then from the mpv checkout, use the same optical
fixture command in this document, plus exactly one
variable on each independent run:

```sh
NVSTUSB_EYE_TIMER_US=3500 \
MPV_DRM_STEREO_USB=1 \
MPV_DRM_STEREO_USB_LIBRARY=/absolute/path/to/libnvstusb/src/.libs/libnvstusb.so \
MPV_DRM_STEREO_USB_FIRMWARE=/absolute/path/to/libnvstusb/nvstusb.fw \
./build/mpv --no-config --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode=2560x1440@120 \
  --drm-vrr-enabled=no --drm-stereo-3d=yes \
  --video-sync=display-resample --no-audio --no-sub \
  --log-file=/tmp/mpv-optical-eye-timer-3500.log \
  /tmp/drm-optical-sbs.mkv
```

Repeat independently at 3500, 4500 and 5500µs
and with the variable **unset** (historical baseline),
ideally at least three runs per setting. Keep
`MPV_DRM_STEREO_USB_INVERT_EYES` unchanged.
Record for each run: which of the first two
phases showed clean black/white, approximate height
of any horizontal contaminated stripe, whether
the stripe was stationary or moved during the
20s test, and whether the two lenses behaved
symmetrically. A camera/sensor observing *through*
each lens is preferred; do not wear the glasses
during phase-calibration experiments.

If changing the timer moves the contamination
boundary consistently within a run, investigate
the firmware timer's real phase effect and measure
with a photodiode before deciding on calibration.
If the boundary changes unpredictably *between*
runs with the same timer, first investigate emitter
receiver lock/acquisition and startup synchrony.
A constant offset alone will not correct random
phase lock or drift. No KMS scheduler modification
is justified by the current transport-perfect log.


## 3500us optical repeats: highly variable visible phase (2026-10-25)

Four **independent** launches with the same requested
`NVSTUSB_EYE_TIMER_US=3500`, same 20-second
`drm-optical-sbs.mkv` 4-phase fixture, same
DP-1 2560x1440@120Hz, Intel Iris Xe/i915 and
`v0.41.0-dev-g2237b8193-dirty` build:
`mpv-optical-timer-3500.log`,
`mpv-optical-timer-3500-2.log`,
`mpv-optical-timer-3500-3.log`, and
`mpv-optical-timer-3500-4.log`.

| Run | User-observed through glasses | Confirmed/sent | Plan exact | Max real USB call | Signed UST-to-send range |
| --- | --- | --- | --- | --- | --- |
| 1 | ~half vertical square contaminated | 2404/2404 | 477/477 | 0.374ms | -0.440..+0.421ms |
| 2 | tiny contamination band at top | 2403/2403 | 477/477 | 0.285ms | -0.441..+2.367ms (one >2ms wake) |
| 3 | ~one-fifth of square contaminated | 2399/2399 | 477/477 | 0.179ms | -0.459..-0.264ms |
| 4 | almost ideal, faint bottom leakage | 2399/2399 | 476/477 | 0.769ms | -0.458..-0.287ms |

All four: `startup_skipped=0 skipped_msc=0
unaccounted=0 wake_errors=0 usb_errors=0`,
KMS `missed_vblanks=parity_errors=wrong_eye=
wrong_hold=commit_errors=fence_errors=0`,
`pair_unpresented_discards=0`, no full/GBM
queue pressure. Startup gate READY preceded first
KMS commit by 80–115ms depending on the run.
No sustained reported USB transfer lag. The
single one-cycle plan discrepancy in run 4 does
not explain the between-run full-height phase
difference. Plan/USB software success does not
establish correct optical lens phase.

**Most informative inference:** with unchanged
requested timer, optical band location varies
substantially and even switches top vs bottom.
A single **fixed** eye timer offset cannot
reliably explain/repair the observed between-run
variation. Candidate unmeasured mechanisms:
IR firmware timer state/acquisition, USB-to-IR
phase relative to scanout, glasses receiver
locking, and LCD raster/pixel-response. The
sample is a manually observed four-run series,
not calibrated optical timing data.

**Caveat: actual library timer setting was not
verifiable from these mpv logs.** They contain
mpv's `override requested: 3500 us`, not
libnvstusb's stderr acknowledgement. For
reproducible A/B tests, a follow-up calibration
branch of `libnvstusb` now exports
`nvstusb_effective_eye_timer_us()` and the mpv
USB worker dynamically reads it. If the env
override is present and the getter is missing
or reports a different value, VO initialization
fails closed (status=-5). mpv now logs the
confirmed effective microseconds. The default
USB path continues to work with old libraries
when no timer override was requested.

Note 2394–2399 of the 2399–2404 signed
`CLOCK_MONOTONIC - DRM UST` samples on each
run were **negative**, typically about
-0.3..-0.46ms. Such a signed result must not
be interpreted literally as a command preceding
physical scanout; the semantics/offset of
DRM UST need external validation before using
it to fit optical lens phase.

**Recommended next controlled experiment:**
compile both updated branches, verify the log
`Stereo USB: verified firmware eye timer=3500 us`,
then run independent interleaved values 3500,
4500, 5500 and **unset** (native rate default)
several times each. Record whether the
contamination boundary is stationary or MOVES
within one uninterrupted run, its approximate
fraction and whether top or bottom. If the
same verified timer still yields markedly
different fixed phases across launches, focus
on initial acquisition/timer state and phase
locking rather than further guessing offsets.
Do not claim an optical PASS, change KMS
pacing, or apply arbitrary per-run compensation
without lens/screen photodiode timing.


## Causal test: does one extra full L/R movie hold improve optical phase?

After four `NVSTUSB_EYE_TIMER_US=3500` optical runs, only run
4 (subjectively closest to correct 3D) exhibited a one-cycle
**movie-presentation** mismatch:
`Stereo plan row: requested=2 actual_3=1` and
`late_promotions=1`. The other three runs were
`exact=477/477`; run 4 was `exact=476/477`.
All four had `missed_vblanks=0`, `parity_errors=0`,
`wrong_eye=0`, `skipped_msc=0`, and
`confirmed=sent`. Run 4 had **no lost physical
left/right flip and no lost USB shutter command**.
Instead, a movie frame remained displayed for
three complete L/R cycles instead of two
(an extra 2 physical refreshes, ~16.67ms at
120Hz). This is a *duration* error for one movie
generation, not a 120Hz eye-parity error or a
USB packet drop. In the four-run sample, the
optical outcome and mismatch are correlated,
but a direct causal mechanism has not been shown.

To separate accidental startup/optical phase variation
from an actual response to a repeated movie pair,
the optional **`MPV_DRM_STEREO_TEST_EXTRA_HOLD_GEN`**
environment variable intentionally postpones exactly
one *eligible* generation promotion by a full L/R
cycle. It is accepted only for native stereo with
`PLAN_HOLD=1`; allowed generation 2..100000.
If unset, no scheduling behavior changes.
The 120Hz KMS worker still flips L,R,L,R and
the USB bridge still emits one command on every
confirmed physical flip. A test therefore
reproduces one `requested=2,actual=3`-style
presentation error **without** suppressing a
physical KMS flip or resetting the USB firmware.

Build the updated mpv repository and use a
verified libnvstusb calibration .so (see preceding
section). On a known, directly connected IR
emitter, keep the same 3500us requested timer,
same calibration clip, same display mode and
same eye inversion for every launch.
Use 4 paired runs: baseline, injection, baseline,
injection, optionally test a later generation
120 (~5 seconds). For example:

```sh
# Use the same command as the optical fixture run.
NVSTUSB_EYE_TIMER_US=3500 \
MPV_DRM_STEREO_TEST_EXTRA_HOLD_GEN=10 \
MPV_DRM_STEREO_USB=1 \
MPV_DRM_STEREO_USB_LIBRARY=/absolute/path/to/libnvstusb/src/.libs/libnvstusb.so \
MPV_DRM_STEREO_USB_FIRMWARE=/absolute/path/to/libnvstusb/nvstusb.fw \
  ./build/mpv --no-config --vo=gpu --gpu-context=drm \
    --drm-connector=DP-1 --drm-mode=2560x1440@120 \
    --drm-stereo-3d=yes --drm-vrr-enabled=no \
    --video-sync=display-resample --no-audio --no-sub \
    --log-file=/tmp/mpv-extra-lr-hold-gen10.log \
    /tmp/drm-optical-sbs.mkv
```

Expected summary for injection, if the selected
pending pair was ready at the due boundary:

```text
Stereo DIAGNOSTIC extra hold final: target_generation=10 injected=1 ...
Stereo plan ... mismatched>=1 ...
Stereo plan row: requested=2 ... actual_3=1 ...
Stereo USB final: ... confirmed=<N> sent=<N> skipped_msc=0 ...
```

An injected hold for a generation whose planned
duration was 3 will instead produce a 3->4
interval. Check the *actual* plan row in every
test. If `injected=0`, this specific run did
not test the hypothesis; do not treat it as
an injection success. The experiment is a
video-timing perturbation, so it intentionally
worsens the plan's exact-match statistics.

Record the optical boundary position at the
START and END of each run through a camera or
photodiodes, including whether it moves over
time. If a targeted extra full L/R cycle
improves optical results **consistently** across
independent randomized runs, it suggests an
indirect influence worth instrumenting further.
If not, focus on emitter/receiver acquisition
phase. A full pair consists of one LEFT and
one RIGHT flip, so it does not by itself
change left/right parity or the within-8.33ms
offset between each KMS callback and the
corresponding USB command. It also cannot
retroactively change the *first* USB command
if injected at generation 10 or 120.

This test is explicitly opt-in and must
never become default movie playback behavior
without a demonstrated optical mechanism.
