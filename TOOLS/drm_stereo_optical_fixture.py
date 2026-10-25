#!/usr/bin/env python3
"""Build a deterministic Half-SBS optical shutter fixture via FFmpeg.

Four equal phases, each constant across all encoded video frames:
  1: left WHITE, right BLACK
  2: left BLACK, right WHITE
  3: left WHITE, right WHITE (lens duty/baseline)
  4: left BLACK, right BLACK (dark/background baseline)

Do not infer lens optical phase from KMS/USB event counters. Record the actual
light transmitted through each lens using a fast photosensor where possible.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def make_command(output, *, width, height, phase_seconds, fps, crf, codec, overwrite):
    p = phase_seconds
    # Unescaped commas are filter separators; escape those used in enable.
    left = f"lt(t\\,{p})+gte(t\\,{2*p})*lt(t\\,{3*p})"
    right = f"gte(t\\,{p})*lt(t\\,{3*p})"
    vf = (f"drawbox=x=0:y=0:w={width//2}:h={height}:c=white:t=fill:enable='{left}',"
          f"drawbox=x={width//2}:y=0:w={width//2}:h={height}:c=white:t=fill:enable='{right}'")
    args = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y" if overwrite else "-n", "-f", "lavfi",
            "-i", f"color=c=black:s={width}x{height}:r={fps}:d={p*4}",
            "-vf", vf, "-an", "-c:v", codec, "-pix_fmt", "yuv420p"]
    if codec == "libx264":
        args += ["-preset", "ultrafast", "-crf", str(crf), "-x264-params", "keyint=24:min-keyint=24:scenecut=0"]
    args += ["-metadata:s:v:0", "stereo_mode=left_right", "-f", "matroska", str(output)]
    return args


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path, help="Output MKV path (must not already contain valuable data)")
    p.add_argument("--size", default="1920x1080", help="Even-numbered pixel dimensions")
    p.add_argument("--phase-seconds", type=int, default=5, help="Each of four phases, 1..30 seconds")
    p.add_argument("--fps", default="24000/1001", help="Movie frame rate; default ~23.976fps")
    p.add_argument("--codec", choices=["libx264", "mpeg4"], default="libx264")
    p.add_argument("--crf", type=int, default=16)
    p.add_argument("--overwrite", action="store_true", help="Allow replacement of existing output")
    args = p.parse_args()
    try:
        w, h = map(int, args.size.split("x"))
    except (ValueError, TypeError):
        p.error("--size must be WIDTHxHEIGHT")
    if w < 320 or h < 240 or w % 4 or h % 2:
        p.error("width must be divisible by 4 and >=320; height even and >=240")
    if not 1 <= args.phase_seconds <= 30:
        p.error("--phase-seconds must be 1..30")
    if not 0 <= args.crf <= 51:
        p.error("--crf must be 0..51")
    if args.output.exists() and not args.overwrite:
        p.error("output exists; pass --overwrite to replace it")
    if shutil.which("ffmpeg") is None:
        p.error("ffmpeg executable not found in PATH")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = make_command(args.output, width=w, height=h,
                           phase_seconds=args.phase_seconds, fps=args.fps,
                           crf=args.crf, codec=args.codec, overwrite=args.overwrite)
    print("Generating deterministic L/R Half-SBS optical fixture:")
    print("  phase 1: LEFT white / RIGHT black")
    print("  phase 2: LEFT black / RIGHT white")
    print("  phase 3: BOTH white")
    print("  phase 4: BOTH black")
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as exc:
        print(f"ffmpeg failed with exit status {exc.returncode}", file=sys.stderr)
        return exc.returncode
    print(f"File: {args.output} ({args.output.stat().st_size} bytes)")
    print("Phase time:", args.phase_seconds, "s; total time:", args.phase_seconds*4, "s")
    print("The MKV stereo_mode tag is written for compatibility; mpv still needs --drm-stereo-3d=yes.")
    print("This test video does not prove actual lens/IR phase. Never use the bare screen's 120Hz flicker as the glasses timing reference.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
