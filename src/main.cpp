#include <Arduino.h>

/**
 * Universal Weather Node – Heltec WiFi LoRa V3
 *
 * Sensors:
 *   - Davis rain sensor     (reed switch, PCNT)
 *   - Davis wind sensor     (reed switch wind speed PCNT + potentiometer direction ADC)
 *   - BME280                (temperature, humidity, barometric pressure – I2C)
 *   - DS18B20 Bus 1         (up to 3 sensors, 1-Wire)
 *   - LiPo battery          (voltage measurement via voltage divider + ADC, optional)
 *   - PCB v2.0 only (board_io.cpp, all off by default): AUX analog + pulse
 *     input, heater/fan outputs (PWM, on/auto), fan tacho, Victron VE.Direct
 *
 * Transmission: LoRaWAN (CayenneLPP or compact binary, region configurable)
 * and/or WiFi + MQTT, see README.md.
 *
 * Configuration: hold the user button (GPIO 0) for 3 s during operation to
 * open the Wi-Fi portal (SSID: WeatherNode). On first boot (not yet
 * provisioned) the portal opens automatically. All parameters are stored in
 * NVS (cfgdata partition).
 *
 * Button operation:
 *   Short press → turn on display for 20 s
 *   Hold 3 s    → config portal
 *
 * LoRaWAN downlink commands (FPort 1 only - other ports are ignored, 1 byte):
 *   0x01 → reset rain accumulator (since start)
 *   0x02 → restart device
 *   0x03 → open Wi-Fi config portal (takes effect after current send cycle)
 *   0x04 → enable debug mode (persistent)
 *   0x05 → disable debug mode (persistent)
 *   0x06 → reset AUX pulse total (PCB v2.0)
 *   0x10 → set parameter(s): [0x10, paramId, valueMSB, valueLSB, ...]
 *           paramId 1..50 (signed 16-bit values, validated — see ParamId in
 *           params.h, also reachable via MQTT and the web settings page)
 *   0xFF → factory reset: wipe all config (NVS) and restart into the
 *           first-boot config portal
 *
 * Payload channel/field mapping: see README.md ("Payload") and
 * chirpstack/decoder.js - lora_payload.cpp is the encoder.
 *
 * File layout: this file is orchestration only (setup/loop, sampling,
 * button, LoRaWAN send). See sensors.cpp (sensor reads), lora_payload.cpp
 * + payload_binary.cpp (CayenneLPP/binary encoding), portal.cpp (Wi-Fi config portal + web
 * routes), config.cpp (NVS), params.cpp (shared downlink/MQTT parameters),
 * wifi_mqtt.cpp / web_api.cpp (parallel WiFi+MQTT transport).
 */

// ============================================================================
// INCLUDES
// ============================================================================
// The SX1262 instance lives in heltec_board.cpp (BoardSX1262: Heltec V4 RF
// front end, radio_board.h); heltec_setup() still begins the HSPI bus.
#include <SPI.h>
extern SPIClass* hspi;
#define HELTEC_NO_RADIO_INSTANCE
#include <heltec_unofficial.h>
#include <LoRaWAN_ESP32.h>
#include "heltec_board.h"
#include "radio_board.h"
#include <pins_arduino.h>
#include <esp_task_wdt.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <WiFi.h>
#include "config.h"
#include "globals.h"
#include "params.h"
#include "units.h"
#include "wifi_mqtt.h"
#include "web_api.h"
#include "history.h"
#include "sensors.h"
#include "lora_payload.h"
#include "portal.h"
#include "serial_log.h"
#include "partition_check.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "system_restart.h"
#include "board_io.h"
#include "low_power.h"
#include "lora_transport.h"
#include "wind_window.h"
#include "rf_sensors.h"
#include "sources.h"
#include "loxone.h"

// ============================================================================
// DISPLAY MANAGEMENT
// ============================================================================
// Start + duration rather than an absolute end time, so the timeout stays
// correct across the millis() wrap (every 49.7 days).
static bool          displayActive    = false;
static unsigned long displayOnSinceMs = 0;
static unsigned long displayOnForMs   = 0;

static void drawDisplay();

// Switches the display on and redraws the latest values - turning it on
// alone would show whatever was drawn last (possibly hours-old data or the
// "LoRaWAN Join..." screen).
static void activateDisplay(unsigned long durationMs = 20000)
{
    displayActive    = true;
    displayOnSinceMs = millis();
    displayOnForMs   = durationMs;
    display.displayOn();
    drawDisplay();
    display.display();
}

static void updateDisplayState()
{
    if (displayActive && millis() - displayOnSinceMs >= displayOnForMs) {
        displayActive = false;
        display.displayOff();
    }
}

// ============================================================================
// BUTTON HANDLER (GPIO 0, active LOW)
//   Short press (< 3 s, on release) → turn display on for 20 s
//   Long press  (≥ 3 s)            → open config portal, then restart
//
// Note: GPIO0 must NOT be held during power-on/reset (triggers bootloader).
//       The portal is therefore only opened via long press during operation,
//       or automatically on first boot (no DevEUI stored).
//
// An ISR on CHANGE captures press and release timestamps so that button
// events are never missed, even while sendLoRa() blocks for several seconds.
// The ISR only writes volatile variables; all actual actions happen in the
// main context via handleButton().
// ============================================================================
Config gCfg;

