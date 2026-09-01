#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <esp_heap_caps.h>

#include <cctype>
#include <cmath>
#include <cstring>

#include "config.h"
#include "services/clock_time.h"

/**
 * Root CA bundle embedded in the ESP-IDF image (esp_crt_bundle component).
 * Referencing the symbol directly avoids shipping a copy of the Mozilla root
 * list in this repo and keeps it in step with the framework.
 */
extern const uint8_t kRootCaBundle[] asm("_binary_x509_crt_bundle_start");

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
/**
 * A TLS connect needs seconds, not milliseconds. The old 200 ms budget made
 * every connect fail, so the code retried the handshake back to back for the
 * whole request window — which saturated the radio and left the config page
 * unreachable. One honest attempt plus one retry is both faster and quieter.
 */
constexpr int kConnectAttemptMs = 4000;
constexpr uint8_t kConnectAttempts = 2;
constexpr unsigned long kRequestTimeoutMs = 6000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;

// Persistent TLS client + HTTP client so the connection to adsb.fi is kept
// alive (setReuse) and the expensive TLS handshake is not repeated every fetch.
WiFiClientSecure s_client;
HTTPClient s_http;
bool s_http_ready = false;
bool s_tls_verified = false;
uint8_t s_tls_failures = 0;
bool s_tls_gave_up = false;
/** Own namespace so it cannot collide with the other Preferences handles. */
constexpr char kPrefsNamespace[] = "adsb";
constexpr char kPrefsTlsKey[] = "tlsVerify";

bool s_tls_verify_enabled = config::kAdsbVerifyTls;
bool s_prefs_loaded = false;
bool s_force_rebuild = false;
/** When the pooled connection was built; recycled periodically (see below). */
unsigned long s_client_built_ms = 0;

Health s_health = {};

void setError(const char* msg) {
  strncpy(s_health.last_error, msg, sizeof(s_health.last_error) - 1);
  s_health.last_error[sizeof(s_health.last_error) - 1] = '\0';
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  // A connect blocks the whole loop (the web server included), so once we are
  // already in a failure streak a second attempt only doubles the stall — the
  // backoff will come back around soon enough.
  const uint8_t attempts =
      (s_health.consecutive_failures > 0) ? 1 : kConnectAttempts;
  int code = HTTPC_ERROR_CONNECTION_REFUSED;
  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    pollNetwork();
    code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    pollNetwork();
    delay(20);
  }
  return code;
}

/**
 * Read the body through HTTPClient rather than off the raw stream.
 *
 * The API answers with Transfer-Encoding: chunked, where getSize() is -1 and
 * the stream carries the chunk framing (hex length lines and CRLFs) inline.
 * Reading it directly meant the loop never saw an end and ran into its timeout
 * every time, and the payload was not valid JSON — ArduinoJson parsed the
 * leading hex length as a number, reported success, and left an empty document
 * behind. The result was a radar that fetched happily and showed nothing.
 */
