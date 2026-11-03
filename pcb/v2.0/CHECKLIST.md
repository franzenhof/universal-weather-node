# PCB v2.0 – Assembly and Bring-up Checklist

Nothing on this board has been tested on real hardware yet. Use this list for the
JLCPCB order and for the first prototypes (order 5, test, then more). Current layout:
100 × 100 mm, with a 92 × 92 mm mounting-hole pattern (M3 holes, centres 4 mm
from each adjacent board edge).

**Production exports refreshed on 2026-11-03:** the Gerber/drill ZIP and JLCPCB
BOM/CPL were generated from the current verified PCB, including the revised J17
position, corner mounting holes, separate JP1/JP11 headers, D32 UV-input TVS
and revised VIN/5 V reverse-polarity protection, buck layout/BOM corrections,
hardware copper keepouts, high-current via groups and the detailed back-side
operating reference.
Final DRC: **0 errors, 0 unconnected items, 0 schematic-parity issues**;
nine known terminal silkscreen-edge warnings remain.
BOM: 146 populated references in 65 groups, all with LCSC and MPN fields.
CPL: 125 top-side SMD placements, checked against PCB positions and rotations;
the 21 THT components are in the BOM but not the CPL and require separate/manual
assembly arrangements. DNP parts are excluded from both files.
KiCad rotations are not a guarantee of JLCPCB library orientation: complete the
assembly preview checks below before paying. Availability, price and actual
hardware performance have not been qualified.

## A. JLCPCB assembly preview (before paying)

The back silkscreen contains a standalone 0.8 mm operating reference: power/USB
restrictions, connector pin numbers, jumper positions, voltage/current limits,
analog scales, GPIOs, test points, startup and mounting notes. It is readable
from the back; numbered pinouts are not the left-to-right order seen from below.
Keep this documentation and the existing labels beside the connector pins in
the fabrication preview. Do not shrink it below 0.8 mm to accommodate substitutions.

JLCPCB rotates parts according to its own library, which often differs from KiCad for
polarized parts, connectors and less common packages. Check every line in the
*parts placement* preview and correct with the rotate buttons (or leave a note).

