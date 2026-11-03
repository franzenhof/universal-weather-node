// Encoder/decoder round trip for the binary format (FPort 3): compiles the
// firmware's src/payload_binary.cpp for the host, encodes known values and
// checks that chirpstack/decoder.js decodes them back. Needs a C++ compiler
// (c++/g++/clang++, CXX overrides); skipped with a message if none exists.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { execFileSync } = require('child_process');

const root = path.join(__dirname, '..');
const sandbox = { console };
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(root, 'chirpstack', 'decoder.js'), 'utf8'), sandbox);

const exe = path.join(root, '.payload-roundtrip-test');
process.on('exit', () => { try { fs.unlinkSync(exe); } catch (_) {} });
let compiled = false;
for (const cxx of [process.env.CXX, 'c++', 'g++', 'clang++'].filter(Boolean)) {
  try {
    execFileSync(cxx, ['-std=c++11', '-Wall', '-Werror', '-I', path.join(root, 'src'),
      path.join(root, 'src', 'payload_binary.cpp'), path.join(__dirname, 'payload_roundtrip.cpp'),
      '-o', exe], { stdio: 'pipe' });
    compiled = true;
    break;
  } catch (e) {
    if (e.code !== 'ENOENT') throw new Error(`${cxx} failed:\n${e.stderr}`);
  }
}
if (!compiled) {
  console.log('SKIPPED payload round trip: no C++ compiler found.');
  process.exit(0);
}

const cases = {};
for (const c of JSON.parse(execFileSync(exe, { encoding: 'utf8' }))) cases[c.name] = c.bytes;
const decode = (name) => sandbox.decodeUplink({ fPort: 3, bytes: cases[name] }).data;

function eq(actual, expected, message) {
  assert.strictEqual(actual, expected, `${message}: got ${actual}, expected ${expected}`);
}

for (const [name, imp] of [['metric', false], ['imperial', true]]) {
  const d = decode(name);
  eq(d.proto_version, 2, `${name} version`);
  eq(d.wind_direction_deg, 338, `${name} wind dir`);
  eq(d.wind_speed_avg, 7, `${name} wind speed`);
  eq(d.wind_gust, 10.42, `${name} wind gust`);
  eq(d.rain_rate_mmh, 2.4, `${name} rain rate`);
  eq(d.rain_cycle_mm, imp ? 0.008 : 0.2, `${name} rain cycle (single tip)`);
  eq(d.rain_since_start_mm, imp ? 315.354 : 8010, `${name} rain since start (beyond uint16)`);
  eq(d.status.rain_unit, imp ? 'in' : 'mm', `${name} unit`);
  eq(d.battery_v, 3.97, `${name} battery`);
  eq(d.bme280_humidity_pct, 49.5, `${name} humidity`);
  eq(d.bme280_pressure_qnh_hpa, imp ? 29.99 : 1015.8, `${name} QNH`);
  eq(d.bme280_pressure_abs_hpa, imp ? 28.06 : 950.1, `${name} abs pressure`);
  eq(d.bme280_temperature_c, imp ? 82.9 : 28.3, `${name} temp`);
  eq(d.ds18b20_0_c, 10.5, `${name} ds18b20 0`);
  eq(d.ds18b20_2_c, 12.5, `${name} ds18b20 2`);
  eq(d.debug_cycle_counter, 42, `${name} debug cycle`);
  eq(d.debug_cpu_temperature_c, 41.2, `${name} debug cpu temp`);
  eq(d.debug_send_fail_count, 1, `${name} debug fail`);
  eq(d.debug_free_heap_kib, 122, `${name} debug heap`);
  eq(d.debug_uptime_h, 1234.6, `${name} debug uptime`);
}

eq(cases.minimal.length, 6, 'minimal size (header + status)');
eq(decode('minimal').status.bme280_present, false, 'minimal status');

const clamp = decode('clamp');
eq(clamp.wind_speed_avg, 327.67, 'wind clamps at int16 max');
eq(clamp.wind_gust, -327.68, 'gust clamps at int16 min');
eq(clamp.rain_cycle_mm, 0, 'unsigned rain clamps at 0');

