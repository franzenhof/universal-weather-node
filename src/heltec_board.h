#pragma once
// Heltec WiFi LoRa 32 V3 / V4: board detection and the V4 RF front end.
//
// The V4 (ESP32-S3R2, 16 MB flash) is pin-compatible with the V3 (ESP32-S3FN8,
// 8 MB in-package flash) for the SX1262, OLED, Vext, PRG button and battery
// divider, but puts an RF front-end module (FEM: PA + LNA) between the SX1262
// and the antenna, controlled by GPIOs that the V3 leaves free on the header:
//
//   GPIO7   VFEM_Ctrl  FEM supply LDO enable (HIGH = on)             V4.2 + V4.3
//   GPIO2   CSD        FEM chip enable (HIGH = on, LOW = shutdown)    V4.2 + V4.3
//   GPIO46  CPS        GC1109 PA mode (HIGH = full PA, LOW = bypass)  V4.2 only
//   GPIO5   CTX        KCT8103L RX path (LOW = LNA, HIGH = bypass/TX) V4.3 only
//   DIO2    (SX1262)   TX/RX select: GC1109 CTX / KCT8103L CPS (RadioLib DIO2 RF switch)
//
// Levels follow Meshtastic's LoRaFEMInterface (variants/esp32s3/heltec_v4):
//   GC1109   RX: CSD=1 CPS=0 (CTX=DIO2=0 -> LNA)   TX: CSD=1 CPS=1 (CTX=DIO2=1 -> PA)
//   KCT8103L RX: CSD=1 CTX=0 (-> LNA)             TX: CSD=1 CTX=1 (CPS=DIO2=1 -> PA)
//   sleep: CSD=0, CPS/CTX=0; KCT8103L also VFEM=0 (GC1109 keeps VFEM on)
// The FEM type is read from GPIO2 as input with VFEM on: HIGH = KCT8103L.
//
// PCB Rev 1.2 / 1.3 and the V3 sockets of v2.0 use GPIO2 (vane), GPIO5
// (1-Wire) and GPIO7 (rain), which the V4 needs for the FEM. PCB v2.0 has a
// second socket set for the V4 with its own pin map (boardPins(), docs/pcb.md
// "Heltec V3 or V4 on the PCB"), recognised by the revision ID on GPIO15. A
// V4 without that ID runs the PCB-less build (wireless sensors, optional
// BME280 on GPIO41/42) and leaves every PCB input / output unconfigured
// (boardPcbAllowed()).
//
// This header is host-compiled (test/firmware_board.cpp): no RadioLib here.
#include <stdint.h>

enum BoardKind : uint8_t { BOARD_V3 = 3, BOARD_V4 = 4 };
// Config::boardSel: 0 = auto (by flash size), 3 = V3, 4 = V4
constexpr int BOARD_SEL_AUTO = 0;
inline bool boardSelValid(int v) { return v == BOARD_SEL_AUTO || v == BOARD_V3 || v == BOARD_V4; }

enum FemType : uint8_t { FEM_NONE = 0, FEM_GC1109 = 1, FEM_KCT8103L = 2 };

// eFuse PSRAM_CAP (BLK1 bits 131-132): 0 = none, 1 = 8 MB, 2 = 2 MB
constexpr int PSRAM_CAP_8MB = 1;

struct BoardInfo {
    BoardKind kind       = BOARD_V3;   // in effect (setting or detection)
    BoardKind detected   = BOARD_V3;   // by flash size
    bool      autoSel    = true;       // setting is "Auto"
    uint32_t  flashBytes = 0;          // physical flash size (JEDEC ID), 0 = unknown
    int       psramCap   = -1;         // eFuse PSRAM_CAP, -1 = not read
    FemType   fem        = FEM_NONE;   // V4: detected front-end module
    bool      pcbOnV4    = false;      // V4: PCB v2.0 revision ID found on GPIO15
};
extern BoardInfo gBoard;

// The V3 has 8 MB (in-package, ESP32-S3FN8), the V4 16 MB (external). The
// V4-R8 variant (ESP32-S3R8, 8 MB octal PSRAM) also has 16 MB but a
// different pin map (Vext GPIO40, GPIO33-37 taken by the PSRAM): flagged by
// boardIsR8(), not supported.
inline BoardKind boardDetect(uint32_t flashBytes)
{
    return flashBytes >= 16UL * 1024 * 1024 ? BOARD_V4 : BOARD_V3;
}

inline BoardKind boardResolve(int sel, BoardKind detected)
{
    if (sel == BOARD_V3) return BOARD_V3;
    if (sel == BOARD_V4) return BOARD_V4;
    return detected;
}

inline bool boardIsV4() { return gBoard.kind == BOARD_V4; }
inline bool boardIsR8() { return gBoard.kind == BOARD_V4 && gBoard.psramCap == PSRAM_CAP_8MB; }
// V3: always; V4: only on a PCB v2.0 (its V4 sockets; the revision ID on
// GPIO15 was read) - otherwise the PCB pins would collide with the FEM.
inline bool boardPcbAllowed() { return !boardIsV4() || gBoard.pcbOnV4; }

