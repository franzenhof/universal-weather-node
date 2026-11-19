// New-node defaults (Wi-Fi station with a wireless sensor) and the wireless
// sensor type "Auto (search)": receive rotation, dwell times, the lock
// decision and parameters 50 / 51.
#include "config.h"
#include "globals.h"
#include "params.h"
#include "rf_sensors.h"
#include <cassert>
#include <cstdio>

// Receive scheduler as rfSensorsLoop() runs it (loop every 50 ms).
struct Sched {
    int set, protoSet, phase;
    unsigned long start = 0;
    explicit Sched(int cfgProto) : set(rfPhaseSet(rfProtoSetFor(cfgProto))), protoSet(rfProtoSetFor(cfgProto)),
                                   phase(rfNextPhase(set, RF_PROTO_ITPLUS9)) {}
    bool foPhase() const { return phase == RF_PROTO_ITPLUS17 && (protoSet & RF_PROTO_FINEOFFSET); }
    void step(unsigned long now)
    {
        if ((set & (set - 1)) && now - start >= rfPhaseDwellMs(phase, foPhase())) { phase = rfNextPhase(set, phase); start = now; }
    }
};

// First packet heard (whole packet inside the sensor's phase), ms after the
// search started; sensor sends every intervalMs from offsetMs on.
static unsigned long firstCatch(int sensorPhase, unsigned long intervalMs, unsigned long offsetMs, unsigned long airMs)
{
    Sched s(RF_PROTO_AUTO);
    unsigned long next = offsetMs, rxStart = 0;
    bool inPacket = false;
    for (unsigned long t = 0; t < 600000; t += 50) {
        int before = s.phase;
        s.step(t);
        if (s.phase != before) inPacket = false;          // receiver restarted: packet lost
        if (!inPacket && t >= next) {                      // packet starts
            inPacket = s.phase == sensorPhase;
            rxStart = next;
            next += intervalMs;
        }
        if (inPacket && t >= rxStart + airMs) return rxStart + airMs;
    }
    return ~0UL;
}

