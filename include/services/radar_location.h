#pragma once

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/** Set center from validated numeric coordinates; persist to NVS. */
bool saveLatLon(double lat, double lon);

/**
 * Resolve an ICAO code (e.g. "EDQH") against the on-board airport dataset and,
 * if found, set the center to that airport and persist it. Case-insensitive.
 */
bool saveFromIcao(const char* icao);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
