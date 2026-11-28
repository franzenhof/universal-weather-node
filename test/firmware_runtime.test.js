// Runs real firmware modules with deterministic hardware fakes. No flashing.
const {execFileSync} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const root = path.join(__dirname, '..');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'weathernode-runtime-'));
const common = ['src/sensors.cpp', 'src/board_io.cpp', 'src/vedirect.cpp', 'src/sources.cpp', 'src/rf_sensors_data.cpp', 'src/rf_schedule.cpp', 'src/reception_window.cpp', 'src/derived.cpp', 'src/derived_now.cpp', 'src/calibration.cpp', 'src/rf_stats.cpp', 'src/rf_names.cpp', 'src/rf_minrssi.cpp',
  'src/lsensors.cpp', 'src/series_def.cpp', 'src/series_store.cpp', 'src/placeholders.cpp', 'test/host/runtime.cpp'];
try {
  // generated translation headers (include/i18n_*.h, also done by every PlatformIO build)
  execFileSync(process.env.PYTHON || 'python3', ['-B', 'tools/i18n_gen.py', root], {cwd: root, stdio: 'pipe'});
  for (const [name, sources] of [
    ['sensors', common],
    ['io', [...common, 'src/params.cpp']],
    ['lora', ['src/lora_transport.cpp', 'src/out_status.cpp', 'test/host/runtime.cpp']],
    ['windows', ['src/wind_window.cpp']],
    ['derived', ['src/derived.cpp']],
    ['rainstats', ['src/rain_stats.cpp']],
    ['timesource', ['src/time_source.cpp']],
    ['gnss', ['src/nmea.cpp', 'src/time_source.cpp']],
    ['qr', ['src/qr_code.cpp', 'src/wifi_ap_logic.cpp', 'src/time_source.cpp']],
    ['wifiap', ['src/wifi_ap_logic.cpp', 'src/qr_code.cpp', 'src/time_source.cpp']],
    ['rfauto', [...common, 'src/params.cpp']],
    ['loxone', ['src/loxone_lines.cpp']],
    ['itplus', ['lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp', 'lib/TfaWeatherSensorReceiver/src/ItPlusEncoder.cpp']],
    ['fineoffset', ['lib/FineOffsetSensorReceiver/src/FineOffsetDecoder.cpp', 'lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp',
      'src/rf_sensors_data.cpp', 'src/rf_schedule.cpp', 'src/reception_window.cpp', 'src/calibration.cpp', 'src/rf_stats.cpp', 'src/rf_names.cpp', 'src/rf_minrssi.cpp', 'src/lsensors.cpp']],
    ['davis', ['lib/DavisSensorReceiver/src/DavisDecoder.cpp', 'lib/DavisSensorReceiver/src/DavisHopper.cpp']],
    ['netids', []],
    ['otacheck', []],
    ['otaupload', ['src/ota_upload.cpp']],
    ['telemetry', [...common, 'src/json_payload.cpp', 'src/units.cpp', 'src/loxone_lines.cpp']],
    ['metrics', [...common, 'src/metrics.cpp']],
    ['influx', [...common, 'src/influx.cpp']],
    ['influxqueue', ['src/influx_queue.cpp']],
    ['srclive', [...common]],
    ['srcbind', [...common, 'src/params.cpp']],
    ['netsensor', [...common, 'src/net_sensor.cpp']],
    ['vsensor', ['src/vsensor.cpp', 'src/vs_store.cpp', 'src/storage_migrate.cpp', 'test/host/psram_host.cpp', 'lib/FineOffsetSensorReceiver/src/FineOffsetEncoder.cpp', 'lib/FineOffsetSensorReceiver/src/FineOffsetDecoder.cpp',
      'lib/TfaWeatherSensorReceiver/src/ItPlusEncoder.cpp', 'lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp']],
    ['vsdavis', ['src/vsensor.cpp', 'lib/FineOffsetSensorReceiver/src/FineOffsetEncoder.cpp', 'lib/FineOffsetSensorReceiver/src/FineOffsetDecoder.cpp',
      'lib/TfaWeatherSensorReceiver/src/ItPlusEncoder.cpp', 'lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp']],
    ['foencode', ['lib/FineOffsetSensorReceiver/src/FineOffsetEncoder.cpp', 'lib/FineOffsetSensorReceiver/src/FineOffsetDecoder.cpp']],
    ['rf', [...common, 'src/params.cpp']],
    ['i18n', [...common, 'src/params.cpp', 'src/i18n.cpp']],
    ['wizard', [...common, 'src/params.cpp', 'src/setup_wizard.cpp']],
    ['awekas', ['src/awekas.cpp', 'src/derived.cpp']],
    ['calibration', [...common]],
    ['records', ['src/records.cpp', 'src/units.cpp']],
    ['astro', ['src/astro.cpp']],
    ['backup', ['src/settings_backup.cpp', 'src/lsensors.cpp', 'src/rf_names.cpp', 'test/host/runtime.cpp']],
    ['placeholders', ['src/placeholders.cpp']],
    ['oledcustom', ['src/oled_custom.cpp']],
    ['seriesdef', ['src/series_def.cpp', 'src/series_store.cpp', 'src/placeholders.cpp']],
    ['screensaver', ['src/screensaver.cpp']],
    ['statusled', ['src/status_led.cpp']],
    ['button', []],
    ['httputil', ['src/http_util.cpp']],
    ['rfstats', ['src/rf_stats.cpp', 'test/host/runtime.cpp']],
    ['rfnames', ['src/rf_names.cpp', 'src/settings_backup.cpp', 'src/lsensors.cpp', 'test/host/runtime.cpp']],
    ['lsensors', [...common]],
    ['rfminrssi', ['src/rf_minrssi.cpp', 'src/settings_backup.cpp', 'src/lsensors.cpp', 'src/rf_names.cpp', 'test/host/runtime.cpp']],
    ['netcheck', ['src/net_check.cpp']],
    ['mqttsession', []],
    ['alarmqueue', []],
    ['hagroups', []],
    ['storage', ['src/storage_migrate.cpp']],
    ['configsanitize', ['src/config_sanitize.cpp', 'src/loxone_lines.cpp']],
    ['configmerge', ['src/config_merge.cpp']],
    ['settingsreset', ['src/settings_reset.cpp', 'src/storage_write.cpp']],
    ['resethistory', ['src/reset_history.cpp', 'src/storage_migrate.cpp']],
    ['tlsgate', ['src/tls_gate.cpp']],
    ['https', ['src/https_cert.cpp', 'src/storage_migrate.cpp']],
    ['rfautosearch', ['src/rf_autosearch.cpp']],
    ['rfschedule', ['src/rf_schedule.cpp']],
    ['reception', ['src/reception_window.cpp', 'src/rf_schedule.cpp']],
    ['manifest', ['src/web_manifest.cpp', 'src/i18n.cpp']],
    ['alarms', ['src/alarms.cpp', 'src/placeholders.cpp', 'src/units.cpp']],
    ['notify', ['src/notify.cpp', 'src/placeholders.cpp']],
    ['wxformats', ['src/wx_formats.cpp', 'src/derived.cpp']],
    ['outstatus', ['src/out_status.cpp', 'test/host/runtime.cpp']],
    ['rftrace', [...common, 'src/rf_trace.cpp', 'lib/FineOffsetSensorReceiver/src/FineOffsetDecoder.cpp',
      'lib/TfaWeatherSensorReceiver/src/ItPlusDecoder.cpp']],
    ['harf', [...common, 'src/json_payload.cpp', 'src/units.cpp', 'src/ha_rf.cpp']],
    ['history', [...common.filter((f) => f !== 'test/host/runtime.cpp'), 'src/units.cpp']],   // includes runtime.cpp + history.cpp
    ['board', [...common, 'src/params.cpp']],
    ['payload', [...common, 'src/lora_payload.cpp', 'src/payload_binary.cpp', 'src/units.cpp',
      '.pio/libdeps/heltec_wifi_lora_32_V4/CayenneLPP/src/CayenneLPP.cpp',
      '.pio/libdeps/heltec_wifi_lora_32_V4/CayenneLPP/src/CayenneLPPPolyline.cpp']],
  ]) {
    const exe = path.join(tmp, name);
    execFileSync(process.env.CXX || 'c++', [
      '-std=c++11', '-Wall', '-Wextra', '-Werror',
      '-I', 'test/host', '-I', 'src', '-I', 'include', '-I', 'lib/TfaWeatherSensorReceiver/src', '-I', 'lib/FineOffsetSensorReceiver/src',
      '-I', 'lib/DavisSensorReceiver/src',
      '-I', '.pio/libdeps/heltec_wifi_lora_32_V4/ArduinoJson/src',
      '-I', '.pio/libdeps/heltec_wifi_lora_32_V4/CayenneLPP/src',
      ...(process.env.HOST_CXXFLAGS ? process.env.HOST_CXXFLAGS.split(/\s+/).filter(Boolean) : []),   // e.g. sanitizers
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
