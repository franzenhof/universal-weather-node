// Service worker of the node's web interface (served as /sw.js, registered by
// the dashboard only in a secure context - https, e.g. behind a reverse proxy).
// Network first for pages and /api/data + /api/history, falling back to the
// last cached copy when offline (the dashboard then shows "offline - last
// data from ..."); cache first only for the icons and the manifest. Never
// cached: settings, setup, log, firmware update, any POST or anything sent
// with credentials. The cache name carries the firmware version, so an OTA
// update starts a fresh cache.
const CACHE = 'uwn-@@VERSION@@';
const STATIC = ['/icon-192.png', '/icon-512.png', '/icon-maskable-512.png', '/apple-touch-icon.png', '/favicon-32.png',
                '/manifest.webmanifest'];
const NEVER = /^\/(api\/settings|api\/log|api\/events|api\/alarms\/|api\/rf\/|setup|update|u$|api\/davis)/;

self.addEventListener('install', (e) => { self.skipWaiting(); });
self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET' || req.headers.has('Authorization')) return;   // POSTs, logged-in requests: browser only
  const url = new URL(req.url);
  if (url.origin !== self.location.origin || NEVER.test(url.pathname)) return;
  if (STATIC.indexOf(url.pathname) >= 0) {                                 // cache first
    e.respondWith(caches.match(req).then((hit) => hit || fetch(req).then((r) => {
      if (r.ok) { const c = r.clone(); caches.open(CACHE).then((cc) => cc.put(req, c)); }
      return r;
    })));
    return;
  }
  const page = req.mode === 'navigate' && url.pathname === '/';
  const data = url.pathname === '/api/data' || url.pathname === '/api/history';
  if (!page && !data) return;
  // History: only the current windows (?range=...) are cached, one entry per range. Past windows and compare
  // (end=, res=) go to the network only, or they would replace the entry offline shows as "now".
  if (url.pathname === '/api/history' && (url.searchParams.has('end') || url.searchParams.has('res'))) return;
  const key = page ? '/' : url.pathname + (url.pathname === '/api/history' ? '?range=' + (url.searchParams.get('range') || '24h') : '');
  e.respondWith(fetch(req).then((r) => {                                 // network first
    if (r.ok) {
      const c = r.clone();
      caches.open(CACHE).then((cc) => c.blob().then((b) => cc.put(key, new Response(b, {
        headers: { 'Content-Type': r.headers.get('Content-Type') || '', 'X-UWN-Cached': new Date().toISOString() } }))));
    }
    return r;
  }).catch(() => caches.match(key).then((hit) => hit || Response.error())));
});
