#include "services/clock_time.h"

#include <Arduino.h>

#include <ctime>
#include <cstdio>

#include "config.h"

namespace services::clock_time {

namespace {

bool s_started = false;
bool s_synced = false;
unsigned long s_last_check_ms = 0;
char s_iso[32] = "not synced";

/** Anything before this means SNTP has not answered yet. */
constexpr time_t kPlausibleEpoch = 1735689600;  // 2025-01-01

void refreshIso(time_t t) {
  struct tm tm_utc = {};
  gmtime_r(&t, &tm_utc);
  strftime(s_iso, sizeof(s_iso), "%Y-%m-%d %H:%M:%S UTC", &tm_utc);
}

}  // namespace

void begin() {
  if (s_started) {
    return;
  }
  // UTC only: certificate validity is absolute, and nothing here shows a local
  // time yet. A timezone belongs with the clock display, not with TLS.
  configTime(0, 0, config::kNtpServer1, config::kNtpServer2, config::kNtpServer3);
  s_started = true;
  Serial.println("NTP: sync requested");
}

bool synced() { return s_synced; }

void loop() {
  if (!s_started || s_synced) {
    return;
  }
  const unsigned long now = millis();
  if (now - s_last_check_ms < config::kNtpPollIntervalMs) {
    return;
  }
  s_last_check_ms = now;

  const time_t t = time(nullptr);
  if (t < kPlausibleEpoch) {
    return;
  }
  s_synced = true;
  refreshIso(t);
  Serial.printf("NTP: clock set to %s\n", s_iso);
}

const char* isoUtc() {
  if (s_synced) {
    refreshIso(time(nullptr));
  }
  return s_iso;
}

}  // namespace services::clock_time
