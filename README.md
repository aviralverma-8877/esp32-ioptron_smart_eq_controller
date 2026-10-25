# ESP32 iOptron SmartEQ Controller

Firmware for the **ESP32-WROOM-32** *RJ9 Adapter* board. It turns an iOptron
Go2Nova / SmartEQ-class mount into a wireless target:

* **Classic Bluetooth "serial port" (SPP)** ⇄ **RS-232 / RJ9** — every byte is
  forwarded unmodified, both directions, so a phone / PC / planetarium app
  (iOptron Commander, ASCOM …) drives the mount over Bluetooth. Like a plain
  serial Bluetooth module it is **always visible** and pairs without a code —
  nothing to do on the controller.
* **Optional LX200 / NexStar translation** — switch the Bluetooth side to
  **Meade LX200** or **Celestron NexStar** and apps with those drivers built in
  (Stellarium, SkySafari, INDI/KStars …) connect directly, no ASCOM or
  Commander needed. See [Protocol translation](#protocol-translation-lx200--nexstar).
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
`:SG/:SDS/:SC/:SL` time-set and `:Sg/:St` site-set all confirmed, and
Bluetooth GoTo + tracking verified from Stellarium desktop (directly, Protocol
NexStar, and via ASCOM → iOptron Commander) and Stellarium Mobile on Android.

## Behaviour

| State | Bridge | Poll / GPS sync | TFT header |
|---|---|---|---|
| **No BT client** | idle | polls the mount every ~0.4 s; auto-syncs from GPS when a fix is stable for 6 s, then every 3 h | `MOUNT OK` / `NO MOUNT` |
| **BT client connected** | fully transparent (Protocol iOptron) or translated (LX200 / NexStar) | **stopped** — screen holds last values. GPS sync: deferred until disconnect (iOptron), runs between client commands (LX200 / NexStar) | `BRIDGE` (+ `LX200` / `NexStar`) |

The GPS sync first writes the **time zone** (`:SG`, from the *Time zone* menu
setting — *Auto* derives it from the GPS position) with **DST off** (`:SDS0`),
then local date, local time, longitude and latitude (`:SC`, `:SL`, `:Sg`, `:St`),
so the mount's internal UTC is always exact. It then logs the mount's read-back
(`:GLT#`, `:GLS#`, `:GAC#`). A GPS fix that drops in and out does **not** re-sync
(re-writing time/site while tracking upsets the mount); only the 3-hour timer, a
time-zone change or *Sync GPS now* do. The sync waits for the mount to answer
(*fix ok - waiting for mount*), and a failed one is retried after a minute.

The NEO-6M needs sky view (≥ 4 satellites). Without a fix (indoors, cold
start), a planetarium app connected in LX200 / NexStar mode can set the time
and site instead - see [Protocol translation](#protocol-translation-lx200--nexstar).

## Pin map (from the schematic)

| Function | ESP32 | Net | Goes to |
|---|---|---|---|
| RS-232 TX | GPIO17 | `TX2` | MAX3232 T1IN → RJ1 pin 4 |
| RS-232 RX | GPIO16 | `RX2` | MAX3232 R1OUT ← RJ1 pin 3 |
| GPS RX (NMEA in) | GPIO32 | `IO32` | NEO-6M TX (receive-only) |
| TFT SCLK / MOSI / MISO | 18 / 23 / 19 | `SPI_*` | ST7789 (VSPI) |
| TFT CS / DC / RST | 25 / 26 / 27 | `TFT_*` | ST7789 |
| LED2 (mount link) | GPIO2 | `IO2` | 470 Ω → LED → GND |
| BTN A (up) | GPIO36 | — | tactile switch to GND, ext. 10 kΩ pull-up |
| BTN B (down) | GPIO39 | — | tactile switch to GND, ext. 10 kΩ pull-up |
| BOOT (select / hold: back) | GPIO0 | — | on-board BOOT switch; don't hold it while resetting |
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

**Buttons** — **A** (GPIO36) = up, **B** (GPIO39) = down (hold either = fast
scroll), **BOOT** (GPIO0) = select / open / TRACK, **hold BOOT** = back. Status
page: any button opens the menu (it opens without an SD card too; only the
catalog rows need one):

| Menu item | What it does |
|---|---|
| Messier objects / Bright stars / Planets & Moon | catalog lists (SD card) |
| Sync GPS now | push GPS time zone, date, time and site into the mount |
| Stop mount | `:q#` |
| Go to zero position | `:MH#` — select twice to confirm |
| Time zone | BOOT to edit, A/B = ±15 min or **Auto** (from GPS position), BOOT saves (NVS) and re-syncs, hold BOOT cancels |
| Bluetooth (N paired) | **Protocol** (iOptron / LX200 / NexStar), list / forget paired devices — see [Pairing](#pairing) |
| Factory reset | select twice: forgets all paired Bluetooth devices, clears saved settings, reboots (the mount's own settings are untouched) |
| Debug log | live log output on the screen (A / B scroll back and forth, BOOT = live, hold BOOT = back) — see [Debug console](#debug-console-usb-115200-baud) |

**Object page** — left: thumbnail + name; right: object info with a suggested
scope (focal length to frame it on an APS-C sensor), camera/filter and exposure,
and the **TRACK** button. TRACK is enabled only when the object is ≥ 10° above
the horizon **and at least 15° from the Sun** (needs time + site from GPS or
the mount). BOOT arms it
(*CONFIRM?*), BOOT again starts the GoTo (A / B / hold BOOT cancel): J2000 → JNow precession on the device (planets / Moon: on-device ephemeris,
Moon with topocentric parallax), then
`:ST1#` `:Sr` `:Sd` `:MS#` (the same sequence iOptron Commander uses). Any
button while slewing = **STOP** (`:q#`). Leaving the page and coming back to the
object the mount is tracking shows **TRACKING** again (falls back to TRACK if
tracking is stopped elsewhere). GoTo is disabled while a Bluetooth client is
connected (the app owns the mount then).

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

The controller works like a plain serial Bluetooth module: it is **always
visible** as **"SmartEQ-RJ9"**, pairs with *Just Works* (no code, nothing to
press on the controller) and accepts any device. Anyone within Bluetooth range
can therefore connect and move the mount. Only one device is connected at a time.
Pairings live in the ESP32's NVS, so they survive reboots and re-flashing (but
not *Factory reset* or an *Erase device* flash).

1. **Android:** in **Stellarium Mobile**, tap **Select** in the telescope panel,
   wait for the scan, pick SmartEQ-RJ9 and tap **Pair** — always pair from a
   fresh scan (see below). Other apps: add Bluetooth device SmartEQ-RJ9 in
   Android's Bluetooth settings.
2. **Windows:** *Settings → Bluetooth & devices → Add device → Bluetooth →
   SmartEQ-RJ9* — "Your device is ready to go", no code. Then note the
   **Outgoing** COM port (below).
3. Open that serial port at **9600 8-N-1**.

The Bluetooth screen lists the paired devices; select one twice to forget it, or
use *Forget all devices*.

**"Connection to service failed" on Android?** Android shows its one-tap *Pair*
prompt on screen only for a device it has just found in a scan; otherwise the
prompt hides in a notification and the app gives up. This happens when the phone
still remembers a pairing the controller no longer has (after *Factory reset*,
an erase-flash or *Forget*). Tap **Select** in Stellarium to scan, pick
SmartEQ-RJ9 and tap **Pair**.

**Windows creates two COM ports** per pairing. Use the **Outgoing** one (its
device ID contains the controller's Bluetooth address; *Settings → Bluetooth &
devices → More Bluetooth settings → COM Ports*). The **Incoming** port blocks
forever when opened — pointing iOptron Commander at it makes Commander hang on
start, and Stellarium freezes ("Not Responding"); you can remove the incoming
port there. **The numbers can change every time you pair again** (e.g. Outgoing
COM16 one time, COM8 the next), so check this tab after re-pairing and update
the port in Stellarium / Commander.

### Stellarium (Windows, via ASCOM - Protocol iOptron)

Chain: Stellarium → ASCOM *iOptron CEM60, iEQ45Pro/30Pro, CEM25, SmartEQ Pro+…*
driver (`ASCOM.iOptron2014.Telescope`) → iOptron Commander 2014 → Bluetooth COM
port → controller → mount.

1. Pair the PC (above) and note the **Outgoing** COM port.
2. Start iOptron Commander once, choose that COM port, connect, close it.
3. Stellarium → Telescope Control (Ctrl+0) → add a telescope: **ASCOM**,
   device *iOptron CEM60…SmartEQ Pro+…*, equinox **JNow**, *connect at startup*.
4. Select an object → **Ctrl+1** GoTo · **Ctrl+3** sync · **Ctrl+5** abort.

Tips:

- After **Go to zero position** the mount stops tracking, and Stellarium's GoTo
  does not switch it back on — the mount may then sit in "slewing" state and
  drift at sidereal rate. Abort (Ctrl+5 / *Stop*) and turn tracking on in
  Commander, or GoTo from the controller's own TRACK button (it sends `:ST1#`).
- If the **controller restarts** (power, re-flash) while connected, Windows does
  not re-open the Bluetooth link by itself and Commander keeps polling a dead
  port. Close Stellarium **and** Commander, then start Stellarium again. If
  Commander shows "Cannot find any supported iOptron devices!", the first
  Bluetooth connection took longer than its probe — just press OK / Connect again.
- Quitting Stellarium can send the mount to its zero position (`:MH#`).

## Protocol translation (LX200 / NexStar)

The mount speaks the iOptron command language, which Stellarium, SkySafari and
most other apps don't have a built-in driver for. The controller can translate:
set **Menu → Bluetooth → Protocol** (BOOT cycles it; saved, survives reboots):

| Protocol | Bluetooth client speaks | Use with |
|---|---|---|
| **iOptron** (default) | iOptron, byte-transparent | iOptron Commander, ASCOM iOptron driver |
| **LX200** | Meade LX200 / Autostar | Stellarium *Meade LX200 (compatible)*, SkySafari *Meade LX-200 Classic*, INDI *LX200 Basic*, ASCOM *Meade Generic* |
| **NexStar** | Celestron NexStar hand control | Stellarium *Celestron NexStar (compatible)*, SkySafari *Celestron NexStar/Advanced GT*, INDI *Celestron NexStar* |

The status header shows the active protocol (`MOUNT OK  LX200`). The
on-device menu, catalog, GPS sync and TRACK work the same in every mode.

**Stellarium, direct (Windows / Linux / macOS):** pair (above), then Telescope
Control (Ctrl+0) → *Add* → *Telescope controlled by: Stellarium, directly
through a serial port* → serial port = the **Outgoing** Bluetooth COM port,
device model *Meade LX200 (compatible)* (Protocol **LX200**) or *Celestron
NexStar (compatible)* (Protocol **NexStar**), coordinate system **JNow**,
tick *Start/connect at startup*. The serial-port box is editable — type the
port (e.g. `COM16`) if the list picks the wrong one. OK → **Start**; the status
turns *Connected*. Select an object (F3 to search) → Ctrl+1 GoTo, or use the
*Slew telescope to* window (*Current object* → *Slew*) · Ctrl+3 sync (LX200
only; Stellarium's NexStar driver has no sync) · Ctrl+5 abort.

**Stellarium Mobile (Android):** tested with Protocol **NexStar** - pair the
phone, pick a Celestron NexStar telescope over Bluetooth → *SmartEQ-RJ9*. GoTo,
the direction buttons and stop all work; with the *Time* / *Location* **Sync**
switches on, it sets the mount's clock and site from the phone (while the GPS
has no fix) and shows *Time: correct*.

**SkySafari (Android):** Settings → Telescope → Setup → scope type as in the
table, mount type *Equatorial GoTo (German)*, connect via Bluetooth to
*SmartEQ-RJ9*. (iPhones can't use Classic Bluetooth serial ports.)

What is translated:

| | LX200 | NexStar | iOptron sent to the mount |
|---|---|---|---|
| position | `:GR#` `:GD#` (`:U#` / `:P#` precision), `:GA#` `:GZ#` | `E` `e`, `Z` `z` | `:GEC#` (reused for 250 ms) |
| GoTo | `:Sr` `:Sd` `:MS#` → `0` / `1…#` below horizon / `2…#` error | `R` `r` | `:ST1#` `:Sr` `:Sd` `:MS#` |
| sync | `:CM#` | `S` `s` | `:Sr` `:Sd` `:CM#` |
| stop | `:Q#` | `M` | `:q#` |
| slewing? | `:D#` | `L` | `:GAS#` |
| manual move | `:Mn/s/e/w#`, `:Qn/s/e/w#`, `:RG/RC/RM/RS#` (2×, 16×, 128×, max), `:R0#`–`:R9#` | `P` fixed / variable-rate axis moves | `:SRn#`, `:mn/ms/me/mw#`, `:qR#` `:qD#` |
| tracking | `:Te#` `:Td#`, `:TQ/TL/TS#`, `:GW#` | `t` `T` | `:ST1#` `:ST0#`, `:RT0/1/2#` |
| park | `:hP#` | — | `:MH#` (zero position) |
| info | `ACK`→`P`, `:GVP/GVN/GVD/GVT#`, `:Gc#`, `:GT#` | `V` (4.21), `m` (20, AVX), `J`, `K` | — |
| time / site read | `:GL# :Ga# :GC# :GG# :Gt# :Gg#` | `h` `w` | — (from GPS / last mount poll) |
| time / site set | `:SG` + `:SL` + `:SC` (applied once date and time are in), `:St`, `:Sg` | `H`, `W` | `:SG` `:SDS0#` `:SC` `:SL`, `:Sg` `:St` — **only while the GPS has no fix** |

Notes:

- Time and site **set** commands from the app are written to the mount only
  while the GPS has **no fix** — with a fix the GPS stays the authority and the
  app's values are acknowledged but not applied. The time zone written is the
  controller's *Time zone* setting (Auto: from the site) whenever the app's
  offset is within an hour of it; an app time that already matches the mount
  (±2 s) isn't rewritten (rewriting the clock while tracking upsets the mount).
- NexStar sends the UTC offset in **whole hours**: India's +5:30 arrives as +5
  with the true local time. The controller keeps +5:30 for the mount and
  answers `h` with the local time that matches the whole-hour offset (UTC + 5:00),
  so the app's UTC check passes (Stellarium Mobile: *Time: correct*).
- After a GoTo, the controller watches the slew and turns tracking back on if
  the mount ends it stopped, so Stellarium GoTos work right after *Go to zero position*.
- NexStar Alt-Az GoTo (`B`/`b`) isn't supported. If the arrow buttons in an app
  move the wrong way East/West, use the app's reverse-direction setting.
- The translator logic has a host test with a mock mount:
  `sh tools/xlate-test/run.sh` (g++; on Windows run it in WSL).
- To see exactly what an app sends, `trace` on the USB console logs every
  client command and reply, or run the NexStar hand-controller simulator
  `python tools/scope-sim/scope_sim.py COMx` on a PC's *Incoming* Bluetooth
  COM port and point the app at the PC.

### Debug console (USB, 115200 baud)

Everything logged here also goes to the **Debug log** screen on the TFT (menu
→ *Debug log*), so the device can be debugged without a PC. Console commands
are echoed into that log, and all except `btn`, `shot`, `baud` and `tft` also
switch the screen to it (so button presses and screen dumps can drive and film
the normal UI).

| Command | Does |
|---|---|
| `:cmd#` | sent to the mount, reply printed (idle only) |
| `.` | toggles the 3-second `[state]` lines |
| `proto` / `proto ioptron\|lx200\|nexstar` | shows / sets the Bluetooth client protocol |
| `trace` | log every translated client command and reply |
| `bonds` / `forget <n\|all>` | list / remove paired Bluetooth devices |
| `btn a\|b\|sel\|back\|a+\|b+` | press a button (A, B, BOOT short, BOOT held, A / B held) — drives the current screen |
| `shot [hz]` / `shotd [hz]` | dump the TFT (whole screen / only rows changed since the last dump); `python tools/tftshot.py log.txt out.png` (or `--all frames/`) turns it into PNGs. Reads the panel back over SPI, max ~11 MHz |
| `baud <n>` | switch the console baud rate (e.g. `baud 921600` for fast screen dumps) |
| `tft` | show / leave the Debug log screen |

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

Setters used by the GPS sync: `:SG±MMM#` (time-zone minutes), `:SDS0#` (DST off),
`:SCYYMMDD#`, `:SLHHMMSS#` (local), `:Sg±SSSSSS#`, `:St±SSSSSS#` — each returns `1`
on OK. **Site is in whole arc-seconds, 6 digits.** The 8-digit 0.01″ form is
also acknowledged with `1` but stored wrapped (a site at 13° N becomes −41.9°),
which silently breaks every GoTo — from this controller and the hand controller.
`:Gt#` / `:Gg#` read back in the same arc-second units; `:GAC#` (altitude ·
azimuth in 0.01″) equals the latitude when the mount is at its zero position.

GoTo (verified against iOptron Commander on this mount): `:ST1#` tracking on,
`:SrRRRRRRRR#` RA in ms, `:Sd±DDDDDDDD#` Dec in 0.01″, `:MS#` slew (`1` OK /
`0` below horizon or limits), `:q#` stop — all reply `1`.

## Source layout

| File | Job |
|---|---|
| `src/config.h` | build-time config (pins/bauds), `LOG` macro, TFT colours |
| `src/debuglog.*` | `LOG()` sink: USB console + line ring for the Debug log screen |
| `src/message.*` | transient footer status line (`setMsg`) |
| `src/mount.*` | RS-232 link: `askMount`, iOptron reply decoders, idle poll loop, link tracking |
| `src/bt_bridge.*` | SPP endpoint + transparent byte pumping (or the translator); always-visible Just Works pairing, bond list (NVS) |
| `src/xlate.*` | LX200 / NexStar ⇄ iOptron translator (pure C++, host-tested in `tools/xlate-test/`) |
| `src/proto.*` | translator glue: mount / GPS / site / Bluetooth hooks, protocol setting (NVS) |
| `src/gps.*` | NEO-6M UART + TinyGPS++ + lock / status |
| `src/gps_sync.*` | automatic + manual (menu) GPS→mount time-zone/time/site sync, time-zone setting (NVS) |
| `src/display.*` | ST7789 + status page (changed-field redraw) |
| `src/buttons.*` | 10 ms sampled buttons → up / down (+ hold-repeat) / select / back events |
| `src/sky.*` | SD hot-plug, catalog, thumbnails, info text, precession + alt/az |
| `src/ephem.*` | planets + Moon positions (JPL elements / Meeus), Sun for the 15° safety guard |
| `src/ui.*` | screens: status → menu → object list → object page / TRACK; time zone, Bluetooth, factory reset, Debug log |
| `tools/sdcard/` | builds the SD content + `docs/sdcard/*.zip` |
| `tools/xlate-test/` | host test of the translator against a mock mount |
| `tools/scope-sim/` | NexStar hand-controller simulator on a PC serial port (see what an app sends) |
| `tools/tftshot.py` | turns `shot` / `shotd` console dumps into PNG screenshots / frames |
| `src/main.cpp` | `setup()` / `loop()` wiring only |
| `scripts/merge_bin.py` | post-build: merged image + manifest for the web flasher |
| `docs/` | GitHub Pages web flasher (`index.html`, `manifest.json`, `firmware/`) |

## Notes

- Stateless bridge, no flow control (RTS/CTS/DTR not wired on this board).
- `pump()` is bounded by `available()`, so `loop()` never blocks on the bridge.
  In LX200 / NexStar mode each client command blocks for its mount round trip
  (~50–120 ms at 9600 baud; GoTo ~1 s); an idle client gets a `:GAS#` keepalive
  every 2 s so LED2 keeps showing the mount link.
- Buttons are sampled by a 10 ms `esp_timer` into a queue, so a poll-blocked `loop()` can't miss a press (no GPIO36/39 interrupts - ESP32 erratum).
- TFT redraws only changed fields — no flicker.
- `huge_app.csv` partition (3 MB app, no OTA) — Bluedroid SPP is large.
- Bluetooth is deliberately open: SSP IO capability *NoInputNoOutput* (Just
  Works, no MITM protection) and any device may open the SPP link. Only one
  client is served at a time; while one is connected, GoTo from the
  controller's own menu is disabled.
