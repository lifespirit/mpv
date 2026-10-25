#!/usr/bin/env python3
"""Software-only native DRM stereo calibration transition smoke test.

Requires a real 120 Hz DRM session with an available local TTY and one normal
movie file, but NO NVIDIA emitter/firmware: USB is dry-run. This does not
measure optical phase. Do not wear shutter glasses for unverified testing.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time


def request(conn, request_id, command):
    payload = json.dumps({"command": command, "request_id": request_id}) + "\n"
    conn.sendall(payload.encode())
    deadline = time.monotonic() + 8.0
    buffer = b""
    while time.monotonic() < deadline:
        conn.settimeout(max(.1, deadline - time.monotonic()))
        try:
            block = conn.recv(65536)
        except (ConnectionResetError, BrokenPipeError, socket.timeout, OSError) as exc:
            raise RuntimeError(f"mpv disconnected its IPC client: {exc}") from exc
        if not block:
            raise RuntimeError("mpv closed the IPC connection unexpectedly")
        buffer += block
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            answer = json.loads(line)
            if answer.get("request_id") == request_id:
                if answer.get("error") != "success":
                    raise RuntimeError(f"IPC {command!r}: {answer}")
                return answer.get("data")
    raise TimeoutError(f"missing IPC reply to {command!r}")


def wait_socket(path, process):
    until = time.monotonic() + 15
    while time.monotonic() < until:
        if process.poll() is not None:
            raise RuntimeError(f"mpv exited during startup: {process.returncode}")
        if path.exists():
            return
        time.sleep(.05)
    raise TimeoutError("mpv IPC socket did not appear")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path, help="Existing playable movie")
    parser.add_argument("--mpv", type=Path, default=Path("./build/mpv"))
    parser.add_argument("--connector", default="DP-1")
    parser.add_argument("--mode", default="2560x1440@120")
    parser.add_argument("--log", type=Path,
                        default=Path("/tmp/mpv-stereo-native-calibration-smoke.log"))
    args = parser.parse_args()
    if not args.video.is_file() or not args.mpv.is_file():
        parser.error("video and mpv binary must exist")

    args.log.parent.mkdir(parents=True, exist_ok=True)
    # mpv may fail before it has initialized VO; retain every diagnostic.
    # Do not send both stdout and stderr to DEVNULL.
    console_log = args.log.with_suffix(".console.log")
    failure = None
    with tempfile.TemporaryDirectory(prefix="mpv-stereo-cal-") as tmp:
        ipc = Path(tmp) / "control.sock"
        env = os.environ.copy()
        for name in ("MPV_DRM_STEREO_PLAN_HOLD", "MPV_DRM_STEREO_TWO_SLOT_QUEUE",
                     "MPV_DRM_STEREO_USB", "MPV_DRM_STEREO_USB_CONTROL",
                     "MPV_DRM_STEREO_USB_DRY_RUN", "NVSTUSB_EYE_TIMER_US"):
            env.pop(name, None)
        env.pop("MPV_DRM_STEREO_CALIBRATION", None)
        env["MPV_DRM_STEREO_USB"] = "1"
        env["MPV_DRM_STEREO_USB_DRY_RUN"] = "1"
        cmd = [
            str(args.mpv.resolve()), "--no-config", "--terminal=no",
            "--input-terminal=no", "--vo=gpu", "--gpu-context=drm",
            f"--drm-connector={args.connector}", f"--drm-mode={args.mode}",
            "--drm-vrr-enabled=no", "--drm-stereo-3d=yes",
            "--drm-stereo-calibration=before-play",
            "--drm-stereo-pacing-eye=right", "--video-sync=display-resample",
            "--no-audio", "--no-sub", f"--input-ipc-server={ipc}",
            f"--log-file={args.log.resolve()}", str(args.video.resolve())]
        with console_log.open("wb") as console:
            process = subprocess.Popen(
                cmd, env=env, stdin=subprocess.DEVNULL,
                stdout=console, stderr=subprocess.STDOUT)
        try:
            wait_socket(ipc, process)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
                conn.connect(str(ipc))
                seq = 0

                def call(*items):
                    nonlocal seq
                    seq += 1
                    return request(conn, seq, list(items))

                # Some commands are accepted before the VO is ready; wait
                # for the configured display and first calibration frame.
                until = time.monotonic() + 15
                while True:
                    try:
                        if call("get_property", "video-out-params"):
                            break
                    except RuntimeError as exc:
                        # A property may be unavailable until first VO
                        # reconfiguration. An IPC disconnect is NOT transient.
                        if "property unavailable" not in str(exc).lower():
                            raise
                    rc = process.poll()
                    if rc is not None:
                        raise RuntimeError(
                            f"mpv exited before VIDEO_RECONFIG (returncode={rc})")
                    if time.monotonic() >= until:
                        raise TimeoutError(
                            "video output was not configured within 15s")
                    time.sleep(.1)
                if call("get_property", "pause") is not True:
                    raise AssertionError("startup calibration did not pause media")
                time.sleep(.6)

                for pattern in (1, 2, 3, 0):
                    call("stereo-calibrate", "pattern", pattern)
                    time.sleep(.2)

                call("stereo-calibrate", "accept")
                time.sleep(.5)
                if call("get_property", "pause") is not False:
                    raise AssertionError("accept did not resume the film")

                call("stereo-calibrate", "screen")
                time.sleep(.3)
                if call("get_property", "pause") is not True:
                    raise AssertionError("entering calibration failed to pause")
                call("stereo-calibrate", "pattern", 2)
                call("stereo-calibrate", "accept")
                time.sleep(.3)
                if call("get_property", "pause") is not False:
                    raise AssertionError("second accept did not resume")
                # mpv may close the IPC socket before acknowledging quit.
                conn.sendall(json.dumps({"command": ["quit"]}).encode() + b"\n")
            process.wait(timeout=12)
        except Exception as exc:
            failure = f"{type(exc).__name__}: {exc}"
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    content = args.log.read_text(errors="replace") if args.log.exists() else ""
    kms = re.findall(r"Stereo DRM final:[^\n]*", content)
    usb = re.findall(r"Stereo USB final:[^\n]*", content)
    errors = []
    if failure:
        errors.append(f"smoke test: {failure}")
    if process.returncode != 0:
        errors.append(f"mpv exit code {process.returncode}")
    if not kms or not usb:
        errors.append("missing final DRM or USB accounting")
    else:
        for field in ("missed_vblanks", "parity_errors", "wrong_eye",
                      "wrong_hold", "commit_errors", "fence_errors"):
            match = re.search(rf"\b{field}=(\d+)", kms[-1])
            if not match or int(match.group(1)):
                errors.append(f"KMS {field}: nonzero/missing")
        for field in ("wake_errors", "usb_errors"):
            match = re.search(rf"\b{field}=(\d+)", usb[-1])
            if not match or int(match.group(1)):
                errors.append(f"USB {field}: nonzero/missing")
    if "Native DRM stereo calibration active before playback" not in content:
        errors.append("missing startup calibration indication")
    for e in errors:
        print("FAIL:", e)
    if not errors:
        print("PASS: native calibration state transitions and software transport")
    print("mpv log:", args.log)
    print("mpv console:", console_log)
    if errors:
        for diagnostic in (args.log, console_log):
            print(f"--- {diagnostic}: last 65 lines ---")
            if diagnostic.exists():
                lines = diagnostic.read_text(errors="replace").splitlines()
                for line in lines[-65:]:
                    print(line)
            else:
                print("(not created)")
    print("Optical timing NOT measured; real USB timer adjustment NOT exercised.")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
