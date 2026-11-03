// PCB v2.0 I/O block: revision ID, UV ADC, AUX analog/pulse inputs, heater
// and fan outputs, fan tacho and isolated VE.Direct. Functions are off by
// default and ignored on Rev 1.2/1.3 boards.
#pragma once

#include <Arduino.h>
#include "vedirect.h"

#define PIN_HW_ID        20   // revision ID divider (ADC2 - read before WiFi starts)
#define PIN_UV_AIN        3   // PCB v2.0 UV input (ADC1), input-only even when disabled
#define PIN_AUX_AIN       4   // AUX analog input (ADC1), 10k series, optional 15k divider
#define PIN_AUX_PULSE    48   // AUX pulse input via inverting Schmitt trigger
#define PIN_HEATER       38   // heater output J12, low side, HIGH = on
#define PIN_FAN          39   // fan output J13, high side, HIGH = on
#define PIN_TACHO        40   // fan tacho via inverting Schmitt trigger
#define PIN_VE_RX        33   // from the Victron TX (through the isolator)
#define PIN_VE_TX        34   // to the Victron RX (not used, idle high)

#define PCNT_UNIT_AUX_PULSE  PCNT_UNIT_2
#define PCNT_UNIT_TACHO      PCNT_UNIT_3

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
    bool     hwIdDetected = false;   // revision ID divider read as v2.0
    float    uvSensorVolts = NAN;    // terminal voltage, cycle average
    float    uvIndex = NAN;          // only present with explicitly enabled, valid calibration
    bool     uvIndexCalibrated = false;
    float    auxAinV = 0.0f;         // cycle average, already scaled
    uint32_t auxPulsesCycle = 0;
    uint32_t auxPulsesTotal = 0;     // since power-on (kept across restarts)
    float    wind2SpeedMs = 0.0f;    // 2nd anemometer (wind2Mode): cycle average
    float    wind2GustMs = 0.0f;     // highest sub-sample of the cycle
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

// Reads the revision ID divider on GPIO20 and returns true for PCB v2.0.
// Must run before WiFi is started (ADC2).
bool detectPcbV2();

// True when the board is v2.0 (detected or set manually).
bool ioV2();

// Puts the outputs into a safe state (off) and starts whatever is enabled.
// Call once after sensorsInit(); no-op on Rev 1.2/1.3.
void ioInit();
// Latches heater/fan off until restart, including during config/OTA.
void ioSuspendOutputs();
// Every loop() iteration: VE.Direct reception and picking up enable changes
// made by applyParam() (downlink, MQTT, web settings).
void ioPoll();
// Every sub-sample: AUX analog/pulse and tacho accumulation, output control.
void ioSample();
// End of a measurement cycle: finished values into gIo, accumulators reset.
void ioFinishCycle(unsigned long elapsedMs);

uint8_t ioStatusByte();
bool    ioAnyEnabled();        // any v2.0 function enabled (status byte is sent)
void    ioResetAuxPulseTotal();
// Wind-vane supply (PCB v2.0: Q6 on GPIO19, LOW = on). On permanently in
// normal operation, only around each reading in low-power mode. No-op on
// Rev 1.2/1.3 (GPIO3 is the wind-speed input on Rev 1.2).
void    windVaneSupply(bool on);