static volatile unsigned long isrPressedAtMs  = 0;
static volatile unsigned long isrReleasedAtMs = 0;
static volatile bool          isrPressEvent   = false;
static volatile bool          isrReleaseEvent = false;
// Guards the four variables above: handleButton() takes and clears them in
// one step, so an edge arriving in between is not lost.
static portMUX_TYPE           buttonMux       = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR buttonISR()
{
    // Simple software debounce: ignore edges < 50 ms after the previous one
    static unsigned long lastEdgeMs = 0;
    unsigned long now = millis();
    if (now - lastEdgeMs < 50) return;
    lastEdgeMs = now;

    bool pressed = digitalRead(BUTTON) == LOW;
    portENTER_CRITICAL_ISR(&buttonMux);
    if (pressed) {
        isrPressedAtMs = now;
        isrPressEvent  = true;
    } else {
        isrReleasedAtMs = now;
        isrReleaseEvent = true;
    }
    portEXIT_CRITICAL_ISR(&buttonMux);
}

// Runs the config portal, persists the result, shows a status screen, and
// restarts. Shared by all three portal-entry paths (button long-press,
// first boot, LoRaWAN-downlink 0x03), which otherwise duplicated this exact
// sequence. `showStartScreen` is skipped on first boot, where setup()'s own
// boot banner already covers it (runConfigPortal() shows its own "Starting
// config portal..." message immediately anyway).
static void openPortalSaveAndRestart(bool showStartScreen = true)
{
    ioSuspendOutputs();
    if (showStartScreen) {
        activateDisplay(5000);
        display.clear();
        display.drawString(0,  0, "Config Portal");
        display.drawString(0, 16, "starting...");
        display.display();
        delay(1000);
    }
    // Free port 80 before WiFiManager's own captive-portal web server tries
    // to bind it — our REST API's WebServer can still be sitting on it from
    // normal operation (only wifiEn/webApiEn toggling off stops it
    // otherwise), and two listeners on the same port is at best undefined,
    // at worst leaves the portal's own server unable to bind at all. This is
    // very likely why the portal was reliable right after a fresh boot
    // (webApiSetup()'s server hasn't started yet at that point) but not on a
    // later, manually-triggered invocation during normal operation.
    webApiStop();
    // Unsubscribe from the task watchdog before entering the portal. It's
    // added to this (loop) task at the very end of setup() — so the
    // first-boot automatic portal call (which runs before that point) was
    // never at risk, but any later, manually-triggered invocation (button/
    // downlink/MQTT) runs with the 120 s watchdog already armed. WiFiManager's
    // startConfigPortal() blocks here for as long as the user takes to fill
    // in the form, without ever calling esp_task_wdt_reset() (that only
    // happens once per our own loop() iteration, which isn't running while
    // blocked here) — so a user taking longer than 120 s would get the
    // watchdog resetting the device mid-session, before saveConfig() is ever
    // reached. Since every portal exit leads to ESP.restart() regardless of
    // outcome, there's no need to re-subscribe afterward. Harmless no-op if
    // this is the first-boot call, where the task was never subscribed yet.
    esp_task_wdt_delete(nullptr);
    gCfg = runConfigPortal(gCfg);
    gCfg.provisioned = 1;
    saveConfig(gCfg);
    display.clear();
    display.drawString(0,  0, "Config saved!");
    display.drawString(0, 16, "Restarting...");
    display.display();
    delay(2000);
    systemRestart("config portal closed");
}

static void handleButton()
{
    static unsigned long pressedAtMs = 0;
    static bool          longFired   = false;

    // Take the ISR's events atomically (check-then-clear could drop an edge)
    portENTER_CRITICAL(&buttonMux);
    bool          pressEvent   = isrPressEvent;
    bool          releaseEvent = isrReleaseEvent;
    unsigned long pressMs      = isrPressedAtMs;
    unsigned long releaseMs    = isrReleasedAtMs;
    isrPressEvent = isrReleaseEvent = false;
    portEXIT_CRITICAL(&buttonMux);

    // Both pending: a release older than the press belongs to the previous press.
    if (pressEvent && releaseEvent && (long)(releaseMs - pressMs) < 0) {
        if (!longFired && (releaseMs - pressedAtMs) < 3000) activateDisplay(20000);
        releaseEvent = false;
    }

    // Press event (falling edge)
    if (pressEvent) {
        pressedAtMs = pressMs;
        longFired   = false;
    }

    // Release event (rising edge)
    if (releaseEvent) {
        if (!longFired && (releaseMs - pressedAtMs) < 3000) {
            activateDisplay(20000);
        }
    }

    // Long press: button currently held for ≥ 3 s
    if (!longFired && pressedAtMs > 0 && (digitalRead(BUTTON) == LOW)
            && (millis() - pressedAtMs >= 3000)) {
        longFired = true;
        openPortalSaveAndRestart();
    }
}

// ============================================================================
// MEASUREMENT DATA
// ============================================================================
RTC_DATA_ATTR uint8_t loopCount = 0;

int  sendFailCount            = 0;
bool portalRequestedViaDownlink = false;

// Uplinks in a row without any downlink. With ADR on (RadioLib default), the
// node sets ADRACKReq after 64 silent uplinks and the network server must
// answer within 32 more, so a working session never stays silent this long.
// After 3 x (64 + 32) silent uplinks the session is assumed broken (ADR has
// long since backed off to SF12) and a fresh join is scheduled - LoRaWAN 1.0.4
// recommends a rejoin once the ADR backoff is exhausted. Observed in the field:
// a session that never received a downlink again, while uplinks kept working.
// If a rejoin does not help (still no downlink at all), the threshold doubles
// each time up to 8x (2304 uplinks, 8 days at 300 s); any downlink resets it.
static constexpr uint32_t NO_DOWNLINK_REJOIN = 3 * (64 + 32);
static constexpr uint8_t  NO_DOWNLINK_MAX_SHIFT = 3;
static uint32_t uplinksWithoutDownlink = 0;
static uint8_t  noDownlinkShift = 0;

