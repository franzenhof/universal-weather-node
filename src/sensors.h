#pragma once

#include <Arduino.h>

// ============================================================================
// FIXED PIN ASSIGNMENT (independent of PCB revision)
// ============================================================================
#define PIN_ADC_VOLTBATTERY     1   // LiPo voltage measurement (ADC)
#define PIN_ADC_WINDDIRECTION   2   // Davis wind direction (potentiometer, ADC)
#define PIN_ADC_BATCTRL        37   // HIGH enables ADC measurement path (from heltec wifi lora v3 board rev. 3.2)
#define PIN_1WIRE_HSS          47   // High-side switch VCC_1WIRE (LP0701N3-G via TN2106N3-G, from lorawan-weather-node pcb rev 1.3)
#define PIN_I2C_SDA_BME280     41
#define PIN_I2C_SCL_BME280     42

// ============================================================================
// PCNT CONFIGURATION (reed switch to GND). Hardware debounce on the PCB (see
// README "Custom PCB"): wind speed 330 Ω + 4.7 kΩ + 100 nF (tau ~0.5-0.9 ms),
// rain 330 Ω + 10 kΩ + 1 µF (tau ~10-15 ms), plus a 74LVC2G14 Schmitt
// trigger on Rev 1.3+. The wind filter is deliberately short (fast pulses at
// high wind), so occasional contact bounce/EMI can still get through — see
// MAX_PLAUSIBLE_WIND_SPEED_MS below for the software-side guard.
// ============================================================================
#define PCNT_UNIT_WINDSPEED     PCNT_UNIT_0
#define PCNT_CHANNEL_WINDSPEED  PCNT_CHANNEL_0
#define FILTER_VALUE_WINDSPEED  100   // approx. 1.25 µs at APB=80 MHz → PCNT glitch filter

#define PCNT_UNIT_RAIN          PCNT_UNIT_1
#define PCNT_CHANNEL_RAIN       PCNT_CHANNEL_0
#define FILTER_VALUE_RAIN       100

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
PinSet getPins(int pcbVersion);

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
float    readPulseCounters();   // updates windSpeedMs/windGustMs/rainMMSinceStart; returns this sub-sample's rain (mm)
void     readBme280();          // updates bme280TempC/Humidity/PressAbs/PressSea
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
