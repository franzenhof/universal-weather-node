// API contract: the documented JSON (docs/api/schemas, docs/api/examples)
// must match what the firmware's real serializers emit.
//
// Builds test/firmware_api.cpp with buildJsonPayload() / buildHistoryJson() /
// buildInfoJson() and deterministic hardware fakes, runs every fixture and
//  - compares the output with the committed examples in docs/api/examples/
//    (regenerate: UPDATE_API_EXAMPLES=1 node --test test/api_contract.test.js),
//  - validates every document against its JSON Schema,
//  - fails on any emitted key the schema does not describe (a new firmware
//    field must be documented), and on schema properties without description,
//  - checks that no configured secret ends up in any document.
const test = require('node:test');
const assert = require('node:assert');
const {execFileSync} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {SchemaSet} = require('./json_schema_lite');

const root = path.join(__dirname, '..');
const examplesDir = path.join(root, 'docs/api/examples');
const schemas = new SchemaSet(path.join(root, 'docs/api/schemas'));
const FIXTURES = ['bresser-7in1-bme280', 'ecowitt-ws90-wh31-wh51-wh57', 'pcb-v2-full', 'pcb-v2-imperial', 'heltec-v4-wireless', 'davis-vantage-pro2'];
const DOCS = {data: 'data.schema.json', 'history-24h': 'history.schema.json', 'history-7d': 'history.schema.json', info: 'info.schema.json'};
const UPDATE = process.env.UPDATE_API_EXAMPLES === '1';
const REGEN = 'UPDATE_API_EXAMPLES=1 node --test test/api_contract.test.js';

function build() {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'weathernode-api-'));
  const exe = path.join(tmp, 'firmware_api');
  try {
    execFileSync(process.env.CXX || 'c++', [
      '-std=c++11', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-DFIRMWARE_VERSION="2.0.0"',
      '-I', 'test/host', '-I', 'src', '-I', 'lib/TfaWeatherSensorReceiver/src', '-I', 'lib/FineOffsetSensorReceiver/src',
      '-I', '.pio/libdeps/heltec_wifi_lora_32_V3/ArduinoJson/src',
      'src/sensors.cpp', 'src/board_io.cpp', 'src/vedirect.cpp', 'src/sources.cpp', 'src/rf_sensors_data.cpp',
      'src/derived.cpp', 'src/derived_now.cpp', 'src/units.cpp', 'src/json_payload.cpp', 'src/api_info.cpp',
      'test/firmware_api.cpp', '-o', exe,
    ], {cwd: root, stdio: 'pipe'});
  } catch (error) {
    if (error.stderr) process.stderr.write(error.stderr);
    throw error;
  }
  execFileSync(exe, ['hostname'], {stdio: 'inherit'});   // hostname sanitiser asserts (throws on failure)
  const out = {};
  for (const f of FIXTURES) {
    out[f] = {};
    for (const line of execFileSync(exe, [f], {encoding: 'utf8'}).trim().split('\n')) {
      const sp = line.indexOf(' ');
      out[f][line.slice(0, sp)] = JSON.parse(line.slice(sp + 1));
    }
  }
  fs.rmSync(tmp, {recursive: true, force: true});
  return out;
}

// Pretty JSON, but arrays of numbers/strings on one line (history series).
function format(v, ind = '') {
  if (Array.isArray(v)) {
    if (v.every((x) => x === null || typeof x !== 'object')) return '[' + v.map((x) => JSON.stringify(x)).join(', ') + ']';
    return '[\n' + v.map((x) => ind + '  ' + format(x, ind + '  ')).join(',\n') + '\n' + ind + ']';
  }
  if (v && typeof v === 'object') {
    const keys = Object.keys(v);
    if (!keys.length) return '{}';
    return '{\n' + keys.map((k) => ind + '  ' + JSON.stringify(k) + ': ' + format(v[k], ind + '  ')).join(',\n') + '\n' + ind + '}';
  }
  return JSON.stringify(v);
}

// First difference (key order included); numbers within a small tolerance,
// so a last-digit libm difference between platforms does not fail the test.
function firstDiff(a, b, at = '$') {
  if (typeof a === 'number' && typeof b === 'number')
    return Math.abs(a - b) <= 1e-4 * Math.max(1, Math.abs(a), Math.abs(b)) ? null : `${at}: ${b} (example) vs ${a} (firmware)`;
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object')
    return a === b ? null : `${at}: ${JSON.stringify(b)} (example) vs ${JSON.stringify(a)} (firmware)`;
  if (Array.isArray(a) !== Array.isArray(b)) return `${at}: array/object mismatch`;
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.join() !== kb.join()) return `${at}: keys [${kb}] (example) vs [${ka}] (firmware)`;
  for (const k of ka) {
    const d = firstDiff(a[k], b[k], Array.isArray(a) ? `${at}[${k}]` : `${at}.${k}`);
    if (d) return d;
  }
  return null;
}

const output = build();

test('docs/api/examples match the firmware serializers', () => {
  const problems = [];
  for (const f of FIXTURES) {
    for (const doc of Object.keys(DOCS)) {
      const file = path.join(examplesDir, f, `${doc}.json`);
      const text = format(output[f][doc]) + '\n';
      if (UPDATE) {
        fs.mkdirSync(path.dirname(file), {recursive: true});
        fs.writeFileSync(file, text);
        continue;
      }
      if (!fs.existsSync(file)) { problems.push(`${path.relative(root, file)} missing`); continue; }
      const d = firstDiff(output[f][doc], JSON.parse(fs.readFileSync(file, 'utf8')));
      if (d) problems.push(`${path.relative(root, file)}: ${d}`);
    }
  }
  const dirs = fs.existsSync(examplesDir) ? fs.readdirSync(examplesDir).filter((d) => fs.statSync(path.join(examplesDir, d)).isDirectory()) : [];
  for (const d of dirs) if (!FIXTURES.includes(d)) problems.push(`docs/api/examples/${d}: no such fixture (remove it)`);
  assert.deepStrictEqual(problems, [], `API examples are out of date - check the change, then regenerate with:\n  ${REGEN}\n`);
});

