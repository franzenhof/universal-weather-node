// Runs real firmware modules with deterministic hardware fakes. No flashing.
const {execFileSync} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const root = path.join(__dirname, '..');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'weathernode-runtime-'));
const common = ['src/sensors.cpp', 'src/board_io.cpp', 'src/vedirect.cpp', 'src/sources.cpp', 'src/rf_sensors_data.cpp', 'src/derived.cpp', 'src/derived_now.cpp', 'test/host/runtime.cpp'];
try {
  for (const [name, sources] of [
    ['sensors', common],
    ['io', [...common, 'src/params.cpp']],
    ['lora', ['src/lora_transport.cpp', 'test/host/runtime.cpp']],
    ['windows', ['src/wind_window.cpp']],
    ['derived', ['src/derived.cpp']],
    ['rainstats', ['src/rain_stats.cpp']],
    ['rfauto', [...common, 'src/params.cpp']],
    ['loxone', ['src/loxone_lines.cpp']],
    ['itplus', ['lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp']],
    ['fineoffset', ['lib/FineOffsetSensorReceiver/src/FineOffsetDecoder.cpp', 'lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp',
      'src/rf_sensors_data.cpp']],
    ['netids', []],
    ['otacheck', []],
    ['telemetry', [...common, 'src/json_payload.cpp', 'src/units.cpp', 'src/loxone_lines.cpp']],
    ['rf', common],
    ['history', [...common.filter((f) => f !== 'test/host/runtime.cpp'), 'src/units.cpp']],   // includes runtime.cpp + history.cpp
    ['payload', [...common, 'src/lora_payload.cpp', 'src/payload_binary.cpp', 'src/units.cpp',
      '.pio/libdeps/heltec_wifi_lora_32_V3/CayenneLPP/src/CayenneLPP.cpp',
      '.pio/libdeps/heltec_wifi_lora_32_V3/CayenneLPP/src/CayenneLPPPolyline.cpp']],
  ]) {
    const exe = path.join(tmp, name);
    execFileSync(process.env.CXX || 'c++', [
      '-std=c++11', '-Wall', '-Wextra', '-Werror',
      '-I', 'test/host', '-I', 'src', '-I', 'lib/TfaWeatherSensorReceiver/src', '-I', 'lib/FineOffsetSensorReceiver/src',
      '-I', '.pio/libdeps/heltec_wifi_lora_32_V3/ArduinoJson/src',
      '-I', '.pio/libdeps/heltec_wifi_lora_32_V3/CayenneLPP/src',
      ...sources, `test/firmware_${name}.cpp`, '-o', exe,
    ], {cwd: root, stdio: 'pipe'});
    execFileSync(exe, [], {stdio: 'inherit'});
    console.log(`Firmware ${name} regressions passed.`);
  }
} catch (error) {
  if (error.stderr) process.stderr.write(error.stderr);
  throw error;
} finally {
  fs.rmSync(tmp, {recursive: true, force: true});
}