// ============================================================================
// SAMPLING AND AGGREGATION
// Wind is sampled every sampleSec (1 or 3 s): raw pulses + one vane reading
// go into the 10-min wind ring (3-s gust, WMO values) and into one WindAcc
// per path. Slow sensors (BME280) are read every SLOW_SAMPLE_MS, at least
// once per base cycle. The base cycle is the shortest send interval
// (LoRaWAN, or MQTT if faster); at its end the globals (status page,
// history, display, weather-network feed) get the base window's values,
// and each path that is due sends its own window's values:
//   - LoRaWAN / MQTT: "interval" (since this path's last send) or "10 min"
//     (WMO values from the ring), see loraWindAgg / mqttWindAgg;
//   - Loxone UDP: "interval" (since its last send; loxSec 0 = every base cycle);
//   - weather networks: always the WMO 10-min wind (wx_upload.cpp).
// Rain: one running total (rainWindowTotalMm(), unaffected by a reset of
// rainMMSinceStart); every path reports the difference since its own last
// send (RainWindow).
// ============================================================================
enum PathIdx { PATH_BASE = 0, PATH_LORA = 1, PATH_MQTT = 2, PATH_LOX = 3, PATH_COUNT = 4 };
static_assert(PATH_COUNT == SRC_PATHS, "wireless wind keeps one window per path (sources.h)");
static WindRing      windRing;
static WindAcc       windAcc[PATH_COUNT];
static RainWindow    rainWin[PATH_COUNT];
static unsigned long pathLastMs[PATH_COUNT];
static constexpr unsigned long SLOW_SAMPLE_MS = 10000;   // BME280: slow-changing values

static float         bme280TempSum    = 0.0f;
static float         bme280PressAbsSum = 0.0f;
static float         bme280PressSeaSum = 0.0f;
static float         bme280HumSum     = 0.0f;
static int           windSampleCount  = 0;   // wind samples in the current base cycle (log only)
static int           bme280SampleCount = 0;
static unsigned long lastWindSampleMs = 0;
static unsigned long lastSlowSampleMs = 0;
unsigned long        lastSendMs       = 0;   // end of the last base cycle (json_payload: nextCycleInSec)

// Set while a send cycle is waiting on the non-blocking DS18B20 conversion
// (see loop()) - sampling/next-cycle checks are frozen during this window.
static bool          sendPending      = false;
static bool          vaneOffForMode   = false;   // true while low-power mode keeps the vane supply off
static unsigned long sendElapsedMs    = 0;

// ============================================================================
// SAMPLING FUNCTIONS
// ============================================================================

static void resetPath(int idx, unsigned long now)
{
    windAcc[idx].reset();
    srcRfWindReset(idx);
    rainWin[idx].reset(rainWindowTotalMm(), now);
    pathLastMs[idx] = now;
}

// One wind sample (every sampleSec). Also collects the rain pulses and
// runs the PCB v2.0 I/O sampling (AUX analog, 2nd anemometer, outputs).
static void takeWindSample()
{
    PulseSample ps = readPulseCounters();   // also adds rain to rainMMSinceStart

    if (gCfg.windEn) {
        // Low-power mode: the vane supply is only on around the reading; the
        // filter (330R + 10k + up to 5k wiper, 1 uF: tau ~15 ms) starts fully
        // discharged and needs ~6 tau for < 0.3 % error (spent in light sleep).
        // Otherwise the supply stays on (also after leaving low-power mode).
        bool vaneSwitched = lowPowerActive() && ioV2() && gCfg.vanePowerSave;
        if (vaneSwitched) { windVaneSupply(true); lowPowerSleep(100); }
        else windVaneSupply(true);
        uint16_t dir = readWindDirection();
        if (vaneSwitched) windVaneSupply(false);
        if (ps.windValid) {
            float gust = windRing.push(ps.windPulses, ps.elapsedMs, dir, true, gCfg.windMsPerPps);
            for (int i = 0; i < PATH_COUNT; i++) windAcc[i].add(ps.windPulses, ps.elapsedMs, dir, true, gust);
        }
    }
    ioSample();   // PCB v2.0: AUX analog input, 2nd anemometer, fan state for the stall check
    windSampleCount++;
}

// Slow sensors (every SLOW_SAMPLE_MS, at least once per base cycle).
static void takeSlowSample()
{
    if (readBme280()) {
        bme280TempSum     += bme280TempC;
        bme280PressAbsSum += bme280PressAbs;
        bme280PressSeaSum += bme280PressSea;
        bme280HumSum      += bme280Humidity;
        bme280SampleCount++;
    }
}

// Wind/rain values of one path at the moment it sends.
static PathValues pathValues(int idx, int aggMode, unsigned long now)
{
    PathValues v;
    // wind from the selected source (board anemometer or a wireless sensor)
    WindStats w = srcIsRadio(SG_WIND) ? srcRfWind(idx, aggMode == WIND_AGG_10MIN)
                : (aggMode == WIND_AGG_10MIN) ? windRing.stats(WIND_SPAN_10MIN_MS, gCfg.windMsPerPps)
                                              : windAcc[idx].result(gCfg.windMsPerPps);
    // No valid sample in the window (all rejected, wireless sensor silent):
    // windValid stays false and the payloads leave wind out - not the
    // previous cycle's values as if they were current. Speed/gust 0 for the
    // globals; the vane keeps its last direction.
    v.windValid  = w.valid;
    v.windDirDeg = (w.valid && w.dirValid) ? w.dirDeg : windDirection;
    if (w.valid) {
        v.windSpeedMs = w.meanMs;
        v.windGustMs  = w.gustMs;
    }
    if (srcActive(SG_RAIN)) {
        const float total = rainWindowTotalMm();
        v.rainMm      = rainWin[idx].amount(total);
        v.rainRateMmH = rainWin[idx].rateMmH(total, now);
    }
    return v;
}

