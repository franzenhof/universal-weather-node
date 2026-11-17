#include "board_io.h"
#include "globals.h"
#include "serial_log.h"
#include "sensors.h"
#include "uv_calibration.h"
#include "wind_window.h"

#include "driver/pcnt.h"
#include <driver/gpio.h>
#include <DallasTemperature.h>   // DEVICE_DISCONNECTED_C
#include <soc/usb_serial_jtag_reg.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

IoState gIo;

// Kept across ESP.restart()/watchdog resets like the rain accumulator (see
// restoreRainAccumulator() in sensors.cpp), reset on power-on.
RTC_NOINIT_ATTR uint32_t auxPulsesTotalKept;
RTC_NOINIT_ATTR uint32_t auxPulsesTotalMagic;
static constexpr uint32_t AUX_TOTAL_MAGIC = 0x41555850;   // "AUXP"

namespace {

// v2.0: 1M / 1M from 3V3 -> ~1650 mV. Rev 1.2/1.3: GPIO20 not connected.
constexpr uint32_t HW_ID_V2_MIN_MV = 1300;
constexpr uint32_t HW_ID_V2_MAX_MV = 2000;

constexpr int     LEDC_CH_FAN    = 7;      // channels 0-5 left to the Heltec library/display
constexpr int     FAN_PWM_FREQ_HZ = 100;   // the fan's high-side P-FET has a resistive gate pull-up
constexpr int     PWM_BITS       = 8;
constexpr int64_t HEATER_PERIOD_US = 10000000;
constexpr uint64_t HEATER_TICK_US = HEATER_PERIOD_US / 100;
constexpr int16_t PCNT_H_LIM     = 32767;
constexpr unsigned long VE_TIMEOUT_MS = 10000;
constexpr float   HYSTERESIS_C   = 1.0f;
constexpr float   BATT_HYST_V    = 0.2f;

VeDirectParser veParser;
bool          veRunning = false;
unsigned long veLastBlockMs = 0;

bool    auxPulseRunning = false, tachoRunning = false;
int16_t auxPulseLast = 0, tachoLast = 0;
uint32_t auxPulsesAcc = 0, tachoPulsesAcc = 0;

// 2nd anemometer on AUX (wind2Mode): pulses since the last sub-sample, and
// the cycle accumulators (same scheme as the main wind sensor, wind_window.h):
// pulse-weighted mean (sum of pulses / sum of time), highest 3-s gust.
uint32_t wind2Pulses = 0;
unsigned long wind2LastMs = 0;
uint32_t wind2PulseSum = 0, wind2MsSum = 0;
float    wind2Gust = 0.0f, wind2SinSum = 0.0f, wind2CosSum = 0.0f;
int      wind2Samples = 0;
WindGust wind2GustWin;
unsigned long tachoSinceMs = 0;

float auxAinMvSum = 0.0f;
int   auxAinSamples = 0;
float uvAinMvSum = 0.0f;
int   uvAinSamples = 0;
bool  sampled = false;            // first sub-sample taken (sensor values are real)
bool  fanFullWholeCycle = true;   // fan at 100 % in every sub-sample of this cycle
int   cycleSamples = 0;

bool fanPwmAttached = false;
esp_timer_handle_t heaterTimer = nullptr;
bool heaterTimerRunning = false;
portMUX_TYPE heaterMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t heaterOutputDuty = 0;
int64_t heaterPeriodStartUs = 0;
bool heaterWant = false, fanWant = false;   // auto-mode state (with hysteresis)
bool outputsSuspended = false;

int32_t pcntDelta(int16_t cur, int16_t last)
{
    return (cur >= last) ? (int32_t)cur - last : (int32_t)(PCNT_H_LIM - last) + cur;
}

// Both inputs sit behind an inverting Schmitt trigger: the falling edge at
// the GPIO is the rising edge at the terminal - one count per pulse.
void startPcnt(pcnt_unit_t unit, int gpio)
{
    pcnt_config_t c = {
        .pulse_gpio_num = gpio,
        .ctrl_gpio_num  = PCNT_PIN_NOT_USED,
        .pos_mode       = PCNT_COUNT_DIS,
        .neg_mode       = PCNT_COUNT_INC,
        .counter_h_lim  = PCNT_H_LIM,
        .counter_l_lim  = -32768,
        .unit           = unit,
        .channel        = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&c);
    pcnt_set_filter_value(unit, 100);   // ~1.25 µs glitch filter, the RC filter on the board does the rest
    pcnt_filter_enable(unit);
    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    pcnt_counter_resume(unit);
}

void stopPcnt(pcnt_unit_t unit)
{
    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
}

uint32_t readPcnt(pcnt_unit_t unit, int16_t& last)
{
    int16_t cur = 0;
    pcnt_get_counter_value(unit, &cur);
    int32_t d = pcntDelta(cur, last);
    last = cur;
    return d > 0 ? (uint32_t)d : 0;
}

// 0 % / 100 % as plain GPIO levels (no PWM edges at all), PWM in between.
void driveOutput(int pin, int ch, int pct, bool& attached)
{
    if (pct <= 0 || pct >= 100) {
        if (attached) { ledcDetachPin(pin); attached = false; }
        pinMode(pin, OUTPUT);
        digitalWrite(pin, pct >= 100 ? HIGH : LOW);
    } else {
        if (!attached) {
            ledcSetup(ch, FAN_PWM_FREQ_HZ, PWM_BITS);
            ledcAttachPin(pin, ch);
            attached = true;
        }
        ledcWrite(ch, (uint32_t)pct * ((1 << PWM_BITS) - 1) / 100);
    }
}

// A timer task keeps the heating window running during blocking sensor/LoRa calls.
void heaterTick(void*)
{
    portENTER_CRITICAL(&heaterMux);
    int64_t phase = (esp_timer_get_time() - heaterPeriodStartUs) % HEATER_PERIOD_US;
    digitalWrite(PIN_HEATER, phase < HEATER_PERIOD_US * heaterOutputDuty / 100 ? HIGH : LOW);
    portEXIT_CRITICAL(&heaterMux);
}

void driveHeater(uint8_t pct)
{
    portENTER_CRITICAL(&heaterMux);
    if (heaterOutputDuty == 0) heaterPeriodStartUs = esp_timer_get_time();
    heaterOutputDuty = pct;
    int64_t phase = (esp_timer_get_time() - heaterPeriodStartUs) % HEATER_PERIOD_US;
    digitalWrite(PIN_HEATER, phase < HEATER_PERIOD_US * pct / 100 ? HIGH : LOW);
    portEXIT_CRITICAL(&heaterMux);

    bool needsTimer = pct > 0 && pct < 100;
    if (heaterTimerRunning && !needsTimer) {
        ESP_ERROR_CHECK(esp_timer_stop(heaterTimer));
        heaterTimerRunning = false;
    } else if (!heaterTimerRunning && needsTimer) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(heaterTimer, HEATER_TICK_US));
        heaterTimerRunning = true;
    }
}

