"""Build the SmartEQ SD-card sky database.

  python tools/sdcard/build_sd.py                    # images + card folder + .img
  python tools/sdcard/build_sd.py --refresh-catalog  # also re-download catalog.csv

Stages
  1. catalog.csv  (committed)  Messier objects from OpenNGC + bright stars from
                               SIMBAD (stars.txt) + planets/Moon (positions are
                               computed on the device). Only rebuilt on request.
  2. images       (cached)     DSS2-colour cutout per object from CDS hips2fits
                               (planets: NASA images via Wikimedia Commons),
                               converted to 96x96 raw RGB565 (*.565).
  3. card folder  build/sdcard/card/SMARTEQ/...
  4. outputs      docs/sdcard/smarteq-sd.img.zip    flash with Etcher / Rufus
                  docs/sdcard/smarteq-sd-files.zip  or unzip onto a FAT32 card

Card layout (8.3 names, read by the firmware):
  /SMARTEQ/CATALOG.CSV     key,label,name,type,con,mag,size,ra_h,dec_d (J2000)
  /SMARTEQ/IMG/<key>.565   'R565' + u16 w + u16 h + w*h little-endian RGB565
  /SMARTEQ/INFO/<key>.TXT  4 lines: what it is / Scope: / Camera: / Exposure:
  /SMARTEQ/README.TXT      credits
"""
import argparse
import csv
import datetime
import io
import math
import re
import struct
import sys
import time
import zipfile
from pathlib import Path
from xml.etree import ElementTree

import requests
from PIL import Image, ImageOps

HERE      = Path(__file__).resolve().parent
ROOT      = HERE.parent.parent
CATALOG   = HERE / "catalog.csv"
STARS     = HERE / "stars.txt"
DESCS     = HERE / "descriptions.txt"
BUILD     = ROOT / "build" / "sdcard"
CACHE     = BUILD / "cache"
CARD      = BUILD / "card"
OUT_DIR   = ROOT / "docs" / "sdcard"

OPENNGC   = "https://raw.githubusercontent.com/mattiaverga/OpenNGC/master/database_files/"
SESAME    = "https://cds.unistra.fr/cgi-bin/nph-sesame/-oxp/S?"
HIPS2FITS = "https://alasky.cds.unistra.fr/hips-image-services/hips2fits"
HIPS      = "CDS/P/DSS2/color"
COMMONS   = "https://commons.wikimedia.org/wiki/Special:FilePath/"
USER_AGENT = ("SmartEQ-sdbuild/1.0 "
              "(+https://github.com/aviralverma-8877/esp32-ioptron_smart_eq_controller)")

IMG_W = IMG_H = 96              # left-panel thumbnail on the object page
FETCH_PX      = 160             # cutout size requested from hips2fits
STAR_FOV_DEG  = 1.0
EPOCH_YEAR    = 2026.5          # proper motion applied up to this epoch
IMG_MB        = 64              # FAT32 image size; fits any card >= 64 MB

HEADER = ("# SmartEQ sky catalog v1 - key,label,name,type,con,mag,size_arcmin,"
          "ra_hours_j2000,dec_deg_j2000")

TYPE_NAMES = {
    "G": "Galaxy", "GPair": "Galaxy pr", "GTrpl": "Galaxy gr", "GGroup": "Galaxy gr",
    "OCl": "Open cl.", "GCl": "Globular", "PN": "Planetary", "Neb": "Nebula",
    "HII": "Nebula", "EmN": "Nebula", "RfN": "Nebula", "SNR": "SN remnant",
    "Cl+N": "Cl+Nebula", "**": "Double", "*Ass": "Star cloud", "Other": "Asterism",
}