const io = decode('io');
eq(io.io_status.heater_on, true, 'io heater on');
eq(io.io_status.fan_stalled, true, 'io fan stalled');
eq(io.io_status.fan_on, false, 'io fan bit');
eq(io.aux_analog_v, 4.321, 'io AUX analog');
eq(io.aux_pulses, 65535, 'io AUX pulses clamp at uint16');
eq(io.aux_pulses_total, 123456789, 'io AUX total');
eq(io.heater_duty_pct, 60, 'io heater duty');
eq(io.fan_duty_pct, 100, 'io fan duty');
eq(io.fan_rpm, 2950, 'io fan rpm');
eq(io.victron_battery_v, 13.28, 'io Victron battery V');
eq(io.victron_battery_a, -1.23, 'io Victron battery A');
eq(io.victron_pv_v, 37.5, 'io Victron PV V');
eq(io.victron_pv_w, 45, 'io Victron PV W');
eq(io.victron_charge_state_text, 'bulk', 'io Victron CS');
eq(io.victron_error, 0, 'io Victron error');
eq(io.victron_yield_today_kwh, 0.12, 'io Victron yield');
eq(io.victron_soc_pct, undefined, 'io Victron SOC not sent by an MPPT');
eq(io.victron_load_a, 0.31, 'io Victron load current');
eq(io.proto_version, 2, 'io keeps protocol version 2');
eq(io.wind2_direction_deg, 271, 'io wind 2 direction');
eq(io.wind2_speed_avg, 4.56, 'io wind 2 speed');
eq(io.wind2_gust, 9.87, 'io wind 2 gust');

const uv = decode('uv');
eq(uv.proto_version, 3, 'UV selects binary v3');
eq(uv.uv_sensor_voltage_v, 4.999, 'UV sensor voltage millivolt precision');
eq(uv.uv_index, 327.67, 'binary UV index clamps to signed centi-unit ceiling');
eq(uv.uv_index_calibrated, true, 'UV calibration flag');
const uvUncalibrated = decode('uv_uncalibrated');
eq(uvUncalibrated.uv_sensor_voltage_v, 3.3, 'uncalibrated UV voltage remains available');
eq(uvUncalibrated.uv_index, undefined, 'uncalibrated UV index omitted');
eq(uvUncalibrated.uv_index_calibrated, false, 'uncalibrated UV flag');
eq(uvUncalibrated.proto_version, 3, 'uncalibrated UV uses extension version');
const uvOutOfRange = decode('uv_out_of_range');
eq(uvOutOfRange.proto_version, 2, 'invalid UV reading does not emit a v3 field');
eq(uvOutOfRange.uv_sensor_voltage_v, undefined, 'out-of-range UV reading omitted');

// Fitting into the data-rate limit: least important groups go first.
eq(cases.all.length > 51, true, 'everything incl. v2.0 block exceeds 51 bytes');
eq(cases.all.length <= 85, true, 'everything incl. UV fits the encoder buffer');
const f51 = decode('fit51');
eq(cases.fit51.length <= 51, true, `fit51 within 51 bytes (got ${cases.fit51.length})`);
eq(f51.debug_cycle_counter, undefined, 'fit51 drops debug first');
eq(f51.aux_pulses, undefined, 'fit51 drops AUX pulses');
eq(f51.uv_sensor_voltage_v, undefined, 'fit51 sheds UV group when needed');
eq(f51.proto_version, 2, 'fitted packet returns to legacy format when UV is shed');
eq(f51.wind2_speed_avg, undefined, 'fit51 drops wind 2 before Victron');
eq(f51.wind_speed_avg, 7, 'fit51 keeps wind');
eq(f51.rain_rate_mmh, 2.4, 'fit51 keeps rain');
eq(f51.bme280_temperature_c, 28.3, 'fit51 keeps the BME280');
eq(f51.status.bme280_present, true, 'fit51 keeps the status byte');
const f11 = decode('fit11');
eq(cases.fit11.length <= 11, true, `fit11 within 11 bytes (got ${cases.fit11.length})`);
eq(f11.status.bme280_present, true, 'fit11 keeps at least the status byte');

console.log(`Payload round trip passed (full binary payload: ${cases.metric.length} bytes).`);
