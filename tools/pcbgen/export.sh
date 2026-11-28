#!/bin/bash
# export.sh <gen dir> <repo pcb dir, e.g. pcb/v4-full> <name, e.g. universal-weather-node-v4-full>
set -e
G=$1; T=$2; N=$3
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/out
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
PY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
mkdir -p "$WORK"
mkdir -p $T/production
cp $G/proj/$N.kicad_pcb $G/proj/$N.kicad_sch $T/
[ -f $G/proj/$N.kicad_pro ] && cp $G/proj/$N.kicad_pro $T/
[ -f $G/proj/$N.kicad_dru ] && cp $G/proj/$N.kicad_dru $T/
cd $T
rm -rf production/gerber; mkdir -p production/gerber
$K pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts -o production/gerber/ $N.kicad_pcb >/dev/null 2>&1
$K pcb export drill --format excellon --drill-origin absolute --excellon-units mm -o production/gerber/ $N.kicad_pcb >/dev/null 2>&1
rm -f production/$N-gerber.zip; (cd production/gerber && zip -q -r ../$N-gerber.zip .); rm -r production/gerber
$K pcb export pos --format csv --units mm --side front --exclude-dnp -o "$WORK/export-pos-$N.csv" $N.kicad_pcb >/dev/null 2>&1
$PY - "$WORK/export-pos-$N.csv" $N "$WORK/export-raw-$N.csv" <<'PYEOF'
import csv, sys, re, pcbnew
rows=list(csv.DictReader(open(sys.argv[1]))); N=sys.argv[2]
b=pcbnew.LoadBoard(N+'.kicad_pcb'); info={}; smd=set()
for f in b.GetFootprints():
    r=f.GetReference()
    if r.startswith(('H','TP')) or f.IsDNP() or f.IsExcludedFromBOM(): continue
    info[r]=(f.GetValue(), str(f.GetFPID().GetUniStringLibId()), f.GetFieldText('LCSC') if f.HasField('LCSC') else '', f.GetFieldText('MPN') if f.HasField('MPN') else '')
    pads=list(f.Pads())
    if pads and all(p.GetAttribute()==pcbnew.PAD_ATTRIB_SMD for p in pads): smd.add(r)
with open(sys.argv[3],'w',newline='') as fh:
    w=csv.writer(fh); w.writerow(['Designator','Mid X','Mid Y','Layer','Rotation']); n=0
    for r in rows:
        if r['Ref'] not in smd: continue
        w.writerow([r['Ref'], r['PosX']+'mm', r['PosY']+'mm', 'Top' if r['Side']=='top' else 'Bottom', r['Rot']]); n+=1
key=lambda s:(re.match(r'[A-Z]+',s).group(), int(re.search(r'\d+',s).group()))
# JLCPCB assembles the SMD parts only; through-hole parts go to a separate LCSC list for hand soldering
groups={}; tht={}
for r,v in info.items():
    if r in smd: groups.setdefault(v,[]).append(r)
    else: tht.setdefault(v[1:],[]).append(r)
order=lambda g: sorted(g.items(), key=lambda kv: key(sorted(kv[1],key=key)[0]))
with open(f'production/{N}-bom-jlcpcb.csv','w',newline='') as fh:
    w=csv.writer(fh, quoting=csv.QUOTE_ALL); w.writerow(["Comment","Designator","Footprint","LCSC Part #","MPN"])
    for (v,fp,lc,mpn),refs in order(groups):
        w.writerow([v, ",".join(sorted(refs,key=key)), fp, lc, mpn])
with open(f'production/{N}-tht-lcsc.csv','w',newline='') as fh:
    w=csv.writer(fh, quoting=csv.QUOTE_ALL); w.writerow(["LCSC Part #","MPN","Qty per board","Designator","Use","Footprint"])
    for (fp,lc,mpn),refs in order(tht):
        refs=sorted(refs,key=key)
        w.writerow([lc, mpn, len(refs), ",".join(refs), "; ".join(dict.fromkeys(info[r][0] for r in refs)), fp])
print('CPL SMD placements', n, 'BOM lines', len(groups), 'THT lines (LCSC list)', len(tht),
      'missing LCSC', [r for r,v in info.items() if not v[2]])
PYEOF
$PY -B "$HERE/jlcpcb_cpl.py" \
    "$WORK/export-raw-$N.csv" \
    "production/$N-bom-jlcpcb.csv" \
    "production/$N-cpl-jlcpcb.csv"
rm -f "$WORK/export-raw-$N.csv"
rm -f "$WORK/export-pos-$N.csv"
$PY $HERE/models3d.py $N.kicad_pcb 2>/dev/null | grep '^3D models' || true
$K sch export pdf -o $N-schematic.pdf $N.kicad_sch >/dev/null 2>&1
for side in top bottom; do $K pcb render --side $side --width 1800 --height 1450 --quality high --zoom 0.92 -o $N-$side.png $N.kicad_pcb >/dev/null 2>&1; done
$K pcb render --width 1800 --height 1350 --quality high --rotate '-50,0,-30' --zoom 0.90 -o $N-3d.png $N.kicad_pcb >/dev/null 2>&1
$K pcb drc --schematic-parity --severity-all --format json -o "$WORK/export-drc-$N.json" $N.kicad_pcb >/dev/null 2>&1
python3 -I -c "
import json;d=json.load(open('$WORK/export-drc-$N.json'))
print('DRC errors', sum(1 for k in ('violations','unconnected_items','schematic_parity') for x in d.get(k,[]) if x['severity']=='error'))"
rm -f "$WORK/export-drc-$N.json"
