// Dependent form fields (data-dep / DepScope in web_api.cpp): every
// condition names a field that exists on the pages, and the script is there.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const web = fs.readFileSync(path.join(__dirname, '..', 'src/web_api.cpp'), 'utf8');

test('every data-dep condition references an existing field', () => {
  const fields = new Set([...web.matchAll(/field(?:Text|Check|Select|Secret)\s*\(\s*html,\s*"([a-z0-9_]+)"/g)].map((m) => m[1]));
  // per-rule / per-upload names built at run time
  for (let i = 0; i < 12; i++) for (const x of ['src', 'rfsel', 'rff']) fields.add(`a${i}_${x}`);
  for (let i = 0; i < 2; i++) fields.add(`cu${i}post`);
  const deps = [...web.matchAll(/DepScope \w+\("([^"]+)"\)/g)].map((m) => m[1]);
  assert.ok(deps.length >= 15, `found ${deps.length}`);
  for (const d of deps)
    for (const alt of d.split(','))
      for (const cond of alt.split('&')) {
        const m = cond.match(/^(\w+)(!?=)/);
        assert.ok(m, cond);
        assert.ok(fields.has(m[1]), `${d}: unknown field ${m[1]}`);
      }
  // the run-time patterns: rule fields, custom-upload body
  assert.match(web, /"a%d_src!=0"/);
  assert.match(web, /"a%d_src=%d", i, AS_RF/);
  assert.match(web, /"cu%dpost=1"/);
  // the shared script and the [hidden] rule; data-dep written by labelStart()
  assert.match(web, /querySelectorAll\('\[data-dep\]'\)/);
  assert.match(web, /\[hidden\]\{display:none!important;\}/);
  assert.match(web, /if \(gDep\) \{ html \+= " data-dep='"/);
});
