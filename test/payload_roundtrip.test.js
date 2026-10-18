// Encoder/decoder round trip for the binary format (FPort 3): compiles the
// firmware's src/payload_binary.cpp for the host, encodes known values and
// checks that chirpstack/decoder.js decodes them back. Needs a C++ compiler
// (c++/g++/clang++, CXX overrides); skipped with a message if none exists.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { execFileSync } = require('child_process');

const root = path.join(__dirname, '..');
const sandbox = { console };
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(root, 'chirpstack', 'decoder.js'), 'utf8'), sandbox);

const exe = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'payload-rt-')), 'roundtrip');
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

console.log(`Payload round trip passed (full binary payload: ${cases.metric.length} bytes).`);
