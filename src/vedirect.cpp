// VE.Direct text protocol parser - see vedirect.h. Plain C++ on purpose.
#include "vedirect.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void VeDirectParser::reset()
{
    state = ST_IDLE;
    stateBeforeHex = ST_IDLE;
    sum = 0;
    nameLen = valueLen = 0;
    fieldCount = 0;
    okCount = errCount = 0;
    vals.battV = vals.battA = vals.pvV = vals.pvW = vals.yieldTodayKwh = vals.socPct = vals.loadA = NAN;
    vals.chargeState = vals.error = -1;
}

bool VeDirectParser::feed(uint8_t b)
{
    // HEX messages can start anywhere except as the checksum byte (which may
    // legitimately be ':') and are excluded from the checksum.
    if (b == ':' && state != ST_CHECKSUM && state != ST_HEX) {
        stateBeforeHex = state;
        state = ST_HEX;
    }
    if (state == ST_HEX) {
        if (b == '\n') state = stateBeforeHex;
        return false;
    }

    sum += b;
    switch (state) {
        case ST_IDLE:
            // Every field starts with "\r\n"; the label follows the '\n'.
            if (b == '\n') { state = ST_NAME; nameLen = 0; }
            break;
        case ST_NAME:
            if (b == '\t') {
                name[nameLen] = '\0';
                if (strcmp(name, "Checksum") == 0) {
                    state = ST_CHECKSUM;
                } else {
                    state = ST_VALUE;
                    valueLen = 0;
                }
            } else if (b == '\r' || b == '\n' || nameLen >= NAME_LEN - 1) {
                state = ST_IDLE;   // malformed line - the checksum will fail anyway
            } else {
                name[nameLen++] = (char)b;
            }
            break;
        case ST_VALUE:
            if (b == '\r') {
                value[valueLen] = '\0';
                storeField();
                state = ST_IDLE;
            } else if (valueLen < VALUE_LEN - 1) {
                value[valueLen++] = (char)b;
            }
            break;
        case ST_CHECKSUM: {
            // The checksum byte is already in `sum`.
            bool ok = (sum == 0);
            if (ok) { commit(); okCount++; }
            else    errCount++;
            sum = 0;
            fieldCount = 0;
            state = ST_IDLE;
            return ok;
        }
        case ST_HEX:
            break;
    }
    return false;
}

void VeDirectParser::storeField()
{
    if (fieldCount >= MAX_FIELDS) return;
    Field& f = fields[fieldCount++];
    memcpy(f.name, name, sizeof(f.name));
    memcpy(f.value, value, sizeof(f.value));
}

// Applies a checksum-verified block. Only numeric labels this node uses are
// read; everything else (PID, SER#, FW, LOAD, alarms, history) is ignored.
void VeDirectParser::commit()
{
    for (size_t i = 0; i < fieldCount; i++) {
        const char* n = fields[i].name;
        const char* v = fields[i].value;
        char* end = nullptr;
        long x = strtol(v, &end, 10);
        if (end == v || *end != '\0') continue;   // "---" (not available) or text
        if      (!strcmp(n, "V"))   vals.battV         = x / 1000.0f;
        else if (!strcmp(n, "I"))   vals.battA         = x / 1000.0f;
        else if (!strcmp(n, "VPV")) vals.pvV           = x / 1000.0f;
        else if (!strcmp(n, "PPV")) vals.pvW           = (float)x;
        else if (!strcmp(n, "H20")) vals.yieldTodayKwh = x / 100.0f;
        else if (!strcmp(n, "SOC")) vals.socPct        = x / 10.0f;
        else if (!strcmp(n, "IL"))  vals.loadA         = x / 1000.0f;
        else if (!strcmp(n, "CS"))  vals.chargeState   = (int)x;
        else if (!strcmp(n, "ERR")) vals.error         = (int)x;
    }
}
