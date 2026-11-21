// Decoder regression tests for chirpstack/decoder.js (no hardware needed):
//   node test/decoder.test.js
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const decoderPath = path.join(__dirname, '..', 'chirpstack', 'decoder.js');
const sandbox = { console };
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(decoderPath, 'utf8'), sandbox, { filename: decoderPath });
const { Decode, decodeUplink } = sandbox;

function eq(actual, expected, message) {
  assert.strictEqual(actual, expected, `${message}: got ${actual}, expected ${expected}`);
}

function testLppMetric() {
  const bytes = [
    1, 132, 0x01, 0x0E,                // ch1 direction 270°
    1, 2, 0x02, 0x9A,                  // ch1 wind 6.66
    3, 2, 0x00, 0x50,                  // ch3 rain rate 0.80
    1, 130, 0x00, 0x00, 0x00, 0xC8,    // ch1 distance 0.200 mm
    2, 130, 0x00, 0x05, 0x0D, 0xD0,    // ch2 distance 331.216 mm
    2, 0, 0x83,                        // status: BME, bus1, wind m/s
    1, 115, 0x27, 0xA7,                // ch1 baro 1015.1 hPa
    1, 103, 0x00, 0x7B                 // ch1 temp 12.3
  ];
  const d = Decode(1, bytes);
  eq(d.wind_direction_deg, 270, 'LPP direction');
  eq(d.wind_speed_avg, 6.66, 'LPP wind');
  eq(d.rain_rate_mmh, 0.8, 'LPP rain rate');
  eq(d.rain_cycle_mm, 0.2, 'LPP rain cycle');
  eq(d.rain_since_start_mm, 331.216, 'LPP rain since start keeps 3 decimals');
  eq(d.status.wind_speed_unit, 'm/s', 'LPP status wind unit');
  eq(d.bme280_pressure_qnh_hpa, 1015.1, 'LPP pressure metric');
  eq(d.bme280_temperature_c, 12.3, 'LPP temp');
}

function testLppImperialResolution() {
  const bytes = [
    1, 130, 0x00, 0x00, 0x00, 0x08,    // ch1 distance 0.008 in (one 0.2 mm tip)
    2, 0, 0x41,                        // status: BME, imperial
    1, 115, 0x0B, 0xBD                 // ch1 baro raw 3005 -> 30.05 inHg
  ];
  const d = Decode(1, bytes);
  eq(d.rain_cycle_mm, 0.008, 'LPP imperial single tip not rounded to 0');
  eq(d.status.pressure_unit, 'inHg', 'LPP imperial unit');
  eq(d.bme280_pressure_qnh_hpa, 30.05, 'LPP imperial pressure 0.01 inHg');
}

function testLppDebugChannels() {
  const wide = [
    203, 100, 0x00, 0x00, 0x00, 0x79,  // ch203 generic: 121 KiB
    204, 100, 0x00, 0x00, 0x30, 0x39   // ch204 generic: 12345 -> 1234.5 h
  ];
  const d = Decode(1, wide);
  eq(d.debug_free_heap_kib, 121, 'LPP generic heap');
  eq(d.debug_uptime_h, 1234.5, 'LPP generic uptime > 327 h');

  const legacy = [203, 2, 0x2F, 0x80];  // older firmware: analog 121.60 KiB
  eq(Decode(1, legacy).debug_free_heap_kib, 121.6, 'LPP legacy analog heap');
}

