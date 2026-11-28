#pragma once
// Heltec WiFi LoRa 32 V4: board check and the RF front end.
//
// This firmware line runs on the V4 only (ESP32-S3R2: 16 MB flash, 2 MB quad
// PSRAM in the package); the V3 is served by the 2.x line (branch 2.x). The
// V4 puts an RF front-end module (FEM: PA + LNA) between the SX1262 and the
// antenna, controlled by these GPIOs:
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
// PCB: two boards carry a V4 (same pin map, boardPins()), recognised by the
// revision ID divider on GPIO15: V4 Full (1M/1M, ~1.65 V: wind, rain, 1-Wire,
// I2C plus the I/O block - heater, fan, tacho, AUX, UV, VE.Direct) and V4
// Light (1M/470k, ~1.06 V: wind, rain, 1-Wire, I2C only). A V4 without an ID
// runs as a wireless station (wireless sensors, optional BME280 on GPIO47/48,
// optional GNSS module on the V4's GNSS connector) and leaves every PCB
// input / output unconfigured (boardPcbAllowed()).
//
// This header is host-compiled (test/firmware_board.cpp): no RadioLib here.
#include <stdint.h>

enum FemType : uint8_t { FEM_NONE = 0, FEM_GC1109 = 1, FEM_KCT8103L = 2 };

// eFuse PSRAM_CAP (BLK1 bits 131-132): 0 = none, 1 = 8 MB (S3R8, V4-R8), 2 = 2 MB (S3R2, V4)
constexpr int PSRAM_CAP_8MB = 1;
constexpr int PSRAM_CAP_2MB = 2;

struct BoardInfo {
    uint32_t  flashBytes = 0;          // physical flash size (JEDEC ID), 0 = unknown
    int       psramCap   = -1;         // eFuse PSRAM_CAP, -1 = not read
    FemType   fem        = FEM_NONE;   // detected front-end module
    bool      pcb        = false;      // a PCB revision ID (V4 Full or V4 Light) found on GPIO15
    bool      pcbLight   = false;      // ... and it is the V4 Light (no I/O block)
};
extern BoardInfo gBoard;

// A Heltec V4: 16 MB flash and the 2-MB quad PSRAM of the ESP32-S3R2. A V3
// (8 MB, no PSRAM) belongs to firmware 2.x; the V4-R8 (ESP32-S3R8, 8 MB
// octal PSRAM on GPIO33-37) has another pin map and is not supported.
inline bool boardSupportedBy(uint32_t flashBytes, int psramCap)
{
    return flashBytes >= 16UL * 1024 * 1024 && psramCap == PSRAM_CAP_2MB;
}
inline bool boardSupported() { return boardSupportedBy(gBoard.flashBytes, gBoard.psramCap); }
inline bool boardIsR8() { return gBoard.psramCap == PSRAM_CAP_8MB; }
// PCB functions (wind, rain, vane switch, 1-Wire) only on a PCB (V4 Full or
// V4 Light, revision ID read) - otherwise its pins would collide with the FEM.
inline bool boardPcbAllowed() { return gBoard.pcb; }
// The I/O block (heater, fan, tacho, AUX analog/pulse, UV, VE.Direct): V4 Full only.
inline bool boardPcbFull() { return gBoard.pcb && !gBoard.pcbLight; }
inline const char* boardPcbName() { return !gBoard.pcb ? "none" : gBoard.pcbLight ? "V4 Light" : "V4 Full"; }

// GPIOs of the PCB functions; -1 = not present (no PCB).
struct BoardPins {
    int8_t windSpeed, rain, oneWire;   // pulse counters (falling edge via the Schmitt trigger), 1-Wire DQ
    int8_t windDir;                    // wind direction ADC (ADC1)
    int8_t vaneEn;                     // vane supply, LOW = on
    int8_t veRx, veTx;                 // VE.Direct
};
inline BoardPins boardPins(bool pcb, bool light = false)
{
    if (!pcb) return { -1, -1, -1, -1, -1, -1, -1 };
    if (light) return { 44, 16, 43, 6, 34, -1, -1 };   // V4 Light: no VE.Direct
    return { 44, 16, 43, 6, 34, 33, 45 };              // V4 Full
}
inline BoardPins boardPins() { return boardPins(gBoard.pcb, gBoard.pcbLight); }
// Revision ID divider of the V4 Full / V4 Light on the V4's GPIO15 (header P4)
constexpr int BOARD_HW_ID_PIN = 15;

