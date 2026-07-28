#include "services/presence.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"

namespace services::presence {

namespace {

constexpr char kPrefsNamespace[] = "presence";
constexpr char kKeyEnabled[] = "enabled";

bool s_enabled = false;
bool s_hb_seen = false;
unsigned long s_last_hb_ms = 0;
unsigned long s_grace_start_ms = 0;
bool s_last_online = true;

bool computeOnline() {
  if (s_hb_seen) {
    return (millis() - s_last_hb_ms) < config::kPresenceTimeoutMs;
  }
  // No heartbeat yet: stay awake during the boot/enable grace window.
  return (millis() - s_grace_start_ms) < config::kPresenceBootGraceMs;
}

}  // namespace

void init() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    s_enabled = prefs.getBool(kKeyEnabled, false);
    prefs.end();
  }
  s_grace_start_ms = millis();
  s_last_online = true;
}

bool enabled() { return s_enabled; }

void setEnabled(bool on) {
  s_enabled = on;
  s_grace_start_ms = millis();  // restart the grace window
  s_last_online = true;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kKeyEnabled, on);
    prefs.end();
  }
  Serial.printf("PC presence auto-standby: %s\n", on ? "on" : "off");
}

void heartbeat() {
  s_last_hb_ms = millis();
  s_hb_seen = true;
}

Edge update() {
  if (!s_enabled) {
    s_last_online = true;
    return Edge::None;
  }
  const bool online = computeOnline();
  if (online == s_last_online) {
    return Edge::None;
  }
  s_last_online = online;
  return online ? Edge::WentOnline : Edge::WentOffline;
}

const char* stateStr() {
  if (!s_enabled) {
    return "off";
  }
  if (!s_hb_seen) {
    return (millis() - s_grace_start_ms < config::kPresenceBootGraceMs)
               ? "waiting"
               : "offline";
  }
  return ((millis() - s_last_hb_ms) < config::kPresenceTimeoutMs) ? "online"
                                                                  : "offline";
}

}  // namespace services::presence