function testLppIoBlock() {
  const bytes = [
    3, 0, 0x03,                        // io status: heater + fan on
    5, 2, 0x01, 0xF4,                  // ch5 AUX analog 5.00 V
    10, 100, 0x00, 0x00, 0x00, 0x07,   // ch10 AUX pulses 7
    11, 100, 0x00, 0x01, 0x00, 0x00,   // ch11 AUX total 65536
    20, 1, 0x32,                       // ch20 heater 50 %
    21, 1, 0x64,                       // ch21 fan 100 %
    22, 100, 0x00, 0x00, 0x0B, 0xB8,   // ch22 fan 3000 rpm
    1, 116, 0x01, 0x8F,                // ch1 voltage: LiPo 3.99 V
    30, 116, 0x05, 0x2D,               // ch30 voltage: Victron battery 13.25 V
    30, 2, 0xFE, 0x0C,                 // ch30 analog: battery -5.00 A
    31, 116, 0x0E, 0xD8,               // ch31 voltage: PV 38.00 V
    30, 128, 0x00, 0x2A,               // ch30 power: PV 42 W
    30, 131, 0x00, 0x00, 0x00, 0x7B,   // ch30 energy: 0.123 kWh
    30, 120, 0x57,                     // ch30 percentage: SOC 87 %
    30, 0, 0x05,                       // ch30 digital in: CS float
    31, 0, 0x00,                       // ch31 digital in: no error
    2, 132, 0x00, 0x5A,                // ch2 direction: wind 2 at 90 deg
    6, 2, 0x01, 0xC8,                  // ch6 analog: wind 2 speed 4.56
    7, 2, 0x03, 0xDB                   // ch7 analog: wind 2 gust 9.87
  ];
  const d = Decode(1, bytes);
  eq(d.io_status.heater_on, true, 'LPP io heater on');
  eq(d.io_status.fan_stalled, false, 'LPP io fan stalled');
  eq(d.aux_analog_v, 5, 'LPP AUX analog');
  eq(d.aux_pulses, 7, 'LPP AUX pulses');
  eq(d.wind2_direction_deg, 90, 'LPP wind 2 direction');
  eq(d.wind2_speed_avg, 4.56, 'LPP wind 2 speed');
  eq(d.wind2_gust, 9.87, 'LPP wind 2 gust');
  eq(d.aux_pulses_total, 65536, 'LPP AUX total');
  eq(d.heater_duty_pct, 50, 'LPP heater duty');
  eq(d.fan_duty_pct, 100, 'LPP fan duty');
  eq(d.fan_rpm, 3000, 'LPP fan rpm');
  eq(d.battery_v, 3.99, 'LPP LiPo voltage stays on ch1');
  eq(d.victron_battery_v, 13.25, 'LPP Victron battery V');
  eq(d.victron_battery_a, -5, 'LPP Victron battery A');
  eq(d.victron_pv_v, 38, 'LPP Victron PV V');
  eq(d.victron_pv_w, 42, 'LPP Victron PV W');
  eq(d.victron_yield_today_kwh, 0.123, 'LPP Victron yield');
  eq(d.victron_soc_pct, 87, 'LPP Victron SOC');
  eq(d.victron_charge_state_text, 'float', 'LPP Victron CS');
  eq(d.victron_error, 0, 'LPP Victron error');
}

function testUvLppChannels() {
  const d = Decode(1, [
    12, 116, 0x01, 0xF4,  // 5.00 V
    13, 2, 0x04, 0xD2,   // index 12.34
    14, 0, 0x03          // enabled and calibrated
  ]);
  eq(d.uv_sensor_voltage_v, 5, 'LPP UV voltage');
  eq(d.uv_sensor_voltage_unit, 'V', 'LPP UV voltage unit');
  eq(d.uv_index, 12.34, 'LPP UV index');
  eq(d.uv_index_unit, 'index', 'LPP UV index unit');
  eq(d.uv_sensor_enabled, true, 'LPP UV enabled flag');
  eq(d.uv_index_calibrated, true, 'LPP UV calibrated flag');
}

