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

test('every wizard field is handled by the settings handler', () => {
  const post = body(web, 'void handleSettingsPost()');
  const missing = fields.filter((f) => !post.includes(`"${f}"`));
  assert.deepStrictEqual(missing, []);
});

test('every wizard field is rendered by the wizard', () => {
  const page = body(web, 'void renderWizard(');
  const missing = fields.filter((f) => !page.includes(`"${f}"`) && !page.includes(`name='${f}'`));
  assert.deepStrictEqual(missing, []);
});

test('wizard steps post with wizard=1 to the settings handler', () => {
  const page = body(web, 'void renderWizard(');
  assert.ok(page.includes("action='/api/settings'") && page.includes("name='wizard'"));
});
