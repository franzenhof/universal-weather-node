#!/usr/bin/env python3
# Long-run logger for the Heltec V4 (work package A5): samples /api/info and
# /api/data every N seconds into a CSV (heap, PSRAM, task stacks, resets) and
# prints a short summary of that CSV with --report.
import argparse, csv, json, os, sys, time, urllib.request
from datetime import datetime

BASE = ["time", "error", "uptimeSec", "heapFree", "heapMin", "heapLargest",
        "psramFree", "psramLargest", "resetReason", "version", "rfSensors", "cpuTemp"]


def get(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def sample(url):
    info = get(url + "/api/info")
    data = get(url + "/api/data")
    s = info.get("system", {})
    ps = s.get("psram") or {}
    row = {
        "uptimeSec": s.get("uptimeSec", info.get("device", {}).get("uptimeSec")),
        "heapFree": s.get("heapFree"), "heapMin": s.get("heapMin"),
        "heapLargest": s.get("heapLargest"),
        "psramFree": ps.get("free"), "psramLargest": ps.get("largest"),
        "resetReason": data.get("resetReason"),
        "version": info.get("firmware", {}).get("version"),
        "rfSensors": len(data.get("rfSensors") or []),
        "cpuTemp": data.get("cpuTemp"),
    }
    for k, v in (s.get("stackFree") or {}).items():
        row["stack_" + k] = v
    return row


def run(a):
    path = os.path.expanduser(a.out)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    cols = None
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, newline="") as f:
            cols = next(csv.reader(f), None)
    pending = []
    while True:
        t0 = time.time()
        row = {"time": datetime.now().astimezone().isoformat(timespec="seconds")}
        try:
            row.update(sample(a.url.rstrip("/")))
        except Exception as e:
            row["error"] = (type(e).__name__ + ": " + str(e)).replace("\n", " ")[:200]
        pending.append(row)
        if cols is None and "error" not in row:
            cols = BASE + sorted(k for k in row if k.startswith("stack_"))
            hdr = True
        else:
            hdr = False
        if cols is None:  # no good sample yet: stack columns unknown, keep errors in memory
            pass
        else:
            try:
                with open(path, "a", newline="") as f:
                    w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
                    if hdr:
                        w.writeheader()
                    for r in pending:
                        w.writerow(r)
                    f.flush()
                pending = []
            except Exception as e:
                print("write failed:", e, file=sys.stderr)
        time.sleep(max(0.0, a.interval - (time.time() - t0)))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def report(a):
    with open(os.path.expanduser(a.out), newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print("empty"); return
    ok = [r for r in rows if not r.get("error")]
    print(f"{rows[0]['time']} .. {rows[-1]['time']}")
    print(f"samples {len(rows)}, errors {len(rows) - len(ok)}, version {ok[-1]['version'] if ok else '-'}")
    last_restart = 0
    prev = None
    for i, r in enumerate(ok):
        u = num(r["uptimeSec"])
        if prev is not None and u is not None and u < prev:
            print(f"restart at {r['time']} (reset reason: {r['resetReason']})")
            last_restart = i
        if u is not None:
            prev = u
    for k in ("heapFree", "heapMin", "heapLargest"):
        v = [num(r[k]) for r in ok if num(r[k]) is not None]
        if v:
            print(f"{k}: min {min(v):.0f} max {max(v):.0f} first {v[0]:.0f} last {v[-1]:.0f}")
    stacks = [c for c in rows[0].keys() if c.startswith("stack_")]
    if stacks:
        parts = []
        for c in stacks:
            v = [num(r.get(c)) for r in ok if num(r.get(c)) is not None]
            if v:
                parts.append(f"{c[6:]} {min(v):.0f}")
        print("min stack free: " + ", ".join(parts))
    pts = []
    for r in ok[last_restart:]:
        u, h = num(r["uptimeSec"]), num(r["heapFree"])
        if u is not None and h is not None:
            pts.append((u, h))
    if len(pts) >= 3 and pts[-1][0] > pts[0][0]:
        n = len(pts)
        mx = sum(p[0] for p in pts) / n
        my = sum(p[1] for p in pts) / n
        sxx = sum((p[0] - mx) ** 2 for p in pts)
        slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx * 86400
        print(f"heapFree trend since last restart: {slope:+.0f} bytes/day ({n} samples, {(pts[-1][0]-pts[0][0])/3600:.1f} h)")
    else:
        print("heapFree trend: too few samples")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", default="http://172.24.10.107")
    p.add_argument("--interval", type=float, default=300)
    p.add_argument("--out", default="~/uwn-longrun/v4.csv")
    p.add_argument("--report", action="store_true")
    a = p.parse_args()
    report(a) if a.report else run(a)


if __name__ == "__main__":
    main()