function testBinaryUvExtensionAndTruncation() {
  const bytes = [3, 0, 0, 0, 0x80, 0x13, 0x87, 0x09, 0xF6, 0x03];
  const d = Decode(3, bytes);
  eq(d.proto_version, 3, 'binary v3 parsed');
  eq(d.uv_sensor_voltage_v, 4.999, 'binary UV voltage');
  eq(d.uv_sensor_voltage_unit, 'V', 'binary UV voltage unit');
  eq(d.uv_index, 25.5, 'binary UV index');
  eq(d.uv_index_unit, 'index', 'binary UV index unit');
  eq(d.uv_index_calibrated, true, 'binary UV calibration flag');
  const truncated = Decode(3, bytes.slice(0, -1));
  assert.ok(typeof truncated.error === 'string' && truncated.error.indexOf('truncated binary UV') !== -1,
    'truncated UV extension rejected');
}

function testBinaryVersion1StillDecoded() {
  const bytes = [
    0x01,
    0x30, 0x00, 0x00, 0x00,            // bits 4,5 (rain cycle, rain since)
    0x00, 0x02,                        // rain cycle 0.2 mm (x10)
    0x0C, 0x80                         // rain since 320.0 mm (uint16 x10)
  ];
  const d = Decode(3, bytes);
  eq(d.proto_version, 1, 'binary v1 version');
  eq(d.rain_cycle_mm, 0.2, 'binary v1 rain cycle');
  eq(d.rain_since_start_mm, 320, 'binary v1 rain since');
}

function testBinaryV2VictronBlockStillDecoded() {
  const bytes = [0x02, 0x00, 0x00, 0x00, 0x04,         // bit 26: 14-byte VE.Direct block (v1-v3)
    0x05, 0x30, 0xFF, 0x85, 0x0E, 0xA6, 0x00, 0x2D, 0x03, 0x00, 0x00, 0x0C, 0xFF, 0xFF];
  const d = Decode(3, bytes);
  eq(d.proto_version, 2, 'binary v2 version');
  eq(d.victron_battery_v, 13.28, 'v2 Victron battery V');
  eq(d.victron_battery_a, -1.23, 'v2 Victron battery A');
  eq(d.victron_pv_v, 37.5, 'v2 Victron PV V');
  eq(d.victron_pv_w, 45, 'v2 Victron PV W');
  eq(d.victron_charge_state_text, 'bulk', 'v2 Victron CS');
  eq(d.victron_yield_today_kwh, 0.12, 'v2 Victron yield');
  eq(d.victron_soc_pct, undefined, 'v2 Victron SOC absent');
}

function testLppLightAndSources() {
  const d = Decode(1, [
    15, 100, 0x00, 0x01, 0x81, 0xCD,   // generic sensor: 98765 lx
    16, 100, 0x00, 0x00, 0x82, 0x9A    // sources: tempHum wireless, wind wireless, rain board, UV wireless, light wireless, battery low
  ]);
  eq(d.light_lux, 98765, 'LPP illuminance');
  eq(d.sources.temp_humidity, 'wireless', 'LPP source temp/hum');
  eq(d.sources.rain, 'board', 'LPP source rain');
  eq(d.sources.light, 'wireless', 'LPP source light');
  eq(d.wireless_battery_low, true, 'LPP wireless battery low');
  eq(d.sources.pressure, 'none', 'LPP pressure bits 10-11 unset (older firmware)');
  // pressure source (bits 10-11): board 0x0400, wireless 0x0800; the other groups unchanged
  const b = Decode(1, [16, 100, 0x00, 0x00, 0x04, 0x1A]);   // pressure board, tempHum wireless, wind wireless, rain board
  eq(b.sources.pressure, 'board', 'LPP source pressure board');
  eq(b.sources.temp_humidity, 'wireless', 'LPP source temp/hum with pressure bits');
  eq(b.sources.rain, 'board', 'LPP source rain with pressure bits');
  eq(b.wireless_battery_low, false, 'LPP no battery low');
  const w = Decode(1, [16, 100, 0x00, 0x00, 0x88, 0x00]);   // pressure wireless + battery low
  eq(w.sources.pressure, 'wireless', 'LPP source pressure wireless');
  eq(w.sources.temp_humidity, 'none', 'LPP source temp/hum none');
  eq(w.wireless_battery_low, true, 'LPP battery low with pressure bits');
}