// ---------------------------------------------------------------------------
// GNSS connector of the V4 (B9), only on a V4 without PCB: the V4 Full / V4
// Light use GPIO34 and 38-42 themselves (vane supply, heater, fan, tacho ...).
// Pins and levels from Meshtastic's heltec_v4 variant (variants/esp32s3/
// heltec_v4/variant.h, read 2026-11-26; hardware facts only, no code):
//   GPIO34  VGNSS_Ctrl  module supply, active LOW (GPS_EN_ACTIVE LOW)  - to verify on the device
//   GPIO39  module TX -> ESP RX   (verified on the device; GNSS_UART_SWAPPED)
//   GPIO38  ESP TX -> module RX
//   GPIO40  wake / standby: HIGH = forced awake, LOW = may sleep
//   GPIO41  PPS (1 pulse per second, rising edge = start of the UTC second)
//   GPIO42  reset, active LOW (L76K: LOW > 100 ms resets)
// UART1 (UART0 belongs to the boot ROM / USB log).
// ---------------------------------------------------------------------------
constexpr int  GNSS_PIN_VCTRL     = 34;
constexpr int  GNSS_VCTRL_ON      = 0;      // LOW = module powered (verified on a V4.3 with the Heltec module, 2026-11-26)
// Verified on a V4.3 (2026-11-26): the ESP receives the module's NMEA on GPIO39 and sends on GPIO38
// (the reverse of the reading of the third-party pin list) - hence "swapped".
constexpr bool GNSS_UART_SWAPPED  = true;
constexpr int  GNSS_PIN_ESP_RX    = GNSS_UART_SWAPPED ? 39 : 38;
constexpr int  GNSS_PIN_ESP_TX    = GNSS_UART_SWAPPED ? 38 : 39;
constexpr int  GNSS_PIN_WAKE      = 40;
constexpr int  GNSS_PIN_PPS       = 41;
constexpr int  GNSS_PIN_RESET     = 42;
constexpr int  GNSS_UART_NUM      = 1;

// GNSS runs only on a V4 without PCB and when it is switched on in the settings.
inline bool gnssActiveFor(bool pcb, int gnssEn) { return gnssEn && !pcb; }

// ---------------------------------------------------------------------------
// BME280 pins. With a PCB (V4 Full / V4 Light): GPIO41/42, where the PCB has
// it. Without PCB (wireless build): GPIO47/48 on the S3's second I2C
// controller (the OLED keeps its own bus on 17/18), because 41/42 belong to
// the GNSS connector. Old wiring (2.x / 3.x documented 41/42 for every board)
// is still found: when nothing answers on 47/48 and GNSS is off, 41/42 are
// probed and used, with a hint to move the sensor. With GNSS on, 41/42 are
// never driven as I2C (41 = PPS input, 42 = the module's reset).
// ---------------------------------------------------------------------------
constexpr int BME_PCB_SDA = 41, BME_PCB_SCL = 42;     // PCB, and the old wireless wiring
constexpr int BME_WIRELESS_SDA = 47, BME_WIRELESS_SCL = 48;

struct BmePins {
    int8_t sda, scl;
    bool   oldWiring;   // a wireless build with the BME280 on the old pins 41/42
};
// Probe 41/42 after 47/48 found nothing?
inline bool bmeProbeOldPins(bool pcb, bool gnssOn, bool found4748) { return !pcb && !gnssOn && !found4748; }
// The pins to keep (also for the hot-plug re-probe) after probing.
inline BmePins bmePinSelect(bool pcb, bool gnssOn, bool found4748, bool found4142)
{
    if (pcb) return { BME_PCB_SDA, BME_PCB_SCL, false };
    if (found4748) return { BME_WIRELESS_SDA, BME_WIRELESS_SCL, false };
    if (!gnssOn && found4142) return { BME_PCB_SDA, BME_PCB_SCL, true };
    return { BME_WIRELESS_SDA, BME_WIRELESS_SCL, false };
}

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
void boardLog();                // the board line in the log (after the serial log started)
void femInit();                 // V4: power the FEM, detect its type, RF switch table on the radio
