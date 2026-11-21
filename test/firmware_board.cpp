// Heltec V3 / V4 board detection, the V4 FEM TX-power mapping and the
// "no PCB on a V4" gating (heltec_board.h).
#include "heltec_board.h"
#include "sensors.h"
#include "globals.h"
#include "board_io.h"
#include "params.h"
#include <cassert>

extern int hostModes[64];

int main()
{
    // ---- detection by flash size ----
    const uint32_t MB = 1024UL * 1024UL;
    assert(boardDetect(8 * MB) == BOARD_V3);     // V3: ESP32-S3FN8
    assert(boardDetect(16 * MB) == BOARD_V4);    // V4: 16 MB external flash
    assert(boardDetect(32 * MB) == BOARD_V4);
    assert(boardDetect(0) == BOARD_V3);          // unknown -> V3 (never drive the FEM pins on a guess)
    assert(boardDetect(4 * MB) == BOARD_V3);
    assert(boardResolve(BOARD_SEL_AUTO, BOARD_V4) == BOARD_V4);
    assert(boardResolve(BOARD_SEL_AUTO, BOARD_V3) == BOARD_V3);
    assert(boardResolve(BOARD_V3, BOARD_V4) == BOARD_V3);
    assert(boardResolve(BOARD_V4, BOARD_V3) == BOARD_V4);
    assert(boardSelValid(0) && boardSelValid(3) && boardSelValid(4));
    assert(!boardSelValid(1) && !boardSelValid(2) && !boardSelValid(5) && !boardSelValid(-1));
    gBoard.kind = BOARD_V4; gBoard.psramCap = 2; assert(boardIsV4() && !boardIsR8());
    gBoard.psramCap = 1; assert(boardIsR8());
    gBoard.kind = BOARD_V3; assert(!boardIsR8() && boardPcbAllowed());

    // ---- TX power: SX1262 setting for a wanted FEM output ----
    // V3 (no FEM): unchanged, only clamped to the SX1262 range
    assert(femChipPowerDbm(16, FEM_NONE) == 16);
    assert(femChipPowerDbm(22, FEM_NONE) == 22);
    assert(femChipPowerDbm(30, FEM_NONE) == 22);
    assert(femChipPowerDbm(-20, FEM_NONE) == -9);
    // GC1109 (V4.2), +11 dB small-signal: EU868 max EIRP 16 dBm -> 5 dBm, 14 -> 3
    assert(femChipPowerDbm(16, FEM_GC1109) == 5);
    assert(femChipPowerDbm(14, FEM_GC1109) == 3);
    assert(femChipPowerDbm(2, FEM_GC1109) == -9);   // lowest ADR step: -9 dBm (SX1262 minimum, ~2 dBm out)
    assert(femChipPowerDbm(27, FEM_GC1109) == 18);  // PA compression: gain 9 at 18 dBm in
    assert(femChipPowerDbm(30, FEM_GC1109) == 21);  // end of the curve: 21+7 = 28 dBm
    assert(femChipPowerDbm(25, FEM_GC1109) == 14);  // 15 dBm would give 26 dBm
    // KCT8103L (V4.3), +13 dB: 16 -> 3, 14 -> 1
    assert(femChipPowerDbm(16, FEM_KCT8103L) == 3);
    assert(femChipPowerDbm(14, FEM_KCT8103L) == 1);
    assert(femChipPowerDbm(20, FEM_KCT8103L) == 7);
    assert(femChipPowerDbm(2, FEM_KCT8103L) == -9);
    // never above the request (within the curve's 1-dB steps), for every legal EIRP
    for (int t = 2; t <= 27; t++)
        for (FemType f : { FEM_GC1109, FEM_KCT8103L }) {
            int chip = femChipPowerDbm((int8_t)t, f);
            assert(chip >= -9 && chip <= 22);
            if (chip > -9) assert(femOutputDbm(chip, f) <= t);
        }
    assert(femOutputDbm(5, FEM_GC1109) == 16 && femOutputDbm(3, FEM_KCT8103L) == 16);
    assert(femGainDb(FEM_GC1109, -9) == 11 && femGainDb(FEM_KCT8103L, 40) == 7);

    // ---- V4: wireless build only, the PCB's GPIOs are never configured ----
    gBoard.kind = BOARD_V4;
    gCfg.pcbVersion = 20; gCfg.windEn = 0; gCfg.rainEn = 0;
    assert(!boardPcbAllowed() && !ioV2());
    for (int i = 0; i < 64; i++) hostModes[i] = -1;
    sensorsEarlyInit();
    sensorsInit(getPins(gCfg.pcbVersion));
    assert(windDirPin() == -1 && hostModes[2] == -1);   // GPIO2 = FEM CSD
    assert(hostModes[PIN_1WIRE_HSS] == -1);
    assert(hostModes[5] == -1 && hostModes[7] == -1); // KCT8103L CTX, VFEM
    startOneWireConversion();
    assert(bus1Count == 0 && pollOneWireConversion());
    assert(readWindDirection() == 0);
    assert(!applyParam(PARAM_WIND_EN, 1) && !applyParam(PARAM_RAIN_EN, 1));
    assert(applyParam(PARAM_WIND_EN, 0) && applyParam(PARAM_RAIN_EN, 0));
    assert(!applyParam(PARAM_HEATER_MODE, 1) && !applyParam(PARAM_UV_EN, 1));
    ioInit();
    assert(hostModes[38] == -1 && hostModes[39] == -1);   // heater/fan pins = GNSS connector on the V4
    // V3 with a v2.0 PCB: unchanged
    gBoard.kind = BOARD_V3;
    assert(ioV2() && applyParam(PARAM_WIND_EN, 1));

    // ---- pin map: V3 / V4 x PCB revision ----
    {
        BoardPins p = boardPins(BOARD_V3, false, 12);
        assert(p.windSpeed == 3 && p.rain == 4 && p.oneWire == 5 && p.windDir == 2 && p.vaneEn == -1 && p.veTx == -1);
        p = boardPins(BOARD_V3, false, 13);
        assert(p.windSpeed == 6 && p.rain == 7 && p.oneWire == 5 && p.windDir == 2 && p.vaneEn == -1);
        p = boardPins(BOARD_V3, false, 20);
        assert(p.windSpeed == 6 && p.rain == 7 && p.oneWire == 5 && p.windDir == 2 && p.vaneEn == 19 && p.veRx == 33 && p.veTx == 34);
        assert(boardPins(BOARD_V3, true, 20).vaneEn == 19);          // pcbOnV4 means nothing on a V3
        for (int rev : { 12, 13, 20 }) {
            p = boardPins(BOARD_V4, false, rev);                      // bare V4: nothing
            assert(p.windSpeed < 0 && p.rain < 0 && p.oneWire < 0 && p.windDir < 0 && p.vaneEn < 0 && p.veRx < 0 && p.veTx < 0);
            p = boardPins(BOARD_V4, true, rev);                       // V4 sockets of PCB v2.0
            assert(p.windSpeed == 44 && p.rain == 16 && p.oneWire == 43 && p.windDir == 6 && p.vaneEn == 34 && p.veRx == 33 && p.veTx == 45);
        }
        // the V4 map never touches its USB (19/20), FEM (2/5/7/46) or the PCB ID pin 15 as an output
        for (int8_t pin : { p.windSpeed, p.rain, p.oneWire, p.windDir, p.vaneEn, p.veRx, p.veTx })
            assert(pin != 19 && pin != 20 && pin != 2 && pin != 5 && pin != 7 && pin != 46 && pin != 15);
    }

    // ---- revision ID pin and UART0 decision (by detected hardware) ----
    assert(boardHwIdPin(BOARD_V3) == 20 && boardHwIdPin(BOARD_V4) == 15);
    assert(boardUseUart0(BOARD_V3) && !boardUseUart0(BOARD_V4));

    // ---- ID detection paths ----
    auto detect = [](BoardKind det, int mv) {
        for (int i = 0; i < 64; i++) hostAdc[i] = 0;
        gBoard.detected = det; gBoard.kind = det; gBoard.pcbOnV4 = false;
        hostAdc[boardHwIdPin(det)] = mv;
        return detectPcbV2();
    };
    assert(detect(BOARD_V4, 1650) && gBoard.pcbOnV4 && gIo.hwIdDetected && boardPcbAllowed());
    assert(hostModes[15] == INPUT && hostModes[20] != OUTPUT);       // discharged, released; USB D+ untouched
    assert(!detect(BOARD_V4, 0) && !gBoard.pcbOnV4 && !boardPcbAllowed());   // bare V4 ~0 V
    assert(!detect(BOARD_V4, 3300) && !gBoard.pcbOnV4);
    assert(!detect(BOARD_V4, 1299) && detect(BOARD_V4, 1300) && detect(BOARD_V4, 2000) && !detect(BOARD_V4, 2001));
    hostAdc[20] = 1650; gBoard.detected = BOARD_V4; gBoard.pcbOnV4 = false;
    assert(!detectPcbV2() && !gBoard.pcbOnV4);                       // a V4 never reads GPIO20
    assert(detect(BOARD_V3, 1650) && gIo.hwIdDetected && !gBoard.pcbOnV4 && boardPcbAllowed());
    assert(!detect(BOARD_V3, 0) && boardPcbAllowed());                // Rev 1.x
    hostAdc[15] = 1650; gBoard.detected = BOARD_V3;
    assert(!detectPcbV2());                                           // a V3 never reads GPIO15

    // ---- V4 on PCB v2.0: sensors and I/O on the V4 pins ----
    gBoard.kind = gBoard.detected = BOARD_V4; gBoard.pcbOnV4 = true;
    gCfg.pcbVersion = 20; gCfg.windEn = 1; gCfg.rainEn = 1;
    assert(boardPcbAllowed() && ioV2() && windDirPin() == 6);
    assert(getPins(20).windSpeed == 44 && getPins(20).rain == 16 && getPins(20).oneWire == 43);
    assert(applyParam(PARAM_WIND_EN, 1) && applyParam(PARAM_HEATER_MODE, 1));
    for (int i = 0; i < 64; i++) { hostModes[i] = -1; hostPins[i] = -1; }
    gCfg.veDirectEn = 0;
    assert(hostModes[45] == -1);                                      // nothing before ioInit()
    ioInit();                                                         // end of setup(), after the ID read
    assert(hostModes[45] == OUTPUT && hostPins[45] == HIGH);          // VE TX idle high, VE.Direct off
    assert(hostModes[34] == OUTPUT && hostPins[34] == LOW);           // vane supply on (LOW = on)
    assert(hostModes[19] == -1 && hostModes[20] == -1);               // V3 vane/ID pins = V4 USB: untouched
    // bare V4: VE TX never driven
    gBoard.pcbOnV4 = false;
    for (int i = 0; i < 64; i++) { hostModes[i] = -1; hostPins[i] = -1; }
    ioPoll();
    assert(hostModes[45] == -1 && hostModes[34] == -1);
    return 0;
}
