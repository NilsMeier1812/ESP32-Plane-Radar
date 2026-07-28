/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/config_server.h"
#include "services/radar_location.h"
#include "services/tracking.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
bool g_standby = false;
int g_hint_phase = -1;  // -1 none, 0 "release for standby", 1 "hold for reset"
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_draw_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void enterStandby() {
  statusScreenStandby();
  tft.sleep();  // panel low-power; activity (fetch/draw) is paused in loop()
  g_standby = true;
  g_radar_visible = false;
  Serial.println("Standby on");
}

void exitStandby() {
  tft.wakeup();
  g_standby = false;
  g_radar_visible = false;  // force a fresh radar draw next loop
  Serial.println("Standby off");
}

// Draw the hold-gesture hint on threshold crossings. Returns true while a hint
// is showing, so the caller pauses the normal radar redraw.
bool updateHoldHint() {
  if (g_standby) {
    g_hint_phase = -1;
    return false;
  }
  const unsigned long held = bootButtonHeldMs();
  int phase = -1;
  if (held >= config::kBootResetWarnMs) {
    phase = 1;
  } else if (held >= config::kBootStandbyHoldMs) {
    phase = 0;
  }
  if (phase != g_hint_phase) {
    g_hint_phase = phase;
    if (phase == 0) {
      statusScreenHoldHint("Loslassen:", "Standby");
    } else if (phase == 1) {
      statusScreenHoldHint("Weiter halten:", "WLAN-Reset");
    }
  }
  return phase >= 0;
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeStandby()) {
    if (g_standby) {
      exitStandby();
    } else {
      enterStandby();
    }
    g_hint_phase = -1;
    return;
  }
  if (!g_standby && bootButtonConsumeTap()) {
    onRangeTap();
  }
}

void fetchAircraft() {
  namespace tr = services::tracking;
  const float fetch_km = ui::radar::fetchRadiusKm();

  if (tr::active() && tr::needsLocate()) {
    // Acquiring or re-acquiring the target: global callsign/registration lookup.
    float la = 0.0f;
    float lo = 0.0f;
    if (services::adsb::locate(tr::matchKey(), &la, &lo)) {
      tr::onLocated(la, lo);
    } else {
      tr::onLocateFailed();
    }
  } else if (tr::active()) {
    // Following: area query centred on the target, matched in the results.
    bool matched = false;
    float la = 0.0f;
    float lo = 0.0f;
    services::adsb::fetchUpdate(tr::centerLat(), tr::centerLon(), fetch_km,
                                tr::matchKey(), &matched, &la, &lo);
    tr::onAreaResult(matched, la, lo);
  } else {
    services::adsb::fetchUpdate(tr::centerLat(), tr::centerLon(), fetch_km);
  }

  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  services::tracking::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  const bool hinting = updateHoldHint();
  wifiLoop();

  if (g_standby) {
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (!hinting) {
      // Center/scale/tracking changed via the web UI: refetch asap.
      if (services::config_server::consumeChanged()) {
        g_last_adsb_fetch_ms = 0;
      }
      const unsigned long now = millis();
      if (now - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
        g_last_adsb_fetch_ms = now;
        fetchAircraft();
      }
      // Redraw far more often than we fetch; aircraft are dead-reckoned between
      // fetches so movement looks smooth.
      if (!g_standby && now - g_last_draw_ms >= config::kRadarRedrawIntervalMs) {
        g_last_draw_ms = now;
        ui::radarDisplayRefreshAircraft();
      }
    }
  }

  delay(10);
}