| Parts | What to check |
|-------|---------------|
| D1 (SMBJ30CA), D4 (SMBJ5.0CA) – SMB, bidirectional | No polarity; ignore the standard footprint's cathode silk bar. Verify the **CA** suffix: unidirectional A parts recreate a reverse-polarity short. D1: 30 V standoff / 48.4 V max clamp; D4: 5 V / 9.2 V |
| D24 (SMBJ30A), D5–D7, D14, D20, D29 (SMLVT3V3) – SMB, unidirectional | Cathode band on the silkscreen bar (KiCad pad 1 = cathode) |
| D22, D23 (B360A-13-F, C94193) – SMA | 3 A / 60 V, replacing the 40 V SS34 parts that were below D1's 48.4 V maximum clamp. Cathode band on KiCad pad 1 / silkscreen bar; former D3 is removed |
| D26 (BZT52C12) – SOD-123 | Cathode band on the bar (gate clamp of Q4) |
| D8, D9, D11–D13, D15, D18, D19, D25, D30 (BAT54S), D16, D17, D27, D28, D32 (PESD5V0S2BT) – SOT-23 | Pin 1 at the silkscreen mark, all three pins on their pads. D32 pin 1=UV SIGNAL, pin 3=GND, pin 2 unused |
| Q1, Q6 (AO3401A), Q2 (AO3400A), Q5 (AO3422) – SOT-23 | Pin 1 (gate) at the mark |
| Q9 (AO3401A) – SOT-23 | Pin 1=5V_GATE, pin 2/source=5V_PROT to JP8 pin 3, pin 3/drain=5V_IN_F from F2/D4. Do not interchange source and drain |
| Q7 (NCE60P50K) – TO-252-3 | Verify pad 2/drain goes to VIN_F and source pad 3 goes to +VLOAD; reverse-polarity protection only, not an overvoltage blocker |
| Q4 (NCE60P50K) – TO-252-3 | Verify source pad 3 goes to the selected fan supply and drain pad 2 goes to FAN_SW |
| Q3 (AON7264E) – DFN 3.3 × 3.3 | Pins 1–3 source, pin 4 gate on the small-pad side, the large pad (drain) towards F5 |
| U1 (LMR36520ADDAR) – SOIC-8 PowerPAD, U2/U3 (74LVC2G14) – SOT-23-6, U4 (ADuM121N) – SOIC-8, U5 (74AHCT1G125) – SC-70-5 | Pin-1 mark and placement; U1 pins 1–8: PGND, VIN, EN, PG (unused), FB, VCC, BOOT, SW; EP9=GND |
| C6 (100 µF electrolytic) | "+" towards the + mark |
| J3, J9–J13 (bottom edge) | Wire openings face the **bottom** board edge |
| J5, J7 (right edge) | Wire openings face the **right** board edge |
| J17 (UV sensor, right edge above mounting hole) | Pin 1 is at (94.5, 81) mm from the top-left board corner, 19 mm above the bottom edge. Wire opening faces the **right** board edge; pin order by terminal number is GND, SIGNAL, 3V3, 5V (top-to-bottom on the board: 5V, 3V3, SIGNAL, GND) |
| J14 (left edge) | Wire opening faces the **left** board edge |
| J4, J6 (RJ11) | Plug opening flush with the right edge, both plastic pegs in their holes |
| J15 (JST-PH) | Latch side as on the silkscreen outline |
| F7 Littelfuse 045106.3MRL (C178982), F5 SETsafe SCT6125T4A250V (C19712551), F6 (2920 PTC) | F7 is an actual SMD fuse, not the former incorrectly mapped 5×20 mm cartridge. F5/F7 use the NANO2-451/453 land pattern: 6.86 mm overall, 1.96×2.95 mm pads; this also closely matches F5's recommended 6.9 mm overall / 2.0×3.0 mm pads. Check the actual parts and JLCPCB preview; the footprint name does not substitute a Littelfuse part for F5 |
| L1 Bourns SRP5030T-100M (C189932) | 10 µH, 5.7×5.2 mm body; use the SRP5030T footprint, not the previous 4 mm inductor footprint |
| J1, J2 (sockets), JP1/JP11 (two separate 1×2 headers), JP2, JP8/JP9/JP10 (headers), L1, fuses, R, C | No polarity – only check the position. JP1/JP11 rows are 5.08 mm apart with a 2.54 mm gap between their plastic bodies; do not fit a 2×2 block |
| R25, J16, TP1–TP5 | Not populated (DNP) |

Also compare the RJ11 jack JLCPCB offers for C3097716 with the footprint (pin rows,
peg positions) — the footprint is the Amphenol 54601 layout, chosen after a datasheet
comparison with the EVERCOM 5301-6P6C.

## B. Bring-up of the first prototypes

Use a lab supply with current limit. Heltec module **not** inserted for steps 1–3.

1. **Visual check:** solder bridges at U1, U4, Q3 and the SOT-23-6 parts.
2. **JP8 source selector:** with power disconnected, fit one shunt on JP8 pins 1–2.
   Apply VIN 12 V, bench supply current limit 100 mA, Heltec absent. Verify
   TP3 (+5V) = 5.0 V ± 2 %. Disconnect VIN, move the shunt to pins 2–3, then
   apply current-limited external 5 V to J3 pins 2/3 and verify TP3.
   Use one source at a time; never bridge all three JP8 pins.
   Never connect USB-C while VIN or external 5 V is present.
3. **Reverse polarity, one input at a time:** disconnect USB, batteries and all
   other sources; leave the Heltec absent and discharge the rails before each
   polarity change. Use a bench supply limited to 10 mA, not a battery.
   - VIN: JP8 1–2, apply reversed 12 V, then up to reversed 29.2 V. Q7 must
     block; bidirectional D1 must not forward-crowbar and F7 must remain intact.
     With a temporary 10k load from TP2 to GND, check TP2 remains near 0 V
     (target magnitude below 0.1 V at room temperature), not a negative supply.
   - External 5 V: JP8 2–3, VIN absent, apply reversed 5 V. Q9 must block;
     D4 must not forward-crowbar and F2 must not need to trip. With a temporary
     10k load from TP3 to GND, check TP3 remains near 0 V as above.
   - Neither test should remain at the 10 mA current limit; disconnect
     immediately if it does. Some TVS leakage, especially D4 at 5 V, is normal.
     Remove the temporary loads afterwards and repeat the positive-supply tests.
   These are prototype acceptance tests, not measurements already performed.
