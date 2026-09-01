#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  /** Barometric (or geometric) altitude in feet; only valid if alt_known. */
  float alt_ft;
  bool alt_known;
  char callsign[9];
  char type[5];
  char alt[12];
  /** ICAO 24-bit address, lower case. Stable identity across fetches. */
  char hex[8];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/**
 * True while requests run against a verified certificate chain. False before
 * the clock is set, and after a fallback to unverified.
 */
bool tlsVerified();

/** Runtime switch for certificate verification (persisted; default on). */
bool tlsVerifyEnabled();
void setTlsVerifyEnabled(bool on);

/**
 * Why the radar is (or is not) getting data. Everything needed to tell a rate
 * limit (HTTP 429) from a refused handshake (negative code) from a device that
 * has run out of heap — none of which look any different on the display.
 */
/**
 * Compact reason for the last failure. Numeric so the device can show it on
 * the 240x240 display, where a sentence does not fit: read the number off the
 * debug screen and it says exactly which of these happened.
 */
enum class Fail : uint8_t {
  None = 0,          // last fetch succeeded
  NoConnection = 1,  // transport failed: network, DNS, or rejected handshake
  RateLimit = 2,     // HTTP 429 — the API is throttling us
  HttpError = 3,     // any other non-200 status
  EmptyBody = 4,     // connected, status 200, but no payload arrived
  LowHeap = 5,       // not enough free heap to attempt a TLS handshake
  BeginFailed = 6,   // HTTPClient could not even parse/open the URL
  BadJson = 7,       // payload arrived but did not parse
};

struct Health {
  /** Last code from the API: >0 is an HTTP status, <0 an HTTPClient error. */
  int last_http_code;
  /** Numeric failure reason, mirrored by last_error as text. */
  Fail last_fail;
  /** Consecutive failed fetches; drives the retry backoff. */
  uint16_t consecutive_failures;
  uint32_t ok_count;
  uint32_t fail_count;
  /** millis() of the last successful fetch and the last attempt. */
  unsigned long last_ok_ms;
  unsigned long last_attempt_ms;
  /** How long the last attempt took (ms) — a blocking connect shows up here. */
  uint32_t last_duration_ms;
  /** Free heap right before the last attempt, for spotting exhaustion. */
  uint32_t heap_before_last;
  char last_error[48];
};

const Health& health();

/**
 * How long to wait before the next fetch. Normally the configured interval;
 * after repeated failures it backs off, which keeps a dead API from blocking
 * the loop (and the config page) every couple of seconds.
 */
unsigned long fetchIntervalMs();

/** Drop the pooled TLS connection so the next request builds a fresh one. */
void resetConnection();

/** millis() timestamp of the last successful fetch (0 if none yet). */
unsigned long lastUpdateMillis();

/**
 * Dead-reckon `ac` forward `elapsed_s` seconds from its last reported fix using
 * ground speed + track, writing the extrapolated position to out_lat/out_lon.
 * Falls back to the reported position when speed/time is zero.
 */
void extrapolate(const Aircraft& ac, float elapsed_s, float* out_lat,
                 float* out_lon);

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/**
 * Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi.
 * If match_key is set, scans the results for an aircraft whose callsign or
 * registration equals it (upper-case), reporting via out_matched/out_lat/out_lon.
 */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km,
                 const char* match_key = nullptr, bool* out_matched = nullptr,
                 float* out_lat = nullptr, float* out_lon = nullptr);

/**
 * Locate an aircraft globally by callsign or registration (tries both).
 * On success writes its position and returns true.
 */
bool locate(const char* target, float* out_lat, float* out_lon);

}  // namespace services::adsb
