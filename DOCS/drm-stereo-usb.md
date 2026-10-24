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
