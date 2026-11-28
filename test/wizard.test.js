// Setup wizard: every field a step posts (src/setup_wizard.cpp) is rendered by
// the wizard and handled by the one settings handler (POST /api/settings) -
// no second validation path.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const wizardSrc = fs.readFileSync(path.join(root, 'src/setup_wizard.cpp'), 'utf8');
const web = fs.readFileSync(path.join(root, 'src/web_api.cpp'), 'utf8');

function body(src, signature) {
  const start = src.indexOf(signature);
  assert.ok(start >= 0, `${signature} not found`);
  let depth = 0, i = src.indexOf('{', start);
  for (let j = i; j < src.length; j++) {
    if (src[j] === '{') depth++;
    else if (src[j] === '}' && --depth === 0) return src.slice(i, j + 1);
  }
  throw new Error('unbalanced ' + signature);
}

// FIELDS[] of setup_wizard.cpp: one (possibly concatenated) string per step
const fieldsBlock = /FIELDS\[WIZ_STEPS\] = \{([\s\S]*?)\};/.exec(wizardSrc)[1];
const steps = fieldsBlock.split('\n').filter((l) => l.includes('"')).reduce((acc, l) => {
  const s = [...l.matchAll(/"([^"]*)"/g)].map((m) => m[1]).join('');
  if (/\/\/ \d/.test(l) || !acc.length) acc.push(s); else acc[acc.length - 1] += s;
  return acc;
}, []);
const fields = steps.join(',').split(',').filter(Boolean);

test('the wizard has seven steps and posts known fields', () => {
  assert.strictEqual(steps.length, 7);
  assert.ok(fields.length > 20);
});

// "name*" = a prefix (rfname_<ID>)
const literal = (f) => f.replace(/\*$/, '');

test('every wizard field is handled by the settings handler', () => {
  const post = body(web, 'void handleSettingsPost()');
  const missing = fields.filter((f) => !post.includes(f.endsWith('*') ? `"${literal(f)}` : `"${f}"`));
  assert.deepStrictEqual(missing, []);
});

// the wizard page and the sensor rows of step 2
const page = body(web, 'void renderWizard(') + body(web, 'void wizardSensorRows(');

test('every wizard field is rendered by the wizard', () => {
  const missing = fields.filter((f) => f.endsWith('*') ? !page.includes(`name='${literal(f)}`)
    : !page.includes(`"${f}"`) && !page.includes(`name='${f}'`));
  assert.deepStrictEqual(missing, []);
});

// mirrors wizardFieldAllowed() (src/setup_wizard.cpp): an open wizard POST
// with any other field is refused
function allowed(step, name) {
  if (['form', 'wizard', 'wstep', 'wstay'].includes(name)) return true;
  return steps[step - 1].split(',').filter(Boolean).some((f) =>
    f.endsWith('*') ? name.startsWith(literal(f)) : name === f || name === f + '_clear');
}

test('every field a wizard step renders is in that step\'s list', () => {
  const wiz = body(web, 'void renderWizard(');
  const rows = body(web, 'void wizardSensorRows(');
  const names = (src) => [...src.matchAll(/field\w+\s*\(html,\s*"(\w+)"/g), ...src.matchAll(/name='(\w+)/g)].map((m) => m[1]);
  const bad = [];
  for (let step = 1; step < 7; step++) {
    const start = wiz.indexOf(`case ${step}:`);
    assert.ok(start >= 0, `case ${step} not found`);
    const end = wiz.indexOf(step < 6 ? `case ${step + 1}:` : 'default:', start);
    let src = wiz.slice(start, end);
    if (step === 2) src += rows;
    for (const n of names(src)) if (!allowed(step, n)) bad.push(`${step}:${n}`);
  }
  assert.deepStrictEqual(bad, []);
});

test('wizard steps post with wizard=1 to the settings handler', () => {
  const page = body(web, 'void renderWizard(');
  assert.ok(page.includes("action='/api/settings'") && page.includes("name='wizard'"));
});
