#pragma once
#include <OneWire.h>
constexpr float DEVICE_DISCONNECTED_C=-127.0f;
extern int hostProbeCount;
extern bool hostConversionComplete;
extern float hostProbeTemps[3];
struct DallasTemperature {
    explicit DallasTemperature(OneWire*) {}
    void begin() {}
    int getDeviceCount() { return hostProbeCount; }
    void setWaitForConversion(bool) {}
    void requestTemperatures() {}
    bool isConversionComplete() { return hostConversionComplete; }
    float getTempCByIndex(int i) { return hostProbeTemps[i]; }
};