// GPIOs of the PCB functions; -1 = not present on that board / revision.
struct BoardPins {
    int8_t windSpeed, rain, oneWire;   // pulse counters (falling edge via the Schmitt trigger on Rev 1.3+), 1-Wire DQ
    int8_t windDir;                    // wind direction ADC (ADC1)
    int8_t vaneEn;                     // v2.0 vane supply, LOW = on
    int8_t veRx, veTx;                 // v2.0 VE.Direct
};
// kind = the board in effect, pcbOnV4 = V4 with the v2.0 ID, pcbRev = 12 / 13 / 20
inline BoardPins boardPins(BoardKind kind, bool pcbOnV4, int pcbRev)
{
    if (kind == BOARD_V4) {
        if (!pcbOnV4) return { -1, -1, -1, -1, -1, -1, -1 };
        return { 44, 16, 43, 6, 34, 33, 45 };   // V4 sockets of PCB v2.0
    }
    if (pcbRev >= 20) return { 6, 7, 5, 2, 19, 33, 34 };
    if (pcbRev >= 13) return { 6, 7, 5, 2, -1, -1, -1 };
    return { 3, 4, 5, 2, -1, -1, -1 };
}
// Revision ID divider: V3 GPIO20 (USB D+ of the unused USB PHY), V4 GPIO15 (P4).
inline int boardHwIdPin(BoardKind detected) { return detected == BOARD_V4 ? 15 : 20; }
// UART0 (GPIO43/44) only on the V3 (CP2102 USB bridge): on a V4 these pins
// are 1-Wire and wind speed on the PCB, the log goes over the native USB.
// Decided by the hardware (flash size), before the board setting is known.
inline bool boardUseUart0(BoardKind detected) { return detected != BOARD_V4; }

inline const char* boardName(BoardKind k) { return k == BOARD_V4 ? "V4" : "V3"; }
inline const char* femName(FemType f)
{
    return f == FEM_GC1109 ? "GC1109" : f == FEM_KCT8103L ? "KCT8103L" : "none";
}

// ---------------------------------------------------------------------------
// TX power with the FEM. LoRaWAN (RadioLib) asks for a radiated power (the
// regional max EIRP minus the ADR steps, EU868: 16 dBm) and sets it at the
// SX1262 - correct for the V3, ~+11..13 dB too much behind the V4's PA.
// femChipPowerDbm() returns the SX1262 setting for a wanted FEM output, from
// Meshtastic's measured net gain curves (SX1262 -> pi attenuator -> PA, per
// SX1262 dBm 0..21, LoRaFEMInterface::powerConversion()): the highest SX1262
// setting (-9..21 dBm) whose output stays at or below the wanted power, else
// the SX1262 minimum -9 dBm. Same result as Meshtastic in the linear range
// (EU868 16 dBm: GC1109 5 dBm, KCT8103L 3 dBm); in the compression range
// Meshtastic's search can end 1 dB above the request, this one cannot.
// Without a FEM: the request, clamped to -9..22 dBm.
// ---------------------------------------------------------------------------
constexpr int FEM_GAIN_POINTS = 22;
inline int femGainDb(FemType f, int chipDbm)
{
    static const uint8_t gc1109[FEM_GAIN_POINTS]   = { 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
                                                       11, 11, 11, 11, 11, 10, 10, 9, 9, 8, 7 };
    static const uint8_t kct8103l[FEM_GAIN_POINTS] = { 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
                                                       13, 13, 13, 12, 12, 11, 11, 10, 9, 8, 7 };
    if (f == FEM_NONE) return 0;
    const uint8_t* g = f == FEM_GC1109 ? gc1109 : kct8103l;
    if (chipDbm < 0) chipDbm = 0;                       // below the curve: small-signal gain
    if (chipDbm >= FEM_GAIN_POINTS) chipDbm = FEM_GAIN_POINTS - 1;
    return g[chipDbm];
}

constexpr int SX1262_MIN_DBM = -9, SX1262_MAX_DBM = 22;

inline int8_t femChipPowerDbm(int8_t targetDbm, FemType f)
{
    if (f == FEM_NONE)
        return (int8_t)(targetDbm < SX1262_MIN_DBM ? SX1262_MIN_DBM : targetDbm > SX1262_MAX_DBM ? SX1262_MAX_DBM : targetDbm);
    for (int c = FEM_GAIN_POINTS - 1; c > SX1262_MIN_DBM; c--)
        if (c + femGainDb(f, c) <= targetDbm) return (int8_t)c;
    return (int8_t)SX1262_MIN_DBM;
}

// Expected FEM output (dBm) for an SX1262 setting (status page).
inline int femOutputDbm(int chipDbm, FemType f) { return chipDbm + femGainDb(f, chipDbm); }

// ---- target only (heltec_board.cpp) ----
void boardDetectHardware();     // flash size + eFuse: first thing in setup()
void boardApply(int boardSel);  // after loadConfig(): resolves gBoard.kind
void femInit();                 // V4: power the FEM, detect its type, RF switch table on the radio
