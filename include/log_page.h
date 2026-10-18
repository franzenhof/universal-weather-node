#pragma once

#include <Arduino.h>

// Rolling view of dbgSerial's ring buffer (this device's own log output —
// see serial_log.h for exactly what is/isn't captured: our own code, not
// library-internal prints, and never anything before setup() runs). The
// WebServer library has no server-push mechanism, so the page just polls
// /api/log/raw on an interval instead.
static const char LOG_PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WeatherNode log</title>
<style>
  body{font-family:sans-serif;max-width:900px;margin:1rem auto;padding:0 1rem;}
  nav a{margin-right:0.75rem;}
  #log{background:#111;color:#ddd;font-family:ui-monospace,Consolas,monospace;font-size:0.85rem;
       padding:0.75rem;border-radius:6px;height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-word;}
  button{margin:0.5rem 0.5rem 0.5rem 0;}
</style>
</head>
<body>
<nav><a href="/status">Status</a><a href="/">Chart</a><a href="/api/settings">Settings</a><a href="/help">Help / Documentation</a></nav>
<h3>Device log</h3>
<p>Rolling view of this device's own serial output (library-internal messages, e.g. from WiFiManager/RadioLib, aren't captured). Auto-refreshes every second.</p>
<button id="toggle">Pause</button>
<button id="clear">Clear</button>
<pre id="log">Loading...</pre>
<script>
var paused = false, atBottom = true;
var logEl = document.getElementById('log');
logEl.addEventListener('scroll', function () {
  atBottom = (logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight) < 20;
});
document.getElementById('toggle').addEventListener('click', function (e) {
  paused = !paused;
  e.target.textContent = paused ? 'Resume' : 'Pause';
});
document.getElementById('clear').addEventListener('click', function () {
  fetch('/api/log/clear', { method: 'POST' }).then(refresh);
});
function refresh() {
  if (paused) return;
  fetch('/api/log/raw').then(function (r) { return r.text(); }).then(function (t) {
    logEl.textContent = t;
    if (atBottom) logEl.scrollTop = logEl.scrollHeight;
  }).catch(function (e) { logEl.textContent = 'Failed to load log: ' + e; });
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
)HTML";
