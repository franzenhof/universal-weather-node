// Victron VE.Direct text protocol parser (19200 Bd, 8N1). Plain C++ (no
// Arduino), so it can be tested on the host (test/vedirect.test.js).
//
// The device sends a block of "\r\n<label>\t<value>" lines about once per
// second, closed by "\r\nChecksum\t<byte>": the sum of all bytes of the block
// incl. that byte is 0 (mod 256). Asynchronous HEX-protocol messages
// (":...\n") may be interleaved and are not part of the checksum. Only blocks
// with a valid checksum update the values; a BMV sends two blocks per second
// with different labels, so each block only updates the labels it contains.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct VeDirectValues {
    // NAN / -1 = not sent by this device (e.g. no PV fields on a BMV, no SOC
    // on an MPPT charger).
    float battV;        // V      (label V,   mV)
    float battA;        // A      (label I,   mA; negative = discharge)
    float pvV;          // V      (label VPV, mV)
    float pvW;          // W      (label PPV)
    float yieldTodayKwh;// kWh    (label H20, 0.01 kWh)
    float socPct;       // %      (label SOC, 0.1 %; BMV/SmartShunt)
    float loadA;        // A      (label IL, mA; MPPT load output current)
    int   chargeState;  // label CS: 0 off, 2 fault, 3 bulk, 4 absorption, 5 float, ...
    int   error;        // label ERR: 0 = no error
};

class VeDirectParser {
public:
    VeDirectParser() { reset(); }

    void reset();
    // Feeds one received byte; returns true when it completed a valid block.
    bool feed(uint8_t b);

    const VeDirectValues& values() const { return vals; }
    uint32_t validBlocks() const { return okCount; }
    uint32_t checksumErrors() const { return errCount; }

private:
    enum State { ST_IDLE, ST_NAME, ST_VALUE, ST_CHECKSUM, ST_HEX };
    static constexpr size_t NAME_LEN = 10, VALUE_LEN = 34, MAX_FIELDS = 24;

    struct Field { char name[NAME_LEN]; char value[VALUE_LEN]; };

    void storeField();
    void commit();

    State   state, stateBeforeHex;
    uint8_t sum;
    char    name[NAME_LEN];
    char    value[VALUE_LEN];
    size_t  nameLen, valueLen;
    Field   fields[MAX_FIELDS];
    size_t  fieldCount;
    VeDirectValues vals;
    uint32_t okCount, errCount;
};
