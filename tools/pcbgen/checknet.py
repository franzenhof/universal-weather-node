import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, find, get
from design import PARTS
import json
d = json.load(open(sys.argv[2]))
v = [x for s in d['sheets'] for x in s['violations']]
print("ERC violations:", len(v), sorted({(x['severity'], x['type']) for x in v}))
n = parse(open(sys.argv[1]).read())[0]
got = {}
for net in find(get(n, 'nets'), 'net'):
    name = get(net, 'name')[1]
    nodes = frozenset((get(x, 'ref')[1], get(x, 'pin')[1]) for x in find(net, 'node') if not get(x, 'ref')[1].startswith('#'))
    if nodes: got[name] = nodes
want = {}
for p in PARTS:
    for pin, net in p['pins'].items():
        if net: want.setdefault(net, set()).add((p['ref'], pin))
gotn = {k.lstrip('/'): v for k, v in got.items()}
bad = 0
for k, v in want.items():
    if gotn.get(k) != frozenset(v): bad += 1; print("MISMATCH", k, sorted(v), sorted(gotn.get(k, [])))
extra = [k for k, v in gotn.items() if k not in want and len(v) > 1]
print("nets wanted", len(want), "mismatches", bad, "extra multi-pin nets", extra)
