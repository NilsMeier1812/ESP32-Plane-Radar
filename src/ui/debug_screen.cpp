#include "ui/debug_screen.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/clock_time.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace uifonts = lgfx::v1::fonts;

namespace ui {

namespace {

/** Keep lines short: the panel is round, so the corners are not there. */
constexpr size_t kMaxLines = 10;
constexpr int kLineGap = 3;
constexpr float kBodyVlw = 0.78f;
constexpr auto& kBodyGfx = uifonts::FreeSans9pt7b;
constexpr auto& kTitleGfx = uifonts::FreeSansBold9pt7b;

char s_lines[kMaxLines][22];
size_t s_line_count = 0;
lgfx::LGFXBase* s_target = &tft;

void reset() { s_line_count = 0; }

void addLine(const char* fmt, ...) {
  if (s_line_count >= kMaxLines) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_lines[s_line_count], sizeof(s_lines[0]), fmt, args);
  va_end(args);
  ++s_line_count;
}

/** Uptime as the largest sensible unit — "143m" beats "8580s" when read aloud. */
void formatUptime(char* out, size_t len) {
  const unsigned long s = millis() / 1000UL;
  if (s < 120) {
    snprintf(out, len, "%lus", s);
  } else if (s < 7200) {
    snprintf(out, len, "%lum", s / 60UL);
  } else {
    snprintf(out, len, "%luh", s / 3600UL);
  }
}

void buildDataPage() {
  const services::adsb::Health& h = services::adsb::health();

  addLine("DEBUG 1/%u", static_cast<unsigned>(kDebugPageCount));
  addLine("E%u  HTTP %d", static_cast<unsigned>(h.last_fail), h.last_http_code);
  addLine("FEHL %u  OK %lu", static_cast<unsigned>(h.consecutive_failures),
          static_cast<unsigned long>(h.ok_count));
  addLine("PLANES %u", static_cast<unsigned>(services::adsb::aircraftCount()));

  if (h.last_ok_ms == 0) {
    addLine("ALTER  nie");
  } else {
    addLine("ALTER %lus", (millis() - h.last_ok_ms) / 1000UL);
  }
  addLine("TAKT %lus  T %lums",
          services::adsb::fetchIntervalMs() / 1000UL,
          static_cast<unsigned long>(h.last_duration_ms));
  addLine("HEAP %lu/%luk",
          static_cast<unsigned long>(ESP.getFreeHeap() / 1024UL),
          static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024UL));
  // The block size is what a handshake actually needs, so it earns its line.
  addLine("BLOCK %luk",
          static_cast<unsigned long>(
              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024UL));
  addLine("TLS %d  NTP %d", services::adsb::tlsVerified() ? 1 : 0,
          services::clock_time::synced() ? 1 : 0);
}

void buildSystemPage() {
  char uptime[8];
  formatUptime(uptime, sizeof(uptime));

  addLine("DEBUG 2/%u", static_cast<unsigned>(kDebugPageCount));
  addLine("WIFI %d  RSSI %d", WiFi.status() == WL_CONNECTED ? 1 : 0,
          static_cast<int>(WiFi.RSSI()));

  // The IP is the fallback route to the config page when mDNS will not resolve.
  const String ip = WiFi.localIP().toString();
  addLine("%s", ip.c_str());

  char ssid[18];
  strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  addLine("%s", ssid);

  addLine("UP %s  RST %d", uptime, static_cast<int>(esp_reset_reason()));
  addLine("DROPS %lu", static_cast<unsigned long>(wifiDisconnectCount()));
  addLine("HEAP %lu/%luk",
          static_cast<unsigned long>(ESP.getFreeHeap() / 1024UL),
          static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024UL));
  addLine("ZOOM %u  ALT %d-%d", static_cast<unsigned>(radar::rangeIndex()),
          radar::altMinFt() / 100, radar::altMaxFt() / 100);
  addLine("%s", config::kFirmwareVersion);
}

void applyStyle(bool title) {
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(*s_target, kBodyVlw);
  } else {
    displayFontSetBitmap(*s_target, title ? &kTitleGfx : &kBodyGfx);
  }
}

}  // namespace

void debugScreenDraw(uint8_t page) {
  reset();
  if (page == 0) {
    buildDataPage();
  } else {
    buildSystemPage();
  }

  LGFX_Sprite* sprite = radarFrameSprite();
  s_target = (sprite != nullptr) ? static_cast<lgfx::LGFXBase*>(sprite)
                                 : static_cast<lgfx::LGFXBase*>(&tft);
  displayFontEnsureLoaded(*s_target);

  s_target->fillScreen(config::kColorBlack);
  s_target->setTextDatum(textdatum_t::middle_center);

  applyStyle(false);
  const int line_h = s_target->fontHeight();
  const int total_h =
      static_cast<int>(s_line_count) * line_h +
      (static_cast<int>(s_line_count) - 1) * kLineGap;
  int y = (config::kDisplayHeight - total_h) / 2;

  for (size_t i = 0; i < s_line_count; ++i) {
    applyStyle(i == 0);
    s_target->setTextColor(
        i == 0 ? radar::kColorRunwayLabel : config::kTextOnBlack,
        config::kColorBlack);
    s_target->drawString(s_lines[i], config::kDisplayWidth / 2, y + line_h / 2);
    y += line_h + kLineGap;
  }

  if (sprite != nullptr) {
    sprite->pushSprite(0, 0);
  }
  tft.setTextDatum(textdatum_t::top_left);
  s_target = &tft;
}

void debugScreenLog() {
  const services::adsb::Health& h = services::adsb::health();
  Serial.println("---- debug ----");
  Serial.printf("fail=%u http=%d err=\"%s\" fails=%u ok=%lu bad=%lu\n",
                static_cast<unsigned>(h.last_fail), h.last_http_code,
                h.last_error, static_cast<unsigned>(h.consecutive_failures),
                static_cast<unsigned long>(h.ok_count),
                static_cast<unsigned long>(h.fail_count));
  Serial.printf("aircraft=%u age=%lums interval=%lums last=%lums heap_before=%lu\n",
                static_cast<unsigned>(services::adsb::aircraftCount()),
                h.last_ok_ms == 0 ? 0UL : (millis() - h.last_ok_ms),
                services::adsb::fetchIntervalMs(),
                static_cast<unsigned long>(h.last_duration_ms),
                static_cast<unsigned long>(h.heap_before_last));
  Serial.printf("block=%lu\n", static_cast<unsigned long>(
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  Serial.printf("heap=%lu min=%lu wifi=%d rssi=%d ip=%s ssid=%s\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()),
                WiFi.status() == WL_CONNECTED ? 1 : 0,
                static_cast<int>(WiFi.RSSI()),
                WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
  Serial.printf("tls=%d ntp=%d time=%s reset=%d uptime=%lus version=%s\n",
                services::adsb::tlsVerified() ? 1 : 0,
                services::clock_time::synced() ? 1 : 0,
                services::clock_time::isoUtc(),
                static_cast<int>(esp_reset_reason()), millis() / 1000UL,
                config::kFirmwareVersion);
  Serial.println("---------------");
}

}  // namespace ui
