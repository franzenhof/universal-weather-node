#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

class String;
using std::isfinite;
using std::isnan;
using std::min;
using std::max;
template<class T> T constrain(T v, T lo, T hi) { return min(max(v, lo), hi); }
constexpr int INPUT=0, OUTPUT=1, LOW=0, HIGH=1, SERIAL_8N1=0;
constexpr float DEG_TO_RAD=0.017453292519943295f, RAD_TO_DEG=57.29577951308232f;
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_BROWNOUT, ESP_RST_SW };
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
extern unsigned long hostNow;
extern int hostPins[64], hostModes[64], hostAdc[64], hostPwm[8], hostAttached[64];
inline unsigned long millis() { return hostNow; }
inline void delay(unsigned long ms) { hostNow += ms; }
inline void delayMicroseconds(unsigned) {}
inline void pinMode(int p, int m) { hostModes[p]=m; }
inline void digitalWrite(int p, int v) { hostPins[p]=v; }
inline int digitalRead(int p) { return hostPins[p]; }
inline int analogRead(int p) { return hostAdc[p]; }
inline uint32_t analogReadMilliVolts(int p) { return hostAdc[p]; }
inline void adcAttachPin(int) {}
inline void analogReadResolution(int) {}
inline int* hostPwmFrequencies() { static int frequencies[8]={}; return frequencies; }
inline void ledcSetup(int ch, int frequency, int) { hostPwmFrequencies()[ch]=frequency; }
inline void ledcAttachPin(int p, int ch) { hostAttached[p]=ch+1; }
inline void ledcDetachPin(int p) { hostAttached[p]=0; }
inline void ledcWrite(int ch, uint32_t v) { hostPwm[ch]=v; }
inline long map(long v,long a,long b,long c,long d) { return (v-a)*(d-c)/(b-a)+c; }
inline void setCpuFrequencyMhz(int) {}
struct HostESP {
    bool restarted=false;
    unsigned getFreeHeap() const { return 125000; }
    uint64_t getEfuseMac() const { return 0x563412CDAB24ULL; }   // MAC 24:AB:CD:12:34:56 (byte 0 in the lowest bits)
    void restart() { restarted=true; }
};
extern HostESP ESP;
struct HostSerial {
    std::string input;
    void setRxBufferSize(unsigned) {}
    void begin(int,int,int,int) {}
    void end() {}
    int available() { return input.size(); }
    int read() { unsigned char c=input.front(); input.erase(0,1); return c; }
};
extern HostSerial Serial1;
