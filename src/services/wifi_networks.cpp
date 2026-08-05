#include "services/wifi_networks.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

namespace services::wifi_networks {

namespace {

/** Own namespace: the "wifi" prefs are opened elsewhere for the portal flags. */
constexpr char kPrefsNamespace[] = "wifinets";
constexpr char kCountKey[] = "n";

Network s_networks[kMaxNetworks];
size_t s_count = 0;

/** Key names stay short — NVS caps them at 15 characters. */
void ssidKey(size_t index, char* out, size_t out_len) {
  snprintf(out, out_len, "s%u", static_cast<unsigned>(index));
}

void passKey(size_t index, char* out, size_t out_len) {
  snprintf(out, out_len, "p%u", static_cast<unsigned>(index));
}

void copyField(char* dst, size_t dst_len, const char* src) {
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

/** Rewrites the whole list; small enough that a full rewrite beats bookkeeping. */
void save() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("wifi networks: NVS unavailable, not saved");
    return;
  }
  prefs.clear();
  prefs.putUChar(kCountKey, static_cast<uint8_t>(s_count));
  for (size_t i = 0; i < s_count; ++i) {
    char key[8];
    ssidKey(i, key, sizeof(key));
    prefs.putString(key, s_networks[i].ssid);
    passKey(i, key, sizeof(key));
    prefs.putString(key, s_networks[i].pass);
  }
  prefs.end();
}

int indexOf(const char* ssid) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < s_count; ++i) {
    if (strcmp(s_networks[i].ssid, ssid) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace

void init() {
  s_count = 0;
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const size_t stored = prefs.getUChar(kCountKey, 0);
  for (size_t i = 0; i < stored && s_count < kMaxNetworks; ++i) {
    char key[8];
    ssidKey(i, key, sizeof(key));
    const String ssid = prefs.getString(key, "");
    if (ssid.length() == 0) {
      continue;
    }
    passKey(i, key, sizeof(key));
    const String pass = prefs.getString(key, "");
    copyField(s_networks[s_count].ssid, kSsidLen, ssid.c_str());
    copyField(s_networks[s_count].pass, kPassLen, pass.c_str());
    ++s_count;
  }
  prefs.end();
  Serial.printf("wifi networks: %u saved\n", static_cast<unsigned>(s_count));
}

size_t count() { return s_count; }

const Network& at(size_t index) { return s_networks[index]; }

bool contains(const char* ssid) { return indexOf(ssid) >= 0; }

bool add(const char* ssid, const char* pass) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }

  const int existing = indexOf(ssid);
  if (existing >= 0) {
    copyField(s_networks[existing].pass, kPassLen, pass);
    save();
    Serial.printf("wifi networks: updated %s\n", ssid);
    return true;
  }

  if (s_count >= kMaxNetworks) {
    Serial.println("wifi networks: list full");
    return false;
  }

  copyField(s_networks[s_count].ssid, kSsidLen, ssid);
  copyField(s_networks[s_count].pass, kPassLen, pass);
  ++s_count;
  save();
  Serial.printf("wifi networks: added %s (%u total)\n", ssid,
                static_cast<unsigned>(s_count));
  return true;
}

bool remove(const char* ssid) {
  const int index = indexOf(ssid);
  if (index < 0) {
    return false;
  }
  for (size_t i = static_cast<size_t>(index); i + 1 < s_count; ++i) {
    s_networks[i] = s_networks[i + 1];
  }
  --s_count;
  save();
  Serial.printf("wifi networks: removed %s\n", ssid);
  return true;
}

void clear() {
  s_count = 0;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

}  // namespace services::wifi_networks
