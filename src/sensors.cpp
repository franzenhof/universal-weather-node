#include "sensors.h"
#include "globals.h"
#include "serial_log.h"
#include "sources.h"

#include "driver/pcnt.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================================
// SENSOR STATE (declared extern in globals.h)
// ============================================================================
bool     bme280Present  = false;
float    bme280TempC    = 0.0f;
float    bme280PressAbs = 0.0f;
float    bme280PressSea = 0.0f;
float    bme280Humidity = 0.0f;

float    bus1Temps[MAX_SENSORS_PER_BUS];
int      bus1Count = 0;

float    windSpeedMs   = 0.0f;
float    windGustMs    = 0.0f;
float    rainMM        = 0.0f;
float    rainRateMmH   = 0.0f;
uint16_t windDirection = 0;
float    batteryVolt   = 0.0f;
float    cpuTemperature = 0.0f;

// RTC_NOINIT_ATTR, not RTC_DATA_ATTR: the bootloader re-initializes
// RTC_DATA_ATTR on every boot except a deep-sleep wake - and this node never
// deep-sleeps, so the accumulator would reset on every ESP.restart() (e.g.
// after a manual restart) or watchdog reset. RTC_NOINIT survives those; it
// holds garbage after power-on, hence the magic word (see
// restoreRainAccumulator()).
RTC_NOINIT_ATTR float    rainMMSinceStart;
RTC_NOINIT_ATTR uint32_t rainMMSinceStartMagic;
static constexpr uint32_t RAIN_ACCU_MAGIC = 0x5241494E;   // "RAIN"

static void restoreRainAccumulator()
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool keep = reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT &&
                rainMMSinceStartMagic == RAIN_ACCU_MAGIC &&
                isfinite(rainMMSinceStart) && rainMMSinceStart >= 0.0f;
    if (!keep) {
        rainMMSinceStart      = 0.0f;
        rainMMSinceStartMagic = RAIN_ACCU_MAGIC;
    }
    dbgSerial.printf("Rain since start: %.1f mm (%s)\n", rainMMSinceStart, keep ? "kept across restart" : "reset");
}

PinSet gPins = { .windSpeed = 3, .rain = 4, .oneWire = 5 };   // set by sensorsInit()

namespace {

TwoWire            Wire2(1);
Adafruit_BME280    bme;
OneWire*           oneWireBus1 = nullptr;
DallasTemperature* sensorsBus1 = nullptr;

bool windPcntConfigured = false;
bool rainPcntConfigured = false;
int16_t pulsesLastCountWindspeed = 0;
int16_t pulsesLastCountRain      = 0;
unsigned long previousMillis     = 0;

} // namespace

// Pulses since the previous read of a free-running PCNT counter. The
// hardware counter resets to 0 when it reaches counter_h_lim (32767), so a
// plain cur - last would go negative across that wrap. Sub-samples are a few
// seconds apart, far below 32767 pulses, so at most one wrap can occur.
constexpr int16_t PCNT_H_LIM = 32767;
static int32_t pcntDelta(int16_t cur, int16_t last)
{
    return (cur >= last) ? (int32_t)cur - last : (int32_t)(PCNT_H_LIM - last) + cur;
}

PinSet getPins(int pcbVersion)
{
    // PCB v2.0 keeps the Rev 1.3 pins for wind, rain and 1-Wire
    if (pcbVersion >= PCB_REV_1_3) return { .windSpeed = 6, .rain = 7, .oneWire = 5 };
    return { .windSpeed = 3, .rain = 4, .oneWire = 5 };
}

