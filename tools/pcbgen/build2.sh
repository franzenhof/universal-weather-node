#!/bin/bash
# build.sh <outdir> : schematic -> ERC -> netlist -> board (placed) -> DRC
set -e
G=$(cd "$(dirname "$0")" && pwd); D=$1; S=${OUT:-$G/out}; mkdir -p $S
export LAYOUT=${LAYOUT:-layout_w1} SCHEM=${SCHEM:-schem_w} NOPLANE=${NOPLANE:-1} JITTER=${JITTER:-1}
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
PY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
N=${N:-universal-weather-node}
SCHEM=${SCHEM:-schem_w} python3 -I $G/gen_sch2.py $D/$N.kicad_sch
$K sch erc --severity-all --format json -o $S/erc.json $D/$N.kicad_sch 2>/dev/null | tail -1
$K sch export netlist --format kicadsexpr -o $S/net.net $D/$N.kicad_sch 2>/dev/null | tail -1
python3 -I $G/checknet.py $S/net.net $S/erc.json
rm -f $D/$N.kicad_pcb
$PY $G/gen_pcb2.py $D/$N.kicad_pcb $D/$N.uuids.json $S/net.net 2>&1 | grep -v -E "Fontconfig|traits" | cut -c1-200
# --- autoroute (Freerouting), GND pours, DRC: independent attempts run in parallel ---
# Freerouting's autorouter is essentially single-threaded and needs ~2.5 GB peak, so
# PARALLEL (default 4) attempts run side by side, each in its own directory with a
# copy of the project; the attempt with the fewest DRC errors is kept.
R=$S/route; rm -rf $R; mkdir -p $R
cp $D/$N.kicad_pcb $R/placed.kicad_pcb
ATTEMPTS=${ATTEMPTS:-12}; PARALLEL=${PARALLEL:-4}
attempt() {
  i=$1; A=$R/a$i; mkdir -p $A
  cp $R/placed.kicad_pcb $A/$N.kicad_pcb; cp $D/$N.kicad_sch $D/$N.kicad_pro $A/ 2>/dev/null
  if [ "${JITTER:-0}" = "1" ] && [ "$i" != "1" ]; then     # varied placement for every further attempt
    SEED=$((i + ${SEED_BASE:-0})) $PY $G/gen_pcb2.py $A/$N.kicad_pcb $D/$N.uuids.json $S/net.net > $A/place.log 2>&1
  fi
  $PY $G/finish.py $A/$N.kicad_pcb - > $A/finish0.log 2>&1
  $PY -c "import pcbnew; b=pcbnew.LoadBoard('$A/$N.kicad_pcb'); pcbnew.ExportSpecctraDSN(b,'$A/board.dsn')" 2>/dev/null
  (cd $A && java -Xmx2600m -Djava.awt.headless=true -jar ${FREEROUTING_JAR:-$G/freerouting-2.5.0.jar} --gui.enabled=false -de board.dsn -do board.ses -mp 100 -mt 2 -l en > fr.log 2>&1)
  $PY $G/finish.py $A/$N.kicad_pcb $A/board.ses > $A/finish.log 2>&1
  $K pcb drc --schematic-parity --severity-all --refill-zones --format json -o $A/drc.json $A/$N.kicad_pcb >/dev/null 2>&1
  python3 -I -c "import json;d=json.load(open('$A/drc.json'));print(sum(1 for k in ('violations','unconnected_items','schematic_parity') for x in d.get(k,[]) if x['severity']=='error'))" > $A/errs 2>/dev/null || echo 999 > $A/errs
  echo "routing attempt $i: DRC errors = $(cat $A/errs)"
}
export -f attempt; export R N D G PY K S SEED_BASE
seq 1 $ATTEMPTS | xargs -P $PARALLEL -I{} bash -c 'attempt {}'
best=$(for A in $R/a*; do echo "$(cat $A/errs) $A"; done | sort -n | head -1)
bestA=${best#* }; echo "kept best attempt: DRC errors = ${best%% *} ($bestA)"
cp $bestA/$N.kicad_pcb $D/$N.kicad_pcb; cp $bestA/drc.json $S/drc.json; cp $bestA/fr.log $R/fr.log
$PY $G/brand.py $D/$N.kicad_pcb 2>&1 | grep -v -E "Fontconfig|traits"
$K pcb drc --schematic-parity --severity-all --refill-zones --format json -o $S/drc.json $D/$N.kicad_pcb >/dev/null 2>&1
python3 -I $G/drc.py $S/drc.json 20
