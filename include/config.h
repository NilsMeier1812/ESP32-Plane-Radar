#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/**
 * HTTPS companion page that reads the phone GPS and hands the coordinates back
 * to the device (browsers only expose geolocation over HTTPS, which the device
 * itself cannot serve). Host docs/gps.html via GitHub Pages and point this URL
 * at it. The device passes ?device=<ip> so the page can redirect back.
 */
constexpr char kGpsHelperUrl[] =
    "https://nilsmeier1812.github.io/esp32-plane-radar/gps.html";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
// Gesture tiers, classified by how long BOOT is held:
//   tap  (< standby)         → cycle zoom range
//   hold (standby .. reset)  → toggle standby (screen off / activity paused)
//   hold (>= reset)          → Wi-Fi reset + reboot
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/** Hold at least this long (but less than reset) to toggle standby. */
constexpr unsigned long kBootStandbyHoldMs = 1500UL;
/** Show the "keep holding for Wi-Fi reset" hint from here. */
constexpr unsigned long kBootResetWarnMs = 8000UL;
/** Hold this long to wipe Wi-Fi credentials and reboot. */
constexpr unsigned long kBootResetHoldMs = 10000UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_2;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** How often to fetch fresh data from adsb.fi (public API limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 2000;
/**
 * Radar redraw interval (ms). Between fetches, aircraft are dead-reckoned from
 * their last position + ground speed + track, so movement stays smooth at this
 * rate without extra API calls. If a frame takes longer than this, redraws just
 * run as fast as they can.
 */
constexpr unsigned long kRadarRedrawIntervalMs = 100;
/**
 * Cap on how far ahead an aircraft is dead-reckoned when data goes stale (e.g.
 * fetch failures), so planes don't drift off indefinitely without a fresh fix.
 */
constexpr float kAircraftMaxExtrapolateSec = 15.0f;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- Aircraft tracking (follow a callsign / registration) ---
/** No fresh fix for the tracked aircraft this long → show "signal lost". */
constexpr unsigned long kTrackSignalLostMs = 6000UL;
/** Lost this long → recenter on the home location (tracking stays armed). */
constexpr unsigned long kTrackRevertHomeMs = 90000UL;
/** adsb.fi lookup endpoints for locating a target by callsign / registration. */
constexpr char kAdsbCallsignUrl[] = "https://opendata.adsb.fi/api/v2/callsign/";
constexpr char kAdsbRegistrationUrl[] =
    "https://opendata.adsb.fi/api/v2/registration/";

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
