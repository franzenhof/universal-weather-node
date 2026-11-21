// Alarm notifications and custom uploads: the request URL can carry a
// token (Telegram bot token, webhook / custom-upload keys) - it must never
// reach the log.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');

test('notification and custom-upload log lines carry no URL, token or body', () => {
  for (const f of ['src/alarms_run.cpp', 'src/wx_upload.cpp', 'src/notify.cpp']) {
    const src = fs.readFileSync(path.join(root, f), 'utf8');
    for (const m of src.matchAll(/dbgSerial\.print(?:f|ln)?\(([^;]*)\);/g)) {
      assert.doesNotMatch(m[1], /\breq\.(url|body|auth)|\burl\b|Token|token|whUrl|\.key\b|poUser|tgChat/,
        `${f}: log line may contain a secret: ${m[1].slice(0, 80)}`);
    }
  }
});