// End of a base cycle: the base window's values become the globals
// (status page, REST, history, display, weather-network feed).
static void computeFinalValues(unsigned long elapsedMs, unsigned long now)
{
    if (srcActive(SG_WIND)) {
        PathValues b = pathValues(PATH_BASE, WIND_AGG_INTERVAL, now);
        windCycleValid = b.windValid;
        windSpeedMs   = b.windSpeedMs;
        windGustMs    = b.windGustMs;
        windDirection = b.windDirDeg;
        if (!b.windValid) dbgSerial.println("Wind: no valid sample this cycle - left out.");
    } else {
        windCycleValid = false;
    }
    if (srcActive(SG_RAIN)) {
        const float total = rainWindowTotalMm();
        rainMM      = rainWin[PATH_BASE].amount(total);
        rainRateMmH = rainWin[PATH_BASE].rateMmH(total, now);
    }
    if (bme280Present && bme280SampleCount > 0) {
        bme280TempC    = bme280TempSum    / bme280SampleCount;
        bme280PressAbs = bme280PressAbsSum / bme280SampleCount;
        bme280PressSea = bme280PressSeaSum / bme280SampleCount;
        bme280Humidity = bme280HumSum     / bme280SampleCount;
    }
    srcUpdateCycle();   // temperature/humidity, UV, light from the selected sources
    bme280TempSum = bme280PressAbsSum = bme280PressSeaSum = bme280HumSum = 0.0f;
    bme280SampleCount = 0;

    cpuTemperature = heltec_temperature();
    batteryVolt    = gCfg.lipoEn ? readBatteryVoltage() : 0.0f;
    ioFinishCycle(elapsedMs);

    dbgSerial.printf("[Cycle %d wind samples] Wind: %d°, %.1f m/s (gust %.1f) | "
                  "Rain: %.1f mm @ %.1f mm/h | Batt: %.2fV | CPU: %.1f°C\n",
                  windSampleCount, windDirection, windSpeedMs, windGustMs,
                  rainMM, rainRateMmH, batteryVolt, cpuTemperature);
    windSampleCount = 0;
}

// A path is due once its interval has passed. A little slack (at most 2 s,
// at most half a base cycle) so a base cycle ending a few ms early does not
// push the send a whole cycle - but not more: half a base cycle let a path
// with a multiple of the base interval send up to 25 % too often.
static constexpr unsigned long PATH_JITTER_MS = 2000;
static bool pathDue(int idx, unsigned long now, unsigned long intervalMs, unsigned long baseMs)
{
    return now - pathLastMs[idx] + min(baseMs / 2, PATH_JITTER_MS) >= intervalMs;
}

// Thin LoRaWAN-specific parser: byte-frame commands map onto the shared,
// transport-agnostic actions in params.cpp/h, which are also reachable via
// the MQTT command topic (see wifi_mqtt.cpp). Commands are only accepted on
// DOWNLINK_PORT (README: FPort 1); other application ports are ignored.
static constexpr uint8_t DOWNLINK_PORT = 1;
static void processDownlink(uint8_t fPort, const uint8_t* data, size_t len)
{
    if (len < 1) return;
    if (fPort != DOWNLINK_PORT) {
        dbgSerial.printf("Downlink on FPort %u ignored (commands: FPort %u).\n", fPort, DOWNLINK_PORT);
        return;
    }

    switch (data[0]) {
        case 0x01:
            cmdResetRainAccumulator();
            break;
        case 0x02:
            cmdRestart();
            break;
        case 0x03:
            cmdRequestConfigPortal();
            break;
        case 0x04:
            cmdSetDebugMode(true);
            break;
        case 0x05:
            cmdSetDebugMode(false);
            break;
        case 0x06:
            cmdResetAuxPulses();
            break;
        case 0xFF:
            cmdFactoryReset();
            break;
        case 0x10: {
            // Format: [0x10, paramId, valueMSB, valueLSB, ...] (1..9)
            if (len < 4 || ((len - 1) % 3) != 0) {
                dbgSerial.println("Downlink: invalid parameter payload length.");
                break;
            }
            bool anyApplied = false;
            for (size_t i = 1; i + 2 < len; i += 3) {
                uint8_t paramId = data[i];
                int16_t value = (int16_t)(((uint16_t)data[i + 1] << 8) | data[i + 2]);
                bool ok = applyParam(paramId, value);
                dbgSerial.printf("Downlink: set param %u = %d -> %s\n",
                              paramId, value, ok ? "OK" : "INVALID");
                anyApplied = anyApplied || ok;
            }
            if (anyApplied) {
                saveConfig(gCfg);
            }
            break;
        }
        default:
            dbgSerial.printf("Downlink: unknown command 0x%02X\n", data[0]);
            break;
    }
}

