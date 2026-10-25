"""Decode TFT screen dumps ("shot" / "shotd" on the USB console) into PNGs.

    python tools/tftshot.py capture.log out.png          # final screen
    python tools/tftshot.py capture.log frames/ --all    # every frame, numbered

Blocks look like:  SHOT 320 240 [D] / R<y> <rgb565 hex>[*<count>] ... / SHOTEND <ms>
A delta (D) block only carries the rows that changed; they are applied on top
of the previous frame, so a capture should start with a full "shot".
"""
import os
import struct
import sys
import zlib


def rgb(c):
    return ((c >> 11) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def frames(text):
    """Yield (device_ms, rows) after each SHOTEND; rows is a list of pixel rows."""
    img, w = None, 0
    for ln in text.splitlines():
        if ln.startswith("SHOT "):
            p = ln.split()
            w, h = int(p[1]), int(p[2])
            if img is None or len(p) < 4:               # full frame: start clean
                img = [[(0, 0, 0)] * w for _ in range(h)]
        elif ln.startswith("SHOTEND"):
            if img is not None:
                p = ln.split()
                yield (int(p[1]) if len(p) > 1 else 0), img
        elif ln.startswith("R") and img is not None and ln[1:2].isdigit():
            parts = ln.split()
            y, x, row = int(parts[0][1:]), 0, []
            for tok in parts[1:]:
                c, _, n = tok.partition("*")
                row += [rgb(int(c, 16))] * int(n or 1)
            if 0 <= y < len(img):
                img[y] = (row + [(0, 0, 0)] * w)[:w]


def png(path, img):
    h, w = len(img), len(img[0])
    raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in img)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    if "--all" in sys.argv:
        os.makedirs(sys.argv[2], exist_ok=True)
        n = 0
        for ms, img in frames(text):
            png(os.path.join(sys.argv[2], f"{n:05d}_{ms}.png"), img)
            n += 1
        print(f"{n} frames")
    else:
        last = None
        for _, img in frames(text):
            last = [r[:] for r in img]
        if last is None:
            sys.exit("no SHOT block")
        png(sys.argv[2], last)
        print(sys.argv[2])