int main()
{
    // ---- defaults of a new node: Wi-Fi + web API on, LoRaWAN/MQTT off, wireless on (Auto), no board wind/rain ----
    const Config d;
    assert(d.loraEn == 0 && d.wifiEn == 1 && d.webApiEn == 1 && d.mqttEn == 0 && d.mqttCtrlEn == 0);
    assert(d.rfEn == RF_RADIO_SX1262 && d.rfProto == RF_PROTO_AUTO && d.rfIds[0] == '\0');
    assert(d.windEn == 0 && d.rainEn == 0 && d.rainPersist == 1 && d.lowPowerEn == 0);
    for (int g = 0; g < 5; g++) assert(d.srcSel[g] == 0);   // value sources: Auto
    assert(d.provisioned == 0);                             // first boot opens the portal
    // a config stored by an older firmware keeps the old defaults for missing keys
    const ConfigLegacyDefaults l;
    assert(l.loraEn == 1 && l.wifiEn == 0 && l.webApiEn == 0 && l.rfEn == RF_RADIO_OFF && l.rfProto == RF_PROTO_BRESSER);
    assert(l.windEn == 1 && l.rainEn == 1);

    // ---- valid sensor types: bit masks 1..15 and 16 = Auto ----
    for (int v = 1; v <= 15; v++) assert(rfProtoValid(v));
    assert(rfProtoValid(RF_PROTO_AUTO) && RF_PROTO_AUTO == 16);
    assert(!rfProtoValid(0) && !rfProtoValid(17) && !rfProtoValid(32) && !rfProtoValid(128) && !rfProtoValid(-1));
    assert(applyParam(PARAM_RF_PROTO, 16) && gCfg.rfProto == RF_PROTO_AUTO);
    assert(!applyParam(PARAM_RF_PROTO, 17) && !applyParam(PARAM_RF_PROTO, 0) && gCfg.rfProto == RF_PROTO_AUTO);
    assert(applyParam(PARAM_RF_PROTO, 10) && gCfg.rfProto == 10);
    // parameter 51: rain week/month/year in flash
    assert(PARAM_RAIN_PERSIST == 51);
    assert(applyParam(PARAM_RAIN_PERSIST, 0) && gCfg.rainPersist == 0);
    assert(applyParam(PARAM_RAIN_PERSIST, 1) && gCfg.rainPersist == 1);
    assert(!applyParam(PARAM_RAIN_PERSIST, 2) && gCfg.rainPersist == 1);

    // ---- protocol sets and rotation ----
    assert(rfProtoSetFor(RF_PROTO_AUTO) == RF_PROTO_ALL);
    assert(rfPhaseSet(rfProtoSetFor(RF_PROTO_AUTO)) == (RF_PROTO_BRESSER | RF_PROTO_ITPLUS17 | RF_PROTO_ITPLUS9));
    assert(rfProtoSetFor(1) == 1 && rfProtoSetFor(10) == 10 && rfProtoSetFor(0) == RF_PROTO_BRESSER && rfProtoSetFor(32) == RF_PROTO_BRESSER);
    const int all = rfPhaseSet(RF_PROTO_ALL);
    assert(rfNextPhase(all, RF_PROTO_ITPLUS9) == RF_PROTO_BRESSER);
    assert(rfNextPhase(all, RF_PROTO_BRESSER) == RF_PROTO_ITPLUS17);
    assert(rfNextPhase(all, RF_PROTO_ITPLUS17) == RF_PROTO_ITPLUS9);
    assert(rfNextPhase(RF_PROTO_ITPLUS17, RF_PROTO_ITPLUS17) == RF_PROTO_ITPLUS17);
    assert(rfPhaseDwellMs(RF_PROTO_BRESSER, false) == 17000 && rfPhaseDwellMs(RF_PROTO_ITPLUS17, true) == 17000);
    assert(rfPhaseDwellMs(RF_PROTO_ITPLUS9, false) == 5000 && rfPhaseDwellMs(RF_PROTO_ITPLUS17, false) == 5000);
    {
        Sched s(RF_PROTO_AUTO);   // Bresser 17 s -> 17.241 kbit/s + Fine Offset 17 s -> IT+ 9.579 kbit/s 5 s
        assert(s.phase == RF_PROTO_BRESSER);
        int seen[3] = {};
        unsigned long switches[8] = {}; int n = 0;
        int last = s.phase;
        for (unsigned long t = 0; t <= 80000; t += 50) {
            s.step(t);
            if (s.phase != last && n < 8) { switches[n++] = t; last = s.phase; }
            seen[s.phase == RF_PROTO_BRESSER ? 0 : s.phase == RF_PROTO_ITPLUS17 ? 1 : 2]++;
            if (s.phase == RF_PROTO_ITPLUS17) assert(s.foPhase());
        }
        assert(switches[0] == 17000 && switches[1] == 34000 && switches[2] == 39000 && switches[3] == 56000);
        assert(seen[0] && seen[1] && seen[2]);
    }

    // ---- every family is heard within one rotation (39 s), whatever its phase ----
    struct Fam { const char* name; int phase; unsigned long intervalMs, airMs; };
    const Fam fams[] = {
        { "Bresser 7-in-1 (16 s)", RF_PROTO_BRESSER, 16000, 60 },
        { "Bresser 5-in-1 (12 s)", RF_PROTO_BRESSER, 12000, 60 },
        { "Fine Offset WH65 (16 s)", RF_PROTO_ITPLUS17, 16000, 20 },
        { "Fine Offset WS90 (8.8 s)", RF_PROTO_ITPLUS17, 8800, 30 },
        { "IT+ 17.241 kbit/s (4 s)", RF_PROTO_ITPLUS17, 4000, 5 },
        { "IT+ 9.579 kbit/s (4 s)", RF_PROTO_ITPLUS9, 4000, 6 },
    };
    for (const Fam& f : fams) {
        unsigned long worst = 0;
        for (unsigned long off = 0; off < f.intervalMs; off += 250) {
            unsigned long c = firstCatch(f.phase, f.intervalMs, off, f.airMs);
            if (c > worst) worst = c;
        }
        printf("rf auto: %s heard after at most %.1f s\n", f.name, worst / 1000.0);
        assert(worst <= 39000 + 100);
    }

    // ---- lock decision: first valid packet from an accepted ID, only while searching ----
    assert(rfAutoShouldLock(RF_PROTO_AUTO, true));
    assert(!rfAutoShouldLock(RF_PROTO_AUTO, false));          // ID list set, other sensor
    assert(!rfAutoShouldLock(RF_PROTO_BRESSER, true) && !rfAutoShouldLock(10, true));   // already locked / manual type
    assert(rfAutoDetectedProto(RF_PROTO_BRESSER) == 1);
    assert(rfAutoDetectedProto(RF_PROTO_ITPLUS17) == 10 && rfAutoDetectedProto(RF_PROTO_FINEOFFSET) == 10);
    assert(rfAutoDetectedProto(RF_PROTO_ITPLUS9) == 4);
    assert(rfAutoDetectedProto(0) == 0 && rfAutoDetectedProto(RF_PROTO_AUTO) == 0);
    for (int p : { RF_PROTO_BRESSER, RF_PROTO_ITPLUS17, RF_PROTO_FINEOFFSET, RF_PROTO_ITPLUS9 }) {
        int type = rfAutoDetectedProto(p);
        assert(rfProtoValid(type) && !rfAutoShouldLock(type, true));   // stored type: no further lock
        // after the lock: one fixed phase, no alternating
        int set = rfPhaseSet(rfProtoSetFor(type));
        assert((set & (set - 1)) == 0);
        assert(set == (p == RF_PROTO_FINEOFFSET ? RF_PROTO_ITPLUS17 : p));
    }
    // ID list: rfParseIds() decides what counts as accepted
    std::vector<uint32_t> ids = rfParseIds("39582376, 30000301");
    assert(ids.size() == 2 && ids[1] == 0x30000301);
    return 0;
}
