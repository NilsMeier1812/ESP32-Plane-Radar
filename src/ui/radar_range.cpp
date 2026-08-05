#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsAltMinKey[] = "altMin";
constexpr char kPrefsAltMaxKey[] = "altMax";
constexpr char kPrefsTrailsKey[] = "trails";
constexpr char kPrefsAutoZoomKey[] = "autoZoom";
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
int s_alt_min_ft = 0;  // 0 = no bound
int s_alt_max_ft = 0;  // 0 = no bound
bool s_show_trails = false;
bool s_auto_zoom = false;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveOption(const char* key, bool value) {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(key, value);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_alt_min_ft = s_prefs.getInt(kPrefsAltMinKey, 0);
  s_alt_max_ft = s_prefs.getInt(kPrefsAltMaxKey, 0);
  s_show_trails = s_prefs.getBool(kPrefsTrailsKey, false);
  s_auto_zoom = s_prefs.getBool(kPrefsAutoZoomKey, false);
  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

void rangeSetIndex(uint8_t index) {
  if (index >= kRangePresetCount) {
    index = static_cast<uint8_t>(kRangePresetCount - 1);
  }
  if (index == s_range_index) {
    return;
  }
  s_range_index = index;
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

int altMinFt() { return s_alt_min_ft; }

int altMaxFt() { return s_alt_max_ft; }

void setAltFilter(int min_ft, int max_ft) {
  if (min_ft < 0) {
    min_ft = 0;
  }
  if (max_ft < 0) {
    max_ft = 0;
  }
  // A window that excludes everything is almost certainly a typo; drop the cap.
  if (min_ft > 0 && max_ft > 0 && max_ft < min_ft) {
    max_ft = 0;
  }
  s_alt_min_ft = min_ft;
  s_alt_max_ft = max_ft;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putInt(kPrefsAltMinKey, s_alt_min_ft);
    s_prefs.putInt(kPrefsAltMaxKey, s_alt_max_ft);
    s_prefs.end();
  }
  Serial.printf("Altitude filter: %d..%d ft (0 = off)\n", s_alt_min_ft,
                s_alt_max_ft);
}

bool altitudePasses(float alt_ft, bool alt_known) {
  if (s_alt_min_ft == 0 && s_alt_max_ft == 0) {
    return true;
  }
  if (!alt_known) {
    return true;  // never hide traffic just because it reports no altitude
  }
  if (s_alt_min_ft > 0 && alt_ft < static_cast<float>(s_alt_min_ft)) {
    return false;
  }
  if (s_alt_max_ft > 0 && alt_ft > static_cast<float>(s_alt_max_ft)) {
    return false;
  }
  return true;
}

bool showTrails() { return s_show_trails; }

void setShowTrails(bool on) {
  s_show_trails = on;
  saveOption(kPrefsTrailsKey, on);
  Serial.printf("Trails: %s\n", on ? "on" : "off");
}

bool autoZoom() { return s_auto_zoom; }

void setAutoZoom(bool on) {
  s_auto_zoom = on;
  saveOption(kPrefsAutoZoomKey, on);
  Serial.printf("Auto zoom: %s\n", on ? "on" : "off");
}

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = false;
  s_show_runways = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
