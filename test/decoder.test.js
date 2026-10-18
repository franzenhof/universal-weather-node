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

function testBinaryUnknownVersion() {
  const d = decodeUplink({ fPort: 3, bytes: [0x09, 0, 0, 0, 0] }).data;
  assert.ok(typeof d.error === 'string' && d.error.indexOf('unsupported binary protocol version') !== -1,
    'binary unknown version rejected');
}

testLppMetric();
testLppImperialResolution();
testLppDebugChannels();
testBinaryVersion1StillDecoded();
testBinaryUnknownVersion();
console.log('All decoder tests passed.');
