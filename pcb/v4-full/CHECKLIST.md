# Weather Node V4 Full Rev 1.0 – Assembly and Bring-up Checklist

(formerly PCB v2.0; the V3 socket set J1/J2 was removed in Rev 1.0)

Step-by-step ordering (board settings, assembly options, THT parts): [Ordering the PCB (JLCPCB)](../../docs/pcb.md#ordering-the-pcb-jlcpcb).

Use this list for the JLCPCB order and for bringing up the first boards (tip: order
5, test, then more). Current layout:
100 × 100 mm, with a 92 × 92 mm mounting-hole pattern (M3 holes, centres 4 mm
from each adjacent board edge).

**Production exports refreshed for the four-resistor fan bank:** the Gerber/drill ZIP and JLCPCB
BOM/CPL were generated from the current verified PCB, including the revised J17
position, corner mounting holes, separate JP1/JP11 headers, D32 UV-input TVS
and revised VIN reverse-polarity protection, buck layout/BOM corrections,
hardware copper keepouts, high-current via groups and the detailed back-side
operating reference. The external 5 V input and its F2/D4/Q9/R56/U6/R57–R60/
C28–C30 protection branch are now removed. J3 is **two-pin VIN/GND** and JP8
is a **two-pin buck disconnect: REMOVE BEFORE USB**. The DMP6023LE fan switch
and JP12 tacho pull-up remain. The four stability improvements below add
C31-C34, replace C1 with a specified bias-characterized part and revise R32.
The fan gate-driver resistor is now a four-part R33/R62/R63/R64 bank.
Final DRC: **0 errors, 0 unconnected items, 0 schematic-parity issues**;
warnings only (terminal silkscreen edge, dangling vias / track stubs from the scripted re-routes; see docs/pcb.md).
BOM (SMD only): 139 populated references in 48 lines, all with LCSC and MPN fields.
CPL: 139 top-side SMD placements, checked against PCB positions and rotations;
the 24 through-hole references are listed separately in `production/*-tht-lcsc.csv` (10 lines,
LCSC order list) and require separate/manual assembly arrangements. DNP parts are excluded from both files.
KiCad rotations are not a guarantee of JLCPCB library orientation: complete the
assembly preview checks below before paying. Availability, price and actual
hardware performance have not been qualified.

**Fan turn-off update:** R32 is **75 kOhm**. The gate-driver bank is
**(R33 + R62) || (R63 + R64) = 2 kOhm**. Each resistor is 2 kOhm,
UNI-ROYAL **1206W4F2001T5E, LCSC C17944**, 1%, 0.25 W, 1206.
Two series pairs are connected in parallel, not four resistors directly in parallel.
The existing article is reused: no additional article/feeder, but three additional
placements and more PCB area; the assembly quote determines any actual savings.
The bounded
Diodes DMP6023LE subcircuit simulation reports 375 us at an assumed 4.65 V
fan rail, 407 us at 5 V, and 864 us at 29.2 V for the 100 Hz / 90% PWM
turn-off-to-10%-output criterion; the resistor-corner gate-drive estimate is
4.527 V. This is not whole-board or real-fan qualification. Worst-case
steady-state bank dissipation at 29.2 V is estimated as 0.171 W total and at most
0.04275 W per resistor across all sixteen independent +/-1% corners.
The nominal summed rating is 1 W, not a qualified all-temperature PCB rating.
At the 48.4 V TVS clamp ceiling the DC-equivalent bank power is
0.714 W total / 0.179 W per resistor; pulse/ambient derating and thermal coupling
have not been qualified, so hardware surge
testing and the resistor manufacturer's pulse limits remain blocking checks.

**Preview update, 2026-11-05 (11:48:05 UTC):** the supplied JLCPCB preview
shows R33/R62/R63/R64 present and aligned with their pads. C31 remains plus
right / minus left; Q4 remains gate lower left, source lower right, drain tab
top. The screenshot does not establish article identity or placement count:
confirm C17944 for all four resistors and 135 SMD placements in the order list.
This is visual placement evidence, not hardware qualification.

## Heltec V4 socket (V3 socket set removed)

V4 Full Rev 1.0 takes a **Heltec WiFi LoRa 32 V4** (V4.2 / V4.3, not V4-R8) in J19/J20 + J21 only.
Pin map, revision ID and the 5 V/USB rule: [Heltec V4 on the PCB](../../docs/pcb.md#heltec-v4-on-the-pcb).
History: v2.0 carried a second socket set J1/J2 for the Heltec V3; the V4 set was added by
`tools/pcbgen/dual_socket.sh` (J19/J20 2.54 mm towards the old J1, J21 = P4 GPIO15/16 from the
Heltec V4 STEP model, U8/U9 TI LM66100DCKR ideal diodes C2869734 + C39 on the V4 5 V pin,
R65 VE.Direct TX pull-down, GPIO19 net renamed `VANE_EN`, VE.Direct moat 1.65 mm lower).
For Rev 1.0 `tools/pcbgen/remove_v3.sh` removed J1/J2 from the routed board in place: the
connections that ran through a V3 pad were re-routed locally by `route_grid.py` (four
connections), the stubs left at the old V3 pads were pruned; the rest of the reviewed routing is
unchanged. `checknet.py` checks every function net on its V4 pin and that the V4's
RF-front-end/USB pins stay open. V3 builds: v2.0 files in the `2.x` branch / release 2.0.0 (firmware 2.x).

**Order / assembly**

- [ ] THT list: J19/J20 (Kinghelm C2905422, 1×18, 8.5 mm) plus J21 (SHOU HAN PM2.54-1x2PZZ-H8.5, C54876674).
- [x] Preview 2026-11-21: the 08:35 UTC preview showed U2/U3 (UMW SN74LVC2G14DBVR, C5375976) and U8/U9 (LM66100, C2869734) turned by 90 degrees; corrected in the CPL (U2/U3 +180, U8/U9 +270 from the raw KiCad angle). The 08:51:47 UTC preview confirms all four: leads on the pads, pin 1 at the PCB mark (U2 lower left, U3 lower right, U8 upper left / VIN, U9 lower right / VIN). R65, C39 present.
- [ ] Before soldering J21: plug a V4 (with its P4 pins) into J19/J20 dry and check that P4 meets J21 (position from the 3D model, 0.45 mm off grid — not yet measured on a real V4). Check whether the V4 is supplied with the 2×2 P4 pins.

**Bring-up with a V4 (first board)**

- [ ] Without module, VIN mode (JP8 fitted): TP3 ≈ 5.0 V, J19.2 ≈ 4.95 V (U8 on). Then hold J19.2 at 5.2 V from a lab supply: no current flows into J19.2 from the board (U8 off) and TP3 stays at the buck voltage.
- [ ] Without module, unpowered board, JP8 removed: 5 V via 100 Ω into J19.2 (simulated USB) → TP3 rises to ≈ 4.9 V through U9 (USB-only mode works); J19.2 at 4.7 V with VIN on → the board does not drive the lower supply by more than a few mA (U8 hysteresis ≈ 0.15 V).
- [ ] With V4, VIN mode: module 5 V pin under 27 dBm LoRa TX + Wi-Fi ≥ 4.7 V (U8 ≈ 0.1 V at 1 A); U8 temperature; module works; display, Wi-Fi.
- [ ] USB flashing with the PCB fitted and JP8 removed / VIN off (native USB on GPIO19/20 — both pins open on J19).
- [ ] Scope at power-up: GPIO45 (VE TX, R65) stays LOW until the firmware runs; GPIO43 (1-Wire) boot-log toggling harmless; fan gate (GPIO39).
- [ ] Revision ID on GPIO15 ≈ 1.65 V → firmware reports PCB V4 Full; all sensors on their V4 GPIOs (wind 44/6, rain 16, 1-Wire 43, vane 34).
- [ ] `espefuse.py summary` of the V4 (VDD_SPI_FORCE, DIS_PAD_JTAG, UART_PRINT_CONTROL) and resistance of GPIO15/16/45/34/44 to GND/3V3 on the bare module.
- [ ] Never plug a GNSS module into a V4 on this PCB (shares GPIO34 and 38–42).
- [ ] Thermal / drop check of the 0.3 mm necks where routes now pass between socket pins: +5V next to J19 pins 8|9 (whole module + sensor 5 V current in VIN mode), 3V3_HELTEC, +5V_V4 at J19.2, 3V3_LDO near J20 3|4; measure the 5 V drop J19.2 vs TP3 at LoRa TX.
- [ ] Visual review of the re-routed area under and around the module (scripted grid router, 45° segments, dangling-via/stub DRC warnings, see the final DRC) in the Gerber viewer before ordering.

## Electrical-stability improvements

| Improvement | Installed selection | Verification / limits |
|-------------|---------------------|-----------------------|
| Local U5 supply bypass | C32: Samsung CL05B104KO5NNNC, C1525, 100 nF/16 V X7R 0402 | Supply pad within 3 mm of U5 pin 5; ground pad within 2 mm of U5 pin 3, with a nearby ground via. Scope U5 supply and Q3 gate during heater PWM/reset |
| Buck input reserve | C1 + C33: Murata GRM32ER71H106KA12L, C77102, each 10 µF/50 V X7R 1210 (replaces the unavailable 100 V GRM32EC72A106KE05K, C5444485) | Operation up to 29.2 V (58% of rating); D1 clamps at max. 48.4 V, below 50 V. Estimated effective capacitance at 29.2 V/25 C roughly 3.5-4 µF each, 7-8 µF total (not taken from the manufacturer curve; earlier part: 9.17 µF). Still above the TI minimum of 4.7 µF; measure on your board |
| Buck output reserve | C34: Murata GRM32ER71E226KE15L, C21397, 22 µF/25 V X7R 1210; original C4/C5 retained | Manufacturer typical bias data at 5 V/25 C: 19.045 µF for C34; with -10% initial tolerance: 17.141 µF. C4/C5 are additional capacitance, not credited to a verified effective minimum |
| Protected VIN buffering | C31: Nichicon UCD1J470MNL1GS, C445452, 47 µF/63 V, +/-20%, SMD 8 x 10 mm | Connected to +VLOAD/GND after Q7, outside the isolated VE.Direct area; positive pad 1 is on the right in top view. Feed widened to 0.8-1.0 mm; only the run between the Q7 pins stays 0.3-0.5 mm (ESR of C31 dominates anyway) |
| Separate sensor 3.3 V rail | U7: Diodes AP2112K-3.3TRG1, C51118, 600 mA LDO, SOT-23-5, from +5V; EN tied to VIN. C35 (input) and C36 (output, before F3): Murata GRM32ER71H106KA12L, C77102 (same article as C1/C33), 10 µF/50 V X7R 1210 | Datasheet: stable with >= 1 µF ceramic output capacitance; input 2.5-6 V (6.5 V absolute maximum), so it is only for the 5 V rail. Typical sensor load 10-20 mA. Check TP4 = 3.3 V with VIN and with USB, and scope TP4 during Wi-Fi transmit |
| 1-Wire switch-on dip | C15 1 µF/50 V 0805 (C28323, was 10 µF 1210); new C38 22 µF/25 V 0805 (Samsung C45783 basic, same part as C4/C5) at the Q1 source | Switching on Q1 charges C15 from the 3.3 V sensor rail. 10 µF against about 10 µF on the rail could pull it down by up to about 1.6 V (worst case); now about 0.1-0.2 V. Scope TP4 and TP7 (OW_SW) while the firmware switches GPIO47 on; no wind/rain counts may occur |
| Fewer extended parts | BAT54S C7420333 (R+O, preferred), BZT52C12 C19077410 (preferred), SMBJ33A C19077586 for D24 (preferred), 22 µF 0805 Samsung C45783 (basic), SS210 C14996 for D22/D23 (basic, 2 A / 100 V), R52 100k basic (UV 3.3 V scale 1.1 instead of 1.10989) | About 24 extended SMD types instead of 30. The 2026-11-06 10:19:27 UTC preview confirms all ten BAT54S (pin 1 at the PCB triangle, +180 degrees) and the cathode bands of D22, D23, D24, D26 and D31 on KiCad pad 1 |
| Buck input PTC | F1: LUTE 2920L150/60GR, C19078738, 1.5 A hold / 60 V 2920 (same article as F6; was Littelfuse 2920L110/60MR 1.1 A) | More margin at low VIN: at 6-9 V input with a 5 V fan the buck input current came close to the former 1.1 A hold current. F7 (6.3 A) and the LMR36520 current limit stay upstream/downstream |
| UV sensor 5 V feed | F8: PTC 0.1 A (nSMD010, C70065, same part type as F4) between +5V and J17.4 | A shorted UV cable trips F8 instead of pulling down the shared 5 V rail. Check J17.4 = 5 V |
| VE.Direct isolator side 1 | U4 pin 1 and C21 on 3V3_HELTEC instead of the fused sensor rail | GPIO45 (V4 VE.Direct TX) and U4 VDD1 share one supply; a tripped F3 no longer back-feeds U4 through its input. Check U4 pin 1 = 3.3 V (TP6 on the bottom) |
| Q7 gate pull-down | R45 100k (C149504) instead of 10k | Standing D31/R45 current at 29.2 V about 0.17 mA (about 5 mW) instead of 1.7 mA. Q7 is static (not PWM); turn-on at power-up is slower but carried by its body diode |
| Heltec 3.3 V bulk | C37: Murata GRM32ER71E226KE15L, C21397, 22 µF/25 V X7R 1210 at J20.2/J20.3 (no new part type) | Supplements the module's own decoupling; the ESP32 peaks are drawn locally on the module. Check Heltec start-up and brown-out-free Wi-Fi transmit; remove C37 if the module regulator misbehaves with the extra capacitance |
| Faster fan turn-off | R32: 75 kOhm, UNI-ROYAL 0805W8F7502T5E, C17819; R33/R62/R63/R64: each 2 kOhm/0.25 W 1206, C17944, two series pairs in parallel | 2 kOhm effective; four model cases pass the 1 ms off-window criterion at 100 Hz/90% duty; no firmware/GPIO changes |

Capacitor evidence (for the previous 100 V input part; the current
[input part](https://pim.murata.com/en-us/pim/details/?partNum=GRM32ER71H106KA12%23)
still needs its curve checked): the official Murata
[former input part](https://pim.murata.com/en-us/pim/details/?partNum=GRM32EC72A106KE05%23)
and [output part](https://pim.murata.com/en-us/pim/details/?partNum=GRM32ER71E226KE15%23)
characteristic service
(`https://pimapi.murata.com/public/api/pim/v1/characteristics/characteristics`,
`ReqType=Characteristics`, `chara_type=c_dcbias_capchange_cap`).
Input data use 25 C/AC 1 Vrms, interpolated between 29.0 and 29.5 V;
output data use 25 C/AC 0.5 Vrms at 5 V. Curves are **typical**, not
guaranteed production minima or the actual converter ripple conditions.

As an illustrative additional class-temperature allowance, multiplying the
former part's tolerance-adjusted input estimate by 0.78 (X7S) gave 6.43 µF; multiplying the
output estimate by 0.85 (X7R) gives 14.57 µF for C34 alone.
These independent-factor estimates are **not guaranteed simultaneous
DC-bias/temperature limits** and do not include aging. Verify installed
parts over the intended temperature range. The
[LMR36520 datasheet](https://www.ti.com/lit/ds/symlink/lmr36520.pdf)
calls for at least 4.7 µF effective input capacitance; its 5 V/400 kHz/10 µH
example uses 2 x 22 µF rated output capacitors, not a blanket 44 µF effective
minimum. The output bank now has 66 µF nominal ceramic capacitance plus C6
when JP8 is fitted. The original C1/C2 high-frequency input loop, SW, BOOT
and VCC routes remain on F.Cu; C33's additional reservoir branch uses two
vias and a short B.Cu link.

C31 is rated to 105 C, with 250 mA ripple / 650 mOhm impedance at 100 kHz
and 2000 h at 105 C per the
[selected part listing](https://jlcpcb.com/partdetail/Nichicon-UCD1J470MNL1GS/C445452).
Check its datasheet and actual assembly availability before ordering.
At 29.2 V, its nominal added charge is 1.37 mC and energy about 20 mJ
(1.65 mC / 24 mJ at +20% capacitance). This is not inrush limiting:
measure hot-plug current, Q7 startup SOA, cable ringing and capacitor ripple/
temperature using the intended supply and cable. The 63 V rating exceeds
D1's specified 48.4 V clamp, but does not establish system surge immunity.
With JP8 removed, C31/C33/C34 remain outside the USB-powered bank;
C6 stays 100 µF and C32 adds only 100 nF to the shared 5 V rail.

Fan evidence is in [fan-pwm-stability.json](fan-pwm-stability.json);
[simulate_fan_pwm.py](../../tools/pcbgen/simulate_fan_pwm.py) reproduces it
with the official downloaded Q4 model. Q5 is an ideal switch, the zener is
approximate and the load is resistive, not a BLDC motor. Fast edges can
increase EMI. At 29.2 V, the bank's conservative 0.171 W estimate uses a 10.8 V
clamp allowance and -1% effective resistance. The individual-corner calculation
bounds each resistor below 0.043 W; each must stay below its **derated** rating
in the enclosure. Do not equate the four 0.25 W nameplates with an all-temperature
1 W board rating or four times the pulse endurance. Scope the real fan output to confirm turn-off within the 1 ms
off-window at 90% duty, including low fan voltage, temperature, startup and
reset. No whole-board simulation, surge certification or hardware approval
is claimed.

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
| [JLCPCB CPL](production/universal-weather-node-v4-full-cpl-jlcpcb.csv) | 139 top-side SMD parts; Q4 and C31 corrections preview-confirmed; U8/U9 (LM66100) and the new U2/U3 article preview-confirmed 2026-11-21 |
| [JLCPCB BOM](production/universal-weather-node-v4-full-bom-jlcpcb.csv) | SMD parts only (48 lines); THT parts are hand-soldered |
| [THT parts (LCSC)](production/universal-weather-node-v4-full-tht-lcsc.csv) | 10 LCSC articles for terminals, RJ11, the Heltec V4 sockets J19/J20/J21, JST, headers and the optional DC jack J18, quantity per board; add jumper shunts separately |

Offsets: U1-U5/Q3 **+270 degrees**, Q1/Q2/Q5/Q6/Q8 and the selected
BAT54S/PESD5V0S2BT SOT-23 diodes **+180 degrees**. C6, Q7 and
two-terminal diodes retain their reviewed offsets. Q4 now uses **+180 degrees**;
Q4's corrected placement matches the 19:31:57 UTC preview.
Q7 (TO-252) keeps 0 degrees but its centroid is moved **1.4 mm towards the leads**: JLC's model
sits off the KiCad origin (leads only touched the pad edge, metal tab beyond the tab pad).
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
| D24 (SMBJ33A, C19077586), D5–D7, D14, D20, D29 (SMLVT3V3) – SMB, unidirectional | Cathode band on the silkscreen bar (KiCad pad 1 = cathode) |
| D22, D23 (SS210, C14996, basic) – SMA | 2 A / 100 V (was B360A-13-F 3 A / 60 V; the 40 V SS34 was below D1's 48.4 V maximum clamp). Cathode band on KiCad pad 1, confirmed in the 2026-11-06 10:19:27 UTC preview. Cathode band on KiCad pad 1 / silkscreen bar; former D3 is removed |
| D26 (BZT52C12) – SOD-123 | Cathode band on the bar (gate clamp of Q4) |
| D8, D9, D11–D13, D15, D18, D19, D25, D30 (BAT54S), D16, D17, D27, D28, D32 (PESD5V0S2BT) – SOT-23 | Pin 1 at the silkscreen mark, all three pins on their pads. D32 pin 1=UV SIGNAL, pin 3=GND, pin 2 unused |
| Q1, Q6 (AO3401A), Q2 (AO3400A), Q5 (AO3422) – SOT-23 | Pin 1 (gate) at the mark |
| Q7 (NCE60P50K) – TO-252-3 | Verify pad 2/drain goes to VIN_F and source pad 3 goes to +VLOAD; reverse-polarity protection only, not an overvoltage blocker |
| Q4 (DMP6023LE, C154901) – SOT-223 | CPL +180 degrees: gate 1 lower left, source 3 lower right, drain 2/tab top matched the 2026-11-04 19:31:57 UTC preview. Old TO-252 approval does not apply |
| C31 (Nichicon UCD1J470MNL1GS, C445452) – SMD 8 x 10 mm | The 2026-11-05 11:10:52 UTC preview showed reversed polarity. CPL adds **180 degrees** (raw 180 -> CPL 0). The **11:15:14 UTC preview confirms plus right, minus left**, matching PCB pad 1/right. Recheck the final order |
| Q3 (AON7264E) – DFN 3.3 × 3.3 | Pins 1–3 source, pin 4 gate on the small-pad side, the large pad (drain) towards F5 |
| U1 (LMR36520ADDAR) – SOIC-8 PowerPAD, U2/U3 (74LVC2G14) – SOT-23-6, U4 (ADuM121N) – SOIC-8, U5 (74AHCT1G125) – SC-70-5 | Pin-1 mark and placement; U1 pins 1–8: PGND, VIN, EN, PG (unused), FB, VCC, BOOT, SW; EP9=GND |
| U7 (AP2112K-3.3TRG1, C51118) – SOT-23-5, under the Heltec near J20 | The 2026-11-06 09:19:56 UTC preview showed the raw angle 90 degrees off (three leads on the right, pin 1 lower right). CPL adds **270 degrees** (raw 90 -> CPL 0). The **09:27:10 UTC preview confirms** pin 1/VIN lower left, three leads at the bottom (VIN, GND, EN), two at the top (NC, VOUT) |
| C6 (100 µF electrolytic) | "+" towards the + mark |
| J3, J9–J13 (bottom edge) | Wire openings face the **bottom** board edge |
| J3 / JP8 | Both now have two pins. J3: 1 VIN, 2 GND. JP8: 1 buck 5 V, 2 shared 5 V; fitted for VIN, REMOVE BEFORE USB. Do not reuse the former three-pin parts or pinout |
| J5, J7 (right edge) | Wire openings face the **right** board edge |
| J17 (UV sensor, right edge above mounting hole) | Pin 1 is at (94.5, 81) mm from the top-left board corner, 19 mm above the bottom edge. Wire opening faces the **right** board edge; pin order by terminal number is GND, SIGNAL, 3V3, 5V (top-to-bottom on the board: 5V, 3V3, SIGNAL, GND) |
| J14 (left edge) | Wire opening faces the **left** board edge |
| J18 DC jack (optional, lower left corner, THT list C381116) | Opening faces the **left** board edge, front about 0.45 mm beyond it. Pin 1 (centre) = VIN_IN, pins 2/3 (sleeve/switch) = GND. Footprint is KiCad `BarrelJack_Horizontal`; the XKB DC-005-5A-2.0 drawing differs by 0.1 mm (pin 1–2 5.9 vs 6.0 mm, pin 3 2.9 vs 3.0 mm), within the 1.0 × 3.0 mm slots — check the first part fits. 24 V contact rating: J3 only for 8s LiFePO4. Never feed J3 and J18 at the same time |
| J4, J6 (RJ11) | Plug opening flush with the right edge, both plastic pegs in their holes |
| J15 (JST-PH) | Latch side as on the silkscreen outline |
| F7 Littelfuse 045106.3MRL (C178982), F5 SETsafe SCT6125T4A250V (C19712551), F6 (2920 PTC) | F7 is an actual SMD fuse, not the former incorrectly mapped 5×20 mm cartridge. F5/F7 use the NANO2-451/453 land pattern: 6.86 mm overall, 1.96×2.95 mm pads; this also closely matches F5's recommended 6.9 mm overall / 2.0×3.0 mm pads. Check the actual parts and JLCPCB preview; the footprint name does not substitute a Littelfuse part for F5 |
| L1 Bourns SRP5030T-100M (C189932) | 10 µH, 5.7×5.2 mm body; use the SRP5030T footprint, not the previous 4 mm inductor footprint |
| J19, J20, J21 (sockets), JP1/JP11 (two separate 1×2 headers), JP2, JP8/JP9/JP10/JP12 (headers), L1, fuses, R, C | No polarity – only check the position. JP1/JP11 rows are 5.08 mm apart with a 2.54 mm gap between their plastic bodies; do not fit a 2×2 block |
| R25, J16, TP1–TP7 | Not populated (DNP); TP6/TP7 are bare SMD probe pads |

Also compare the RJ11 jack JLCPCB offers for C3097716 with the footprint (pin rows,
peg positions) — the footprint is the Amphenol 54601 layout, chosen after a datasheet
comparison with the EVERCOM 5301-6P6C.

## B. Bring-up of the first boards

### USB-C versus VIN: expected operation

See the [complete power-source function matrix](../../docs/pcb.md#functions-by-power-source-v4-full)
for operating limits. Check the following expectations on your first board.

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

Both modes feed the sensor rail from the 5 V rail through its own LDO U7 and
the sensor PTCs (F3 nominal 250 mA, F4 nominal 100 mA for 1-Wire); those ratings
are not guaranteed supply budgets. The Heltec 3.3 V regulator supplies only the module, the HW-ID divider and the MCU side of U4.
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
   These are acceptance tests for a new board.
4. With VIN disconnected and the JP8 shunt removed, insert the Heltec and flash over USB.
   Unplug USB before refitting JP8 and reconnecting VIN. The log shows
   `HW revision ID (GPIO15): ~1650 mV -> PCB V4 Full`; TP3 = 5 V, TP4 = 3.3 V (sensor LDO U7).
   Repeat TP4 under USB power: U7 must still regulate (dropout from the module's 5 V path).
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
   (ADC/input ratio 0.909091; firmware input scale 1.1); pins 2–3 select
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
   - Heater 50 % gives approximately 5 s on / 5 s off (0.1 Hz, 10 s cycle);
     0 % stays off and 100 % stays on. Check timing during LoRa transmissions,
     immediate shutdown on battery lockout/config portal/OTA, and mode *auto*
     following the BME280 temperature. Fan PWM remains 100 Hz.
   - Q3 gate ≈ 5 V while on (driver U5); Q3 only lukewarm at 3 A; heater stays off during reset
     and boot (R39 pulls the driver input low).
8. **Fan:** JP9 shunt 1–2 selects 5 V by default (max 0.5 A); 2–3 selects protected VIN
   (max 1 A; use a fan rated for that voltage). J13 pins are FAN+, GND, TACH. Verify a
   24/29.2 V tacho pull-up does not raise the MCU-side 3.3 V rail; disconnected tacho should
   report *stalled*. Fit JP12 for an open-collector PC fan (R61 10k to +5V).
   **Remove JP12 before connecting externally pulled-up or push-pull tacho signals**,
   especially 12/24 V; otherwise R61 can feed the +5V rail.
   With a simulated open-collector output, verify two pulses/revolution and stall.
   Check at least 4.65 V at FAN_SUPPLY under intended 5 V loads; R32=75k / bank=2k
   gives about 4.527 V gate drive at the resistor tolerance corner in the
   bounded model. R33/R62/R63/R64 are each 0.25 W / 1206 (estimated at most
   0.043 W each at 29.2 V before derating); check all four temperatures,
   thermal coupling and the 48.4 V clamp-pulse rating. Measure
   Q4 turn-on/off edges with the actual fan at 100 Hz and low/high PWM duty.
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

| Port | Installed protection | Limits / checks |
|------|----------------------|---------------------------|
| UV J17 SIGNAL | D32 PESD5V0S2BT to GND at the terminal, then R51 10k, D30 BAT54S rail clamps and C27 filter | D32 has 5 V standoff; it does not clamp the ADC directly to 3.3 V. Select JP10 for the signal range. No protection against sustained miswiring, direct lightning or powered-sensor backfeed with the controller off is guaranteed |
| AUX J11 AIN / PULSE | D17 PESD5V0S2BT, series resistors, D18/D19 BAT54S and filters | ESD/transient protection, not an arbitrary-voltage input |
| Heater J12 | Input-side VIN TVS D1 upstream of Q7, branch fuse F5, D22 freewheel diode | No dedicated output TVS. Check cable/load transients and diode/MOSFET voltage margins; a freewheel diode is not a TVS |
| Fan J13 | Input-rail protection, PTC F6, D23 freewheel diode; TACH has D24 TVS and Q8 level shifting | No dedicated FAN+ TVS. JP9 permits both 5 V and VIN: a fixed 5 V output TVS would be incompatible with VIN mode; a VIN-rated TVS cannot protect a 5 V fan against sustained overvoltage |

D32 uses the same JLCPCB part as the other data-line TVS devices (C49338).
The [PESD5V0S2BT datasheet](https://assets.nexperia.com/documents/data-sheet/PESD5V0S2BT.pdf)
specifies 5 V standoff and 100 nA maximum leakage at 5 V / 25 °C. Its pulse
ratings apply to specified waveforms, not continuous faults or system-level
surge certification. Verify analog accuracy over temperature on your board.

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
| Output diode voltage margin | D22/D23 changed from 40 V SS34 to 100 V / 2 A SS210 (basic; B360A-13-F 60 V / 3 A before), retaining the SMA footprint and cathode/anode nets | Check freewheel temperature under PWM and cable-induced ringing; the diode is still not an output TVS |
| Test access / analog calibration | TP1–TP5 retained for GND, protected VIN, +5V and +3V3; TP6 (bottom, 1.5 mm pad) on 3V3_HELTEC and TP7 (top, 1.0 mm pad next to Q1) on the switched 1-Wire supply OW_SW; heater gate is probed at R31 pad 1 (HEAT_G); bottom S/N and DATE fields for marking boards; C17 and C27 provide ADC-side probe pads. R25 15k keeps a nominal 5 V input at 3.0 V ADC; firmware hints/downlink example updated without changing defaults/NVS | Probe access with the actual Heltec/enclosure; sensor-specific UV-to-UVI calibration and analog offset/gain/temperature tests. UV remains disabled until configured |

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
The acceptance tests above remain mandatory.

References: [LMR36520 datasheet](https://www.ti.com/lit/ds/symlink/lmr36520.pdf),
[Bourns SRP5030T](https://www.bourns.com/docs/product-datasheets/srp5030t.pdf),
[Diodes B360A](https://www.diodes.com/part/view/B360A),
[Littelfuse 045106.3MRL](https://www.littelfuse.com/products/fuses-overcurrent-protection/fuses/surface-mount-fuses/nano-2-fuses/451/045106-3),
[SETsafe SCT6125 datasheet](https://setsafe.cn/SdwlUploads/Ed/file/20241012/Miniature%20Fuses_SMFL_SCT6125_CHN_SETsafe%20%26%20SETfuse.pdf).
