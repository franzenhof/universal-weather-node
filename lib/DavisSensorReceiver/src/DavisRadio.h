#pragma once
// SX1262 (RadioLib 7.x) set up for Davis ISS packets: 19.2 kbit/s GFSK,
// +-9.9 kHz deviation, 16-bit preamble detector, sync word CB 89, fixed
// length 10 bytes, no hardware CRC, no whitening. Same usage pattern as the
// other receivers in lib/: the radio is not owned (setRadio()).
//
//   DavisRadio::setRadio(&radio);
//   DavisRadio::setPacketHook(fn);        // optional, called from the ISR
//   dr.begin(868.0589f, 93.8f);
//   dr.tune(hopper.tuneMHz());            // standby, retune, receive
//   uint8_t b[10]; float rssi; int64_t t;
//   if (dr.read(b, rssi, t)) hopper.onPacket(b, t);
//
// The packet time t is taken in the DIO1 interrupt ("RX done", esp_timer
// microseconds), so the hop timing does not depend on how fast the caller
// reads the packet.
#include <RadioLib.h>
#include "DavisDecoder.h"

class DavisRadio {
public:
    static void setRadio(SX1262* r) { radio = r; }
    // Called from the interrupt after every packet (must be IRAM-safe), e.g.
    // to wake a task.
    static void setPacketHook(void (*hook)()) { packetHook = hook; }

    // FSK setup and reception on freqMHz; rxBwKhz one of the SX1262 steps
    // (58.6, 78.2, 93.8, 117.3, 156.2, 187.2, 234.3 ...). RadioLib error code.
    int16_t begin(float freqMHz, float rxBwKhz);
    // Standby, new frequency, receive again.
    int16_t tune(float freqMHz);
    // Restart reception on the current frequency.
    int16_t restart();
    // Packet since the last call? Bytes as received (bit-reversed, see
    // DavisDecoder.h), RSSI of the packet (dBm), RX-done time (us).
    bool    read(uint8_t buf[DAVIS_PACKET_LEN], float& rssi, int64_t& rxUs);
    bool    pending() const { return received; }
    void    standby();

private:
    static SX1262* radio;
    static volatile bool received;
    static volatile int64_t rxTimeUs;
    static void (*packetHook)();
    static void onPacket();
};
