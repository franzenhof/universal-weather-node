"""Build the preview-matched JLCPCB CPL from raw KiCad positions and the BOM."""

import argparse
import csv
from pathlib import Path

# SOIC rule: Fabrication-Toolkit, commit 7f627b6be76081b962161b9eb9fe3a326a18c915,
# plugins/transformations.csv (^SOIC-, +270 degrees). Not an assembly approval.
SOIC_CANDIDATES = {
    "U1": ("C2879422", "Package_SO:SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.514x3.2mm", 0),
    "U4": ("C190783", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", 180),
}
SOT6_CANDIDATES = {
    "U2": ("C12401", "Package_TO_SOT_SMD:SOT-23-6", 90),
    "U3": ("C12401", "Package_TO_SOT_SMD:SOT-23-6", 180),
}
SC70_CANDIDATES = {
    "U5": ("C350557", "Package_TO_SOT_SMD:SOT-353_SC-70-5", 270),
}
DFN_CANDIDATES = {
    "Q3": ("C431191", "Package_SO:Vishay_PowerPAK_1212-8_Single", 270),
}
CANDIDATES = {**SOIC_CANDIDATES, **SOT6_CANDIDATES, **SC70_CANDIDATES, **DFN_CANDIDATES}
SOT23_PARTS = {"C15127", "C20917", "C37130", "C20526", "C49338", "C7420333"}
# C7420333 (R+O BAT54S) follows the same +180 degrees rule: 2026-11-06 10:19:27 UTC preview
SOT23_PENDING = set()
NEW_PARTS = {
    "Q4": ("C154901", "Package_TO_SOT_SMD:SOT-223-3_TabPin2", 90),
}
Q7_LEAD_SHIFT = 1.4
CPL_FIELDS = ["Designator", "Mid X", "Mid Y", "Layer", "Rotation"]
REVIEW_FIELDS = ["Designator", "LCSC Part #", "Footprint", "KiCad Rotation",
                 "Candidate Offset", "Candidate Rotation", "Status", "Check"]


def build(raw_rows, bom_rows):
    parts = {}
    for row in bom_rows:
        for ref in row["Designator"].split(","):
            ref = ref.strip()
            if ref in parts:
                raise ValueError(f"Duplicate BOM reference: {ref}")
            parts[ref] = row
    placements, review, seen = [], [], set()
    for row in raw_rows:
        ref = row["Designator"]
        if ref in seen:
            raise ValueError(f"Duplicate placement: {ref}")
        seen.add(ref)
        if row["Layer"] != "Top":
            raise ValueError(f"{ref}: only top-side review is supported")
        part = parts[ref]
        angle = float(row["Rotation"])
        offset = 0
        status = "MANUAL_CHECK"
        check = "Verify package pads and pin 1 / polarity in JLCPCB"
        if ref in CANDIDATES:
            lcsc, footprint, raw_angle = CANDIDATES[ref]
            if (part["LCSC Part #"], part["Footprint"]) != (lcsc, footprint):
                raise ValueError(f"{ref}: part/footprint changed; reassess correction")
            if angle % 360 != raw_angle:
                raise ValueError(f"{ref}: expected raw KiCad angle {raw_angle}; "
                                 "input already corrected or board changed")
            offset = 270
            status = "PREVIEW_MATCHED_DFN" if ref == "Q3" else "PREVIEW_MATCHED_IC"
            check = {"U1": "Pin 1 upper left", "U4": "Pin 1 lower right",
                     "U2": "Pin 1 lower left", "U3": "Pin 1 lower right",
                     "U5": "Pin 1 upper right; three leads top, two bottom",
                     "Q3": "Pin 1/source upper right; pin 4/gate upper left; drain pins 5-8 bottom"}[ref]
            check += "; board top view with sensor terminals at bottom"
        elif ref in NEW_PARTS:
            lcsc, footprint, raw_angle = NEW_PARTS[ref]
            if (part["LCSC Part #"], part["Footprint"], angle % 360) != (lcsc, footprint, raw_angle):
                raise ValueError(f"{ref}: new part/footprint/angle changed; reassess placement")
            offset = 180
            status = "PREVIEW_MATCHED_SOT223"
            check = ("2026-11-04 19:31:57 UTC preview confirms +180 degrees: "
                     "gate/pin 1 lower left, source/pin 3 lower right, drain tab top; "
                     "board top view with sensor terminals at bottom")
        elif (part["Footprint"] == "Package_TO_SOT_SMD:SOT-23"
              and part["LCSC Part #"] in SOT23_PARTS):
            offset = 180
            if part["LCSC Part #"] in SOT23_PENDING:
                status = "NEEDS_PREVIEW_SOT23"
                check = "New BAT54S article (R+O): pin 1 must match PCB triangle; confirm +180 degrees"
            else:
                status = "PREVIEW_MATCHED_SOT23"
                check = "Pin 1 must match PCB triangle; verify all three leads on pads"
        elif ref.startswith("R") or (
                ref.startswith("C") and part["Footprint"].startswith("Capacitor_SMD:C_")):
            status = "NONPOLAR_CHECK"
            check = "Both terminals must align; 180 degrees is electrically equivalent"
        elif ref == "D1":
            status = "BIDIRECTIONAL_TVS"
            check = "No polarity; verify CA suffix and package alignment"
        elif ref in ("D5", "D6", "D7", "D14", "D20", "D29", "D22", "D23",
                     "D24", "D26", "D31"):
            status = "PREVIEW_MATCHED_POLARITY"
            check = "Cathode band matches PCB pad 1; retain article and verify final order"
        elif ref == "Q7":
            if (part["LCSC Part #"], angle % 360) != ("C130104", 90):
                raise ValueError(f"{ref}: TO-252 part/angle changed; reassess centroid shift")
            status = "PREVIEW_MATCHED_TO252"
            check = ("Tab top, gate/pin 1 lower left. JLC's model sits 1.4 mm towards the tab "
                     "(2026-11-06 previews: leads only touch the pad edge, metal tab beyond the "
                     "tab pad): centroid moved 1.4 mm towards the leads; verify in the final order")
        elif ref == "C31":
            if (part["LCSC Part #"], part["Footprint"], angle % 360) != (
                    "C445452", "Capacitor_SMD:CP_Elec_8x10", 180):
                raise ValueError(f"{ref}: capacitor part/footprint/angle changed; reassess polarity")
            offset = 180
            status = "PREVIEW_MATCHED_POLARITY"
            check = ("2026-11-05 11:15:14 UTC preview confirms plus/pad 1 right, minus left; "
                     "+180 degrees correction, raw 180 -> CPL 0; verify final order")
        elif ref == "U7":
            if (part["LCSC Part #"], part["Footprint"], angle % 360) != (
                    "C51118", "Package_TO_SOT_SMD:SOT-23-5", 90):
                raise ValueError(f"{ref}: LDO part/footprint/angle changed; reassess placement")
            offset = 270
            status = "PREVIEW_MATCHED_SOT23_5"
            check = ("2026-11-06 09:19:56 UTC preview showed the raw angle 90 degrees off (three "
                     "leads right, pin 1 lower right); +270 degrees, raw 90 -> CPL 0. The 09:27:10 UTC "
                     "preview confirms pin 1/VIN lower left, three leads bottom (VIN, GND, EN), two top "
                     "(NC, VOUT)")
        elif ref == "C6":
            status = "PREVIEW_MATCHED_POLARITY"
            check = "Plus bottom, minus top; do not apply generic electrolytic offset"
        elif ref == "JP12":
            status = "THT_MANUAL"
            check = "Through-hole header; fit shunt for open-collector PC fan tacho"
        elif ref.startswith("F") or ref == "L1":
            status = "NONPOLAR_CHECK"
            check = "No electrical polarity; verify selected package and terminal alignment"
        rotation = (angle + offset) % 360
        placement = dict(row)
        placement["Rotation"] = f"{rotation:.6f}"
        if ref == "Q7":
            # leads point down on the board (raw 90 degrees); the CPL Y axis points up
            placement["Mid Y"] = f"{float(row['Mid Y'].removesuffix('mm')) - Q7_LEAD_SHIFT:.6f}mm"
        placements.append(placement)
        review.append(dict(zip(REVIEW_FIELDS, [
            ref, part["LCSC Part #"], part["Footprint"], row["Rotation"],
            offset, placement["Rotation"], status, check,
        ])))
    missing = (CANDIDATES.keys() | NEW_PARTS.keys()) - seen
    if missing:
        raise ValueError(f"Missing corrected placements: {sorted(missing)}")
    return placements, review


def write_csv(path, fields, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_cpl", type=Path)
    parser.add_argument("bom", type=Path)
    parser.add_argument("candidate_cpl", type=Path)
    args = parser.parse_args()
    paths = [p.resolve() for p in (
        args.raw_cpl, args.bom, args.candidate_cpl)]
    if len(set(paths)) != len(paths):
        raise ValueError("Input and output paths must be distinct")
    with args.raw_cpl.open(newline="") as stream:
        raw = list(csv.DictReader(stream))
    with args.bom.open(newline="") as stream:
        bom = list(csv.DictReader(stream))
    placements, _ = build(raw, bom)
    write_csv(args.candidate_cpl, CPL_FIELDS, placements)
    print(f"CPL: {len(placements)} placements, five previously reviewed ICs and Q3 +270 degrees, "
          "article-matched SOT-23 parts +180 degrees; "
          "Q4 +180 degrees confirmed in the 2026-11-04 19:31:57 UTC preview; "
          "Q7 centroid 1.4 mm towards the leads; "
          "External 5V input and U6 removed. Check the final order preview. "
          "Hardware qualification remains open.")


if __name__ == "__main__":
    main()
