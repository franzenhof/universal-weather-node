# PCB v2.0 – Assembly and Bring-up Checklist

Nothing on this board has been tested on real hardware yet. Use this list for the
JLCPCB order and for the first prototypes (order 5, test, then more). Current layout:
100 × 100 mm, with a 92 × 92 mm mounting-hole pattern (M3 holes, centres 4 mm
from each adjacent board edge).

**Production exports refreshed on 2026-11-04:** the Gerber/drill ZIP and JLCPCB
BOM/CPL were generated from the current verified PCB, including the revised J17
position, corner mounting holes, separate JP1/JP11 headers, D32 UV-input TVS
and revised VIN reverse-polarity protection, buck layout/BOM corrections,
hardware copper keepouts, high-current via groups and the detailed back-side
operating reference. The external 5 V input and its F2/D4/Q9/R56/U6/R57–R60/
C28–C30 protection branch are now removed. J3 is **two-pin VIN/GND** and JP8
is a **two-pin buck disconnect: REMOVE BEFORE USB**. The DMP6023LE fan switch
and JP12 tacho pull-up remain.
Final DRC: **0 errors, 0 unconnected items, 0 schematic-parity issues**;
nine known terminal silkscreen-edge warnings remain.
BOM: 144 populated references in 67 groups, all with LCSC and MPN fields.
CPL: 122 top-side SMD placements, checked against PCB positions and rotations;
the 22 THT components are in the BOM but not the CPL and require separate/manual
assembly arrangements. DNP parts are excluded from both files.
KiCad rotations are not a guarantee of JLCPCB library orientation: complete the
assembly preview checks below before paying. Availability, price and actual
hardware performance have not been qualified.

**R33 sourcing update, 2026-11-04:** replaced the unavailable TE CRG1206F2K2
with UNI-ROYAL **1206W4F2201T5E, C17948**. Resistance (2.2 kOhm), tolerance
(1%), rated power (0.25 W) and 1206 footprint are unchanged. JLCPCB lists this
as an Extended part; confirm available quantity in the actual order.

**Ordering update, 2026-11-03:** Q8 now uses Jiangsu Changjing
MMBT3904(RANGE:100-300), **C20526**, instead of the unavailable Nexperia C108429.
It remains a 40 V / 200 mA NPN in SOT-23 with pins 1=base, 2=emitter,
3=collector; board geometry and circuit are unchanged. JLCPCB lists C20526 as
a Basic part; confirm stock in the actual order.

**JLCPCB orientation conversion checked on 2026-11-03:** corrections were
reviewed in five successive preview steps, ending with the supplied capture
at 21:39:32 UTC. Q3's purple marker was confirmed by the user as library pin 1;
the final image puts it upper right, with source/gate at top and drain below.
U1-U5 and the SOT-23 pin-1 marks match the PCB; visible two-terminal diode
cathode bands and C6's plus mark also agree. That review covered the former
TO-252 Q4, not its new SOT-223 replacement.

**Preview update, 2026-11-04 (19:26:57 UTC):** Q4 pin 1 appeared upper right instead of lower left;
the CPL now adds **180 degrees** (raw 90 -> CPL 270). The subsequent
**19:31:57 UTC preview confirms** Q4 gate lower left, source lower right
and drain tab top. U6 was subsequently removed with the external 5 V branch.
Upload the newly generated files; J3 and JP8 now have different footprints.

| File | Use |
|------|-----|
| [JLCPCB CPL](production/universal-weather-node-v2.0-cpl-jlcpcb.csv) | 122 top-side SMD parts; Q4 correction retained |
| [JLCPCB BOM](production/universal-weather-node-v2.0-bom-jlcpcb.csv) | Current part selection including Q8/C20526 |

