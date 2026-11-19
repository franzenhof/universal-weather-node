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
// Layout: warnings/update badges, a "now" block (temperature hero card with
// feels-like, today's min/max and weather trend + one tile per measured
// value), then rain / derived values / system panels, the wireless sensors
// (signal, battery, age, which value comes from which sensor), records and
// the trend charts. Every card, panel, row and chart is only shown while its
// JSON key exists, so a minimal wireless build (no PCB, no LoRaWAN) looks
// complete rather than full of empty boxes.
//
// Localized (en/de/fr/es/pt, see the I18N object below) and unit-switchable
// (Metric/Imperial) — both are pure client-side view preferences, persisted
// in localStorage like the theme, and deliberately don't touch the device's
// own gCfg.unitSystem: that field is a device-wide setting (also affects
// the LoRaWAN payload and MQTT/Home Assistant), whereas this is just "how
// I, this one visitor, want to *look at* the already-fetched history" —
// conflating the two would mean anyone opening /chart changes what every
// other consumer of this node's data sees.
//
// Notes on the script (its comments are kept short — they ship in flash):
// - unit conversion uses the same formulas as units.cpp and only applies when
//   a *Unit field differs from the chosen system; /api/history's *Unit
//   fields always exist, /api/data's only while that value does;
// - the absolute time axis is the browser's clock minus each point's ageSec
//   (the history carries no timestamps);
// - the device only produces new data once per cycle, so identical fetches
//   (fast auto-refresh) are not re-rendered; nextCycleInSec and the debug
//   fields tick every second and are ignored for that comparison;
// - the dew point history and the hourly/daily rain bars are computed here
//   from the existing history arrays, no extra backend data.
static const char CHART_PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WeatherNode</title>
<style>
:root{ --bg:#f4f6f8; --panel:#ffffff; --border:#dde3e9; --text:#1a2028; --dim:#5f6d79; --accent:#0f9d94; --ok:#1d7f46; --bad:#c0392b; --warn:#a86400; }
@media (prefers-color-scheme: dark){
:root{ --bg:#12161b; --panel:#1a2028; --border:#2a323c; --text:#e7ecf1; --dim:#8f9cab; --accent:#3fd0c9; --ok:#4cc38a; --bad:#ff7b72; --warn:#f0a541; }
}
:root[data-theme="dark"]{ --bg:#12161b; --panel:#1a2028; --border:#2a323c; --text:#e7ecf1; --dim:#8f9cab; --accent:#3fd0c9; --ok:#4cc38a; --bad:#ff7b72; --warn:#f0a541; }
:root[data-theme="light"]{ --bg:#f4f6f8; --panel:#ffffff; --border:#dde3e9; --text:#1a2028; --dim:#5f6d79; --accent:#0f9d94; --ok:#1d7f46; --bad:#c0392b; --warn:#a86400; }

*{box-sizing:border-box;}
body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);
max-width:1120px;margin:0 auto;padding:0 1rem 2rem;}
nav{display:flex;flex-wrap:wrap;gap:0.4rem 1.1rem;padding:0.9rem 0;border-bottom:1px solid var(--border);margin-bottom:1rem;}
nav a{color:var(--dim);text-decoration:none;font-size:0.9rem;}
nav a:hover,nav a:focus{color:var(--accent);}
.head{display:flex;align-items:baseline;justify-content:space-between;gap:0.3rem 1rem;flex-wrap:wrap;margin-bottom:0.25rem;}
.head h3{margin:0;}
.head .sub{color:var(--dim);font-size:0.85rem;}
.head.sec{margin:1.5rem 0 0.75rem;}
button,select{font:inherit;background:var(--panel);color:var(--text);border:1px solid var(--border);
border-radius:6px;padding:0.4rem 0.75rem;cursor:pointer;}
button:hover,select:hover{border-color:var(--accent);}
button:focus-visible,select:focus-visible{outline:2px solid var(--accent);outline-offset:1px;}
.toolbar{display:flex;gap:0.5rem;flex-wrap:wrap;margin:0.75rem 0 1rem;}
#empty{color:var(--dim);}
.dim{color:var(--dim);}
.ok{color:var(--ok);} .bad{color:var(--bad);} .warnc{color:var(--warn);}

.alerts{display:flex;flex-wrap:wrap;gap:0.5rem;margin:0 0 1rem;}
.alert{padding:0.4rem 0.8rem;border-radius:8px;font-size:0.9rem;font-weight:600;border:1px solid;}
.al-frost{background:rgba(59,130,246,0.14);border-color:rgba(59,130,246,0.6);}
.al-fog{background:rgba(128,138,150,0.18);border-color:rgba(128,138,150,0.7);}
.al-sultry{background:rgba(234,140,40,0.16);border-color:rgba(234,140,40,0.7);}
.al-bad{background:rgba(220,60,60,0.14);border-color:rgba(220,60,60,0.65);}
.al-info{background:rgba(15,157,148,0.12);border-color:rgba(15,157,148,0.6);font-weight:500;}

.top{display:grid;grid-template-columns:minmax(260px,1fr) 2fr;gap:0.75rem;margin-bottom:0.75rem;}
.top.nohero{grid-template-columns:1fr;}
@media (max-width:720px){ .top{grid-template-columns:1fr;} }
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:0.75rem;}
.top:not(.nohero) .cards{grid-template-columns:repeat(var(--cols,3),minmax(0,1fr));}
@media (max-width:720px){ .top:not(.nohero) .cards{grid-template-columns:repeat(2,minmax(0,1fr));} }
.card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:0.75rem 0.9rem;
display:flex;flex-direction:column;min-width:0;}
.card-label{font-size:0.78rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;
white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.card-label::before{content:'';display:inline-block;width:0.5rem;height:0.5rem;border-radius:50%;
background:var(--c);margin-right:0.4rem;vertical-align:0.05em;}
.card-value{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;
font-size:1.5rem;margin:0.15rem 0;white-space:nowrap;}
.card-value .unit{font-size:0.85rem;color:var(--dim);margin-left:0.15rem;}
.card-sub{font-size:0.75rem;color:var(--dim);min-height:1.1em;line-height:1.45;}
.card-sub.sub2:empty{display:none;}
.spark{width:100%;height:34px;display:block;margin-top:auto;padding-top:0.3rem;}
.hero .card-value{font-size:3.3rem;line-height:1.1;margin:0.3rem 0 0.2rem;}
.hero .card-value .unit{font-size:1.2rem;}
.hero .card-sub{font-size:0.85rem;}
.hero .spark{height:70px;}
.hero-top{display:flex;justify-content:space-between;align-items:center;gap:0.5rem;}
.hrange{display:flex;align-items:center;gap:0.5rem;font-size:0.82rem;margin:0.55rem 0 0.3rem;
font-variant-numeric:tabular-nums;white-space:nowrap;}
.hrange .record-track{flex:1;margin:0;}
.chip{display:inline-block;font-size:0.74rem;padding:0.08rem 0.5rem;border-radius:999px;
border:1px solid var(--border);white-space:nowrap;}
.chip.acc{border-color:var(--accent);}
.arr{display:inline-block;}

.panels{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:0.75rem;margin-bottom:0.75rem;}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:0.9rem;min-width:0;}
.panel.blk{margin-bottom:0.75rem;}
.panel h4{margin:0 0 0.5rem;font-size:0.85rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
.compass-wrap{display:flex;flex-direction:column;align-items:center;}
.compass-readout{text-align:center;margin-top:0.4rem;font-family:ui-monospace,Consolas,monospace;
font-variant-numeric:tabular-nums;}
.compass-readout b{font-size:1.2rem;}
.compass-readout .dim{font-size:0.8rem;}
.kv{display:flex;justify-content:space-between;align-items:baseline;gap:0.75rem;padding:0.36rem 0;
font-size:0.88rem;border-top:1px solid var(--border);}
.kv:first-child{border-top:0;}
.kv>span{color:var(--dim);}
.kv b{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;font-weight:600;text-align:right;white-space:nowrap;}
.kv b .chip{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;font-weight:600;margin-left:0.3rem;}

#rfList{display:grid;grid-template-columns:repeat(auto-fill,minmax(290px,1fr));gap:0.6rem;}
.rf{border:1px solid var(--border);border-radius:8px;padding:0.6rem 0.75rem;font-size:0.85rem;}
.rf-head{display:flex;justify-content:space-between;align-items:baseline;gap:0.2rem 0.6rem;flex-wrap:wrap;}
.rf-meta,.rf-vals,.rf-src{display:flex;flex-wrap:wrap;align-items:center;gap:0.3rem 0.8rem;margin-top:0.45rem;}
.rf-vals{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;}
.rf-src{font-size:0.8rem;gap:0.3rem;}
.sig{width:16px;height:13px;vertical-align:-1px;color:var(--accent);}
.st{text-decoration:line-through;color:var(--dim);}

.record-row{padding:0.45rem 0;border-top:1px solid var(--border);}
#records{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));column-gap:1.5rem;}
.record-head{display:flex;justify-content:space-between;font-size:0.85rem;}
.record-head b{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;}
.record-track{position:relative;height:6px;border-radius:3px;background:var(--border);margin:0.35rem 0;}
.record-fill{position:absolute;top:-3px;width:10px;height:12px;border-radius:3px;
transform:translateX(-50%);border:2px solid var(--panel);}
.record-minmax{display:flex;justify-content:space-between;font-size:0.72rem;color:var(--dim);}

.chart{background:var(--panel);border:1px solid var(--border);border-radius:10px;
padding:0.75rem 0.75rem 0.5rem;margin-bottom:0.9rem;}
.chart h4{margin:0 0 0.4rem;font-size:0.85rem;color:var(--dim);text-transform:uppercase;letter-spacing:0.04em;}
canvas.trend{width:100%;height:170px;display:block;cursor:crosshair;touch-action:pan-y;}
.chart{position:relative;}
.tip{position:absolute;display:none;pointer-events:none;z-index:2;background:var(--panel);
border:1px solid var(--border);border-radius:8px;padding:0.45rem 0.6rem;font-size:0.78rem;
box-shadow:0 6px 18px rgba(0,0,0,0.18);white-space:nowrap;}
.tip .when{color:var(--dim);margin-bottom:0.3rem;}
.tip .row{display:flex;align-items:center;gap:0.4rem;line-height:1.5;}
.tip .sw{width:0.6rem;height:0.6rem;border-radius:50%;flex:none;}
.tip b{font-family:ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;margin-left:auto;padding-left:0.9rem;}
</style>
</head>
<body>
<nav>
  <a href="/status" data-i18n="navStatus">Status</a><a href="/api/log" data-i18n="navLog">Log</a><a href="/api/settings" data-i18n="navSettings">Settings</a><a href="/help" data-i18n="navHelp">Help / Documentation</a>
</nav>
<div class="head"><h3 data-i18n="heading">Weather station</h3><span class="sub" id="updated"></span></div>
<div class="toolbar">
  <select id="lang" aria-label="Language"></select>
  <button id="range" data-i18n="range24h">24h</button>
  <button id="timeMode" data-i18n="timeAbsolute">Time: Absolute</button>
  <select id="autoRefresh" aria-label="Auto-refresh interval"></select>
  <button id="units" data-i18n="unitsMetric">Units: Metric</button>
  <button id="theme" data-i18n="toggleTheme">Toggle theme</button>
</div>
<div id="alerts" class="alerts" role="status" aria-live="polite" style="display:none"></div>
<p id="empty" data-i18n="loading">Loading...</p>

<div id="top" class="top"><div id="cards" class="cards"></div></div>

<div class="panels">
  <div class="panel compass-wrap" id="windPanel">
    <h4 data-i18n="wind">Wind</h4>
    <canvas id="compass" width="220" height="220" role="img" aria-label="Wind" style="width:100%;max-width:220px;height:auto;"></canvas>
    <div class="compass-readout" id="compassText"></div>
  </div>
  <div class="panel" id="rainPanel" style="display:none"><h4 data-i18n="rain">Rain</h4><div id="rainList"></div></div>
  <div class="panel" id="morePanel" style="display:none"><h4 data-i18n="more">Derived values</h4><div id="moreList"></div></div>
  <div class="panel" id="sysPanel" style="display:none"><h4 data-i18n="system">System</h4><div id="sysList"></div></div>
</div>
<div class="panel blk" id="rfPanel" style="display:none">
  <h4 data-i18n="rfSensors">Wireless sensors</h4>
  <div id="rfList"></div><div id="rfBoard" class="rf-src"></div>
</div>

<div class="head sec"><h3 data-i18n="history">Measurement history</h3><span class="sub" id="coverage"></span></div>
<div class="panel blk" id="recPanel">
  <h4 data-i18n="records">Records (min &middot; avg &middot; max)</h4>
  <div id="records"></div>
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
    if (lastHist) render(lastHist, lastCurrent);   // canvases read the theme colours at draw time
  });
})();

