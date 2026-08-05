#pragma once

namespace services::clock_time {

/**
 * Wall-clock time from NTP.
 *
 * Right now this exists for one reason: TLS certificate validation needs to
 * know the date, otherwise every certificate looks "not yet valid" and the
 * only option is to skip verification. The API is deliberately a bit broader
 * than that so a clock display can use it later.
 */

/** Kick off SNTP (non-blocking). Call once the Wi-Fi link is up. */
void begin();

/** True once the system clock holds a plausible date (not the 1970 default). */
bool synced();

/** Poll for a first sync; cheap, safe to call every loop iteration. */
void loop();

/** "2026-08-05 09:12:44 UTC", or "not synced". For diagnostics. */
const char* isoUtc();

}  // namespace services::clock_time
