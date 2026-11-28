#!/usr/bin/env python3
"""Checks behind the login for the Heltec V4 web server (work package A2).

    python3 tools/webtest_v4.py [--url http://172.24.10.107]
                                [--password-file PATH]   (first line = password, mode 0600)
                                [--ota FILE.bin]         (really flashes a valid 4.x app image!)
                                [--trace]                (starts a radio trace "until stopped")

Python 3, standard library only. The admin password is asked for with getpass
(or read from --password-file); it is never printed, logged or written to the
result file. Everything is read-only except: --trace (starts a radio trace) and
--ota (flashes). The settings form is never posted.
Results: printed as PASS / FAIL / INFO / SKIP and written to
~/uwn-webtest/result-<timestamp>.txt
"""
import argparse, getpass, json, os, re, stat, sys, time, uuid
import urllib.error, urllib.request
from datetime import datetime

USER = "admin"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WRONG_PW = "wrong-test-pw"   # deliberately wrong, only used for the refusal / brake tests
SECRET_KEYS = ["webadminpw", "wifipass", "mqttpass", "appkey", "awkpass", "wukey", "wckey", "wnpkey",
               "windypw", "osemtok", "wowpin", "nttok", "potok", "pouser", "tgtok", "whurl",
               "cu0url", "cu1url", "influxtok"]

results = []
out_lines = []


def emit(line):
    print(line, flush=True)
    out_lines.append(line)


def check(name, ok, reason="", info=False):
    tag = "INFO" if info else ("PASS" if ok else "FAIL")
    results.append(tag)
    emit("%-5s %s%s" % (tag, name, (" - " + reason) if reason else ""))
    return ok


def skip(name, reason):
    results.append("SKIP")
    emit("SKIP  %s - %s" % (name, reason))


class Client:
    """One opener with Digest login. Never prints the password."""

    def __init__(self, base, password):
        self.base = base.rstrip("/")
        self.password = password

    def opener(self, password=None, auth=True):
        handlers = []
        if auth:
            mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            mgr.add_password(None, self.base, USER, self.password if password is None else password)
            handlers.append(urllib.request.HTTPDigestAuthHandler(mgr))
        return urllib.request.build_opener(*handlers)

    def req(self, path, data=None, headers=None, password=None, auth=True, timeout=30):
        """-> (status, headers, body bytes, seconds). Network errors: status 0."""
        h = {"Origin": self.base}   # the browser sends it on POSTs; the node checks same-origin
        h.update(headers or {})
        r = urllib.request.Request(self.base + path, data=data, headers=h)
        t = time.time()
        try:
            with self.opener(password, auth).open(r, timeout=timeout) as resp:
                return resp.status, resp.headers, resp.read(), time.time() - t
        except urllib.error.HTTPError as e:
            body = e.read() if e.fp else b""
            return e.code, e.headers, body, time.time() - t
        except Exception as e:
            return 0, {}, str(type(e).__name__).encode(), time.time() - t


def multipart(field, filename, content):
    b = "----uwn" + uuid.uuid4().hex
    body = (("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n" % (b, field, filename)).encode()
            + content + ("\r\n--%s--\r\n" % b).encode())
    return body, "multipart/form-data; boundary=" + b


def upload(c, filename, content):
    # exactly the browser's XHR: POST /api/ota?js=1, multipart field "firmware"
    body, ctype = multipart("firmware", filename, content)
    return c.req("/api/ota?js=1", data=body, headers={"Content-Type": ctype}, timeout=120)


def info_json(c):
    s, _, b, _ = c.req("/api/info", auth=False)
    return json.loads(b) if s == 200 else {}


def read_password(a):
    if a.password_file:
        p = os.path.expanduser(a.password_file)
        try:
            mode = os.stat(p).st_mode
        except OSError as e:
            sys.exit("Cannot read password file: %s" % e.strerror)
        if mode & (stat.S_IRWXG | stat.S_IRWXO):
            sys.exit("Password file %s is readable by group/others.\nFix it with:  chmod 600 %s" % (p, p))
        with open(p, "r", encoding="utf-8") as f:
            return f.readline().rstrip("\r\n")
    return getpass.getpass("Admin password for %s (not shown): " % USER)


