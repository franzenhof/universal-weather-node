#include "sensors.h"
#include "heltec_board.h"
#include "globals.h"
#include "serial_log.h"
#include "sources.h"

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

bool     windCycleValid = false;
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

// Rain taken away by resetRainAccumulator() since boot (the windows restart
// with the boot anyway).
static float rainResetMm = 0.0f;

float rainWindowTotalMm() { return rainMMSinceStart + rainResetMm; }

void resetRainAccumulator()
{
    rainResetMm     += rainMMSinceStart;
    rainMMSinceStart = 0.0f;
}

PinSet gPins = { .windSpeed = 3, .rain = 4, .oneWire = 5 };   // set by sensorsInit()

namespace {

TwoWire            Wire2(1);
Adafruit_BME280    bme;
OneWire*           oneWireBus1 = nullptr;
DallasTemperature* sensorsBus1 = nullptr;

bool windPcntConfigured = false;
bool rainPcntConfigured = false;
uint32_t pulsesLastCountWindspeed = 0;
uint32_t pulsesLastCountRain      = 0;
unsigned long previousMillis     = 0;

} // namespace

// Pulses since the previous read (the driver accumulates the counter's
// overflows, pulse_counter.h).
static int32_t pcntPulses(PulseCounterId id, uint32_t& last)
{
    const uint32_t cur = pulseCounterRead(id);
    const uint32_t d = cur - last;
    last = cur;
    return (int32_t)d;
}

PinSet getPins()
{
    // V4 on a PCB (V4 Full / V4 Light): wind 44, rain 16, 1-Wire 43; without PCB none
    const BoardPins b = boardPins();
    return { .windSpeed = b.windSpeed, .rain = b.rain, .oneWire = b.oneWire };
}

int windDirPin() { return boardPins().windDir; }

// V4 Full / V4 Light: an inverting Schmitt trigger (74LVC2G14 / 74HC14) sits between the RC
// filter and the GPIO, so what was the rising edge at the sensor is the
// falling edge here — count negative edges, one count per physical pulse.
static bool pulseInputsInverted() { return true; }

void configureWindPcnt()
{
    windPcntConfigured |= pulseCounterStart(PCNT_UNIT_WINDSPEED, gPins.windSpeed, pulseInputsInverted());
    pulsesLastCountWindspeed = 0;
}

void configureRainPcnt()
{
    rainPcntConfigured |= pulseCounterStart(PCNT_UNIT_RAIN, gPins.rain, pulseInputsInverted());
    pulsesLastCountRain = 0;
}

void stopWindPcnt()
{
    if (!windPcntConfigured) return;
    pulseCounterStop(PCNT_UNIT_WINDSPEED);
    pulsesLastCountWindspeed = 0;
}

void stopRainPcnt()
{
    if (!rainPcntConfigured) return;
    pulseCounterStop(PCNT_UNIT_RAIN);
    pulsesLastCountRain = 0;
}

void sensorsEarlyInit()
{
    restoreRainAccumulator();
    pinMode(PIN_ADC_BATCTRL,       OUTPUT);
    pinMode(PIN_ADC_VOLTBATTERY,   INPUT);   // analogRead() attaches the ADC channel
    // GPIO2 is the RF front end's chip enable on a Heltec V4 (heltec_board.h)
    if (boardPcbAllowed()) {
        pinMode(windDirPin(), INPUT);
    }
    analogReadResolution(12);
}

// Probe + configure the BME280 (0x76, else 0x77; begin() checks chip ID
// 0x60). begin() re-reads chip ID and calibration and soft-resets the chip,
// so this is also the re-init path after the sensor was unplugged/replugged
// (a power-cycled BME280 comes up with ctrl_hum = skip, which setSampling()
// restores).
static uint8_t bmeAddr = I2C_ADRESS_BME280;
static BmePins bmePins = { BME_WIRELESS_SDA, BME_WIRELESS_SCL, false };
static bool    wireStarted = false;

static bool initBme280()
{
    const uint8_t other = bmeAddr == 0x76 ? 0x77 : 0x76;
    if (!bme.begin(bmeAddr, &Wire2)) {
        if (!bme.begin(other, &Wire2)) return false;
        bmeAddr = other;
    }
    // Forced mode: sensor only measures on request (takeForcedMeasurement) → ~0 µA idle current
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,   // Temperatur
                    Adafruit_BME280::SAMPLING_X1,   // Luftdruck
                    Adafruit_BME280::SAMPLING_X1,   // Luftfeuchte
                    Adafruit_BME280::FILTER_OFF,
                    Adafruit_BME280::STANDBY_MS_0_5);
    return true;
}