void configureWindPcnt()
{
    if (!windPcntConfigured) {
        // Rev 1.3+ only: an inverting Schmitt trigger (74LVC2G14) sits
        // between the RC filter and this GPIO, so what was the rising edge
        // at the sensor is now the falling edge here — count negative
        // instead of positive edges to keep one count per physical pulse.
        // Rev 1.2 has no such stage (direct, non-inverted wiring) and must
        // keep counting the rising edge.
        bool inverted = (gCfg.pcbVersion >= PCB_REV_1_3);
        pcnt_config_t pcnt_wind = {
            .pulse_gpio_num = gPins.windSpeed,
            .ctrl_gpio_num  = PCNT_PIN_NOT_USED,
            .pos_mode       = inverted ? PCNT_COUNT_DIS : PCNT_COUNT_INC,
            .neg_mode       = inverted ? PCNT_COUNT_INC : PCNT_COUNT_DIS,
            .counter_h_lim  = PCNT_H_LIM,
            .counter_l_lim  = -32768,
            .unit           = PCNT_UNIT_WINDSPEED,
            .channel        = PCNT_CHANNEL_WINDSPEED,
        };
        pcnt_unit_config(&pcnt_wind);
        pcnt_set_filter_value(PCNT_UNIT_WINDSPEED, FILTER_VALUE_WINDSPEED);
        pcnt_filter_enable(PCNT_UNIT_WINDSPEED);
        windPcntConfigured = true;
    }

    pcnt_counter_pause(PCNT_UNIT_WINDSPEED);
    pcnt_counter_clear(PCNT_UNIT_WINDSPEED);
    pcnt_counter_resume(PCNT_UNIT_WINDSPEED);
    pulsesLastCountWindspeed = 0;
}

void configureRainPcnt()
{
    if (!rainPcntConfigured) {
        // Rev 1.3+ only: same inverting Schmitt trigger stage as wind speed
        // (74LVC2G14) — count the falling edge there; Rev 1.2 keeps the
        // rising edge. See configureWindPcnt() for the full explanation.
        bool inverted = (gCfg.pcbVersion >= PCB_REV_1_3);
        pcnt_config_t pcnt_rain = {
            .pulse_gpio_num = gPins.rain,
            .ctrl_gpio_num  = PCNT_PIN_NOT_USED,
            .pos_mode       = inverted ? PCNT_COUNT_DIS : PCNT_COUNT_INC,
            .neg_mode       = inverted ? PCNT_COUNT_INC : PCNT_COUNT_DIS,
            .counter_h_lim  = PCNT_H_LIM,
            .counter_l_lim  = -32768,
            .unit           = PCNT_UNIT_RAIN,
            .channel        = PCNT_CHANNEL_RAIN,
        };
        pcnt_unit_config(&pcnt_rain);
        pcnt_set_filter_value(PCNT_UNIT_RAIN, FILTER_VALUE_RAIN);
        pcnt_filter_enable(PCNT_UNIT_RAIN);
        rainPcntConfigured = true;
    }

    pcnt_counter_pause(PCNT_UNIT_RAIN);
    pcnt_counter_clear(PCNT_UNIT_RAIN);
    pcnt_counter_resume(PCNT_UNIT_RAIN);
    pulsesLastCountRain = 0;
}

void stopWindPcnt()
{
    if (!windPcntConfigured) return;
    pcnt_counter_pause(PCNT_UNIT_WINDSPEED);
    pcnt_counter_clear(PCNT_UNIT_WINDSPEED);
    pulsesLastCountWindspeed = 0;
}

void stopRainPcnt()
{
    if (!rainPcntConfigured) return;
    pcnt_counter_pause(PCNT_UNIT_RAIN);
    pcnt_counter_clear(PCNT_UNIT_RAIN);
    pulsesLastCountRain = 0;
}

void sensorsEarlyInit()
{
    restoreRainAccumulator();
    pinMode(PIN_ADC_BATCTRL,       OUTPUT);
    pinMode(PIN_ADC_VOLTBATTERY,   INPUT);
    pinMode(PIN_ADC_WINDDIRECTION, INPUT);
    adcAttachPin(PIN_ADC_VOLTBATTERY);
    adcAttachPin(PIN_ADC_WINDDIRECTION);
    analogReadResolution(12);
}

