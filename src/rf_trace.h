#pragma once
// Radio trace ("Funk-Mitschnitt"): a time-limited capture of every packet the
// wireless receiver gets, in every receive phase (Bresser, IT+ 17.241 and
// 9.579 kbit/s, the shared 17.241 kbit/s IT+ / Fine Offset phase, Davis) -
// also frames that fail their checks or come from unknown sensor families,
// which is the point: a user sends us the file, we see what their sensors
// actually transmit.
//
// - Started from the status page / POST /api/rf/trace (duration, filter);
//   Davis mode starts an open-ended one by itself (its sniffer lines,
//   formerly /api/davis/log, go here).
// - Memory: RF_TRACE_SIZE ring of text lines plus statistics, allocated on
//   start (refused when the heap is short), kept after the capture ended so
//   it can be downloaded, freed by "Discard", a new start or a restart.
// - Writers: the main loop only (rf_sensors.cpp, davis_rx.cpp's main-loop
//   side). Readers: the web task (rfTraceWriteFile, rfTraceStatusNow); a
//   spinlock guards the buffer.
// - The file (rfTraceWriteFile) starts with a header of the receive settings
//   and the sensor table - never Wi-Fi, MQTT, admin, LoRaWAN or weather
//   network credentials (host-tested, test/firmware_rftrace.cpp).
#include <stddef.h>
#include <stdint.h>

constexpr size_t   RF_TRACE_SIZE = 24 * 1024;            // text ring
constexpr size_t   RF_TRACE_HEAP_RESERVE = 40 * 1024;    // free heap that must remain after allocating
constexpr size_t   RF_TRACE_FRAME_LEN = 64;              // Fine Offset phase: bytes read per frame while a capture runs
                                                         // (normally FO_MAX_FRAME_LEN = 32): longer unknown frames, e.g. Ecowitt IoT 0xB9, complete
constexpr uint32_t RF_TRACE_MAX_MIN = 120;               // longest capture (minutes)
constexpr int      RF_TRACE_MAX_IDS = 48;                // per-ID statistics

enum RfTraceFilter : uint8_t {
    RF_TRACE_ALL      = 0,   // every packet
    RF_TRACE_PROBLEMS = 1,   // only frames that failed a check or are unknown
    RF_TRACE_ONE_ID   = 2,   // only one sensor ID
};

enum RfTraceResult : uint8_t {
    RF_TR_OK = 0,      // decoded, accepted
    RF_TR_IGNORED,     // decoded, but not taken (ID not in the list, table full, other Davis transmitter)
    RF_TR_FAILED,      // a known format whose check (CRC, sum, digest, parity, values) failed
    RF_TR_UNKNOWN,     // no known format
    RF_TR_EVENT,       // receiver event / info line (Davis: miss, search, settings)
    RF_TR_RESULTS
};

enum RfTracePhase : uint8_t {
    RF_TP_BRESSER = 0,   // 8.21 kbit/s
    RF_TP_ITPLUS17,      // IT+ alone, 17.241 kbit/s
    RF_TP_ITPLUS9,       // IT+ 9.579 kbit/s
    RF_TP_FSK17,         // shared 17.241 kbit/s phase: Fine Offset (+ IT+)
    RF_TP_DAVIS,
    RF_TP_COUNT
};
const char* rfTracePhaseName(uint8_t phase);
const char* rfTraceResultName(uint8_t result);

// One received frame.
struct RfTracePacket {
    uint8_t        phase = RF_TP_BRESSER;
    float          rssi = 0.0f;
    bool           hasFreqErr = false;
    float          freqErrKhz = 0.0f;
    const uint8_t* raw = nullptr;
    size_t         rawLen = 0;
    RfTraceResult  result = RF_TR_UNKNOWN;
    const char*    check = "";       // e.g. "CRC ok, sum ok", "CRC fail", "digest fail"
    const char*    protocol = "";    // decoded as: "Fine Offset", "IT+", "Bresser 7-in-1", ...
    const char*    model = nullptr;  // static string
    bool           hasId = false;
    uint32_t       id = 0;
    const char*    values = "";      // short decoded values
    // reception quality (statistics in the file header)
    uint32_t       nominalMs = 0;    // the sensor's send interval (rfNominalIntervalMs()), 0 = unknown
    bool           hasReadDelay = false;
    uint32_t       readDelayMs = 0;  // packet interrupt -> read by the firmware
};

