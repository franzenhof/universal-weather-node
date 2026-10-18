#include "sensors.h"
#include "globals.h"
#include "serial_log.h"

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
// after 10 failed uplinks) or watchdog reset. RTC_NOINIT survives those; it
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

namespace {

TwoWire            Wire2(1);
Adafruit_BME280    bme;
OneWire*           oneWireBus1 = nullptr;
DallasTemperature* sensorsBus1 = nullptr;

PinSet gPins = { .windSpeed = 3, .rain = 4, .oneWire = 5 };   // set by sensorsInit()

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
    if (pcbVersion == PCB_REV_1_3) return { .windSpeed = 6, .rain = 7, .oneWire = 5 };
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
    bme280Present = bme.begin(I2C_ADRESS_BME280, &Wire2);
    if (bme280Present) {
        // Forced mode: sensor only measures on request (takeForcedMeasurement) → ~0 µA idle current
        bme.setSampling(Adafruit_BME280::MODE_FORCED,
                        Adafruit_BME280::SAMPLING_X1,   // Temperatur
                        Adafruit_BME280::SAMPLING_X1,   // Luftdruck
                        Adafruit_BME280::SAMPLING_X1,   // Luftfeuchte
                        Adafruit_BME280::FILTER_OFF,
                        Adafruit_BME280::STANDBY_MS_0_5);
    } else {
        dbgSerial.println("BME280 not found – check wiring.");
    }

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

float readPulseCounters()
{
    int16_t       curWind = 0, curRain = 0;
    unsigned long now     = millis();
    unsigned long elapsed = now - previousMillis;
    previousMillis = now;
    float subSampleRainMm = 0.0f;

    if (gCfg.windEn) {
        pcnt_get_counter_value(PCNT_UNIT_WINDSPEED, &curWind);
        int32_t pulsesWind = pcntDelta(curWind, pulsesLastCountWindspeed);
        pulsesLastCountWindspeed = curWind;
        float pps    = (elapsed > 0) ? (float)pulsesWind / ((float)elapsed / 1000.0f) : 0.0f;
        // gCfg.windMsPerPps: sensor-specific calibration (Davis default: 1.00584,
        // derived from "2.25 mph per Hz"); configurable for other anemometers.
        float newSpeedMs = pps * gCfg.windMsPerPps;
        if (newSpeedMs > MAX_PLAUSIBLE_WIND_SPEED_MS || newSpeedMs < 0.0f) {
            // Reed-switch contact bounce / EMI burst on the pulse line can
            // produce a short, physically impossible pulse rate on a single
            // sub-sample. Drop the sample entirely so neither windSpeedMs nor
            // windGustMs ever sees it (see MAX_PLAUSIBLE_WIND_SPEED_MS).
            dbgSerial.printf("Wind: %ld pulses → %.1f m/s REJECTED (implausible, gust stays %.1f)\n",
                          (long)pulsesWind, newSpeedMs, windGustMs);
        } else {
            windSpeedMs = newSpeedMs;
            if (windSpeedMs > windGustMs) windGustMs = windSpeedMs;
            dbgSerial.printf("Wind: %ld pulses → %.1f m/s (gust %.1f)\n", (long)pulsesWind, windSpeedMs, windGustMs);
        }
    }

    if (gCfg.rainEn) {
        pcnt_get_counter_value(PCNT_UNIT_RAIN, &curRain);
        int32_t pulsesRain = pcntDelta(curRain, pulsesLastCountRain);
        pulsesLastCountRain = curRain;
        // gCfg.rainMmPerTip: sensor-specific calibration (Davis default: 0.2mm/tip);
        // configurable for other tipping-bucket rain gauges.
        subSampleRainMm  = (float)pulsesRain * gCfg.rainMmPerTip;
        rainMMSinceStart += subSampleRainMm;
        dbgSerial.printf("Rain: %ld pulses → %.1f mm\n", (long)pulsesRain, subSampleRainMm);
    }
    // rainMM/rainRateMmH are deliberately NOT written here: they are the
    // finished cycle's values (computeFinalValues() in main.cpp) and are read
    // by the web UI/API at any time - a 3 s sub-sample would show 0 most of
    // the time, or 240 mm/h for a single tip.
    return subSampleRainMm;
}

void readBme280()
{
    // Forced mode: trigger a single measurement, then wait for completion
    if (!bme.takeForcedMeasurement()) {
        dbgSerial.println("BME280: forced measurement failed.");
        return;
    }
    bme280TempC    = bme.readTemperature();
    bme280PressAbs = bme.readPressure() / 100.0f;
    bme280PressSea = bme.seaLevelForAltitude(gCfg.altitudeM, bme280PressAbs);
    bme280Humidity = bme.readHumidity();

    dbgSerial.printf("BME280: %.1f °C, %.1f %%rH, %.1f hPa (abs), %.1f hPa (QNH)\n",
                  bme280TempC, bme280Humidity, bme280PressAbs, bme280PressSea);
}

static bool oneWireHssPowered = false;

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
    if (bus1Count > 0) {
        sensorsBus1->setWaitForConversion(false);   // non-blocking: request returns immediately
        sensorsBus1->requestTemperatures();
    } else {
        dbgSerial.println("1-Wire: no sensor found");
        if (oneWireHssPowered) {
            digitalWrite(PIN_1WIRE_HSS, LOW);
            pinMode(gPins.oneWire, INPUT);
        }
    }
}

bool pollOneWireConversion()
{
    if (bus1Count == 0) return true;   // nothing was requested, see startOneWireConversion()
    if (!sensorsBus1->isConversionComplete()) return false;

    for (int i = 0; i < bus1Count; i++) {
        bus1Temps[i] = sensorsBus1->getTempCByIndex(i);
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C)
            dbgSerial.printf("1-Wire sensor %d: %.1f °C\n", i, bus1Temps[i]);
    }

    if (oneWireHssPowered) {
        digitalWrite(PIN_1WIRE_HSS, LOW);
        // DQ line high-impedance → pull-up to VCC_1WIRE=0V, no parasitic power
        pinMode(gPins.oneWire, INPUT);
    }
    return true;
}