// Air temperature for the auto modes: BME280, else the first DS18B20.
float controlTempC()
{
    if (!sampled) return NAN;   // before the first reading bme280TempC is still 0.0
    if (bme280Present) return bme280TempC;
    if (bus1Count > 0 && bus1Temps[0] != DEVICE_DISCONNECTED_C) return bus1Temps[0];
    return NAN;
}

// Auto mode: heaterMinDutyPct at the switch-on threshold (and in the 1 K
// hysteresis band above it), rising linearly to maxDuty at
// heaterFullBelowDc and colder.
int heaterAutoDuty(float t, float onBelow, int maxDuty)
{
    int   minDuty = constrain((int)gCfg.heaterMinDutyPct, 1, maxDuty);
    float full    = gCfg.heaterFullBelowDc / 10.0f;
    if (isnan(t) || full >= onBelow || t <= full) return maxDuty;
    if (t >= onBelow) return minDuty;
    float frac = (onBelow - t) / (onBelow - full);
    return constrain((int)lroundf(minDuty + frac * (maxDuty - minDuty)), minDuty, maxDuty);
}

void updateOutputs()
{
    // A configured limit is binding even if VE.Direct is disabled.
    float minV = gCfg.outMinBattDv / 10.0f;
    bool wasLocked = gIo.battLockout;
    if (gCfg.outMinBattDv <= 0)
        gIo.battLockout = false;
    else if (!gCfg.veDirectEn || !gIo.veValid || !isfinite(gIo.ve.battV))
        gIo.battLockout = true;
    else
        gIo.battLockout = gIo.battLockout ? gIo.ve.battV < minV + BATT_HYST_V : gIo.ve.battV < minV;
    if (wasLocked != gIo.battLockout)
        dbgSerial.printf("Output battery lockout: %s (limit %.1f V; VE.Direct %s).\n",
                         gIo.battLockout ? "active" : "cleared", minV,
                         gCfg.veDirectEn && gIo.veValid && isfinite(gIo.ve.battV) ? "valid" : "unavailable");

    float t = controlTempC();

    // Heater auto: on below the threshold, off 1 K above it.
    float below = gCfg.heaterOnBelowDc / 10.0f;
    if (isnan(t))                          heaterWant = false;
    else if (t < below)                    heaterWant = true;
    else if (t >= below + HYSTERESIS_C)    heaterWant = false;

    // Fan auto: on from the threshold, off 1 K below it; optionally only
    // while the PV array delivers enough power (sunshine = radiation error).
    float above = gCfg.fanOnAboveDc / 10.0f;
    if (isnan(t))                          fanWant = false;
    else if (t >= above)                   fanWant = true;
    else if (t < above - HYSTERESIS_C)     fanWant = false;
    bool pvOk = gCfg.fanMinPvW <= 0 ||
                (gCfg.veDirectEn && gIo.veValid && isfinite(gIo.ve.pvW) && gIo.ve.pvW >= gCfg.fanMinPvW);
    static bool pvBlocked = false;
    bool nowPvBlocked = gCfg.fanMode == 2 && fanWant && !pvOk;
    if (pvBlocked != nowPvBlocked)
        dbgSerial.printf("Fan PV interlock: %s (limit %d W).\n",
                         nowPvBlocked ? "blocked: low or unavailable PV power" : "cleared", (int)gCfg.fanMinPvW);
    pvBlocked = nowPvBlocked;

    bool heaterOn = !outputsSuspended && !gIo.battLockout && (gCfg.heaterMode == 1 || (gCfg.heaterMode == 2 && heaterWant));
    bool fanOn    = !outputsSuspended && !gIo.battLockout && (gCfg.fanMode == 1    || (gCfg.fanMode == 2 && fanWant && pvOk));
    uint8_t heaterDuty = 0;
    if (heaterOn) {
        int maxDuty = constrain((int)gCfg.heaterDutyPct, 1, 100);
        heaterDuty = (uint8_t)(gCfg.heaterMode == 2 ? heaterAutoDuty(t, below, maxDuty) : maxDuty);
    }
    uint8_t fanDuty    = fanOn    ? (uint8_t)constrain((int)gCfg.fanDutyPct,    1, 100) : 0;

    if (heaterDuty != gIo.heaterDuty) {
        driveHeater(heaterDuty);
        dbgSerial.printf("Heater: %u %%\n", heaterDuty);
    }
    if (fanDuty != gIo.fanDuty) {
        driveOutput(PIN_FAN, LEDC_CH_FAN, fanDuty, fanPwmAttached);
        dbgSerial.printf("Fan: %u %%\n", fanDuty);
    }
    gIo.heaterOn = heaterOn;  gIo.heaterDuty = heaterDuty;
    gIo.fanOn    = fanOn;     gIo.fanDuty    = fanDuty;
}

} // namespace