// Probe + configure the BME280. begin() re-reads chip ID and calibration
// and soft-resets the chip, so this is also the re-init path after the
// sensor was unplugged/replugged (a power-cycled BME280 comes up with
// ctrl_hum = skip, which setSampling() restores).
static bool initBme280()
{
    if (!bme.begin(I2C_ADRESS_BME280, &Wire2)) return false;
    // Forced mode: sensor only measures on request (takeForcedMeasurement) → ~0 µA idle current
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,   // Temperatur
                    Adafruit_BME280::SAMPLING_X1,   // Luftdruck
                    Adafruit_BME280::SAMPLING_X1,   // Luftfeuchte
                    Adafruit_BME280::FILTER_OFF,
                    Adafruit_BME280::STANDBY_MS_0_5);
    return true;
}

void sensorsInit(const PinSet& pins)
{
    gPins = pins;

    for (int i = 0; i < MAX_SENSORS_PER_BUS; i++)
        bus1Temps[i] = DEVICE_DISCONNECTED_C;

    // High-side switch for VCC_1WIRE (Rev 1.3+ only)
    if (gCfg.pcbVersion >= PCB_REV_1_3) {
        pinMode(PIN_1WIRE_HSS, OUTPUT);
        digitalWrite(PIN_1WIRE_HSS, LOW);   // sensors off at startup
        dbgSerial.println("1-Wire HSS (GPIO47) initialized – sensors off.");
    }

    // BME280
    Wire2.begin(PIN_I2C_SDA_BME280, PIN_I2C_SCL_BME280, I2C_FREQUENCY);
    Wire2.setTimeOut(50);
    bme280Present = initBme280();
    if (!bme280Present)
        dbgSerial.println("BME280 not found – check wiring.");

    // 1-Wire (pin configured at runtime). The bus is searched in
    // startOneWireConversion(), once it is powered.
    oneWireBus1 = new OneWire(pins.oneWire);
    sensorsBus1 = new DallasTemperature(oneWireBus1);

    // PCNT counters are configured once and can be toggled later via downlink.
    if (gCfg.windEn) configureWindPcnt();
    if (gCfg.rainEn) configureRainPcnt();

    previousMillis = millis();
}

int adcAverage(int pin, int samples)
{
    int sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += analogRead(pin);
        delayMicroseconds(200);   // ESP32 ADC: ~200 µs settling time between samples
    }
    return sum / samples;
}

float readBatteryVoltage()
{
    constexpr int   ADC_BITS   = 12;
    constexpr float ADC_MAX    = (1 << ADC_BITS) - 1;
    constexpr float ADC_VREF   = 3.3f;
    constexpr int   R_UPPER    = 390;   // R13
    constexpr int   R_LOWER    = 100;   // R14
    // Calibration measurement: 4.095 V measured when 4.2 V was applied
    constexpr float CAL_FACTOR = (ADC_VREF / ADC_MAX)
                                 * ((R_UPPER + R_LOWER) / (float)R_LOWER)
                                 * (4.2f / 4.095f);

    digitalWrite(PIN_ADC_BATCTRL, HIGH);
    delay(100);
    int raw = adcAverage(PIN_ADC_VOLTBATTERY);
    digitalWrite(PIN_ADC_BATCTRL, LOW);
    return CAL_FACTOR * raw;
}

uint16_t readWindDirection()
{
    int raw = adcAverage(PIN_ADC_WINDDIRECTION);
    int rawDeg;

    if (gCfg.windDirSensorType == 1 && gCfg.windDirCalCount > 0) {
        // Calibrated lookup table (e.g. reed-switch/resistor-ladder vanes,
        // which output discrete voltage steps rather than a continuous
        // ramp) — nearest-neighbor match against the calibrated ADC values
        // captured via the portal's "Calibrate Wind Direction" page.
        int bestIdx  = 0;
        int bestDiff = abs(raw - (int)gCfg.windDirCalAdc[0]);
        for (int i = 1; i < gCfg.windDirCalCount; i++) {
            int diff = abs(raw - (int)gCfg.windDirCalAdc[i]);
            if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
        }
        rawDeg = gCfg.windDirCalDeg[bestIdx];
    } else {
        // Potentiometer powered by full VCC3.3, linear 0–4095 → 0–359°.
        rawDeg = map(raw, 0, 4095, 0, 359);
    }

    // windDirOffsetDeg corrects the north alignment during installation,
    // applied on top of either sensing mode above.
    // Double modulo: correct for any offset, including values below -360
    // (the portal/params limit it, but a stale NVS value must not wrap the
    // uint16 result to 65xxx°).
    return (uint16_t)(((rawDeg + gCfg.windDirOffsetDeg) % 360 + 360) % 360);
}

