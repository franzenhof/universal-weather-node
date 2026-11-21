// Home screen / installed app: routes and headers, head tags on every page,
// and the service worker's rules (never POSTs, settings, setup, log, OTA).
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.join(__dirname, '..');
const read = (f) => fs.readFileSync(path.join(root, f), 'utf8');

test('icon, manifest and service-worker routes with their headers', () => {
  const web = read('src/web_api.cpp');
  for (const r of ['/manifest.webmanifest', '/sw.js', '/icon-192.png', '/icon-512.png', '/icon-maskable-512.png', '/apple-touch-icon.png',
                   '/apple-touch-icon-precomposed.png', '/favicon-32.png', '/favicon.ico'])
    assert.ok(web.includes(`server.on("${r}"`), r);
  assert.match(web, /sendHeader\("Cache-Control", "public, max-age=604800"\)/);
  assert.match(web, /sendHeader\("Service-Worker-Allowed", "\/"\)/);
  assert.match(web, /"application\/manifest\+json"/);
  // the icons exist and are PNGs
  for (const f of ['icon-192.png', 'icon-512.png', 'icon-maskable-512.png', 'apple-touch-icon.png', 'favicon-32.png'])
    assert.deepStrictEqual([...fs.readFileSync(path.join(root, 'pictures/icon', f)).subarray(0, 4)], [0x89, 0x50, 0x4e, 0x47], f);
});

test('every page has the manifest, icon, apple-touch and theme-color tags', () => {
  const tags = [/rel=["']manifest["'] href=["']\/manifest\.webmanifest/, /rel=["']icon["'][^>]*href=["']\/favicon-32\.png/,
                /rel=["']apple-touch-icon["'] href=["']\/apple-touch-icon\.png/, /name=["']theme-color["'][^>]*prefers-color-scheme: light/,
                /name=["']theme-color["'][^>]*prefers-color-scheme: dark/, /name=["']apple-mobile-web-app-capable["']/,
                /name=["']mobile-web-app-capable["']/, /name=["']apple-mobile-web-app-title["']/];
  const web = read('src/web_api.cpp');
  const pageStart = web.slice(web.indexOf('void pageStart('), web.indexOf('</head>', web.indexOf('void pageStart(')));
  for (const [name, text] of [['pageStart (status, log, settings, wizard)', pageStart], ['dashboard', read('include/chart_page.h')],
                              ['help', read('include/help_page.h')]])
    for (const re of tags) assert.match(text, re, `${name}: ${re}`);
  // every server page goes through pageStart
  assert.ok((web.match(/pageStart\(html,/g) || []).length >= 6);
  // the dashboard registers the service worker only in a secure context
  assert.match(read('include/chart_page.h'), /'serviceWorker' in navigator && window\.isSecureContext/);
});

test('service worker: network first for the dashboard and its data, never POSTs or private pages', () => {
  const handlers = {};
  const responded = [];
  const sandbox = {
    self: { addEventListener: (t, f) => { handlers[t] = f; }, location: { origin: 'https://node' }, skipWaiting() {}, clients: { claim() {} } },
    caches: { match: () => Promise.resolve(undefined), open: () => Promise.resolve({ put() {} }), keys: () => Promise.resolve([]) },
    fetch: () => Promise.resolve({ ok: true, clone() { return this; }, blob: () => Promise.resolve(''), headers: { get: () => '' } }),
    Response: function () {}, URL, Date, Promise,
  };
  sandbox.Response.error = () => ({});
  vm.runInNewContext(read('web/sw.js').replace('@@VERSION@@', '2.0.0'), sandbox);
  assert.ok(handlers.fetch && handlers.install && handlers.activate);
  function request(method, url, extra = {}) {
    let used = false;
    handlers.fetch({ request: { method, url, mode: extra.mode || 'cors', headers: { has: (h) => !!(extra.auth && h === 'Authorization') } },
                     respondWith() { used = true; } });
    responded.push(used);
    return used;
  }
  assert.equal(request('POST', 'https://node/api/settings'), false);
  assert.equal(request('POST', 'https://node/api/data'), false);
  for (const p of ['/api/settings', '/api/settings/backup', '/setup', '/api/log', '/api/log/raw', '/update', '/api/rf/trace', '/calibrate', '/calibrate/sample', '/decoder'])
    assert.equal(request('GET', 'https://node' + p), false, p);
  assert.equal(request('GET', 'https://node/api/data', { auth: true }), false);       // with credentials
  assert.equal(request('GET', 'https://other/api/data'), false);                      // other origin
  assert.equal(request('GET', 'https://node/api/data'), true);
  assert.equal(request('GET', 'https://node/api/history?range=7d'), true);
  assert.equal(request('GET', 'https://node/', { mode: 'navigate' }), true);
  assert.equal(request('GET', 'https://node/icon-192.png'), true);
  assert.equal(request('GET', 'https://node/status', { mode: 'navigate' }), false);   // other pages: browser only
  // the cache name carries the firmware version
  assert.match(read('web/sw.js'), /const CACHE = 'uwn-@@VERSION@@'/);
  assert.match(read('tools/version.py'), /replace\("@@VERSION@@", version_string\)/);
});