function testBinaryUnknownVersion() {
  const r = decodeUplink({ fPort: 3, bytes: [0x09, 0, 0, 0, 0] });
  // ChirpStack v4: errors[] carries the message, data stays empty
  eq(Object.keys(r.data).length, 0, 'binary unknown version: no data');
  assert.ok(r.errors.length === 1 && r.errors[0].indexOf('unsupported binary protocol version') !== -1,
    'binary unknown version rejected');
  // v3 entry point unchanged: { error }
  const d = Decode(3, [0x09, 0, 0, 0, 0]);
  assert.ok(typeof d.error === 'string' && d.error.indexOf('unsupported binary protocol version') !== -1,
    'v3 binary unknown version rejected');
}

function testLppTruncated() {
  const full = [1, 132, 0x01, 0x0E, 1, 2, 0x02, 0x9A, 2, 0, 0x83];
  eq(decodeUplink({ fPort: 1, bytes: full }).errors.length, 0, 'complete LPP payload');
  // cut inside a field's data, and after a lone channel byte
  for (const cut of [full.length - 1, 6, 9]) {
    const r = decodeUplink({ fPort: 1, bytes: full.slice(0, cut) });
    eq(Object.keys(r.data).length, 0, 'truncated LPP (' + cut + ' bytes): no data');
    assert.ok(r.errors.length === 1 && r.errors[0].indexOf('truncated CayenneLPP') !== -1,
      'truncated LPP (' + cut + ' bytes) rejected');
    const d = Decode(1, full.slice(0, cut));
    assert.ok(typeof d.error === 'string' && d.error.indexOf('truncated CayenneLPP') !== -1,
      'v3: truncated LPP (' + cut + ' bytes) rejected');
  }
  // a generic sensor (4 bytes) with only 3 data bytes
  const g = decodeUplink({ fPort: 1, bytes: [10, 100, 0x00, 0x01, 0x02] });
  assert.ok(g.errors.length === 1, 'truncated LPP generic sensor rejected');
}

function testBinaryTruncatedV4() {
  // wind dir + speed announced (bits 0, 1), speed missing
  const r = decodeUplink({ fPort: 3, bytes: [4, 0x03, 0x01, 0x0E] });
  assert.ok(r.errors.length === 1 && r.errors[0].indexOf('truncated binary payload') !== -1,
    'truncated v4 binary payload rejected');
  eq(Object.keys(r.data).length, 0, 'truncated v4 binary payload: no data');
}

function testDerivedValues() {
  // v4 uplink from the field (Rev 1.2): 1.4 °C, 89 %, wind 47° 5.79 km/h
  const hex = '04ff9e808020002f0243028c000000000000001601b2278524f7000e0015';
  const d = decodeUplink({ fPort: 3, bytes: [...Buffer.from(hex, 'hex')] }).data;
  eq(d.wind_direction_text, 'NE', 'derived compass text');
  eq(d.dew_point, -0.2, 'derived dew point');
  eq(d.feels_like, -0.3, 'derived wind chill');
  eq(d.cloud_base_m, 202, 'derived cloud base');
  eq(d.warnings.frost, false, 'derived frost warning');
  eq(d.warnings.fog, false, 'derived fog warning');
}

testLppMetric();
testLppImperialResolution();
testLppDebugChannels();
testLppIoBlock();
testUvLppChannels();
testBinaryUvExtensionAndTruncation();
testBinaryVersion1StillDecoded();
testBinaryV2VictronBlockStillDecoded();
testLppLightAndSources();
testBinaryUnknownVersion();
testLppTruncated();
testBinaryTruncatedV4();
testDerivedValues();
console.log('All decoder tests passed.');
