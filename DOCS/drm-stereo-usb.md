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
