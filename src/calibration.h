#pragma once
// Calibration of the main values (temperature, humidity, station pressure,
// rain, wind speed) - applied after the value source was chosen, so it works
// for the board sensors and wireless sensors alike (sources.cpp, main.cpp),
// and per wireless sensor (temperature / humidity offsets by sensor ID,
// applied when a packet is stored: rfSensors[], <prefix>/rf/<ID>, Home
// Assistant per sensor and the value sources all see the corrected value;
// the main offsets then come on top). Settings: Config::cal*, rfCal;
// parameters 59-63. Host-tested (test/firmware_calibration.cpp).
#include <stddef.h>
#include <stdint.h>
#include "wind_window.h"

float calTemp(float c);                  // + calTempDc / 10
float calHumidity(float pct);            // + calHumDpct / 10, kept within 0..100
float calStationPressure(float hpa);     // + calPressDhpa / 10 (before the QNH reduction)
float calRain(float mm);                 // x calRainPermil / 1000 (every rain increment)
WindStats calWind(WindStats w);          // mean and gust x calWindPermil / 1000

// Per wireless sensor: list "ID:t:h[:rain:wind:uv:lux[:flags]],..." (hex ID,
// offsets in 0.1 K and 0.1 %, gains in thousandths, flags bit 0 = WN20
// preset; entries without gains = 1.000). T / H offsets and the wind / UV /
// light gains apply when a packet is stored (rfSensors, MQTT, HA and the
// value sources see corrected values); the rain gain applies to the counter's
// increments (main rain path and per-sensor rain today / 1 h), never to the
// raw counter shown. The WN20 preset (Ecowitt wiki correction) replaces the
// plain rain gain: 0.908 up to 5 mm/h, 0.773 above (measured up to 10 mm/h).
struct RfCal {
    int      t10 = 0, h10 = 0;
    uint16_t rain = 1000, wind = 1000, uv = 1000, lux = 1000;
    bool     wn20 = false;   // flags bit 0: WN20 preset (Ecowitt wiki)
    bool     wh40 = false;   // flags bit 1: WH40 preset x1.016 (0.1016 mm tips reported as 0.1 mm)
    uint16_t ad0 = 0, ad100 = 0;   // soil moisture two-point calibration (raw AD at 0 % / 100 %), ad0 == ad100 = off
};
// Soil moisture % from the raw AD with the two-point calibration (clamped 0-100); false = not calibrated
bool rfCalMoisture(const RfCal& c, uint16_t ad, uint8_t& pct);
bool rfCalGet(const char* list, uint32_t id, RfCal& out);   // false = no entry (defaults)
bool rfCalLookup(const char* list, uint32_t id, int& t10, int& h10);
// Settings text "ID:temp:hum[:rain:wind:uv:lux[:wn20|:wh40][:ad=A0-A100]], ..."
// (hex ID, offsets in K and %, gains 0.5-2, hum and the gains optional,
// -10..10 K / -20..20 %, at most RF_CAL_MAX entries) -> list; false =
// invalid (out unchanged).
constexpr int RF_CAL_MAX = 32;
constexpr size_t RF_CAL_LIST_LEN = 1024;   // = sizeof(Config::rfCal)
bool rfCalParseText(const char* in, char* out, size_t len);
// list -> "A1B2C3D4:+0.5:-2.0[:1.000:1.050:1.000:1.000[:wn20]], ..." for the settings field
void rfCalFormatText(const char* list, char* out, size_t len);
// The gCfg.rfCal offsets of id, in K / %
void rfCalOffsets(uint32_t id, float& dT, float& dH);
float rfWn20Gain(float rateMmH);                  // the WN20 preset's factor at that rain rate
float rfRainGain(const RfCal& c, float rateMmH);  // the factor for a rain increment
