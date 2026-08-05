#pragma once

#include <cstddef>

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
