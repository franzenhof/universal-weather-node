#pragma once
#include <Wire.h>
struct Adafruit_BME280 {
    enum { MODE_FORCED, SAMPLING_X1, FILTER_OFF, STANDBY_MS_0_5 };
    bool begin(int,TwoWire*) { return false; }
    void setSampling(int,int,int,int,int,int) {}
    bool takeForcedMeasurement() { return false; }
    float readTemperature() { return NAN; }
    float readPressure() { return NAN; }
    float readHumidity() { return NAN; }
    float seaLevelForAltitude(int,float p) { return p; }
};
