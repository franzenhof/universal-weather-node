// Web interface translations (i18n/*.json, tools/i18n_gen.py): every key in
// every language, the same placeholders and HTML tags, no unused or unknown
// keys in the firmware sources, safe apostrophes.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const LANGS = ['en', 'de', 'fr', 'es', 'pt'];
const load = (l) => JSON.parse(fs.readFileSync(path.join(root, 'i18n', `${l}.json`), 'utf8'));
const tables = Object.fromEntries(LANGS.map((l) => [l, load(l)]));
const en = tables.en;
const placeholders = (s) => [...new Set(s.match(/\{\d\}/g) || [])].sort().join(',');
const tags = (s) => (s.match(/<\/?[a-z0-9]+/gi) || []).map((t) => t.toLowerCase()).sort().join(',');
const outsideTags = (s) => s.replace(/<[^>]*>/g, '');

test('every language has every key, no extra keys', () => {
  const problems = [];
  for (const l of LANGS.slice(1)) {
    for (const k of Object.keys(en)) if (typeof tables[l][k] !== 'string' || !tables[l][k].trim()) problems.push(`${l}: missing ${k}`);
    for (const k of Object.keys(tables[l])) if (!(k in en)) problems.push(`${l}: extra ${k}`);
  }
  assert.deepStrictEqual(problems, []);
});

test('placeholders and HTML tags match English', () => {
  const problems = [];
  for (const l of LANGS.slice(1)) {
    for (const [k, v] of Object.entries(en)) {
      const t = tables[l][k];
      if (typeof t !== 'string') continue;
      if (placeholders(t) !== placeholders(v)) problems.push(`${l}: ${k}: placeholders ${placeholders(t)} != ${placeholders(v)}`);
      if (tags(t) !== tags(v)) problems.push(`${l}: ${k}: HTML tags differ`);
      // an ASCII apostrophe in the text could end an attribute ('...') or a JS string
      if (outsideTags(t).includes("'")) problems.push(`${l}: ${k}: ASCII apostrophe (use ’)`);
    }
  }
  assert.deepStrictEqual(problems, []);
});

test('compass points: 16 per language', () => {
  for (const l of LANGS) assert.strictEqual(tables[l].c_compass.split(',').length, 16, l);
});

test('firmware sources use exactly the keys of en.json', () => {
  const used = new Set();
  for (const f of ['src/web_api.cpp', 'src/portal.cpp', 'include/calibrate_page.h', 'src/i18n.cpp', 'test/firmware_i18n.cpp',
    'src/alarms_run.cpp', 'src/oled_screens.cpp']) {
    for (const m of fs.readFileSync(path.join(root, f), 'utf8').matchAll(/\bT_([a-z][a-z0-9_]*)\b/g)) used.add(m[1]);
  }
  assert.deepStrictEqual([...used].filter((k) => !(k in en)).sort(), [], 'keys used in the sources but missing in en.json');
  assert.deepStrictEqual(Object.keys(en).filter((k) => !used.has(k)).sort(), [], 'keys in en.json no source uses');
});

test('key names are valid C identifiers', () => {
  for (const k of Object.keys(en)) assert.match(k, /^[a-z][a-z0-9_]*$/);
});
