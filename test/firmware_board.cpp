// Heltec V4 board check (V3 / V4-R8 refused), the FEM TX-power mapping and
// the PCB gating by the revision ID: V4 Full, V4 Light or none (heltec_board.h).
#include "heltec_board.h"
#include "sensors.h"
#include "globals.h"
#include "board_io.h"
#include "params.h"
#include <cassert>
#include <string>

extern int hostModes[64];

int main()
{
    // ---- supported board: 16 MB flash + 2-MB quad PSRAM (ESP32-S3R2) ----
    const uint32_t MB = 1024UL * 1024UL;
    assert(boardSupportedBy(16 * MB, PSRAM_CAP_2MB));    // Heltec V4
    assert(boardSupportedBy(32 * MB, PSRAM_CAP_2MB));
    assert(!boardSupportedBy(8 * MB, 0));                 // V3: ESP32-S3FN8 -> firmware 2.x
    assert(!boardSupportedBy(16 * MB, PSRAM_CAP_8MB));    // V4-R8: octal PSRAM on GPIO33-37
    assert(!boardSupportedBy(16 * MB, 0) && !boardSupportedBy(0, PSRAM_CAP_2MB) && !boardSupportedBy(16 * MB, -1));
    gBoard.flashBytes = 16 * MB; gBoard.psramCap = PSRAM_CAP_2MB; assert(boardSupported() && !boardIsR8());
    gBoard.psramCap = PSRAM_CAP_8MB; assert(boardIsR8() && !boardSupported());
    gBoard.psramCap = PSRAM_CAP_2MB;
    gBoard.pcb = false; assert(!boardPcbAllowed());

    // ---- TX power: SX1262 setting for a wanted FEM output ----
    // no FEM: unchanged, only clamped to the SX1262 range
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

    // ---- no PCB: wireless station, the PCB's GPIOs are never configured ----
    gBoard.pcb = false;
    gCfg.pcbVersion = 0; gCfg.windEn = 0; gCfg.rainEn = 0;
    assert(!boardPcbAllowed() && !ioFull() && !boardPcbFull());
    for (int i = 0; i < 64; i++) hostModes[i] = -1;
    sensorsEarlyInit();
    sensorsInit(getPins());
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

    // ---- pin map: V4 Full, V4 Light (no VE.Direct) or nothing ----
    {
        BoardPins p = boardPins(false);
        assert(p.windSpeed < 0 && p.rain < 0 && p.oneWire < 0 && p.windDir < 0 && p.vaneEn < 0 && p.veRx < 0 && p.veTx < 0);
        p = boardPins(true, true);   // V4 Light
        assert(p.windSpeed == 44 && p.rain == 16 && p.oneWire == 43 && p.windDir == 6 && p.vaneEn == 34 && p.veRx < 0 && p.veTx < 0);
        p = boardPins(true);
        assert(p.windSpeed == 44 && p.rain == 16 && p.oneWire == 43 && p.windDir == 6 && p.vaneEn == 34 && p.veRx == 33 && p.veTx == 45);
        // never the USB (19/20), the FEM (2/5/7/46) or the PCB ID pin 15 as an output
        for (int8_t pin : { p.windSpeed, p.rain, p.oneWire, p.windDir, p.vaneEn, p.veRx, p.veTx })
            assert(pin != 19 && pin != 20 && pin != 2 && pin != 5 && pin != 7 && pin != 46 && pin != 15);
    }
    assert(BOARD_HW_ID_PIN == 15);

    // ---- ID detection (GPIO15) ----
    assert(PCB_REV_V4_FULL == 20 && PCB_REV_V4_LIGHT == 21);       // API / MQTT pcbRev, never renumber
    auto detect = [](int mv) {
        for (int i = 0; i < 64; i++) hostAdc[i] = 0;
        gBoard.pcb = gBoard.pcbLight = false;
        hostAdc[BOARD_HW_ID_PIN] = mv;
        return detectPcb();
    };
    assert(detect(1650) == PCB_REV_V4_FULL && gBoard.pcb && !gBoard.pcbLight && gIo.hwIdDetected);
    assert(boardPcbAllowed() && boardPcbFull() && ioFull() && std::string(boardPcbName()) == "V4 Full");
    assert(hostModes[15] == INPUT && hostModes[20] != OUTPUT);       // discharged, released; USB D+ untouched
    assert(detect(0) == 0 && !gBoard.pcb && !boardPcbAllowed());      // bare V4 ~0 V
    assert(std::string(boardPcbName()) == "none");
    assert(detect(3300) == 0 && !gBoard.pcb);
    assert(detect(1299) == 0 && detect(1300) == PCB_REV_V4_FULL && detect(2000) == PCB_REV_V4_FULL && detect(2001) == 0);
    // V4 Light: 1M / 470k -> ~1.06 V, window 850..1250 mV
    assert(detect(1060) == PCB_REV_V4_LIGHT && gBoard.pcb && gBoard.pcbLight && gIo.hwIdDetected);
    assert(boardPcbAllowed() && !boardPcbFull() && !ioFull() && std::string(boardPcbName()) == "V4 Light");
    assert(detect(840) == 0 && !gBoard.pcb && !gBoard.pcbLight);
    assert(detect(849) == 0 && detect(850) == PCB_REV_V4_LIGHT);
    assert(detect(900) == PCB_REV_V4_LIGHT && detect(1200) == PCB_REV_V4_LIGHT && detect(1250) == PCB_REV_V4_LIGHT);
    assert(detect(1251) == 0 && detect(1290) == 0 && !gBoard.pcb);  // gap between the windows
    assert(detect(1650) == PCB_REV_V4_FULL && !gBoard.pcbLight);   // Full after Light: flag cleared
    for (uint32_t mv = 0; mv <= 3300; mv += 10) {
        int r = pcbRevFromHwIdMv(mv);
        assert(r == (mv >= 1300 && mv <= 2000 ? PCB_REV_V4_FULL : mv >= 850 && mv <= 1250 ? PCB_REV_V4_LIGHT : 0));
    }
    hostAdc[15] = 0; hostAdc[20] = 1650; gBoard.pcb = false;
    assert(detectPcb() == 0 && !gBoard.pcb);                          // GPIO20 (the V3's ID pin) is never read

    // ---- V4 Light: wind, rain, vane switch, 1-Wire; no I/O block ----
    gBoard.pcb = true; gBoard.pcbLight = true;
    gCfg = Config{};
    gCfg.pcbVersion = PCB_REV_V4_LIGHT; gCfg.windEn = 1; gCfg.rainEn = 1;
    // settings of a V4 Full left on (moved module, restored backup): ignored
    gCfg.heaterMode = 2; gCfg.fanMode = 1; gCfg.tachoEn = 1; gCfg.veDirectEn = 0;
    gCfg.auxAinEn = 1; gCfg.auxPulseEn = 1; gCfg.uvEn = 1;
    assert(boardPcbAllowed() && !ioFull() && windDirPin() == 6);
    assert(getPins().windSpeed == 44 && getPins().rain == 16 && getPins().oneWire == 43);
    assert(!ioAnyEnabled() && !auxPulseInputUsed());
    assert(applyParam(PARAM_WIND_EN, 1) && applyParam(PARAM_RAIN_EN, 1));
    assert(!applyParam(PARAM_HEATER_MODE, 1) && !applyParam(PARAM_FAN_MODE, 1) && !applyParam(PARAM_UV_EN, 1));
    assert(!applyParam(PARAM_VEDIRECT_EN, 1) && !applyParam(PARAM_WIND2_MODE, 1) && !applyParam(PARAM_AUX_PULSE_EN, 1));
    assert(!applyParam(PARAM_AUX_AIN_EN, 1) && !applyParam(PARAM_TACHO_EN, 1));
    assert(applyParam(PARAM_HEATER_MODE, 0) && applyParam(PARAM_VEDIRECT_EN, 0));   // switching off is fine
    for (int i = 0; i < 64; i++) { hostModes[i] = -1; hostPins[i] = -1; }
    sensorsEarlyInit();
    sensorsInit(getPins());
    assert(hostModes[PIN_1WIRE_HSS] == OUTPUT && hostPins[PIN_1WIRE_HSS] == LOW);   // 1-Wire supply (GPIO47) present
    assert(hostModes[6] == INPUT);                                                 // vane ADC
    ioInit();
    assert(hostModes[34] == OUTPUT && hostPins[34] == LOW);           // vane supply on (LOW = on)
    windVaneSupply(false);
    assert(hostPins[34] == HIGH);
    windVaneSupply(true);
    ioPoll(); ioSample(); ioFinishCycle(1000); ioSuspendOutputs();
    for (int pin : { 3, 4, 33, 38, 39, 40, 45, 48 })                  // UV, AUX, VE.Direct, heater, fan, tacho
        assert(hostModes[pin] == -1);
    assert(!gIo.heaterOn && !gIo.fanOn && ioStatusByte() == 0);
    startOneWireConversion();                                         // bus searched (no probe: supply off again)
    assert(hostModes[PIN_1WIRE_HSS] == OUTPUT && hostPins[PIN_1WIRE_HSS] == LOW && hostModes[43] == INPUT);
    gBoard.pcbLight = false;

    // ---- V4 on the V4 Full: sensors and I/O on the V4 pins ----
    gBoard.pcb = true;
    gCfg = Config{};
    gCfg.pcbVersion = PCB_REV_V4_FULL; gCfg.windEn = 1; gCfg.rainEn = 1;
    assert(boardPcbAllowed() && ioFull() && windDirPin() == 6);
    assert(getPins().windSpeed == 44 && getPins().rain == 16 && getPins().oneWire == 43);
    assert(applyParam(PARAM_WIND_EN, 1) && applyParam(PARAM_HEATER_MODE, 1));
    for (int i = 0; i < 64; i++) { hostModes[i] = -1; hostPins[i] = -1; }
    gCfg.veDirectEn = 0;
    assert(hostModes[45] == -1);                                      // nothing before ioInit()
    ioInit();                                                         // end of setup(), after the ID read
    assert(hostModes[45] == OUTPUT && hostPins[45] == HIGH);          // VE TX idle high, VE.Direct off
    assert(hostModes[34] == OUTPUT && hostPins[34] == LOW);           // vane supply on (LOW = on)
    assert(hostModes[19] == -1 && hostModes[20] == -1);               // USB: untouched
    // bare V4: VE TX never driven
    gBoard.pcb = false;
    for (int i = 0; i < 64; i++) { hostModes[i] = -1; hostPins[i] = -1; }
    ioPoll();
    assert(hostModes[45] == -1 && hostModes[34] == -1);
    return 0;
}