# Solar-system bodies: no fixed RA/Dec (the firmware's ephemeris computes them).
# key, label, name, type, typical V mag, Commons image (NASA, public domain unless noted)
PLANETS = [
    ("P001", "Mercury", "Innermost planet", "Planet", "0.0",  "Mercury_in_true_color.jpg"),
    ("P002", "Venus",   "Evening/morning star", "Planet", "-4.2", "Venus-real_color.jpg"),
    ("P003", "Mars",    "The red planet", "Planet", "0.5",  "OSIRIS_Mars_true_color.jpg"),
    ("P004", "Jupiter", "Gas giant", "Planet", "-2.3", "Jupiter_and_its_shrunken_Great_Red_Spot.jpg"),
    ("P005", "Saturn",  "Ringed planet", "Planet", "0.6",  "Saturn_during_Equinox.jpg"),
    ("P006", "Uranus",  "Ice giant", "Planet", "5.7",  "Uranus2.jpg"),
    ("P007", "Neptune", "Ice giant", "Planet", "7.8",  "Neptune_Full.jpg"),
    ("P008", "Moon",    "Earth's Moon", "Moon", "-12.7", "FullMoon2010.jpg"),  # G. H. Revera, CC-BY-SA-3.0
]
SOLAR = {"Planet", "Moon"}

# gaps in OpenNGC: names for objects without a common name, V magnitudes it lacks
NAME_FIX = {40: "Winnecke 4", 102: "Spindle Galaxy"}
MAG_FIX  = {22: 5.1, 35: 5.3, 62: 6.5, 69: 7.6, 70: 7.9, 71: 8.2, 107: 7.9}


# ---------------------------------------------------------------- catalog
def hms_to_h(s):
    h, m, sec = (float(x) for x in s.split(":"))
    return h + m / 60 + sec / 3600


def dms_to_d(s):
    sign = -1 if s.strip().startswith("-") else 1
    d, m, sec = (abs(float(x)) for x in s.split(":"))
    return sign * (d + m / 60 + sec / 3600)


def pretty_designation(name):
    m = re.match(r"([A-Za-z]+)0*(\d+.*)", name)
    return f"{m.group(1)} {m.group(2)}" if m else name


def messier_rows():
    tables = []
    for f in ("NGC.csv", "addendum.csv"):
        r = requests.get(OPENNGC + f, timeout=60)
        r.raise_for_status()
        tables += list(csv.DictReader(io.StringIO(r.text), delimiter=";"))
    by_m = {}
    for t in tables:
        if t.get("M") and t["Name"] != "M102":      # OpenNGC aliases M102 to M101
            by_m.setdefault(int(t["M"]), t)
    by_m[102] = next(t for t in tables if t["Name"] == "NGC5866")   # Spindle Galaxy
    rows = []
    for n in range(1, 111):
        t = by_m[n]
        common = (t["Common names"] or "").split(",")[0].strip()
        name = NAME_FIX.get(n) or common or pretty_designation(t["Name"])
        mag = t["V-Mag"] or t["B-Mag"] or MAG_FIX.get(n, "")
        rows.append({
            "key": f"M{n:03d}", "label": f"M{n}", "name": name,
            "type": TYPE_NAMES.get(t["Type"], t["Type"]), "con": t["Const"],
            "mag": f"{float(mag):.1f}" if mag else "",
            "size": f"{float(t['MajAx']):.1f}" if t["MajAx"] else "",
            "ra": f"{hms_to_h(t['RA']):.6f}", "dec": f"{dms_to_d(t['Dec']):.5f}",
        })
    return rows


