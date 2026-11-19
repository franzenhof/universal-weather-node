#!/usr/bin/env python3
"""Mock Universal Weather Node: serves /api/info, /api/data and /api/history
from the example responses in docs/api/examples/<fixture>/ with slowly
drifting values, so a UI can be developed without hardware.

Python 3 standard library only:

    python3 tools/api-mock/mock_server.py                       # http://127.0.0.1:8080
    python3 tools/api-mock/mock_server.py --fixture pcb-v2-imperial --port 9000 --cycle 10

See tools/api-mock/README.md for what is simulated and what is not.
"""

import argparse
import copy
import json
import math
import random
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent.parent.parent
EXAMPLES = ROOT / "docs" / "api" / "examples"
DISPLAY = ROOT / "examples" / "simple-display" / "index.html"

COMPASS = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
HIST_KEYS = ["windSpeed", "windGust", "windDirDeg", "rainRate", "rainMm", "temp", "humidityPct", "pressureQnh",
             "batteryVolt", "uvSensorVolts", "uvIndex", "ds18b20_0", "ds18b20_1", "ds18b20_2"]


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


class Units:
    """Conversions between the fixture's units and base units (C, hPa, m/s, mm)."""

    def __init__(self, info):
        u = info["units"]
        self.f = u["temp"] == "F"
        self.inhg = u["pressure"] == "inHg"
        self.wind = {"m/s": 1.0, "km/h": 3.6, "mph": 2.23694}[u["wind"]]
        self.rain = 1 / 25.4 if u["rain"] == "in" else 1.0

    def to_c(self, t): return (t - 32) / 1.8 if self.f else t
    def from_c(self, c): return c * 1.8 + 32 if self.f else c
    def from_hpa(self, p): return p / 33.8639 if self.inhg else p
    def to_hpa(self, p): return p * 33.8639 if self.inhg else p


def dew_point_c(t, rh):
    a, b = 17.62, 243.12
    g = math.log(max(rh, 1) / 100) + a * t / (b + t)
    return b * g / (a - g)


def svp_kpa(t):
    return 0.6108 * math.exp(17.27 * t / (t + 237.3))


def tendency(delta_hpa):
    d = abs(delta_hpa)
    if d < 0.1:
        return "steady"
    word = "rising" if delta_hpa > 0 else "falling"
    if d < 1.6:
        return word + " slowly"
    if d < 3.6:
        return word
    if d < 6.0:
        return word + " quickly"
    return word + " very rapidly"


