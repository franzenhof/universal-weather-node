#pragma once

#include <Arduino.h>

// Dashboard rebuild modeled after cerevisis/ESP32-Weather-Station's UI
// (card tiles w/ sparkline, wind compass+rose, min/avg/max records, full
// trend charts) — see the project README for the reference. Unlike that
// project (Bootstrap 5 from a CDN), this stays vanilla CSS/canvas with no
// external dependency, so it still works from the isolated captive-portal
// AP with no internet route — same "works fully offline" convention as
// /help. Light/dark theme via CSS custom properties (prefers-color-scheme,
// overridable per-browser via the toggle button, persisted in
// localStorage). Auto-refreshes on the same fetch+setInterval pattern
// already used by the log console above.
//
// Localized (en/de/fr/es/pt, see the I18N object below) and unit-switchable
// (Metric/Imperial) — both are pure client-side view preferences, persisted
// in localStorage like the theme, and deliberately don't touch the device's
// own gCfg.unitSystem: that field is a device-wide setting (also affects
// the LoRaWAN payload and MQTT/Home Assistant), whereas this is just "how
// I, this one visitor, want to *look at* the already-fetched history" —
// conflating the two would mean anyone opening /chart changes what every
// other consumer of this node's data sees.
static const char CHART_PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WeatherNode chart</title>
<style>
:root{ --bg:#f4f6f8; --panel:#ffffff; --border:#dde3e9; --text:#1a2028; --dim:#62707c; --accent:#0f9d94; }
@media (prefers-color-scheme: dark){
  :root{ --bg:#12161b; --panel:#1a2028; --border:#2a323c; --text:#e7ecf1; --dim:#8996a3; --accent:#3fd0c9; }
}
:root[data-theme="dark"]{ --bg:#12161b; --panel:#1a2028; --border:#2a323c; --text:#e7ecf1; --dim:#8996a3; --accent:#3fd0c9; }
:root[data-theme="light"]{ --bg:#f4f6f8; --panel:#ffffff; --border:#dde3e9; --text:#1a2028; --dim:#62707c; --accent:#0f9d94; }

*{box-sizing:border-box;}
body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);
     max-width:1000px;margin:0 auto;padding:0 1rem 2rem;}
nav{display:flex;gap:1.1rem;padding:0.9rem 0;border-bottom:1px solid var(--border);margin-bottom:1rem;}
nav a{color:var(--dim);text-decoration:none;font-size:0.9rem;}
nav a:hover,nav a:focus{color:var(--accent);}
.head{display:flex;align-items:baseline;justify-content:space-between;gap:1rem;flex-wrap:wrap;margin-bottom:0.25rem;}
.head h3{margin:0;}
.head .sub{color:var(--dim);font-size:0.85rem;}
button,select{font:inherit;background:var(--panel);color:var(--text);border:1px solid var(--border);
       border-radius:6px;padding:0.4rem 0.75rem;cursor:pointer;}
button:hover,select:hover{border-color:var(--accent);}
button:focus-visible,select:focus-visible{outline:2px solid var(--accent);outline-offset:1px;}
.toolbar{display:flex;gap:0.5rem;flex-wrap:wrap;margin:0.75rem 0 1.25rem;}
#empty{color:var(--dim);}

.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:0.75rem;margin-bottom:1.25rem;}
.card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:0.75rem 0.9rem;}
.card-label{font-size:0.78rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
.card-value{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;
            font-size:1.5rem;margin:0.15rem 0;}
.card-value .unit{font-size:0.85rem;color:var(--dim);margin-left:0.15rem;}
.card-sub{font-size:0.75rem;color:var(--dim);min-height:1.1em;}
.spark{width:100%;height:34px;display:block;margin-top:0.3rem;}

.panels{display:grid;grid-template-columns:minmax(220px,320px) 1fr;gap:0.75rem;margin-bottom:1.25rem;}
@media (max-width:640px){ .panels{grid-template-columns:1fr;} }
.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:0.9rem;}
.panel h4{margin:0 0 0.5rem;font-size:0.85rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
.compass-wrap{display:flex;flex-direction:column;align-items:center;}
.compass-readout{text-align:center;margin-top:0.4rem;font-family:ui-monospace,Consolas,monospace;
                  font-variant-numeric:tabular-nums;}
.compass-readout b{font-size:1.2rem;}
.compass-readout .dim{color:var(--dim);font-size:0.8rem;}

.record-row{padding:0.45rem 0;}
.record-row+.record-row{border-top:1px solid var(--border);}
.record-head{display:flex;justify-content:space-between;font-size:0.85rem;}
.record-head b{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;}
.record-track{position:relative;height:6px;border-radius:3px;background:var(--border);margin:0.35rem 0;}
.record-fill{position:absolute;top:-3px;width:10px;height:12px;border-radius:3px;
             transform:translateX(-50%);border:2px solid var(--panel);}
.record-minmax{display:flex;justify-content:space-between;font-size:0.72rem;color:var(--dim);}

.chart{background:var(--panel);border:1px solid var(--border);border-radius:10px;
       padding:0.75rem 0.75rem 0.5rem;margin-bottom:0.9rem;}
.chart h4{margin:0 0 0.4rem;font-size:0.85rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
canvas.trend{width:100%;height:170px;display:block;}
</style>
</head>
<body>
<nav>
  <a href="/status" data-i18n="navStatus">Status</a><a href="/api/log" data-i18n="navLog">Log</a><a href="/api/settings" data-i18n="navSettings">Settings</a><a href="/help" data-i18n="navHelp">Help / Documentation</a>
</nav>
<div class="head"><h3 data-i18n="heading">Measurement history</h3><span class="sub" id="coverage"></span></div>
<p id="empty" data-i18n="loading">Loading...</p>
<div class="toolbar">
  <select id="lang" aria-label="Language"></select>
  <button id="range" data-i18n="range24h">24h</button>
  <button id="timeMode" data-i18n="timeAbsolute">Time: Absolute</button>
  <select id="autoRefresh" aria-label="Auto-refresh interval"></select>
  <button id="units" data-i18n="unitsMetric">Units: Metric</button>
  <button id="theme" data-i18n="toggleTheme">Toggle theme</button>
</div>

<div id="cards" class="cards"></div>

<div class="panels">
  <div class="panel compass-wrap">
    <h4 data-i18n="wind">Wind</h4>
    <canvas id="compass" width="220" height="220" style="width:100%;max-width:220px;height:auto;"></canvas>
    <div class="compass-readout" id="compassText"></div>
  </div>
  <div class="panel">
    <h4 data-i18n="records">Records (min &middot; avg &middot; max)</h4>
    <div id="records"></div>
  </div>
</div>

<div id="charts"></div>

<script>
(function () {
  var root = document.documentElement;
  var stored = localStorage.getItem('wnTheme');
  if (stored) root.setAttribute('data-theme', stored);
  document.getElementById('theme').addEventListener('click', function () {
    var dark = matchMedia('(prefers-color-scheme: dark)').matches;
    var cur = root.getAttribute('data-theme') || (dark ? 'dark' : 'light');
    var next = cur === 'dark' ? 'light' : 'dark';
    root.setAttribute('data-theme', next);
    localStorage.setItem('wnTheme', next);
  });
})();

// --- Localization (en/de/fr/es/pt) — pure client-side UI text, persisted in
// localStorage like the theme. Server-side JSON stays language-neutral
// (field names, *Unit labels like "km/h"/"hPa") — only this page's own
// chrome is translated. ---
var I18N = {
  en: { navStatus:'Status', navLog:'Log', navSettings:'Settings', navHelp:'Help / Documentation',
        heading:'Measurement history', buffered:'of history buffered', loading:'Loading...',
        noHistory:'No history yet — check back after the next measurement cycle.',
        toggleTheme:'Toggle theme', range24h:'24h', range7d:'7 days',
        timeAbsolute:'Time: Absolute', timeRelative:'Time: Relative', agoTemplate:'{n} ago',
        refreshOff:'Off',
        unitsMetric:'Units: Metric', unitsImperial:'Units: Imperial',
        temperature:'Temperature', humidity:'Humidity', pressure:'Pressure', pressureQnh:'Pressure (QNH)',
        wind:'Wind', windChart:'Wind speed / gust', rainRate:'Rain rate', rainAmount:'Rain amount', battery:'Battery',
        ds18b20:'Ext. temp', ds18b20Chart:'External temperature sensors',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Gust', speed:'Speed', min:'Min', max:'Max', records:'Records (min · avg · max)',
        noData:'No data yet', noDataShort:'no data', from:'from',
        failedLoad:'Failed to load history: ',
        dirs:['N','E','S','W'] },
  de: { navStatus:'Status', navLog:'Protokoll', navSettings:'Einstellungen', navHelp:'Hilfe / Dokumentation',
        heading:'Messverlauf', buffered:'Verlauf gespeichert', loading:'Lädt...',
        noHistory:'Noch keine Daten — nach dem nächsten Messzyklus wieder vorbeischauen.',
        toggleTheme:'Darstellung wechseln', range24h:'24h', range7d:'7 Tage',
        timeAbsolute:'Zeit: Absolut', timeRelative:'Zeit: Relativ', agoTemplate:'vor {n}',
        refreshOff:'Aus',
        unitsMetric:'Einheiten: Metrisch', unitsImperial:'Einheiten: Imperial',
        temperature:'Temperatur', humidity:'Luftfeuchte', pressure:'Luftdruck', pressureQnh:'Luftdruck (QNH)',
        wind:'Wind', windChart:'Windgeschwindigkeit / Böen', rainRate:'Regenrate', rainAmount:'Niederschlag', battery:'Batterie',
        ds18b20:'Außentemp.', ds18b20Chart:'Externe Temperatursensoren',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Böe', speed:'Geschwindigkeit', min:'Min', max:'Max', records:'Rekorde (Min · Ø · Max)',
        noData:'Noch keine Daten', noDataShort:'keine Daten', from:'aus',
        failedLoad:'Verlauf konnte nicht geladen werden: ',
        dirs:['N','O','S','W'] },
  fr: { navStatus:'État', navLog:'Journal', navSettings:'Paramètres', navHelp:'Aide / Documentation',
        heading:'Historique des mesures', buffered:"d'historique en mémoire", loading:'Chargement...',
        noHistory:'Pas encore de données — revenez après le prochain cycle de mesure.',
        toggleTheme:'Changer le thème', range24h:'24h', range7d:'7 jours',
        timeAbsolute:'Heure : Absolue', timeRelative:'Heure : Relative', agoTemplate:'il y a {n}',
        refreshOff:'Désactivé',
        unitsMetric:'Unités : Métrique', unitsImperial:'Unités : Impérial',
        temperature:'Température', humidity:'Humidité', pressure:'Pression', pressureQnh:'Pression (QNH)',
        wind:'Vent', windChart:'Vitesse du vent / rafales', rainRate:'Intensité de pluie', rainAmount:'Quantité de pluie', battery:'Batterie',
        ds18b20:'Temp. ext.', ds18b20Chart:'Sondes de température externes',
        probe0:'Capteur 1', probe1:'Capteur 2', probe2:'Capteur 3',
        gust:'Rafale', speed:'Vitesse', min:'Min', max:'Max', records:'Records (min · moy · max)',
        noData:'Pas encore de données', noDataShort:'aucune donnée', from:'de',
        failedLoad:"Échec du chargement de l'historique : ",
        dirs:['N','E','S','O'] },
  es: { navStatus:'Estado', navLog:'Registro', navSettings:'Ajustes', navHelp:'Ayuda / Documentación',
        heading:'Historial de mediciones', buffered:'de historial almacenado', loading:'Cargando...',
        noHistory:'Aún no hay datos — vuelve después del próximo ciclo de medición.',
        toggleTheme:'Cambiar tema', range24h:'24h', range7d:'7 días',
        timeAbsolute:'Hora: Absoluta', timeRelative:'Hora: Relativa', agoTemplate:'hace {n}',
        refreshOff:'Desactivado',
        unitsMetric:'Unidades: Métrico', unitsImperial:'Unidades: Imperial',
        temperature:'Temperatura', humidity:'Humedad', pressure:'Presión', pressureQnh:'Presión (QNH)',
        wind:'Viento', windChart:'Velocidad del viento / rachas', rainRate:'Intensidad de lluvia', rainAmount:'Cantidad de lluvia', battery:'Batería',
        ds18b20:'Temp. ext.', ds18b20Chart:'Sensores de temperatura externos',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Racha', speed:'Velocidad', min:'Mín', max:'Máx', records:'Récords (mín · media · máx)',
        noData:'Aún no hay datos', noDataShort:'sin datos', from:'desde',
        failedLoad:'No se pudo cargar el historial: ',
        dirs:['N','E','S','O'] },
  pt: { navStatus:'Estado', navLog:'Registro', navSettings:'Configurações', navHelp:'Ajuda / Documentação',
        heading:'Histórico de medições', buffered:'de histórico armazenado', loading:'Carregando...',
        noHistory:'Ainda sem dados — volte após o próximo ciclo de medição.',
        toggleTheme:'Alternar tema', range24h:'24h', range7d:'7 dias',
        timeAbsolute:'Hora: Absoluta', timeRelative:'Hora: Relativa', agoTemplate:'há {n}',
        refreshOff:'Desativado',
        unitsMetric:'Unidades: Métrico', unitsImperial:'Unidades: Imperial',
        temperature:'Temperatura', humidity:'Umidade', pressure:'Pressão', pressureQnh:'Pressão (QNH)',
        wind:'Vento', windChart:'Velocidade do vento / rajadas', rainRate:'Intensidade de chuva', rainAmount:'Quantidade de chuva', battery:'Bateria',
        ds18b20:'Temp. ext.', ds18b20Chart:'Sensores de temperatura externos',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Rajada', speed:'Velocidade', min:'Mín', max:'Máx', records:'Registros (mín · média · máx)',
        noData:'Ainda sem dados', noDataShort:'sem dados', from:'de',
        failedLoad:'Falha ao carregar histórico: ',
        dirs:['N','E','S','O'] }
};
var SUPPORTED_LANGS = ['en', 'de', 'fr', 'es', 'pt'];
var LANG_NAMES = { en:'English', de:'Deutsch', fr:'Français', es:'Español', pt:'Português' };

function detectLang() {
  var stored = localStorage.getItem('wnLang');
  if (stored && SUPPORTED_LANGS.indexOf(stored) !== -1) return stored;
  var browserLang = (navigator.language || 'en').slice(0, 2).toLowerCase();
  return SUPPORTED_LANGS.indexOf(browserLang) !== -1 ? browserLang : 'en';
}
var lang = detectLang();
function t(key) {
  var v = (I18N[lang] || I18N.en)[key];
  if (v === undefined) v = I18N.en[key];
  return v === undefined ? key : v;
}
function translateStatic() {
  document.querySelectorAll('[data-i18n]').forEach(function (el) {
    el.textContent = t(el.getAttribute('data-i18n'));
  });
}

(function () {
  var sel = document.getElementById('lang');
  SUPPORTED_LANGS.forEach(function (code) {
    var opt = document.createElement('option');
    opt.value = code;
    opt.textContent = LANG_NAMES[code];
    sel.appendChild(opt);
  });
  sel.value = lang;
  translateStatic();
  sel.addEventListener('change', function () {
    lang = sel.value;
    localStorage.setItem('wnLang', lang);
    translateStatic();
    translateDynamicTitles();
    updateUnitsButtonLabel();
    updateRangeButtonLabel();
    updateTimeModeLabel();
    updateAutoRefreshOptions();
    if (lastHist) render(lastHist, lastCurrent);
  });
})();

// --- Units (Metric/Imperial) — pure client-side conversion of the values
// already returned by /api/history, using the same formulas as units.cpp;
// the *Unit fields tell us what the device actually sent, so conversion
// only kicks in when that differs from the current preference. ---
var unitPrefStored = localStorage.getItem('wnUnitPref');
var unitPref = unitPrefStored || 'metric'; // corrected to match the device on first-ever load, see render()
function convTemp(v, from, to) {
  if (v === null || v === undefined || from === to) return v;
  var c = from === 'F' ? (v - 32) / 1.8 : v;
  return to === 'F' ? c * 1.8 + 32 : c;
}
function convWind(v, from, to) {
  if (v === null || v === undefined || from === to) return v;
  var kmh = from === 'mph' ? v * 1.60934 : (from === 'm/s' ? v * 3.6 : v);
  return to === 'mph' ? kmh / 1.60934 : (to === 'm/s' ? kmh / 3.6 : kmh);
}
// mm<->in conversion, shared by rainRate (mm/h, in/h) and the rain-amount
// chart's rainMm (mm, in) — checked by prefix rather than an exact 'in/h'
// match so both unit-string shapes work with the same function.
function convMm(v, from, to) {
  if (v === null || v === undefined || from === to) return v;
  var isImpFrom = from.indexOf('in') === 0, isImpTo = to.indexOf('in') === 0;
  var mm = isImpFrom ? v * 25.4 : v;
  return isImpTo ? mm / 25.4 : mm;
}
function convPressure(v, from, to) {
  if (v === null || v === undefined || from === to) return v;
  var hpa = from === 'inHg' ? v * 33.8639 : v;
  return to === 'inHg' ? hpa / 33.8639 : hpa;
}
var UNIT_TARGETS = {
  metric:   { tempUnit: 'C', windUnit: 'km/h', rainRateUnit: 'mm/h', rainUnit: 'mm', pressureUnit: 'hPa' },
  imperial: { tempUnit: 'F', windUnit: 'mph',  rainRateUnit: 'in/h', rainUnit: 'in', pressureUnit: 'inHg' }
};
// Converts either an array (history: one value per bucket) or a single
// number (current reading from /api/data) — same shape either way, so
// applyUnitPref() works unchanged on both without a separate scalar variant.
function convField(v, from, to, fn) {
  if (Array.isArray(v)) return v.map(function (x) { return fn(x, from, to); });
  return fn(v, from, to);
}
function applyUnitPref(raw) {
  var target = UNIT_TARGETS[unitPref];
  var out = Object.assign({}, raw);
  if (raw.tempUnit && raw.tempUnit !== target.tempUnit) {
    out.temp = convField(raw.temp, raw.tempUnit, target.tempUnit, convTemp);
    // ds18b20_0/_1/_2: history.cpp's per-probe trend arrays, unit-tagged by
    // the same shared tempUnit field as `temp` (no separate ds18b20Unit in
    // /api/history — see history.cpp's serializeTier). Only present on
    // rawHist; convField(undefined, ...) is a harmless no-op on rawCurrent.
    out.ds18b20_0 = convField(raw.ds18b20_0, raw.tempUnit, target.tempUnit, convTemp);
    out.ds18b20_1 = convField(raw.ds18b20_1, raw.tempUnit, target.tempUnit, convTemp);
    out.ds18b20_2 = convField(raw.ds18b20_2, raw.tempUnit, target.tempUnit, convTemp);
    out.tempUnit = target.tempUnit;
  }
  // /api/data's `ds18b20` array has its own ds18b20Unit sibling (set
  // whenever a probe is present, independent of whether BME280 is — see
  // json_payload.cpp), separate from tempUnit above since tempUnit is only
  // emitted when BME280 itself is present.
  if (raw.ds18b20Unit && raw.ds18b20Unit !== target.tempUnit) {
    out.ds18b20 = convField(raw.ds18b20, raw.ds18b20Unit, target.tempUnit, convTemp);
    out.ds18b20Unit = target.tempUnit;
  }
  if (raw.windUnit && raw.windUnit !== target.windUnit) {
    out.windSpeed = convField(raw.windSpeed, raw.windUnit, target.windUnit, convWind);
    out.windGust  = convField(raw.windGust,  raw.windUnit, target.windUnit, convWind);
    out.windUnit = target.windUnit;
  }
  if (raw.rainRateUnit && raw.rainRateUnit !== target.rainRateUnit) {
    out.rainRate = convField(raw.rainRate, raw.rainRateUnit, target.rainRateUnit, convMm);
    out.rainRateUnit = target.rainRateUnit;
  }
  if (raw.rainUnit && raw.rainUnit !== target.rainUnit) {
    // rainMm: history buffer's bucketed amount. rain: /api/data's this-
    // cycle amount (the "live" reading the rain-amount card's currentKey
    // points at) — same unit, different field name, both need converting.
    out.rainMm = convField(raw.rainMm, raw.rainUnit, target.rainUnit, convMm);
    out.rain   = convField(raw.rain,   raw.rainUnit, target.rainUnit, convMm);
    out.rainUnit = target.rainUnit;
  }
  if (raw.pressureUnit && raw.pressureUnit !== target.pressureUnit) {
    out.pressureQnh = convField(raw.pressureQnh, raw.pressureUnit, target.pressureUnit, convPressure);
    out.pressureUnit = target.pressureUnit;
  }
  return out;
}
function updateUnitsButtonLabel() {
  document.getElementById('units').textContent = t(unitPref === 'imperial' ? 'unitsImperial' : 'unitsMetric');
}
document.getElementById('units').addEventListener('click', function () {
  unitPref = unitPref === 'imperial' ? 'metric' : 'imperial';
  unitPrefStored = unitPref;
  localStorage.setItem('wnUnitPref', unitPref);
  updateUnitsButtonLabel();
  if (lastHist) render(lastHist, lastCurrent);
});

// --- Time range (24h fine tier / 7d coarse tier) — unlike theme/units/lang,
// this can't be redrawn from cached data: the two tiers are genuinely
// separate server-side ring buffers (see history.cpp), so switching means a
// real re-fetch via ?range=7d, not just a local re-render. ---
var timeRange = localStorage.getItem('wnRange') || '24h';
function updateRangeButtonLabel() {
  document.getElementById('range').textContent = t(timeRange === '7d' ? 'range7d' : 'range24h');
}
document.getElementById('range').addEventListener('click', function () {
  timeRange = timeRange === '7d' ? '24h' : '7d';
  localStorage.setItem('wnRange', timeRange);
  updateRangeButtonLabel();
  refresh();
});

// --- Chart x-axis: absolute time (default) vs. relative "ago" — a pure
// redraw, not a re-fetch: the device has no synced wall clock at all (no
// NTP anywhere in this project), but the *browser* does, so "absolute" just
// takes Date.now() at render time as the anchor and subtracts each point's
// ageSec — no device-side timestamp needed. Locale-aware for free via
// toLocaleTimeString/toLocaleDateString, matching whichever of the 5
// languages is currently selected (not necessarily the browser's own). ---
var LOCALES = { en: 'en-US', de: 'de-DE', fr: 'fr-FR', es: 'es-ES', pt: 'pt-BR' };
var timeMode = localStorage.getItem('wnTimeMode') || 'absolute';
function updateTimeModeLabel() {
  document.getElementById('timeMode').textContent = t(timeMode === 'absolute' ? 'timeAbsolute' : 'timeRelative');
}
document.getElementById('timeMode').addEventListener('click', function () {
  timeMode = timeMode === 'absolute' ? 'relative' : 'absolute';
  localStorage.setItem('wnTimeMode', timeMode);
  updateTimeModeLabel();
  if (lastHist) render(lastHist, lastCurrent);
});

// --- Auto-refresh interval — a fixed set of steps (Grafana-style), not a
// free-form number: Wind/rain/BME280 readings actually do update every
// sampleSec (see takeSample() in main.cpp — it overwrites windSpeedMs/
// bme280TempC/rainMM etc. with a fresh single-sample reading, not just once
// per uplinkIntervalSec), so a short interval can be genuinely useful, not
// just extra load for no benefit. "Off" (0) is one of the steps rather than
// a separate toggle, so pausing (e.g. to read a chart without it jumping)
// and picking a cadence are the same one control.
var REFRESH_STEPS_SEC = [0, 1, 5, 10, 30, 60];
var autoRefreshSec = parseInt(localStorage.getItem('wnAutoRefreshSec'), 10);
if (isNaN(autoRefreshSec) || REFRESH_STEPS_SEC.indexOf(autoRefreshSec) === -1) autoRefreshSec = 10;
var refreshTimer = null;
// durationLabel() (defined further below) already renders "1s"/"30s"/"1m"
// in the plain numeric style this dropdown wants — reused here instead of
// duplicating that formatting. Safe to call before its own declaration:
// it's a function declaration, hoisted for the whole script.
function refreshStepLabel(sec) { return sec === 0 ? t('refreshOff') : durationLabel(sec); }
(function () {
  var sel = document.getElementById('autoRefresh');
  REFRESH_STEPS_SEC.forEach(function (sec) {
    var opt = document.createElement('option');
    opt.value = sec;
    opt.textContent = refreshStepLabel(sec);
    sel.appendChild(opt);
  });
  sel.value = autoRefreshSec;
  sel.addEventListener('change', function () {
    autoRefreshSec = parseInt(sel.value, 10);
    localStorage.setItem('wnAutoRefreshSec', String(autoRefreshSec));
    applyAutoRefreshTimer();
    if (autoRefreshSec > 0) refresh();
  });
})();
// Only the "Off" option's text is actually language-dependent — the numeric
// steps ("5s", "1m", ...) read the same in all 5 languages — but rebuilding
// every option's text uniformly is simpler than special-casing just one.
function updateAutoRefreshOptions() {
  var sel = document.getElementById('autoRefresh');
  for (var i = 0; i < sel.options.length; i++) {
    sel.options[i].textContent = refreshStepLabel(parseInt(sel.options[i].value, 10));
  }
}
function applyAutoRefreshTimer() {
  if (refreshTimer) { clearInterval(refreshTimer); refreshTimer = null; }
  if (autoRefreshSec > 0) refreshTimer = setInterval(refresh, autoRefreshSec * 1000);
}

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
function durationLabel(sec) {
  if (sec < 60) return sec + 's';
  if (sec < 3600) return Math.round(sec / 60) + 'm';
  return (sec / 3600).toFixed(1) + 'h';
}
// Relative: reuses durationLabel() for the "14m" part, wrapped in the
// current language's ago-template (a suffix in English, a prefix in the
// other four — see agoTemplate in I18N above).
function agoLabel(sec) {
  return t('agoTemplate').replace('{n}', durationLabel(sec));
}
// Absolute: anchorMs is the browser's Date.now() captured once per render()
// call (not per tick), so every point on a given chart draw is consistent
// with every other point even though computing them isn't instantaneous.
// Beyond 24h (relevant for the 7-day range) a short weekday is added, since
// a bare time-of-day would otherwise be ambiguous about which day.
function absoluteLabel(sec, anchorMs) {
  var d = new Date(anchorMs - sec * 1000);
  var locale = LOCALES[lang] || 'en-US';
  var time = d.toLocaleTimeString(locale, { hour: '2-digit', minute: '2-digit' });
  if (sec < 86400) return time;
  return d.toLocaleDateString(locale, { weekday: 'short' }) + ' ' + time;
}
function xAxisLabel(sec, anchorMs) {
  return timeMode === 'absolute' ? absoluteLabel(sec, anchorMs) : agoLabel(sec);
}
function polar(cx, cy, r, bearingDeg) {
  var rad = (bearingDeg - 90) * Math.PI / 180;
  return { x: cx + Math.cos(rad) * r, y: cy + Math.sin(rad) * r };
}

// --- Metric cards (big number + sparkline), matching the reference's tile layout ---
var CARDS = [
  { key: 'temp',        titleKey: 'temperature', unitKey: 'tempUnit',     color: '#e2694f', isTemp: true },
  { key: 'humidityPct',  titleKey: 'humidity',    unit: '%',               color: '#4aa8e0' },
  { key: 'pressureQnh', titleKey: 'pressure',    unitKey: 'pressureUnit', color: '#59b370' },
  { key: 'windSpeed',   titleKey: 'wind',        unitKey: 'windUnit',     color: '#4a90d9', secondaryKey: 'windGust', secondaryLabelKey: 'gust' },
  // "Live" value comes from /api/data's `rain` (this-cycle amount, not a
  // rate) via currentKey, but the sparkline/records still track the
  // history buffer's bucketed `rainMm` — the two fields aren't the same
  // key, hence currentKey as an override of which field render() reads
  // from `current` (defaults to `key` for every other card).
  { key: 'rainMm', currentKey: 'rain', titleKey: 'rainAmount', unitKey: 'rainUnit', color: '#2f6690' },
  { key: 'batteryVolt', titleKey: 'battery',     unit: 'V',               color: '#e0a63e' },
  // DS18B20 probe 0 only — the other two probes (if present) show up in the
  // ds18b20 trend chart's legend but don't get their own card (three
  // near-identical temperature tiles would crowd the row for a sensor most
  // deployments only use one of). data.ds18b20_0 is a real /api/history
  // field (history.cpp); current.ds18b20_0 is derived from /api/data's
  // slot-indexed ds18b20 array — see the preprocessing step in render().
  { key: 'ds18b20_0',   titleKey: 'ds18b20',     unitKey: 'ds18b20Unit',  color: '#c96fa8', isTemp: true }
];
var cardsEl = document.getElementById('cards'), cardRefs = {};
CARDS.forEach(function (c) {
  var div = document.createElement('div');
  div.className = 'card';
  div.innerHTML =
    '<div class="card-label"></div>' +
    '<div class="card-value"><span class="num">--</span><span class="unit"></span></div>' +
    '<div class="card-sub"></div>' +
    '<canvas class="spark"></canvas>';
  cardsEl.appendChild(div);
  cardRefs[c.key] = {
    container: div,
    label: div.querySelector('.card-label'),
    num: div.querySelector('.num'), unit: div.querySelector('.unit'),
    sub: div.querySelector('.card-sub'), spark: div.querySelector('.spark')
  };
});

function drawSpark(canvas, values, color) {
  var w = canvas.clientWidth || 160, h = 34;
  canvas.width = w; canvas.height = h;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  var vals = values.filter(function (v) { return v !== null && v !== undefined; });
  if (vals.length < 2) return;
  var min = Math.min.apply(null, vals), max = Math.max.apply(null, vals);
  if (min === max) { min -= 1; max += 1; }
  var n = values.length;
  function x(i) { return (w * i) / (n - 1); }
  function y(v) { return 2 + (h - 4) - ((v - min) / (max - min)) * (h - 4); }
  ctx.beginPath();
  var started = false, firstX = 0, lastX = 0;
  for (var i = 0; i < n; i++) {
    var v = values[i];
    if (v === null || v === undefined) { started = false; continue; }
    var xx = x(i), yy = y(v);
    if (!started) { ctx.moveTo(xx, yy); firstX = xx; started = true; } else ctx.lineTo(xx, yy);
    lastX = xx;
  }
  ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.stroke();
  ctx.lineTo(lastX, h); ctx.lineTo(firstX, h); ctx.closePath();
  ctx.globalAlpha = 0.12; ctx.fillStyle = color; ctx.fill(); ctx.globalAlpha = 1;
}

// --- Wind compass + rose, built from the same windDirDeg history already
// returned by /api/history — no extra backend data needed. ---
// `data` (history array, for the rose histogram — deliberately still the
// *full* selected-range buffer, so the direction distribution reflects
// whatever range is currently shown) and `current` (the /api/data snapshot,
// for the needle/readout, so it never goes stale by up to a whole bucket).
function drawCompass(data, current) {
  var canvas = document.getElementById('compass');
  var w = canvas.width, h = canvas.height, cx = w / 2, cy = h / 2, R = Math.min(w, h) / 2 - 22;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  var dim = cssVar('--dim'), border = cssVar('--border'), accent = cssVar('--accent');

  ctx.strokeStyle = border; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.arc(cx, cy, R, 0, 2 * Math.PI); ctx.stroke();

  var dirHistory = data.windDirDeg || [];
  var bins = new Array(16).fill(0);
  dirHistory.forEach(function (d) {
    if (d === null || d === undefined) return;
    bins[Math.floor(((d + 11.25) % 360) / 22.5)]++;
  });
  var maxBin = Math.max.apply(null, bins) || 1;
  ctx.fillStyle = accent; ctx.globalAlpha = 0.28;
  for (var i = 0; i < 16; i++) {
    if (!bins[i]) continue;
    var r = (bins[i] / maxBin) * (R - 8);
    var a0 = (i * 22.5 - 9 - 90) * Math.PI / 180, a1 = (i * 22.5 + 9 - 90) * Math.PI / 180;
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.arc(cx, cy, r, a0, a1); ctx.closePath(); ctx.fill();
  }
  ctx.globalAlpha = 1;

  ctx.fillStyle = dim; ctx.font = '11px sans-serif'; ctx.textAlign = 'center';
  var dirLabels = t('dirs');
  [[dirLabels[0], 0], [dirLabels[1], 90], [dirLabels[2], 180], [dirLabels[3], 270]].forEach(function (d) {
    var p = polar(cx, cy, R + 12, d[1]);
    ctx.fillText(d[0], p.x, p.y + 4);
  });

  var curDir = current.windDirDeg === undefined ? null : current.windDirDeg;
  var curSpeed = current.windSpeed === undefined ? null : current.windSpeed;
  var curGust = current.windGust === undefined ? null : current.windGust;
  var unit = current.windUnit || data.windUnit || '';
  if (curDir !== null) {
    var tip = polar(cx, cy, R - 10, curDir);
    ctx.strokeStyle = accent; ctx.lineWidth = 2.5; ctx.lineCap = 'round';
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(tip.x, tip.y); ctx.stroke();
    ctx.fillStyle = accent; ctx.beginPath(); ctx.arc(cx, cy, 3.5, 0, 2 * Math.PI); ctx.fill();
  }
  document.getElementById('compassText').innerHTML =
    (curSpeed === null ? '<b>--</b>' : '<b>' + curSpeed.toFixed(1) + '</b> ' + unit) +
    '<div class="dim">' + (curDir === null ? t('noDataShort') : t('from') + ' ' + Math.round(curDir) + '&deg;') +
    (curGust === null ? '' : ' &middot; ' + t('gust') + ' ' + curGust.toFixed(1) + ' ' + unit) + '</div>';
}

// --- Records: min / avg / max across the fetched buffer, per metric ---
function buildRecords(data) {
  var el = document.getElementById('records');
  el.innerHTML = '';
  CARDS.forEach(function (c) {
    var vals = (data[c.key] || []).filter(function (v) { return v !== null && v !== undefined; });
    if (!vals.length) return;
    var min = Math.min.apply(null, vals), max = Math.max.apply(null, vals);
    var avg = vals.reduce(function (a, b) { return a + b; }, 0) / vals.length;
    var unit = (c.isTemp ? '°' : '') + (c.unit || data[c.unitKey] || '');
    var pct = max > min ? ((avg - min) / (max - min)) * 100 : 50;
    var row = document.createElement('div');
    row.className = 'record-row';
    row.innerHTML =
      '<div class="record-head"><span>' + t(c.titleKey) + '</span><b>' + avg.toFixed(1) + ' ' + unit + '</b></div>' +
      '<div class="record-track"><div class="record-fill" style="left:' + pct + '%;background:' + c.color + '"></div></div>' +
      '<div class="record-minmax"><span>' + t('min') + ' ' + min.toFixed(1) + '</span><span>' + t('max') + ' ' + max.toFixed(1) + '</span></div>';
    el.appendChild(row);
  });
}

// --- Full trend charts: gridlines, area fill (single-series), legend
// (multi-series), several relative-time x-axis ticks. ---
var CHARTS = [
  { id: 'temp',     titleKey: 'temperature', series: [{ key: 'temp', color: '#e2694f' }], unitKey: 'tempUnit' },
  { id: 'humidity', titleKey: 'humidity',    series: [{ key: 'humidityPct', color: '#4aa8e0' }], unit: '%' },
  { id: 'pressure', titleKey: 'pressureQnh', series: [{ key: 'pressureQnh', color: '#59b370' }], unitKey: 'pressureUnit' },
  { id: 'wind',     titleKey: 'windChart',   series: [{ key: 'windSpeed', color: '#4a90d9', labelKey: 'speed' }, { key: 'windGust', color: '#a468c9', labelKey: 'gust' }], unitKey: 'windUnit', minZero: true },
  { id: 'rain',     titleKey: 'rainRate',    series: [{ key: 'rainRate', color: '#3f7fc1' }], unitKey: 'rainRateUnit', minZero: true },
  { id: 'rainMm',   titleKey: 'rainAmount',  series: [{ key: 'rainMm', color: '#2f6690' }], unitKey: 'rainUnit', minZero: true },
  { id: 'battery',  titleKey: 'battery',     series: [{ key: 'batteryVolt', color: '#e0a63e' }], unit: 'V' },
  { id: 'ds18b20',  titleKey: 'ds18b20Chart',
    series: [{ key: 'ds18b20_0', color: '#c96fa8', labelKey: 'probe0' },
             { key: 'ds18b20_1', color: '#7a5fc4', labelKey: 'probe1' },
             { key: 'ds18b20_2', color: '#3f9b8c', labelKey: 'probe2' }],
    unitKey: 'tempUnit' }
];
var chartsEl = document.getElementById('charts'), canvases = {}, chartTitleEls = {}, chartContainerEls = {};
CHARTS.forEach(function (c) {
  var div = document.createElement('div');
  div.className = 'chart';
  div.innerHTML = '<h4></h4><canvas class="trend"></canvas>';
  chartsEl.appendChild(div);
  canvases[c.id] = div.querySelector('canvas');
  chartTitleEls[c.id] = div.querySelector('h4');
  chartContainerEls[c.id] = div;
});

// Card labels and chart headings are built once above but their text
// depends on the current language, so it's set here (and again on every
// language change) rather than baked in at creation time.
function translateDynamicTitles() {
  CARDS.forEach(function (c) { cardRefs[c.key].label.textContent = t(c.titleKey); });
  CHARTS.forEach(function (c) { chartTitleEls[c.id].textContent = t(c.titleKey); });
}
translateDynamicTitles();

function drawChart(canvas, ageSec, series, unit, anchorMs, minZero) {
  var w = canvas.clientWidth || 900, h = 170;
  canvas.width = w; canvas.height = h;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  var pad = { l: 48, r: 10, t: 8, b: 20 };
  var plotW = w - pad.l - pad.r, plotH = h - pad.t - pad.b;
  var grid = cssVar('--border'), dim = cssVar('--dim');

  var allVals = [];
  series.forEach(function (s) { s.data.forEach(function (v) { if (v !== null && v !== undefined) allVals.push(v); }); });
  if (!allVals.length) {
    ctx.fillStyle = dim; ctx.font = '12px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText(t('noData'), w / 2, h / 2);
    return;
  }
  var min = Math.min.apply(null, allVals), max = Math.max.apply(null, allVals);
  // minZero: rain rate/amount and wind speed/gust can't physically go
  // negative — force the floor to 0 rather than the symmetric min-1/max+1
  // fallback below, which otherwise shows a nonsensical "-1 to 1" axis
  // while there's no rain yet / it's calm (all-zero data, min === max).
  if (minZero) { min = 0; if (max <= 0) max = 1; }
  else if (min === max) { min -= 1; max += 1; }
  var n = ageSec.length;
  function xAt(i) { return pad.l + (n <= 1 ? 0 : (plotW * i) / (n - 1)); }
  function yAt(v) { return pad.t + plotH - ((v - min) / (max - min)) * plotH; }

  ctx.strokeStyle = grid; ctx.fillStyle = dim; ctx.font = '10px sans-serif'; ctx.textAlign = 'left'; ctx.lineWidth = 1;
  var rows = 4;
  for (var r = 0; r <= rows; r++) {
    var yy = pad.t + (plotH * r) / rows;
    ctx.beginPath(); ctx.moveTo(pad.l, yy); ctx.lineTo(pad.l + plotW, yy); ctx.stroke();
    ctx.fillText((max - ((max - min) * r) / rows).toFixed(1), 2, yy + 3);
  }
  ctx.textAlign = 'center';
  // Named `ticks`/`ti`, not `t` — this function is itself named `t()` (see
  // the i18n helper above); a `var t` loop counter here would shadow it for
  // the whole function body (hoisting), silently breaking the t('noData')
  // call a few lines up.
  var tickCount = 4;
  for (var ti = 0; ti <= tickCount; ti++) {
    var idx = Math.round((n - 1) * ti / tickCount);
    ctx.fillText(xAxisLabel(ageSec[idx], anchorMs), xAt(idx), h - 5);
  }

  series.forEach(function (s) {
    ctx.beginPath();
    var started = false, firstX = 0, lastX = 0;
    for (var i = 0; i < n; i++) {
      var v = s.data[i];
      if (v === null || v === undefined) { started = false; continue; }
      var x = xAt(i), y = yAt(v);
      if (!started) { ctx.moveTo(x, y); firstX = x; started = true; } else ctx.lineTo(x, y);
      lastX = x;
    }
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.75; ctx.stroke();
    if (series.length === 1) {
      ctx.lineTo(lastX, pad.t + plotH); ctx.lineTo(firstX, pad.t + plotH); ctx.closePath();
      ctx.globalAlpha = 0.12; ctx.fillStyle = s.color; ctx.fill(); ctx.globalAlpha = 1;
    }
  });

  if (series.length > 1) {
    var lx = pad.l + plotW - 78, ly = pad.t + 2;
    ctx.textAlign = 'left'; ctx.font = '10px sans-serif';
    series.forEach(function (s, i) {
      ctx.fillStyle = s.color;
      ctx.beginPath(); ctx.arc(lx, ly + 12 * i + 4, 3, 0, 2 * Math.PI); ctx.fill();
      ctx.fillStyle = dim; ctx.fillText(t(s.labelKey) || s.key, lx + 8, ly + 12 * i + 7);
    });
  }
}

var lastHist = null, lastCurrent = null;
// rawHist: /api/history (the selected tier's bucketed arrays — trend/sparklines/
// records/rose). rawCurrent: /api/data (the latest completed cycle's actual
// values — cards' big numbers, compass needle/readout). Split so "now" stays
// as fresh as before even though history buckets can span several minutes
// (see history.cpp) — before this, "now" was just the history array's last
// entry, which went stale for up to a whole bucket on short uplink intervals.
function render(rawHist, rawCurrent) {
  lastHist = rawHist; lastCurrent = rawCurrent;
  // Captured once per render() call (including a pure cache redraw, e.g. a
  // theme/language toggle) rather than once per chart/tick, so absolute
  // time labels stay correct relative to "right now" even between fetches,
  // while every point *within* one draw pass shares the same anchor.
  var nowMs = Date.now();
  // No explicit preference saved yet (unitPrefStored only gets set once the
  // units button is actually clicked): keep mirroring whatever the device
  // is currently configured for on every refresh, rather than freezing in
  // whatever it happened to be on the very first load. Once the user picks
  // a preference themselves, this block stops running and that choice wins.
  if (!unitPrefStored) {
    unitPref = (rawCurrent.tempUnit || rawCurrent.ds18b20Unit) === 'F' ? 'imperial' : 'metric';
    updateUnitsButtonLabel();
  }
  var data = applyUnitPref(rawHist);
  var current = applyUnitPref(rawCurrent);
  // /api/data's `ds18b20` is a slot-indexed array (null for a disconnected
  // probe, see json_payload.cpp), not flat fields like every other current
  // reading — expand it into ds18b20_0/_1/_2 so CARDS/CHARTS can address a
  // probe the same way they address everything else.
  var curDs = current.ds18b20 || [];
  for (var dsI = 0; dsI < 3; dsI++) {
    current['ds18b20_' + dsI] = curDs[dsI] === undefined || curDs[dsI] === null ? null : curDs[dsI];
  }

  var empty = document.getElementById('empty');
  var n = (data.ageSec || []).length;
  empty.style.display = n === 0 ? 'block' : 'none';
  empty.textContent = t('noHistory');
  document.getElementById('coverage').textContent =
    n > 1 ? durationLabel(data.ageSec[0]) + ' ' + t('buffered') : '';

  CARDS.forEach(function (c) {
    var arr = data[c.key] || [];
    var last = current[c.currentKey || c.key];
    if (last === undefined) last = null;
    var ref = cardRefs[c.key];
    // Hide entirely rather than show a permanent "--"/flat sparkline for a
    // sensor that's absent (disabled, or never detected e.g. no BME280 /
    // no DS18B20 probe wired up) — same "never had data -> not shown" rule
    // buildRecords() already applies below, just generalized to every card.
    var hasData = last !== null || arr.some(function (v) { return v !== null && v !== undefined; });
    ref.container.style.display = hasData ? '' : 'none';
    if (!hasData) return;
    var unit = (c.isTemp ? '°' : '') + (c.unit || current[c.unitKey] || data[c.unitKey] || '');
    ref.num.textContent = last === null ? '--' : last.toFixed(c.key === 'batteryVolt' ? 2 : 1);
    ref.unit.textContent = last === null ? '' : (' ' + unit);
    if (c.secondaryKey) {
      var sec = current[c.secondaryKey];
      if (sec === undefined) sec = null;
      ref.sub.textContent = sec === null ? '' : (t(c.secondaryLabelKey) + ' ' + sec.toFixed(1) + ' ' + unit);
    }
    drawSpark(ref.spark, arr, c.color);
  });

  drawCompass(data, current);
  buildRecords(data);

  CHARTS.forEach(function (c) {
    var series = c.series.map(function (s) { return { color: s.color, labelKey: s.labelKey, data: data[s.key] || [] }; })
      .filter(function (s) { return s.data.some(function (v) { return v !== null && v !== undefined; }); });
    var container = chartContainerEls[c.id];
    // A multi-series chart (wind, ds18b20) keeps whichever series actually
    // have data (e.g. only 1 of 3 DS18B20 probes wired up) and only hides
    // the whole panel once every one of its series is empty.
    if (!series.length) { container.style.display = 'none'; return; }
    container.style.display = '';
    var unit = c.unit || data[c.unitKey] || '';
    drawChart(canvases[c.id], data.ageSec || [], series, unit, nowMs, c.minZero);
  });
}

// The device only advances its sensor readings and appends a new history
// bucket once per uplink cycle (main.cpp calls sendLoRa() then
// pushHistorySample() together, back to back) — at a fast auto-refresh
// setting (down to 1s) most polls would otherwise fetch and fully re-render
// (7 canvas chart redraws, up to 288 points each, plus every sparkline)
// byte-identical data. nextCycleInSec (and, in debug builds, cpuTemp/
// freeHeapKiB/uptimeH) tick every second regardless of the uplink cycle,
// but this page never displays them (see /status for those) — excluded
// here so they can't defeat the comparison.
var lastRenderSig = null;
function renderSignature(histText, current) {
  var c = Object.assign({}, current);
  delete c.nextCycleInSec; delete c.cpuTemp; delete c.freeHeapKiB; delete c.uptimeH;
  return histText + '|' + JSON.stringify(c);
}
function refresh() {
  var histUrl = '/api/history' + (timeRange === '7d' ? '?range=7d' : '');
  Promise.all([
    fetch(histUrl).then(function (r) { return r.text(); }),
    fetch('/api/data').then(function (r) { return r.json(); })
  ]).then(function (results) {
    var sig = renderSignature(results[0], results[1]);
    if (sig === lastRenderSig) return;
    lastRenderSig = sig;
    render(JSON.parse(results[0]), results[1]);
  }).catch(function (e) { document.getElementById('empty').textContent = t('failedLoad') + e; });
}
// Redraw from the already-fetched data on resize (e.g. rotating a phone) —
// no need to re-hit the device's /api/history over a resize gesture.
window.addEventListener('resize', function () { if (lastHist) render(lastHist, lastCurrent); });
// Sync all buttons to whatever was persisted from a previous visit — the
// markup's data-i18n placeholders are only correct for a brand new session
// (translateStatic() alone would leave a returning "imperial"/"7d"/paused
// visitor looking at "Units: Metric"/"24h" until they clicked something).
// autoRefresh doesn't need the same treatment: its <select> options/value
// are built entirely in JS (see its own IIFE above), already correct for
// the persisted step and current language from the moment they're created.
updateUnitsButtonLabel();
updateRangeButtonLabel();
updateTimeModeLabel();
refresh();
applyAutoRefreshTimer();
</script>
</body>
</html>
)HTML";
