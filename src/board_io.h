// Revision ID (V4 Full / V4 Light) and the V4 Full I/O block: UV ADC, AUX
// analog/pulse inputs, heater and fan outputs, fan tacho and isolated
// VE.Direct. Functions are off by default and ignored on the V4 Light and
// without a PCB.
#pragma once
#include "pulse_counter.h"

#include <Arduino.h>
#include "vedirect.h"

// revision ID divider (ADC2 - read before WiFi starts): GPIO15 (BOARD_HW_ID_PIN, heltec_board.h)
#define PIN_UV_AIN        3   // V4 Full UV input (ADC1), input-only even when disabled
#define PIN_AUX_AIN       4   // AUX analog input (ADC1), 10k series, optional 15k divider
#define PIN_AUX_PULSE    48   // AUX pulse input via inverting Schmitt trigger
#define PIN_HEATER       38   // heater output J12, low side, HIGH = on
#define PIN_FAN          39   // fan output J13, high side, HIGH = on
#define PIN_TACHO        40   // fan tacho via inverting Schmitt trigger
// VE.Direct RX 33 (both), TX V3 34 / V4 45 (strap pin: R65 holds it low at reset,
// driven HIGH = idle only after boot) - heltec_board.h boardPins()

// Counters PCNT_UNIT_AUX_PULSE / PCNT_UNIT_TACHO: pulse_counter.h

// io status bits (payload: CayenneLPP Ch3 Digital Input, binary BIN_IO_STATUS)
constexpr uint8_t IO_HEATER_ON    = 0x01;
constexpr uint8_t IO_FAN_ON       = 0x02;
constexpr uint8_t IO_FAN_STALLED  = 0x04;   // fan on at 100 % for a whole cycle, no tacho pulses
constexpr uint8_t IO_BATT_LOCKOUT = 0x08;   // heater/fan held off: battery below outMinBattDv
                                            // (or no VE.Direct voltage while the limit is set)
constexpr uint8_t IO_VE_NO_DATA   = 0x10;   // VE.Direct enabled, no valid block for > 10 s

// Finished values of the last measurement cycle (and the live output/
// VE.Direct state), read by the payload encoders, MQTT and the web pages.
struct IoState {
    bool     hwIdDetected = false;   // revision ID divider read (V4 Full or V4 Light)
    float    uvSensorVolts = NAN;    // terminal voltage, cycle average
    float    uvIndex = NAN;          // only present with explicitly enabled, valid calibration
    bool     uvIndexCalibrated = false;
    float    auxAinV = 0.0f;         // cycle average, already scaled
    uint32_t auxPulsesCycle = 0;
    uint32_t auxPulsesTotal = 0;     // since power-on (kept across restarts)
    bool     wind2Valid = false;     // 2nd anemometer (wind2Mode): an accepted sample this cycle
    float    wind2SpeedMs = 0.0f;    // cycle average (pulse-weighted); 0 while !wind2Valid
    float    wind2GustMs = 0.0f;     // highest 3-s gust of the cycle
    uint16_t wind2DirDeg = 0;        // vector average incl. wind2DirOffsetDeg (wind2Mode 2)
    bool     heaterOn = false;
    uint8_t  heaterDuty = 0;         // % currently driven
    bool     fanOn = false;
    uint8_t  fanDuty = 0;
    float    fanRpm = 0.0f;          // cycle average
    bool     fanStalled = false;
    bool     battLockout = false;
    bool     veValid = false;        // a valid VE.Direct block within the last 10 s
    VeDirectValues ve = {};
};
extern IoState gIo;

// AUX pulses (counter or 2nd anemometer) counted in software during light
// sleep (low_power.cpp), picked up by the next ioPoll()
extern volatile uint32_t sleepPulsesAux;

// True when the AUX pulse input is in use (counter or 2nd anemometer).
bool    auxPulseInputUsed();

// Board type for a revision ID voltage: PCB_REV_V4_FULL (1300..2000 mV),
// PCB_REV_V4_LIGHT (850..1250 mV), else 0 (no PCB).
int pcbRevFromHwIdMv(uint32_t mv);
// Reads the revision ID divider on GPIO15, sets gBoard.pcb / pcbLight and
// returns the board type (pcbRevFromHwIdMv()). Must run before WiFi is
// started (ADC2).
int detectPcb();

// True on the V4 Full: the I/O block (heater, fan, tacho, AUX, UV, VE.Direct).
bool ioFull();
// Measurements that really run (one place for the conditions, used by the
// status page, JSON, LoRaWAN, Home Assistant and history).
bool ioUvActive();
bool ioAuxAinActive();       // also false while the 2nd vane (wind2Mode 2) uses AIN
bool ioWind2DirActive();

// Puts the outputs into a safe state (off) and starts whatever is enabled.
// Call once after sensorsInit(); no-op unless on the V4 Full.
void ioInit();
// Latches heater/fan off until restart (config portal; a web OTA leaves them
// under normal control).
void ioSuspendOutputs();
// Every loop() iteration: VE.Direct reception and picking up enable changes
// made by applyParam() (downlink, MQTT, web settings).
void ioPoll();
// Every sub-sample: AUX analog/pulse and tacho accumulation, output control.
void ioSample();
// End of a measurement cycle: finished values into gIo, accumulators reset.
void ioFinishCycle(unsigned long elapsedMs);

uint8_t ioStatusByte();
bool    ioAnyEnabled();        // any V4 Full I/O function enabled (status byte is sent)
void    ioResetAuxPulseTotal();
// Wind-vane supply (V4 Full / V4 Light: GPIO34, LOW = on). On permanently in
// normal operation, only around each reading in low-power mode. No-op
// without a PCB.
void    windVaneSupply(bool on);