test('every example validates against its JSON Schema', () => {
  const errors = [];
  for (const f of FIXTURES)
    for (const [doc, schema] of Object.entries(DOCS))
      for (const e of schemas.validate(output[f][doc], schema)) errors.push(`${f}/${doc}: ${e}`);
  assert.deepStrictEqual(errors, []);
});

test('every emitted key is documented in the schema', () => {
  const missing = new Set();
  for (const f of FIXTURES)
    for (const [doc, schema] of Object.entries(DOCS))
      for (const k of schemas.undocumented(output[f][doc], schema)) missing.add(`${schema}: ${k.replace(/\[\d+\]/g, '[]')}`);
  assert.deepStrictEqual([...missing], [], 'new firmware fields must be added to docs/api/schemas/ (and docs/api.md)');
});

test('every schema property has a description', () => {
  for (const s of new Set(Object.values(DOCS))) assert.deepStrictEqual(schemas.undescribed(s), [], s);
});

test('documents are consistent and contain no secrets', () => {
  for (const f of FIXTURES) {
    const all = JSON.stringify(output[f]);
    assert.ok(!/secret/i.test(all), `${f}: a configured secret leaked into the API`);
    for (const doc of ['data', 'history-24h', 'history-7d', 'info']) assert.strictEqual(output[f][doc].apiVersion, 1, `${f}/${doc}`);
    for (const range of ['history-24h', 'history-7d']) {
      const h = output[f][range];
      for (const [k, v] of Object.entries(h))
        if (Array.isArray(v)) assert.strictEqual(v.length, h.ageSec.length, `${f}/${range}: ${k} length`);
      for (let i = 1; i < h.ageSec.length; i++) assert.ok(h.ageSec[i] < h.ageSec[i - 1], `${f}/${range}: ageSec not oldest-first`);
    }
    const {data, info} = output[f];
    // /api/info describes what /api/data carries
    assert.strictEqual(info.available.temp, 'temp' in data, `${f}: available.temp`);
    assert.strictEqual(info.available.pressure, 'pressureQnh' in data, `${f}: available.pressure`);
    assert.strictEqual(info.available.wirelessSensors, 'rfSensors' in data, `${f}: available.wirelessSensors`);
    assert.strictEqual(info.available.ds18b20Probes, (data.ds18b20 || []).length, `${f}: available.ds18b20Probes`);
    for (const [k, unitKey] of [['temp', 'tempUnit'], ['pressure', 'pressureUnit'], ['wind', 'windUnit'], ['rain', 'rainUnit']])
      if (data[unitKey]) assert.strictEqual(info.units[k], data[unitKey], `${f}: units.${k}`);
    if (data.sources) assert.deepStrictEqual(info.sources, data.sources, `${f}: sources`);
  }
});

test('schema coverage', (t) => {
  const n = Object.fromEntries([...new Set(Object.values(DOCS))].map((s) => [s, schemas.countFields(s)]));
  t.diagnostic(`documented fields: ${JSON.stringify(n)}`);
  assert.ok(n['data.schema.json'] > 100);
});

test('openapi.yaml covers exactly the routes of web_api.cpp', () => {
  const src = fs.readFileSync(path.join(root, 'src/web_api.cpp'), 'utf8');
  const routes = new Set();
  for (const m of src.matchAll(/server\.on\("([^"]+)",\s*HTTP_(\w+)/g)) routes.add(`${m[2].toLowerCase()} ${m[1]}`);
  const loop = /for \(const char\* uri : \{([^}]*)\}\)\s*server\.on\(uri,\s*HTTP_(\w+)/.exec(src);   // OPTIONS loop
  assert.ok(loop, 'OPTIONS route loop not found in web_api.cpp');
  for (const u of loop[1].match(/"[^"]+"/g)) routes.add(`${loop[2].toLowerCase()} ${u.slice(1, -1)}`);

  const yaml = fs.readFileSync(path.join(root, 'docs/api/openapi.yaml'), 'utf8');
  const documented = new Set();
  let current = null, inPaths = false;
  for (const line of yaml.split('\n')) {
    if (/^\S/.test(line)) inPaths = line.startsWith('paths:');
    if (!inPaths) continue;
    let m;
    if ((m = /^ {2}(\/\S*):\s*$/.exec(line))) current = m[1];
    else if ((m = /^ {4}(get|post|put|delete|options|patch|head):/.exec(line)) && current) documented.add(`${m[1]} ${current}`);
  }
  assert.deepStrictEqual([...documented].sort(), [...routes].sort());
  for (const m of yaml.matchAll(/(?:\$ref|externalValue): "(\.\/[^"#]+)/g))
    assert.ok(fs.existsSync(path.join(root, 'docs/api', m[1])), `openapi.yaml references missing ${m[1]}`);
  for (const m of yaml.matchAll(/\$ref: "#\/components\/(\w+)\/(\w+)"/g))
    assert.ok(new RegExp(`^ {2}${m[1]}:[\\s\\S]*?^ {4}${m[2]}:`, 'm').test(yaml), `openapi.yaml: #/components/${m[1]}/${m[2]} missing`);
});
