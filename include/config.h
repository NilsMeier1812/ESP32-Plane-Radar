#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>

namespace config {

/**
 * Shown on the config page and in /api/state so a flashed device can be told
 * apart from another. CI stamps the git tag in via
 * PLATFORMIO_BUILD_FLAGS='-DPLANE_RADAR_VERSION=\"v1.2.3\"'; local builds
 * report "dev".
 */
#ifndef PLANE_RADAR_VERSION
#define PLANE_RADAR_VERSION "dev"
#endif
constexpr char kFirmwareVersion[] = PLANE_RADAR_VERSION;

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
    "https://nilsmeier1812.github.io/ESP32-Plane-Radar/gps.html";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
/**
 * Per-network wait when several networks are saved: each is tried once per
 * pass, ordered by signal strength, so a place whose network is out of range
 * is skipped quickly instead of eating the whole connect budget.
 */
constexpr unsigned long kWifiCandidateAttemptMs = 9000;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids churn on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 2000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 3000;
/**
 * Soft reconnects (WiFi.reconnect(), radio state kept, no screen change) tried
 * before falling back to a full re-begin with the "Connecting" screen. Most
 * drops are the AP kicking us off for a moment and recover on the first try.
 */
constexpr uint8_t kWifiSoftReconnectTries = 5;
constexpr unsigned long kWifiSoftReconnectWaitMs = 5000;
/** How long the connect info (SSID / hostname / IP) stays up after connecting. */
constexpr unsigned long kWifiConnectedInfoMs = 2500;

// --- Wi-Fi radio ---
/**
 * TX power in the wifi_power_t scale (quarter dBm): 34 = 8.5 dBm,
 * 52 = 13 dBm, 60 = 15 dBm, 78 = 19.5 dBm (max).
 *
 * The ESP32-C3 SuperMini has a weak on-board antenna, so the previous 8.5 dBm
 * setting only holds a link close to the access point — the main cause of
 * random drops and of the web page appearing offline. 15 dBm gives far more
 * headroom while staying below the peak current that makes marginal USB
 * supplies brown out. If the board resets while connecting, step this back
 * down (52, then 34).
 */
constexpr int8_t kWifiTxPowerSta = 60;
/** The setup portal is used at arm's length — keep the gentle setting there. */
constexpr int8_t kWifiTxPowerAp = 34;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
// Gesture tiers, classified by how long BOOT is held:
//   tap  (< standby)         → cycle zoom range
//   hold (standby .. reset)  → toggle standby (screen off / activity paused)
//   hold (>= reset)          → Wi-Fi reset + reboot
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/**
 * Two taps within this window open (or leave) the on-device debug screen.
 * A single tap is therefore held back this long before it cycles the zoom.
 */
constexpr unsigned long kBootDoubleTapMs = 400UL;
/** Debug screen refresh; slow enough to read, fast enough to look live. */
constexpr unsigned long kDebugRefreshMs = 500UL;
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

/**
 * Colour depth of the off-screen frame buffer.
 *
 * This is the single biggest RAM decision in the firmware. At 16 bpp the
 * buffer is 240*240*2 = 112 kB, roughly 60% of the heap an ESP32-C3 has left
 * once the Wi-Fi stack is up — which leaves too little for a TLS handshake
 * (40-50 kB contiguous), the web server and JSON parsing all at once. That is
 * not a feature too many; it is one buffer too large.
 *
 * At 8 bpp it is 56 kB and everything else fits comfortably. Colours go
 * through RGB332, so the palette quantises and anti-aliased edges band a
 * little; on a radar of solid greens, reds and white labels that is barely
 * visible, and it buys a device that actually keeps its connection.
 *
 * Set back to 16 only on a board with PSRAM.
 */
constexpr uint8_t kRadarFrameColorDepth = 8;

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
/**
 * No successful fetch for this long → show the struck-through Wi-Fi badge with
 * the age of the data. Longer than a couple of fetch intervals so a single
 * hiccup stays invisible.
 */
constexpr unsigned long kNoDataWarnAfterMs = 15000;

// --- Auto zoom (opt-in; picks the range preset from how busy the sky is) ---
/** Empty radar for this long → widen the range one step. */
constexpr unsigned long kAutoZoomOutAfterMs = 12000;
/** Crowded for this long → tighten the range one step. */
constexpr unsigned long kAutoZoomInAfterMs = 20000;
/** "Crowded" means at least this many aircraft inside the outer ring. */
constexpr size_t kAutoZoomInThreshold = 5;
/** Only zoom in while at least this many aircraft would remain visible. */
constexpr size_t kAutoZoomKeepVisible = 2;

/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- Time (NTP) ---
/** Certificate validity is a date comparison, so TLS needs a real clock. */
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.cloudflare.com";
constexpr char kNtpServer3[] = "time.google.com";
/** How often to check whether SNTP has answered yet. */
constexpr unsigned long kNtpPollIntervalMs = 1000;

// --- TLS ---
/**
 * Verify the adsb.fi certificate against the root CA bundle built into the
 * ESP-IDF image, once the clock is set. Before NTP syncs there is no way to
 * judge validity dates, so the first fetches stay unverified.
 *
 * If verification keeps failing (a root rotation the firmware predates), the
 * client falls back to unverified after kAdsbTlsFailuresBeforeFallback tries
 * and says so on serial and in /api/state — a radar that stops showing
 * aircraft would be a worse outcome than an unauthenticated public data feed.
 */
/**
 * Off by default on this board. Verification is the right thing to want, but
 * on a C3 that already spends 115 kB on the frame buffer it needs more heap
 * during the handshake than is reliably there, and a radar that shows nothing
 * is worse than an unauthenticated public data feed. Turn it on from the
 * config page once the heap readings show room (the setting is persisted).
 */
constexpr bool kAdsbVerifyTls = false;
constexpr uint8_t kAdsbTlsFailuresBeforeFallback = 3;

// --- ADS-B fetch resilience ---
/** Identifies the device in adsb.fi's logs; some APIs refuse an empty one. */
constexpr char kAdsbUserAgent[] = "ESP32-Plane-Radar";
/**
 * A connect blocks the main loop — the config page included — so a dead API
 * must not be retried every couple of seconds. After this many consecutive
 * failures the interval backs off, doubling per further failure up to the cap.
 */
constexpr uint16_t kAdsbBackoffAfterFailures = 3;
constexpr unsigned long kAdsbBackoffBaseMs = 8000;
constexpr unsigned long kAdsbBackoffMaxMs = 60000;
/** Throw the pooled connection away every N failures: it may be unusable. */
constexpr uint16_t kAdsbRebuildEveryNFailures = 4;
/**
 * Rebuild the pooled connection at this age even while it works. A keep-alive
 * socket the peer quietly dropped otherwise stays broken until a reboot.
 */
constexpr unsigned long kAdsbConnectionMaxAgeMs = 300000;  // 5 min
/**
 * Minimum largest *contiguous* free block for a fetch. mbedTLS wants one big
 * run for its record buffers, so total free heap is the wrong measure. Below
 * this the attempt would stall for seconds and fail anyway.
 */
constexpr uint32_t kAdsbMinHeapForFetch = 22000;


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