bool detectPcbV2()
{
    // GPIO20 is USB D+ of the internal USB-Serial-JTAG PHY, which has its own
    // D+ pull-up. The Heltec V3 uses the CP2102 for USB, so detach the PHY
    // from the pads before measuring.
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    // Discharge first: on Rev 1.x the pin floats and would read whatever
    // charge is left on it. On v2.0 the divider recharges C10 (1M || 1M x
    // 100 nF = 50 ms) - 300 ms is 6 time constants.
    pinMode(PIN_HW_ID, OUTPUT);
    digitalWrite(PIN_HW_ID, LOW);
    delay(5);
    pinMode(PIN_HW_ID, INPUT);
    delay(300);

    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_HW_ID);
    mv /= 8;

    bool v2 = (mv >= HW_ID_V2_MIN_MV && mv <= HW_ID_V2_MAX_MV);
    dbgSerial.printf("HW revision ID (GPIO20): %u mV -> %s\n", (unsigned)mv, v2 ? "PCB v2.0" : "no ID (Rev 1.x)");
    if (!v2 && mv > 300)
        dbgSerial.println("HW revision ID: unknown voltage - treated as Rev 1.x");
    gIo.hwIdDetected = v2;
    return v2;
}

bool ioUvActive()       { return gCfg.uvEn; }
bool ioAuxAinActive()   { return gCfg.auxAinEn && gCfg.wind2Mode != 2; }
bool ioWind2DirActive() { return gCfg.wind2Mode == 2; }