// A second OK frame of the same ID within this time is the sensor's repeat
// of the same transmission (WH31, WH51, WN32, ... send each packet twice).
constexpr uint32_t RF_TRACE_REPEAT_MS   = 1500;
constexpr uint32_t RF_TRACE_LATE_READ_MS = 100;

struct RfTraceStatus {
    bool     allocated = false;   // a capture (running or finished) is in memory
    bool     running = false;
    bool     autoStarted = false; // Davis mode
    uint8_t  filter = RF_TRACE_ALL;
    uint32_t filterId = 0;
    uint32_t durationMs = 0;      // 0 = until stopped
    uint32_t elapsedMs = 0;
    uint32_t leftMs = 0;
    uint32_t seen = 0;            // frames received during the capture (all, before the filter)
    uint32_t recorded = 0;        // lines written
    uint32_t usedBytes = 0;       // in the ring (<= RF_TRACE_SIZE)
    uint32_t overwrittenBytes = 0;
    char     stopReason[40] = "";
};

// Starts a capture (an existing one is discarded). durationMs 0 = until
// stopped. false + err: not started (heap, parameters).
bool rfTraceStart(uint32_t durationMs, RfTraceFilter filter, uint32_t id, bool autoStarted, char* err, size_t errLen);
void rfTraceStop(const char* reason);
void rfTraceClear();              // frees the memory
void rfTracePoll();               // ends the capture when its time is up (main loop)
bool rfTraceRunning();
RfTraceStatus rfTraceStatusNow();

// Recording (main loop). rfTraceWants(): cheap pre-check so a caller can skip
// formatting; rfTracePacket()/rfTraceText() count every call in the statistics.
bool rfTraceWants(RfTraceResult r, bool hasId, uint32_t id);
void rfTracePacket(const RfTracePacket& p);
void rfTraceText(uint8_t phase, const char* line, RfTraceResult r, bool hasId, uint32_t id);   // preformatted (Davis)

// Fills check / result / protocol / model / id / rawLen (trimmed to the frame)
// of a 17.241 kbit/s frame none of the enabled decoders accepted (fo: Fine
// Offset enabled, itPlus: IT+ enabled).
void rfTraceClassifyFsk17(const uint8_t* b, size_t len, bool fo, bool itPlus, RfTracePacket& p, char* check, size_t checkLen);
// The same for a 5-byte IT+ frame of the IT+-only phases.
void rfTraceClassifyItPlus(const uint8_t* b, RfTracePacket& p, char* check, size_t checkLen);

// Short decoded values for a trace line ("T 12.5C H 70% ...", base units).
struct FoFrame;
struct ItPlusFrame;
void rfTraceFoValues(const FoFrame& f, char* out, size_t len);
void rfTraceItPlusValues(const ItPlusFrame& f, char* out, size_t len);

// The diagnostic file: header (settings, sensor table, statistics) and the
// recorded lines, written in pieces through w.
typedef void (*RfTraceWriter)(const char* s, size_t n, void* ctx);
// receiverInfo: one line about the receiver state (rf_sensors.cpp, not host-built).
void rfTraceWriteFile(RfTraceWriter w, void* ctx, const char* hostname, const char* receiverInfo = nullptr);
// "uwn-rftrace-<hostname>-<YYYYMMDD-HHMM>.txt" (capture start, local time;
// "...-up<uptime s>s.txt" without a synced clock)
void rfTraceFileName(char* out, size_t len, const char* hostname);

// Heap figures (device: heap_caps; host tests may replace them).
extern size_t (*rfTraceFreeHeap)();
extern size_t (*rfTraceLargestFreeBlock)();