// Returns false if the uplink was not sent because the regional duty-cycle
// limit does not allow it yet (retry at the next cycle, path window kept).
static bool sendLoRa()
{
    uint8_t downlinkData[256] = {};
    size_t  lenDown = sizeof(downlinkData) - 1;

    // payloadFormat selects the uplink encoding + FPort; downlink handling
    // (processDownlink()) is unaffected either way.
    const uint8_t* txBuf;
    size_t         txLen;
    uint8_t        txPort;
    if (gCfg.payloadFormat == 1) {
        txBuf = binBuf; txLen = binLen; txPort = 3;
    } else {
        txBuf = (const uint8_t*)lpp.getBuffer(); txLen = lpp.getSize(); txPort = 1;
    }

    LoRaWANEvent_t evUp = {}, evDown = {};
    loraRadioTaken();   // wireless-sensor reception pauses for the uplink + RX windows
    // RadioLib compares absolute millis() values: after the 49.7-day wrap its
    // scheduled-uplink time would lie ~49 days "in the future" (blocking
    // sleep) and the duty-cycle check would refuse every uplink. Schedule
    // "now" (it is only ever moved forward), and skip the duty-cycle check
    // once if its last-uplink time is from before the wrap.
    loRaNode->scheduleTransmission(0);
    const bool clockWrapped = loRaNode->timeUntilUplink() > 0x80000000UL;
    if (clockWrapped) loRaNode->setDutyCycle(false);
    int16_t state = loRaNode->sendReceive(txBuf, txLen, txPort, downlinkData, &lenDown, false, &evUp, &evDown);
    if (clockWrapped) loRaNode->setDutyCycle(true);

    if (state == RADIOLIB_ERR_UPLINK_UNAVAILABLE) {
        unsigned long waitS = (unsigned long)(loRaNode->timeUntilUplink() / 1000);
        dbgSerial.printf("Uplink skipped: duty-cycle limit (next possible in %lu s), retry next cycle.\n", waitS);
        display.drawString(0, 44, "Uplink deferred");
        display.drawString(0, 55, "duty cycle");
        return false;
    }
    // FCntUp changed: keep the session copy current (RTC RAM + nonces), so a
    // software restart (cmdRestart, settings) never resumes an old counter
    // the network server would reject as a replay.
    if (!persist.saveSession(loRaNode))
        dbgSerial.println("LoRaWAN: failed to persist session/nonces.");

    // status-page diagnostics (read by the web task)
    if (state >= RADIOLIB_ERR_NONE) {
        gLoraDiag.haveUplink = true;
        gLoraDiag.dataRate   = evUp.datarate;
        gLoraDiag.freqMHz    = evUp.freq;
        gLoraDiag.txPowerDbm = evUp.power;
        gLoraDiag.fCntUp     = loRaNode->getFCntUp();
        gLoraDiag.devAddr    = loRaNode->getDevAddr();
    }
    if (state > 0) {
        gLoraDiag.haveDownlink   = true;
        gLoraDiag.lastDownlinkMs = millis();
        gLoraDiag.lastDlRssi     = radio.getRSSI();
        gLoraDiag.lastDlSnr      = radio.getSNR();
    }

    char buf[64];
    if (state == RADIOLIB_ERR_NONE) {
        dbgSerial.println("Uplink sent, no downlink.");
        sendFailCount = 0;
        display.drawString(0, 44, "Uplink sent");
        display.drawString(0, 55, "no downlink");
        if (++uplinksWithoutDownlink >= (NO_DOWNLINK_REJOIN << noDownlinkShift)) {
            dbgSerial.printf("No downlink for %lu uplinks - session assumed broken, rejoining.\n",
                             (unsigned long)uplinksWithoutDownlink);
            uplinksWithoutDownlink = 0;
            if (noDownlinkShift < NO_DOWNLINK_MAX_SHIFT) noDownlinkShift++;
            loraReconnect();
        }
        gLoraDiag.uplinksWithoutDownlink = uplinksWithoutDownlink;
        gLoraDiag.rejoinThreshold = NO_DOWNLINK_REJOIN << noDownlinkShift;
    } else if (state > 0) {
        dbgSerial.printf("Uplink + Downlink (%d bytes), SNR: %.1f dB, RSSI: %.1f dBm\n",
                      lenDown, radio.getSNR(), radio.getRSSI());
        sendFailCount = 0;
        uplinksWithoutDownlink = 0;
        noDownlinkShift = 0;
        gLoraDiag.uplinksWithoutDownlink = 0;
        gLoraDiag.rejoinThreshold = NO_DOWNLINK_REJOIN;
        processDownlink(evDown.fPort, downlinkData, lenDown);
        display.drawString(0, 44, "Uplink + Downlink");
        snprintf(buf, sizeof(buf), "SNR %.1f  RSSI %.1f", radio.getSNR(), radio.getRSSI());
        display.drawString(0, 55, buf);
    } else {
        dbgSerial.printf("Send failed (code %d), attempt %d/10\n", state, ++sendFailCount);
        display.drawString(0, 44, "Send error");
        snprintf(buf, sizeof(buf), "Code %d  Attempt %d/10", state, sendFailCount);
        display.drawString(0, 55, buf);
        if (sendFailCount >= 10) {
            loraReconnect();
            sendFailCount = 0;
        }
    }
    return true;
}

