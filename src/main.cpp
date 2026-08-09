#include <Arduino.h>

/**
 * LoRaWAN Weather Node – Heltec WiFi LoRa V3
 *
 * Sensors:
 *   - Davis rain sensor     (reed switch, PCNT)
 *   - Davis wind sensor     (reed switch wind speed PCNT + potentiometer direction ADC)
 *   - BME280                (temperature, humidity, barometric pressure – I2C)
 *   - DS18B20 Bus 1         (up to 3 sensors, 1-Wire)
 *   - LiPo battery          (voltage measurement via voltage divider + ADC, optional)
 *
 * Transmission: CayenneLPP via LoRaWAN EU868 → own ChirpStack server
 *
 * Configuration: Hold user button (GPIO 0) during boot to open the Wi-Fi
 * portal (SSID: WeatherNode). On first boot (no DevEUI stored) the portal
 * opens automatically. All parameters are stored in NVS.
 *
 * Button operation:
 *   Press → turn on display for 20 s
 *
 * LoRaWAN downlink commands (FPort 1, 1 byte):
 *   0x01 → reset rain accumulator (since start)
 *   0x02 → restart device
 *   0x03 → open Wi-Fi config portal (takes effect after current send cycle)
 *   0x04 → enable debug mode (persistent)
 *   0x05 → disable debug mode (persistent)
 *   0x10 → set parameter(s): [0x10, paramId, valueMSB, valueLSB, ...]
 *           paramId 1..14 (signed 16-bit values, validated — see params.cpp
 *           for the full list, also reachable via MQTT and the web settings page)
 *   0xFF → factory reset: wipe all config (NVS) and restart into the
 *           first-boot config portal
 *
 * LPP channel mapping:
 *   Ch  1 Direction        → wind direction (°, if wind sensor enabled)
 *   Ch  1 Analog Input     → avg wind speed (m/s or km/h, if wind sensor enabled)
 *   Ch  2 Analog Input     → wind gust max (m/s or km/h, if wind sensor enabled)
 *   Ch  3 Analog Input     → rain rate (mm/h, if rain sensor enabled)
 *   Ch200 Analog Input     → free heap (KiB, debug mode only)
 *   Ch204 Analog Input     → uptime (h, debug mode only)
 *   Ch  1 Distance         → rain current cycle (mm, if rain sensor enabled)
 *   Ch  2 Distance         → rain since start (mm, if rain sensor enabled)
 *   Ch  2 Digital Input    → status byte: Bit0=BME280, Bit1=Bus1, Bit2-5=errors, Bit6=imperial units, Bit7=wind unit
 *   Ch200 Digital Input    → cycle counter (0–255, debug mode only)
 *   Ch202 Digital Input    → send fail counter (0-255, debug mode only)
 *   Ch  1 Voltage          → LiPo voltage (V, optional)
 *   Ch  1 Rel. Humidity    → humidity (%, BME280)
 *   Ch  1 Baro. Pressure   → barometric pressure QNH (hPa, BME280)
 *   Ch  2 Baro. Pressure   → barometric pressure absolute (hPa, BME280)
 *   Ch  1 Temperature      → temperature (°C, BME280)
 *   Ch  2–4 Temperature    → DS18B20 Bus 1 sensor 0–2 (°C)
 *   Ch201 Temperature      → CPU temperature (°C, debug mode only)
 *
 * File layout: this file is orchestration only (setup/loop, sampling,
 * button, LoRaWAN send). See sensors.cpp (sensor reads), lora_payload.cpp
 * (CayenneLPP/binary encoding), portal.cpp (Wi-Fi config portal + web
 * routes), config.cpp (NVS), params.cpp (shared downlink/MQTT parameters),
 * wifi_mqtt.cpp / web_api.cpp (parallel WiFi+MQTT transport).
 */

// ============================================================================
// INCLUDES
// ============================================================================
#include <heltec_unofficial.h>
#include <LoRaWAN_ESP32.h>
#include <pins_arduino.h>
#include <esp_task_wdt.h>
#include <esp_netif.h>
#include <esp_event.h>
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
#include "partition_repair.h"

// ============================================================================
// DISPLAY MANAGEMENT
// ============================================================================
static unsigned long displayUntilMs = 0;

