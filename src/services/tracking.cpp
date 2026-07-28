#include "services/tracking.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cctype>
#include <cstring>

#include "config.h"
#include "services/radar_location.h"

namespace services::tracking {

namespace {

constexpr char kPrefsNamespace[] = "track";
constexpr char kKeyTarget[] = "target";
constexpr char kKeyActive[] = "active";
constexpr size_t kTargetLen = 12;

char s_target[kTargetLen + 1] = "";
char s_match[kTargetLen + 1] = "";  // upper-cased, spaces stripped
bool s_active = false;
bool s_has_fix = false;
double s_lat = 0.0;
double s_lon = 0.0;
unsigned long s_last_seen_ms = 0;

void makeMatchKey(const char* src) {
  size_t n = 0;
  for (const char* p = src; *p != '\0' && n < kTargetLen; ++p) {
    if (*p == ' ') {
      continue;
    }
    s_match[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
  }
  s_match[n] = '\0';
}

unsigned long sinceSeen() {
  const unsigned long now = millis();
  return (now >= s_last_seen_ms) ? (now - s_last_seen_ms) : 0;
}

void persist() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kKeyTarget, s_target);
  prefs.putBool(kKeyActive, s_active);
  prefs.end();
}

}  // namespace

void init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const String t = prefs.getString(kKeyTarget, "");
  const bool a = prefs.getBool(kKeyActive, false);
  prefs.end();
  strncpy(s_target, t.c_str(), kTargetLen);
  s_target[kTargetLen] = '\0';
  makeMatchKey(s_target);
  s_active = a && s_match[0] != '\0';
  s_has_fix = false;
  s_last_seen_ms = 0;
}

bool active() { return s_active; }

const char* target() { return s_target; }

State state() {
  if (!s_active) {
    return State::Off;
  }
  if (!s_has_fix) {
    return State::Searching;
  }
  return (sinceSeen() <= config::kTrackSignalLostMs) ? State::Tracking
                                                     : State::Lost;
}

void start(const char* t) {
  if (t == nullptr) {
    return;
  }
  strncpy(s_target, t, kTargetLen);
  s_target[kTargetLen] = '\0';
  makeMatchKey(s_target);
  s_active = s_match[0] != '\0';
  s_has_fix = false;
  s_last_seen_ms = 0;
  persist();
  Serial.printf("Tracking: %s\n", s_active ? s_target : "(invalid)");
}

void stop() {
  s_active = false;
  s_has_fix = false;
  persist();
  Serial.println("Tracking: off");
}

double centerLat() {
  if (s_active && s_has_fix && sinceSeen() <= config::kTrackRevertHomeMs) {
    return s_lat;
  }
  return services::location::lat();
}

double centerLon() {
  if (s_active && s_has_fix && sinceSeen() <= config::kTrackRevertHomeMs) {
    return s_lon;
  }
  return services::location::lon();
}

bool needsLocate() {
  return s_active && (!s_has_fix || sinceSeen() > config::kTrackSignalLostMs);
}

const char* matchKey() { return s_match; }

void onLocated(float lat, float lon) {
  s_lat = lat;
  s_lon = lon;
  s_has_fix = true;
  s_last_seen_ms = millis();
}

void onLocateFailed() { /* keep state; centerLat/Lon fall back after timeout */ }

void onAreaResult(bool found, float lat, float lon) {
  if (found) {
    s_lat = lat;
    s_lon = lon;
    s_has_fix = true;
    s_last_seen_ms = millis();
  }
}

}  // namespace services::tracking
