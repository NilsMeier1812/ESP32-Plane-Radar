#include "services/config_server.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/radar_location.h"
#include "services/tracking.h"
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
    <h2>Anzeige</h2>
    <div class="toggle"><span>Entfernungen in Meilen</span>
      <input id="miles" type="checkbox" style="width:auto" onchange="setOptions()"></div>
    <div class="toggle"><span>Landebahnen anzeigen</span>
      <input id="runways" type="checkbox" style="width:auto" onchange="setOptions()"></div>
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
  document.getElementById('track').value = S.track_target || '';
  const tsmap={off:'Kein Flug verfolgt',searching:'Suche…',tracking:'Wird verfolgt',lost:'Signal verloren'};
  document.getElementById('trackstatus').textContent = tsmap[S.track_state]||'';
  const box = document.getElementById('scales'); box.innerHTML='';
  (S.presets||[]).forEach((km,i)=>{
    const b=document.createElement('button');
    b.textContent = S.use_miles ? Math.round(km/1.609344)+' mi' : km+' km';
    if(i===S.range_index) b.className='active';
    b.onclick=()=>setScale(i); box.appendChild(b);
  });
}
async function load(){ S = await api('/api/state'); render(); }
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
    runways:document.getElementById('runways').checked});
  if(r.ok){S=r.state;render();}
}
function useGps(){
  const url = S.gps_helper + (S.gps_helper.includes('?')?'&':'?') + 'device=' +
              encodeURIComponent(S.ip);
  window.location.href = url;
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
  o["ip"] = WiFi.localIP().toString();
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
  s_changed = true;
  sendState(200, true, nullptr);
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

void handlePage() { s_server.send_P(200, "text/html; charset=utf-8", kPage); }

}  // namespace

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
  s_server.onNotFound(handlePage);
  s_server.begin();
  s_running = true;

#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
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
