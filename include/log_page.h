#pragma once

#include <Arduino.h>

// Rolling view of dbgSerial's ring buffer (this device's own log output —
// see serial_log.h for exactly what is/isn't captured: our own code, not
// library-internal prints, and never anything before setup() runs). The
// WebServer library has no server-push mechanism, so the page just polls
// /api/log/raw on an interval instead. The page itself (navigation, texts in
// the visitor's language) is built by handleLogPage() in web_api.cpp; this
// is only its style and script. The script expects
//   var L = { pause, resume, failed }   (texts, set before it)
static const char LOG_PAGE_STYLE[] PROGMEM = R"HTML(<style>
#log{background:#111;color:#ddd;font-family:ui-monospace,Consolas,monospace;font-size:0.85rem;
     padding:0.75rem;border-radius:6px;height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-word;}
.logbtn{margin:0.5rem 0.5rem 0.5rem 0;}
</style>)HTML";

static const char LOG_PAGE_SCRIPT[] PROGMEM = R"HTML(<script>
var paused = false, atBottom = true;
var logEl = document.getElementById('log');
logEl.addEventListener('scroll', function () {
  atBottom = (logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight) < 20;
});
document.getElementById('toggle').addEventListener('click', function (e) {
  paused = !paused;
  e.target.textContent = paused ? L.resume : L.pause;
});
document.getElementById('clear').addEventListener('click', function () {
  fetch('/api/log/clear', { method: 'POST' }).then(refresh);
});
function refresh() {
  if (paused) return;
  fetch('/api/log/raw').then(function (r) { return r.text(); }).then(function (t) {
    logEl.textContent = t;
    if (atBottom) logEl.scrollTop = logEl.scrollHeight;
  }).catch(function (e) { logEl.textContent = L.failed + ' ' + e; });
}
refresh();
setInterval(refresh, 1000);
</script>)HTML";