// --- Localization (en/de/fr/es/pt): this page's own text only, the JSON stays language-neutral ---
var I18N = {
  en: { navStatus:'Status', navLog:'Log', navSettings:'Settings', navHelp:'Help / Documentation',
        heading:'Weather station', history:'Measurement history', buffered:'of history buffered', loading:'Loading...',
        noHistory:'No history yet — check back after the next measurement cycle.',
        toggleTheme:'Toggle theme', range24h:'24h', range7d:'7 days',
        timeAbsolute:'Time: Absolute', timeRelative:'Time: Relative', agoTemplate:'{n} ago',
        refreshOff:'Off',
        unitsMetric:'Units: Metric', unitsImperial:'Units: Imperial',
        temperature:'Temperature', humidity:'Humidity', pressure:'Pressure', pressureQnh:'Pressure (QNH)',
        uvVoltage:'UV sensor voltage', uvIndex:'UV index',
        wind:'Wind', windChart:'Wind speed / gust', rainRate:'Rain rate', rainAmount:'Rain amount', battery:'Battery',
        ds18b20:'Ext. temp', ds18b20Chart:'External temperature sensors',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Gust', speed:'Speed', min:'Min', max:'Max', records:'Records (min · avg · max)',
        noData:'No data yet', noDataShort:'no data', from:'from',
        failedLoad:'Failed to load history: ',
        feelsLike:'Feels like', dewPoint:'Dew point', today:'today', per3h:'/ 3 h',
        direction:'Direction', dirs:['N','E','S','W'] },
  de: { navStatus:'Status', navLog:'Protokoll', navSettings:'Einstellungen', navHelp:'Hilfe / Dokumentation',
        heading:'Wetterstation', history:'Messverlauf', buffered:'Verlauf gespeichert', loading:'Lädt...',
        noHistory:'Noch keine Daten — nach dem nächsten Messzyklus wieder vorbeischauen.',
        toggleTheme:'Darstellung wechseln', range24h:'24h', range7d:'7 Tage',
        timeAbsolute:'Zeit: Absolut', timeRelative:'Zeit: Relativ', agoTemplate:'vor {n}',
        refreshOff:'Aus',
        unitsMetric:'Einheiten: Metrisch', unitsImperial:'Einheiten: Imperial',
        temperature:'Temperatur', humidity:'Luftfeuchte', pressure:'Luftdruck', pressureQnh:'Luftdruck (QNH)',
        uvVoltage:'UV-Sensorspannung', uvIndex:'UV-Index',
        wind:'Wind', windChart:'Windgeschwindigkeit / Böen', rainRate:'Regenrate', rainAmount:'Niederschlag', battery:'Batterie',
        ds18b20:'Außentemp.', ds18b20Chart:'Externe Temperatursensoren',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Böe', speed:'Geschwindigkeit', min:'Min', max:'Max', records:'Rekorde (Min · Ø · Max)',
        noData:'Noch keine Daten', noDataShort:'keine Daten', from:'aus',
        failedLoad:'Verlauf konnte nicht geladen werden: ',
        feelsLike:'Gefühlt', dewPoint:'Taupunkt', today:'heute', per3h:'/ 3 h',
        direction:'Richtung', dirs:['N','O','S','W'] },
  fr: { navStatus:'État', navLog:'Journal', navSettings:'Paramètres', navHelp:'Aide / Documentation',
        heading:'Station météo', history:'Historique des mesures', buffered:"d'historique en mémoire", loading:'Chargement...',
        noHistory:'Pas encore de données — revenez après le prochain cycle de mesure.',
        toggleTheme:'Changer le thème', range24h:'24h', range7d:'7 jours',
        timeAbsolute:'Heure : Absolue', timeRelative:'Heure : Relative', agoTemplate:'il y a {n}',
        refreshOff:'Désactivé',
        unitsMetric:'Unités : Métrique', unitsImperial:'Unités : Impérial',
        temperature:'Température', humidity:'Humidité', pressure:'Pression', pressureQnh:'Pression (QNH)',
        uvVoltage:'Tension du capteur UV', uvIndex:'Indice UV',
        wind:'Vent', windChart:'Vitesse du vent / rafales', rainRate:'Intensité de pluie', rainAmount:'Quantité de pluie', battery:'Batterie',
        ds18b20:'Temp. ext.', ds18b20Chart:'Sondes de température externes',
        probe0:'Capteur 1', probe1:'Capteur 2', probe2:'Capteur 3',
        gust:'Rafale', speed:'Vitesse', min:'Min', max:'Max', records:'Records (min · moy · max)',
        noData:'Pas encore de données', noDataShort:'aucune donnée', from:'de',
        failedLoad:"Échec du chargement de l'historique : ",
        feelsLike:'Ressenti', dewPoint:'Point de rosée', today:'aujourd\'hui', per3h:'/ 3 h',
        direction:'Direction', dirs:['N','E','S','O'] },
  es: { navStatus:'Estado', navLog:'Registro', navSettings:'Ajustes', navHelp:'Ayuda / Documentación',
        heading:'Estación meteorológica', history:'Historial de mediciones', buffered:'de historial almacenado', loading:'Cargando...',
        noHistory:'Aún no hay datos — vuelve después del próximo ciclo de medición.',
        toggleTheme:'Cambiar tema', range24h:'24h', range7d:'7 días',
        timeAbsolute:'Hora: Absoluta', timeRelative:'Hora: Relativa', agoTemplate:'hace {n}',
        refreshOff:'Desactivado',
        unitsMetric:'Unidades: Métrico', unitsImperial:'Unidades: Imperial',
        temperature:'Temperatura', humidity:'Humedad', pressure:'Presión', pressureQnh:'Presión (QNH)',
        uvVoltage:'Voltaje del sensor UV', uvIndex:'Índice UV',
        wind:'Viento', windChart:'Velocidad del viento / rachas', rainRate:'Intensidad de lluvia', rainAmount:'Cantidad de lluvia', battery:'Batería',
        ds18b20:'Temp. ext.', ds18b20Chart:'Sensores de temperatura externos',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Racha', speed:'Velocidad', min:'Mín', max:'Máx', records:'Récords (mín · media · máx)',
        noData:'Aún no hay datos', noDataShort:'sin datos', from:'desde',
        failedLoad:'No se pudo cargar el historial: ',
        feelsLike:'Sensación', dewPoint:'Punto de rocío', today:'hoy', per3h:'/ 3 h',
        direction:'Dirección', dirs:['N','E','S','O'] },
  pt: { navStatus:'Estado', navLog:'Registro', navSettings:'Configurações', navHelp:'Ajuda / Documentação',
        heading:'Estação meteorológica', history:'Histórico de medições', buffered:'de histórico armazenado', loading:'Carregando...',
        noHistory:'Ainda sem dados — volte após o próximo ciclo de medição.',
        toggleTheme:'Alternar tema', range24h:'24h', range7d:'7 dias',
        timeAbsolute:'Hora: Absoluta', timeRelative:'Hora: Relativa', agoTemplate:'há {n}',
        refreshOff:'Desativado',
        unitsMetric:'Unidades: Métrico', unitsImperial:'Unidades: Imperial',
        temperature:'Temperatura', humidity:'Umidade', pressure:'Pressão', pressureQnh:'Pressão (QNH)',
        uvVoltage:'Tensão do sensor UV', uvIndex:'Índice UV',
        wind:'Vento', windChart:'Velocidade do vento / rajadas', rainRate:'Intensidade de chuva', rainAmount:'Quantidade de chuva', battery:'Bateria',
        ds18b20:'Temp. ext.', ds18b20Chart:'Sensores de temperatura externos',
        probe0:'Sensor 1', probe1:'Sensor 2', probe2:'Sensor 3',
        gust:'Rajada', speed:'Velocidade', min:'Mín', max:'Máx', records:'Registros (mín · média · máx)',
        noData:'Ainda sem dados', noDataShort:'sem dados', from:'de',
        failedLoad:'Falha ao carregar histórico: ',
        feelsLike:'Sensação', dewPoint:'Ponto de orvalho', today:'hoje', per3h:'/ 3 h',
        direction:'Direção', dirs:['N','E','S','O'] }
};
// Dashboard strings; ptd/wt follow the order of PT_KEYS/WT_KEYS (device texts) below
Object.assign(I18N.en, { updated:'Updated', rain:'Rain', last1h:'Last hour', last24h:'Last 24 h', todayCap:'Today',
  yesterday:'Yesterday', week:'This week', month:'This month', year:'This year', sinceStart:'Since start', lastRain:'Last rain',
  more:'Derived values', cloudBase:'Cloud base', vpd:'Vapour pressure deficit', tempTrend:'Temperature trend',
  pressTrend:'Pressure change', pressureAbs:'Station pressure', light:'Illuminance', sunToday:'Sunshine today',
  sunYesterday:'Sunshine yesterday', uvLv:['low','moderate','high','very high','extreme'],
  frost:'Frost risk', fog:'Fog risk', sultry:'Sultry', fwUpdate:'Firmware {v} available', rfLow:'Wireless sensor battery low',
  rfSensors:'Wireless sensors', batOk:'Battery OK', batLow:'Battery low', supercap:'Supercap', stale:'no update for > 5 min',
  supplies:'Provides', onBoard:'On-board sensors', channel:'Ch',
  stKinds:['Weather station','Thermo-hygro sensor','Pool thermometer','Soil sensor','Leak sensor','Lightning sensor','Rain gauge','Air quality sensor','Sensor'],
  soil:'Soil moisture', lightning:'Lightning', rainHour:'Rain per hour', rainDay:'Rain per day',
  srcNames:{ tempHum:'Temp/Hum', wind:'Wind', rain:'Rain', uv:'UV', light:'Light' },
  system:'System', cpuTemp:'CPU temperature', solar:'Solar', victron:'Victron battery', yieldToday:'Yield today',
  heater:'Heater', fan:'Fan', on:'on', off:'off', stalled:'stalled', auxIn:'AUX input', auxPulses:'AUX pulses', wind2:'Wind 2',
  ptd:['falling very rapidly','falling quickly','falling','falling slowly','steady','rising slowly','rising','rising quickly','rising very rapidly'],
  wt:['improving','worsening','storm warning','settled','unsettled','no change'] });
