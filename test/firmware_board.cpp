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
    assert(hostModes[PIN_ADC_WINDDIRECTION] == -1);   // GPIO2 = FEM CSD
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
    return 0;
}
