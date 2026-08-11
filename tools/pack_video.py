#!/usr/bin/env python3
import argparse
import shutil
import struct
import subprocess
import tempfile
from fractions import Fraction
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

MAGIC = b"LVGLMJPG"
HEADER_SIZE = 64


def run(cmd):
    subprocess.run(cmd, check=True)


def probe(ffprobe, src):
    cmd = [
        str(ffprobe), "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,avg_frame_rate",
        "-of", "csv=p=0", str(src),
    ]
    out = subprocess.check_output(cmd, text=True).strip().splitlines()[0]
    width, height, fps = out.split(",")
    return int(width), int(height), Fraction(fps)


def find_ffmpeg_dir(explicit):
    """Locate the directory containing ffmpeg.exe/ffprobe.exe.

    Priority: explicit --ffmpeg-dir, bundled tools/ffmpeg/bin next to this
    script, then ffmpeg/ffprobe found on PATH.
    """
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.append(SCRIPT_DIR / "ffmpeg" / "bin")
    for exe_name in ("ffmpeg", "ffprobe"):
        exe = shutil.which(exe_name)
        if exe:
            candidates.append(Path(exe).resolve().parent)

    seen = set()
    for candidate in candidates:
        candidate = Path(candidate)
        if not candidate.is_absolute():
            candidate = Path.cwd() / candidate
        candidate = candidate.resolve()
        if candidate in seen:
            continue
        seen.add(candidate)
        if (candidate / "ffmpeg.exe").is_file() and (candidate / "ffprobe.exe").is_file():
            return candidate

    raise SystemExit(
        "ffmpeg.exe/ffprobe.exe not found.\n"
        "Searched: " + ", ".join(str(p) for p in seen) + "\n"
        "Run with --ffmpeg-dir <dir> pointing to a directory containing both, "
        "or add ffmpeg to PATH."
    )


def main():
    parser = argparse.ArgumentParser(description="Pack an MP4 into an ESP-IDF data partition MJPEG stream.")
    parser.add_argument("--input", default="untitled.mp4", help="source video")
    parser.add_argument("--output", default="assets/video/video.bin", help="output partition image")
    parser.add_argument("--ffmpeg-dir", default=None,
                        help="directory containing ffmpeg.exe (default: tools/ffmpeg/bin next to this script, then PATH)")
    parser.add_argument("--fps", type=int, default=30, help="output frame rate; resolution is not changed")
    parser.add_argument("--quality", type=int, default=24, help="ffmpeg mjpeg q:v value, lower is higher quality")
    parser.add_argument("--max-size", default="0x15F0000", help="video partition size limit")
    args = parser.parse_args()

    ffmpeg_dir = find_ffmpeg_dir(args.ffmpeg_dir)
    ffmpeg = ffmpeg_dir / "ffmpeg.exe"
    ffprobe = ffmpeg_dir / "ffprobe.exe"

    src = Path(args.input)
    dst = Path(args.output)
    max_size = int(str(args.max_size), 0)
    width, height, src_fps = probe(ffprobe, src)
    fps = max(1, min(args.fps, int(src_fps)))

    dst.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lvgl_mjpg_") as td:
        frame_dir = Path(td)
        frame_pattern = frame_dir / "frame_%06d.jpg"
        run([
            str(ffmpeg), "-y", "-i", str(src), "-an",
            "-vf", f"fps={fps}", "-q:v", str(args.quality),
            "-pix_fmt", "yuvj420p", str(frame_pattern),
        ])

        frames = sorted(frame_dir.glob("frame_*.jpg"))
        if not frames:
            raise SystemExit("no frames generated")

        index_offset = HEADER_SIZE
        frames_offset = HEADER_SIZE + len(frames) * 8
        offsets = []
        cursor = frames_offset
        max_frame_size = 0

        with dst.open("wb") as out:
            out.write(b"\x00" * frames_offset)
            for frame in frames:
                data = frame.read_bytes()
                offsets.append((cursor, len(data)))
                out.write(data)
                cursor += len(data)
                max_frame_size = max(max_frame_size, len(data))

            header = struct.pack(
                "<8sHHHHIIIII28s",
                MAGIC, width, height, fps, 1, len(frames), index_offset,
                frames_offset, max_frame_size, 0, b"\x00" * 28,
            )
            out.seek(0)
            out.write(header)
            for offset, size in offsets:
                out.write(struct.pack("<II", offset, size))

        size = dst.stat().st_size
        print(f"packed {len(frames)} frames, {width}x{height}@{fps}fps, max frame {max_frame_size} bytes")
        print(f"output: {dst} ({size} bytes / limit {max_size} bytes)")
        if size > max_size:
            dst.unlink(missing_ok=True)
            raise SystemExit("packed video is larger than the video partition; increase --quality value or lower --fps")


if __name__ == "__main__":
    main()
