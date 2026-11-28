#!/usr/bin/env python3
"""Minimal InfluxDB write sink for testing the node's InfluxDB push.

Accepts POST /api/v2/write (v2, Authorization: Token ...) and POST /write (v1),
answers 204, appends every body with a timestamp to ~/uwn-influx/received.lp,
parses the line protocol and prints a one-line summary + warnings.

  python3 tools/influx_sink.py [--port 8086] [--dir ~/uwn-influx]
Stdlib only.
"""
import argparse, gzip, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


def split_unescaped(s, sep, quoted=False, maxsplit=-1):
    """Split on sep, honouring backslash escapes (and "..." if quoted)."""
    out, cur, i, inq = [], [], 0, False
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            cur.append(c); cur.append(s[i + 1]); i += 2; continue
        if quoted and c == '"': inq = not inq
        if c == sep and not inq and (maxsplit < 0 or len(out) < maxsplit):
            out.append(''.join(cur)); cur = []
        else:
            cur.append(c)
        i += 1
    out.append(''.join(cur))
    return out


def parse_line(line, now_ns, warn):
    # sections separated by unescaped spaces: key fields [timestamp]
    parts = split_unescaped(line, ' ', quoted=True)
    parts = [p for p in parts]
    if len(parts) < 2:
        warn('no fields section (need "key fields [ts]")'); return None
    if '' in parts[:3]:
        warn('empty section: double space or unescaped space in measurement/tag')
        parts = [p for p in parts if p != '']
    if len(parts) > 3:
        warn('more than 3 space-separated sections: unescaped space in a tag value/key or measurement (%d sections)' % len(parts))
        return None
    key, fields = parts[0], parts[1]
    ts = parts[2] if len(parts) == 3 else None
    ks = split_unescaped(key, ',')
    meas, tags = ks[0], {}
    if not meas: warn('empty measurement')
    for t in ks[1:]:
        kv = split_unescaped(t, '=', maxsplit=1)
        if len(kv) != 2 or not kv[0]: warn('malformed tag %r (missing "=" or key; unescaped comma/space/equals?)' % t); continue
        if kv[1] == '': warn('tag %r has empty value (Influx rejects it)' % kv[0])
        if '=' in kv[1].replace('\\=', ''): warn('unescaped "=" in tag value %r' % kv[1])
        tags[kv[0]] = kv[1]
    flds = {}
    for f in split_unescaped(fields, ',', quoted=True):
        kv = split_unescaped(f, '=', quoted=True, maxsplit=1)
        if len(kv) != 2 or not kv[0]: warn('field without value / malformed: %r' % f); continue
        k, v = kv
        if v == '': warn('field %r has empty value' % k); continue
        try:
            if v.endswith('i') and v[:-1].lstrip('+-').isdigit(): val = int(v[:-1])
            elif v.endswith('u') and v[:-1].isdigit(): val = int(v[:-1])
            elif v.lower() in ('t', 'true', 'f', 'false'): val = v.lower() in ('t', 'true')
            elif v.startswith('"') and v.endswith('"') and len(v) >= 2: val = v[1:-1]
            else:
                val = float(v)
                if v.lower() in ('nan', 'inf', '+inf', '-inf'): warn('field %r is %s (not allowed)' % (k, v))
        except ValueError:
            warn('field %r: value %r does not parse' % (k, v)); continue
        flds[k] = val
    if not flds: warn('no valid fields')
    tsn = None
    if ts is not None:
        try:
            tsn = int(ts)
            diff = abs(tsn - now_ns) / 1e9
            if tsn < 1e17 or tsn > 1e19: warn('timestamp %s does not look like nanoseconds (s/ms/us?)' % ts)
            elif diff > 3600: warn('timestamp is %.0f s (%.1f h) away from now' % (diff, diff / 3600))
        except ValueError:
            warn('timestamp %r is not an integer' % ts)
    else:
        warn('no timestamp (server time would be used; the node always sends one)')
    return meas, tags, flds, tsn


class H(BaseHTTPRequestHandler):
    outdir = ''

    def log_message(self, *a): pass

    def do_POST(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n)
        if self.headers.get('Content-Encoding') == 'gzip': raw = gzip.decompress(raw)
        body = raw.decode('utf-8', 'replace')
        if u.path not in ('/api/v2/write', '/write'):
            self.send_response(404); self.end_headers(); print('404 for', self.path); return
        now = time.time(); now_ns = int(now * 1e9)
        with open(os.path.join(self.outdir, 'received.lp'), 'a') as f:
            f.write('# %s %s %s\n%s%s' % (time.strftime('%Y-%m-%dT%H:%M:%S'), u.path, self.client_address[0],
                                          body, '' if body.endswith('\n') else '\n'))
        auth = self.headers.get('Authorization', '')
        warns, summ = [], []
        if u.path == '/api/v2/write':
            if not auth.startswith('Token '): warns.append('v2 write without "Authorization: Token ..." (got %r)' % auth[:20])
            for p in ('org', 'bucket'):
                if p not in q: warns.append('query parameter %s missing' % p)
        ctype = self.headers.get('Content-Type', '')
        nlines = 0
        for ln, line in enumerate(body.split('\n'), 1):
            if not line.strip() or line.startswith('#'): continue
            nlines += 1
            w = lambda m, ln=ln: warns.append('line %d: %s' % (ln, m))
            r = parse_line(line, now_ns, w)
            if r:
                meas, tags, flds, ts = r
                summ.append('%s{%s} %d fields%s' % (meas, ','.join('%s=%s' % kv for kv in tags.items()), len(flds),
                            '' if ts is None else ' ts=%s' % time.strftime('%H:%M:%S', time.localtime(ts / 1e9))))
        if not body.endswith('\n'): warns.append('body does not end with a newline (ok for Influx)')
        print('%s POST %s %d bytes, %d line(s), ctype=%s auth=%s: %s' % (
            time.strftime('%H:%M:%S'), u.path, len(raw), nlines, ctype,
            ('Token ***' if auth.startswith('Token ') else repr(auth[:12])), ' | '.join(summ)), flush=True)
        for w in warns: print('   WARNING:', w, flush=True)
        self.send_response(204); self.end_headers()


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8086)
    ap.add_argument('--dir', default='~/uwn-influx')
    a = ap.parse_args()
    H.outdir = os.path.expanduser(a.dir); os.makedirs(H.outdir, exist_ok=True)
    print('influx sink on 0.0.0.0:%d, writing %s/received.lp (Ctrl-C to stop)' % (a.port, H.outdir), flush=True)
    ThreadingHTTPServer(('0.0.0.0', a.port), H).serve_forever()