bool ioV2()
{
    return gCfg.pcbVersion >= PCB_REV_2_0;
}

static constexpr int PIN_WDIR_EN = 19;  // PCB v2.0 Q6 gate, LOW = vane supply on

void windVaneSupply(bool on)
{
    if (!ioV2()) return;
    // Pad hold: the level must survive the light sleep in lowPowerSleep()
    // (the vane filter settles while the chip sleeps).
    gpio_hold_dis((gpio_num_t)PIN_WDIR_EN);
    pinMode(PIN_WDIR_EN, OUTPUT);
    digitalWrite(PIN_WDIR_EN, on ? LOW : HIGH);
    gpio_hold_en((gpio_num_t)PIN_WDIR_EN);
}

void ioInit()
{
    if (!ioV2()) return;

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT || auxPulsesTotalMagic != AUX_TOTAL_MAGIC) {
        auxPulsesTotalKept  = 0;
        auxPulsesTotalMagic = AUX_TOTAL_MAGIC;
    }
    gIo.auxPulsesTotal = auxPulsesTotalKept;
    wind2LastMs = millis();

    // Outputs off until updateOutputs() decides otherwise (the gate
    // pull-downs on the board already hold them off during boot).
    pinMode(PIN_HEATER, OUTPUT);  digitalWrite(PIN_HEATER, LOW);
    pinMode(PIN_FAN,    OUTPUT);  digitalWrite(PIN_FAN,    LOW);
    if (!heaterTimer) {
        esp_timer_create_args_t timer = {};
        timer.callback = heaterTick;
        timer.dispatch_method = ESP_TIMER_TASK;
        timer.name = "heater";
        timer.skip_unhandled_events = true;
        ESP_ERROR_CHECK(esp_timer_create(&timer, &heaterTimer));
    }
    // GPIO3 is an ADC input on this revision and must never be driven, even
    // when UV measurement is disabled.
    pinMode(PIN_UV_AIN, INPUT);
    adcAttachPin(PIN_UV_AIN);
    // Push-pull Schmitt-trigger outputs and an RC-filtered ADC input: no pulls.
    pinMode(PIN_AUX_PULSE, INPUT);
    pinMode(PIN_TACHO,     INPUT);
    pinMode(PIN_AUX_AIN,   INPUT);
    windVaneSupply(true);
    tachoSinceMs = millis();

    ioPoll();
    dbgSerial.printf("PCB v2.0 I/O: UV=%d AIN=%d PULSE=%d heater=%d fan=%d tacho=%d VE.Direct=%d\n",
                     (int)gCfg.uvEn,
                     (int)gCfg.auxAinEn, (int)gCfg.auxPulseEn, (int)gCfg.heaterMode,
                     (int)gCfg.fanMode, (int)gCfg.tachoEn, (int)gCfg.veDirectEn);
}

void ioSuspendOutputs()
{
    if (!ioV2()) return;
    outputsSuspended = true;
    driveHeater(0);
    driveOutput(PIN_FAN, LEDC_CH_FAN, 0, fanPwmAttached);
    gIo.heaterOn = gIo.fanOn = false;
    gIo.heaterDuty = gIo.fanDuty = 0;
    dbgSerial.println("Heater/fan suspended until restart (config/OTA).");
}