// Pulses seen while the chip was in light sleep (low_power.cpp): the PCNT
// units are not clocked then, so those edges are counted in software.
volatile uint32_t sleepPulsesWind = 0, sleepPulsesRain = 0;

PulseSample readPulseCounters()
{
    int16_t       curWind = 0, curRain = 0;
    unsigned long now     = millis();
    unsigned long elapsed = now - previousMillis;
    previousMillis = now;
    PulseSample out;
    out.elapsedMs = (uint16_t)(elapsed > 65535 ? 65535 : elapsed);

    if (gCfg.windEn) {
        pcnt_get_counter_value(PCNT_UNIT_WINDSPEED, &curWind);
        int32_t pulsesWind = pcntDelta(curWind, pulsesLastCountWindspeed) + (int32_t)sleepPulsesWind;
        pulsesLastCountWindspeed = curWind;
        sleepPulsesWind = 0;
        float pps    = (elapsed > 0) ? (float)pulsesWind / ((float)elapsed / 1000.0f) : 0.0f;
        // gCfg.windMsPerPps: sensor-specific calibration (Davis default: 1.00584,
        // derived from "2.25 mph per Hz"); configurable for other anemometers.
        float newSpeedMs = pps * gCfg.windMsPerPps;
        if (newSpeedMs > MAX_PLAUSIBLE_WIND_SPEED_MS || newSpeedMs < 0.0f || elapsed > 65535) {
            // Reed-switch contact bounce / EMI burst on the pulse line can
            // produce a short, physically impossible pulse rate on a single
            // sub-sample. Drop the sample entirely so neither the means nor
            // the gust ever see it (see MAX_PLAUSIBLE_WIND_SPEED_MS).
            dbgSerial.printf("Wind: %ld pulses → %.1f m/s REJECTED (implausible)\n", (long)pulsesWind, newSpeedMs);
        } else {
            out.windValid  = true;
            out.windPulses = (uint16_t)pulsesWind;
            dbgSerial.printf("Wind: %ld pulses in %lu ms → %.1f m/s\n", (long)pulsesWind, elapsed, newSpeedMs);
        }
    }

    if (gCfg.rainEn) {
        pcnt_get_counter_value(PCNT_UNIT_RAIN, &curRain);
        int32_t pulsesRain = pcntDelta(curRain, pulsesLastCountRain) + (int32_t)sleepPulsesRain;
        pulsesLastCountRain = curRain;
        sleepPulsesRain = 0;
        // gCfg.rainMmPerTip: sensor-specific calibration (Davis default: 0.2mm/tip);
        // configurable for other tipping-bucket rain gauges.
        out.rainMm        = (float)pulsesRain * gCfg.rainMmPerTip;
        srcBoardRain(out.rainMm);   // into rainMMSinceStart only while the board gauge is the rain source
        if (pulsesRain) dbgSerial.printf("Rain: %ld pulses → %.1f mm\n", (long)pulsesRain, out.rainMm);
    }
    // rainMM/rainRateMmH are deliberately NOT written here: they are the
    // finished cycle's values (computeFinalValues() in main.cpp) and are read
    // by the web UI/API at any time - a 3 s sub-sample would show 0 most of
    // the time, or 240 mm/h for a single tip.
    return out;
}

