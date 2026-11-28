// Settings backup: every write-only field of the settings page is a secret
// (src/settings_backup.cpp backupIsSecret) - so a backup without "include
// passwords and keys" can never contain one.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');

test('every fieldSecret is in the backup secret list', () => {
  const web = fs.readFileSync(path.join(root, 'src/web_api.cpp'), 'utf8');
  const lib = fs.readFileSync(path.join(root, 'src/settings_backup.cpp'), 'utf8');
  const list = lib.match(/secrets\[\] = \{([^}]*)\}/)[1];
  const names = new Set([...web.matchAll(/fieldSecret\s*\(\s*html,\s*"([^"]+)"/g)].map((m) => m[1]));
  assert.ok(names.size >= 10);
  for (const n of names) assert.ok(list.includes(`"${n}"`), `${n} missing in backupIsSecret()`);
  // secrets are recorded only when asked for
  assert.match(web, /if \(gCaptureSecrets && backupIsSecret\(name\)\)/);
  // URL fields that may carry a key are recorded only with the secrets as well
  assert.match(web, /backupIsSecret\(name\) && !gCaptureSecrets\)\) return;/);
  for (const n of ['whurl', 'cu0url', 'cu1url']) assert.ok(list.includes(`"${n}"`), n);
});

test('Web UI options are wired end to end (NVS, form, backup capture, /api/info)', () => {
  const rd = (f) => fs.readFileSync(path.join(root, f), 'utf8');
  const cfg = rd('src/config.cpp'), web = rd('src/web_api.cpp'), info = rd('src/api_info.cpp'), page = rd('include/chart_page.h');
  for (const [nvs, field, form, api] of [['uiflash', 'uiFlashEn', 'uiflash', 'flashOnChange'], ['uiage', 'uiAgeEn', 'uiage', 'showAge']]) {
    assert.match(cfg, new RegExp(`c\\.${field}\\s*= prefs\\.getBool\\("${nvs}"`));
    assert.match(cfg, new RegExp(`putBool\\s*\\("${nvs}"`));
    assert.match(web, new RegExp(`fieldCheck \\(html, "${form}"`));   // fieldCheck also records it in the backup
    assert.match(web, new RegExp(`present\\("${form}"\\)\\)\\s+gCfg\\.${field}`));
    assert.ok(nvs.length <= 15);
    assert.match(info, new RegExp(`ui\\["${api}"\\]\\s*= gCfg\\.${field}`));
    assert.ok(page.includes(`o.${api}`), `dashboard reads ${api}`);
  }
});
