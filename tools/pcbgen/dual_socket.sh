#!/bin/bash
# dual_socket.sh <project dir> : V3/V4 double socket on the routed v2.0 board, in place.
# schematic (design.py) -> ERC -> netlist + pin-map check -> board edit + rip-up ->
# grid router for the open connections (rest of the routing untouched) -> silkscreen, back doc, DRC
set -e
G=$(cd "$(dirname "$0")" && pwd); D=$1; S=$G/out/dual; mkdir -p $S
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
PY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
NPY=${NPY:-python3}          # Python with numpy, scipy, pillow (route_grid.py)
N=universal-weather-node
cp $G/heltec_sockets.kicad_dru $D/$N.kicad_dru     # alternative V3 / V4 socket courtyards may overlap
SCHEM=${SCHEM:-schem_w} python3 -I $G/gen_sch2.py $D/$N.kicad_sch
$K sch erc --severity-all --format json -o $S/erc.json $D/$N.kicad_sch >/dev/null 2>&1
$K sch export netlist --format kicadsexpr -o $S/net.net $D/$N.kicad_sch >/dev/null 2>&1
python3 -I $G/checknet.py $S/net.net $S/erc.json
$PY $G/dual_socket.py prepare $D/$N.kicad_pcb $S/net.net $D/$N.uuids.json 2>&1 | grep -v -E "Fontconfig|traits|memory leak|destructor" || true
for pass in 1 2 3; do
  $K pcb drc --severity-all --refill-zones --format json -o $S/drc$pass.json $D/$N.kicad_pcb >/dev/null 2>&1
  $PY $G/dual_socket.py geometry $D/$N.kicad_pcb $S/drc$pass.json $S/geo$pass.json 2>&1 | grep -v -E "Fontconfig|traits|memory leak" || true
  python3 -I -c "import json,sys;sys.exit(0 if json.load(open('$S/geo$pass.json'))['conns'] else 1)" || break
  $NPY -I $G/route_grid.py $S/geo$pass.json $S/routes$pass.json | tail -3
  $PY $G/dual_socket.py apply $D/$N.kicad_pcb $S/routes$pass.json 2>&1 | grep -v -E "Fontconfig|traits|memory leak" || true
done
cp $D/$N.kicad_pcb $S/routed-before-cleanup.kicad_pcb
# dangling stubs / vias left by the rip-up (KiCad's own DRC decides); a round that would
# open a connection is undone
uc() { python3 -I -c "import json;print(len(json.load(open('$1'))['unconnected_items']))"; }
$K pcb drc --severity-all --refill-zones --format json -o $S/prune.json $D/$N.kicad_pcb >/dev/null 2>&1
for pass in 1 2 3; do
  before=$(uc $S/prune.json); cp $D/$N.kicad_pcb $S/prune-undo.kicad_pcb
  $PY $G/dual_socket.py prune $D/$N.kicad_pcb $S/prune.json 2>&1 | grep -E "^dangling" || break
  $K pcb drc --severity-all --refill-zones --format json -o $S/prune.json $D/$N.kicad_pcb >/dev/null 2>&1
  if [ "$(uc $S/prune.json)" -gt "$before" ]; then cp $S/prune-undo.kicad_pcb $D/$N.kicad_pcb; echo "prune round $pass undone"; break; fi
done
# clearance repair: rip up routed copper in clearance violations, re-route with a wider raster margin
for fix in 1 2 3; do
  $K pcb drc --severity-all --refill-zones --format json -o $S/fix.json $D/$N.kicad_pcb >/dev/null 2>&1
  $PY $G/dual_socket.py ripviol $D/$N.kicad_pcb $S/fix.json 2>&1 | grep -E "^clearance" || break
  for pass in 1 2; do
    $K pcb drc --severity-all --refill-zones --format json -o $S/fixdrc.json $D/$N.kicad_pcb >/dev/null 2>&1
    $PY $G/dual_socket.py geometry $D/$N.kicad_pcb $S/fixdrc.json $S/fixgeo.json 2>&1 | grep -E "^geometry" || true
    python3 -I -c "import json,sys;sys.exit(0 if json.load(open('$S/fixgeo.json'))['conns'] else 1)" || break
    ROUTE_MARGIN=0.07 $NPY -I $G/route_grid.py $S/fixgeo.json $S/fixroutes.json | grep -E "^FAILED" || true
    $PY $G/dual_socket.py apply $D/$N.kicad_pcb $S/fixroutes.json 2>&1 | grep -E "^applied" || true
  done
done
$K pcb drc --severity-all --refill-zones --format json -o $S/prune.json $D/$N.kicad_pcb >/dev/null 2>&1
for pass in 1 2 3; do            # second guarded prune after the repair
  before=$(uc $S/prune.json); cp $D/$N.kicad_pcb $S/prune-undo.kicad_pcb
  $PY $G/dual_socket.py prune $D/$N.kicad_pcb $S/prune.json 2>&1 | grep -E "^dangling" || break
  $K pcb drc --severity-all --refill-zones --format json -o $S/prune.json $D/$N.kicad_pcb >/dev/null 2>&1
  if [ "$(uc $S/prune.json)" -gt "$before" ]; then cp $S/prune-undo.kicad_pcb $D/$N.kicad_pcb; echo "prune round $pass undone"; break; fi
done
LAYOUT=layout_w1 $PY $G/finish.py $D/$N.kicad_pcb fix 2>&1 | grep -v -E "Fontconfig|traits|memory leak|destructor" || true
$PY $G/dual_socket.py silk $D/$N.kicad_pcb 2>&1 | grep -v -E "Fontconfig|traits|memory leak" || true
$PY $G/backdoc.py $D/$N.kicad_pcb 2>&1 | grep -v -E "Fontconfig|traits|memory leak|destructor" | grep -v "^Back doc" || true
$PY $G/refplace.py $D/$N.kicad_pcb --fix 2>&1 | grep -E "^(moved|still)" || true   # hidden / clipped references
$K pcb drc --schematic-parity --severity-all --refill-zones --format json -o $S/drc.json $D/$N.kicad_pcb >/dev/null 2>&1
python3 -I $G/drc.py $S/drc.json 60