Offsets: U1-U5/Q3 **+270 degrees**, Q1/Q2/Q5/Q6/Q8 and the selected
BAT54S/PESD5V0S2BT SOT-23 diodes **+180 degrees**. C6, Q7 and
two-terminal diodes retain their reviewed offsets. Q4 now uses **+180 degrees**;
Q4's corrected placement matches the 19:31:57 UTC preview.
Recheck the manufacturer's final order preview and any substitutions before
paying. These visual checks do not verify actual stock, the order's complete
part selection, soldering quality or real hardware performance.

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
| D1 (SMBJ30CA) – SMB, bidirectional | No polarity; ignore the standard footprint's cathode silk bar. Verify the **CA** suffix: a unidirectional A part recreates a reverse-polarity short. 30 V standoff / 48.4 V max clamp |
| D24 (SMBJ30A), D5–D7, D14, D20, D29 (SMLVT3V3) – SMB, unidirectional | Cathode band on the silkscreen bar (KiCad pad 1 = cathode) |
| D22, D23 (B360A-13-F, C94193) – SMA | 3 A / 60 V, replacing the 40 V SS34 parts that were below D1's 48.4 V maximum clamp. Cathode band on KiCad pad 1 / silkscreen bar; former D3 is removed |
| D26 (BZT52C12) – SOD-123 | Cathode band on the bar (gate clamp of Q4) |
| D8, D9, D11–D13, D15, D18, D19, D25, D30 (BAT54S), D16, D17, D27, D28, D32 (PESD5V0S2BT) – SOT-23 | Pin 1 at the silkscreen mark, all three pins on their pads. D32 pin 1=UV SIGNAL, pin 3=GND, pin 2 unused |
| Q1, Q6 (AO3401A), Q2 (AO3400A), Q5 (AO3422) – SOT-23 | Pin 1 (gate) at the mark |
| Q7 (NCE60P50K) – TO-252-3 | Verify pad 2/drain goes to VIN_F and source pad 3 goes to +VLOAD; reverse-polarity protection only, not an overvoltage blocker |
| Q4 (DMP6023LE, C154901) – SOT-223 | CPL +180 degrees: gate 1 lower left, source 3 lower right, drain 2/tab top matched the 2026-11-04 19:31:57 UTC preview. Old TO-252 approval does not apply |
| Q3 (AON7264E) – DFN 3.3 × 3.3 | Pins 1–3 source, pin 4 gate on the small-pad side, the large pad (drain) towards F5 |
| U1 (LMR36520ADDAR) – SOIC-8 PowerPAD, U2/U3 (74LVC2G14) – SOT-23-6, U4 (ADuM121N) – SOIC-8, U5 (74AHCT1G125) – SC-70-5 | Pin-1 mark and placement; U1 pins 1–8: PGND, VIN, EN, PG (unused), FB, VCC, BOOT, SW; EP9=GND |
| C6 (100 µF electrolytic) | "+" towards the + mark |
| J3, J9–J13 (bottom edge) | Wire openings face the **bottom** board edge |
| J3 / JP8 | Both now have two pins. J3: 1 VIN, 2 GND. JP8: 1 buck 5 V, 2 shared 5 V; fitted for VIN, REMOVE BEFORE USB. Do not reuse the former three-pin parts or pinout |
| J5, J7 (right edge) | Wire openings face the **right** board edge |
| J17 (UV sensor, right edge above mounting hole) | Pin 1 is at (94.5, 81) mm from the top-left board corner, 19 mm above the bottom edge. Wire opening faces the **right** board edge; pin order by terminal number is GND, SIGNAL, 3V3, 5V (top-to-bottom on the board: 5V, 3V3, SIGNAL, GND) |
| J14 (left edge) | Wire opening faces the **left** board edge |
| J4, J6 (RJ11) | Plug opening flush with the right edge, both plastic pegs in their holes |
| J15 (JST-PH) | Latch side as on the silkscreen outline |
| F7 Littelfuse 045106.3MRL (C178982), F5 SETsafe SCT6125T4A250V (C19712551), F6 (2920 PTC) | F7 is an actual SMD fuse, not the former incorrectly mapped 5×20 mm cartridge. F5/F7 use the NANO2-451/453 land pattern: 6.86 mm overall, 1.96×2.95 mm pads; this also closely matches F5's recommended 6.9 mm overall / 2.0×3.0 mm pads. Check the actual parts and JLCPCB preview; the footprint name does not substitute a Littelfuse part for F5 |
| L1 Bourns SRP5030T-100M (C189932) | 10 µH, 5.7×5.2 mm body; use the SRP5030T footprint, not the previous 4 mm inductor footprint |
| J1, J2 (sockets), JP1/JP11 (two separate 1×2 headers), JP2, JP8/JP9/JP10/JP12 (headers), L1, fuses, R, C | No polarity – only check the position. JP1/JP11 rows are 5.08 mm apart with a 2.54 mm gap between their plastic bodies; do not fit a 2×2 block |
| R25, J16, TP1–TP5 | Not populated (DNP) |

