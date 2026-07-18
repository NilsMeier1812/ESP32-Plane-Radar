#include "services/radar_location.h"

#include <Preferences.h>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "data/large_airports.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void persist(double lat, double lon) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  prefs.end();
  s_lat = lat;
  s_lon = lon;
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  prefs.end();
}

double lat() { return s_lat; }

double lon() { return s_lon; }

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  return saveLatLon(lat, lon);
}

bool saveLatLon(double lat, double lon) {
  if (!validLatLon(lat, lon)) {
    return false;
  }
  persist(lat, lon);
  Serial.printf("Radar location saved: %.6f, %.6f\n", lat, lon);
  return true;
}

bool saveFromIcao(const char* icao) {
  if (icao == nullptr) {
    return false;
  }
  // Normalise to a 4-char upper-case ICAO ident.
  char key[5];
  size_t n = 0;
  for (const char* p = icao; *p != '\0' && n < 4; ++p) {
    if (*p == ' ') {
      continue;
    }
    key[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
  }
  key[n] = '\0';
  if (n != 4) {
    return false;
  }

  // kAirports is sorted by ident, so binary-search it.
  const auto* airports = data::large_airports::kAirports;
  size_t lo = 0;
  size_t hi = data::large_airports::kAirportCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = std::strcmp(airports[mid].ident, key);
    if (cmp == 0) {
      const double lat = static_cast<double>(airports[mid].lat_e7) * 1e-7;
      const double lon = static_cast<double>(airports[mid].lon_e7) * 1e-7;
      persist(lat, lon);
      Serial.printf("Radar location set to %s: %.6f, %.6f\n", key, lat, lon);
      return true;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  Serial.printf("ICAO %s not found in on-board dataset\n", key);
  return false;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
}

}  // namespace services::location