void ioPoll()
{
    if (!ioV2()) return;

    // ---- VE.Direct ----
    if (gCfg.veDirectEn && !veRunning) {
        veParser.reset();
        Serial1.setRxBufferSize(1024);   // a blocking LoRa send takes a few seconds (1920 B/s)
        Serial1.begin(19200, SERIAL_8N1, PIN_VE_RX, PIN_VE_TX);
        veRunning = true;
        veLastBlockMs = 0;
        dbgSerial.println("VE.Direct: started (GPIO33 RX, 19200 Bd).");
    } else if (!gCfg.veDirectEn && veRunning) {
        Serial1.end();
        veRunning = false;
        gIo.ve = VeDirectValues{ NAN, NAN, NAN, NAN, NAN, NAN, NAN, -1, -1 };
        dbgSerial.println("VE.Direct: stopped.");
    }
    if (veRunning) {
        while (Serial1.available()) {
            if (veParser.feed((uint8_t)Serial1.read())) {
                veLastBlockMs = millis();
                if (veLastBlockMs == 0) veLastBlockMs = 1;
                gIo.ve = veParser.values();
            }
        }
    }
    gIo.veValid = veRunning && veLastBlockMs != 0 && millis() - veLastBlockMs < VE_TIMEOUT_MS;

    // ---- pulse counters (read often: the 16-bit counter wraps within a long
    // cycle at fan-tacho rates) ----
    if (auxPulseInputUsed() && !auxPulseRunning) {
        startPcnt(PCNT_UNIT_AUX_PULSE, PIN_AUX_PULSE);
        auxPulseLast = 0;  auxPulsesAcc = 0;  wind2Pulses = 0;  sleepPulsesAux = 0;  wind2GustWin.clear();
        wind2LastMs = millis();  auxPulseRunning = true;
    } else if (!auxPulseInputUsed() && auxPulseRunning) {
        stopPcnt(PCNT_UNIT_AUX_PULSE);
        auxPulseRunning = false;  gIo.auxPulsesCycle = 0;
    }
    if (auxPulseRunning) {
        uint32_t d = readPcnt(PCNT_UNIT_AUX_PULSE, auxPulseLast) + sleepPulsesAux;
        sleepPulsesAux = 0;
        if (gCfg.wind2Mode) {
            wind2Pulses += d;
        } else {
            auxPulsesAcc   += d;
            gIo.auxPulsesTotal += d;
            auxPulsesTotalKept  = gIo.auxPulsesTotal;
        }
    }

    if (gCfg.tachoEn && !tachoRunning) {
        startPcnt(PCNT_UNIT_TACHO, PIN_TACHO);
        tachoLast = 0;  tachoPulsesAcc = 0;  tachoSinceMs = millis();  tachoRunning = true;
    } else if (!gCfg.tachoEn && tachoRunning) {
        stopPcnt(PCNT_UNIT_TACHO);
        tachoRunning = false;  gIo.fanRpm = 0.0f;  gIo.fanStalled = false;
    }
    if (tachoRunning) tachoPulsesAcc += readPcnt(PCNT_UNIT_TACHO, tachoLast);

    // ---- outputs ----
    updateOutputs();
}

volatile uint32_t sleepPulsesAux = 0;

// One sub-sample of the 2nd anemometer: speed from the pulses since the last
// sub-sample (same calibration and plausibility limit as the main sensor),
// direction (wind2Mode 2) from the AUX analog input, linear 0-3.3 V like an
// uncalibrated Davis vane.
static void sampleWind2()
{
    unsigned long now = millis();
    unsigned long elapsed = now - wind2LastMs;
    wind2LastMs = now;
    uint32_t pulses = wind2Pulses;
    float speed = elapsed > 0 ? pulses / (elapsed / 1000.0f) * gCfg.windMsPerPps : 0.0f;
    wind2Pulses = 0;
    if (elapsed == 0) return;
    if (speed > MAX_PLAUSIBLE_WIND_SPEED_MS || elapsed > 65535) {
        dbgSerial.printf("Wind 2: %.1f m/s REJECTED (implausible)\n", speed);
        wind2GustWin.clear();   // no gust window across the gap
        return;
    }
    wind2PulseSum += pulses;
    wind2MsSum    += (uint32_t)elapsed;
    float gust = wind2GustWin.push(pulses, (uint32_t)elapsed, gCfg.windMsPerPps);
    if (gust > wind2Gust) wind2Gust = gust;
    if (ioWind2DirActive()) {
        int deg = map(adcAverage(PIN_AUX_AIN), 0, 4095, 0, 359) + gCfg.wind2DirOffsetDeg;
        float rad = ((deg % 360 + 360) % 360) * DEG_TO_RAD;
        wind2SinSum += sinf(rad);
        wind2CosSum += cosf(rad);
    }
    wind2Samples++;
}