def sesame(ident):
    for attempt in range(4):
        try:
            r = requests.get(SESAME + requests.utils.quote(ident), timeout=60)
            r.raise_for_status()
            break
        except requests.RequestException as e:
            if attempt == 3:
                raise
            print(f"  retry {ident}: {e}")
            time.sleep(5 * (attempt + 1))
    root = ElementTree.fromstring(r.text)
    res = root.find(".//Resolver")
    if res is None or res.find("jradeg") is None:
        raise RuntimeError(f"SIMBAD could not resolve {ident!r}")
    ra, dec = float(res.findtext("jradeg")), float(res.findtext("jdedeg"))
    pm_ra = float(res.findtext("pm/pmRA") or 0)     # mas/yr, includes cos(dec)
    pm_de = float(res.findtext("pm/pmDE") or 0)
    yrs = EPOCH_YEAR - 2000.0
    dec2 = dec + pm_de * yrs / 3.6e6
    ra2 = ra + pm_ra * yrs / 3.6e6 / max(math.cos(math.radians(dec)), 1e-6)
    return (ra2 % 360) / 15.0, dec2


def star_rows():
    rows = []
    for line in STARS.read_text(encoding="utf8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        ident, name, mag = (x.strip() for x in line.split(";"))
        ra_h, dec_d = sesame(ident)
        rows.append({
            "key": f"S{len(rows) + 1:03d}", "label": name, "name": ident.lstrip("* "),
            "type": "Star", "con": ident.split()[-1], "mag": mag, "size": "",
            "ra": f"{ra_h:.6f}", "dec": f"{dec_d:.5f}",
        })
        time.sleep(0.2)
    return rows


def planet_rows():
    return [{"key": k, "label": l, "name": n, "type": t, "con": "", "mag": m,
             "size": "", "ra": "", "dec": ""} for k, l, n, t, m, _ in PLANETS]


def refresh_catalog():
    rows = messier_rows() + star_rows() + planet_rows()
    with CATALOG.open("w", encoding="ascii", newline="\n") as f:
        f.write(HEADER + "\n")
        for r in rows:
            f.write(",".join(r[k].replace(",", " ") for k in
                             ("key", "label", "name", "type", "con", "mag", "size", "ra", "dec"))
                    + "\n")
    print(f"[catalog] wrote {len(rows)} objects -> {CATALOG.relative_to(ROOT)}")


def load_catalog():
    keys = ("key", "label", "name", "type", "con", "mag", "size", "ra", "dec")
    lines = [l for l in CATALOG.read_text(encoding="ascii").splitlines()
             if l and not l.startswith("#")]
    return [dict(zip(keys, l.split(","))) for l in lines]


# ---------------------------------------------------------------- images
def fov_deg(obj):
    if obj["type"] == "Star":
        return STAR_FOV_DEG
    size = float(obj["size"]) if obj["size"] else 20.0
    return min(max(size * 2.0 / 60.0, 0.25), 4.0)


def fetch_image(obj):
    jpg = CACHE / (obj["key"] + ".jpg")
    if not jpg.exists() and obj["type"] in SOLAR:
        name = next(p[5] for p in PLANETS if p[0] == obj["key"])
        r = requests.get(COMMONS + name, params={"width": 240},
                         headers={"User-Agent": USER_AGENT}, timeout=90)
        r.raise_for_status()
        jpg.write_bytes(r.content)
        time.sleep(0.5)
    if not jpg.exists():
        params = {"hips": HIPS, "width": FETCH_PX, "height": FETCH_PX, "projection": "TAN",
                  "coordsys": "icrs", "fov": f"{fov_deg(obj):.4f}",
                  "ra": f"{float(obj['ra']) * 15:.6f}", "dec": obj["dec"], "format": "jpg"}
        for attempt in range(4):
            try:
                r = requests.get(HIPS2FITS, params=params, timeout=90)
                r.raise_for_status()
                jpg.write_bytes(r.content)
                break
            except requests.RequestException as e:
                if attempt == 3:
                    raise
                print(f"  retry {obj['key']}: {e}")
                time.sleep(3 * (attempt + 1))
        time.sleep(0.3)                               # be polite to CDS
    return jpg


def to_rgb565(jpg, solar=False):
    img = Image.open(jpg).convert("RGB")
    if solar:                         # keep true colours, letterbox (Saturn's rings)
        img = ImageOps.pad(img, (IMG_W, IMG_H), Image.LANCZOS, color=(0, 0, 0))
    else:
        img = img.resize((IMG_W, IMG_H), Image.LANCZOS)
        img = ImageOps.autocontrast(img, cutoff=0.5)
    out = bytearray(b"R565" + struct.pack("<HH", IMG_W, IMG_H))
    px = img.tobytes()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        out += struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return bytes(out)


# ---------------------------------------------------------------- object info
SENSOR_MM   = 23.5                  # APS-C width, the reference camera for framing
EMISSION    = {"Nebula", "SN remnant", "Cl+Nebula", "Planetary"}
REFLECTION  = {"M045", "M078"}       # blue dust: broadband only


def scope_for(obj):
    if obj["type"] == "Moon":
        return "Any scope; ~600-1000mm frames the whole disc on APS-C"
    if obj["type"] == "Planet":
        return "127-200mm Mak/SCT at f/15-f/25 (2-3x Barlow)"
    if obj["type"] in ("Star", "Double"):
        return "Any scope; 80-150mm aperture at high power for detail"
    size = float(obj["size"]) if obj["size"] else 10.0
    fov = max(size * 1.6 / 60.0, 0.05)                       # object fills ~60%
    fl = SENSOR_MM / (2 * math.tan(math.radians(fov) / 2))
    if fl < 200:
        return f"~{fl:.0f}mm: camera lens or 50-60mm refractor"
    if fl < 450:
        return f"~{fl:.0f}mm: 60-72mm refractor"
    if fl < 800:
        return f"~{fl:.0f}mm: 80mm refractor / 130mm reflector"
    if fl < 1300:
        return f"~{fl:.0f}mm: 150mm reflector / 127mm Mak"
    return f"~{min(fl, 2500):.0f}mm: 127-150mm Mak/SCT (+Barlow)"


def camera_for(obj):
    if obj["type"] in SOLAR:
        return "Planetary cam (IMX462/585), ADC for low planets"
    if obj["key"] in REFLECTION:
        return "Colour OSC + UV/IR cut (no narrowband)"
    if obj["type"] in EMISSION:
        return "Colour OSC + dual-band Ha/OIII filter"
    if obj["type"] in ("Star", "Double"):
        return "Planetary cam or DSLR, short frames"
    return "Colour OSC (e.g. IMX533/585) + UV/IR cut"


def exposure_for(obj):
    if obj["type"] == "Moon":
        return "1-5 ms frames, stack best 20%; mosaic at long FL"
    if obj["type"] == "Planet":
        return "60-90s video at high fps, stack best 10%"
    if obj["type"] in ("Star", "Double"):
        return "0.1-2s frames, stack the best"
    mag = float(obj["mag"]) if obj["mag"] else 9.0
    if mag < 5:
        return "30-60s subs, 1h+ total"
    if mag < 8:
        return "60-120s subs, 2h+ total (guided)"
    return "120-180s subs, 3h+ total (guided)"


def info_text(obj, descs):
    lines = [descs.get(obj["key"], obj["name"]),
             "Scope: " + scope_for(obj), "Camera: " + camera_for(obj),
             "Exposure: " + exposure_for(obj)]
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def load_descriptions():
    out = {}
    for line in DESCS.read_text(encoding="ascii").splitlines():
        if line.strip() and not line.startswith("#"):
            k, _, t = line.partition("|")
            out[k.strip()] = t.strip()
    return out


# ---------------------------------------------------------------- card folder
def card_files(objs):
    stamp = datetime.date.today().isoformat()
    files = {
        "SMARTEQ/CATALOG.CSV": CATALOG.read_bytes(),
        "SMARTEQ/README.TXT": (
            f"SmartEQ sky database ({len(objs)} objects, built {stamp})\r\n\r\n"
            "Catalog: OpenNGC by Mattia Verga (CC-BY-SA-4.0) and SIMBAD, CDS Strasbourg.\r\n"
            "Images: DSS2 colour via CDS hips2fits. The Digitized Sky Surveys were\r\n"
            "produced at the Space Telescope Science Institute under U.S. Government\r\n"
            "grant NAG W-2166. Non-commercial use with acknowledgement.\r\n"
            "Planet images: NASA / JPL / ESA (public domain) via Wikimedia Commons;\r\n"
            "Moon: Gregory H. Revera, CC-BY-SA-3.0.\r\n").encode(),
    }
    descs = load_descriptions()
    missing = [o["key"] for o in objs if o["key"] not in descs]
    if missing:
        print("[info] no description for:", " ".join(missing))
    for i, o in enumerate(objs, 1):
        files[f"SMARTEQ/INFO/{o['key']}.TXT"] = info_text(o, descs)
        print(f"\r[images] {i}/{len(objs)} {o['key']}   ", end="", flush=True)
        files[f"SMARTEQ/IMG/{o['key']}.565"] = to_rgb565(fetch_image(o), o["type"] in SOLAR)
    print()
    return files


# ---------------------------------------------------------------- FAT32 image
SEC = 512


def _83(name):
    base, _, ext = name.upper().partition(".")
    assert 1 <= len(base) <= 8 and len(ext) <= 3, f"not an 8.3 name: {name}"
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def _dirent(name11, attr, clus, size, dt):
    t = (dt.hour << 11) | (dt.minute << 5) | (dt.second // 2)
    d = ((dt.year - 1980) << 9) | (dt.month << 5) | dt.day
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, t, d, d,
                       clus >> 16, t, d, clus & 0xFFFF, size)


def build_fat32_image(files, size_mb=IMG_MB, label="SMARTEQ"):
    """MBR + one FAT32 partition (type 0x0C) holding `files` ({'A/B.EXT': bytes})."""
    total = size_mb * 2048
    part_lba, part_secs = 2048, size_mb * 2048 - 2048
    rsvd, nfats, spc = 32, 2, 1
    fatsz = 1
    while True:
        clusters = (part_secs - rsvd - nfats * fatsz) // spc
        need = -(-(clusters + 2) * 4 // SEC)
        if need == fatsz:
            break
        fatsz = need
    assert clusters >= 65525, "too few clusters for FAT32"
    data_lba = rsvd + nfats * fatsz
    csize = spc * SEC
    now = datetime.datetime.now()

    # directory tree: {'': {...}, 'SMARTEQ': {...}, ...}
    tree = {"": []}
    for path in sorted(files):
        parts = path.split("/")
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in tree:
                tree[d] = []
                tree["/".join(parts[:i - 1])].append((parts[i - 1], "dir", d))
        tree["/".join(parts[:-1])].append((parts[-1], "file", path))

    def dir_bytes_len(d):
        n = len(tree[d]) + (1 if d == "" else 2)            # label / '.' + '..'
        return max(n * 32, csize)

    # allocate clusters: dirs first (root = cluster 2), then files
    alloc, nxt = {}, 2
    order = [("dir", d) for d in sorted(tree, key=lambda x: (x.count("/"), x))] + \
            [("file", f) for f in sorted(files)]
    for kind, key in order:
        n = dir_bytes_len(key) if kind == "dir" else len(files[key])
        nclus = -(-n // csize) if n else 0
        alloc[(kind, key)] = (nxt if nclus else 0, nclus)
        nxt += nclus
    assert nxt - 2 <= clusters, "files do not fit in the image"

    img = bytearray(total * SEC)
    part = part_lba * SEC

    def write_clusters(first, data):
        off = part + (data_lba + (first - 2) * spc) * SEC
        img[off:off + len(data)] = data

    fat = bytearray(fatsz * SEC)
    struct.pack_into("<II", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF)
    for first, n in alloc.values():
        for c in range(first, first + n):
            struct.pack_into("<I", fat, c * 4, c + 1 if c < first + n - 1 else 0x0FFFFFFF)

    for d, entries in tree.items():
        first, _ = alloc[("dir", d)]
        buf = bytearray()
        if d == "":
            buf += _dirent(label.upper().ljust(11)[:11].encode(), 0x08, 0, 0, now)
        else:
            parent = d.rpartition("/")[0]
            pclus = alloc[("dir", parent)][0] if parent else 0
            buf += _dirent(b".          ", 0x10, first, 0, now)
            buf += _dirent(b"..         ", 0x10, pclus, 0, now)
        for name, kind, key in entries:
            c, _ = alloc[(kind, key)]
            if kind == "dir":
                buf += _dirent(_83(name), 0x10, c, 0, now)
            else:
                buf += _dirent(_83(name), 0x20, c, len(files[key]), now)
        write_clusters(first, buf)
    for path, data in files.items():
        first, n = alloc[("file", path)]
        if n:
            write_clusters(first, data)

    # boot sector + FSInfo (+ backups at 6/7)
    bs = bytearray(SEC)
    bs[0:3] = b"\xEB\x58\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBHHHII", bs, 11, SEC, spc, rsvd, nfats, 0, 0, 0xF8, 0,
                     63, 255, part_lba, part_secs)
    struct.pack_into("<IHHIHH", bs, 36, fatsz, 0, 0, 2, 1, 6)
    struct.pack_into("<BBBI", bs, 64, 0x80, 0, 0x29, int(time.time()) & 0xFFFFFFFF)
    bs[71:82] = label.upper().ljust(11)[:11].encode()
    bs[82:90] = b"FAT32   "
    bs[510:512] = b"\x55\xAA"
    fsi = bytearray(SEC)
    struct.pack_into("<I", fsi, 0, 0x41615252)
    struct.pack_into("<III", fsi, 484, 0x61417272, clusters - (nxt - 2), nxt)
    struct.pack_into("<I", fsi, 508, 0xAA550000)
    for base in (0, 6):
        img[part + base * SEC: part + (base + 1) * SEC] = bs
        img[part + (base + 1) * SEC: part + (base + 2) * SEC] = fsi
    for i in range(nfats):
        off = part + (rsvd + i * fatsz) * SEC
        img[off:off + len(fat)] = fat

    # MBR
    mbr = bytearray(SEC)
    struct.pack_into("<I", mbr, 440, 0x534D5145)                 # disk id 'SMQE'
    struct.pack_into("<B3sB3sII", mbr, 446, 0x00, b"\xFE\xFF\xFF", 0x0C,
                     b"\xFE\xFF\xFF", part_lba, part_secs)
    mbr[510:512] = b"\x55\xAA"
    img[0:SEC] = mbr
    print(f"[fat32] {size_mb} MB, {clusters} clusters, {nxt - 2} used "
          f"({(nxt - 2) * csize // 1024} KB)")
    return bytes(img)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--refresh-catalog", action="store_true",
                    help="re-download OpenNGC / SIMBAD data into catalog.csv")
    args = ap.parse_args()

    CACHE.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if args.refresh_catalog or not CATALOG.exists():
        refresh_catalog()
    objs = load_catalog()
    files = card_files(objs)

    for path, data in files.items():                  # plain folder for inspection
        p = CARD / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    with zipfile.ZipFile(OUT_DIR / "smarteq-sd-files.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for path, data in sorted(files.items()):
            z.writestr(path, data)
    img = build_fat32_image(files)
    with zipfile.ZipFile(OUT_DIR / "smarteq-sd.img.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("smarteq-sd.img", img)
    for f in ("smarteq-sd.img.zip", "smarteq-sd-files.zip"):
        print(f"[out] docs/sdcard/{f}  {(OUT_DIR / f).stat().st_size // 1024} KB")


if __name__ == "__main__":
    sys.exit(main())
