#pragma once
// Receiver for Fine Offset / Ecowitt / Froggit 868 MHz sensors on an SX1262
// (RadioLib 7.x). Same usage pattern as TfaWeatherSensorReceiver:
//
//   FineOffsetSensor::setRadio(&radio);   // the application's SX1262
//   fo.startReceive();                    // again after other users of the radio
//   FoFrame f; float rssi;
//   if (fo.getMessage(f, rssi)) { ... }   // non-blocking, call in loop()
//
// The radio is not owned: the application may use it for something else
// (e.g. LoRaWAN) and calls startReceive() again afterwards.
//
// Other protocols with the same PHY (17.241 kbit/s FSK, sync 2D D4 - e.g.
// LaCrosse / TFA IT+) can be received in the same session: start with a
// receiver bandwidth that covers both, read the raw frame with getRaw() and
// try each decoder (the frame is always FO_MAX_FRAME_LEN bytes; shorter
// frames are followed by noise, which the decoders ignore).
#include <RadioLib.h>
#include "FineOffsetDecoder.h"

class FineOffsetSensor {
public:
    struct RxConfig {
        float   frequencyMhz = 868.3f;
        float   bandwidthKhz = 234.3f;   // SX1262 double-sided RX bandwidth (234.3, 312.0, ...)
        uint8_t frameLen     = FO_MAX_FRAME_LEN;
    };

    static void setRadio(SX1262* r) { radio = r; }

    // Configures FSK (17.241 kbit/s, sync 2D D4, fixed frame length) and starts
    // interrupt-driven reception. Returns a RadioLib error code.
    int16_t startReceive() { return startReceive(RxConfig()); }
    int16_t startReceive(const RxConfig& cfg);

    // true = a frame was received since the last call: its bytes after the
    // sync word (len = the configured frame length, at most cap). Not checked.
    // Restarts the reception after each packet.
    bool getRaw(uint8_t* buf, size_t cap, size_t& len, float& rssi);

    // true = a valid Fine Offset frame (family, CRC, sum) was received since
    // the last call; rssi in dBm.
    bool getMessage(FoFrame& frame, float& rssi);

    // 0x24 family: scale as WH24 / HP1000 instead of WH65 / WS69 (see foDecode()).
    bool wh24Units = false;

    // Time from the packet interrupt to the getRaw() that read it, ms: a
    // packet arriving meanwhile replaces the unread one (diagnostics).
    static uint32_t lastReadDelayMs() { return readDelayMs; }

private:
    static SX1262* radio;
    static volatile bool received;
    static volatile uint32_t irqMs;
    static uint32_t readDelayMs;
    static void onPacket();
    uint8_t frameLen = FO_MAX_FRAME_LEN;
};