4. With VIN and J3 external 5 V disconnected and the JP8 shunt removed, insert the Heltec and flash over USB.
   Unplug USB before reconnecting either external supply. The log shows
   `HW revision ID (GPIO20): ~1650 mV -> PCB v2.0`; TP3 = 5 V, TP4 = 3.3 V.
5. **Sensors:** BME280 at J9 (Cat5, SDA/SCL each with a GND wire), DS18B20 at J10,
   Davis wind and rain at the RJ11 jacks **and** at J5/J7 — counts, wind direction
   0–359°, rain tips. JP1 (SDA) and JP11 (SCL) jumpers fitted for extra pull-ups; JP2 open for
   the Davis vane, fitted for a bare potentiometer (R8 pull-up) — direction still
   plausible with JP2 fitted.
   **Vane switch (Q6, GPIO19):** WDIR_VCC at J5 pin 4 = 3.3 V normally; with
   *low-power* and *vane power save* on, 0 V between samples and the direction
   still updates.
   **UV sensor (J17, GPIO3 ADC1):** pin order is GND, SIGNAL, 3V3, 5V; power the
   sensor from exactly one supply pin. JP10 pins 1–2 select the 0–3.3 V range
   (ADC/input ratio 0.900990; firmware input scale 1.10989); pins 2–3 select
   0–5 V (ratio 0.600479; firmware input scale 1.66534). The divider limits a
   5 V input to about 3.0024 V. Set JP10 for the sensor’s maximum output before
   applying a signal; fit only one range shunt.

6. **AUX:** with R25 absent, 1.5 V at AIN (scale 1) → 1.50 V ± 2 %;
   switch at PULSE → counts. For a 0–5 V sensor, fit R25 = **15k** and set
   scale **1.66667** (downlink integer 1667). At 5.000 V on AIN, expect about
   3.000 V at C17/ADC and about 5.000 V in the scaled reading; check midscale too.
   The direct input has a 3.3 V electrical limit, but the useful ESP32-S3 ADC
   range is approximately 0–3.1 V. Repeat with heater/fan PWM enabled to check
   ground noise. Existing NVS settings are not automatically changed.
7. **Heater, 3 A:** 4 Ω / 50 W load at J12, VIN 12 V, heater mode *on*, 100 %:
   - measure load voltage; Q7 is the common protected-input switch and the former D21 series diode is removed,
   - measure Q7, Q3, F5 and copper temperatures during the test; stop if any part exceeds its datasheet limit.
   - 50 % PWM works, mode *auto* follows the BME280 temperature.
   - Q3 gate ≈ 5 V while on (driver U5); Q3 only lukewarm at 3 A; heater stays off during reset
     and boot (R39 pulls the driver input low).
8. **Fan:** JP9 shunt 1–2 selects 5 V by default (max 0.5 A); 2–3 selects protected VIN
   (max 1 A; use a fan rated for that voltage). J13 pins are FAN+, GND, TACH. Verify a
   24/29.2 V tacho pull-up does not raise the MCU-side 3.3 V rail; disconnected tacho should
   report *stalled*.
9. **VE.Direct:** J14/J15 pin 2 is node TX to Victron RX; pin 3 is Victron TX to node RX.
   Connect a real Victron MPPT at J15 (original cable) — values on the status
   page within 10 s; no continuity between GND and the Victron side (isolation).
10. **LoRaWAN at SF12:** ADR off, data rate DR0 in ChirpStack/TTN — uplinks arrive;
    the status page shows which values were left out (if any).