bool readResponseBody(HTTPClient& http, String& payload) {
  pollNetwork();
  payload = http.getString();
  pollNetwork();
  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

/** Numeric altitude in feet; false when the aircraft reports none (or "ground"). */
bool readAltitudeFt(const JsonObject& plane, float* out_ft) {
  return readJsonFloat(plane, "alt_baro", out_ft) ||
         readJsonFloat(plane, "alt_geom", out_ft);
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readAltitudeFt(plane, &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  ac->alt_ft = 0.0f;
  ac->alt_known = readAltitudeFt(plane, &ac->alt_ft);
}

// True if plane[field] equals key ignoring case and trailing spaces.
bool fieldMatches(const JsonObject& plane, const char* field, const char* key) {
  if (!plane[field].is<const char*>()) {
    return false;
  }
  const char* s = plane[field].as<const char*>();
  char buf[16];
  size_t n = 0;
  for (const char* c = s; *c != '\0' && n < sizeof(buf) - 1; ++c) {
    if (*c == ' ') {
      continue;
    }
    buf[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
  }
  buf[n] = '\0';
  return std::strcmp(buf, key) == 0;
}

bool planeMatches(const JsonObject& plane, const char* key) {
  return fieldMatches(plane, "flight", key) || fieldMatches(plane, "r", key);
}

/**
 * Pick the TLS trust mode and (re)configure the client when it changes.
 * Certificates cannot be judged before the clock is set, so verification turns
 * itself on the moment NTP lands.
 */
/** The switch has to survive a reboot, or turning it off fixes nothing. */
void loadPrefsOnce() {
  if (s_prefs_loaded) {
    return;
  }
  s_prefs_loaded = true;
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  s_tls_verify_enabled = prefs.getBool(kPrefsTlsKey, config::kAdsbVerifyTls);
  prefs.end();
}

void ensureTlsMode() {
  loadPrefsOnce();
  const bool want_verify = s_tls_verify_enabled && !s_tls_gave_up &&
                           services::clock_time::synced();
  const unsigned long now = millis();
  // Recycle the pooled connection now and then: a keep-alive socket the peer
  // dropped, or a TLS session that got wedged, otherwise stays broken until
  // the next reboot — which is what a radar that "just stops" looks like.
  const bool stale = s_http_ready && s_client_built_ms != 0 &&
                     now - s_client_built_ms > config::kAdsbConnectionMaxAgeMs;

  if (s_http_ready && !s_force_rebuild && !stale &&
      want_verify == s_tls_verified) {
    return;
  }

  if (s_http_ready) {
    Serial.printf("adsb: rebuilding connection (%s)\n",
                  s_force_rebuild ? "after failures"
                                  : (stale ? "age" : "trust mode"));
  }
  s_http.end();
  s_client.stop();
  if (want_verify) {
    s_client.setCACertBundle(kRootCaBundle);
    Serial.println("adsb: TLS certificate verification on");
  } else {
    s_client.setInsecure();
  }
  s_tls_verified = want_verify;
  s_http.setReuse(true);  // keep the TLS connection alive between requests
  s_http_ready = true;
  s_force_rebuild = false;
  s_client_built_ms = now;
}

/** A verified connection that keeps failing falls back rather than going dark. */
void noteTlsFailure() {
  if (!s_tls_verified || s_tls_gave_up) {
    return;
  }
  if (++s_tls_failures < config::kAdsbTlsFailuresBeforeFallback) {
    return;
  }
  s_tls_gave_up = true;
  s_force_rebuild = true;

  // Remember it. Otherwise every reboot repeats the same sequence: fetches
  // work until NTP lands, verification switches itself on, fails, and the
  // radar goes quiet for as long as the fallback takes — which is exactly the
  // "runs fine for fifteen seconds after a restart" report.
  s_tls_verify_enabled = false;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsTlsKey, false);
    prefs.end();
  }

  // Recovering is now one unverified request away, so do not make the backoff
  // sit on it for another eight seconds.
  s_health.consecutive_failures = 0;

  Serial.println(
      "adsb: certificate verification failed repeatedly — switched off and "
      "saved (this board has too little contiguous heap for the handshake)");
}

void noteFailure(int code, Fail kind, const char* what) {
  s_health.last_fail = kind;
  ++s_health.fail_count;
  ++s_health.consecutive_failures;
  s_health.last_http_code = code;
  setError(what);
  // Every few failures, throw the connection away instead of retrying into a
  // socket that may already be unusable.
  if (s_health.consecutive_failures % config::kAdsbRebuildEveryNFailures == 0) {
    s_force_rebuild = true;
  }
  Serial.printf("adsb: fetch failed (%s, code %d, %u in a row, heap %u)\n", what,
                code, static_cast<unsigned>(s_health.consecutive_failures),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

void noteSuccess() {
  ++s_health.ok_count;
  s_health.consecutive_failures = 0;
  s_health.last_ok_ms = millis();
  s_health.last_http_code = HTTP_CODE_OK;
  s_health.last_fail = Fail::None;
  s_tls_failures = 0;
  setError("");
}

bool httpGetJson(const String& url, JsonDocument& doc) {
  const unsigned long started = millis();
  s_health.last_attempt_ms = started;
  s_health.heap_before_last = ESP.getFreeHeap();
  // A handshake needs one big contiguous block; total free can look fine while
  // the largest run is far too small, so judge on that.
  s_health.largest_block_before =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  struct DurationGuard {
    unsigned long start;
    ~DurationGuard() {
      s_health.last_duration_ms = static_cast<uint32_t>(millis() - start);
    }
  } guard{started};

  // A TLS handshake needs a sizeable contiguous block. Attempting one without
  // it stalls the loop for seconds and fails anyway, so skip and say so.
  if (s_health.largest_block_before < config::kAdsbMinHeapForFetch) {
    noteFailure(0, Fail::LowHeap, "zu wenig Speicher");
    s_force_rebuild = true;
    return false;
  }

  ensureTlsMode();
  if (!s_http.begin(s_client, url)) {
    noteFailure(0, Fail::BeginFailed, "http.begin fehlgeschlagen");
    s_force_rebuild = true;
    return false;
  }
  s_http.setTimeout(kRequestTimeoutMs);
  // Public APIs are within their rights to refuse anonymous clients, and it
  // makes this device identifiable in adsb.fi's logs.
  s_http.setUserAgent(config::kAdsbUserAgent);
  const int code = performGetWithPoll(s_http);
  if (code != HTTP_CODE_OK) {
    s_http.end();
    if (code < 0) {
      noteFailure(code, Fail::NoConnection, "keine Verbindung");
      noteTlsFailure();  // a rejected handshake also surfaces as a negative code
    } else if (code == 429) {
      noteFailure(code, Fail::RateLimit, "Ratenlimit (HTTP 429)");
    } else {
      noteFailure(code, Fail::HttpError, "HTTP-Fehler");
    }
    return false;
  }
  String payload;
  if (!readResponseBody(s_http, payload)) {
    s_http.end();
    noteFailure(code, Fail::EmptyBody, "leere Antwort");
    s_force_rebuild = true;
    return false;
  }
  s_http.end();  // with setReuse(true) this keeps the socket open for next time

  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    noteFailure(code, Fail::BadJson, err.c_str());
    return false;
  }
  // deserializeJson stops after the first complete value and ignores whatever
  // follows, so a mangled body can parse "successfully" into a number. Insist
  // on the object the API actually returns, or the failure stays invisible.
  if (!doc.is<JsonObject>()) {
    noteFailure(code, Fail::BadJson, "Antwort ist kein JSON-Objekt");
    return false;
  }
  noteSuccess();
  return true;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool tlsVerified() { return s_tls_verified; }

bool tlsVerifyEnabled() {
  loadPrefsOnce();
  return s_tls_verify_enabled;
}

void setTlsVerifyEnabled(bool on) {
  loadPrefsOnce();
  if (s_tls_verify_enabled == on) {
    return;
  }
  s_tls_verify_enabled = on;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsTlsKey, on);
    prefs.end();
  }
  s_tls_gave_up = false;  // an explicit choice clears an earlier auto-fallback
  s_tls_failures = 0;
  s_force_rebuild = true;
  Serial.printf("adsb: certificate verification %s\n", on ? "on" : "off");
}

const Health& health() { return s_health; }

unsigned long fetchIntervalMs() {
  const uint16_t fails = s_health.consecutive_failures;
  if (fails < config::kAdsbBackoffAfterFailures) {
    return config::kAdsbFetchIntervalMs;
  }
  // Each further step doubles the wait, up to the cap. Failing quietly every
  // half minute beats stalling the loop every two seconds.
  unsigned long interval = config::kAdsbBackoffBaseMs;
  for (uint16_t i = config::kAdsbBackoffAfterFailures; i < fails && i < 16; ++i) {
    interval *= 2;
    if (interval >= config::kAdsbBackoffMaxMs) {
      return config::kAdsbBackoffMaxMs;
    }
  }
  return interval;
}

void resetConnection() {
  s_force_rebuild = true;
  s_health.consecutive_failures = 0;  // retry at full speed after a manual reset
  Serial.println("adsb: connection reset requested");
}

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMillis() { return s_last_update_ms; }

void extrapolate(const Aircraft& ac, float elapsed_s, float* out_lat,
                 float* out_lon) {
  *out_lat = ac.lat;
  *out_lon = ac.lon;
  if (ac.gs_knots <= 0.0f || elapsed_s <= 0.0f) {
    return;
  }
  constexpr float kDegToRad = 0.01745329252f;
  constexpr float kKmPerDeg = 111.0f;
  // gs_knots = nautical miles per hour.
  const float dist_km = ac.gs_knots * kKmPerNm * (elapsed_s / 3600.0f);
  const float track_rad = ac.track_deg * kDegToRad;
  const float d_lat = (dist_km / kKmPerDeg) * cosf(track_rad);
  const float cos_lat = cosf(ac.lat * kDegToRad);
  const float d_lon = (cos_lat > 0.0001f)
                          ? (dist_km / (kKmPerDeg * cos_lat)) * sinf(track_rad)
                          : 0.0f;
  *out_lat = ac.lat + d_lat;
  *out_lon = ac.lon + d_lon;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km,
                 const char* match_key, bool* out_matched, float* out_lat,
                 float* out_lon) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  if (out_matched != nullptr) {
    *out_matched = false;
  }

  JsonDocument doc;
  if (!httpGetJson(url, doc)) {
    return false;
  }

  // Snapshot time for dead-reckoning between fetches.
  s_last_update_ms = millis();

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    return true;
  }

  const bool want_match =
      match_key != nullptr && match_key[0] != '\0' && out_matched != nullptr;

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    const float lat = plane["lat"].as<float>();
    const float lon = plane["lon"].as<float>();

    // Match the tracked target even if it is on the ground (just landed).
    if (want_match && !*out_matched && planeMatches(plane, match_key)) {
      *out_matched = true;
      if (out_lat != nullptr) {
        *out_lat = lat;
      }
      if (out_lon != nullptr) {
        *out_lon = lon;
      }
    }

    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = lat;
    s_aircraft[n].lon = lon;
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

namespace {

bool locateVia(const char* base, const char* target, float* out_lat,
               float* out_lon) {
  String url = base;
  url += target;
  JsonDocument doc;
  if (!httpGetJson(url, doc)) {
    return false;
  }
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    return false;
  }
  for (JsonObject plane : ac) {
    if (plane["lat"].is<float>() && plane["lon"].is<float>()) {
      *out_lat = plane["lat"].as<float>();
      *out_lon = plane["lon"].as<float>();
      return true;
    }
  }
  return false;
}

}  // namespace

bool locate(const char* target, float* out_lat, float* out_lon) {
  if (target == nullptr || target[0] == '\0') {
    return false;
  }
  if (locateVia(config::kAdsbCallsignUrl, target, out_lat, out_lon)) {
    return true;
  }
  return locateVia(config::kAdsbRegistrationUrl, target, out_lat, out_lon);
}

}  // namespace services::adsb
