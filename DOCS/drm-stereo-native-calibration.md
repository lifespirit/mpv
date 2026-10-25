# Native DRM stereo calibration (experimental)

Branch: feature/drm-stereo-native-calibration

This is the first **calibration-screen and built-in input** increment. It does
not yet implement MSC-predicted USB dispatch or measured optical phase control.
`stereo-calibrate adjust` changes the libnvstusb *firmware command timer*
through the existing USB owner's FIFO. A successful request is **not** proof
of effective timing or optical synchronization.

## Start

Build this mpv branch against the usual prerequisites and build the separate
libnvstusb branch `feature/usb-eye-timer-calibration`. Example:

```sh
MPV_DRM_STEREO_USB=1 MPV_DRM_STEREO_USB_CONTROL=1 \
MPV_DRM_STEREO_USB_LIBRARY=/absolute/path/to/libnvstusb/src/.libs/libnvstusb.so \
MPV_DRM_STEREO_USB_FIRMWARE=/absolute/path/to/libnvstusb/nvstusb.fw \
./build/mpv --no-config --vo=gpu --gpu-context=drm \
  --drm-connector=DP-1 --drm-mode=2560x1440@120 \
  --drm-stereo-3d=yes --drm-stereo-calibration=before-play \
  --drm-vrr-enabled=no \
  --drm-stereo-pacing-eye=right --video-sync=display-resample \
  --input-ipc-server=/tmp/mpv-stereo-calibration.sock \
  --log-file=/tmp/mpv-stereo-native-calibration.log \
  /absolute/path/to/movie.mkv
```

The player pauses its *media clock* on file open, and renders one real
L-white/R-black pair into GBM. KMS and the USB worker continue alternating at
the physical 120 Hz cadence. It does not decode a separate test clip.

`c` toggles live timer control while the movie plays; `C` opens or
closes the calibration screen. In calibration:
- Left/Right: -/+100 us firmware timer; Up/Down: +25/-25 us.
- 1: left white/right black; 2: left black/right white.
- 3: both white; 4: both black.
- Enter or Esc: return to the movie (preserves changed timer).
- Usual seek arrows return when calibration/tuning exits.
- Same commands can be sent over mpv's JSON IPC: `stereo-calibrate`
  with `screen`, `tune`, `pattern N`, `adjust N`, `accept`.

To launch without initial calibration, use `--drm-stereo-calibration=manual`
(or omit the option, equivalent to `off`); the `c` and `C` controls
remain available whenever native 3D is active. The older opt-in environment
variable `MPV_DRM_STEREO_CALIBRATION=1` remains supported.

## Important limitations

This is an **unbuilt** experimental change pending compilation on the target
Linux/DRM host. The GPU calibration path requires existing EGL native fences.
It preserves the KMS clock and attempts to flush only unpublished queues when
changing patterns, but transitions (including paused redraw) must be verified
against KMS/GBM and USB diagnostics. The experimental existing FIFO must be
enabled before timer hotkeys can work.

No measured physical IR/optical phase and no automatic compensation exist yet.
The next work item is bidirectional USB control plus deadline scheduling tied
to predicted MSC; keep that change separate from validating the native screen.