class Node:
    def __init__(self, fixture, cycle, seed):
        d = EXAMPLES / fixture
        if not d.is_dir():
            raise SystemExit(f"no fixture {fixture!r}; available: {', '.join(sorted(p.name for p in EXAMPLES.iterdir() if p.is_dir()))}")
        load = lambda name: json.loads((d / name).read_text(encoding="utf-8"))
        self.info, self.data = load("info.json"), load("data.json")
        self.lock = threading.Lock()
        self.rng = random.Random(seed)
        self.units = Units(self.info)
        self.cycle = cycle or self.info["measurement"]["cycleSec"]
        self.info["measurement"]["cycleSec"] = self.cycle
        self.start = time.time()
        self.uptime0 = self.info["device"]["uptimeSec"]
        self.last_cycle = self.start - (self.cycle - min(self.data["nextCycleInSec"], self.cycle))
        self.shower = 0                      # cycles of rain left
        self.cycle_rain = 0.0                # rain of the latest cycle
        self.rain_log = []                   # (time, base mm) for rain1h/rain24h increments
        self.temp0 = self.data.get("temp")
        self.press0 = self.data.get("pressureQnh")
        # histories: points with absolute end times; the last one is the open bucket
        self.hist = {}
        for rng, name in (("24h", "history-24h.json"), ("7d", "history-7d.json")):
            h = load(name)
            meta = next(r for r in self.info["history"]["ranges"] if r["range"] == rng)
            pts = [{"end": self.start - age, **{k: h[k][i] for k in HIST_KEYS if k in h}} for i, age in enumerate(h["ageSec"])]
            units = {k: v for k, v in h.items() if not isinstance(v, list) and k != "apiVersion"}
            self.hist[rng] = {"doc": h, "units": units, "points": pts, "bucket": meta["bucketSec"],
                              "max": meta["maxPoints"], "window": self.window(self.start, meta["bucketSec"]),
                              "rainSum": 0.0}

    # ---- simulation ----
    def walk(self, key, step, lo, hi, obj=None):
        obj = self.data if obj is None else obj
        if key in obj and isinstance(obj[key], (int, float)):
            obj[key] = clamp(obj[key] + self.rng.gauss(0, step), lo, hi)

    def new_cycle(self, now):
        d, u, r = self.data, self.units, self.rng
        if self.temp0 is not None:
            self.walk("temp", 0.15 * (1.8 if u.f else 1), self.temp0 - 6 * (1.8 if u.f else 1), self.temp0 + 6 * (1.8 if u.f else 1))
            self.walk("humidityPct", 1.0, 20, 99)
        if self.press0 is not None:
            step = u.from_hpa(0.08)
            self.walk("pressureQnh", step, self.press0 - u.from_hpa(8), self.press0 + u.from_hpa(8))
            d["pressureAbs"] = d["pressureQnh"] - u.from_hpa(66.2)
            if "pressureTrend3h" in d:
                d["pressureTrend3h"] = round(clamp(d["pressureTrend3h"] + r.gauss(0, u.from_hpa(0.05)), u.from_hpa(-4), u.from_hpa(4)), 2)
                d["pressureTendency"] = tendency(u.to_hpa(d["pressureTrend3h"]))
        if "windSpeed" in d:
            ms = clamp(d["windSpeed"] / u.wind + r.gauss(0, 0.5), 0, 25)
            d["windSpeed"] = ms * u.wind
            d["windGust"] = ms * r.uniform(1.3, 1.9) * u.wind + (r.uniform(0, 1.5) if ms > 0.3 else 0)
            d["windDirDeg"] = int(d["windDirDeg"] + r.gauss(0, 15)) % 360
            if "windDirText" in d:
                d["windDirText"] = COMPASS[round(d["windDirDeg"] / 22.5) % 16]
        for i, v in enumerate(d.get("ds18b20", [])):
            if v is not None:
                d["ds18b20"][i] = round(v + r.gauss(0, 0.1), 1)
        if "uvIndex" in d:
            d["uvIndex"] = round(clamp(d["uvIndex"] + r.gauss(0, 0.2), 0, 11), 1)
        if "lightLux" in d:
            d["lightLux"] = round(clamp(d["lightLux"] + r.gauss(0, 2500), 0, 120000))
        self.rain(now)
        self.derive()
        # wireless source sensor follows the top-level values
        for s in d.get("rfSensors", []):
            src = "radio:" + s["id"]
            if d.get("sources", {}).get("tempHum") == src and "temp" in d:
                s["temp"], s["humidityPct"] = round(d["temp"], 1), round(d["humidityPct"])
            if d.get("sources", {}).get("wind") == src and "windSpeed" in d:
                s["windSpeed"], s["windGust"], s["windDirDeg"] = round(d["windSpeed"], 1), round(d["windGust"], 1), d["windDirDeg"]
        self.last_cycle = now
        for rng in self.hist:
            self.push_history(rng, now)

    @staticmethod
    def window(t, bucket):
        """Index of the fixed window (k*bucket, (k+1)*bucket] holding time t, like the node's."""
        return math.ceil(t / bucket) - 1

    def rain(self, now):
        d, u = self.data, self.units
        self.cycle_rain = 0.0
        if "rain" not in d:
            return
        if self.shower == 0 and self.rng.random() < 0.04:
            self.shower = self.rng.randint(2, 8)           # a shower of a few cycles
        mm = 0.2 * self.rng.randint(1, 4) if self.shower else 0.0   # whole tips of 0.2 mm
        self.shower = max(0, self.shower - 1)
        amount = mm * u.rain
        self.cycle_rain = amount
        d["rain"] = amount
        d["rainRate"] = amount * 3600 / self.cycle
        d["rainSinceStart"] += amount
        for k in ("rainToday", "rainWeek", "rainMonth", "rainYear", "rain24h"):
            if k in d:
                d[k] += amount
        self.rain_log = [(t, a) for t, a in self.rain_log if now - t < 3600] + ([(now, amount)] if amount else [])
        if "rain1h" in d:
            d["rain1h"] = max(d["rain1h"] * 0.9, sum(a for _, a in self.rain_log))   # older rain slowly leaves the hour
        if amount:
            if "lastRain" in d or self.info["measurement"]["clockSynced"]:
                d["lastRain"] = datetime.fromtimestamp(now, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            for s in d.get("rfSensors", []):
                if d.get("sources", {}).get("rain") == "radio:" + s["id"] and "rainTotal" in s:
                    s["rainTotal"] += amount
        for h in self.hist.values():
            h["rainSum"] += amount

    def derive(self):
        d, u = self.data, self.units
        if "temp" not in d:
            return
        t = u.to_c(d["temp"])
        if "humidityPct" in d:
            rh = d["humidityPct"]
            dp = dew_point_c(t, rh)
            d["dewPoint"] = round(u.from_c(dp), 1)
            if "vpd" in d:
                d["vpd"] = round(svp_kpa(t) * (1 - rh / 100), 2)
            if "cloudBase" in d:
                cb = max(0.0, (t - dp) * 125)
                d["cloudBase"] = round(cb * 3.28084 if d.get("cloudBaseUnit") == "ft" else cb)
            if "warnings" in d:
                d["warnings"] = {"frost": t <= 0, "fog": t - dp <= 1 and rh >= 95, "sultry": dp >= 16}
        if "feelsLike" in d:
            d["feelsLike"] = round(d["temp"], 1)

    def push_history(self, rng, now):
        h, d = self.hist[rng], self.data
        win = self.window(now, h["bucket"])
        if win != h["window"]:                            # a cycle in a later window: close the open bucket
            closed = h["points"][-1]
            closed["end"] = min(now, (h["window"] + 1) * h["bucket"])
            empty = {k: None for k in closed if k != "end"}
            skipped = min(win - h["window"] - 1, int((now - closed["end"]) // h["bucket"]), h["max"])
            for i in range(1, max(0, skipped) + 1):      # windows without a cycle: gaps
                h["points"].append({"end": closed["end"] + i * h["bucket"], **empty})
            h["points"].append(dict(closed))
            h["window"], h["rainSum"] = win, self.cycle_rain
            del h["points"][:-(h["max"] + 1)]
        open_pt = h["points"][-1]
        for k in HIST_KEYS:
            if k not in open_pt:
                continue
            if k == "rainMm":
                v = h["rainSum"] if d.get("rain") is not None else None
            elif k.startswith("ds18b20_"):
                arr = d.get("ds18b20", [])
                i = int(k[-1])
                v = arr[i] if i < len(arr) else None
            elif k == "batteryVolt":
                v = d.get("batteryVolt")
            else:
                v = d.get(k)
            open_pt[k] = None if v is None else round(v, 2)
        open_pt["end"] = now

    def tick(self):
        with self.lock:
            now = time.time()
            while now - self.last_cycle >= self.cycle:
                self.new_cycle(self.last_cycle + self.cycle)

    # ---- documents ----
    def doc_data(self):
        self.tick()
        with self.lock:
            now = time.time()
            d = copy.deepcopy(self.data)
            d["nextCycleInSec"] = max(0, int(self.cycle - (now - self.last_cycle)))
            for s in d.get("rfSensors", []):
                if "stale" not in s:
                    s["ageSec"] = int((now - self.start + s["ageSec"]) % 16)   # a packet every ~16 s
                else:
                    s["ageSec"] += int(now - self.start)
            if "uptimeH" in d:
                d["uptimeH"] = round(self.uptime() / 3600, 5)
            return d

    def uptime(self):
        return self.uptime0 + int(time.time() - self.start)

    def doc_info(self):
        self.tick()
        i = copy.deepcopy(self.info)
        i["device"]["uptimeSec"] = self.uptime()
        return i

    def doc_history(self, rng):
        self.tick()
        with self.lock:
            now = time.time()
            h = self.hist[rng]
            pts = h["points"]
            doc = {"apiVersion": h["doc"]["apiVersion"],
                   "ageSec": [int(now - p["end"]) for p in pts[:-1]] + [0]}
            for k in HIST_KEYS:
                if k in h["doc"]:
                    doc[k] = [p.get(k) for p in pts]
            doc.update(h["units"])
            return doc


def float32ish(v):
    """About 7 significant digits, like the node's 32-bit floats."""
    if isinstance(v, float):
        return float(f"{v:.7g}")
    if isinstance(v, list):
        return [float32ish(x) for x in v]
    if isinstance(v, dict):
        return {k: float32ish(x) for k, x in v.items()}
    return v


class Handler(BaseHTTPRequestHandler):
    server_version = "WeatherNodeMock/1"
    node = None

    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")

    def send_json(self, obj):
        body = json.dumps(float32ish(obj), separators=(",", ":")).encode()
        self.send_response(200)
        self.cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        if urlsplit(self.path).path not in ("/api/data", "/api/info", "/api/history"):
            return self.send_error(404)
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Access-Control-Max-Age", "86400")
        self.end_headers()

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/api/data":
            return self.send_json(self.node.doc_data())
        if url.path == "/api/info":
            return self.send_json(self.node.doc_info())
        if url.path == "/api/history":
            rng = parse_qs(url.query).get("range", ["24h"])[0]
            return self.send_json(self.node.doc_history("7d" if rng == "7d" else "24h"))
        if url.path == "/" and DISPLAY.exists():   # convenience: the example display, same origin
            body = DISPLAY.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            return self.wfile.write(body)
        if url.path in ("/api/settings", "/api/log", "/api/log/raw", "/api/log/clear"):
            return self.send_error(403, "Not simulated by the mock server")
        self.send_error(404, f"Not found: {url.path}")

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            super().log_message(fmt, *args)


def main():
    fixtures = sorted(p.name for p in EXAMPLES.iterdir() if p.is_dir())
    ap = argparse.ArgumentParser(description="Mock Universal Weather Node HTTP API (docs/api.md)")
    ap.add_argument("--fixture", default="bresser-7in1-bme280", choices=fixtures, help="example setup to serve")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1", help="bind address (0.0.0.0 = reachable from the LAN)")
    ap.add_argument("--cycle", type=int, default=30,
                    help="measurement cycle in seconds (a real node: 300; 0 = the fixture's value)")
    ap.add_argument("--seed", type=int, default=None, help="random seed for reproducible drift")
    ap.add_argument("--quiet", action="store_true", help="no request log")
    a = ap.parse_args()
    Handler.node = Node(a.fixture, a.cycle, a.seed)
    httpd = ThreadingHTTPServer((a.host, a.port), Handler)
    httpd.quiet = a.quiet
    print(f"Mock weather node '{a.fixture}' on http://{a.host}:{a.port}/  (cycle {Handler.node.cycle} s)")
    print("  /api/info  /api/data  /api/history?range=24h|7d   / = examples/simple-display")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
