# ESP32 iOptron SmartEQ Controller

Firmware for the **ESP32-WROOM-32** *RJ9 Adapter* board. It turns an iOptron
Go2Nova / SmartEQ-class mount into a wireless target:

* **Classic Bluetooth "serial port" (SPP)** ⇄ **RS-232 / RJ9** — every byte is
  forwarded unmodified, both directions, so a phone / PC / planetarium app
  (iOptron Commander, ASCOM, INDI, SkySafari …) drives the mount over Bluetooth.
* **Status TFT** — shows what the mount reports about itself: model, firmware,
  time, UTC offset / DST, site lon/lat, RA/Dec, tracking & guide rates, status.
* **Automatic GPS time + site sync** — an on-board NEO-6M feeds date/time and
  longitude/latitude straight into the mount once it holds a fix. No buttons.
* **LED2 = mount-link indicator** — lit whenever the mount answers on the RJ9
  line, off within ~2.5 s of the cable being pulled or the mount powered down.

```
 BT SPP client ──RFCOMM──► ESP32 UART2 (GPIO17 TX / GPIO16 RX) ──► MAX3232 ──► RJ9 ──► mount
   "SmartEQ-RJ9"            byte-for-byte copy, both directions     (RS-232)    RJ1
 NEO-6M GPS  TX ──► GPIO32 (UART1 RX)          TFT: ST7789 320×240, VSPI 18/23/19, CS25 DC26 RST27
```

Bridge + polling run at **9600 8-N-1** (`BRIDGE_BAUD`), the iOptron **legacy
command set**. Verified end-to-end against an **iOptron SmartEQ Pro**
(`:MountInfo#` → `0011`, firmware `190422`, motors `161028`): queries,
`:SG/:SDS/:SC/:SL` time-set and `:Sg/:St` site-set all confirmed.

## Behaviour

| State | Bridge | Poll / GPS sync | TFT header |
|---|---|---|---|
| **No BT client** | idle | polls the mount every ~0.4 s; auto-syncs from GPS when a fix is stable for 6 s, then every 3 h | `MOUNT OK` / `NO MOUNT` |
| **BT client connected** | fully transparent | **stopped** — screen holds last values, GPS sync deferred until disconnect | `BRIDGE` |

The GPS sync keeps the mount's own **UTC offset + DST** and pushes local date,
local time, longitude and latitude (`:SC`, `:SL`, `:Sg`, `:St`).

## Pin map (from the schematic)

| Function | ESP32 | Net | Goes to |
|---|---|---|---|
| RS-232 TX | GPIO17 | `TX2` | MAX3232 T1IN → RJ1 pin 4 |
| RS-232 RX | GPIO16 | `RX2` | MAX3232 R1OUT ← RJ1 pin 3 |
| GPS RX (NMEA in) | GPIO32 | `IO32` | NEO-6M TX (receive-only) |
| TFT SCLK / MOSI / MISO | 18 / 23 / 19 | `SPI_*` | ST7789 (VSPI) |
| TFT CS / DC / RST | 25 / 26 / 27 | `TFT_*` | ST7789 |
| LED2 (mount link) | GPIO2 | `IO2` | 470 Ω → LED → GND |
| BTN B (select / back) | GPIO39 | — | tactile switch to GND, ext. 10 kΩ pull-up |
| BTN A (next / scroll) | GPIO36 | — | tactile switch to GND, ext. 10 kΩ pull-up |
| SD card (on LCD module) | 33 | `SD_CS` | shares VSPI 18/23/19 with the TFT |
| TOUCH_CS | 13 | — | parked HIGH at boot (unused) |
| Debug log | GPIO1/3 | `TX`/`RX` | CH340G / USB-C — **log only**, 115200 |

`-DDEBUG_LOG=1` prints one `[state] …` line every 3 s. Build knobs live in
`platformio.ini` `build_flags` (`TFT_HZ`, `TFT_INVERT`, `BT_NAME`, pin numbers).

## ⚠️ rev 1.0 hardware notes

1. **RJ9 TX/RX swapped.** `RJ1` pin 3 / pin 4 are backwards for a straight
   iOptron cable — the board transmits on pin 4, receives on pin 3; an iOptron
   port is the opposite. Rework `RJ1` (pin 3 → MAX3232 T1OUT, pin 4 → R1IN) and
   fix the `TXD`/`RXD` net labels, or use a cable that **swaps only pins 3↔4**
   (pin 1 GND straight — *not* a plain reversed handset cord, which also swaps
   1↔4 and shorts TX to GND).
