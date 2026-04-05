#!/usr/bin/env python3
"""Prepare 3DS CIA assets: banner.png → 256×128, icon.png → 48×48 on white, silent WAV."""
from __future__ import annotations

import argparse
import sys
import wave
from pathlib import Path

try:
    from PIL import Image
except ImportError as e:
    print("prepare_cia_assets.py requires Pillow: pip install Pillow", file=sys.stderr)
    raise SystemExit(1) from e


def write_silent_wav(path: Path, duration_s: float = 0.2) -> None:
    framerate = 44100
    nframes = int(framerate * duration_s)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(framerate)
        w.writeframes(b"\x00\x00" * nframes)


def normalize_banner(im: Image.Image) -> Image.Image:
    """Ensure 256×128; if needed, scale to fit inside and center on a transparent canvas."""
    tw, th = 256, 128
    im = im.convert("RGBA")
    if im.size == (tw, th):
        return im
    w, h = im.size
    scale = min(tw / w, th / h)
    nw, nh = int(w * scale + 0.5), int(h * scale + 0.5)
    resized = im.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    canvas.paste(resized, ((tw - nw) // 2, (th - nh) // 2), resized)
    return canvas


def icon48_on_white(icon: Image.Image) -> Image.Image:
    icon = icon.convert("RGBA").resize((48, 48), Image.Resampling.LANCZOS)
    base = Image.new("RGB", (48, 48), (255, 255, 255))
    base.paste(icon, (0, 0), icon)
    return base


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--root", type=Path, required=True, help="Project root")
    p.add_argument("--build", type=Path, required=True, help="Build output directory")
    args = p.parse_args()
    root = args.root.resolve()
    bdir = args.build.resolve()
    bdir.mkdir(parents=True, exist_ok=True)

    banner = Image.open(root / "banner.png")
    normalize_banner(banner).save(bdir / "cia_banner.png", "PNG")

    icon = Image.open(root / "icon.png")
    icon48_on_white(icon).save(bdir / "cia_icon48.png", "PNG")

    write_silent_wav(bdir / "cia_silent.wav")
    print("Wrote", bdir / "cia_banner.png", bdir / "cia_icon48.png", bdir / "cia_silent.wav")


if __name__ == "__main__":
    main()
