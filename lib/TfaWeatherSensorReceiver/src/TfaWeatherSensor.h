#pragma once
// Receiver for TFA Dostmann / LaCrosse IT+ 868 MHz sensors on an SX1262
// (RadioLib 7.x). Same usage pattern as the BresserWeatherSensorReceiver fork:
//
//   TfaWeatherSensor::setRadio(&radio);          // the application's SX1262
//   tfa.startReceive(TfaWeatherSensor::IT_PLUS_17241);   // again after other users of the radio
//   ItPlusFrame f; float rssi;
//   if (tfa.getMessage(f, rssi)) { ... }         // non-blocking, call in loop()
//
// The radio is not owned: the application may use it for something else
// (e.g. LoRaWAN) and calls startReceive() again afterwards. Only one data rate
// can be received at a time; to cover both, alternate startReceive() calls.
#include <RadioLib.h>
#include "ItPlusDecoder.h"

class TfaWeatherSensor {
public:
    enum Rate : uint8_t {
        IT_PLUS_17241 = 0,   // TX21/25/27/29-IT, TFA 30.3143/3144/3147/3157/3159.IT, ...
        IT_PLUS_9579  = 1,   // TX35-IT, TFA 30.3155.WD, 30.3156.WD, ...
    };

    static void setRadio(SX1262* r) { radio = r; }

    // Configures FSK (868.26 MHz, sync 2D D4, fixed 5-byte frames) and starts
    // interrupt-driven reception. Returns a RadioLib error code.
    int16_t startReceive(Rate rate);

    // true = a valid frame (CRC, model, BCD checked) was received since the
    // last call; rssi in dBm. Restarts the reception after each packet.
    bool getMessage(ItPlusFrame& frame, float& rssi);

    Rate rate() const { return currentRate; }

private:
    static SX1262* radio;
    static volatile bool received;
    static void onPacket();
    Rate currentRate = IT_PLUS_17241;
};
