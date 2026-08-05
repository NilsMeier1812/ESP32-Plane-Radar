#pragma once

#include <cstddef>

#include "services/adsb_client.h"

namespace services::trails {

/**
 * Short position history per aircraft, so the radar can draw a breadcrumb
 * trail behind each target.
 *
 * Kept out of services::adsb::Aircraft because that array is rebuilt from
 * scratch on every fetch — the history has to survive across fetches and is
 * therefore keyed by the ICAO address (Aircraft::hex).
 */

/** Trail points kept per aircraft (one per fetch, ~2 s apart). */
constexpr size_t kMaxPoints = 6;

/** Append the current positions; call once after each successful fetch. */
void onFetch(const services::adsb::Aircraft* list, size_t count);

/**
 * Copy the trail of `hex` into out_lat/out_lon, oldest first, and return how
 * many points were written (0 when unknown or trails are still filling up).
 */
size_t pointsFor(const char* hex, float* out_lat, float* out_lon, size_t max);

/** Drop all history (range change, tracking switch, trails toggled off). */
void clear();

}  // namespace services::trails
