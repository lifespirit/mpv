#!/usr/bin/env python3
"""Opt-in DRM-stereo USB bridge smoke test. Default: NO USB / NO IR."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def counters(line):
    return {
        key: (float(value) if "." in value else int(value))
        for key, value in re.findall(r"([A-Za-z][A-Za-z0-9_]*)=(-?\d+(?:\.\d+)?)", line)
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("video", type=Path, help="Local Half-SBS video file")
    ap.add_argument("--mpv", default="./build/mpv")
    ap.add_argument("--connector", default="DP-1")
    ap.add_argument("--mode", default="2560x1440@120")
    ap.add_argument("--start", type=int, default=295)
    ap.add_argument("--length", type=int, default=15)
    ap.add_argument("--usb-init-timeout-ms", type=int, default=3000,
                    help="VO preflight readiness timeout, 100..10000 ms")
    ap.add_argument("--live-usb", action="store_true",
                    help="Enable actual USB IR commands; dry-run is default")
    ap.add_argument("--libnvstusb", type=Path,
                    help="Path to locally built libnvstusb.so for --live-usb")
    ap.add_argument("--firmware", type=Path,
                    help="Path to extracted nvstusb.fw for --live-usb")
    ap.add_argument("--invert-eyes", action="store_true")
    ap.add_argument("--outdir", type=Path, default=Path("/tmp"))
    args = ap.parse_args()

    if not args.video.is_file():
        ap.error("video not found: " + str(args.video))
    if args.length < 5 or args.length > 120:
        ap.error("length must be 5..120 seconds")
    if not 100 <= args.usb_init_timeout_ms <= 10000:
        ap.error("--usb-init-timeout-ms must be 100..10000")
    if args.live_usb and (not args.firmware or not args.libnvstusb):
        ap.error("--live-usb requires --firmware and --libnvstusb")
    if args.live_usb and (not args.firmware.is_file() or
                          not args.libnvstusb.is_file()):
        ap.error("firmware or libnvstusb library file does not exist")

    args.outdir.mkdir(parents=True, exist_ok=True)
    run_id = str(int(time.time())) + "-" + str(os.getpid())
    prefix = args.outdir / ("mpv-stereo-usb-" + run_id)
    log_path = Path(str(prefix) + ".log")
    summary_path = Path(str(prefix) + ".json")

    env = os.environ.copy()
    # All policy overrides inherited from a shell could hide a regression.
    for key in ("MPV_DRM_STEREO_PLAN_HOLD", "MPV_DRM_STEREO_TWO_SLOT_QUEUE",
                "MPV_DRM_STEREO_USB", "MPV_DRM_STEREO_USB_DRY_RUN",
                "MPV_DRM_STEREO_USB_LIBRARY",
                "MPV_DRM_STEREO_USB_FIRMWARE",
                "MPV_DRM_STEREO_USB_INVERT_EYES",
                "MPV_DRM_STEREO_USB_INIT_TIMEOUT_MS"):
        env.pop(key, None)
    env["MPV_DRM_STEREO_USB"] = "1"
    env["MPV_DRM_STEREO_USB_INIT_TIMEOUT_MS"] = str(args.usb_init_timeout_ms)
    if not args.live_usb:
        env["MPV_DRM_STEREO_USB_DRY_RUN"] = "1"
    else:
        env["MPV_DRM_STEREO_USB_LIBRARY"] = str(args.libnvstusb.resolve())
        env["MPV_DRM_STEREO_USB_FIRMWARE"] = str(args.firmware.resolve())
        if args.invert_eyes:
            env["MPV_DRM_STEREO_USB_INVERT_EYES"] = "1"
        print("WARNING: LIVE USB IR TEST, OPTICAL PHASE UNCALIBRATED.")
        print("Do not wear shutter glasses for this initial experiment.")

    cmd = [args.mpv, "--no-config", "--terminal=no", "--input-terminal=no",
           "--vo=gpu", "--gpu-context=drm",
           "--drm-connector=" + args.connector,
           "--drm-mode=" + args.mode,
           "--drm-vrr-enabled=no", "--drm-stereo-3d=yes",
           "--drm-stereo-pacing-eye=right", "--drm-stereo-stats=no",
           "--video-sync=display-resample", "--hwdec=auto",
           "--start=" + str(args.start), "--length=" + str(args.length),
           "--log-file=" + str(log_path), str(args.video.resolve())]

    print("Mode:", "LIVE USB" if args.live_usb else "DRY-RUN (no USB)")
    print("Log:", log_path)
    rc, timed_out = 124, False
    try:
        with open(os.devnull, "wb") as output:
            process = subprocess.run(cmd, env=env, stdout=output, stderr=output,
                                     timeout=args.length + 90)
        rc = process.returncode
    except subprocess.TimeoutExpired:
        timed_out = True
        print("ERROR: mpv exceeded test timeout")
    except OSError as exc:
        print("ERROR: could not launch mpv:", exc)

    content = log_path.read_text(errors="replace") if log_path.exists() else ""
    usb = re.findall(r"Stereo USB final:[^\n]*", content)
    kms = re.findall(r"Stereo DRM final:[^\n]*", content)
    policy = re.findall(r"Stereo scheduling policy:[^\n]*", content)
    usb_values = counters(usb[-1]) if usb else {}
    kms_values = counters(kms[-1]) if kms else {}
    # Drop the textual override examples ("PLAN_HOLD=0|1"), which are not
    # current setting values. Only the leading actual policy is meaningful.
    policy_values = counters(policy[-1].split("; overrides:")[0]) if policy else {}

    errors = []
    if rc != 0 or timed_out:
        errors.append("mpv return code / timeout")
    if not usb:
        errors.append("missing Stereo USB final")
    if not kms:
        errors.append("missing Stereo DRM final")
    if policy_values.get("plan_hold") != 1 or policy_values.get("two_slot_queue") != 1:
        errors.append("default QUEUE2/PLAN_HOLD policy not active")
    if usb_values.get("dry_run") != (0 if args.live_usb else 1):
        errors.append("USB dry-run status mismatch")
    if usb_values.get("init_status") != 1:
        errors.append("USB sender failed to initialize")
    if usb_values.get("confirmed", 0) < 100:
        errors.append("insufficient confirmed KMS flips")
    if usb_values.get("sent", 0) < 100:
        errors.append("insufficient USB (or simulated) commands")
    if "startup_skipped" not in usb_values:
        errors.append("missing startup_skipped diagnostic (rebuild mpv)")
    elif usb_values["startup_skipped"] != 0:
        errors.append("USB preflight failed: startup_skipped nonzero")
    if "Stereo USB: startup gate READY" not in content:
        errors.append("USB emitter startup gate not confirmed")
    for field in ("wake_errors", "usb_errors"):
        if usb_values.get(field, 1) != 0:
            errors.append("USB " + field + " nonzero")
    for field in ("missed_vblanks", "parity_errors", "wrong_eye",
                  "wrong_hold", "commit_errors", "fence_errors"):
        if kms_values.get(field, 1) != 0:
            errors.append("KMS " + field + " nonzero")

    # Startup USB firmware configuration often runs concurrently with
    # playback. Report those unsent flips separately from steady-state loss.
    # Neither count is a KMS flip error; both matter for shutter startup.
    warnings = []
    for field in ("startup_skipped", "skipped_msc", "unaccounted",
                  "late_gt_period"):
        if usb_values.get(field, 0) > 0:
            warnings.append("USB " + field + "=" + str(usb_values[field]))
    data = {
        "mode": "live" if args.live_usb else "dry-run",
        "mpv_returncode": rc,
        "timed_out": timed_out,
        "usb_init_timeout_ms": args.usb_init_timeout_ms,
        "log": str(log_path),
        "usb": usb_values, "kms": kms_values,
        "policy": policy_values, "warnings": warnings, "errors": errors,
        "pass": not errors,
        "optical_phase_verified": False,
    }
    summary_path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    print("Summary:", summary_path)
    print("KMS flips:", kms_values.get("flips"), "USB confirmed:",
          usb_values.get("confirmed"), "sent:", usb_values.get("sent"))
    print("USB startup skips:", usb_values.get("startup_skipped"),
          "steady-state MSC skips:", usb_values.get("skipped_msc"),
          "unaccounted:", usb_values.get("unaccounted"),
          "late periods:", usb_values.get("late_gt_period"))
    for warning in warnings:
        print("WARNING:", warning)
    for error in errors:
        print("FAIL:", error)
    print("PASS: software/USB-submission smoke test only"
          if not errors else "FAIL: smoke test")
    print("Optical lens phase has NOT been verified.")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