11. **Thermal and sourcing caveats:** Q7 conduction loss is estimated from datasheet RDS(on),
   not measured; no board thermal validation has been performed. Check fuse/PPTC derating,
   copper temperature and enclosure ambient before sustained operation at combined maximum
   load. Test the buck at minimum/maximum VIN and the intended combined load:
   output regulation, ripple, startup, load steps, inductor and U1 temperature.
   U1's four 0.6/0.3 mm thermal vias are inside its exposed ground pad:
   confirm the assembly process/tenting and solder-paste design avoid excessive
   solder wicking. Fuse/footprint orientation has not been assembly-preview-verified.
12. **Low-power mode:** Wi-Fi off, *low-power* on, heater/fan off — supply current
    at 12 V drops to a few mA between samples; wind and rain pulses during sleep are
    still counted (compare with step 5), the PRG button wakes the board.

## C. External-line protection

| Port | Installed protection | Limits / prototype checks |
|------|----------------------|---------------------------|
| UV J17 SIGNAL | D32 PESD5V0S2BT to GND at the terminal, then R51 10k, D30 BAT54S rail clamps and C27 filter | D32 has 5 V standoff; it does not clamp the ADC directly to 3.3 V. Select JP10 for the signal range. No protection against sustained miswiring, direct lightning or powered-sensor backfeed with the controller off is guaranteed |
| AUX J11 AIN / PULSE | D17 PESD5V0S2BT, series resistors, D18/D19 BAT54S and filters | ESD/transient protection, not an arbitrary-voltage input |
| Heater J12 | Input-side VIN TVS D1 upstream of Q7, branch fuse F5, D22 freewheel diode | No dedicated output TVS. Check cable/load transients and diode/MOSFET voltage margins; a freewheel diode is not a TVS |
| Fan J13 | Input-rail protection, PTC F6, D23 freewheel diode; TACH has D24 TVS and Q8 level shifting | No dedicated FAN+ TVS. JP9 permits both 5 V and VIN: a fixed 5 V output TVS would be incompatible with VIN mode; a VIN-rated TVS cannot protect a 5 V fan against sustained overvoltage |