Object.assign(I18N.de, { updated:'Aktualisiert', rain:'Regen', last1h:'Letzte Stunde', last24h:'Letzte 24 h', todayCap:'Heute',
  yesterday:'Gestern', week:'Diese Woche', month:'Dieser Monat', year:'Dieses Jahr', sinceStart:'Seit Start', lastRain:'Letzter Regen',
  more:'Abgeleitete Werte', cloudBase:'Wolkenuntergrenze', vpd:'Dampfdruckdefizit', tempTrend:'Temperaturtrend',
  pressTrend:'Druckänderung', pressureAbs:'Stationsdruck', light:'Beleuchtungsstärke', sunToday:'Sonnenschein heute',
  sunYesterday:'Sonnenschein gestern', uvLv:['niedrig','mäßig','hoch','sehr hoch','extrem'],
  frost:'Frostgefahr', fog:'Nebelgefahr', sultry:'Schwül', fwUpdate:'Firmware {v} verfügbar', rfLow:'Funksensor-Batterie schwach',
  rfSensors:'Funksensoren', batOk:'Batterie OK', batLow:'Batterie schwach', supercap:'Superkondensator', stale:'seit > 5 min kein Update',
  supplies:'Liefert', onBoard:'Sensoren an der Platine', channel:'Kanal',
  stKinds:['Wetterstation','Thermo-Hygro-Sensor','Poolthermometer','Bodensensor','Leckagesensor','Blitzsensor','Regenmesser','Luftqualitätssensor','Sensor'],
  soil:'Bodenfeuchte', lightning:'Blitze', rainHour:'Niederschlag pro Stunde', rainDay:'Niederschlag pro Tag',
  srcNames:{ tempHum:'Temp./Feuchte', wind:'Wind', rain:'Regen', uv:'UV', light:'Licht' },
  system:'System', cpuTemp:'CPU-Temperatur', solar:'Solar', victron:'Victron-Batterie', yieldToday:'Ertrag heute',
  heater:'Heizung', fan:'Lüfter', on:'an', off:'aus', stalled:'blockiert', auxIn:'AUX-Eingang', auxPulses:'AUX-Impulse', wind2:'Wind 2',
  ptd:['fällt sehr stark','fällt stark','fällt','fällt langsam','gleichbleibend','steigt langsam','steigt','steigt stark','steigt sehr stark'],
  wt:['Besserung','Verschlechterung','Sturmwarnung','beständig','unbeständig','keine Änderung'] });
Object.assign(I18N.fr, { updated:'Mis à jour', rain:'Pluie', last1h:'Dernière heure', last24h:'Dernières 24 h', todayCap:"Aujourd'hui",
  yesterday:'Hier', week:'Cette semaine', month:'Ce mois', year:'Cette année', sinceStart:'Depuis le démarrage', lastRain:'Dernière pluie',
  more:'Valeurs dérivées', cloudBase:'Base des nuages', vpd:'Déficit de pression de vapeur', tempTrend:'Tendance température',
  pressTrend:'Variation de pression', pressureAbs:'Pression station', light:'Éclairement', sunToday:"Ensoleillement aujourd'hui",
  sunYesterday:'Ensoleillement hier', uvLv:['faible','modéré','élevé','très élevé','extrême'],
  frost:'Risque de gel', fog:'Risque de brouillard', sultry:'Temps lourd', fwUpdate:'Firmware {v} disponible', rfLow:'Pile du capteur radio faible',
  rfSensors:'Capteurs sans fil', batOk:'Pile OK', batLow:'Pile faible', supercap:'Supercondensateur', stale:'pas de mise à jour depuis > 5 min',
  supplies:'Fournit', onBoard:'Capteurs de la carte', channel:'Canal',
  stKinds:['Station météo','Thermo-hygromètre','Thermomètre de piscine','Capteur de sol','Détecteur de fuite','Détecteur de foudre','Pluviomètre',"Capteur de qualité de l'air",'Capteur'],
  soil:'Humidité du sol', lightning:'Foudre', rainHour:'Pluie par heure', rainDay:'Pluie par jour',
  srcNames:{ tempHum:'Temp./Hum.', wind:'Vent', rain:'Pluie', uv:'UV', light:'Lumière' },
  system:'Système', cpuTemp:'Température CPU', solar:'Solaire', victron:'Batterie Victron', yieldToday:"Production aujourd'hui",
  heater:'Chauffage', fan:'Ventilateur', on:'marche', off:'arrêt', stalled:'bloqué', auxIn:'Entrée AUX', auxPulses:'Impulsions AUX', wind2:'Vent 2',
  ptd:['baisse très rapide','baisse rapide','en baisse','baisse lente','stable','hausse lente','en hausse','hausse rapide','hausse très rapide'],
  wt:['amélioration','dégradation','alerte tempête','stable','instable','pas de changement'] });
Object.assign(I18N.es, { updated:'Actualizado', rain:'Lluvia', last1h:'Última hora', last24h:'Últimas 24 h', todayCap:'Hoy',
  yesterday:'Ayer', week:'Esta semana', month:'Este mes', year:'Este año', sinceStart:'Desde el inicio', lastRain:'Última lluvia',
  more:'Valores derivados', cloudBase:'Base de nubes', vpd:'Déficit de presión de vapor', tempTrend:'Tendencia de temperatura',
  pressTrend:'Cambio de presión', pressureAbs:'Presión de estación', light:'Iluminancia', sunToday:'Sol hoy',
  sunYesterday:'Sol ayer', uvLv:['bajo','moderado','alto','muy alto','extremo'],
  frost:'Riesgo de helada', fog:'Riesgo de niebla', sultry:'Bochorno', fwUpdate:'Firmware {v} disponible', rfLow:'Batería del sensor inalámbrico baja',
  rfSensors:'Sensores inalámbricos', batOk:'Batería OK', batLow:'Batería baja', supercap:'Supercondensador', stale:'sin actualizar > 5 min',
  supplies:'Aporta', onBoard:'Sensores de la placa', channel:'Canal',
  stKinds:['Estación meteorológica','Termohigrómetro','Termómetro de piscina','Sensor de suelo','Sensor de fugas','Sensor de rayos','Pluviómetro','Sensor de calidad del aire','Sensor'],
  soil:'Humedad del suelo', lightning:'Rayos', rainHour:'Lluvia por hora', rainDay:'Lluvia por día',
  srcNames:{ tempHum:'Temp./Hum.', wind:'Viento', rain:'Lluvia', uv:'UV', light:'Luz' },
  system:'Sistema', cpuTemp:'Temperatura CPU', solar:'Solar', victron:'Batería Victron', yieldToday:'Producción hoy',
  heater:'Calefacción', fan:'Ventilador', on:'encendido', off:'apagado', stalled:'bloqueado', auxIn:'Entrada AUX', auxPulses:'Pulsos AUX', wind2:'Viento 2',
  ptd:['bajada muy rápida','bajada rápida','bajando','bajada lenta','estable','subida lenta','subiendo','subida rápida','subida muy rápida'],
  wt:['mejorando','empeorando','aviso de tormenta','estable','inestable','sin cambios'] });
Object.assign(I18N.pt, { updated:'Atualizado', rain:'Chuva', last1h:'Última hora', last24h:'Últimas 24 h', todayCap:'Hoje',
  yesterday:'Ontem', week:'Esta semana', month:'Este mês', year:'Este ano', sinceStart:'Desde o início', lastRain:'Última chuva',
  more:'Valores derivados', cloudBase:'Base das nuvens', vpd:'Déficit de pressão de vapor', tempTrend:'Tendência de temperatura',
  pressTrend:'Variação de pressão', pressureAbs:'Pressão da estação', light:'Iluminância', sunToday:'Sol hoje',
  sunYesterday:'Sol ontem', uvLv:['baixo','moderado','alto','muito alto','extremo'],
  frost:'Risco de geada', fog:'Risco de nevoeiro', sultry:'Abafado', fwUpdate:'Firmware {v} disponível', rfLow:'Bateria do sensor sem fio fraca',
  rfSensors:'Sensores sem fio', batOk:'Bateria OK', batLow:'Bateria fraca', supercap:'Supercapacitor', stale:'sem atualização há > 5 min',
  supplies:'Fornece', onBoard:'Sensores da placa', channel:'Canal',
  stKinds:['Estação meteorológica','Termo-higrômetro','Termômetro de piscina','Sensor de solo','Sensor de vazamento','Sensor de raios','Pluviômetro','Sensor de qualidade do ar','Sensor'],
  soil:'Umidade do solo', lightning:'Raios', rainHour:'Chuva por hora', rainDay:'Chuva por dia',
  srcNames:{ tempHum:'Temp./Umid.', wind:'Vento', rain:'Chuva', uv:'UV', light:'Luz' },
  system:'Sistema', cpuTemp:'Temperatura da CPU', solar:'Solar', victron:'Bateria Victron', yieldToday:'Produção hoje',
  heater:'Aquecedor', fan:'Ventilador', on:'ligado', off:'desligado', stalled:'travado', auxIn:'Entrada AUX', auxPulses:'Pulsos AUX', wind2:'Vento 2',
  ptd:['queda muito rápida','queda rápida','caindo','queda lenta','estável','subida lenta','subindo','subida rápida','subida muito rápida'],
  wt:['melhorando','piorando','alerta de tempestade','estável','instável','sem mudança'] });
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
  document.documentElement.lang = lang;
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
    updateUpdatedLabel();
    if (lastHist) render(lastHist, lastCurrent);
  });
})();

