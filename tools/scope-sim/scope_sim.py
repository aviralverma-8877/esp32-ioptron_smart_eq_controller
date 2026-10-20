"""Celestron NexStar hand-controller simulator on a serial port, for watching
what an app (Stellarium Mobile, SkySafari ...) sends and expects.

On Windows, pair the phone with this PC and use the PC's *Incoming* Bluetooth
COM port; the app then connects to the PC instead of the SmartEQ-RJ9.

    python scope_sim.py COM8                 # log every command + reply
    python scope_sim.py COM8 --version 5.29  # report another HC firmware

Every command and reply is printed with a timestamp and as hex, so app
behaviour (retries, timeouts, what it reads back) is visible.
"""
import argparse
import datetime as dt
import math
import time

import serial

ARGLEN = {  # bytes after the command letter
    **{c: 0 for c in "EeZzLMJVmthw"},
    "K": 1, "T": 1, "P": 7, "H": 8, "W": 8,
    "R": 9, "S": 9, "B": 9, "r": 17, "s": 17, "b": 17,
}


def show(b: bytes) -> str:
    asc = "".join(chr(c) if 32 <= c < 127 else "." for c in b)
    return f"{b.hex(' ')}   '{asc}'"


class Scope:
    def __init__(self, version):
        self.version = version
        self.ra, self.dec = 2.53, 89.26        # hours, degrees (JNow)
        self.target = None                      # (ra, dec) while slewing
        self.slew_t = 0.0
        self.track = 2                          # 0 off, 1 alt-az, 2 EQ north, 3 EQ south
        # clock: what the app last set (local time + offset hours + dst), advancing in real time
        now = dt.datetime.now()
        self.clock_local, self.clock_set_at = now, time.time()
        self.tz_h, self.dst = 5, 0
        self.lat, self.lon = 13.0194, 77.7233

    # ---- motion ----
    def tick(self):
        if not self.target:
            return
        tra, tdec = self.target
        rate = 4.0 * (time.time() - self.slew_t)  # deg per s
        self.slew_t = time.time()
        dra = ((tra - self.ra + 12) % 24) - 12
        ddec = tdec - self.dec
        step_ra, step_dec = rate / 15.0, rate
        self.ra = (self.ra + max(-step_ra, min(step_ra, dra))) % 24
        self.dec += max(-step_dec, min(step_dec, ddec))
        if abs(dra) < 1e-4 and abs(ddec) < 1e-3:
            self.ra, self.dec, self.target = tra, tdec, None

    def local_now(self):
        return self.clock_local + dt.timedelta(seconds=time.time() - self.clock_set_at)

    # ---- encoding ----
    @staticmethod
    def frac(x):
        return x - math.floor(x)

    def pair(self, precise, a, b):
        if precise:
            x = int(round(self.frac(a) * 2**32)) & 0xFFFFFF00
            y = int(round(self.frac(b) * 2**32)) & 0xFFFFFF00
            return f"{x:08X},{y:08X}#".encode()
        x = int(round(self.frac(a) * 65536)) & 0xFFFF
        y = int(round(self.frac(b) * 65536)) & 0xFFFF
        return f"{x:04X},{y:04X}#".encode()

    @staticmethod
    def unpair(s: bytes, w):
        a, b = int(s[:w], 16), int(s[w + 1:2 * w + 1], 16)
        full = 65536 if w == 4 else 2**32
        ra, dec = a / full * 24, b / full * 360
        return ra, dec - 360 if dec > 180 else dec

    # ---- commands ----
    def handle(self, cmd: bytes) -> bytes:
        c, a = chr(cmd[0]), cmd[1:]
        if c in "Ee":
            return self.pair(c == "e", self.ra / 24, (self.dec % 360) / 360)
        if c in "Zz":
            return self.pair(c == "z", 0.25, 45 / 360)          # fake az 90, alt 45
        if c in "RrSs":
            ra, dec = self.unpair(a, 4 if c in "RS" else 8)
            if c in "Rr":
                self.target, self.slew_t = (ra, dec), time.time()
                note(f"GOTO  RA {ra:.4f}h  Dec {dec:+.4f}")
            else:
                self.ra, self.dec, self.target = ra, dec, None
                note(f"SYNC  RA {ra:.4f}h  Dec {dec:+.4f}")
            return b"#"
        if c in "Bb":
            note("GOTO alt-az (ignored)")
            return b"#"
        if c == "L":
            return b"1#" if self.target else b"0#"
        if c == "M":
            self.target = None
            return b"#"
        if c == "J":
            return b"\x01#"                  # aligned (binary)
        if c == "K":
            return a + b"#"
        if c == "V":
            major, minor = (int(p) for p in self.version.split("."))
            return bytes([major, minor]) + b"#"
        if c == "m":
            return bytes([20]) + b"#"
        if c == "t":
            return bytes([self.track]) + b"#"
        if c == "T":
            self.track = a[0]
            note(f"tracking mode {a[0]}")
            return b"#"
        if c == "h":
            t = self.local_now()
            return bytes([t.hour, t.minute, t.second, t.month, t.day, t.year - 2000,
                          self.tz_h & 0xFF, self.dst]) + b"#"
        if c == "H":
            q, r_, s, mo, d, y, w, x = a
            self.clock_local = dt.datetime(2000 + y, mo, d, q, r_, s)
            self.clock_set_at = time.time()
            self.tz_h = w - 256 if w > 127 else w
            self.dst = x
            utc = self.clock_local - dt.timedelta(hours=self.tz_h + x)
            note(f"SET TIME  local {self.clock_local}  GMT{self.tz_h:+d} dst {x}  "
                 f"(implies UTC {utc:%H:%M:%S}; PC UTC now {dt.datetime.utcnow():%H:%M:%S})")
            return b"#"
        if c == "w":
            def dms(v):
                s = int(round(abs(v) * 3600))
                return [s // 3600, s // 60 % 60, s % 60]
            return bytes(dms(self.lat) + [self.lat < 0] + dms(self.lon) + [self.lon < 0]) + b"#"
        if c == "W":
            lat = a[0] + a[1] / 60 + a[2] / 3600
            lon = a[4] + a[5] / 60 + a[6] / 3600
            self.lat = -lat if a[3] else lat
            self.lon = -lon if a[7] else lon
            note(f"SET SITE  lat {self.lat:+.4f}  lon {self.lon:+.4f}")
            return b"#"
        if c == "P":
            n, dest, pid, d1, d2, d3, nresp = a
            if pid == 254 and dest in (16, 17):
                return bytes([7, 11][:nresp]) + b"#"       # motor firmware
            if dest == 176:                                 # GPS module
                note(f"GPS passthrough id {pid}")
                return bytes(nresp) + b"#"
            if dest in (16, 17) and pid in (36, 37, 6, 7):
                axis = "RA" if dest == 16 else "Dec"
                rate = d1 if pid >= 36 else (d1 << 8 | d2) / 4
                note(f"MOVE {axis} {'+' if pid in (36, 6) else '-'} rate {rate}")
            return bytes(nresp) + b"#"
        return b""


T0 = time.time()


def stamp():
    return f"{time.time() - T0:7.2f}s"


def note(msg):
    print(f"{stamp()}   * {msg}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--version", default="4.21", help="HC firmware reported by V (major.minor)")
    ap.add_argument("--quiet-polls", action="store_true", help="don't print E/e/L/J/t polls")
    args = ap.parse_args()

    sc = Scope(args.version)
    ser = serial.Serial(args.port, args.baud, timeout=0.02)
    print(f"NexStar simulator on {args.port} - waiting for the app ...", flush=True)
    buf = b""
    last_rx = time.time()
    while True:
        sc.tick()
        d = ser.read(64)
        if d:
            buf += d
            last_rx = time.time()
        elif buf and time.time() - last_rx > 1.0:
            print(f"{stamp()}  ?? incomplete, dropped: {show(buf)}", flush=True)
            buf = b""
        while buf:
            c = chr(buf[0])
            if c not in ARGLEN:
                print(f"{stamp()}  ?? unknown byte: {show(buf[:1])}", flush=True)
                buf = buf[1:]
                continue
            need = 1 + ARGLEN[c]
            if len(buf) < need:
                break
            cmd, buf = buf[:need], buf[need:]
            reply = sc.handle(cmd)
            if reply:
                ser.write(reply)
            if not (args.quiet_polls and c in "EeLJt"):
                print(f"{stamp()}  -> {show(cmd)}\n{'':10}<- {show(reply)}", flush=True)


if __name__ == "__main__":
    main()