static void activateDisplay(unsigned long durationMs = 20000)
{
    displayUntilMs = millis() + durationMs;
    display.displayOn();
}

static void updateDisplayState()
{
    if (displayUntilMs > 0 && millis() >= displayUntilMs) {
        displayUntilMs = 0;
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

void IRAM_ATTR buttonISR()
{
    // Simple software debounce: ignore edges < 50 ms after the previous one
    static unsigned long lastEdgeMs = 0;
    unsigned long now = millis();
    if (now - lastEdgeMs < 50) return;
    lastEdgeMs = now;

    if (digitalRead(BUTTON) == LOW) {
        isrPressedAtMs = now;
        isrPressEvent  = true;
    } else {
        isrReleasedAtMs = now;
        isrReleaseEvent = true;
    }
}

// Runs the config portal, persists the result, shows a status screen, and
// restarts. Shared by all three portal-entry paths (button long-press,
// first boot, LoRaWAN-downlink 0x03), which otherwise duplicated this exact
// sequence. `showStartScreen` is skipped on first boot, where setup()'s own
// boot banner already covers it (runConfigPortal() shows its own "Starting
// config portal..." message immediately anyway).
static void openPortalSaveAndRestart(bool showStartScreen = true)
{
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
    ESP.restart();
}

static void handleButton()
{
    static unsigned long pressedAtMs = 0;
    static bool          longFired   = false;

    // Consume ISR press event (falling edge)
    if (isrPressEvent) {
        isrPressEvent = false;
        pressedAtMs   = isrPressedAtMs;
        longFired     = false;
    }

    // Consume ISR release event (rising edge)
    if (isrReleaseEvent) {
        isrReleaseEvent = false;
        if (!longFired && (isrReleasedAtMs - pressedAtMs) < 3000) {
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

// Waits 'ms' milliseconds, polling the button every 50 ms.
// This ensures the button stays responsive during blocking setup loops.
static void waitWithButtonPoll(unsigned long ms)
{
    unsigned long start = millis();
    while (millis() - start < ms) {
        handleButton();
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// HARDWARE OBJECTS
// ============================================================================
LoRaWANNode* loRaNode = nullptr;

// ============================================================================
// MEASUREMENT DATA
// ============================================================================
RTC_DATA_ATTR uint8_t loopCount = 0;

int  sendFailCount            = 0;
bool portalRequestedViaDownlink = false;

// ============================================================================
// SAMPLING ACCUMULATORS
// A sub-sample is taken every sampleSec seconds (max. uplinkIntervalSec).
// At the end of the uplink interval accumulators are reduced to final values.
// Wind direction: vector averaging (sin/cos) so that 350°+10° → 0° is correct.
// ============================================================================
static float         windDirSinSum    = 0.0f;
static float         windDirCosSum    = 0.0f;
static float         windSpeedSum     = 0.0f;
static float         bme280TempSum    = 0.0f;
static float         bme280PressAbsSum = 0.0f;
static float         bme280PressSeaSum = 0.0f;
static float         bme280HumSum     = 0.0f;
static float         rainCycleMm      = 0.0f;
static int           subSampleCount   = 0;
static unsigned long lastSampleMs     = 0;
unsigned long        lastSendMs       = 0;

// Set while a send cycle is waiting on the non-blocking DS18B20 conversion
// (see loop()) — sampling/next-cycle checks are frozen during this window,
// exactly like they were while readOneWire() used to block for it.
static bool          sendPending      = false;
static unsigned long sendElapsedMs    = 0;

// ============================================================================
// SAMPLING FUNCTIONS
// ============================================================================

// Reset accumulators (call after each transmission)
static void resetAccumulators()
{
    windDirSinSum     = 0.0f;
    windDirCosSum     = 0.0f;
    windSpeedSum      = 0.0f;
    bme280TempSum     = 0.0f;
    bme280PressAbsSum = 0.0f;
    bme280PressSeaSum = 0.0f;
    bme280HumSum      = 0.0f;
    rainCycleMm       = 0.0f;
    subSampleCount    = 0;
    loopCount++;   // increment cycle counter after transmission
    // windGustMs is intentionally left untouched here (see takeSample()):
    // resetting it eagerly would make it read 0 — while windSpeedMs still
    // shows the just-finished cycle's average — for however long it takes
    // the new cycle's first sub-sample to arrive. Live consumers (Wi-Fi
    // status page, REST API, MQTT) read these globals at arbitrary moments,
    // not just right after a transmission, so that gap was directly
    // observable as "wind speed has a value, gust is still 0".
}

// Take one sub-sample (called every sampleMs)
static void takeSample()
{
    // Reset the gust baseline for the new cycle right before its first real
    // reading overwrites it (see resetAccumulators() above for why this
    // can't just happen there), so windGustMs never sits at a stale 0 while
    // windSpeedMs still reflects the previous cycle.
    if (subSampleCount == 0) windGustMs = 0.0f;

    readPulseCounters();   // windSpeedMs, windGustMs, rainMM, rainMMSinceStart

    if (gCfg.windEn) {
        uint16_t dir = readWindDirection();
        float rad     = dir * DEG_TO_RAD;
        windDirSinSum += sinf(rad);
        windDirCosSum += cosf(rad);
        windSpeedSum  += windSpeedMs;
    }
    if (gCfg.rainEn)
        rainCycleMm += rainMM;

    if (bme280Present) {
        readBme280();
        bme280TempSum     += bme280TempC;
        bme280PressAbsSum += bme280PressAbs;
        bme280PressSeaSum += bme280PressSea;
        bme280HumSum      += bme280Humidity;
    }
    subSampleCount++;
}

// Calculate final values from accumulators (once per send cycle)
static void computeFinalValues(unsigned long elapsedMs)
{
    if (subSampleCount == 0) return;

    if (gCfg.windEn) {
        // Vector-average wind direction: circular arithmetic correct (350°+10° → 0°)
        float avgSin  = windDirSinSum / subSampleCount;
        float avgCos  = windDirCosSum / subSampleCount;
        int   angleDeg = (int)roundf(atan2f(avgSin, avgCos) * RAD_TO_DEG);
        windDirection = (uint16_t)((angleDeg % 360 + 360) % 360);
        windSpeedMs   = windSpeedSum / subSampleCount;
        // Gust should not be lower than the cycle average, otherwise some
        // dashboard queries may treat gust as missing at very short sample windows.
        if (windGustMs < windSpeedMs) windGustMs = windSpeedMs;
    }
    if (gCfg.rainEn) {
        rainMM      = rainCycleMm;
        // Guard: avoid astronomically high rate on very short first cycle
        rainRateMmH = (elapsedMs >= 1000)
                      ? rainCycleMm / ((float)elapsedMs / 3600000.0f)
                      : 0.0f;
    }
    if (bme280Present) {
        bme280TempC    = bme280TempSum    / subSampleCount;
        bme280PressAbs = bme280PressAbsSum / subSampleCount;
        bme280PressSea = bme280PressSeaSum / subSampleCount;
        bme280Humidity = bme280HumSum     / subSampleCount;
    }

    cpuTemperature = heltec_temperature();
    batteryVolt    = gCfg.lipoEn ? readBatteryVoltage() : 0.0f;

    dbgSerial.printf("[Cycle %d samples] Wind: %d°, %.1f m/s (gust %.1f) | "
                  "Rain: %.1f mm @ %.1f mm/h | Batt: %.2fV | CPU: %.1f°C\n",
                  subSampleCount, windDirection, windSpeedMs, windGustMs,
                  rainMM, rainRateMmH, batteryVolt, cpuTemperature);
}

// Thin LoRaWAN-specific parser: byte-frame commands map onto the shared,
// transport-agnostic actions in params.cpp/h, which are also reachable via
// the MQTT command topic (see wifi_mqtt.cpp).
static void processDownlink(const uint8_t* data, size_t len)
{
    if (len < 1) return;

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
                bool ok = applyDownlinkParam(paramId, value);
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

static void sendLoRa()
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

    int16_t state = loRaNode->sendReceive(txBuf, txLen, txPort, downlinkData, &lenDown);

    char buf[64];
    if (state == RADIOLIB_ERR_NONE) {
        dbgSerial.println("Uplink sent, no downlink.");
        sendFailCount = 0;
        display.drawString(0, 44, "Uplink sent");
        display.drawString(0, 55, "no downlink");
    } else if (state > 0) {
        dbgSerial.printf("Uplink + Downlink (%d bytes), SNR: %.1f dB, RSSI: %.1f dBm\n",
                      lenDown, radio.getSNR(), radio.getRSSI());
        sendFailCount = 0;
        processDownlink(downlinkData, lenDown);
        display.drawString(0, 44, "Uplink + Downlink");
        snprintf(buf, sizeof(buf), "SNR %.1f  RSSI %.1f", radio.getSNR(), radio.getRSSI());
        display.drawString(0, 55, buf);
    } else {
        dbgSerial.printf("Send failed (code %d), attempt %d/10\n", state, ++sendFailCount);
        display.drawString(0, 44, "Send error");
        snprintf(buf, sizeof(buf), "Code %d  Attempt %d/10", state, sendFailCount);
        display.drawString(0, 55, buf);
        if (sendFailCount >= 10) {
            dbgSerial.println("Too many send failures – restarting for new join.");
            delay(1000);
            ESP.restart();
        }
    }
}

// ============================================================================
// DRAW DISPLAY CONTENT (only call when display is active)
// ============================================================================
static void drawDisplay()
{
    char buf[64];
    display.clear();

    if (gCfg.windEn) {
        snprintf(buf, sizeof(buf), "D%03d S%.1f G%.1f %s",
                 windDirection,
                 windSpeedForOutput(windSpeedMs, gCfg),
                 windSpeedForOutput(windGustMs,  gCfg),
                 windSpeedUnitLabel(gCfg));
        display.drawString(0,  0, buf);
    }

    if (gCfg.rainEn)
        snprintf(buf, sizeof(buf), "#%d  Rain: %.1f%s", loopCount,
                 rainForOutput(rainMMSinceStart, gCfg), rainUnitLabel(gCfg));
    else
        snprintf(buf, sizeof(buf), "#%d", loopCount);
    display.drawString(0, 11, buf);

    if (bme280Present)
        snprintf(buf, sizeof(buf), "%.1f%s  %.1f%%rH  %.0f%s",
                 temperatureForOutput(bme280TempC, gCfg), temperatureUnitLabel(gCfg),
                 bme280Humidity,
                 pressureForOutput(bme280PressAbs, gCfg), pressureUnitLabel(gCfg));
    else
        snprintf(buf, sizeof(buf), "no BME280");
    display.drawString(0, 22, buf);

    snprintf(buf, sizeof(buf), "Batt: %.3fV", batteryVolt);
    display.drawString(0, 33, buf);
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
void setup()
{
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
    // missing the dedicated "cfgdata" NVS partition — see partition_repair.h
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
    dbgSerial.printf("PCB rev %d: Wind=GPIO%d, Rain=GPIO%d, 1-Wire=GPIO%d\n",
                  gCfg.pcbVersion, pins.windSpeed, pins.rain, pins.oneWire);

    // Initialize radio + join LoRaWAN — only if the LoRaWAN transport is
    // enabled. This blocks (with infinite retry) until joined, so it must
    // never run for a WiFi/MQTT-only node, or setup() would hang forever.
    if (gCfg.loraEn) {
        int16_t radioState = RADIOLIB_ERR_UNKNOWN;
        display.clear();
        display.drawString(0, 0, "Radio init...");
        display.display();
        while (radioState != RADIOLIB_ERR_NONE) {
            radioState = radio.begin();
            if (radioState != RADIOLIB_ERR_NONE) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Error code: %d", radioState);
                display.clear();
                display.drawString(0,  0, "Radio Error");
                display.drawString(0, 11, buf);
                display.drawString(0, 22, "Retry in 2 s...");
                display.display();
                dbgSerial.printf("Radio init failed (code %d)\n", radioState);
                waitWithButtonPoll(2000);
            }
        }

        // LoRaWAN join
        display.clear();
        display.drawString(0, 0, "LoRaWAN Join...");
        display.display();
        int joinAttempts = 0;
        do {
            persist.provision(gCfg.region, 0x00, gCfg.joinEui, gCfg.devEui, gCfg.appKey, gCfg.appKey);
            persist.isProvisioned();
            loRaNode = persist.manage(&radio);

            if (!loRaNode || !loRaNode->isActivated()) {
                ++joinAttempts;
                dbgSerial.printf("Join failed (attempt %d), retry in 2 s...\n", joinAttempts);
                char buf[32];
                snprintf(buf, sizeof(buf), "Attempt %d – retry in 2 s", joinAttempts);
                display.clear();
                display.drawString(0,  0, "Join failed");
                display.drawString(0, 11, buf);
                display.drawString(0, 22, "Possible causes:");
                display.drawString(0, 33, "- Wrong keys");
                display.drawString(0, 44, "- No gateway");
                display.drawString(0, 55, "Hold 3s: Config portal");
                display.display();
                waitWithButtonPoll(2000);
            }
        } while (!loRaNode || !loRaNode->isActivated());
    } else {
        dbgSerial.println("LoRaWAN disabled (loraEn=0) - skipping radio init/join.");
        display.clear();
        display.drawString(0, 0, "LoRaWAN disabled");
        display.drawString(0, 16, gCfg.wifiEn ? "WiFi/MQTT mode" : "No transport enabled!");
        display.display();
        delay(1500);
    }

    // BME280, 1-Wire, PCNT counters, bus1Temps[] init
    sensorsInit(pins);

    lastSampleMs = millis();
    lastSendMs   = millis();

    // Parallel WiFi + MQTT transport (fully optional, see wifiMqttPoll() in
    // loop() for the non-blocking connect state machine).
    wifiMqttSetup();
    webApiSetup();

    // Setup abgeschlossen – jetzt normaler Messbetrieb. Display noch 20 s
    // anzeigen, damit der erste Messwert sichtbar ist, danach Timeout.
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
    // Web server (handleClient()) now runs on its own FreeRTOS task on core
    // 0 (spawned by webApiSetup() in setup()) instead of being polled here —
    // see web_api.cpp's webApiTask for why (mainly: so an HTTP request isn't
    // stuck waiting behind sendLoRa()'s blocking airtime).

    const unsigned long now           = millis();
    const unsigned long sendTargetMs  = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
    // Sample interval from config; never larger than uplink interval
    const unsigned long sampleMs      = min((unsigned long)gCfg.sampleSec * 1000UL, sendTargetMs);

    // ── Take sub-sample ────────────────────────────────────────────────────
    if (!sendPending && now - lastSampleMs >= sampleMs) {
        lastSampleMs = now;
        takeSample();
    }

    // ── Uplink when target interval reached ───────────────────────────────
    if (!sendPending && now - lastSendMs >= sendTargetMs) {
        sendElapsedMs = now - lastSendMs;
        lastSendMs    = now;

        computeFinalValues(sendElapsedMs);
        startOneWireConversion();   // non-blocking; finished off below once ready
        sendPending = true;
    }

    // ── Finish the pending send once the DS18B20 conversion is ready ──────
    // pollOneWireConversion() returns false immediately if the ~750 ms
    // conversion isn't done yet, so loop() (button, WiFi/MQTT poll, watchdog)
    // keeps running normally in the meantime instead of stalling on it —
    // matters most at short cycle times (WiFi/MQTT-only nodes can go down to
    // a few seconds, where a blocking 750 ms wait would be a large fraction
    // of the whole cycle).
    if (sendPending && pollOneWireConversion()) {
        sendPending = false;

        if (gCfg.loraEn) {
            if (gCfg.payloadFormat == 1) buildBinaryPacket();
            else                          buildLppPacket();
        }

        // Display update and LoRa send are independent conditions - keep
        // them as separate statements rather than duplicating the loraEn
        // check across both display-state branches.
        if (displayUntilMs > 0) drawDisplay();
        if (gCfg.loraEn)        sendLoRa();
        if (displayUntilMs > 0) display.display();

        mqttPublishTelemetry();   // no-op unless wifiEn+mqttEn and currently connected
        pushHistorySample();      // record this cycle's finished values for /chart

        resetAccumulators();

        if (portalRequestedViaDownlink) {
            portalRequestedViaDownlink = false;
            openPortalSaveAndRestart();
        }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);   // short yield, no busy-wait
}