// ============================================================================
// DRAW DISPLAY CONTENT (only call when display is active)
// ============================================================================
static void drawDisplay()
{
    char buf[64];
    display.clear();

    if (srcActive(SG_WIND)) {
        snprintf(buf, sizeof(buf), "D%03d S%.1f G%.1f %s",
                 windDirection,
                 windSpeedForOutput(windSpeedMs, gCfg),
                 windSpeedForOutput(windGustMs,  gCfg),
                 windSpeedUnitLabel(gCfg));
        display.drawString(0,  0, buf);
    }

    if (srcActive(SG_RAIN))
        snprintf(buf, sizeof(buf), "#%d  Rain: %.1f%s", loopCount,
                 rainForOutput(rainMMSinceStart, gCfg), rainUnitLabel(gCfg));
    else
        snprintf(buf, sizeof(buf), "#%d", loopCount);
    display.drawString(0, 11, buf);

    if (gSrc.tempValid && bme280Present)
        snprintf(buf, sizeof(buf), "%.1f%s  %.1f%%rH  %.0f%s",
                 temperatureForOutput(gSrc.tempC, gCfg), temperatureUnitLabel(gCfg),
                 gSrc.humidity,
                 pressureForOutput(bme280PressAbs, gCfg), pressureUnitLabel(gCfg));
    else if (gSrc.tempValid)
        snprintf(buf, sizeof(buf), "%.1f%s  %.1f%%rH (radio)",
                 temperatureForOutput(gSrc.tempC, gCfg), temperatureUnitLabel(gCfg), gSrc.humidity);
    else
        snprintf(buf, sizeof(buf), "no BME280");
    display.drawString(0, 22, buf);

    if (gIo.veValid && !isnan(gIo.ve.battV)) {
        // Victron: battery + PV power, plus which outputs are on
        int n = snprintf(buf, sizeof(buf), "Bat %.2fV", gIo.ve.battV);
        if (!isnan(gIo.ve.pvW)) n += snprintf(buf + n, sizeof(buf) - n, " PV %.0fW", gIo.ve.pvW);
        snprintf(buf + n, sizeof(buf) - n, "%s%s", gIo.heaterOn ? " H" : "", gIo.fanOn ? " F" : "");
    } else {
        snprintf(buf, sizeof(buf), "Batt: %.3fV%s%s", batteryVolt, gIo.heaterOn ? " H" : "", gIo.fanOn ? " F" : "");
    }
    display.drawString(0, 33, buf);

    // Without LoRaWAN the two bottom lines (uplink result) are free: show
    // where to reach the node and what the wireless receiver does.
    if (!gCfg.loraEn) {
        if (!gCfg.wifiEn)               snprintf(buf, sizeof(buf), "WiFi off");
        else if (wifiIsConnected())     snprintf(buf, sizeof(buf), "http://%s", WiFi.localIP().toString().c_str());
        else                            snprintf(buf, sizeof(buf), "WiFi: connecting...");
        display.drawString(0, 44, buf);
        if (gCfg.rfEn) {
            int n = 0;
            for (int i = 0; i < RF_MAX_SENSORS; i++) n += gRfSensors[i].used;
            if (gCfg.rfProto == RF_PROTO_AUTO) snprintf(buf, sizeof(buf), "RF: searching sensor...");
            else                               snprintf(buf, sizeof(buf), "RF: %d sensor(s)", n);
            display.drawString(0, 55, buf);
        }
    }
}

// ============================================================================
// LOOP TASK STACK SIZE
// ============================================================================
// The WiFi/MQTT feature set grew Config to ~2.5 KB (mostly the 2048-byte MQTT
// CA cert buffer), and runConfigPortal()/WiFiManager stack further local
// buffers on top of that on the same call path (setup() ->
// openPortalSaveAndRestart() -> runConfigPortal()). The Arduino-ESP32 default
// 8 KB loopTask stack is no longer enough for that combined depth, which
// crashes as "Stack canary watchpoint triggered (loopTask)" — most reliably
// on first boot / whenever the config portal is entered. Override the core's
// weak getArduinoLoopTaskStackSize() to grow it; ESP32-S3 has 512 KB SRAM, so
// this is cheap.
size_t getArduinoLoopTaskStackSize(void)
{
    return 24576; // 24 KB
}

// ============================================================================
// SETUP
// ============================================================================
static const char* resetReasonText(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external reset";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "crash (panic)";
        case ESP_RST_INT_WDT:   return "watchdog (interrupt)";
        case ESP_RST_TASK_WDT:  return "watchdog (task)";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "brownout (supply voltage dip)";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "unknown";
    }
}

