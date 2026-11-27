"""Build the SmartEQ LittleFS sky database (no SD card needed).

  python tools/littlefs/build_littlefs.py

Shrinks the 96x96 thumbnails already built for the SD card
(docs/sdcard/smarteq-sd-files.zip, committed - no network access here) down to
63x63 so the whole catalog fits the esp32dev-littlefs env's ~2.3 MB flash
partition (partitions_littlefs.csv), and writes the result to
tools/littlefs/data/SMARTEQ/... - PlatformIO's `-t buildfs` / `-t uploadfs`
pack that folder for that one environment (scripts/littlefs_datadir.py).

File layout (read by src/sky.cpp, same as the SD card except image size):
  /SMARTEQ/CATALOG.CSV     unchanged (~10 KB)
  /SMARTEQ/IMG/<key>.565   'R565' + u16 w + u16 h + w*h little-endian RGB565,
                           resized from 96x96 to 63x63 (7946 B each)
  /SMARTEQ/INFO/<key>.TXT  unchanged (~40 KB)
"""
import struct
import sys
import zipfile
from pathlib import Path

from PIL import Image

HERE    = Path(__file__).resolve().parent
ROOT    = HERE.parent.parent
SRC_ZIP = ROOT / "docs" / "sdcard" / "smarteq-sd-files.zip"
OUT_DIR = HERE / "data"

# LittleFS allocates whole 4 KB blocks per file (minus a 4-byte CTZ pointer in
# every block after the first), and mklittlefs doesn't inline small files: each
# INFO/*.TXT costs a block, each thumbnail ceil(size / ~4 KB) blocks. 64x64
# (8200 B) needs 3 blocks - 178 objects x (3 + 1) = 712 > the 592-block
# partition, and mklittlefs drops whatever doesn't fit. 63x63 (7946 B) needs 2.
THUMB_W = THUMB_H = 63         # down from the SD card's 96x96 - fits on-chip flash
BLOCK      = 4096
FS_BYTES   = 0x250000          # spiffs partition size, partitions_littlefs.csv


def shrink_565(data, w, h):
    px = memoryview(data)
    img = Image.new("RGB", (w, h))
    pix = img.load()
    for y in range(h):
        for x in range(w):
            v = px[(y * w + x) * 2] | (px[(y * w + x) * 2 + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            pix[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    img = img.resize((THUMB_W, THUMB_H), Image.LANCZOS)
    out = bytearray(b"R565" + struct.pack("<HH", THUMB_W, THUMB_H))
    for pxl in img.getdata():
        r, g, b = pxl
        out += struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return bytes(out)


def main():
    if not SRC_ZIP.exists():
        sys.exit(f"missing {SRC_ZIP.relative_to(ROOT)} - run tools/sdcard/build_sd.py first "
                  "(or fetch it from the repo's docs/sdcard/)")
    if OUT_DIR.exists():
        for p in sorted(OUT_DIR.rglob("*"), reverse=True):
            if p.is_file(): p.unlink()
            else: p.rmdir()

    n = 0
    with zipfile.ZipFile(SRC_ZIP) as z:
        for info in z.infolist():
            data = z.read(info)
            out_path = OUT_DIR / info.filename
            out_path.parent.mkdir(parents=True, exist_ok=True)
            if info.filename.endswith(".565"):
                hdr, w, h = data[:4], *struct.unpack_from("<HH", data, 4)
                assert hdr == b"R565", info.filename
                data = shrink_565(data[8:], w, h)
                n += 1
            out_path.write_bytes(data)
    files = [p for p in OUT_DIR.rglob("*") if p.is_file()]
    dirs  = [p for p in OUT_DIR.rglob("*") if p.is_dir()]
    total = sum(p.stat().st_size for p in files)
    # rough block budget: file blocks + 2 metadata blocks per dir (+ root, + slack)
    blocks = sum(max(1, -(-p.stat().st_size // (BLOCK - 4))) for p in files) + 2 * (len(dirs) + 1) + 8
    avail  = FS_BYTES // BLOCK
    print(f"[littlefs] {n} thumbnails shrunk to {THUMB_W}x{THUMB_H}; "
          f"{total / 1024:.0f} KB in {len(files)} files, ~{blocks}/{avail} blocks "
          f"-> {OUT_DIR.relative_to(ROOT)}")
    if blocks > avail:
        sys.exit(f"[littlefs] won't fit the {FS_BYTES // 1024} KB partition - lower THUMB_W/THUMB_H")
    print("Next: pio run -e esp32dev-littlefs -t buildfs   (or -t uploadfs to flash it)")


if __name__ == "__main__":
    sys.exit(main())
