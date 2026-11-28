#!/bin/bash
# export_light.sh <repo pcb dir, e.g. pcb/v4-light> [name] : Gerber/drill ZIP, hand-assembly BOM,
# schematic PDF, renders and final DRC of the Weather Node V4 Light (all THT, no assembly files)
set -e
T=$1; N=${2:-universal-weather-node-v4-light}
HERE=$(cd "$(dirname "$0")" && pwd); WORK=$HERE/out; mkdir -p "$WORK"
K=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
cd "$T"; mkdir -p production
G=$WORK/gerber-$N; rm -rf "$G"; mkdir -p "$G"
$K pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts -o "$G/" $N.kicad_pcb >/dev/null 2>&1
$K pcb export drill --format excellon --drill-origin absolute --excellon-units mm -o "$G/" $N.kicad_pcb >/dev/null 2>&1
rm -f production/$N-gerber.zip; (cd "$G" && zip -q -r - .) > production/$N-gerber.zip; rm -r "$G"
python3 -I "$HERE/bom_light.py" production/$N-bom.csv
$K sch export pdf -o $N-schematic.pdf $N.kicad_sch >/dev/null 2>&1
for side in top bottom; do $K pcb render --side $side --width 1800 --height 1400 --quality high --zoom 0.92 -o $N-$side.png $N.kicad_pcb >/dev/null 2>&1; done
$K pcb render --width 1800 --height 1350 --quality high --rotate '-50,0,-30' --zoom 0.90 -o $N-3d.png $N.kicad_pcb >/dev/null 2>&1
$K pcb drc --schematic-parity --severity-all --refill-zones --format json -o "$WORK/export-drc-$N.json" $N.kicad_pcb >/dev/null 2>&1
python3 -I "$HERE/drc.py" "$WORK/export-drc-$N.json" 3