bool readBme280()
{
    // Hot-plug: while absent, re-probe on every sub-sample (a missing device
    // just NACKs its address, so this costs well under a millisecond).
    if (!bme280Present) {
        if (!initBme280()) return false;
        bme280Present = true;
        dbgSerial.println("BME280: detected, re-initialized.");
    }

    // Forced mode: trigger a single measurement, then wait for completion.
    // A failed measurement or NAN (channel skipped — the chip was power-cycled
    // and lost its config) means the sensor is gone or needs a re-init.
    bool ok = bme.takeForcedMeasurement();
    float t = ok ? bme.readTemperature()          : NAN;
    float p = ok ? bme.readPressure() / 100.0f    : NAN;
    float h = ok ? bme.readHumidity()             : NAN;
    if (isnan(t) || isnan(p) || isnan(h)) {
        bme280Present = false;
        dbgSerial.println("BME280: measurement failed – marked absent, will re-probe.");
        return false;
    }
    bme280TempC    = t;
    bme280PressAbs = p;
    bme280PressSea = bme.seaLevelForAltitude(gCfg.altitudeM, bme280PressAbs);
    bme280Humidity = h;

    dbgSerial.printf("BME280: %.1f °C, %.1f %%rH, %.1f hPa (abs), %.1f hPa (QNH)\n",
                  bme280TempC, bme280Humidity, bme280PressAbs, bme280PressSea);
    return true;
}

static bool oneWireHssPowered = false;
static unsigned long oneWireStartedMs = 0;
static constexpr unsigned long ONE_WIRE_TIMEOUT_MS = 1000;

static void powerDownOneWire()
{
    if (!oneWireHssPowered) return;
    digitalWrite(PIN_1WIRE_HSS, LOW);
    pinMode(gPins.oneWire, INPUT);
    oneWireHssPowered = false;
}

void startOneWireConversion()
{
    // Rev 1.3+: power up VCC_1WIRE now; switched back off once the
    // conversion completes, in pollOneWireConversion().
    oneWireHssPowered = (gCfg.pcbVersion >= PCB_REV_1_3);
    if (oneWireHssPowered) {
        digitalWrite(PIN_1WIRE_HSS, HIGH);
        delay(5);   // DS18B20 needs ≥1 ms after VDD power-on before first reset
    }

    // Search the bus every cycle, after VCC_1WIRE is up: getDeviceCount()
    // only returns the count from the last begin(), and on Rev 1.3 the bus is
    // unpowered at boot (a search there finds nothing). This also makes the
    // probes genuinely hot-pluggable on both revisions.
    sensorsBus1->begin();
    bus1Count = min((int)sensorsBus1->getDeviceCount(), MAX_SENSORS_PER_BUS);
    for (int i = bus1Count; i < MAX_SENSORS_PER_BUS; i++)
        bus1Temps[i] = DEVICE_DISCONNECTED_C;
    if (bus1Count > 0) {
        sensorsBus1->setWaitForConversion(false);   // non-blocking: request returns immediately
        sensorsBus1->requestTemperatures();
        oneWireStartedMs = millis();
    } else {
        dbgSerial.println("1-Wire: no sensor found");
        powerDownOneWire();
    }
}

bool pollOneWireConversion()
{
    if (bus1Count == 0) return true;   // nothing was requested, see startOneWireConversion()
    if (!sensorsBus1->isConversionComplete()) {
        if (millis() - oneWireStartedMs < ONE_WIRE_TIMEOUT_MS) return false;
        dbgSerial.println("1-Wire: conversion timed out after 1000 ms; readings unavailable, retry next cycle.");
        for (int i = 0; i < bus1Count; i++)
            bus1Temps[i] = DEVICE_DISCONNECTED_C;
        powerDownOneWire();
        return true;
    }

    for (int i = 0; i < bus1Count; i++) {
        bus1Temps[i] = sensorsBus1->getTempCByIndex(i);
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C)
            dbgSerial.printf("1-Wire sensor %d: %.1f °C\n", i, bus1Temps[i]);
    }

    powerDownOneWire();
    return true;
}
