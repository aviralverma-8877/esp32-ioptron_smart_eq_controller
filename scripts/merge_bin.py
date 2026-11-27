"""PlatformIO post-build hook, used by both environments.

After every `pio run` this merges bootloader + partitions + boot_app0 + app into
one flashable image for the browser web-flasher and drops it in docs/firmware/
(served by GitHub Pages), then bumps the version in that env's manifest.

  esp32dev           docs/firmware/smarteq-rj9-merged.bin           (0x0)
                     docs/manifest.json
  esp32dev-littlefs  docs/firmware/smarteq-rj9-littlefs-merged.bin  (0x0)
                     docs/firmware/smarteq-littlefs.bin             (spiffs partition offset)
                     docs/manifest-littlefs.json
                     The filesystem image is copied by `pio run -e esp32dev-littlefs -t buildfs`
                     (run tools/littlefs/build_littlefs.py first).
"""
import csv
import datetime
import json
import shutil
import subprocess
from pathlib import Path

Import("env")  # noqa: F821  (injected by PlatformIO)

board       = env.BoardConfig()                     # noqa: F821
PROJECT_DIR = Path(env["PROJECT_DIR"])              # noqa: F821
DOCS        = PROJECT_DIR / "docs"
LITTLEFS    = env["PIOENV"] == "esp32dev-littlefs"  # noqa: F821

if LITTLEFS:
    OUT_BIN  = DOCS / "firmware" / "smarteq-rj9-littlefs-merged.bin"
    OUT_FS   = DOCS / "firmware" / "smarteq-littlefs.bin"
    MANIFEST = DOCS / "manifest-littlefs.json"
else:
    OUT_BIN  = DOCS / "firmware" / "smarteq-rj9-merged.bin"
    OUT_FS   = None
    MANIFEST = DOCS / "manifest.json"


def fs_offset():
    """Offset of the data partition ("spiffs", used by LittleFS) from the env's partition CSV."""
    csv_path = PROJECT_DIR / env.GetProjectOption("board_build.partitions")  # noqa: F821
    for row in csv.reader(l for l in csv_path.read_text().splitlines()
                          if l.strip() and not l.lstrip().startswith("#")):
        row = [c.strip() for c in row]
        if row[0] == "spiffs":
            return int(row[3], 0)
    raise RuntimeError("no spiffs partition in " + str(csv_path))


def bump_manifest(fs_off=None):
    if not MANIFEST.exists():
        return "?"
    m = json.loads(MANIFEST.read_text())
    m["version"] = datetime.datetime.now().strftime("%Y.%m.%d-%H%M")
    if fs_off is not None:                          # keep the FS part's offset in sync with the CSV
        for b in m.get("builds", []):
            for p in b.get("parts", []):
                if p["path"].endswith(OUT_FS.name):
                    p["offset"] = fs_off
    MANIFEST.write_text(json.dumps(m, indent=2) + "\n")
    return m["version"]


def merge_and_publish(*_a, **_kw):
    build_dir = Path(env.subst("$BUILD_DIR"))       # noqa: F821
    progname  = env.subst("$PROGNAME")              # noqa: F821
    boot_app0 = Path(env.subst("$PROJECT_PACKAGES_DIR")) / (
        "framework-arduinoespressif32/tools/partitions/boot_app0.bin")

    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        env.subst("$PYTHONEXE"), env.subst("$OBJCOPY"),
        "--chip", board.get("build.mcu", "esp32"), "merge_bin",
        "-o", str(OUT_BIN),
        "--flash_mode", board.get("build.flash_mode", "dio"),
        "--flash_freq", str(board.get("build.f_flash", "40000000L")).replace("000000L", "m"),
        "--flash_size", board.get("upload.flash_size", "4MB"),
        "0x1000",  str(build_dir / "bootloader.bin"),
        "0x8000",  str(build_dir / "partitions.bin"),
        "0xe000",  str(boot_app0),
        "0x10000", str(build_dir / (progname + ".bin")),
    ]
    subprocess.run(cmd, check=True)
    ver = bump_manifest(fs_offset() if LITTLEFS else None)
    print("[merge_bin] -> docs/firmware/%s  (version %s, %d KB)"
          % (OUT_BIN.name, ver, OUT_BIN.stat().st_size // 1024))


def publish_fs(*_a, **_kw):
    src = Path(env.subst("$BUILD_DIR")) / "littlefs.bin"   # noqa: F821
    shutil.copyfile(src, OUT_FS)
    ver = bump_manifest(fs_offset())
    print("[merge_bin] -> docs/firmware/%s  (offset 0x%X, version %s, %d KB)"
          % (OUT_FS.name, fs_offset(), ver, OUT_FS.stat().st_size // 1024))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_and_publish)   # noqa: F821
if LITTLEFS:
    env.AddPostAction("$BUILD_DIR/littlefs.bin", publish_fs)         # noqa: F821