Also compare the RJ11 jack JLCPCB offers for C3097716 with the footprint (pin rows,
peg positions) — the footprint is the Amphenol 54601 layout, chosen after a datasheet
comparison with the EVERCOM 5301-6P6C.

## B. Bring-up of the first prototypes

### USB-C versus VIN: expected operation

See the [complete power-source function matrix](../../README.md#functions-by-power-source-v20)
for operating limits. The following expectations are to be checked on prototypes,
not results of measurements already performed.

| Check | USB-C 5 V only | VIN 6–29.2 V only |
|-------|---------------|------------------|
| Preparation, with power off | Remove JP8; disconnect VIN | Fit two-pin JP8; unplug USB |
| Heltec, display, Wi-Fi / LoRaWAN / dashboard | Operational | Operational via buck |
| Davis wind/direction/rain, I²C BME280, 1-Wire DS18B20, AUX, UV | Operational within supply/signal limits | Same |
| 3.3 V / 5 V sensor supplies | Available; UV uses one supply only and correct JP10 range | Same |
| 5 V fan, JP9 1–2 | Test combined USB/Heltec-path budget and startup; no extra 0.5 A guarantee | Up to 0.5 A design target, sharing buck capacity |
| VIN fan, JP9 2–3 | No load power | Up to 1 A design target; load rated for actual VIN |
| Heater J12 | No load power | Up to 3 A design target at actual protected VIN |
| Tacho electronics | Operational; board-powered fan must use 5 V mode | Operational in either fan-supply mode |
| VE.Direct | Requires Victron power on isolated side | Same |
| USB flashing | Available | Switch to USB mode first; OTA does not require USB |

Both modes use the Heltec 3.3 V regulator and sensor PTCs (F3 nominal 250 mA,
F4 nominal 100 mA for 1-Wire); those ratings are not guaranteed supply budgets.
The nominal 2 A buck rating is shared by all 5 V loads in VIN mode.
Check all load ratings against battery full-charge voltage: VIN outputs are
not regulated to 12 V or 24 V. Check combined load, temperature, USB inrush,
fan startup and supply dips before accepting either mode.

### Test procedure

Use a lab supply with current limit. Heltec module **not** inserted for steps 1–3.

1. **Visual check:** solder bridges at U1, U4, Q3 and the SOT-23-6 parts.
2. **JP8 buck disconnect:** with power disconnected, fit one shunt on JP8 pins 1–2.
   Apply VIN 12 V, bench supply current limit 100 mA, Heltec absent. Verify
   TP3 (+5V) = 5.0 V ± 2 %. Power off, discharge, remove JP8, then reapply
   VIN and confirm the buck side of JP8 is 5 V while TP3 remains unpowered
   (use a temporary 10k discharge/load resistor if needed).
   Disconnect VIN and discharge before the USB test. No external 5 V input exists.
3. **VIN reverse polarity:** disconnect USB, batteries and all
   other sources; leave the Heltec absent and discharge the rails before each
   polarity change. Use a bench supply limited to 10 mA, not a battery.
   - VIN: JP8 1–2, apply reversed 12 V, then up to reversed 29.2 V. Q7 must
     block; bidirectional D1 must not forward-crowbar and F7 must remain intact.
     With a temporary 10k load from TP2 to GND, check TP2 remains near 0 V
     (target magnitude below 0.1 V at room temperature), not a negative supply.
   - The test must not remain at the 10 mA current limit; disconnect
     immediately if it does. Some TVS leakage is normal.
     Remove the temporary loads afterwards and repeat the positive-supply tests.
   These are prototype acceptance tests, not measurements already performed.
4. With VIN disconnected and the JP8 shunt removed, insert the Heltec and flash over USB.
   Unplug USB before refitting JP8 and reconnecting VIN. The log shows
   `HW revision ID (GPIO20): ~1650 mV -> PCB v2.0`; TP3 = 5 V, TP4 = 3.3 V.
   With USB only, verify the buck side of open JP8 and TP2 remain unpowered
   with a temporary 10k load; remove the load afterwards. Check logic and sensors
   in this mode. A 5 V fan shares the USB/cable/Heltec-path budget; no additional
   0.5 A is guaranteed. Heater and VIN-mode fan must remain unpowered.
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
   report *stalled*. Fit JP12 for an open-collector PC fan (R61 10k to +5V).
   **Remove JP12 before connecting externally pulled-up or push-pull tacho signals**,
   especially 12/24 V; otherwise R61 can feed the +5V rail.
   With a simulated open-collector output, verify two pulses/revolution and stall.
   Check at least 4.65 V at FAN_SUPPLY under intended 5 V loads; R32=100k/R33=2.2k
   then give more than 4.5 V gate drive including 1% resistor corners.
   R33 is now 0.25 W / 1206 (about 0.14 W at 29.2 V); check resistor temperature
   and Q4 turn-on/off edges at 100 Hz and low/high PWM duty.
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

Q7 is not an overvoltage disconnect or reverse-current blocker.
Use one source at a time: unplug USB for VIN mode; disconnect VIN and remove
JP8 for USB mode. Rewire only with all rails discharged. The external 5 V
input and eFuse acceptance tests no longer apply because that branch is removed.
The fan-switch datasheet remains [DMP6023LE](https://www.diodes.com/assets/Datasheets/DMP6023LE.pdf).

## D. Optimization review: implemented versus still requiring hardware

| Item | Design/file verification | Still required before release |
|------|--------------------------|-------------------------------|
| Source separation | Two-pin JP8 manually disconnects the buck; front REMOVE BEFORE USB warning and full rear instructions. J3 is two-pin VIN/GND only. No mux or automatic USB isolation is claimed | Verify open-JP8 isolation in both directions as above; never parallel USB and buck |
| Buck correctness | LMR36520 400 kHz: C26 corrected to mandatory 1 µF/50 V, C2 to 220 nF/100 V X7R, L1 to 10 µH. SW, BOOT, VCC and VIN_BUCK are front-only; total SW copper is 8.07 mm including the bootstrap branch, BOOT 3.60 mm, VCC 4.27 mm. Close input PGND connection, capacitor ground returns and four EP thermal vias added | Oscilloscope/load/thermal tests; neither DRC nor the IC's 2 A rating qualifies the assembled board for continuous 2 A |
| High-current transitions | Each original VIN_IN, VIN_F and +VLOAD transition now has three 1.0/0.5 mm vias with connections on both copper layers. Heater-path widths retained | Confirm copper/plating stack-up and combined-load temperatures in the closed enclosure |
| Screws, washers, standoffs | Four copper rule areas reserve a 3.75 mm radius. Pads, tracks, vias and filled copper checked against a 7 mm hardware diameter plus 0.25 mm radial margin. Hole positions unchanged | Use hardware no larger than 7 mm OD; check real enclosure, underside clearance, screw length, connector bodies and cable bends |
| BOM / assembly | F7 cartridge/SMD mismatch removed; F5 land pattern matched; L1 footprint and missing 1 µF sourcing entries corrected. Pin/net assignments retained | JLCPCB stock/substitutions and full placement/rotation preview have not been uploaded or approved |
| Silkscreen / routing hygiene | No dangling-via/track warnings and no text overlap/copper warnings in the final routed board. Nine remaining silk-edge warnings are standard terminal pin-1 triangles outside the outline, not electrical faults | Inspect fabrication preview for clipped terminal triangles; pin numbers/functions remain labelled inside the board |
| TVS return paths | Local ground returns retained: nearest ground via about 0.85 mm from D1 and 1.30 mm from D17; D32 has a direct pad/via return. Isolation-domain check remains separate | Cable transient/ESD tests; this is not lightning or system-level surge certification |
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

### Nominal subcircuit simulations, 2026-11-04

Before the external 5 V input was removed, sixteen transient cases were run
using KiCad's ngspice 45.2: five external-5V eFuse cases, six fan-driver cases
and five tacho frequencies. **The five eFuse cases are historical and do not
validate any part of the current VIN/USB power path.** These are
**subcircuit simulations, not a whole-board simulation or hardware qualification**.
The TPS259533 and DMP6023LE use manufacturer models; Q5/zener behavior and the
tacho NPN are approximations. Fan loads are resistive, not real BLDC motors.

- **eFuse:** at 5 V input and 10 Ohm load, nominal output is 4.986 V;
  at 5 Ohm load it is 4.972 V. A 5 -> 9.2 V step with 100 ns rise time
  gives about 5.010 V peak output in this idealized model, followed by
  shutdown and recovery. A resistive overload demanding about 2 A at 5 V
  settles near 1.14 A / 2.86 V during the tested 10 ms overload.
  Upstream Q9/PTC/TVS, source impedance, cables and PCB parasitics are omitted;
  these results do not guarantee real fault-step voltage/current peaks.
- **Model discrepancy:** the older TI model reaches 90% output at about
  5.1 ms absolute time (input starts rising at 1 ms), faster than the current
  datasheet's approximately 12 ms full-ramp estimate for C28=100 nF.
  Do not treat the simulated startup timing as validated; measure it.
- **Fan PWM:** nominal gate drive is about 4.55 V at 4.65 V supply.
  With the official Q4 capacitance model and the resistive gate pull-up,
  command-to-output-below-10% delay is about 0.54 ms at 5 V and 1.14 ms
  at 29.2 V, at 100 Hz / 50% duty. At 29.2 V / 90% duty the output does
  **not** fall low within the 1 ms off window. High-duty VIN-mode PWM
  therefore needs real-fan verification; do not assume duty-linear control.
- **Tacho:** with JP12 fitted, generic BF=100 NPN, 50% duty and nominal
  5 V / 3.3 V supplies, the filtered input swings approximately
  0.05..3.19 V at 100 Hz and 0.064..2.71 V at 200 Hz.
  At two pulses/revolution these correspond to 3000 and 6000 rpm.
  At 500 Hz (15000 rpm), its high level is only about 1.76 V:
  Schmitt switching is not guaranteed. This is a filter-bandwidth limitation,
  not a qualified maximum RPM; actual pulse duty and component corners matter.

ESD/EFT/surge, reverse-polarity dynamics, buck stability, thermal behavior,
environmental resistance and full firmware interaction were not simulated.
The prototype acceptance tests above remain mandatory.

References: [LMR36520 datasheet](https://www.ti.com/lit/ds/symlink/lmr36520.pdf),
[Bourns SRP5030T](https://www.bourns.com/docs/product-datasheets/srp5030t.pdf),
[Diodes B360A](https://www.diodes.com/part/view/B360A),
[Littelfuse 045106.3MRL](https://www.littelfuse.com/products/fuses-overcurrent-protection/fuses/surface-mount-fuses/nano-2-fuses/451/045106-3),
[SETsafe SCT6125 datasheet](https://setsafe.cn/SdwlUploads/Ed/file/20241012/Miniature%20Fuses_SMFL_SCT6125_CHN_SETsafe%20%26%20SETfuse.pdf).
