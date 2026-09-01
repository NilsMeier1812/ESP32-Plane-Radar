#include "services/config_server.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstring>

#include <esp_system.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/adsb_client.h"
#include "services/clock_time.h"
#include "services/radar_location.h"
#include "services/tracking.h"
#include "services/wifi_networks.h"
#include "ui/radar_range.h"

namespace services::config_server {

namespace {

WebServer s_server(80);
bool s_running = false;
bool s_changed = false;

// Companion config page. Static HTML/CSS/JS; all dynamic values come from
// /api/state so the markup can live in flash unchanged.
const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Plane Radar</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: system-ui, sans-serif; background: #0b0f0c;
         color: #e6f2ea; padding: 16px; max-width: 520px; margin: 0 auto; }
  h1 { font-size: 1.3rem; margin: 8px 0 4px; }
  h1 span { color: #3ad07a; }
  .sub { color: #7d8f84; font-size: .85rem; margin-bottom: 16px; }
  .card { background: #121a15; border: 1px solid #1f2c24; border-radius: 14px;
          padding: 16px; margin-bottom: 14px; }
  .card h2 { font-size: .8rem; text-transform: uppercase; letter-spacing: .08em;
             color: #7d8f84; margin: 0 0 12px; }
  label { display: block; font-size: .8rem; color: #9fb0a5; margin: 10px 0 4px; }
  input { width: 100%; padding: 11px 12px; border-radius: 10px; font-size: 1rem;
          background: #0b0f0c; border: 1px solid #263429; color: #e6f2ea; }
  input:focus { outline: none; border-color: #3ad07a; }
  .row { display: flex; gap: 8px; }
  .row > * { flex: 1; }
  button { border: none; border-radius: 10px; padding: 11px 14px; font-size: .95rem;
           font-weight: 600; background: #3ad07a; color: #04120a; cursor: pointer; }
  button.ghost { background: #1c2a22; color: #cfe6d8; }
  button:active { transform: translateY(1px); }
  .mt { margin-top: 10px; }
  .scales { display: flex; gap: 8px; flex-wrap: wrap; }
  .scales button { flex: 1 1 60px; }
  .scales button.active { outline: 2px solid #3ad07a; background: #04120a;
                          color: #3ad07a; }
  .cur { font-variant-numeric: tabular-nums; font-size: 1.05rem; }
  .toggle { display: flex; align-items: center; justify-content: space-between;
            padding: 8px 0; }
  .toast { position: fixed; left: 50%; bottom: 20px; transform: translateX(-50%);
           background: #1c2a22; color: #e6f2ea; padding: 10px 16px; border-radius: 10px;
           border: 1px solid #3ad07a; opacity: 0; transition: opacity .2s;
           pointer-events: none; font-size: .9rem; }
  .toast.show { opacity: 1; }
  .hint { color: #7d8f84; font-size: .75rem; margin-top: 6px; }
  .netlist { margin-bottom: 4px; }
  .net { display: flex; align-items: center; gap: 8px; padding: 8px 10px;
         background: #0b0f0c; border: 1px solid #263429; border-radius: 10px;
         margin-bottom: 6px; }
  .net span { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .net .tag { flex: 0 0 auto; color: #3ad07a; font-size: .75rem; }
  .net button { flex: 0 0 auto; padding: 6px 10px; font-size: .8rem;
                background: #2a1a1a; color: #e8b0b0; }
  .empty { color: #7d8f84; font-size: .8rem; margin-bottom: 8px; }
  .diag { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px;
          font-size: .8rem; }
  .diag b { color: #9fb0a5; font-weight: 500; white-space: nowrap; }
  .diag span { font-variant-numeric: tabular-nums; word-break: break-word; }
  .diag .bad { color: #ff9b9b; }
  .diag .good { color: #3ad07a; }
</style>
</head>
<body>
  <h1>Plane <span>Radar</span></h1>
  <div class="sub">Mittelpunkt: <span id="cur" class="cur">…</span></div>

  <div class="card">
    <h2>Position — ICAO-Code</h2>
    <div class="row">
      <input id="icao" placeholder="z.B. EDQH" maxlength="4"
             autocapitalize="characters" autocomplete="off">
      <button style="flex:0 0 auto" onclick="setIcao()">Setzen</button>
    </div>
    <div class="hint">Flugplatz-Code — direkt vom Gerät aufgelöst.</div>
  </div>

  <div class="card">
    <h2>Position — Koordinaten</h2>
    <div class="row">
      <div><label>Breite (lat)</label><input id="lat" type="number" step="0.000001"></div>
      <div><label>Länge (lon)</label><input id="lon" type="number" step="0.000001"></div>
    </div>
    <button class="mt" style="width:100%" onclick="setLatLon()">Koordinaten setzen</button>
    <button class="ghost mt" style="width:100%" onclick="useGps()">📍 Aktuelle Position vom Handy</button>
    <div class="hint">GPS öffnet eine gesicherte Helfer-Seite (der Browser gibt
      GPS nur über HTTPS frei) und schickt die Koordinaten zurück ans Gerät.</div>
  </div>

  <div class="card">
    <h2>Skalierung</h2>
    <div id="scales" class="scales"></div>
  </div>

  <div class="card">
    <h2>Flug verfolgen</h2>
    <div class="row">
      <input id="track" placeholder="Callsign / Kennzeichen (z.B. DLH400)"
             autocapitalize="characters" autocomplete="off">
      <button style="flex:0 0 auto" onclick="trackStart()">Verfolgen</button>
    </div>
    <button class="ghost mt" style="width:100%" onclick="trackStop()">Tracking stoppen</button>
    <div id="trackstatus" class="hint"></div>
  </div>

  <div class="card">
    <h2>WLAN-Netzwerke</h2>
    <div id="netlist" class="netlist"></div>
    <label>Netzwerkname (SSID)</label>
    <div class="row">
      <input id="netssid" list="scanlist" placeholder="z.B. Fritz!Box 7590"
             autocomplete="off">
      <button style="flex:0 0 auto" class="ghost" onclick="scanNets()">Suchen</button>
    </div>
    <datalist id="scanlist"></datalist>
    <label>Passwort</label>
    <input id="netpass" type="password" placeholder="leer lassen bei offenem Netz"
           autocomplete="new-password">
    <button class="mt" style="width:100%" onclick="addNet()">Netzwerk speichern</button>
    <div class="hint">Bis zu 5 Netzwerke. Beim Verbinden nimmt das Gerät das
      verfügbare Netz mit dem stärksten Signal — so läuft es an mehreren Orten
      ohne Neueinrichtung.</div>
  </div>

  <div class="card">
    <h2>Höhenfilter</h2>
    <div class="row">
      <div><label>Mindesthöhe (ft)</label>
        <input id="altmin" type="number" min="0" step="500" placeholder="0 = aus"></div>
      <div><label>Maximalhöhe (ft)</label>
        <input id="altmax" type="number" min="0" step="500" placeholder="0 = aus"></div>
    </div>
    <button class="mt" style="width:100%" onclick="setAltFilter()">Filter übernehmen</button>
    <div class="hint">Blendet Flugzeuge außerhalb des Bereichs aus. 0 bedeutet
      keine Grenze. Maschinen ohne Höhenangabe bleiben immer sichtbar.</div>
  </div>

  <div class="card">
    <h2>Anzeige</h2>
    <div class="toggle"><span>Entfernungen in Meilen</span>
      <input id="miles" type="checkbox" style="width:auto" onchange="setOptions()"></div>
    <div class="toggle"><span>Landebahnen anzeigen</span>
      <input id="runways" type="checkbox" style="width:auto" onchange="setOptions()"></div>
    <div class="toggle"><span>Spuren hinter Flugzeugen</span>
      <input id="trails" type="checkbox" style="width:auto" onchange="setOptions()"></div>
    <div class="toggle"><span>Automatischer Zoom</span>
      <input id="autozoom" type="checkbox" style="width:auto" onchange="setOptions()"></div>
    <div class="hint">Auto-Zoom weitet den Bereich, wenn nichts fliegt, und
      zieht ihn zusammen, wenn es voll wird.</div>
  </div>

  <div class="card">
    <h2>Diagnose</h2>
    <div id="diag" class="diag"></div>
    <div class="row mt">
      <button class="ghost" onclick="load(true)">Aktualisieren</button>
      <button class="ghost" onclick="reconnect()">Verbindung neu aufbauen</button>
    </div>
    <div class="toggle"><span>TLS-Zertifikat prüfen</span>
      <input id="tlsverify" type="checkbox" style="width:auto" onchange="setOptions()"></div>
    <div class="hint">Wenn „Letzter Fehler" dauerhaft etwas meldet, steht hier
      die Ursache: „keine Verbindung" = Netz/TLS, „Ratenlimit" = die API
      bremst, „zu wenig Speicher" = Heap voll. Zum Ausschließen von TLS den
      Haken kurz entfernen.</div>
  </div>

  <div class="card">
    <h2>Firmware</h2>
    <div class="hint" id="fwinfo" style="margin-top:0"></div>
    <label>Firmware-Datei (.bin)</label>
    <input id="fwfile" type="file" accept=".bin">
    <button class="mt" style="width:100%" onclick="upload()">Update installieren</button>
    <div class="hint" id="fwstatus">Das Gerät startet nach dem Update neu. Bei
      einem Fehler läuft die bisherige Version einfach weiter.</div>
  </div>

  <div id="toast" class="toast"></div>
<script>
let S = {};
function toast(m){const t=document.getElementById('toast');t.textContent=m;
  t.classList.add('show');clearTimeout(t._t);t._t=setTimeout(()=>t.classList.remove('show'),1800);}
async function api(path, body){
  const opt = body ? {method:'POST', headers:{'Content-Type':'application/json'},
                      body:JSON.stringify(body)} : {};
  const r = await fetch(path, opt); return r.json();
}
function fmt(v){return Number(v).toFixed(5);}
function render(){
  document.getElementById('cur').textContent = fmt(S.lat)+', '+fmt(S.lon);
  document.getElementById('lat').value = fmt(S.lat);
  document.getElementById('lon').value = fmt(S.lon);
  document.getElementById('miles').checked = !!S.use_miles;
  document.getElementById('runways').checked = !!S.show_runways;
  document.getElementById('trails').checked = !!S.trails;
  document.getElementById('autozoom').checked = !!S.auto_zoom;
  document.getElementById('altmin').value = S.alt_min_ft || 0;
  document.getElementById('altmax').value = S.alt_max_ft || 0;
  document.getElementById('tlsverify').checked = !!S.tls_verify_enabled;
  renderDiag();
  document.getElementById('fwinfo').textContent =
    'Version ' + (S.version||'?') + ' · Zeit ' + (S.time_utc||'?') +
    ' · TLS ' + (S.tls_verified ? 'geprüft' : 'ungeprüft');
  document.getElementById('track').value = S.track_target || '';
  const tsmap={off:'Kein Flug verfolgt',searching:'Suche…',tracking:'Wird verfolgt',lost:'Signal verloren'};
  document.getElementById('trackstatus').textContent = tsmap[S.track_state]||'';
  const nets = document.getElementById('netlist'); nets.innerHTML='';
  if(!(S.networks||[]).length){
    nets.innerHTML = '<div class="empty">Noch kein Netzwerk gespeichert.</div>';
  }
  (S.networks||[]).forEach(n=>{
    const row=document.createElement('div'); row.className='net';
    const name=document.createElement('span'); name.textContent=n.ssid;
    row.appendChild(name);
    if(n.current){const t=document.createElement('span');t.className='tag';
      t.textContent='verbunden';row.appendChild(t);}
    const del=document.createElement('button'); del.textContent='Entfernen';
    del.onclick=()=>removeNet(n.ssid); row.appendChild(del);
    nets.appendChild(row);
  });
  const box = document.getElementById('scales'); box.innerHTML='';
  (S.presets||[]).forEach((km,i)=>{
    const b=document.createElement('button');
    b.textContent = S.use_miles ? Math.round(km/1.609344)+' mi' : km+' km';
    if(i===S.range_index) b.className='active';
    b.onclick=()=>setScale(i); box.appendChild(b);
  });
}
function renderDiag(){
  const d = S.diag || {};
  const box = document.getElementById('diag');
  const fails = d.fails_in_row || 0;
  const age = (d.data_age_s === undefined || d.data_age_s < 0)
    ? 'noch nie' : d.data_age_s + ' s';
  const rows = [
    ['Flugzeuge', (d.aircraft ?? '?') + ''],
    ['Daten alt', age, fails > 0 ? 'bad' : 'good'],
    ['Fehler in Folge', fails + '', fails > 0 ? 'bad' : 'good'],
    ['Letzter Fehler', d.last_error || '—', d.last_error ? 'bad' : 'good'],
    ['Letzter Code', (d.last_http_code ?? 0) + ''],
    ['Abrufe ok / Fehler', (d.ok_count||0) + ' / ' + (d.fail_count||0)],
    ['Abrufdauer', (d.last_duration_ms||0) + ' ms'],
    ['Abstand', Math.round((d.fetch_interval_ms||0)/1000) + ' s'],
    ['Heap frei / min', Math.round((d.heap_free||0)/1024) + ' / ' +
                        Math.round((d.heap_min||0)/1024) + ' kB'],
    ['WLAN-Signal', (d.rssi ?? '?') + ' dBm'],
    ['Laufzeit', Math.floor((d.uptime_s||0)/60) + ' min'],
    ['Neustartgrund', (d.reset_reason ?? '?') + ''],
    ['TLS', S.tls_verified ? 'geprüft' : 'ungeprüft'],
  ];
  box.innerHTML = '';
  rows.forEach(([k,v,cls])=>{
    const b=document.createElement('b'); b.textContent=k; box.appendChild(b);
    const sp=document.createElement('span'); sp.textContent=v;
    if(cls) sp.className=cls; box.appendChild(sp);
  });
}
async function load(showToast){
  S = await api('/api/state'); render();
  if(showToast) toast('Aktualisiert');
}
async function reconnect(){
  const r=await api('/api/reconnect',{});
  if(r.ok){S=r.state;render();toast(r.msg||'Neu verbunden');}
}
async function setIcao(){
  const v=document.getElementById('icao').value.trim();
  if(!v) return;
  const r=await api('/api/center',{icao:v});
  if(r.ok){S=r.state;render();toast('Zentriert auf '+v.toUpperCase());}
  else toast(r.msg||'ICAO nicht gefunden');
}
async function setLatLon(){
  const lat=parseFloat(document.getElementById('lat').value);
  const lon=parseFloat(document.getElementById('lon').value);
  const r=await api('/api/center',{lat,lon});
  if(r.ok){S=r.state;render();toast('Position gesetzt');} else toast(r.msg||'Ungültig');
}
async function setScale(i){
  const r=await api('/api/scale',{index:i});
  if(r.ok){S=r.state;render();toast('Skalierung gesetzt');}
}
async function setOptions(){
  const r=await api('/api/options',{
    miles:document.getElementById('miles').checked,
    runways:document.getElementById('runways').checked,
    trails:document.getElementById('trails').checked,
    auto_zoom:document.getElementById('autozoom').checked,
    tls_verify:document.getElementById('tlsverify').checked});
  if(r.ok){S=r.state;render();}
}
async function setAltFilter(){
  const r=await api('/api/options',{
    alt_min_ft:parseInt(document.getElementById('altmin').value||'0',10),
    alt_max_ft:parseInt(document.getElementById('altmax').value||'0',10)});
  if(r.ok){S=r.state;render();toast('Höhenfilter gesetzt');}
}
function upload(){
  const f=document.getElementById('fwfile').files[0];
  if(!f){toast('Bitte eine .bin-Datei wählen');return;}
  const st=document.getElementById('fwstatus');
  const fd=new FormData(); fd.append('firmware',f,f.name);
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/api/update');
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable)
      st.textContent='Übertrage… '+Math.round(e.loaded/e.total*100)+'%';
  };
  xhr.onload=()=>{
    let m='Update fehlgeschlagen';
    try{m=JSON.parse(xhr.responseText).msg||m;}catch(e){}
    st.textContent=m; toast(m);
  };
  xhr.onerror=()=>{st.textContent='Verbindung beim Update verloren';};
  st.textContent='Übertrage… 0%';
  xhr.send(fd);
}
function useGps(){
  const url = S.gps_helper + (S.gps_helper.includes('?')?'&':'?') + 'device=' +
              encodeURIComponent(S.ip);
  window.location.href = url;
}
async function addNet(){
  const ssid=document.getElementById('netssid').value.trim();
  if(!ssid) return;
  const pass=document.getElementById('netpass').value;
  const r=await api('/api/wifi',{ssid,pass});
  if(r.ok){S=r.state;render();
    document.getElementById('netssid').value='';
    document.getElementById('netpass').value='';
    toast('Netzwerk gespeichert');}
  else toast(r.msg||'Fehler');
}
async function removeNet(ssid){
  if(!confirm('Netzwerk "'+ssid+'" entfernen?')) return;
  const r=await api('/api/wifi',{remove:ssid});
  if(r.ok){S=r.state;render();toast('Netzwerk entfernt');} else toast(r.msg||'Fehler');
}
async function scanNets(){
  toast('Suche Netzwerke…');
  const r=await api('/api/wifi/scan');
  const list=document.getElementById('scanlist'); list.innerHTML='';
  (r.networks||[]).forEach(n=>{
    const o=document.createElement('option'); o.value=n.ssid;
    o.label=n.rssi+' dBm'; list.appendChild(o);
  });
  toast((r.networks||[]).length+' Netzwerke gefunden');
}
async function trackStart(){
  const v=document.getElementById('track').value.trim();
  if(!v) return;
  const r=await api('/api/track',{target:v});
  if(r.ok){S=r.state;render();toast('Verfolge '+v.toUpperCase());} else toast(r.msg||'Fehler');
}
async function trackStop(){
  const r=await api('/api/track',{stop:true});
  if(r.ok){S=r.state;render();toast('Tracking gestoppt');}
}
load();
</script>
</body>
</html>)HTML";

void fillState(JsonObject o) {
  o["lat"] = services::location::lat();
  o["lon"] = services::location::lon();
  o["range_index"] = ui::radar::rangeIndex();
  o["ring3_km"] = ui::radar::rangeCurrent().ring3_km;
  o["outer_km"] = ui::radar::rangeCurrent().outer_km;
  o["use_miles"] = ui::radar::useMiles();
  o["show_runways"] = ui::radar::showRunways();
  o["alt_min_ft"] = ui::radar::altMinFt();
  o["alt_max_ft"] = ui::radar::altMaxFt();
  o["trails"] = ui::radar::showTrails();
  o["auto_zoom"] = ui::radar::autoZoom();
  o["version"] = config::kFirmwareVersion;
  o["time_utc"] = services::clock_time::isoUtc();
  o["tls_verified"] = services::adsb::tlsVerified();
  o["tls_verify_enabled"] = services::adsb::tlsVerifyEnabled();

  const services::adsb::Health& h = services::adsb::health();
  JsonObject diag = o["diag"].to<JsonObject>();
  diag["uptime_s"] = millis() / 1000UL;
  diag["heap_free"] = ESP.getFreeHeap();
  diag["heap_min"] = ESP.getMinFreeHeap();
  diag["rssi"] = WiFi.RSSI();
  diag["reset_reason"] = static_cast<int>(esp_reset_reason());
  diag["last_http_code"] = h.last_http_code;
  diag["fails_in_row"] = h.consecutive_failures;
  diag["ok_count"] = h.ok_count;
  diag["fail_count"] = h.fail_count;
  diag["last_error"] = h.last_error;
  diag["last_duration_ms"] = h.last_duration_ms;
  diag["heap_before_last"] = h.heap_before_last;
  diag["fetch_interval_ms"] = services::adsb::fetchIntervalMs();
  diag["data_age_s"] =
      (h.last_ok_ms == 0) ? -1 : static_cast<long>((millis() - h.last_ok_ms) / 1000UL);
  diag["aircraft"] = services::adsb::aircraftCount();
  o["ip"] = WiFi.localIP().toString();
  o["ssid"] = WiFi.SSID();
  JsonArray nets = o["networks"].to<JsonArray>();
  for (size_t i = 0; i < services::wifi_networks::count(); ++i) {
    const char* ssid = services::wifi_networks::at(i).ssid;
    JsonObject n = nets.add<JsonObject>();
    n["ssid"] = ssid;  // passwords are never sent back out
    n["current"] = WiFi.SSID() == ssid;
  }
  o["gps_helper"] = config::kGpsHelperUrl;
  o["track_active"] = services::tracking::active();
  o["track_target"] = services::tracking::target();
  const char* ts = "off";
  switch (services::tracking::state()) {
    case services::tracking::State::Searching: ts = "searching"; break;
    case services::tracking::State::Tracking: ts = "tracking"; break;
    case services::tracking::State::Lost: ts = "lost"; break;
    case services::tracking::State::Off: ts = "off"; break;
  }
  o["track_state"] = ts;
  JsonArray presets = o["presets"].to<JsonArray>();
  for (size_t i = 0; i < ui::radar::kRangePresetCount; ++i) {
    presets.add(ui::radar::kRangePresets[i].ring3_km);
  }
}

void sendState(int code, bool ok, const char* msg) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (msg != nullptr) {
    doc["msg"] = msg;
  }
  fillState(doc["state"].to<JsonObject>());
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

void handleStateGet() {
  JsonDocument doc;
  fillState(doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

// GET with ?lat=&lon= is used by the HTTPS GPS helper page, which redirects the
// browser here (a top-level navigation, allowed from HTTPS to this HTTP page).
void handleCenterGet() {
  if (s_server.hasArg("lat") && s_server.hasArg("lon")) {
    const double lat = s_server.arg("lat").toDouble();
    const double lon = s_server.arg("lon").toDouble();
    if (services::location::saveLatLon(lat, lon)) {
      s_changed = true;
      s_server.sendHeader("Location", "/");
      s_server.send(303, "text/plain", "ok");
      return;
    }
  }
  s_server.sendHeader("Location", "/");
  s_server.send(303, "text/plain", "bad");
}

void handleCenterPost() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    sendState(400, false, "Ungültige Anfrage");
    return;
  }
  const char* icao = doc["icao"] | "";
  if (icao[0] != '\0') {
    if (services::location::saveFromIcao(icao)) {
      s_changed = true;
      sendState(200, true, nullptr);
    } else {
      sendState(404, false, "ICAO nicht im Datensatz");
    }
    return;
  }
  if (doc["lat"].is<double>() && doc["lon"].is<double>()) {
    if (services::location::saveLatLon(doc["lat"].as<double>(),
                                       doc["lon"].as<double>())) {
      s_changed = true;
      sendState(200, true, nullptr);
    } else {
      sendState(400, false, "Koordinaten außerhalb des gültigen Bereichs");
    }
    return;
  }
  sendState(400, false, "lat/lon oder icao erforderlich");
}

void handleScalePost() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain")) || !doc["index"].is<int>()) {
    sendState(400, false, "index erforderlich");
    return;
  }
  ui::radar::rangeSetIndex(static_cast<uint8_t>(doc["index"].as<int>()));
  s_changed = true;
  sendState(200, true, nullptr);
}

void handleOptionsPost() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    sendState(400, false, "Ungültige Anfrage");
    return;
  }
  if (doc["miles"].is<bool>()) {
    ui::radar::saveMilesFromPortal(doc["miles"].as<bool>() ? "T" : "");
  }
  if (doc["runways"].is<bool>()) {
    ui::radar::saveRunwaysFromPortal(doc["runways"].as<bool>() ? "T" : "");
  }
  if (doc["trails"].is<bool>()) {
    ui::radar::setShowTrails(doc["trails"].as<bool>());
  }
  if (doc["auto_zoom"].is<bool>()) {
    ui::radar::setAutoZoom(doc["auto_zoom"].as<bool>());
  }
  if (doc["tls_verify"].is<bool>()) {
    services::adsb::setTlsVerifyEnabled(doc["tls_verify"].as<bool>());
  }
  if (doc["alt_min_ft"].is<int>() || doc["alt_max_ft"].is<int>()) {
    ui::radar::setAltFilter(doc["alt_min_ft"] | ui::radar::altMinFt(),
                            doc["alt_max_ft"] | ui::radar::altMaxFt());
  }
  s_changed = true;
  sendState(200, true, nullptr);
}

// Add / update / remove a saved network. Adding does not disturb the current
// link: the new entry is simply part of the candidate list next time the
// device connects (or is moved to another place).
void handleWifiPost() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    sendState(400, false, "Ungültige Anfrage");
    return;
  }

  const char* remove_ssid = doc["remove"] | "";
  if (remove_ssid[0] != '\0') {
    if (!services::wifi_networks::remove(remove_ssid)) {
      sendState(404, false, "Netzwerk nicht gespeichert");
      return;
    }
    sendState(200, true, nullptr);
    return;
  }

  const char* ssid = doc["ssid"] | "";
  if (ssid[0] == '\0') {
    sendState(400, false, "SSID erforderlich");
    return;
  }
  if (strlen(ssid) >= services::wifi_networks::kSsidLen) {
    sendState(400, false, "SSID zu lang");
    return;
  }
  const char* pass = doc["pass"] | "";
  if (strlen(pass) >= services::wifi_networks::kPassLen) {
    sendState(400, false, "Passwort zu lang");
    return;
  }
  if (!services::wifi_networks::add(ssid, pass)) {
    sendState(409, false, "Liste voll — erst ein Netzwerk entfernen");
    return;
  }
  sendState(200, true, nullptr);
}

// On-demand scan so the SSID can be picked instead of typed. Blocks for a
// couple of seconds and briefly interrupts traffic, hence not automatic.
void handleWifiScanGet() {
  const int found = WiFi.scanNetworks(false /*async*/, false /*show_hidden*/);
  JsonDocument doc;
  JsonArray nets = doc["networks"].to<JsonArray>();
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) {
      continue;
    }
    bool duplicate = false;
    for (JsonObject seen : nets) {
      if (ssid == seen["ssid"].as<const char*>()) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    JsonObject n = nets.add<JsonObject>();
    n["ssid"] = ssid;
    n["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

void handleTrackPost() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    sendState(400, false, "Ungültige Anfrage");
    return;
  }
  if (doc["stop"].is<bool>() && doc["stop"].as<bool>()) {
    services::tracking::stop();
    s_changed = true;
    sendState(200, true, nullptr);
    return;
  }
  const char* t = doc["target"] | "";
  if (t[0] == '\0') {
    sendState(400, false, "Callsign oder Kennzeichen erforderlich");
    return;
  }
  services::tracking::start(t);
  s_changed = true;
  sendState(200, true, nullptr);
}

// --- OTA ---
// The upload arrives in chunks through WebServer's upload callback; each one is
// streamed straight into the inactive OTA slot, so no buffering of a 1.3 MB
// image in RAM (which would never fit).
bool s_ota_ok = false;
String s_ota_error;

void handleUpdatePost() {
  const bool ok = s_ota_ok && !Update.hasError();
  JsonDocument doc;
  doc["ok"] = ok;
  doc["msg"] = ok ? "Update installiert — Gerät startet neu"
                  : (s_ota_error.length() > 0 ? s_ota_error
                                              : String("Update fehlgeschlagen"));
  String out;
  serializeJson(doc, out);
  s_server.sendHeader("Connection", "close");
  s_server.send(ok ? 200 : 500, "application/json", out);

  if (ok) {
    Serial.println("OTA: update installed, restarting");
    delay(400);
    ESP.restart();
  }
}

void handleUpdateUpload() {
  HTTPUpload& upload = s_server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    s_ota_ok = false;
    s_ota_error = "";
    Serial.printf("OTA: receiving %s\n", upload.filename.c_str());
    // UPDATE_SIZE_UNKNOWN: the browser does not tell us the length up front,
    // so the whole free OTA partition is erased.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      s_ota_error = "Kein Platz für das Update";
      Update.printError(Serial);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (s_ota_error.length() > 0) {
      return;
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      s_ota_error = "Schreibfehler beim Update";
      Update.printError(Serial);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (s_ota_error.length() > 0) {
      Update.abort();
      return;
    }
    if (Update.end(true)) {
      s_ota_ok = true;
      Serial.printf("OTA: %u bytes written\n",
                    static_cast<unsigned>(upload.totalSize));
    } else {
      s_ota_error = "Ungültiges Firmware-Image";
      Update.printError(Serial);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_ota_error = "Upload abgebrochen";
  }
}

/** Drop the pooled TLS connection and retry at full speed. */
void handleReconnectPost() {
  services::adsb::resetConnection();
  sendState(200, true, "Verbindung wird neu aufgebaut");
}

void handlePage() { s_server.send_P(200, "text/html; charset=utf-8", kPage); }

}  // namespace

void announce() {
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.setInstanceName("Plane Radar");
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local\n", config::kPortalHostname);
  } else {
    Serial.println("mDNS: responder failed to start");
  }
#endif
}

void begin() {
  if (s_running) {
    return;
  }
  s_server.on("/", HTTP_GET, handlePage);
  s_server.on("/api/state", HTTP_GET, handleStateGet);
  s_server.on("/api/center", HTTP_GET, handleCenterGet);
  s_server.on("/api/center", HTTP_POST, handleCenterPost);
  s_server.on("/api/scale", HTTP_POST, handleScalePost);
  s_server.on("/api/options", HTTP_POST, handleOptionsPost);
  s_server.on("/api/track", HTTP_POST, handleTrackPost);
  s_server.on("/api/wifi", HTTP_POST, handleWifiPost);
  s_server.on("/api/wifi/scan", HTTP_GET, handleWifiScanGet);
  s_server.on("/api/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
  s_server.on("/api/reconnect", HTTP_POST, handleReconnectPost);
  s_server.onNotFound(handlePage);
  s_server.begin();
  s_running = true;

  announce();
  Serial.printf("Config page: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stop() {
  if (!s_running) {
    return;
  }
  s_server.stop();
  s_running = false;
#ifdef WM_MDNS
  MDNS.end();
#endif
}

bool running() { return s_running; }

void handle() {
  if (s_running) {
    s_server.handleClient();
  }
}

bool consumeChanged() {
  const bool c = s_changed;
  s_changed = false;
  return c;
}

}  // namespace services::config_server
