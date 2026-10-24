#!/usr/bin/env python3
"""Send a live NVIDIA shutter timer adjustment to an opt-in mpv DRM process.

This controls the firmware timer PARAMETER of subsequent USB eye commands;
it does not modify KMS scanout timing, auto-detect optical phase, or measure
the actual latency of the glasses. Observe results through a safe sensor.
"""
import argparse
import os
import re
import stat
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parent", action="store_true",
                        help="Target the parent mpv process (for mpv input.conf 'run')")
    parser.add_argument("pid", nargs="?", type=int,
                        help="Running mpv PID (omit when --parent is set)")
    parser.add_argument("command", nargs="?", help="get, +N, -N, or set:N (microseconds)")
    opts = parser.parse_args()

    # When invoked by mpv's 'run', use the parent's PID and treat
    # the single positional argument as a command, not as a PID.
    if opts.parent:
        arguments = sys.argv[1:]
        if "--parent" in arguments:
            arguments.remove("--parent")
        if len(arguments) != 1:
            parser.error("--parent needs one command (e.g. --parent +100)")
        pid = os.getppid()
        command = arguments[0]
    else:
        if opts.pid is None or opts.command is None:
            parser.error("provide mpv PID and command")
        pid = opts.pid
        command = opts.command

    if not (command == "get" or re.fullmatch(r"[+-][0-9]{1,4}", command)
            or re.fullmatch(r"set:[0-9]{3,4}", command)):
        parser.error("command must be get, +N, -N or set:N")
    if command.startswith("set:"):
        command = "set " + command[4:]
    if pid <= 0:
        parser.error("PID must be positive")

    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        parser.error("XDG_RUNTIME_DIR must be set")
    base = Path(runtime)
    bst = base.stat()
    if not stat.S_ISDIR(bst.st_mode) or bst.st_uid != os.geteuid() or bst.st_mode & 0o077:
        parser.error("XDG_RUNTIME_DIR must be private (owned by you, no group/other access)")
    fifo = base / f"mpv-stereo-usb-{pid}.fifo"
    try:
        old = fifo.lstat()
        if not stat.S_ISFIFO(old.st_mode) or old.st_uid != os.geteuid():
            parser.error("control path is not an owned FIFO: " + str(fifo))
        fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            cur = os.fstat(fd)
            if cur.st_ino != old.st_ino or cur.st_dev != old.st_dev:
                parser.error("FIFO was replaced while opening")
            os.write(fd, (command + "\n").encode("ascii"))
        finally:
            os.close(fd)
    except OSError as exc:
        parser.error(f"cannot write to {fifo}: {exc}")

    print(f"Sent {command!r} to mpv pid {pid}. Check the mpv log for "
          "'Stereo USB phase control: eye_timer_us=...'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
