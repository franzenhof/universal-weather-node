#pragma once

// Guided wind-direction calibration page, served from the WiFiManager
// portal and from the web interface (/calibrate, /calibrate/sample,
// /calibrate/save; portal_pages.h, portal.cpp). Works with any resistor-ladder/reed-switch
// wind vane (discrete voltage steps per direction) regardless of vendor —
// the user physically rotates the vane through each known position while
// this page captures the live ADC reading, building a calibration table
// without needing to know the sensor's internal resistor values.
//
// Self-contained (no CDN), so it also works while connected to the
// isolated WiFiManager AP portal. A template: @N@ = text N of CALIBRATE_TEXTS
// (HTML), @jN@ = the same as a JavaScript string, @L@ = language code -
// filled in by sendI18nTemplate() (portal.cpp).
#include "i18n_keys.h"
static const TextId CALIBRATE_TEXTS[] = {
    T_cal_title,
    T_cal_intro,
    T_cal_how_many,
    T_cal_n_positions,
    T_cal_live_adc,
    T_cal_capture,
    T_cal_all_captured,
    T_cal_save,
    T_cal_step,
    T_cal_capture_failed,
    T_cal_direction,
    T_cal_save_failed,
    T_ota_back,
};
static const char CALIBRATE_PAGE_HTML[] PROGMEM = R"CALPAGE(
<!doctype html>
<html lang="@L@">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>@0@</title>
<style>
  :root{color-scheme:light dark;}
  body{font-family:sans-serif;max-width:600px;margin:1rem auto;padding:0 1rem;line-height:1.5;}
  a,a:visited{color:#0b7f78;}
  @media (prefers-color-scheme: dark){body{background:#12161b;color:#e7ecf1;}a,a:visited{color:#3fd0c9;}}
  button{font-size:1rem;padding:0.5rem 1rem;margin:0.25rem 0.25rem 0.25rem 0;}
  table{border-collapse:collapse;margin:1rem 0;width:100%;}
  th,td{border:1px solid #ccc;padding:4px 8px;text-align:left;}
  #msg{margin-top:1rem;font-weight:bold;}
</style>
</head>
<body>
<h3>@0@</h3>
<p>@1@</p>

<div id="setup">
  <p>@2@</p>
  <button onclick="start(8)" id="b8"></button>
  <button onclick="start(16)" id="b16"></button>
</div>

<div id="step" style="display:none">
  <p id="stepLabel"></p>
  <p>@4@ <strong id="liveAdc">-</strong></p>
  <button id="capBtn" onclick="capture()">@5@</button>
</div>

<div id="done" style="display:none">
  <p>@6@</p>
  <table id="resultTable"></table>
  <button onclick="save()">@7@</button>
</div>

<div id="msg"></div>
<p><a href="#" onclick="history.back();return false;">@12@</a></p>

<script>
var L = { npos: @j3@, step: @j8@, capFail: @j9@, dir: @j10@, saveFail: @j11@ };
function fmt(s, a) { return s.replace(/\{(\d)\}/g, function (m, i) { return a[+i]; }); }
document.getElementById('b8').textContent = fmt(L.npos, [8]);
document.getElementById('b16').textContent = fmt(L.npos, [16]);
var count = 0, idx = 0, positions = [];
var dirs = ['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];

function compassLabel(deg) { return dirs[Math.round(deg / 22.5) % 16]; }

function start(n) {
  count = n; idx = 0; positions = [];
  document.getElementById('setup').style.display = 'none';
  document.getElementById('step').style.display = 'block';
  showStep();
}

function showStep() {
  var deg = Math.round(idx * 360 / count);
  document.getElementById('stepLabel').textContent =
    fmt(L.step, [idx + 1, count, compassLabel(deg), deg]);
}

function capture() {
  var btn = document.getElementById('capBtn');
  if (btn.disabled) return;
  btn.disabled = true;   // a double tap must not record two positions from one vane position
  fetch('/calibrate/sample').then(function(r) { return r.json(); }).then(function(j) {
    if (typeof j.adc !== 'number') throw new Error('no ADC value');
    var deg = Math.round(idx * 360 / count);
    positions.push({ deg: deg, adc: j.adc });
    idx++;
    if (idx < count) {
      showStep();
    } else {
      document.getElementById('step').style.display = 'none';
      document.getElementById('done').style.display = 'block';
      renderTable();
    }
  }).catch(function(e) {
    document.getElementById('msg').textContent = L.capFail + ' ' + e;
  }).then(function() { btn.disabled = false; });
}

function renderTable() {
  var html = '<tr><th>' + L.dir + '</th><th>ADC</th></tr>';
  positions.forEach(function(p) {
    html += '<tr><td>' + compassLabel(p.deg) + ' (' + p.deg + '°)</td><td>' + p.adc + '</td></tr>';
  });
  document.getElementById('resultTable').innerHTML = html;
}

function save() {
  fetch('/calibrate/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ positions: positions })
  }).then(function(r) { return r.text(); }).then(function(t) {
    document.getElementById('msg').textContent = t;
  }).catch(function(e) {
    document.getElementById('msg').textContent = L.saveFail + ' ' + e;
  });
}

// Live ADC preview while positioning the vane, before Capture is clicked.
setInterval(function() {
  if (document.getElementById('step').style.display === 'none') return;
  fetch('/calibrate/sample').then(function(r) { return r.json(); }).then(function(j) {
    document.getElementById('liveAdc').textContent = j.adc;
  }).catch(function() {});
}, 1000);
</script>
</body>
</html>
)CALPAGE";
