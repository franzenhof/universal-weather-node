#pragma once
#include <Wire.h>
// hostBmeOk = false (default): no sensor. true: readings hostBmeT/P(Pa)/H.
extern bool  hostBmeOk;
extern float hostBmeT, hostBmeP, hostBmeH;
// hostBmeSda >= 0: the sensor answers only on that SDA pin; hostBmeAddr >= 0: only at that address
extern int   hostBmeSda, hostBmeAddr;
struct Adafruit_BME280 {
    enum { MODE_FORCED, SAMPLING_X1, FILTER_OFF, STANDBY_MS_0_5 };
    bool begin(int a, TwoWire* w) { return hostBmeOk && (hostBmeSda < 0 || w->sda == hostBmeSda) && (hostBmeAddr < 0 || a == hostBmeAddr); }
    void setSampling(int,int,int,int,int,int) {}
    bool takeForcedMeasurement() { return hostBmeOk; }
    float readTemperature() { return hostBmeOk ? hostBmeT : NAN; }
    float readPressure() { return hostBmeOk ? hostBmeP : NAN; }
    float readHumidity() { return hostBmeOk ? hostBmeH : NAN; }
    float seaLevelForAltitude(int,float p) { return p; }
};