# ---------------------------------------------------------------- checks

def t_unauth(c):
    for p in ["/api/settings", "/api/settings/backup", "/api/log", "/api/rf/trace?status=1"]:
        s, h, _, _ = c.req(p, auth=False)
        wa = h.get("WWW-Authenticate", "") if s else ""
        check("no login: %s -> 401 + Digest" % p, s == 401 and wa.startswith("Digest") and "nonce=" in wa and "realm=" in wa,
              "HTTP %s, %s" % (s, wa[:60] if wa else "no WWW-Authenticate"))


def one_wrong_try(c, path="/api/settings"):
    """Exactly ONE wrong login attempt (urllib's Digest handler retries up to 6 times per call,
    which would count as six): fetch a nonce, answer it by hand with a wrong password.
    -> (status, seconds of the answering request)"""
    import hashlib
    s, h, _, _ = c.req(path, auth=False)
    wa = h.get("WWW-Authenticate", "") if s == 401 else ""
    f = dict(re.findall(r'(\w+)="?([^",]*)"?', wa))
    if not f.get("nonce"):
        return s, 0.0
    md5 = lambda x: hashlib.md5(x.encode()).hexdigest()
    cn, nc = uuid.uuid4().hex[:16], "00000001"
    resp = md5("%s:%s:%s:%s:auth:%s" % (md5("%s:%s:%s" % (USER, f["realm"], WRONG_PW)), f["nonce"], nc, cn, md5("GET:" + path)))
    auth = ('Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", opaque="%s", qop=auth, nc=%s, cnonce="%s"'
            % (USER, f["realm"], f["nonce"], path, resp, f.get("opaque", ""), nc, cn))
    s, _, _, dt = c.req(path, headers={"Authorization": auth}, auth=False)
    return s, dt


def t_wrong_password(c):
    # the node waits 1 s per try after 5 wrong logins within a minute (http_server.cpp)
    times = [one_wrong_try(c) for _ in range(8)]
    emit("      brake: single wrong tries = " + ", ".join("%s/%.2fs" % t for t in times))
    check("wrong passwords always 401, never 200/500", all(s == 401 for s, _ in times), "codes " + ",".join(str(s) for s, _ in times))
    check("brake slows repeated wrong passwords (about 1 s per try after 5)", times[-1][1] > 0.8,
          "first %.2f s, last %.2f s%s" % (times[0][1], times[-1][1],
          "" if times[0][1] < 0.5 else " (brake was already active: wrong tries within the last minute)"))
    s, _, _, dt = c.req("/api/settings", password=WRONG_PW)
    check("wrong password is refused (urllib)", s == 401, "HTTP %s in %.2f s" % (s, dt))


def t_login(c):
    s, _, b, dt = c.req("/api/settings")
    check("login works (settings page)", s == 200 and len(b) > 5000, "HTTP %s, %d bytes, %.2f s" % (s, len(b), dt))
    return b.decode("utf-8", "replace") if s == 200 else ""


def t_settings_page(page):
    if not page:
        skip("settings page checks", "no page")
        return
    for tab in ["basics", "sensors", "outputs", "alarms", "display", "system"]:
        check("settings page has tab '%s'" % tab, ("id='%s'" % tab) in page or ('id="%s"' % tab) in page)
    check("settings form present", "id='sform'" in page and "action='/api/settings'" in page)
    check("backup link and firmware upload present", "/api/settings/backup" in page and "name='firmware'" in page)
    check("password field is not pre-filled with a value", not re.search(r"name='webadminpw'[^>]*value='[^']+'", page))
    selects = re.findall(r"<select[^>]*name='(src[a-z]+)'[^>]*>(.*?)</select>", page, re.S)
    check("value-source selects found (srcair, srcwind, ...)", len(selects) >= 6, "%d selects" % len(selects))
    for name, body in selects:
        vals = re.findall(r"<option[^>]*value='([^']*)'", body)
        bad = [v for v in vals if not (re.fullmatch(r"-?\d+", v) or re.fullmatch(r"r[0-9A-Fa-f]{8}", v))]
        rcount = sum(1 for v in vals if v.startswith("r"))
        check("select %s: values numeric or r<ID>" % name, bool(vals) and not bad,
              "%d options, %d r<ID>%s" % (len(vals), rcount, (", odd: " + ",".join(bad[:3])) if bad else ""))


