"""PlatformIO post-build hook.

After every `pio run` this merges bootloader + partitions + boot_app0 + app into
one flashable image for the browser web-flasher and drops it in docs/firmware/
(served by GitHub Pages), then bumps the version in docs/manifest.json.

  output: docs/firmware/smarteq-rj9-merged.bin   (flash to offset 0x0)
"""
import datetime
import json
import subprocess
from pathlib import Path

Import("env")  # noqa: F821  (injected by PlatformIO)

board       = env.BoardConfig()                     # noqa: F821
PROJECT_DIR = Path(env["PROJECT_DIR"])              # noqa: F821
DOCS        = PROJECT_DIR / "docs"
OUT_BIN     = DOCS / "firmware" / "smarteq-rj9-merged.bin"


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

    manifest = DOCS / "manifest.json"
    if manifest.exists():
        m = json.loads(manifest.read_text())
        m["version"] = datetime.datetime.now().strftime("%Y.%m.%d-%H%M")
        manifest.write_text(json.dumps(m, indent=2) + "\n")
        ver = m["version"]
    else:
        ver = "?"
    print("[merge_bin] -> docs/firmware/%s  (version %s, %d KB)"
          % (OUT_BIN.name, ver, OUT_BIN.stat().st_size // 1024))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_and_publish)   # noqa: F821
