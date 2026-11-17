#pragma once
#include <Wire.h>
// hostBmeOk = false (default): no sensor. true: readings hostBmeT/P(Pa)/H.
extern bool  hostBmeOk;
extern float hostBmeT, hostBmeP, hostBmeH;
struct Adafruit_BME280 {
    enum { MODE_FORCED, SAMPLING_X1, FILTER_OFF, STANDBY_MS_0_5 };
    bool begin(int,TwoWire*) { return hostBmeOk; }
    void setSampling(int,int,int,int,int,int) {}
    bool takeForcedMeasurement() { return hostBmeOk; }
    float readTemperature() { return hostBmeOk ? hostBmeT : NAN; }
    float readPressure() { return hostBmeOk ? hostBmeP : NAN; }
    float readHumidity() { return hostBmeOk ? hostBmeH : NAN; }
    float seaLevelForAltitude(int,float p) { return p; }
};