def t_backup(c):
    s, h, b, dt = c.req("/api/settings/backup")
    cd = h.get("Content-Disposition", "") if s else ""
    check("backup download", s == 200 and "attachment" in cd, "HTTP %s, %d bytes, %.2f s" % (s, len(b), dt))
    try:
        j = json.loads(b)
    except Exception:
        check("backup is valid JSON", False, "not JSON")
        return
    check("backup is valid JSON with format 'uwn-settings'",
          j.get("format") == "uwn-settings" and isinstance(j.get("settings"), dict) and j.get("version", 0) >= 1,
          "version %s, firmware %s, %d keys" % (j.get("version"), j.get("firmware"), len(j.get("settings", {}))))
    check("backup header says secrets=false", j.get("secrets") is False)
    present = [k for k in SECRET_KEYS if k in j.get("settings", {})]
    check("no secrets in the default backup", not present, "found: " + ",".join(present) if present else "none of %d secret keys" % len(SECRET_KEYS))
    srcs = {k: v for k, v in j.get("settings", {}).items() if k.startswith("src")}
    emit("      value sources in backup: " + json.dumps(srcs))
    check("backup value sources are numeric or r<ID>",
          all(re.fullmatch(r"-?\d+|r[0-9A-Fa-f]{8}", str(v)) for v in srcs.values()) and bool(srcs))
    s2, _, b2, _ = c.req("/api/settings/backup?secrets=1")
    try:
        j2 = json.loads(b2)
        # only reports the keys that are there, never their values
        got = [k for k in SECRET_KEYS if k in j2.get("settings", {})]
        check("backup with ?secrets=1 flags secrets=true", s2 == 200 and j2.get("secrets") is True,
              "secret keys present with a value: %s" % (",".join(got) or "none (none set)"))
    except Exception:
        check("backup with ?secrets=1", False, "HTTP %s, not JSON" % s2)


def t_log_trace(c):
    s, h, b, dt = c.req("/api/log")
    check("log page", s == 200 and b"<pre" in b, "HTTP %s, %d bytes, %.2f s" % (s, len(b), dt))
    s, h, b, dt = c.req("/api/log/raw")
    check("log raw text", s == 200 and len(b) > 0, "HTTP %s, %d bytes, %.2f s" % (s, len(b), dt))
    s, h, b, dt = c.req("/api/rf/trace?status=1")
    try:
        j = json.loads(b)
        check("radio trace status", s == 200 and "running" in j and "bufferSize" in j,
              "running=%s allocated=%s packets=%s" % (j.get("running"), j.get("allocated"), j.get("packets")))
    except Exception:
        check("radio trace status", False, "HTTP %s, not JSON" % s)
    s, _, b, _ = c.req("/api/rf/sensor", data=b"id=0&action=mine")
    check("POST with id=0 is rejected without changing anything (409)", s == 409, "HTTP %s" % s)


def t_cross_origin(c):
    s, _, _, _ = c.req("/api/log/clear", data=b"", headers={"Origin": "http://evil.test"})
    check("state-changing POST from a foreign Origin is refused (403)", s == 403, "HTTP %s" % s)


def t_ota_refusals(c):
    v0 = info_json(c).get("firmware", {}).get("version")
    s, _, b, dt = upload(c, "notes.txt", b"this is not a firmware image\n" * 40)
    check("upload of a small text file refused", s == 400 and b"not an ESP32" in b or s == 400,
          "HTTP %s, '%s' %.2f s" % (s, b[:90].decode("utf-8", "replace"), dt))
    fp = os.path.join(REPO, "firmware", "universal-weather-node-2.0.0-factory.bin")
    if os.path.exists(fp):
        with open(fp, "rb") as f:
            data = f.read()
        s, _, b, dt = upload(c, os.path.basename(fp), data)
        check("2.0.0 factory image (other partition table) refused", s == 400 and b"partition table" in b,
              "HTTP %s, '%s' %.2f s" % (s, b[:110].decode("utf-8", "replace"), dt))
    else:
        skip("2.0.0 factory image refused", "file %s not found" % fp)
    time.sleep(2)
    i = info_json(c)
    check("node still up, version unchanged after the refused uploads",
          i.get("firmware", {}).get("version") == v0 and v0 is not None, "version %s, uptime %ss" % (v0, i.get("system", {}).get("uptimeSec")))


