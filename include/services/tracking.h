#pragma once

namespace services::tracking {

/** Follow a specific aircraft (by callsign / registration): the radar center
 *  moves to that aircraft's position each update. */

enum class State { Off, Searching, Tracking, Lost };

/** Load persisted target/active state. Call once after boot. */
void init();

bool active();
State state();
/** Human-readable target (callsign / registration), "" when none. */
const char* target();

/** Enable tracking of `t` (callsign or registration) and persist it. */
void start(const char* t);
/** Disable tracking and persist. */
void stop();

/** Effective radar center: the tracked aircraft when we have a recent fix,
 *  otherwise the configured home location. */
double centerLat();
double centerLon();

// --- Fetch integration (driven from the main loop) ---

/** True when we should do a global callsign/registration lookup this cycle
 *  (i.e. acquiring, or the target has been lost for a while). */
bool needsLocate();
/** Upper-cased target, for matching against area-query results. */
const char* matchKey();
/** A global lookup found the target at this position. */
void onLocated(float lat, float lon);
/** A global lookup found nothing. */
void onLocateFailed();
/** Result of an area query: whether the target was among the results. */
void onAreaResult(bool found, float lat, float lon);

}  // namespace services::tracking