void ioSample()
{
    if (!ioV2()) return;
    if (ioUvActive()) {
        uint32_t mv = 0;
        for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_UV_AIN);
        uvAinMvSum += mv / 8.0f;
        uvAinSamples++;
    }
    if (ioAuxAinActive()) {
        uint32_t mv = 0;
        for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_AUX_AIN);
        auxAinMvSum += mv / 8.0f;
        auxAinSamples++;
    }
    if (gCfg.wind2Mode) sampleWind2();
    if (!(gIo.fanOn && gIo.fanDuty >= 100)) fanFullWholeCycle = false;
    cycleSamples++;
    sampled = true;
    updateOutputs();   // react to the new temperature right away
}

void ioFinishCycle(unsigned long /*elapsedMs*/)
{
    if (!ioV2()) return;
    ioPoll();   // collect the last pulses

    gIo.auxAinV = (ioAuxAinActive() && auxAinSamples > 0)
                  ? auxAinMvSum / auxAinSamples / 1000.0f * gCfg.auxAinScale : 0.0f;
    gIo.uvSensorVolts = NAN;
    gIo.uvIndex = NAN;
    gIo.uvIndexCalibrated = false;
    if (ioUvActive() && uvAinSamples > 0 && isfinite(gCfg.uvAinScale) &&
        gCfg.uvAinScale >= 1.0f && gCfg.uvAinScale <= 5.0f) {
        float sensorVolts = uvAinMvSum / uvAinSamples / 1000.0f * gCfg.uvAinScale;
        if (isfinite(sensorVolts) && sensorVolts >= 0.0f && sensorVolts <= 5.5f) {
            gIo.uvSensorVolts = sensorVolts;
            gIo.uvIndexCalibrated = gCfg.uvIndexEn == 1 &&
                uvCalibrationValid(gCfg.uvCalibrated == 1, gCfg.uvIndexSlope, gCfg.uvIndexOffset);
            if (gIo.uvIndexCalibrated)
                gIo.uvIndex = calculateUvIndex(sensorVolts, gCfg.uvIndexSlope, gCfg.uvIndexOffset);
            if (gIo.uvIndexCalibrated && gIo.uvIndex > UV_INDEX_REPORT_MAX)
                dbgSerial.printf("UV index %.2f exceeds telemetry ceiling %.2f; protocol/history values will be capped.\n",
                                 gIo.uvIndex, UV_INDEX_REPORT_MAX);
        } else {
            dbgSerial.printf("UV ADC: scaled voltage %.3f V out of range; reading omitted\n", sensorVolts);
        }
    } else if (ioUvActive() && uvAinSamples == 0) {
        dbgSerial.println("UV ADC: enabled but no samples were collected.");
    } else if (ioUvActive() && (!isfinite(gCfg.uvAinScale) || gCfg.uvAinScale < 1.0f || gCfg.uvAinScale > 5.0f)) {
        dbgSerial.println("UV ADC: invalid divider scale in config; reading omitted.");
    }
    if (ioUvActive() && isfinite(gIo.uvSensorVolts)) {
        if (gIo.uvIndexCalibrated)
            dbgSerial.printf("UV sensor: %.3f V, calibrated index %.2f\n", gIo.uvSensorVolts, gIo.uvIndex);
        else if (gCfg.uvIndexEn)
            dbgSerial.println("UV index unavailable: calibration is not explicitly enabled or has invalid slope/offset.");
        else
            dbgSerial.printf("UV sensor: %.3f V (index calculation disabled)\n", gIo.uvSensorVolts);
    }
    gIo.auxPulsesCycle = (auxPulseRunning && !gCfg.wind2Mode) ? auxPulsesAcc : 0;
    if (gCfg.wind2Mode && wind2Samples > 0 && wind2MsSum > 0) {
        gIo.wind2SpeedMs = (float)wind2PulseSum * 1000.0f / (float)wind2MsSum * gCfg.windMsPerPps;
        gIo.wind2GustMs  = max(wind2Gust, gIo.wind2SpeedMs);
        if (gCfg.wind2Mode == 2) {
            int deg = (int)roundf(atan2f(wind2SinSum, wind2CosSum) * RAD_TO_DEG);
            gIo.wind2DirDeg = (uint16_t)((deg % 360 + 360) % 360);
        }
        dbgSerial.printf("Wind 2: %.1f m/s (gust %.1f), %u deg\n", gIo.wind2SpeedMs, gIo.wind2GustMs, gIo.wind2DirDeg);
    } else if (!gCfg.wind2Mode) {
        gIo.wind2SpeedMs = gIo.wind2GustMs = 0.0f;  gIo.wind2DirDeg = 0;
    }

    if (tachoRunning) {
        unsigned long ms = millis() - tachoSinceMs;
        gIo.fanRpm = (ms > 0) ? tachoPulsesAcc / (float)gCfg.tachoPpr * 60000.0f / ms : 0.0f;
        // A 2-wire fan on PWM has no valid tacho signal between the pulses -
        // only judge a fan that ran at 100 % for the whole cycle.
        gIo.fanStalled = fanFullWholeCycle && cycleSamples > 0 && tachoPulsesAcc == 0;
        if (gIo.fanStalled) dbgSerial.println("Fan: on but no tacho pulses - stalled or not connected?");
    }

    if (gCfg.auxAinEn || gCfg.auxPulseEn || gCfg.tachoEn)
        dbgSerial.printf("AUX: %.3f V, %lu pulses (total %lu) | fan %.0f rpm\n", gIo.auxAinV,
                         (unsigned long)gIo.auxPulsesCycle, (unsigned long)gIo.auxPulsesTotal, gIo.fanRpm);
    if (gIo.veValid)
        dbgSerial.printf("VE.Direct: %.2f V %.2f A, PV %.2f V %.0f W, CS %d ERR %d\n", gIo.ve.battV, gIo.ve.battA,
                         gIo.ve.pvV, gIo.ve.pvW, gIo.ve.chargeState, gIo.ve.error);

    auxAinMvSum = 0.0f;  auxAinSamples = 0;
    uvAinMvSum = 0.0f;   uvAinSamples = 0;
    auxPulsesAcc = 0;
    wind2Gust = wind2SinSum = wind2CosSum = 0.0f;  wind2Samples = 0;
    wind2PulseSum = wind2MsSum = 0;
    tachoPulsesAcc = 0;  tachoSinceMs = millis();
    fanFullWholeCycle = true;  cycleSamples = 0;
}

uint8_t ioStatusByte()
{
    uint8_t s = 0;
    if (gIo.heaterOn)    s |= IO_HEATER_ON;
    if (gIo.fanOn)       s |= IO_FAN_ON;
    if (gIo.fanStalled)  s |= IO_FAN_STALLED;
    if (gIo.battLockout) s |= IO_BATT_LOCKOUT;
    if (gCfg.veDirectEn && !gIo.veValid) s |= IO_VE_NO_DATA;
    return s;
}

bool auxPulseInputUsed()
{
    return ioV2() && (gCfg.auxPulseEn || gCfg.wind2Mode);
}

bool ioAnyEnabled()
{
    return ioV2() && (gCfg.uvEn || gCfg.auxAinEn || gCfg.auxPulseEn || gCfg.wind2Mode || gCfg.heaterMode || gCfg.fanMode ||
                      gCfg.tachoEn || gCfg.veDirectEn);
}

void ioResetAuxPulseTotal()
{
    gIo.auxPulsesTotal = 0;
    auxPulsesTotalKept = 0;
}