// --- Units (Metric/Imperial): client-side conversion, same formulas as units.cpp (see header note) ---
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
// mm<->in for both rain amounts (mm/in) and rates (mm/h, in/h): checked by prefix
function convMm(v, from, to) {
  if (v === null || v === undefined || from === to) return v;
  var isImpFrom = (from || '').indexOf('in') === 0, isImpTo = (to || '').indexOf('in') === 0;
  var mm = isImpFrom ? v * 25.4 : v;
  return isImpTo ? mm / 25.4 : mm;
}
function convPressure(v, from, to) {
  if (v === null || v === undefined || from === to) return v;
  var hpa = from === 'inHg' ? v * 33.8639 : v;
  return to === 'inHg' ? hpa / 33.8639 : hpa;
}
var UNIT_TARGETS = {
  metric:   { tempUnit: 'C', windUnit: 'm/s', rainRateUnit: 'mm/h', rainUnit: 'mm', pressureUnit: 'hPa', cloudBaseUnit: 'm' },
  imperial: { tempUnit: 'F', windUnit: 'mph',  rainRateUnit: 'in/h', rainUnit: 'in', pressureUnit: 'inHg', cloudBaseUnit: 'ft' }
};
// Array (history) or single number (/api/data)
function convField(v, from, to, fn) {
  if (Array.isArray(v)) return v.map(function (x) { return fn(x, from, to); });
  return fn(v, from, to);
}
function convKeys(out, raw, keys, from, to, fn) {
  keys.forEach(function (k) { if (raw[k] !== undefined) out[k] = convField(raw[k], from, to, fn); });
}
// dev: /api/history (its *Unit fields always exist) — units of the rfSensors values
function applyUnitPref(raw, dev) {
  var target = UNIT_TARGETS[unitPref];
  var out = Object.assign({}, raw);
  dev = dev || raw;
  if (raw.tempUnit && raw.tempUnit !== target.tempUnit) {
    // ds18b20_*: history arrays share tempUnit; dewPoint: computed client-side for the history
    convKeys(out, raw, ['temp', 'ds18b20_0', 'ds18b20_1', 'ds18b20_2', 'dewPoint', 'feelsLike'], raw.tempUnit, target.tempUnit, convTemp);
    if (has(raw.tempTrend1h)) out.tempTrend1h = target.tempUnit === 'F' ? raw.tempTrend1h * 1.8 : raw.tempTrend1h / 1.8;   // a difference
    out.tempUnit = target.tempUnit;
  }
  // /api/data's ds18b20 array has its own ds18b20Unit (tempUnit only exists with a temperature source)
  if (raw.ds18b20Unit && raw.ds18b20Unit !== target.tempUnit) {
    out.ds18b20 = convField(raw.ds18b20, raw.ds18b20Unit, target.tempUnit, convTemp);
    out.ds18b20Unit = target.tempUnit;
  }
  if (raw.cpuTempUnit && raw.cpuTempUnit !== target.tempUnit) {
    out.cpuTemp = convTemp(raw.cpuTemp, raw.cpuTempUnit, target.tempUnit);
    out.cpuTempUnit = target.tempUnit;
  }
  if (raw.windUnit && raw.windUnit !== target.windUnit) {
    convKeys(out, raw, ['windSpeed', 'windGust'], raw.windUnit, target.windUnit, convWind);
    out.windUnit = target.windUnit;
  }
  if (raw.rainRateUnit && raw.rainRateUnit !== target.rainRateUnit) {
    out.rainRate = convField(raw.rainRate, raw.rainRateUnit, target.rainRateUnit, convMm);
    out.rainRateUnit = target.rainRateUnit;
  }
  if (raw.rainUnit && raw.rainUnit !== target.rainUnit) {
    // history buckets + /api/data's amounts, all in rainUnit
    convKeys(out, raw, ['rainMm', 'rain', 'rain1h', 'rain24h', 'rainToday', 'rainYesterday', 'rainWeek', 'rainMonth',
                        'rainYear', 'rainSinceStart'], raw.rainUnit, target.rainUnit, convMm);
    out.rainUnit = target.rainUnit;
  }
  if (raw.pressureUnit && raw.pressureUnit !== target.pressureUnit) {
    convKeys(out, raw, ['pressureQnh', 'pressureAbs', 'pressureTrend3h'], raw.pressureUnit, target.pressureUnit, convPressure);
    out.pressureUnit = target.pressureUnit;
  }
  if (raw.cloudBaseUnit && raw.cloudBaseUnit !== target.cloudBaseUnit) {
    out.cloudBase = raw.cloudBaseUnit === 'm' ? raw.cloudBase * 3.28084 : raw.cloudBase / 3.28084;
    out.cloudBaseUnit = target.cloudBaseUnit;
  }
  if (Array.isArray(raw.rfSensors)) {
    out.rfSensors = raw.rfSensors.map(function (s) {
      var o = Object.assign({}, s);
      o.temp = convTemp(s.temp, dev.tempUnit, target.tempUnit);
      o.windSpeed = convWind(s.windSpeed, dev.windUnit, target.windUnit);
      o.windGust = convWind(s.windGust, dev.windUnit, target.windUnit);
      o.rainTotal = convMm(s.rainTotal, dev.rainUnit, target.rainUnit);
      return o;
    });
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

// --- Time range (24h fine tier / 7d coarse tier): separate ring buffers, so switching re-fetches ---
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

// --- Chart x-axis: absolute (browser clock minus ageSec — the device has no NTP) or relative "ago" ---
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

// --- Auto-refresh interval: fixed steps, "Off" (0) pauses ---
var REFRESH_STEPS_SEC = [0, 1, 5, 10, 30, 60];
var autoRefreshSec = parseInt(localStorage.getItem('wnAutoRefreshSec'), 10);
if (isNaN(autoRefreshSec) || REFRESH_STEPS_SEC.indexOf(autoRefreshSec) === -1) autoRefreshSec = 10;
var refreshTimer = null;
// durationLabel() is a hoisted function declaration further below
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
  if (sec < 60) return Math.round(sec) + 's';
  if (sec < 3600) return Math.round(sec / 60) + 'm';
  if (sec < 172800) return (sec / 3600).toFixed(1) + 'h';
  return (sec / 86400).toFixed(1) + 'd';
}
// "14m" wrapped in the language's ago-template
function agoLabel(sec) {
  return t('agoTemplate').replace('{n}', durationLabel(sec));
}
// anchorMs: Date.now() captured once per render(); beyond 24h a weekday is added
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

// --- Small formatting helpers for the "now" block and the panels ---
function has(v) { return v !== undefined && v !== null; }
function isIn(u) { return (u || '').indexOf('in') === 0; }
function sgn(v, d) { return (v > 0 ? '+' : v < 0 ? '−' : '±') + Math.abs(v).toFixed(d); }
function esc(s) { return String(s).replace(/[&<>"']/g, function (c) { return '&#' + c.charCodeAt(0) + ';'; }); }
var PT_KEYS = ['falling very rapidly', 'falling quickly', 'falling', 'falling slowly', 'steady',
               'rising slowly', 'rising', 'rising quickly', 'rising very rapidly'];
var PT_ARROWS = '↓↓↘↘→↗↗↑↑';
var WT_KEYS = ['improving', 'worsening', 'storm warning', 'settled', 'unsettled', 'no change'];
function ptText(s) { var i = PT_KEYS.indexOf(s); return i < 0 ? esc(s) : t('ptd')[i]; }
function ptArrow(s) { var i = PT_KEYS.indexOf(s); return i < 0 ? '' : PT_ARROWS[i]; }
function wtText(s) { var i = WT_KEYS.indexOf(s); return i < 0 ? esc(s) : t('wt')[i]; }
var DIRS16 = ['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];
// Compass point in the current language (device windDirText or from the bearing)
function dirText(deg, txt) {
  var s = txt || DIRS16[Math.round(deg / 22.5) % 16], d = t('dirs');
  return s.replace(/[NESW]/g, function (c) { return d['NESW'.indexOf(c)]; });
}
// WHO UV index scale: low / moderate / high / very high / extreme.
var UV_COLORS = ['#3ea72d', '#fff300', '#f18b00', '#e53210', '#b567a4'];
function uvChip(u) {
  var l = u < 3 ? 0 : u < 6 ? 1 : u < 8 ? 2 : u < 11 ? 3 : 4;
  return '<span class="chip" style="background:' + UV_COLORS[l] + ';border-color:' + UV_COLORS[l] +
         ';color:' + (l < 3 ? '#111' : '#fff') + '">' + t('uvLv')[l] + '</span>';
}
function luxText(v) { return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 0 : 1) + ' klx' : Math.round(v) + ' lx'; }
function minText(m) { return m < 60 ? m + ' min' : Math.floor(m / 60) + ' h ' + ('0' + (m % 60)).slice(-2) + ' min'; }
function trendArrow(v, thr) { return v >= thr ? '↗' : v <= -thr ? '↘' : '→'; }
function windArrow(deg) {   // points where the wind blows to
  return '<span class="arr" aria-hidden="true" style="transform:rotate(' + Math.round(deg) + 'deg)">↓</span>';
}
function fillKv(listId, panelId, rows) {
  var html = '';
  rows.forEach(function (r) { if (r) html += '<div class="kv"><span>' + r[0] + '</span><b>' + r[1] + '</b></div>'; });
  document.getElementById(listId).innerHTML = html;
  document.getElementById(panelId).style.display = html ? '' : 'none';
  return html !== '';
}

// --- Metric cards (big number + sparkline); temperature is the large hero card ---
var CARDS = [
  { key: 'temp',        titleKey: 'temperature', unitKey: 'tempUnit',     color: '#e2694f', isTemp: true, hero: true },
  { key: 'humidityPct',  titleKey: 'humidity',    unit: '%',               color: '#4aa8e0', dec: 0 },
  { key: 'pressureQnh', titleKey: 'pressure',    unitKey: 'pressureUnit', color: '#59b370' },
  { key: 'windSpeed',   titleKey: 'wind',        unitKey: 'windUnit',     color: '#4a90d9', secondaryKey: 'windGust', secondaryLabelKey: 'gust' },
  // rangeSum: without the device's rainToday, the total over the selected range
  { key: 'rainMm', rangeSum: true, titleKey: 'rainAmount', unitKey: 'rainUnit', color: '#2f6690' },
  { key: 'uvIndex',     titleKey: 'uvIndex',     unit: '',                color: '#d9822b' },
  { key: 'batteryVolt', titleKey: 'battery',     unit: 'V',               color: '#e0a63e', dec: 2 },
  // DS18B20 probe 0 only (probes 2/3: chart legend + system panel); current.ds18b20_0 comes from render()
  { key: 'ds18b20_0',   titleKey: 'ds18b20',     unitKey: 'ds18b20Unit',  color: '#c96fa8', isTemp: true }
];
var topEl = document.getElementById('top'), cardsEl = document.getElementById('cards'), cardRefs = {};
CARDS.forEach(function (c) {
  var div = document.createElement('div');
  div.className = 'card' + (c.hero ? ' hero' : '');
  div.style.setProperty('--c', c.color);
  div.innerHTML =
    (c.hero ? '<div class="hero-top"><div class="card-label"></div><span class="chip acc" id="wtrend"></span></div>'
            : '<div class="card-label"></div>') +
    '<div class="card-value"><span class="num">--</span><span class="unit"></span></div>' +
    '<div class="card-sub"></div><div class="card-sub sub2"></div>' +
    (c.hero ? '<div class="hrange" id="hRange"></div>' : '') +
    '<canvas class="spark" aria-hidden="true"></canvas>';
  if (c.hero) topEl.insertBefore(div, cardsEl); else cardsEl.appendChild(div);
  cardRefs[c.key] = {
    container: div,
    label: div.querySelector('.card-label'),
    num: div.querySelector('.num'), unit: div.querySelector('.unit'),
    sub: div.querySelector('.card-sub'), sub2: div.querySelector('.sub2'), spark: div.querySelector('.spark')
  };
});

function drawSpark(canvas, values, color) {
  var w = canvas.clientWidth || 160, h = (canvas.clientHeight || 38) - 4;
  var ctx = setupHiDpi(canvas, w, h);
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

// --- Wind compass + rose: rose from the history's windDirDeg, needle/readout from /api/data ---
function drawCompass(data, current) {
  var canvas = document.getElementById('compass');
  var w = canvas.width, h = canvas.height, cx = w / 2, cy = h / 2, R = Math.min(w, h) / 2 - 22;
  var ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  var dim = cssVar('--dim'), border = cssVar('--border'), accent = cssVar('--accent');

  ctx.strokeStyle = border; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.arc(cx, cy, R, 0, 2 * Math.PI); ctx.stroke();
  for (var k = 0; k < 16; k++) {   // tick marks every 22.5°
    var p0 = polar(cx, cy, R - (k % 4 ? 3 : 6), k * 22.5), p1 = polar(cx, cy, R, k * 22.5);
    ctx.beginPath(); ctx.moveTo(p0.x, p0.y); ctx.lineTo(p1.x, p1.y); ctx.stroke();
  }

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
  var readout = document.getElementById('compassText');
  readout.innerHTML =
    (curSpeed === null ? '<b>--</b>' : '<b>' + curSpeed.toFixed(1) + '</b> ' + unit) +
    '<div class="dim">' + (curDir === null ? t('noDataShort') : t('from') + ' ' + dirText(curDir, current.windDirText) + ' ' + Math.round(curDir) + '&deg;') +
    (curGust === null ? '' : ' &middot; ' + t('gust') + ' ' + curGust.toFixed(1) + ' ' + unit) + '</div>';
  canvas.setAttribute('aria-label', t('wind') + ': ' + readout.textContent);
}

function buildRecords(data) {
  var el = document.getElementById('records');
  el.innerHTML = '';
  CARDS.forEach(function (c) {
    var vals = (data[c.key] || []).filter(function (v) { return v !== null && v !== undefined; });
    if (!vals.length) return;
    var min = Math.min.apply(null, vals), max = Math.max.apply(null, vals);
    var avg = vals.reduce(function (a, b) { return a + b; }, 0) / vals.length;
    var unit = (c.isTemp ? '°' : '') + (c.unit || data[c.unitKey] || (c.isTemp ? data.tempUnit : '') || '');
    var pct = max > min ? ((avg - min) / (max - min)) * 100 : 50;
    var row = document.createElement('div');
    row.className = 'record-row';
    row.innerHTML =
      '<div class="record-head"><span>' + t(c.titleKey) + '</span><b>' + avg.toFixed(1) + ' ' + unit + '</b></div>' +
      '<div class="record-track"><div class="record-fill" style="left:' + pct + '%;background:' + c.color + '"></div></div>' +
      '<div class="record-minmax"><span>' + t('min') + ' ' + min.toFixed(1) + '</span><span>' + t('max') + ' ' + max.toFixed(1) + '</span></div>';
    el.appendChild(row);
  });
  document.getElementById('recPanel').style.display = el.firstChild ? '' : 'none';
}

// --- Trend charts; `bars`: one bar per clock hour (24h) / day (7 days) ---
var CHARTS = [
  { id: 'temp',     titleKey: 'temperature', series: [{ key: 'temp', color: '#e2694f', labelKey: 'temperature' }, { key: 'dewPoint', color: '#4f86c6', labelKey: 'dewPoint' }], unitKey: 'tempUnit', isTemp: true },
  { id: 'humidity', titleKey: 'humidity',    series: [{ key: 'humidityPct', color: '#4aa8e0' }], unit: '%', decimals: 0 },
  { id: 'pressure', titleKey: 'pressureQnh', series: [{ key: 'pressureQnh', color: '#59b370' }], unitKey: 'pressureUnit' },
  { id: 'wind',     titleKey: 'windChart',   series: [{ key: 'windSpeed', color: '#4a90d9', labelKey: 'speed' }, { key: 'windGust', color: '#a468c9', labelKey: 'gust' }], unitKey: 'windUnit', minZero: true },
  { id: 'rain',     titleKey: 'rainRate',    series: [{ key: 'rainRate', color: '#3f7fc1' }], unitKey: 'rainRateUnit', minZero: true },
  { id: 'rainMm',   titleKey: 'rainAmount',  series: [{ key: 'rainMm', color: '#2f6690' }], unitKey: 'rainUnit', minZero: true, rangeSum: true, bars: true },
  { id: 'battery',  titleKey: 'battery',     series: [{ key: 'batteryVolt', color: '#e0a63e' }], unit: 'V', decimals: 2 },
  { id: 'uvVoltage', titleKey: 'uvVoltage', series: [{ key: 'uvSensorVolts', color: '#7b61a8' }], unit: 'V', decimals: 3, minZero: true },
  { id: 'uvIndex', titleKey: 'uvIndex', series: [{ key: 'uvIndex', color: '#d9822b' }], unit: 'UV', decimals: 1, minZero: true },
  { id: 'ds18b20',  titleKey: 'ds18b20Chart',
    series: [{ key: 'ds18b20_0', color: '#c96fa8', labelKey: 'probe0' },
             { key: 'ds18b20_1', color: '#7a5fc4', labelKey: 'probe1' },
             { key: 'ds18b20_2', color: '#3f9b8c', labelKey: 'probe2' }],
    unitKey: 'tempUnit', isTemp: true }
];
var chartsEl = document.getElementById('charts'), canvases = {}, chartTitleEls = {}, chartContainerEls = {}, tipEls = {};
CHARTS.forEach(function (c) {
  var div = document.createElement('div');
  div.className = 'chart';
  div.innerHTML = '<h4></h4><canvas class="trend"></canvas><div class="tip"></div>';
  chartsEl.appendChild(div);
  var canvas = div.querySelector('canvas');
  canvases[c.id] = canvas;
  chartTitleEls[c.id] = div.querySelector('h4');
  chartContainerEls[c.id] = div;
  tipEls[c.id] = div.querySelector('.tip');
  canvas.addEventListener('pointermove', function (e) { setHover(chartIndexAt(canvas, e.clientX), c.id); });
  canvas.addEventListener('pointerdown', function (e) { setHover(chartIndexAt(canvas, e.clientX), c.id); });
  // touch: tooltip stays until the next tap outside a chart
  canvas.addEventListener('pointerleave', function (e) { if (e.pointerType !== 'touch') setHover(null, null); });
});
document.addEventListener('pointerdown', function (e) {
  if (!(e.target.classList && e.target.classList.contains('trend'))) setHover(null, null);
});

// Card/chart titles depend on the language: set here and on every change
function translateDynamicTitles() {
  CARDS.forEach(function (c) { cardRefs[c.key].label.textContent = t(c.titleKey); });
  CHARTS.forEach(function (c) { chartTitleEls[c.id].textContent = t(c.titleKey); });
}
translateDynamicTitles();

// Canvas backing store at devicePixelRatio, drawing stays in CSS pixels
function setupHiDpi(canvas, w, h) {
  var dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  var ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return ctx;
}
function withAlpha(hex, a) {
  var v = parseInt(hex.slice(1), 16);
  return 'rgba(' + (v >> 16) + ',' + ((v >> 8) & 255) + ',' + (v & 255) + ',' + a + ')';
}

function drawChart(canvas, st) {
  var w = canvas.clientWidth || 900, h = 170;
  var ctx = setupHiDpi(canvas, w, h);
  canvas._geo = null;
  var pad = { l: 44, r: 10, t: 8, b: 20 };
  var plotW = w - pad.l - pad.r, plotH = h - pad.t - pad.b;
  var grid = cssVar('--border'), dim = cssVar('--dim'), series = st.series, bars = st.bars;

  var allVals = [];
  if (bars) Object.keys(bars.sum).forEach(function (k) { allVals.push(bars.sum[k]); });
  else series.forEach(function (s) { s.data.forEach(function (v) { if (v !== null && v !== undefined) allVals.push(v); }); });
  if (!allVals.length) {
    ctx.fillStyle = dim; ctx.font = '12px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText(t('noData'), w / 2, h / 2);
    return;
  }
  var min = Math.min.apply(null, allVals), max = Math.max.apply(null, allVals);
  // minZero: rain/wind can't go negative — no "-1 to 1" axis for all-zero data
  if (st.minZero) { min = 0; if (max <= 0) max = 1; }
  else if (min === max) { min -= 1; max += 1; }
  var n = st.ageSec.length;
  function xAt(i) { return pad.l + (n <= 1 ? 0 : (plotW * i) / (n - 1)); }
  function yAt(v) { return pad.t + plotH - ((v - min) / (max - min)) * plotH; }

  ctx.strokeStyle = grid; ctx.fillStyle = dim; ctx.font = '10px sans-serif'; ctx.textAlign = 'left'; ctx.lineWidth = 1;
  var rows = 4, span = max - min, yDec = span < 2 ? 2 : span < 20 ? 1 : 0;
  for (var r = 0; r <= rows; r++) {
    var yy = pad.t + (plotH * r) / rows;
    ctx.beginPath(); ctx.moveTo(pad.l, yy); ctx.lineTo(pad.l + plotW, yy); ctx.stroke();
    ctx.fillText((max - (span * r) / rows).toFixed(yDec), 2, yy + 3);
  }
  // never `var t` here: it would shadow the i18n t() (hoisting). Edge ticks aligned inwards.
  var tickCount = w < 560 ? 2 : 4;
  for (var ti = 0; ti <= tickCount; ti++) {
    var idx = Math.round((n - 1) * ti / tickCount);
    ctx.textAlign = ti === 0 ? 'left' : ti === tickCount ? 'right' : 'center';
    ctx.fillText(xAxisLabel(st.ageSec[idx], st.anchorMs), ti === tickCount ? w - 2 : xAt(idx), h - 5);
  }

  if (bars) {
    var step = n > 1 ? plotW / (n - 1) : plotW, i0 = 0;
    ctx.fillStyle = withAlpha(series[0].color, 0.8);
    for (var i = 1; i <= n; i++) {
      if (i < n && bars.key[i] === bars.key[i0]) continue;
      var bv = bars.sum[bars.key[i0]];
      if (bv) {
        var x0 = Math.max(pad.l, xAt(i0) - step / 2) + 1, x1 = Math.min(pad.l + plotW, xAt(i - 1) + step / 2) - 1, y0 = yAt(bv);
        ctx.fillRect(x0, y0, Math.max(1, x1 - x0), pad.t + plotH - y0);
      }
      i0 = i;
    }
  } else series.forEach(function (s, si) {
    ctx.beginPath();
    var started = false, firstX = 0, lastX = 0;
    for (var i = 0; i < n; i++) {
      var v = s.data[i];
      if (v === null || v === undefined) { started = false; continue; }
      var x = xAt(i), y = yAt(v);
      if (!started) { ctx.moveTo(x, y); firstX = x; started = true; } else ctx.lineTo(x, y);
      lastX = x;
    }
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.75; ctx.lineJoin = 'round'; ctx.stroke();
    if (si === 0) {
      ctx.lineTo(lastX, pad.t + plotH); ctx.lineTo(firstX, pad.t + plotH); ctx.closePath();
      var grad = ctx.createLinearGradient(0, pad.t, 0, pad.t + plotH);
      grad.addColorStop(0, withAlpha(s.color, series.length > 1 ? 0.18 : 0.28)); grad.addColorStop(1, withAlpha(s.color, 0.02));
      ctx.fillStyle = grad; ctx.fill();
    }
  });

  if (series.length > 1) {
    ctx.textAlign = 'left'; ctx.font = '10px sans-serif';
    var lw = 0;
    series.forEach(function (s) { lw = Math.max(lw, ctx.measureText(t(s.labelKey)).width); });
    var lx = pad.l + plotW - lw - 12, ly = pad.t + 2;
    ctx.fillStyle = withAlpha(cssVar('--panel'), 0.85);
    ctx.fillRect(lx - 7, ly - 2, lw + 17, 12 * series.length + 4);
    series.forEach(function (s, i) {
      ctx.fillStyle = s.color;
      ctx.beginPath(); ctx.arc(lx, ly + 12 * i + 4, 3, 0, 2 * Math.PI); ctx.fill();
      ctx.fillStyle = dim; ctx.fillText(t(s.labelKey) || s.key, lx + 8, ly + 12 * i + 7);
    });
  }

  canvas._geo = { n: n, xAt: xAt, padL: pad.l, plotW: plotW, top: pad.t };
  if (hoverIdx !== null && hoverIdx < n) {
    var hx = xAt(hoverIdx), panel = cssVar('--panel');
    ctx.strokeStyle = dim; ctx.lineWidth = 1; ctx.setLineDash([3, 3]);
    ctx.beginPath(); ctx.moveTo(hx, pad.t); ctx.lineTo(hx, pad.t + plotH); ctx.stroke();
    ctx.setLineDash([]);
    series.forEach(function (s) {
      var v = bars ? bars.sum[bars.key[hoverIdx]] || 0 : s.data[hoverIdx];
      if (v === null || v === undefined) return;
      ctx.beginPath(); ctx.arc(hx, yAt(v), 4, 0, 2 * Math.PI);
      ctx.fillStyle = s.color; ctx.fill();
      ctx.lineWidth = 2; ctx.strokeStyle = panel; ctx.stroke();
    });
  }
}

// --- Hover crosshair (all charts share one time axis) + tooltip in the hovered chart; mouse and touch ---
var hoverIdx = null, hoverChartId = null, hoverFrame = 0, chartState = {};
function chartIndexAt(canvas, clientX) {
  var g = canvas._geo;
  if (!g || g.n < 1) return null;
  var x = clientX - canvas.getBoundingClientRect().left;
  var i = g.n <= 1 ? 0 : Math.round((x - g.padL) / g.plotW * (g.n - 1));
  return Math.max(0, Math.min(g.n - 1, i));
}
function setHover(idx, chartId) {
  if (idx === hoverIdx && chartId === hoverChartId) return;
  hoverIdx = idx; hoverChartId = chartId;
  if (hoverFrame) return;
  hoverFrame = requestAnimationFrame(function () { hoverFrame = 0; redrawCharts(); });
}
function drawChartFor(id) {
  var st = chartState[id];
  if (st) drawChart(canvases[id], st);
}
function redrawCharts() {
  CHARTS.forEach(function (c) { drawChartFor(c.id); });
  updateTooltip();
}
function tooltipTime(sec, anchorMs) {
  var d = new Date(anchorMs - sec * 1000), locale = LOCALES[lang] || 'en-US';
  return d.toLocaleDateString(locale, { weekday: 'short', day: 'numeric', month: 'short' }) + ' ' +
         d.toLocaleTimeString(locale, { hour: '2-digit', minute: '2-digit' }) + ' &middot; ' + agoLabel(sec);
}
function tipRow(color, label, value) {
  return '<div class="row"><span class="sw" style="background:' + color + '"></span>' + label + '<b>' + value + '</b></div>';
}
function updateTooltip() {
  CHARTS.forEach(function (c) {
    var tip = tipEls[c.id], st = chartState[c.id], canvas = canvases[c.id], g = canvas._geo;
    if (hoverIdx === null || c.id !== hoverChartId || !st || !g || hoverIdx >= g.n) { tip.style.display = 'none'; return; }
    var html = '<div class="when">' + tooltipTime(st.ageSec[hoverIdx], st.anchorMs) + '</div>';
    st.series.forEach(function (s) {
      var v = st.bars ? st.bars.sum[st.bars.key[hoverIdx]] || 0 : s.data[hoverIdx];
      html += tipRow(s.color, st.bars ? st.barLabel : s.labelKey ? t(s.labelKey) : t(c.titleKey),
                     v === null || v === undefined ? '--' : v.toFixed(st.decimals) + ' ' + st.unit);
    });
    var dir = st.dir ? st.dir[hoverIdx] : null;
    if (dir !== null && dir !== undefined) html += tipRow(cssVar('--accent'), t('direction'), dirText(dir) + ' ' + Math.round(dir) + '&deg;');
    tip.innerHTML = html;
    tip.style.display = 'block';
    var x = canvas.offsetLeft + g.xAt(hoverIdx), container = chartContainerEls[c.id];
    var left = x + 14;
    if (left + tip.offsetWidth > container.clientWidth - 4) left = x - 14 - tip.offsetWidth;
    tip.style.left = Math.max(4, left) + 'px';
    tip.style.top = (canvas.offsetTop + g.top + 4) + 'px';
  });
}

var lastHist = null, lastCurrent = null, lastFetchMs = 0;
// Total of the bucketed amounts, null without any valid bucket
function rangeSum(arr) {
  var sum = null;
  (arr || []).forEach(function (v) { if (v !== null && v !== undefined) sum = (sum || 0) + v; });
  return sum;
}
// The range name once the buffer spans it, else the buffered span (e.g. "5.2h")
function rangeSumPeriod(data) {
  var span = (data.ageSec || []).length ? data.ageSec[0] : 0;
  var nominal = timeRange === '7d' ? 7 * 86400 : 86400;
  return span >= 0.95 * nominal ? t(timeRange === '7d' ? 'range7d' : 'range24h') : durationLabel(span);
}
// Rain bars: buckets grouped by local hour/day (ageSec is a bucket's end, hence -1 s)
function groupBuckets(ageSec, vals, nowMs, daily) {
  var key = [], sum = {};
  ageSec.forEach(function (a, i) {
    var d = new Date(nowMs - a * 1000 - 1000), k = d.toDateString() + (daily ? '' : ' ' + d.getHours());
    key.push(k);
    if (has(vals[i])) sum[k] = (sum[k] || 0) + vals[i];
  });
  return { key: key, sum: sum };
}
// Magnus formula as in derived.cpp — dew point history from temp + humidity buckets
function dewPointC(tc, rh) {
  var g = Math.log(rh / 100) + 17.62 * tc / (243.12 + tc);
  return 243.12 * g / (17.62 - g);
}
function updateUpdatedLabel() {
  document.getElementById('updated').textContent = lastFetchMs ?
    t('updated') + ' ' + new Date(lastFetchMs).toLocaleTimeString(LOCALES[lang] || 'en-US') : '';
}

// --- Now block: warnings, hero card details, tile sub-lines ---
function renderAlerts(current) {
  var al = [], w = current.warnings || {};
  if (current.weatherTrend === 'storm warning') al.push(['bad', '⚠', wtText(current.weatherTrend)]);
  [['frost', '❄'], ['fog', '☁'], ['sultry', '♨']].forEach(function (x) {
    if (w[x[0]]) al.push([x[0], x[1], t(x[0])]);
  });
  if (current.radioBatteryLow) al.push(['bad', '⚠', t('rfLow')]);
  if (current.firmwareUpdate) al.push(['info', '⬆', t('fwUpdate').replace('{v}', esc(current.firmwareUpdate))]);
  var el = document.getElementById('alerts');
  el.innerHTML = al.map(function (a) {
    return '<span class="alert al-' + a[0] + '"><span aria-hidden="true">' + a[1] + '︎</span> ' + a[2] + '</span>';
  }).join('');
  el.style.display = al.length ? '' : 'none';
}
function renderNow(data, current, nowMs) {
  var R = cardRefs, pu = current.pressureUnit || '', pDec = isIn(pu) ? 2 : 1;
  // Hero: feels like, 1 h trend, today's min/max (buckets since local midnight), weather trend
  var subs = [];
  if (has(current.feelsLike)) subs.push(t('feelsLike') + ' ' + current.feelsLike.toFixed(1) + '°');
  if (has(current.tempTrend1h)) subs.push(trendArrow(current.tempTrend1h, 0.3) + ' ' + sgn(current.tempTrend1h, 1) + '°/h');
  R.temp.sub.textContent = subs.join(' · ');
  var mid = new Date(nowMs); mid.setHours(0, 0, 0, 0);
  var since = (nowMs - mid.getTime()) / 1000, tv = [], hr = document.getElementById('hRange');
  (data.temp || []).forEach(function (v, i) { if (has(v) && data.ageSec[i] <= since) tv.push(v); });
  if (has(current.temp)) tv.push(current.temp);
  if (tv.length > 1) {
    var mn = Math.min.apply(null, tv), mx = Math.max.apply(null, tv);
    var pos = has(current.temp) && mx > mn ? (current.temp - mn) / (mx - mn) * 100 : 50;
    hr.innerHTML = '<span class="dim">' + t('todayCap') + '</span><span aria-label="' + t('min') + '">↓ ' + mn.toFixed(1) +
      '°</span><div class="record-track" aria-hidden="true"><div class="record-fill" style="left:' + pos +
      '%;background:#e2694f"></div></div><span aria-label="' + t('max') + '">↑ ' + mx.toFixed(1) + '°</span>';
    hr.style.display = '';
  } else hr.style.display = 'none';
  var wt = document.getElementById('wtrend');
  wt.innerHTML = current.weatherTrend ? ptArrow(current.pressureTendency) + ' ' + wtText(current.weatherTrend) : '';
  wt.style.display = current.weatherTrend ? '' : 'none';
  topEl.className = 'top' + (R.temp.container.style.display === 'none' ? ' nohero' : '');
  // Tiles beside the hero: column count for balanced rows, the last tile
  // widened to fill its row.
  var tiles = CARDS.filter(function (c) { return !c.hero && R[c.key].container.style.display !== 'none'; });
  var nT = tiles.length, cols = matchMedia('(max-width:720px)').matches ? 2 : nT <= 4 ? 2 : nT <= 6 ? 3 : 4, rem = nT % cols;
  cardsEl.style.setProperty('--cols', cols);
  tiles.forEach(function (c, i) {
    R[c.key].container.style.gridColumn = i === nT - 1 && rem && !topEl.className.match('nohero') ? 'span ' + (cols - rem + 1) : '';
  });

  if (has(current.dewPoint)) R.humidityPct.sub.textContent = t('dewPoint') + ' ' + current.dewPoint.toFixed(1) + '°';
  if (has(current.vpd)) R.humidityPct.sub2.textContent = 'VPD ' + current.vpd.toFixed(2) + ' ' + (current.vpdUnit || 'kPa');
  if (has(current.pressureTrend3h)) {
    var dp = current.pressureTrend3h;
    R.pressureQnh.sub.textContent = (ptArrow(current.pressureTendency) || trendArrow(dp, pDec > 1 ? 0.003 : 0.1)) + ' ' +
      sgn(dp, pDec) + ' ' + pu + ' ' + t('per3h');
    if (current.pressureTendency) R.pressureQnh.sub2.innerHTML = ptText(current.pressureTendency);
  }
  if (has(current.windDirDeg)) R.windSpeed.sub2.innerHTML = windArrow(current.windDirDeg) + ' ' +
    dirText(current.windDirDeg, current.windDirText) + ' ' + Math.round(current.windDirDeg) + '°';
  var rr = R.rainMm, rDec = isIn(current.rainUnit) ? 2 : 1;
  if (has(current.rainToday) && rr.container.style.display !== 'none') {
    var sum = rangeSum(data.rainMm);
    rr.num.textContent = current.rainToday.toFixed(rDec);
    rr.unit.textContent = ' ' + (current.rainUnit || '');
    rr.sub.textContent = t('todayCap') + (sum === null ? '' : ' · Σ ' + rangeSumPeriod(data) + ' ' + sum.toFixed(rDec));
  }
  if (has(current.rainRate)) rr.sub2.textContent = current.rainRate.toFixed(isIn(current.rainRateUnit) ? 2 : 1) + ' ' + (current.rainRateUnit || '');
  if (has(current.uvIndex)) R.uvIndex.sub.innerHTML = uvChip(current.uvIndex);
  var sun = [];
  if (has(current.lightLux)) sun.push(luxText(current.lightLux));
  if (has(current.sunshineTodayMin)) sun.push('<span aria-hidden="true">\u2600\uFE0E</span> ' + minText(current.sunshineTodayMin));
  R.uvIndex.sub2.innerHTML = sun.join(' \u00b7 ');
}

// --- Panels: rain, derived values, system (PCB extras) ---
function renderPanels(current) {
  var tu = '°' + (current.tempUnit || ''), pu = ' ' + (current.pressureUnit || ''), pDec = isIn(pu.trim()) ? 2 : 1;
  var ru = ' ' + (current.rainUnit || ''), rDec = isIn(current.rainUnit) ? 2 : 1;
  function rain(key, label) { return has(current[key]) ? [t(label), current[key].toFixed(rDec) + ru] : null; }
  var lastRainSec = current.lastRain ? (Date.now() - Date.parse(current.lastRain)) / 1000 : NaN;
  fillKv('rainList', 'rainPanel', [
    has(current.rainRate) ? [t('rainRate'), current.rainRate.toFixed(isIn(current.rainRateUnit) ? 2 : 1) + ' ' + (current.rainRateUnit || '')] : null,
    rain('rain1h', 'last1h'), rain('rain24h', 'last24h'), rain('rainToday', 'todayCap'), rain('rainYesterday', 'yesterday'),
    rain('rainWeek', 'week'), rain('rainMonth', 'month'), rain('rainYear', 'year'), rain('rainSinceStart', 'sinceStart'),
    isNaN(lastRainSec) ? null : [t('lastRain'), agoLabel(Math.max(0, lastRainSec))]
  ]);

  var dp = current.pressureTrend3h;
  fillKv('moreList', 'morePanel', [
    has(current.dewPoint) ? [t('dewPoint'), current.dewPoint.toFixed(1) + ' ' + tu] : null,
    has(current.feelsLike) ? [t('feelsLike'), current.feelsLike.toFixed(1) + ' ' + tu] : null,
    has(current.cloudBase) ? [t('cloudBase'), Math.round(current.cloudBase) + ' ' + (current.cloudBaseUnit || '')] : null,
    has(current.vpd) ? [t('vpd'), current.vpd.toFixed(2) + ' ' + (current.vpdUnit || 'kPa')] : null,
    has(current.tempTrend1h) ? [t('tempTrend'), trendArrow(current.tempTrend1h, 0.3) + ' ' + sgn(current.tempTrend1h, 1) + ' ' + tu + '/h'] : null,
    has(dp) ? [t('pressTrend'), (ptArrow(current.pressureTendency) || trendArrow(dp, 0.1)) + ' ' + sgn(dp, pDec) + pu + ' ' + t('per3h')] : null,
    has(current.pressureAbs) ? [t('pressureAbs'), current.pressureAbs.toFixed(pDec) + pu] : null,
    has(current.uvIndex) ? [t('uvIndex'), current.uvIndex.toFixed(1) + uvChip(current.uvIndex)] : null,
    has(current.lightLux) ? [t('light'), luxText(current.lightLux)] : null,
    has(current.sunshineTodayMin) ? [t('sunToday'), minText(current.sunshineTodayMin)] : null,
    has(current.sunshineYesterdayMin) ? [t('sunYesterday'), minText(current.sunshineYesterdayMin)] : null
  ]);

  // System panel only with any PCB/LiPo/probe extra — CPU temperature alone isn't worth one
  var rows = [], v = current.victron, he = current.heater, fan = current.fan, w2 = current.wind2;
  if (has(current.batteryVolt)) rows.push([t('battery'), current.batteryVolt.toFixed(2) + ' V']);
  (current.ds18b20 || []).forEach(function (x, i) {
    if (i > 0 && has(x)) rows.push([t('ds18b20') + ' ' + (i + 1), x.toFixed(1) + ' °' + (current.ds18b20Unit || '')]);
  });
  if (v) {
    if (!v.valid) rows.push(['Victron', t('noDataShort')]);
    else {
      if (has(v.pvW)) rows.push([t('solar'), v.pvW + ' W' + (has(v.pvV) ? ' · ' + v.pvV.toFixed(1) + ' V' : '')]);
      var vb = [];
      if (has(v.batteryV)) vb.push(v.batteryV.toFixed(2) + ' V');
      if (has(v.batteryA)) vb.push(v.batteryA.toFixed(2) + ' A');
      if (has(v.socPct)) vb.push(v.socPct + ' %');
      if (vb.length) rows.push([t('victron'), vb.join(' · ')]);
      if (has(v.yieldTodayKWh)) rows.push([t('yieldToday'), v.yieldTodayKWh.toFixed(2) + ' kWh']);
    }
  }
  if (he) rows.push([t('heater'), (he.mode === 'auto' ? 'auto · ' : '') + t(he.on ? 'on' : 'off') + (has(he.dutyPct) ? ' · ' + he.dutyPct + ' %' : '')]);
  if (fan) rows.push([t('fan'), (fan.mode === 'auto' ? 'auto · ' : '') + t(fan.on ? 'on' : 'off') +
    (fan.on && has(fan.dutyPct) ? ' · ' + fan.dutyPct + ' %' : '') + (has(fan.rpm) ? ' · ' + fan.rpm + ' rpm' : '') +
    (fan.stalled ? ' · <span class="bad">' + t('stalled') + '</span>' : '')]);
  if (has(current.auxAnalogV)) rows.push([t('auxIn'), current.auxAnalogV.toFixed(3) + ' V']);
  if (has(current.auxPulses)) rows.push([t('auxPulses'), current.auxPulses + (has(current.auxPulsesTotal) ? ' · Σ ' + current.auxPulsesTotal : '')]);
  if (w2 && has(w2.speed)) rows.push([t('wind2'), w2.speed.toFixed(1) + ' / ' + (w2.gust || 0).toFixed(1) + ' ' + (lastHist.windUnit || '') +
    (has(w2.dirDeg) ? ' · ' + dirText(w2.dirDeg) + ' ' + Math.round(w2.dirDeg) + '°' : '')]);
  if (rows.length && has(current.cpuTemp)) rows.push([t('cpuTemp'), current.cpuTemp.toFixed(1) + ' °' + (current.cpuTempUnit || '')]);
  fillKv('sysList', 'sysPanel', rows);
}

// --- Wireless sensors: signal, age, battery, values (stale struck through), `sources` ---
function sigBars(rssi) {
  var n = rssi >= -70 ? 4 : rssi >= -80 ? 3 : rssi >= -90 ? 2 : rssi >= -100 ? 1 : 0, s = '';
  for (var i = 0; i < 4; i++) s += '<rect x="' + i * 4 + '" y="' + (10 - i * 3) + '" width="3" height="' + (3 + i * 3) + '" rx="0.5" fill="currentColor" opacity="' + (i < n ? 1 : 0.25) + '"/>';
  return '<svg class="sig" viewBox="0 0 16 13" role="img" aria-label="' + rssi + ' dBm">' + s + '</svg>';
}
function sensorKind(s) {
  var ty = s.type;
  var i = [0, 1, 12, 13].indexOf(ty) >= 0 ? 0 : ty === 2 ? 1 : ty === 3 ? 2 : ty === 4 ? 3 : ty === 5 ? 4 :
          ty === 9 ? (has(s.lightningCount) ? 5 : 6) : [8, 10, 11].indexOf(ty) >= 0 ? 7 : 8;
  return t('stKinds')[i];
}
function renderSensors(current) {
  var list = current.rfSensors, src = current.sources || {}, U = UNIT_TARGETS[unitPref], names = t('srcNames');
  var panel = document.getElementById('rfPanel');
  if (!Array.isArray(list) || !list.length) { panel.style.display = 'none'; return; }
  panel.style.display = '';
  var html = '';
  list.forEach(function (s) {
    var stale = s.stale || [], vals = [];
    function val(key, text) { vals.push(stale.indexOf(key) >= 0 ? '<span class="st" title="' + t('stale') + '">' + text + '</span>' : '<span>' + text + '</span>'); }
    if (has(s.temp)) val('temp', s.temp.toFixed(1) + ' °' + U.tempUnit);
    if (has(s.humidityPct)) val('humidity', Math.round(s.humidityPct) + ' %');
    if (has(s.windSpeed)) val('wind', windArrow(s.windDirDeg || 0) + ' ' + s.windSpeed.toFixed(1) + ' / ' + (s.windGust || 0).toFixed(1) + ' ' + U.windUnit);
    if (has(s.rainTotal)) val('rain', 'Σ ' + s.rainTotal.toFixed(isIn(U.rainUnit) ? 2 : 1) + ' ' + U.rainUnit);
    if (has(s.uvIndex)) val('uv', 'UV ' + s.uvIndex.toFixed(1));
    if (has(s.lightLux)) val('light', luxText(s.lightLux));
    if (has(s.soilMoisturePct)) val('soil', t('soil') + ' ' + s.soilMoisturePct + ' %');
    if (has(s.lightningCount)) val('lightning', '<span aria-hidden="true">⚡︎</span> ' + t('lightning') + ' ' + s.lightningCount +
      (has(s.lightningDistKm) ? ' · ' + s.lightningDistKm + ' km' : ''));
    var provides = Object.keys(src).filter(function (g) { return src[g] === 'radio:' + s.id; });
    var sub = [s.model, s.protocol].filter(Boolean).map(esc).join(' · ') + ' · ID ' + esc(s.id) +
              (s.channel ? ' · ' + t('channel') + ' ' + s.channel : '');
    var age = s.ageSec;
    html += '<div class="rf"><div class="rf-head"><b>' + sensorKind(s) + '</b><span class="dim">' + sub + '</span></div>' +
      '<div class="rf-meta"><span>' + sigBars(s.rssi) + ' ' + s.rssi + ' dBm</span>' +
      (has(age) ? '<span class="' + (age > 300 ? 'warnc' : 'dim') + '">' + agoLabel(age) + '</span>' : '') +
      (has(s.batteryOk) ? '<span class="' + (s.batteryOk ? 'ok' : 'bad') + '">' + t(s.batteryOk ? 'batOk' : 'batLow') + '</span>' : '') +
      (has(s.supercapV) ? '<span>' + t('supercap') + ' ' + s.supercapV.toFixed(1) + ' V</span>' : '') + '</div>' +
      (vals.length ? '<div class="rf-vals">' + vals.join('') + '</div>' : '') +
      (provides.length ? '<div class="rf-src"><span class="dim">' + t('supplies') + ':</span>' +
        provides.map(function (g) { return '<span class="chip acc">' + (names[g] || g) + '</span>'; }).join('') + '</div>' : '') +
      '</div>';
  });
  document.getElementById('rfList').innerHTML = html;
  var board = Object.keys(src).filter(function (g) { return src[g] === 'board'; });
  document.getElementById('rfBoard').innerHTML = board.length ? '<span class="dim">' + t('onBoard') + ':</span>' +
    board.map(function (g) { return '<span class="chip">' + (names[g] || g) + '</span>'; }).join('') : '';
}

// rawHist: /api/history (trends, records, rose); rawCurrent: /api/data (now values)
function render(rawHist, rawCurrent) {
  lastHist = rawHist; lastCurrent = rawCurrent;
  var nowMs = Date.now();
  // Without a saved preference, follow the device's unit system on every refresh
  if (!unitPrefStored) {
    unitPref = (rawCurrent.tempUnit || rawCurrent.ds18b20Unit || rawHist.tempUnit) === 'F' ? 'imperial' : 'metric';
    updateUnitsButtonLabel();
  }
  // Metric wind unit = the device's (m/s or km/h)
  var devWind = rawCurrent.windUnit || rawHist.windUnit;
  if (devWind === 'm/s' || devWind === 'km/h') UNIT_TARGETS.metric.windUnit = devWind;
  var hist = Object.assign({}, rawHist), hT = rawHist.temp || [], hH = rawHist.humidityPct || [], fahr = rawHist.tempUnit === 'F';
  hist.dewPoint = hT.map(function (v, i) {
    if (!has(v) || !has(hH[i]) || hH[i] <= 0) return null;
    var d = dewPointC(fahr ? (v - 32) / 1.8 : v, hH[i]);
    return fahr ? d * 1.8 + 32 : d;
  });
  var data = applyUnitPref(hist);
  var current = applyUnitPref(rawCurrent, rawHist);
  // /api/data's slot-indexed ds18b20 array -> ds18b20_0/_1/_2 like the history fields
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

  renderAlerts(current);
  CARDS.forEach(function (c) {
    var arr = data[c.key] || [];
    var last = c.rangeSum ? rangeSum(arr) : current[c.currentKey || c.key];
    if (last === undefined) last = null;
    var ref = cardRefs[c.key];
    ref.sub.textContent = ''; ref.sub2.textContent = '';
    // No data now and none in the history (sensor absent): hide the card
    var hasData = last !== null || arr.some(function (v) { return v !== null && v !== undefined; });
    ref.container.style.display = hasData ? '' : 'none';
    if (!hasData) return;
    var unit = (c.isTemp ? '°' : '') + (c.unit || current[c.unitKey] || data[c.unitKey] || '');
    var cardUnit = c.unit || current[c.unitKey] || data[c.unitKey] || '';
    ref.num.textContent = last === null ? '--' : last.toFixed(c.dec !== undefined ? c.dec : cardUnit.indexOf('in') === 0 ? 2 : 1);
    if (c.rangeSum) ref.sub.textContent = last === null ? '' : 'Σ ' + rangeSumPeriod(data);
    ref.unit.textContent = last === null || !unit ? '' : (' ' + unit);
    if (c.secondaryKey) {
      var sec = current[c.secondaryKey];
      if (sec === undefined) sec = null;
      ref.sub.textContent = sec === null ? '' : (t(c.secondaryLabelKey) + ' ' + sec.toFixed(1) + ' ' + unit);
    }
    drawSpark(ref.spark, arr, c.color);
  });
  renderNow(data, current, nowMs);
  renderPanels(current);
  renderSensors(current);

  var hasWind = has(current.windSpeed) || (data.windDirDeg || []).some(has);
  document.getElementById('windPanel').style.display = hasWind ? '' : 'none';
  if (hasWind) drawCompass(data, current);
  buildRecords(data);

  CHARTS.forEach(function (c) {
    var series = c.series.map(function (s) { return { color: s.color, labelKey: s.labelKey, data: data[s.key] || [] }; })
      .filter(function (s) { return s.data.some(function (v) { return v !== null && v !== undefined; }); });
    var container = chartContainerEls[c.id];
    // multi-series charts keep the series with data; hidden once all are empty
    if (!series.length) { container.style.display = 'none'; delete chartState[c.id]; return; }
    container.style.display = '';
    var unit = c.unit || data[c.unitKey] || '';
    var decimals = unit.indexOf('in') === 0 ? 2 : (c.decimals === undefined ? 1 : c.decimals);
    var barLabel = t(timeRange === '7d' ? 'rainDay' : 'rainHour');
    var title = c.bars ? barLabel : t(c.titleKey);
    if (c.rangeSum) {
      var total = rangeSum(data[c.series[0].key]);
      if (total !== null) title += ' · Σ ' + total.toFixed(decimals) + ' ' + unit + ' (' + rangeSumPeriod(data) + ')';
    }
    if (c.id === 'pressure' && has(current.pressureTrend3h)) {
      title += ' · ' + (ptArrow(current.pressureTendency) || '') + ' ' + sgn(current.pressureTrend3h, decimals) + ' ' + unit + ' ' + t('per3h');
    }
    chartTitleEls[c.id].textContent = title;
    chartState[c.id] = { ageSec: data.ageSec || [], series: series, anchorMs: nowMs, minZero: c.minZero,
                         unit: (c.isTemp ? '°' : '') + unit, decimals: decimals, barLabel: barLabel,
                         bars: c.bars ? groupBuckets(data.ageSec || [], series[0].data, nowMs, timeRange === '7d') : null,
                         dir: c.id === 'wind' ? data.windDirDeg : null };
    drawChartFor(c.id);
  });
  updateTooltip();
}

// Skip re-rendering identical data (fast auto-refresh); ignore the per-second fields
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
    lastFetchMs = Date.now();
    updateUpdatedLabel();
    var sig = renderSignature(results[0], results[1]);
    if (sig === lastRenderSig) return;
    lastRenderSig = sig;
    render(JSON.parse(results[0]), results[1]);
  }).catch(function (e) {
    var empty = document.getElementById('empty');
    empty.style.display = 'block';
    empty.textContent = t('failedLoad') + e;
  });
}
// Redraw from cached data on resize
window.addEventListener('resize', function () { if (lastHist) render(lastHist, lastCurrent); });
// Sync the buttons to the persisted preferences (autoRefresh builds its own options)
updateUnitsButtonLabel();
updateRangeButtonLabel();
updateTimeModeLabel();
refresh();
applyAutoRefreshTimer();
</script>
</body>
</html>
)HTML";
