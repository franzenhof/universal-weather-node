#pragma once
// Davis (exclusive) receiver diagnostics: written by the main loop
// (davis_rx.cpp, from the receive task's snapshots), read by the status page
// and the JSON payload (`davis` in /api/data). Plain values: a torn read only
// shows a value one packet old. No RadioLib: host-linked (rf_sensors_data.cpp).
#include <stdint.h>

// Receiver state (same values as DavisHopState in lib/DavisSensorReceiver)
constexpr uint8_t DAVIS_DIAG_ACQUIRE = 0, DAVIS_DIAG_TRACK = 1, DAVIS_DIAG_LOST = 2, DAVIS_DIAG_RAW = 3;

struct DavisTypeDiag {
    uint32_t      count = 0;          // valid packets of this message type since the start
    unsigned long lastMs = 0;         // millis() of the last one
    uint8_t       bytes[6] = {};      // its bytes 0-5 (firmware order)
    char          text[40] = "";      // decoded value, e.g. "24.9 F / -3.9 C"
};

struct DavisDiag {
    bool     running = false;         // receive task active (Davis mode on)
    int16_t  radioError = 0;          // last RadioLib error (0 = none)
    uint8_t  state = DAVIS_DIAG_ACQUIRE;
    uint8_t  txId = 0;                // tracked transmitter ID 1-8; 0 = none yet
    uint8_t  channel = 0;             // channel (hop index) listened to now
    float    freqMHz = 0.0f;          // incl. the offset in use
    float    periodMs = 0.0f;         // measured packet period (nominal until tracked)
    int32_t  offsetHz = 0;            // configured base offset
    int32_t  learnedOffsetHz = 0;     // offset that received the transmitter (scan / AFC)
    uint32_t packets = 0;             // valid packets of the tracked transmitter
    uint32_t crcErrors = 0;
    uint32_t otherPackets = 0;        // other ID, repeater, bad trailer, out of window
    uint32_t misses = 0;
    uint32_t trackSlots = 0;          // tracked slots resolved (packets + misses)
    uint32_t resyncs = 0;
    bool          havePacket = false;
    unsigned long lastPacketMs = 0;
    float    lastRssi = 0.0f;         // last valid packet
    bool     haveTiming = false;
    int32_t  lastTimingErrUs = 0;     // arrival - prediction of the last tracked packet
    bool     hasRainRate = false;
    float    rainRateMmH = 0.0f;      // from the ISS's time between tips
    bool     hasGust = false;
    float    gust10MinMs = 0.0f;      // the ISS's 10-min high (message 9), m/s
    DavisTypeDiag types[16];          // by message type
};
extern DavisDiag gDavisDiag;

inline const char* davisDiagStateName(uint8_t s)
{
    switch (s) {
        case DAVIS_DIAG_TRACK: return "TRACK";
        case DAVIS_DIAG_LOST:  return "LOST";
        case DAVIS_DIAG_RAW:   return "RAW";
        default:               return "ACQUIRE";
    }
}

// Sync rate in % (tracked slots with a packet), -1 = no slot yet.
inline float davisSyncPct(const DavisDiag& d)
{
    return d.trackSlots ? 100.0f * (float)(d.trackSlots - d.misses) / (float)d.trackSlots : -1.0f;
}

// Message type names (as DavisDecoder's davisTypeName(), kept here so the
// JSON code needs no library)
inline const char* davisDiagTypeName(uint8_t t)
{
    static const char* const names[16] = { "type 0", "type 1", "supercap", "type 3", "uv", "rainrate", "solar", "solarcell",
                                           "temp", "gust", "humidity", "type B", "type C", "type D", "rain", "soil/leaf" };
    return names[t & 0x0F];
}