// The BME280's own I2C bus (the S3's second controller) on a pin pair.
static void bmeBus(int sda, int scl)
{
    if (wireStarted) Wire2.end();
    Wire2.begin(sda, scl, I2C_FREQUENCY);
    Wire2.setTimeOut(50);
    wireStarted = true;
}

void sensorsBmePins(int& sda, int& scl, bool& oldWiring, uint8_t& addr)
{
    if (!wireStarted) {   // before sensorsInit(): where it will be looked for
        const bool pcb = boardPcbAllowed();
        bmePins = bmePinSelect(pcb, gnssActiveFor(pcb, gCfg.gnssEn), false, false);
    }
    sda = bmePins.sda;
    scl = bmePins.scl;
    oldWiring = bmePins.oldWiring;
    addr = bmeAddr;
}

void sensorsInit(const PinSet& pins)
{
    gPins = pins;

    for (int i = 0; i < MAX_SENSORS_PER_BUS; i++)
        bus1Temps[i] = DEVICE_DISCONNECTED_C;

    // High-side switch for VCC_1WIRE (Rev 1.3+ only)
    if (boardPcbAllowed()) {
        pinMode(PIN_1WIRE_HSS, OUTPUT);
        digitalWrite(PIN_1WIRE_HSS, LOW);   // sensors off at startup
        dbgSerial.println("1-Wire HSS (GPIO47) initialized – sensors off.");
    }

    // BME280 (heltec_board.h bmePinSelect): PCB 41/42; without PCB 47/48,
    // the old wiring on 41/42 only while GNSS is off
    {
        const bool pcb = boardPcbAllowed();
        const bool gnssOn = gnssActiveFor(pcb, gCfg.gnssEn);
        bool found4748 = false, found4142 = false;
        if (pcb) {
            bmeBus(BME_PCB_SDA, BME_PCB_SCL);
            found4142 = initBme280();
        } else {
            bmeBus(BME_WIRELESS_SDA, BME_WIRELESS_SCL);
            found4748 = initBme280();
            if (bmeProbeOldPins(pcb, gnssOn, found4748)) {
                bmeBus(BME_PCB_SDA, BME_PCB_SCL);
                found4142 = initBme280();
            }
        }
        const BmePins sel = bmePinSelect(pcb, gnssOn, found4748, found4142);
        if (!found4748 && !found4142 && !pcb) bmeBus(sel.sda, sel.scl);   // none: wait for it on 47/48 (hot-plug)
        bmePins = sel;
        bme280Present = found4748 || found4142;
        if (bme280Present)
            dbgSerial.printf("BME280: found at 0x%02X on GPIO%d/%d (SDA/SCL)\n", bmeAddr, sel.sda, sel.scl);
        else
            dbgSerial.printf("BME280 not found on GPIO%d/%d (SDA/SCL) - check wiring.\n", sel.sda, sel.scl);
        if (sel.oldWiring)
            dbgSerial.println("BME280 on the old pins 41/42 - works; move it to GPIO47/48 for a GNSS module.");
    }

    // 1-Wire (pin configured at runtime). The bus is searched in
    // startOneWireConversion(), once it is powered.
    // Not on a Heltec V4: GPIO5 is the KCT8103L's CTX line (V4.3).
    if (boardPcbAllowed()) {
        oneWireBus1 = new OneWire(pins.oneWire);
        sensorsBus1 = new DallasTemperature(oneWireBus1);
    }

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
    if (!boardPcbAllowed()) return 0;   // Heltec V4: GPIO2 = FEM enable, never an ADC
    int raw = adcAverage(windDirPin());
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

static unsigned long settleFromMs = 0;
static bool          settleStarted = false, settled = false;
void pulseSettleStart() { settleFromMs = millis(); settleStarted = true; }
bool pulseSettling()
{
    if (settled) return false;
    if (!settleStarted || millis() - settleFromMs < PULSE_SETTLE_MS) return true;
    settled = true;
    return false;
}

// A single sub-sample far above the last minute's wind waits for the next
// one to confirm it (wind_window.h); its pulses are kept meanwhile.
static WindSpikeGuard windGuard;
static uint32_t heldWindPulses = 0;
static unsigned long heldWindMs = 0;

PulseSample readPulseCounters()
{
    unsigned long now     = millis();
    unsigned long elapsed = now - previousMillis;
    previousMillis = now;
    PulseSample out;
    out.elapsedMs = (uint16_t)(elapsed > 65535 ? 65535 : elapsed);

    if (gCfg.windEn) {
        int32_t pulsesWind = pcntPulses(PCNT_UNIT_WINDSPEED, pulsesLastCountWindspeed)
                           + (int32_t)sleepPulsesWind;
        sleepPulsesWind = 0;
        float pps    = (elapsed > 0) ? (float)pulsesWind / ((float)elapsed / 1000.0f) : 0.0f;
        // gCfg.windMsPerPps: sensor-specific calibration (Davis default: 1.00584,
        // derived from "2.25 mph per Hz"); configurable for other anemometers.
        float newSpeedMs = pps * gCfg.windMsPerPps;
        if (pulseSettling()) {
            if (pulsesWind) dbgSerial.printf("Wind: %ld pulses ignored (inputs settling after the start)\n", (long)pulsesWind);
        } else if (newSpeedMs > MAX_PLAUSIBLE_WIND_SPEED_MS || newSpeedMs < 0.0f || elapsed > 65535) {
            // Reed-switch contact bounce / EMI burst on the pulse line can
            // produce a short, physically impossible pulse rate on a single
            // sub-sample. Drop the sample entirely so neither the means nor
            // the gust ever see it (see MAX_PLAUSIBLE_WIND_SPEED_MS).
            dbgSerial.printf("Wind: %ld pulses → %.1f m/s REJECTED (implausible)\n", (long)pulsesWind, newSpeedMs);
        } else {
            const SpikeVerdict v = WIND_SPIKE_GUARD_ON ? windGuard.check(newSpeedMs, now) : SPIKE_TAKE;
            if (v == SPIKE_HOLD) {
                heldWindPulses = (uint32_t)pulsesWind; heldWindMs = elapsed;
                dbgSerial.printf("Wind: %ld pulses → %.1f m/s held (far above the last minute, waits for the next sample)\n", (long)pulsesWind, newSpeedMs);
            } else {
                if (v == SPIKE_TAKE_WITH_HELD) {   // confirmed: both samples count, as one
                    pulsesWind += (int32_t)heldWindPulses;
                    elapsed    += heldWindMs;
                    out.elapsedMs = (uint16_t)(elapsed > 65535 ? 65535 : elapsed);
                } else if (heldWindMs) {
                    dbgSerial.println("Wind: held sample not confirmed - dropped");
                }
                heldWindPulses = 0; heldWindMs = 0;
                out.windValid  = true;
                out.windPulses = (uint32_t)pulsesWind;
                dbgSerial.printf("Wind: %ld pulses in %lu ms → %.1f m/s\n", (long)pulsesWind, elapsed, newSpeedMs);
            }
        }
    }

    if (gCfg.rainEn) {
        int32_t pulsesRain = pcntPulses(PCNT_UNIT_RAIN, pulsesLastCountRain)
                           + (int32_t)sleepPulsesRain;
        sleepPulsesRain = 0;
        // gCfg.rainMmPerTip: sensor-specific calibration (Davis default: 0.2mm/tip);
        // configurable for other tipping-bucket rain gauges.
        if (pulseSettling()) {
            if (pulsesRain) dbgSerial.printf("Rain: %ld pulses ignored (inputs settling after the start)\n", (long)pulsesRain);
            pulsesRain = 0;
        }
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
    // Outside the sensor's operating range (datasheet: -40..85 °C, 300..1100
    // hPa, 0..100 %): a corrupted read (I2C glitch, lost calibration after a
    // brown-out) - handled like a failed one, re-init on the next sample.
    if (!(t >= -40.0f && t <= 85.0f) || !(p >= 300.0f && p <= 1100.0f) || !(h >= 0.0f && h <= 100.0f)) {
        bme280Present = false;
        dbgSerial.printf("BME280: implausible reading (%.1f °C, %.1f hPa, %.1f %%) – dropped, will re-probe.\n", t, p, h);
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
    if (!sensorsBus1) { bus1Count = 0; return; }   // Heltec V4: no 1-Wire bus
    oneWireHssPowered = boardPcbAllowed();   // V4 Full / V4 Light: switched 1-Wire supply
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