2. **MAX3232 VCC is on +5 V**, so `R1OUT` drives GPIO16 with ~5 V highs, over the
   ESP32's 3.6 V abs-max. Move U3 VCC to `3v3` (MAX3232ESE runs 3–5.5 V).
3. **TFT panel** is silk-screened "ILI9341" but is an **ST7789 240×320**; the
   firmware drives it as such (`Adafruit_ST7789`, landscape 320×240).
4. **Buttons** (BTN1/BTN2, GPIO39/36) may be unpopulated on early boards — GPS
   sync is fully automatic and needs no button. BTN1 is only an optional manual
   re-sync trigger.

## Sky catalog & GoTo (SD card)

Insert the SD card into the LCD module's slot and the device gains an object
browser: pick a Messier object or a bright star, read what it is and how to
image it, press **TRACK**, and the mount slews there and keeps tracking.
Planets and the Moon are included — their positions are computed on the device.

**Prepare the card** (any card ≥ 64 MB, e.g. 8 GB) — one of:

* Flash [`docs/sdcard/smarteq-sd.img.zip`](docs/sdcard/smarteq-sd.img.zip)
  with [balenaEtcher](https://etcher.balena.io/) or Rufus (they take the `.zip`
  directly). This writes a 64 MB FAT32 partition — the rest of the card stays
  unused.
* Or format the card FAT32 and unzip
  [`docs/sdcard/smarteq-sd-files.zip`](docs/sdcard/smarteq-sd-files.zip) onto it
  (keeps the full card capacity).

The card is detected at boot and when inserted later (the status page shows an
**SD** badge). It contains 110 Messier objects, 60 bright stars, and the
planets Mercury–Neptune plus the Moon (the Sun is deliberately absent):

```
/SMARTEQ/CATALOG.CSV     key,label,name,type,con,mag,size',RA(h),Dec(°)  J2000
                         (P-keys: planets/Moon, RA/Dec empty - computed live)
/SMARTEQ/IMG/<key>.565   96×96 DSS2 thumbnail, raw RGB565
/SMARTEQ/INFO/<key>.TXT  what it is / Scope / Camera / Exposure
```

**Buttons** — single press navigates, double press selects: **A** (GPIO36) =
next (hold = fast scroll), **A twice** = select / open / TRACK, **B** (GPIO39) =
back. Status page: A opens the catalog; *Sync GPS now* is in the menu.

**Object page** — left: thumbnail + name; right: object info with a suggested
scope (focal length to frame it on an APS-C sensor), camera/filter and exposure,
and the **TRACK** button. TRACK is enabled only when the object is ≥ 10° above
the horizon **and at least 15° from the Sun** (needs time + site from GPS or
the mount). A twice arms it
(*CONFIRM?*), A twice again starts the GoTo (B cancels): J2000 → JNow precession on the device (planets / Moon: on-device ephemeris,
Moon with topocentric parallax), then
`:ST1#` `:Sr` `:Sd` `:MS#` (the same sequence iOptron Commander uses). Any
button while slewing = **STOP** (`:q#`). GoTo is disabled while a Bluetooth
client is connected (the bridge stays transparent).

**Rebuild the card content** (Python 3.10+):

```bash
python -m venv tools/sdcard/.venv
tools/sdcard/.venv/Scripts/pip install -r tools/sdcard/requirements.txt   # bin/ on Linux/macOS
tools/sdcard/.venv/Scripts/python tools/sdcard/build_sd.py                # images are cached in build/
tools/sdcard/.venv/Scripts/python tools/sdcard/build_sd.py --refresh-catalog
```

Edit [`tools/sdcard/descriptions.txt`](tools/sdcard/descriptions.txt) for the
object texts and [`tools/sdcard/stars.txt`](tools/sdcard/stars.txt) for the star
list; scope/camera/exposure advice is derived from size, type and magnitude in
[`build_sd.py`](tools/sdcard/build_sd.py).

Credits: planet images NASA/JPL/ESA (public domain), Moon by Gregory H. Revera
(CC-BY-SA-3.0), via Wikimedia Commons; catalog from [OpenNGC](https://github.com/mattiaverga/OpenNGC)
(CC-BY-SA-4.0) and SIMBAD (CDS, Strasbourg); images from the Digitized Sky Survey
(DSS2, STScI) via CDS hips2fits — non-commercial use with acknowledgement.

## Flash from the browser

No toolchain needed — open the web flasher in **desktop Chrome or Edge**, plug
the board in over USB-C, and click *Connect & Flash*:

> **https://aviralverma-8877.github.io/esp32-ioptron_smart_eq_controller/**

It lives in [`docs/`](docs/) (GitHub Pages, `main` branch → `/docs`) and uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/). Every `pio run`
regenerates the single-image `docs/firmware/smarteq-rj9-merged.bin` (flash to
`0x0`) via [`scripts/merge_bin.py`](scripts/merge_bin.py) and bumps
`docs/manifest.json`.