def t_ota_real(c, path):
    with open(path, "rb") as f:
        data = f.read()
    v0 = info_json(c).get("firmware", {}).get("version")
    emit("INFO  uploading %s (%d bytes), running version %s" % (os.path.basename(path), len(data), v0))
    s, _, b, dt = upload(c, os.path.basename(path), data)
    check("firmware upload accepted", s == 200, "HTTP %s, '%s' %.1f s" % (s, b[:100].decode("utf-8", "replace"), dt))
    if s != 200:
        return
    t0 = time.time()
    time.sleep(8)
    ver = None
    while time.time() - t0 < 120:
        try:
            ver = info_json(c).get("firmware", {}).get("version")
        except Exception:
            ver = None
        if ver:
            break
        time.sleep(3)
    check("node is back after the restart", bool(ver), "version %s after %.0f s" % (ver, time.time() - t0))
    emit("      version before: %s, after: %s" % (v0, ver))


def t_trace_start(c):
    # the "until stopped" mode is started by the node itself (Davis mode); the REST start takes
    # 1..120 min, so the longest (120 min) is used; restart it with this script when it ends
    s, _, b, _ = c.req("/api/rf/trace", data=b"action=start&minutes=120&filter=all")
    try:
        j = json.loads(b)
    except Exception:
        j = {}
    check("radio trace started (120 min, all packets)", s == 200 and j.get("running") is True,
          "HTTP %s, %s" % (s, j.get("error") or "running=%s" % j.get("running")))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://172.24.10.107")
    ap.add_argument("--password-file", metavar="PATH")
    ap.add_argument("--ota", metavar="FILE", help="really upload this valid app image and check the version afterwards")
    ap.add_argument("--trace", action="store_true", help="start a radio trace (load for the long run)")
    a = ap.parse_args()
    pw = read_password(a)
    c = Client(a.url, pw)
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    emit("webtest_v4 %s against %s" % (ts, a.url))
    s, _, b, _ = c.req("/api/info", auth=False, timeout=10)
    if s != 200:
        sys.exit("Node not reachable at %s (HTTP %s)" % (a.url, s))
    i = json.loads(b)
    emit("node: firmware %s, board %s, uptime %ss" % (i.get("firmware", {}).get("version"),
         i.get("hardware", {}).get("board"), i.get("system", {}).get("uptimeSec")))
    t_unauth(c)
    t_wrong_password(c)
    page = t_login(c)           # a correct login resets the node's brake
    if not page:
        emit("Login failed - wrong password? The remaining checks are skipped.")
    else:
        t_settings_page(page)
        t_backup(c)
        t_log_trace(c)
        t_cross_origin(c)
        t_ota_refusals(c)
        if a.ota:
            t_ota_real(c, os.path.expanduser(a.ota))
        else:
            skip("real firmware upload", "no --ota FILE given (nothing was flashed)")
        if a.trace:
            t_trace_start(c)
        else:
            skip("radio trace start", "no --trace given (trace untouched)")
    n = lambda t: results.count(t)
    emit("\nSummary: %d PASS, %d FAIL, %d SKIP, %d INFO" % (n("PASS"), n("FAIL"), n("SKIP"), n("INFO")))
    d = os.path.expanduser("~/uwn-webtest")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "result-%s.txt" % ts)
    with open(p, "w", encoding="utf-8") as f:
        f.write("\n".join(out_lines) + "\n")
    print("Result file: " + p)
    sys.exit(1 if n("FAIL") else 0)


if __name__ == "__main__":
    main()
