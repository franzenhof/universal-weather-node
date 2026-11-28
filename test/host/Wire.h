#pragma once
// hostWireSda: SDA pin of the last begin() (the BME280 fake answers only there when hostBmeSda is set)
struct TwoWire {
    int sda = -1, scl = -1;
    explicit TwoWire(int) {}
    void begin(int a, int b, int) { sda = a; scl = b; }
    void end() { sda = scl = -1; }
    void setTimeOut(int) {}
};