void setup()
{
    gResetReason = resetReasonText(esp_reset_reason());
    // Heltec V3 / V4 by flash size (heltec_board.h); applied after loadConfig()
    boardDetectHardware();
    // Watchdog: 120 s timeout – catches hanging joins and blocked sends
    esp_task_wdt_init(120, true);

    // Enable VEXT (GPIO36, active-LOW) before heltec_setup() so the display
    // already has power when the RST pulse is issued.
    heltec_ve(true);

    heltec_setup();

    // Button interrupt – must be registered early so no press is missed
    pinMode(BUTTON, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON), buttonISR, CHANGE);

    display.init();
    display.setContrast(200);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    display.clear();
    display.displayOff();

    // Detects a device whose flash still has the old/stock partition table,
    // missing the dedicated "cfgdata" NVS partition — see partition_check.h
    // for why this can happen after OTA, and why it can only be detected,
    // not fixed, from here. Neither normal operation nor the Wi-Fi portal
    // would work correctly on such a device (no config can be saved either
    // way, so the portal would just silently lose whatever is entered on
    // every restart) — show the problem on the display instead and halt,
    // rather than let the device look like it's working normally.
    if (cfgdataPartitionMissing()) {
        display.displayOn();
        display.clear();
        display.drawString(0,  0, "cfgdata missing!");
        display.drawString(0, 11, "USB reflash needed");
        display.drawString(0, 22, "(OTA can't fix this)");
        display.drawString(0, 33, "See README.md");
        display.display();
        while (true) {
            delay(1000);
        }
    }

    // Load config and set CPU frequency
    gCfg = loadConfig();
    setCpuFrequencyMhz(gCfg.cpuFreqMhz);

    // Board in effect (setting or detection) and, on a V4, its RF front end -
    // before anything uses the radio. A V4 runs without the custom PCB: its
    // wind/rain inputs collide with the FEM pins (GPIO2/GPIO7), they stay off
    // in RAM (the stored settings are not changed here).
    boardApply(gCfg.boardSel);
    femInit();
    if (!boardPcbAllowed()) { gCfg.windEn = 0; gCfg.rainEn = 0; }

    // PCB v2.0 revision ID on GPIO20 (ADC2: before WiFi/the portal). A v2.0
    // board always runs as v2.0; without an ID the stored revision applies
    // (2.0 can also be set manually there).
    if (boardPcbAllowed() && detectPcbV2()) gCfg.pcbVersion = PCB_REV_2_0;
    else if (gCfg.pcbVersion == PCB_REV_2_0)
        dbgSerial.println("PCB v2.0 set manually (no revision ID read).");
    // v2.0: R38 holds the vane supply off until the firmware switches it on -
    // before the first-boot portal, whose calibration wizard reads the vane.
    windVaneSupply(true);

    // Boot-Anzeige: kurzer Hinweis, dann direkt weiter
    display.displayOn();
    display.clear();
    display.drawString(0,  0, "WeatherNode");
    display.drawString(0, 16, "Hold 3s: Config");
    display.display();
    delay(1500);

    // Ensure the TCP/IP stack and default event loop are initialized before
    // WiFiManager starts.  These calls are safe to make even if the Arduino
    // framework already called them – they return ESP_ERR_INVALID_STATE in
    // that case, which we intentionally ignore.
    esp_netif_init();
    esp_event_loop_create_default();

    // Configure ADC — moved ahead of the (possible) first-boot portal call
    // below so the wind-direction ADC is already usable for the portal's
    // "Calibrate Wind Direction" page, even on a brand-new, never-configured
    // device.
    sensorsEarlyInit();

    // First boot (never provisioned) → open portal automatically. Uses an
    // explicit flag rather than devEui==0, since a WiFi/MQTT-only node
    // (loraEn=0) may never set a non-zero DevEUI.
    if (!gCfg.provisioned) {
        openPortalSaveAndRestart(/*showStartScreen=*/false);
    }

    // Revision-dependent pins
    PinSet pins = getPins(gCfg.pcbVersion);
    if (boardPcbAllowed())
        dbgSerial.printf("PCB rev %d: Wind=GPIO%d, Rain=GPIO%d, 1-Wire=GPIO%d\n",
                      gCfg.pcbVersion, pins.windSpeed, pins.rain, pins.oneWire);
    else
        dbgSerial.println("Heltec V4: wireless build only - PCB inputs (wind, rain, 1-Wire, I/O block) disabled.");

    // BME280, 1-Wire, PCNT counters, bus1Temps[] init
    sensorsInit(pins);
    ioInit();   // PCB v2.0 I/O block (outputs off, enabled inputs started)

    {
        unsigned long t = millis();
        lastWindSampleMs = lastSlowSampleMs = lastSendMs = t;
        for (int i = 0; i < PATH_COUNT; i++) resetPath(i, t);
    }

    // Parallel WiFi + MQTT transport (fully optional, see wifiMqttPoll() in
    // loop() for the non-blocking connect state machine).
    wifiMqttSetup();
    loxoneSetup();   // before the web task: it takes the Loxone lock
    webApiSetup();
    wxUploadSetup();

    // Setup done - normal operation. Take a first sub-sample right away so
    // the 20 s boot display shows real readings instead of zeros (the first
    // full cycle only completes after uplinkIntervalSec).
    takeWindSample();
    takeSlowSample();
    activateDisplay(20000);

    // Enable WDT for the loop task from here
    esp_task_wdt_add(nullptr);
}

