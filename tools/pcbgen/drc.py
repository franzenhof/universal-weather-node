import json, sys
from collections import Counter
d = json.load(open(sys.argv[1]))
for k in ('violations', 'unconnected_items', 'schematic_parity'):
    v = d.get(k, []); print(k, len(v), Counter((x['severity'], x['type']) for x in v).most_common(12))
    for x in v[:int(sys.argv[2]) if len(sys.argv) > 2 else 0]:
        if x['severity'] == 'error': print('   ', x['type'], x['description'], [i['description'] for i in x['items']][:2])