D32 uses the same JLCPCB part as the other data-line TVS devices (C49338).
The [PESD5V0S2BT datasheet](https://assets.nexperia.com/documents/data-sheet/PESD5V0S2BT.pdf)
specifies 5 V standoff and 100 nA maximum leakage at 5 V / 25 °C. Its pulse
ratings apply to specified waveforms, not continuous faults or system-level
surge certification. Verify analog accuracy over temperature on the prototype.

The input reverse-polarity circuits are not overvoltage disconnects and do not
isolate simultaneously powered sources. Use only one supply at a time, never
USB with an external supply; rewire only with all rails discharged. D4's 9.2 V
pulse clamp is below Q9's 12 V gate-source and 30 V drain-source limits for the
specified TVS pulse, but it does not make the 5 V rail safe for sustained 12/24 V.

## D. Optimization review: implemented versus still requiring hardware

| Item | Design/file verification | Still required before release |
|------|--------------------------|-------------------------------|
| Source selection | JP8 remains manual; readable front selection/USB warning and full rear instructions. No mux or USB isolation is claimed | Verify assembled jumper orientation; never USB plus an external source |
| Buck correctness | LMR36520 400 kHz: C26 corrected to mandatory 1 µF/50 V, C2 to 220 nF/100 V X7R, L1 to 10 µH. SW, BOOT, VCC and VIN_BUCK are front-only; total SW copper is 8.07 mm including the bootstrap branch, BOOT 3.60 mm, VCC 4.27 mm. Close input PGND connection, capacitor ground returns and four EP thermal vias added | Oscilloscope/load/thermal tests; neither DRC nor the IC's 2 A rating qualifies the assembled board for continuous 2 A |
| High-current transitions | Each original VIN_IN, VIN_F and +VLOAD transition now has three 1.0/0.5 mm vias with connections on both copper layers. Heater-path widths retained | Confirm copper/plating stack-up and combined-load temperatures in the closed enclosure |
| Screws, washers, standoffs | Four copper rule areas reserve a 3.75 mm radius. Pads, tracks, vias and filled copper checked against a 7 mm hardware diameter plus 0.25 mm radial margin. Hole positions unchanged | Use hardware no larger than 7 mm OD; check real enclosure, underside clearance, screw length, connector bodies and cable bends |
| BOM / assembly | F7 cartridge/SMD mismatch removed; F5 land pattern matched; L1 footprint and missing 1 µF sourcing entries corrected. Pin/net assignments retained | JLCPCB stock/substitutions and full placement/rotation preview have not been uploaded or approved |
| Silkscreen / routing hygiene | No dangling-via/track warnings and no text overlap/copper warnings in the final routed board. Nine remaining silk-edge warnings are standard terminal pin-1 triangles outside the outline, not electrical faults | Inspect fabrication preview for clipped terminal triangles; pin numbers/functions remain labelled inside the board |
| TVS return paths | Local ground returns retained: nearest ground via about 0.85 mm from D1 and 1.30 mm from D4/D17; D32 has a direct pad/via return. Isolation-domain check remains separate | Cable transient/ESD tests; this is not lightning or system-level surge certification |
| Output diode voltage margin | D22/D23 changed from 40 V SS34 to 60 V / 3 A B360A-13-F, retaining the SMA footprint and cathode/anode nets | Check freewheel temperature under PWM and cable-induced ringing; the diode is still not an output TVS |
| Test access / analog calibration | TP1–TP5 retained for GND, protected VIN, +5V and +3V3; C17 and C27 provide ADC-side probe pads. R25 15k keeps a nominal 5 V input at 3.0 V ADC; firmware hints/downlink example updated without changing defaults/NVS | Probe access with the actual Heltec/enclosure; sensor-specific UV-to-UVI calibration and analog offset/gain/temperature tests. UV remains disabled until configured |

Conservative resistance/loss estimates, **not thermal qualification**:

- Assuming 35 µm copper and 0.01724 Ω·mm²/m copper resistivity near room
  temperature, summing all routed segments gives upper-bound series estimates
  (parallel branches deliberately not credited): VIN_IN 5.58 mΩ, VIN_F 16.81 mΩ,
  +VLOAD 31.82 mΩ. Their sum is about 54.2 mΩ: about 0.49 W at 3 A or
  1.10 W at 4.5 A, distributed over those nets. Actual currents split into
  branches; return-path, via, connector, fuse and MOSFET losses are additional.
- Q7 at the datasheet's **−10 V gate-drive** condition, RDS(on) ≤28 mΩ:
  about 0.25 W at 3 A or 0.57 W at 4.5 A at the stated datasheet temperature.
  Do not apply that bound at low VIN or elevated junction temperature.
- L1 has up to 128 mΩ DCR: about 0.51 W at 2 A before ripple/core losses.
  Its 3.5 A soft-saturation rating exceeds the LMR36520's 2.8 A maximum
  low-side current limit; it is slightly below the 3.6 A maximum high-side
  limit. The TI soft-saturation allowance therefore still needs load-step
  and short-circuit verification, not an unconditional saturation claim.
- Confirm C1's effective input capacitance remains at least the TI-required
  4.7 µF after tolerance, DC bias and temperature at the intended VIN;
  its 10 µF nominal marking alone does not establish that. Check effective
  C4/C5 capacitance and the selected inductor's thermal derating too.
- F7 is 125 V AC/DC (manufacturer specifies 400 A interrupt at 32 V DC).
  F5's SCT6125 series specifies 125 V DC and 1000 A interrupt at 32/48/63/75 V DC.
  Verify the ordered suffix/revision and available battery fault current;
  fuses/PTCs do not guarantee semiconductor survival during a short.

References: [LMR36520 datasheet](https://www.ti.com/lit/ds/symlink/lmr36520.pdf),
[Bourns SRP5030T](https://www.bourns.com/docs/product-datasheets/srp5030t.pdf),
[Diodes B360A](https://www.diodes.com/part/view/B360A),
[Littelfuse 045106.3MRL](https://www.littelfuse.com/products/fuses-overcurrent-protection/fuses/surface-mount-fuses/nano-2-fuses/451/045106-3),
[SETsafe SCT6125 datasheet](https://setsafe.cn/SdwlUploads/Ed/file/20241012/Miniature%20Fuses_SMFL_SCT6125_CHN_SETsafe%20%26%20SETfuse.pdf).
