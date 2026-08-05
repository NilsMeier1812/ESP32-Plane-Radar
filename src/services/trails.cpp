#include "services/trails.h"

#include <Arduino.h>

#include <cmath>
#include <cstring>

namespace services::trails {

namespace {

/** Fewer slots than kMaxAircraft: trails only matter for what is on screen. */
constexpr size_t kMaxTracks = 32;
/** A slot not refreshed for this long is reused (aircraft left the area). */
constexpr unsigned long kStaleMs = 20000;
/** Ignore jitter: only record a point once the aircraft actually moved. */
constexpr float kMinStepDeg = 0.0005f;  // ~55 m

struct Track {
  char hex[8];
  uint8_t count;
  uint8_t head;  // next write position in the ring buffer
  float lat[kMaxPoints];
  float lon[kMaxPoints];
  unsigned long last_ms;
};

Track s_tracks[kMaxTracks];

Track* findTrack(const char* hex) {
  for (auto& t : s_tracks) {
    if (t.hex[0] != '\0' && strcmp(t.hex, hex) == 0) {
      return &t;
    }
  }
  return nullptr;
}

/** A free slot, the most stale one, or the oldest if all are still fresh. */
Track* claimTrack(const char* hex, unsigned long now) {
  Track* oldest = &s_tracks[0];
  for (auto& t : s_tracks) {
    if (t.hex[0] == '\0') {
      oldest = &t;
      break;
    }
    if (now - t.last_ms > now - oldest->last_ms) {
      oldest = &t;
    }
  }
  memset(oldest, 0, sizeof(*oldest));
  strncpy(oldest->hex, hex, sizeof(oldest->hex) - 1);
  return oldest;
}

void append(Track* t, float lat, float lon, unsigned long now) {
  if (t->count > 0) {
    const uint8_t last =
        static_cast<uint8_t>((t->head + kMaxPoints - 1) % kMaxPoints);
    if (fabsf(t->lat[last] - lat) < kMinStepDeg &&
        fabsf(t->lon[last] - lon) < kMinStepDeg) {
      t->last_ms = now;  // still here, just not moving enough to record
      return;
    }
  }
  t->lat[t->head] = lat;
  t->lon[t->head] = lon;
  t->head = static_cast<uint8_t>((t->head + 1) % kMaxPoints);
  if (t->count < kMaxPoints) {
    ++t->count;
  }
  t->last_ms = now;
}

}  // namespace

void onFetch(const Aircraft* list, size_t count) {
  const unsigned long now = millis();

  for (auto& t : s_tracks) {
    if (t.hex[0] != '\0' && now - t.last_ms > kStaleMs) {
      memset(&t, 0, sizeof(t));
    }
  }

  for (size_t i = 0; i < count; ++i) {
    if (list[i].hex[0] == '\0') {
      continue;  // no stable identity, no trail
    }
    Track* t = findTrack(list[i].hex);
    if (t == nullptr) {
      t = claimTrack(list[i].hex, now);
    }
    append(t, list[i].lat, list[i].lon, now);
  }
}

size_t pointsFor(const char* hex, float* out_lat, float* out_lon, size_t max) {
  if (hex == nullptr || hex[0] == '\0') {
    return 0;
  }
  const Track* t = findTrack(hex);
  if (t == nullptr || t->count == 0) {
    return 0;
  }

  const size_t n = (t->count < max) ? t->count : max;
  // Walk back n entries from the write head so the output is oldest first.
  size_t idx = (t->head + kMaxPoints - n) % kMaxPoints;
  for (size_t i = 0; i < n; ++i) {
    out_lat[i] = t->lat[idx];
    out_lon[i] = t->lon[idx];
    idx = (idx + 1) % kMaxPoints;
  }
  return n;
}

void clear() { memset(s_tracks, 0, sizeof(s_tracks)); }

}  // namespace services::trails