## Build & flash locally

```bash
pio run                       # build (huge_app.csv partition) + refresh docs/firmware
pio run -t upload             # flash over USB-C (CH340 auto-reset)
pio device monitor            # watch [state] lines @ 115200
```

## Pairing

1. Host → **Add Bluetooth device → "SmartEQ-RJ9"** (Just-Works, no PIN). The OS
   assigns an outgoing serial COM port.
2. Open it at **9600 8-N-1**.
3. Re-flashing changes the BT bond — if the host can't reconnect, remove the
   pairing and re-add it.

## iOptron replies decoded (SmartEQ Pro, protocol V1.00)

| Query | Reply | Meaning |
|---|---|---|
| `:MountInfo#` | `0011` (no `#`) | model code |
| `:V#` | `V1.00#` | protocol version |
| `:FW1#` / `:FW2#` | `YYMMDDYYMMDD#` | main+HC / RA+Dec motor firmware dates |
| `:GLT#` | `sMMM d YYMMDD HHMMSS#` | UTC offset min, DST, local date, local time |
| `:GLS#` | `s lon(6″) lat(6″) <GAS 6>#` | site longitude / latitude, arc-seconds |
| `:GAS#` | `G S T M X H#` | GPS, status, track-rate, slew-rate, time-src, hemisphere |
| `:GEC#` | `sDDDDDDDD RRRRRRRR#` | Dec in 0.01″, RA in **ms** |
| `:AG#` | `gggg#` | RA / Dec guide rate ×0.01 |

Setters used by the GPS sync: `:SG±MMM#` (kept), `:SDS0/1#` (kept), `:SCYYMMDD#`,
`:SLHHMMSS#`, `:Sg±TTTTTTTT#`, `:St±TTTTTTTT#` (0.01″) — each returns `1` on OK.

GoTo (verified against iOptron Commander on this mount): `:ST1#` tracking on,
`:SrRRRRRRRR#` RA in ms, `:Sd±DDDDDDDD#` Dec in 0.01″, `:MS#` slew (`1` OK /
`0` below horizon or limits), `:q#` stop — all reply `1`.

## Source layout

| File | Job |
|---|---|
| `src/config.h` | build-time config (pins/bauds), `LOG` macro, TFT colours |
| `src/message.*` | transient footer status line (`setMsg`) |
| `src/mount.*` | RS-232 link: `askMount`, iOptron reply decoders, idle poll loop, link tracking |
| `src/bt_bridge.*` | `BluetoothSerial` SPP endpoint + transparent byte pumping |
| `src/gps.*` | NEO-6M UART + TinyGPS++ + lock / status |
| `src/gps_sync.*` | automatic + manual (menu) GPS→mount time/site sync |
| `src/display.*` | ST7789 + status page (changed-field redraw) |
| `src/buttons.*` | 10 ms sampled buttons → next / hold / repeat / select / back events |
| `src/sky.*` | SD hot-plug, catalog, thumbnails, info text, precession + alt/az |
| `src/ephem.*` | planets + Moon positions (JPL elements / Meeus), Sun for the 15° safety guard |
| `src/ui.*` | screens: status → menu → object list → object page / TRACK |
| `tools/sdcard/` | builds the SD content + `docs/sdcard/*.zip` |
| `src/main.cpp` | `setup()` / `loop()` wiring only |
| `scripts/merge_bin.py` | post-build: merged image + manifest for the web flasher |
| `docs/` | GitHub Pages web flasher (`index.html`, `manifest.json`, `firmware/`) |

## Notes

- Stateless bridge, no flow control (RTS/CTS/DTR not wired on this board).
- `pump()` is bounded by `available()`, so `loop()` never blocks on the bridge.
- Buttons are sampled by a 10 ms `esp_timer` into a queue, so a poll-blocked `loop()` can't miss a press (no GPIO36/39 interrupts - ESP32 erratum).
- TFT redraws only changed fields — no flicker.
- `huge_app.csv` partition (3 MB app, no OTA) — Bluedroid SPP is large.
