#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

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

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  int code = HTTPC_ERROR_CONNECTION_REFUSED;
  for (uint8_t attempt = 0; attempt < kConnectAttempts; ++attempt) {
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

bool readResponseBodyWithPoll(HTTPClient& http, String& payload) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  uint8_t buffer[512];
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

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
void ensureTlsMode() {
  const bool want_verify = config::kAdsbVerifyTls && !s_tls_gave_up &&
                           services::clock_time::synced();
  if (s_http_ready && want_verify == s_tls_verified) {
    return;
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
  Serial.println(
      "adsb: certificate verification failed repeatedly — continuing "
      "unverified (check the firmware's root CA bundle)");
}

bool httpGetJson(const String& url, JsonDocument& doc) {
  ensureTlsMode();
  if (!s_http.begin(s_client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }
  s_http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(s_http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    s_http.end();
    if (code < 0) {
      noteTlsFailure();  // transport-level failure: a handshake reject looks like this
    }
    return false;
  }
  s_tls_failures = 0;
  String payload;
  if (!readResponseBodyWithPoll(s_http, payload)) {
    Serial.println("adsb: empty response");
    s_http.end();
    return false;
  }
  s_http.end();  // with setReuse(true) this keeps the socket open for next time

  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }
  return true;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool tlsVerified() { return s_tls_verified; }

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
