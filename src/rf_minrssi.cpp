#include "rf_minrssi.h"
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

bool rfMinRssiParse(const char* text, int& dbm)
{
    dbm = 0;
    while (text && isspace((unsigned char)*text)) text++;
    if (!text || !*text) return true;
    char* end = nullptr;
    const long v = strtol(text, &end, 10);
    if (end == text) return false;
    while (isspace((unsigned char)*end)) end++;
    if (strncasecmp(end, "dBm", 3) == 0) end += 3;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    if (v == 0) return true;   // 0 = none
    if (v < RF_MINRSSI_LO || v > RF_MINRSSI_HI) return false;
    dbm = (int)v;
    return true;
}

int rfMinRssiLookup(const char* list, uint32_t id)
{
    for (const char* p = list; p && *p;) {
        char* end = nullptr;
        const unsigned long v = strtoul(p, &end, 16);
        const char* sep = strchr(p, '|');
        const char* stop = sep ? sep : p + strlen(p);
        if (end != p && *end == ':' && end < stop && (uint32_t)v == id) {
            char num[12];
            size_t n = (size_t)(stop - end - 1);
            if (n >= sizeof(num)) return 0;
            memcpy(num, end + 1, n);
            num[n] = '\0';
            int d;
            return rfMinRssiParse(num, d) ? d : 0;
        }
        p = sep ? sep + 1 : stop;
    }
    return 0;
}

bool rfMinRssiSet(char* list, size_t len, uint32_t id, int dbm)
{
    if (dbm != 0 && (dbm < RF_MINRSSI_LO || dbm > RF_MINRSSI_HI)) return false;
    char* out = (char*)malloc(len);
    if (!out) return false;
    size_t n = 0;
    out[0] = '\0';
    bool ok = true;
    for (const char* p = list; *p;) {   // keep the valid entries of the other IDs
        const char* sep = strchr(p, '|');
        const char* stop = sep ? sep : p + strlen(p);
        char* end = nullptr;
        const unsigned long v = strtoul(p, &end, 16);
        if (end != p && *end == ':' && end < stop && (uint32_t)v != id && stop > end + 1) {
            const size_t k = (size_t)(stop - p);
            if (n + k + 2 > len) { ok = false; break; }
            if (n) out[n++] = '|';
            memcpy(out + n, p, k);
            n += k;
            out[n] = '\0';
        }
        p = sep ? sep + 1 : stop;
    }
    if (ok && dbm) {
        const int k = snprintf(out + n, len - n, "%s%lX:%d", n ? "|" : "", (unsigned long)id, dbm);
        if (k < 0 || (size_t)k >= len - n) ok = false;
    }
    if (ok) memcpy(list, out, strlen(out) + 1);
    free(out);
    return ok;
}

int rfMinRssiOf(uint32_t id)
{
    return gCfg.rfMinRssi[0] ? rfMinRssiLookup(gCfg.rfMinRssi, id) : 0;
}