// ============================================================================
// LOOP
// ============================================================================
void loop()
{
    esp_task_wdt_reset();
    handleButton();
    updateDisplayState();
    wifiMqttPoll();   // non-blocking; no-op unless wifiEn is set
    ioPoll();         // PCB v2.0: VE.Direct, pulse counters, outputs
    timeSyncPoll();   // starts SNTP once WiFi is first up
    loxonePoll();     // Loxone host name: asynchronous DNS, no waiting
    // Web server (handleClient()) now runs on its own FreeRTOS task on core
    // 0 (spawned by webApiSetup() in setup()) instead of being polled here —
    // see web_api.cpp's webApiTask for why (mainly: so an HTTP request isn't
    // stuck waiting behind sendLoRa()'s blocking airtime).

    const unsigned long now = millis();
    // Intervals: wind sample (1 or 3 s), LoRaWAN, MQTT (0 = like LoRaWAN) and
    // the base cycle = the shortest active send interval, never shorter than
    // one wind sample.
    const unsigned long windSampleMs = (unsigned long)windSampleSecNormalized(gCfg.sampleSec) * 1000UL;
    const unsigned long loraMs       = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
    const bool          mqttOn       = gCfg.wifiEn && gCfg.mqttEn;
    unsigned long       mqttMs       = gCfg.mqttIntervalSec > 0 ? (unsigned long)gCfg.mqttIntervalSec * 1000UL : loraMs;
    mqttMs = max(mqttMs, windSampleMs);
    // Loxone UDP: 0 = every base cycle (does not shorten it), else its own interval
    const bool          loxOn        = gCfg.wifiEn && gCfg.loxEn;
    const unsigned long loxMs        = gCfg.loxSec > 0 ? max((unsigned long)gCfg.loxSec * 1000UL, windSampleMs) : 0;
    unsigned long baseMs = mqttOn ? min(loraMs, mqttMs) : loraMs;
    if (loxOn && loxMs) baseMs = min(baseMs, loxMs);
    baseMs = max(baseMs, windSampleMs);
    const unsigned long slowMs = min(SLOW_SAMPLE_MS, baseMs);
    gBaseCycleMs = baseMs;
    // LoRaWAN duty cycle: never more than 1 % airtime - with a slow data rate
    // (e.g. SF12, ~1.5 s per uplink) the interval is stretched (here ~150 s).
    // The jitter allowance of pathDue() is added, so a stretched interval is
    // never undercut (RadioLib would refuse that uplink, see sendLoRa()).
    unsigned long loraEffMs = loraMs;
    if (loRaNode) loraEffMs = max(loraMs, (unsigned long)loRaNode->getLastToA() * 100UL + PATH_JITTER_MS);
    gLoraDiag.intervalMs = loraEffMs;

    // ── Samples ────────────────────────────────────────────────────────────
    if (!sendPending && now - lastWindSampleMs >= windSampleMs) {
        lastWindSampleMs = now;
        takeWindSample();
    }
    if (!sendPending && now - lastSlowSampleMs >= slowMs) {
        lastSlowSampleMs = now;
        takeSlowSample();
    }

    // ── End of a base cycle ────────────────────────────────────────────────
    if (!sendPending && now - lastSendMs >= baseMs) {
        sendElapsedMs = now - lastSendMs;
        lastSendMs    = now;

        computeFinalValues(sendElapsedMs, now);
        startOneWireConversion();   // non-blocking; finished off below once ready
        sendPending = true;
    }

    // ── Finish the pending cycle once the DS18B20 conversion is ready ──────
    // pollOneWireConversion() returns false immediately if the ~750 ms
    // conversion isn't done yet, so loop() (button, WiFi/MQTT poll, watchdog)
    // keeps running normally in the meantime.
    if (sendPending && pollOneWireConversion()) {
        sendPending = false;
        const unsigned long cycleEnd = lastSendMs;

        if (displayActive) drawDisplay();

        // LoRaWAN: its own window, values as configured (interval / 10 min)
        if (gCfg.loraEn && pathDue(PATH_LORA, cycleEnd, loraEffMs, baseMs)) {
            if (loraReady()) {
                PathValues lv = pathValues(PATH_LORA, gCfg.loraWindAgg, cycleEnd);
                // Largest payload the current data rate allows (ADR can switch to
                // SF12 = 51 bytes on EU868); the builders leave out the least
                // important fields until the packet fits.
                size_t limit = loRaNode->getMaxPayloadLen();
                if (gCfg.payloadFormat == 1) buildBinaryPacket(limit, &lv);
                else                          buildLppPacket(limit, &lv);
                // Deferred by the duty-cycle limit: keep the window, so the
                // next uplink covers this cycle's wind/rain as well.
                if (sendLoRa()) resetPath(PATH_LORA, cycleEnd);
            } else {
                dbgSerial.println("LoRa uplink skipped: not joined (measurements/WiFi continue).");
                resetPath(PATH_LORA, cycleEnd);
            }
            // Low-power mode: SX1262 into sleep (warm start, config kept) until
            // the next uplink - RadioLib wakes it again on the next command.
            if (lowPowerActive()) loraSleep();
        }
        if (displayActive) display.display();

        // MQTT: its own window and interval
        if (mqttOn && pathDue(PATH_MQTT, cycleEnd, mqttMs, baseMs)) {
            PathValues mv = pathValues(PATH_MQTT, gCfg.mqttWindAgg, cycleEnd);
            mqttPublishTelemetry(&mv);   // no-op unless currently connected
            resetPath(PATH_MQTT, cycleEnd);
        }

        // Loxone UDP: its own window (wind "interval", rain since its last send)
        if (loxOn && (loxMs == 0 || pathDue(PATH_LOX, cycleEnd, loxMs, baseMs))) {
            PathValues xv = pathValues(PATH_LOX, WIND_AGG_INTERVAL, cycleEnd);
            loxoneSendTelemetry(&xv);   // skipped (status page) while WiFi is down
            resetPath(PATH_LOX, cycleEnd);
        }

        // A disabled path keeps no window (it would only grow until re-enabled).
        if (!gCfg.loraEn) resetPath(PATH_LORA, cycleEnd);
        if (!mqttOn)      resetPath(PATH_MQTT, cycleEnd);
        if (!loxOn)       resetPath(PATH_LOX, cycleEnd);

        pushHistorySample();   // base cycle values for the dashboard history
        wxUploadPushCycle(srcIsRadio(SG_WIND) ? srcRfWind(PATH_BASE, true)
                                              : windRing.stats(WIND_SPAN_10MIN_MS, gCfg.windMsPerPps));
        resetPath(PATH_BASE, cycleEnd);
        loopCount++;

        if (portalRequestedViaDownlink) {
            portalRequestedViaDownlink = false;
            openPortalSaveAndRestart();
        }
    }

    // RadioLib's individual join exchange is synchronous (several seconds),
    // but no retry loop can hold setup/measurement service indefinitely.
    if (!sendPending) loraPoll();
    // Wireless sensors: restart FSK reception after LoRaWAN used the SX1262,
    // decode received packets (non-blocking; no-op unless rfEn).
    rfSensorsLoop();

    // Low-power mode: light sleep until the next sub-sample / uplink (VE.Direct
    // needs ~2.5 s awake before an uplink to catch a frame); otherwise a short yield.
    if (lowPowerActive() && !sendPending) {
        if (gCfg.vanePowerSave && !vaneOffForMode) { windVaneSupply(false); vaneOffForMode = true; }
        if (!gCfg.vanePowerSave && vaneOffForMode)  { windVaneSupply(true);  vaneOffForMode = false; }
        unsigned long t = millis();
        unsigned long toSample = windSampleMs - min(windSampleMs, t - lastWindSampleMs);
        unsigned long toSlow   = slowMs - min(slowMs, t - lastSlowSampleMs);
        unsigned long toSend   = baseMs - min(baseMs, t - lastSendMs);
        toSample = min(toSample, toSlow);
        if (gCfg.veDirectEn) toSend = (toSend > 2500) ? toSend - 2500 : 0;
        lowPowerSleep(min(toSample, toSend));
    } else {
        if (vaneOffForMode) { windVaneSupply(true); vaneOffForMode = false; }
        vTaskDelay(50 / portTICK_PERIOD_MS);   // short yield, no busy-wait
    }
}
