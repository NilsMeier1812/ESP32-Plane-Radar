#pragma once

namespace services::config_server {

/**
 * Lightweight HTTP server for the companion config page (served on the LAN at
 * http://<device-ip>/ or http://plane-radar.local/). Lets a phone or laptop set
 * the radar center (manual coordinates, ICAO code, or phone GPS via the HTTPS
 * helper page) and the scale, applied live and persisted to NVS.
 *
 * Replaces the old WiFiManager LAN web portal during normal operation;
 * WiFiManager still handles Wi-Fi provisioning (the AP captive portal).
 */

/** Start the server + mDNS. Safe to call repeatedly (no-op if already running). */
void begin();

/** Stop the server + mDNS (e.g. when the Wi-Fi link drops). */
void stop();

/**
 * Restart the mDNS responder (leaving the HTTP server untouched). Called after
 * a reconnect: the responder is bound to the old netif state and stops
 * answering for plane-radar.local until it is set up again.
 */
void announce();

bool running();

/** Service pending HTTP clients; call every loop iteration while connected. */
void handle();

/**
 * Returns true once after the config changed via the web UI, so the caller can
 * force an immediate radar redraw. Self-clearing.
 */
bool consumeChanged();

}  // namespace services::config_server
