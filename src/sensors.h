#pragma once
#include "pulse_counter.h"

#include <Arduino.h>

// ============================================================================
// FIXED PIN ASSIGNMENT (independent of PCB revision)
// ============================================================================
#define PIN_ADC_VOLTBATTERY     1   // LiPo voltage measurement (ADC)
// Davis wind direction (ADC): GPIO2 on a V3, GPIO6 on a V4 with PCB (V4 Full / V4 Light) - boardPins() / windDirPin()
// Below this the LiPo input reads only noise: no battery connected (USB powered)
constexpr float BATTERY_PRESENT_V = 2.5f;
#define PIN_ADC_BATCTRL        37   // HIGH enables ADC measurement path (from heltec wifi lora v3 board rev. 3.2)
#define PIN_1WIRE_HSS          47   // High-side switch VCC_1WIRE (LP0701N3-G via TN2106N3-G, from universal-weather-node pcb rev 1.3)
// BME280 pins: heltec_board.h (bmePinSelect): PCB 41/42, wireless build 47/48

// ============================================================================
// PCNT CONFIGURATION (reed switch to GND). Hardware debounce on the PCB (see
// README "Custom PCB"): wind speed 330 Ω + 4.7 kΩ + 100 nF (tau ~0.5-0.9 ms),
// rain 330 Ω + 10 kΩ + 1 µF (tau ~10-15 ms), plus a 74LVC2G14 Schmitt
// trigger on Rev 1.3+. The wind filter is deliberately short (fast pulses at
// high wind), so occasional contact bounce/EMI can still get through — see
// MAX_PLAUSIBLE_WIND_SPEED_MS below for the software-side guard.
// ============================================================================
// Counters PCNT_UNIT_WINDSPEED / PCNT_UNIT_RAIN, 1.25 µs glitch filter: pulse_counter.h

// Sub-samples above this are rejected as sensor glitches (reed-switch contact
// bounce / EMI burst on the wind pulse line), not real wind: the Davis
// anemometer's own rated measuring range ends at 175 mph / 78 m/s / 281 km/h
// (see datasheet) — nothing above that is physically producible by this
// sensor regardless of actual weather. Still well below the CayenneLPP
// analog-input encoding ceiling (~327 km/h). A rejected sample is dropped
// entirely (not averaged in, not fed into the gust ratchet) rather than
// clamped, since a clamped-but-still-wrong value would silently distort both
// windSpeedMs and windGustMs the same way the raw glitch did.
#define MAX_PLAUSIBLE_WIND_SPEED_MS  78.0f   // 281 km/h — Davis anemometer rated max

// Pulse inputs (wind, rain, AUX / 2nd anemometer) are ignored until
// PULSE_SETTLE_MS after the end of setup(): lines and outputs settle after a
// start - a burst there once read as a 226 km/h gust right after an update.
constexpr unsigned long PULSE_SETTLE_MS = 10000;
void pulseSettleStart();   // end of setup()
bool pulseSettling();      // true until PULSE_SETTLE_MS after pulseSettleStart()

// ============================================================================
// I2C / BME280
// ============================================================================
#define I2C_FREQUENCY           50000
#define I2C_ADRESS_BME280       0x76

// DS18B20
#define MAX_SENSORS_PER_BUS     3

// ============================================================================
// PIN ASSIGNMENT BY PCB REVISION (runtime)
// Rev 1.2: Wind=GPIO3, Rain=GPIO4, 1-Wire=GPIO5
// Rev 1.3: Wind=GPIO6, Rain=GPIO7, 1-Wire=GPIO5
// ============================================================================
struct PinSet { int windSpeed, rain, oneWire; };
PinSet getPins();                 // PCB pins (V4 Full / V4 Light), or none without a PCB (heltec_board.h boardPins())
int windDirPin();                 // wind direction ADC of the board in effect, -1 = none
extern PinSet gPins;

// Wind / rain pulses counted in software during light sleep (low_power.cpp),
// added to the PCNT counts by readPulseCounters()
extern volatile uint32_t sleepPulsesWind, sleepPulsesRain;

// false = the last base cycle had no valid wind sample (all rejected, or a
// wireless wind source without fresh data): windSpeedMs/windGustMs are 0
// then and the payloads leave wind out. Written by main.cpp.
extern bool windCycleValid;

// Running rain total for the per-path rain windows (main.cpp): never goes
// backwards, unlike rainMMSinceStart, which resetRainAccumulator() sets to 0.
float rainWindowTotalMm();
void  resetRainAccumulator();   // rainMMSinceStart = 0, the windows keep counting

// Early ADC pin setup — must run before any possible WiFiManager portal call,
// including the very first boot, so the wind-direction ADC is already usable
// for the portal's "Calibrate Wind Direction" page (see /calibrate/sample in
// portal.cpp). Call once from setup(), before the first-boot portal check.
void sensorsEarlyInit();

// Full sensor init (BME280, 1-Wire, PCNT) — run once gCfg/pcbVersion are
// known, i.e. after the (possible) first-boot portal has completed.
void sensorsInit(const PinSet& pins);

int      adcAverage(int pin, int samples = 8);
float    readBatteryVoltage();
uint16_t readWindDirection();
// One wind sample: raw pulses since the previous read. Aggregation (means,
// 3-s gust, per-path windows) happens in wind_window.h / main.cpp.
struct PulseSample {
    bool     windValid  = false;   // wind enabled and the rate is plausible
    uint32_t windPulses = 0;
    uint16_t elapsedMs  = 0;       // time the pulses were counted over
    float    rainMm     = 0.0f;    // also added to rainMMSinceStart
};
PulseSample readPulseCounters();
// The BME280 pins in use (SDA, SCL), true = the old wireless wiring on 41/42
// (hint: move it to 47/48), and its I2C address.
void     sensorsBmePins(int& sda, int& scl, bool& oldWiring, uint8_t& addr);
bool     readBme280();          // updates bme280TempC/Humidity/PressAbs/PressSea; re-probes/re-inits if absent (hot-plug), false if no valid reading
// Non-blocking DS18B20 read, split across two calls so loop() (button, WiFi/
// MQTT poll, watchdog) stays responsive during the ~750 ms conversion time
// instead of blocking on it. Call startOneWireConversion() once to kick off
// the read (powers the bus, issues the non-blocking conversion request),
// then pollOneWireConversion() on every following loop() iteration until it
// returns true (conversion complete, bus1Temps[] updated, bus powered back
// down) — only then is it safe to use the DS18B20 values for this cycle.
void     startOneWireConversion();
bool     pollOneWireConversion();

// PCNT control, exposed for params.cpp (wind/rain sensor enable downlink/MQTT params).
void configureWindPcnt();
void stopWindPcnt();
void configureRainPcnt();
void stopRainPcnt();
