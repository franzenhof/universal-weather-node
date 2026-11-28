#include "http_util.h"
#include <stdlib.h>
#include <strings.h>

bool httpAcceptsGzip(const char* ae)
{
    for (const char* p = ae ? ae : ""; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char* name = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') p++;
        const bool gzip = p - name == 4 && !strncasecmp(name, "gzip", 4);
        double q = 1.0;
        while (*p && *p != ',') {   // parameters: ;q=0.5
            while (*p == ' ' || *p == '\t' || *p == ';') p++;
            if ((*p == 'q' || *p == 'Q') && p[1] == '=') q = strtod(p + 2, nullptr);
            while (*p && *p != ',' && *p != ';') p++;
        }
        if (gzip) return q > 0.0;
    }
    return false;
}
