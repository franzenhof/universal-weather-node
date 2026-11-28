#!/usr/bin/env python3
"""Strict check of the Prometheus text exposition format served at /metrics.

Usage: python3 tools/check_metrics.py [URL-or-file]   (default http://172.24.10.107/metrics)
Exit 1 on errors. Stdlib only.
"""
import re, sys, urllib.request

NAME = re.compile(r'^[a-zA-Z_:][a-zA-Z0-9_:]*$')
LABEL = re.compile(r'^[a-zA-Z_][a-zA-Z0-9_]*$')
TYPES = {'counter', 'gauge', 'histogram', 'summary', 'untyped'}
# Deviations from "SI base units" that exist in src/metrics.cpp (notes, not errors).
KNOWN_NON_BASE = {'_hpa': 'pressure in hPa (Prometheus convention: pascals)',
                  '_mm': 'millimetres (convention: meters)',
                  '_mm_per_hour': 'mm/h (convention: meters per second)',
                  '_km': 'kilometres (convention: meters)',
                  '_uscm': 'uS/cm (not SI base)',
                  '_ugm3': 'ug/m3 (not SI base)',
                  '_percent': 'percent (convention: ratio 0..1)',
                  '_dbm': 'dBm (no SI base unit)',
                  '_volts': 'ok (SI)'}
VALUE = re.compile(r'^[+-]?(Inf|NaN|(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)$')


def parse_labels(s, ln, err):
    """s starts after '{'; return (dict, rest) or None."""
    labels, i, n = {}, 0, len(s)
    while True:
        while i < n and s[i] == ' ': i += 1
        if i < n and s[i] == '}':
            return labels, s[i + 1:]
        m = re.match(r'[a-zA-Z_][a-zA-Z0-9_]*', s[i:])
        if not m:
            err(ln, 'bad label name near %r' % s[i:i + 15]); return None
        k = m.group(0); i += len(k)
        if k.startswith('__'): err(ln, 'reserved label name %r' % k)
        if s[i:i + 2] != '="':
            err(ln, 'expected =" after label %r' % k); return None
        i += 2; v = []
        while True:
            if i >= n: err(ln, 'unterminated label value'); return None
            c = s[i]
            if c == '\\':
                if i + 1 >= n or s[i + 1] not in '\\"n':
                    err(ln, 'invalid escape in label value'); return None
                v.append({'n': '\n'}.get(s[i + 1], s[i + 1])); i += 2
            elif c == '"': i += 1; break
            else: v.append(c); i += 1
        if k in labels: err(ln, 'duplicate label %r' % k)
        labels[k] = ''.join(v)
        if i < n and s[i] == ',': i += 1; continue
        if i < n and s[i] == '}': return labels, s[i + 1:]
        err(ln, 'expected , or } after label value'); return None


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'http://172.24.10.107/metrics'
    if src.startswith('http'):
        with urllib.request.urlopen(src, timeout=15) as r:
            ctype = r.headers.get('Content-Type', '')
            text = r.read().decode('utf-8')
        print('Content-Type:', ctype)
        if not ctype.startswith('text/plain'): print('WARN: Content-Type is not text/plain')
    else:
        text = open(src, encoding='utf-8').read()
    errors, notes = [], []
    err = lambda ln, m: errors.append('line %d: %s' % (ln, m))
    if not text.endswith('\n'): errors.append('body does not end with a newline')
    helps, types, samples_per, seen, order, started = {}, {}, {}, set(), [], set()
    lines = text.split('\n')
    if text.endswith('\n'): lines = lines[:-1]
    for ln, line in enumerate(lines, 1):
        if line == '': err(ln, 'empty line'); continue
        if line.startswith('#'):
            p = line.split(' ', 3)
            if len(p) >= 2 and p[1] == 'HELP':
                if len(p) < 3 or not NAME.match(p[2]): err(ln, 'bad HELP'); continue
                if p[2] in helps: err(ln, 'second HELP for ' + p[2])
                if p[2] in started: err(ln, 'HELP after first sample of ' + p[2])
                helps[p[2]] = p[3] if len(p) > 3 else ''
            elif len(p) >= 2 and p[1] == 'TYPE':
                if len(p) != 4 or not NAME.match(p[2]): err(ln, 'bad TYPE'); continue
                if p[3] not in TYPES: err(ln, 'invalid TYPE %r' % p[3])
                if p[2] in types: err(ln, 'second TYPE for ' + p[2])
                if p[2] in started: err(ln, 'TYPE after first sample of ' + p[2])
                types[p[2]] = p[3]; order.append(p[2])
            continue
        name = re.match(r'^([^\s{]+)', line).group(1)
        if not NAME.match(name): err(ln, 'invalid metric name %r' % name); continue
        rest = line[len(name):]; labels = {}
        if rest.startswith('{'):
            r = parse_labels(rest[1:], ln, err)
            if r is None: continue
            labels, rest = r
        if not rest.startswith(' '): err(ln, 'no space before value'); continue
        parts = rest[1:].split(' ')
        if len(parts) > 2 or not parts[0]: err(ln, 'bad value/timestamp part %r' % rest); continue
        if not VALUE.match(parts[0]): err(ln, 'value does not parse: %r' % parts[0])
        if len(parts) == 2 and not re.match(r'^-?\d+$', parts[1]): err(ln, 'bad timestamp')
        fam = name
        if name not in types:
            for suf in ('_count', '_sum', '_bucket'):
                if name.endswith(suf) and name[:-len(suf)] in types: fam = name[:-len(suf)]
        if fam not in types: err(ln, 'sample of %s without preceding TYPE' % name)
        if fam not in helps: err(ln, 'sample of %s without preceding HELP' % name)
        started.add(fam)
        key = (name, tuple(sorted(labels.items())))
        if key in seen: err(ln, 'duplicate series %s%s' % (name, labels))
        seen.add(key); samples_per[fam] = samples_per.get(fam, 0) + 1
        if types.get(fam) == 'counter' and not name.endswith('_total'):
            err(ln, 'counter %s should end in _total' % name)
        if types.get(fam) == 'gauge' and name.endswith('_total'):
            notes.append('gauge %s ends in _total (reserved for counters)' % name)
        for k, v in labels.items():
            if v == '': notes.append('%s: empty label value %s="" (Prometheus treats it as absent label)' % (name, k))
    for f in types:
        if f not in samples_per: errors.append('family %s has TYPE but no samples' % f)
        if f not in helps: errors.append('family %s has no HELP' % f)
        for suf, why in KNOWN_NON_BASE.items():
            if f.endswith(suf) and not why.startswith('ok'):
                notes.append('%s: %s' % (f, why)); break
    print('families: %d, samples: %d' % (len(types), sum(samples_per.values())))
    for f in order: print('  %-36s %-8s %d sample(s)' % (f, types[f], samples_per.get(f, 0)))
    if notes:
        print('Notes (not errors):')
        for n in sorted(set(notes)): print('  -', n)
    if errors:
        print('ERRORS (%d):' % len(errors))
        for e in errors: print('  -', e)
        sys.exit(1)
    print('OK: exposition format valid')

main()
