#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdint>
#include <cstdio>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/config_server.h"
#include "services/radar_location.h"
#include "services/wifi_networks.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_standby_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootStandbyHoldMs) {
      s_boot_tap_pending = true;  // short tap → zoom
    } else if (held >= config::kBootStandbyHoldMs &&
               held < config::kBootResetWarnMs) {
      s_boot_standby_pending = true;  // medium hold → standby toggle
    }
    // held in [warn, reset): released during the reset warning → no action
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";
/** Sticky "this device has been through the setup portal once" marker. */
constexpr char kPrefsConfiguredKey[] = "cfgd";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;
bool s_events_registered = false;
/** Set by the GOT_IP event; makes wifiLoop re-announce mDNS after a reconnect. */
volatile bool s_link_up_pending = false;
uint8_t s_soft_reconnect_tries = 0;

void ensureWifiManager();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

/**
 * Remembered across reboots so a momentary read-back glitch of the STA config
 * can never drop a configured device onto the setup screen.
 */
void markConfigured() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  if (!prefs.getBool(kPrefsConfiguredKey, false)) {
    prefs.putBool(kPrefsConfiguredKey, true);
  }
  prefs.end();
}

bool everConfigured() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool configured = prefs.getBool(kPrefsConfiguredKey, false);
  prefs.end();
  return configured;
}

void clearConfigured() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.remove(kPrefsConfiguredKey);
  prefs.end();
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  services::config_server::stop();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  clearConfigured();
  services::wifi_networks::clear();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(static_cast<wifi_power_t>(config::kWifiTxPowerAp));
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

const char* disconnectReasonName(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
    case WIFI_REASON_AUTH_LEAVE: return "auth leave";
    case WIFI_REASON_ASSOC_EXPIRE: return "assoc expired";
    case WIFI_REASON_ASSOC_TOOMANY: return "AP full";
    case WIFI_REASON_NOT_AUTHED: return "not authed";
    case WIFI_REASON_NOT_ASSOCED: return "not assoced";
    case WIFI_REASON_ASSOC_LEAVE: return "assoc leave";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout (weak signal)";
    case WIFI_REASON_NO_AP_FOUND: return "AP not found";
    case WIFI_REASON_AUTH_FAIL: return "auth failed";
    case WIFI_REASON_ASSOC_FAIL: return "assoc failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout (wrong password?)";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    default: return "other";
  }
}

/**
 * Radio settings that must survive every (re)connect. Modem sleep in
 * particular is why the config page often looks offline: with power save on,
 * the ESP misses packets between beacons and both mDNS queries and HTTP
 * requests get dropped.
 */
void applyRadioSettings() {
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setTxPower(static_cast<wifi_power_t>(config::kWifiTxPowerSta));
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      applyRadioSettings();
      s_soft_reconnect_tries = 0;
      s_link_up_pending = true;
      Serial.printf("WiFi up: %s  IP %s  RSSI %d dBm\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println("WiFi: lost IP lease");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.printf("WiFi down: reason %u (%s)\n", reason,
                    disconnectReasonName(reason));
      break;
    }
    default:
      break;
  }
}

void ensureWifiEvents() {
  if (s_events_registered) {
    return;
  }
  WiFi.onEvent(onWifiEvent);
  s_events_registered = true;
}

void ensureWifiManager() {
  ensureWifiEvents();
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void prepareSta() {
  ensureWifiEvents();
  WiFi.persistent(true);  // keep credentials in NVS across reboots
  WiFi.mode(WIFI_STA);
  // Announce a DHCP hostname so the router can resolve "plane-radar" even on
  // networks where mDNS is filtered (many mesh/repeater setups).
  WiFi.setHostname(config::kPortalHostname);
  applyRadioSettings();
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      // Drop the association but keep the radio on: powering it down between
      // attempts loses the STA config and made a flaky AP look like "no
      // credentials", which is what pushed the device onto the setup screen.
      WiFi.disconnect(false);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

/**
 * Serial log + a short on-screen card with SSID, hostname and IP. The IP is
 * the fallback for networks where mDNS never resolves (mesh repeaters, guest
 * VLANs, Android clients), so it is worth showing every time we connect.
 */
void announceConnected() {
  const String ip = WiFi.localIP().toString();
  Serial.printf("Connected: %s  IP %s  RSSI %d dBm\n", WiFi.SSID().c_str(),
                ip.c_str(), WiFi.RSSI());
  statusScreenConnected(WiFi.SSID().c_str(), config::kPortalHostUrl, ip.c_str());
  const unsigned long until = millis() + config::kWifiConnectedInfoMs;
  while (millis() < until) {
    bootButtonPollLongPress();
    delay(20);
  }
}

/**
 * Fold the credentials the provisioning portal stored into the network list,
 * so everything downstream can work off a single list.
 */
void adoptPortalCredentials() {
  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0 || services::wifi_networks::contains(ssid.c_str())) {
    return;
  }
  services::wifi_networks::add(ssid.c_str(), s_wm.getWiFiPass().c_str());
}

/** One connect attempt against a single network; no internal retries. */
bool tryCandidateOnce(const services::wifi_networks::Network& net, bool show_ui,
                      unsigned long wait_ms) {
  Serial.printf("WiFi trying %s\n", net.ssid);
  if (show_ui) {
    statusScreenConnectingBegin(net.ssid);
  }
  WiFi.disconnect(false);
  delay(200);
  startStaConnect(net.ssid, net.pass);
  return waitForLinkWithUi(net.ssid, wait_ms);
}

/**
 * Order the saved networks by how strongly they are being heard right now, so
 * the device picks the network of the place it is actually in. Networks that
 * the scan did not see are kept as a fallback (hidden SSIDs never show up, and
 * a scan can simply miss an AP) but are tried last.
 *
 * Returns the number of entries written to `order`.
 */
size_t orderCandidatesByScan(uint8_t* order, size_t order_len) {
  const size_t saved = services::wifi_networks::count();
  int32_t rssi[services::wifi_networks::kMaxNetworks];
  for (size_t i = 0; i < saved; ++i) {
    rssi[i] = INT32_MIN;  // not seen
  }

  prepareSta();
  statusScreenSearchingBegin();
  const int found = WiFi.scanNetworks(false /*async*/, true /*show_hidden*/);
  Serial.printf("WiFi scan: %d networks in range\n", found);
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    for (size_t n = 0; n < saved; ++n) {
      if (ssid == services::wifi_networks::at(n).ssid &&
          WiFi.RSSI(i) > rssi[n]) {
        rssi[n] = WiFi.RSSI(i);
      }
    }
  }
  WiFi.scanDelete();

  size_t n_order = 0;
  for (size_t i = 0; i < saved && n_order < order_len; ++i) {
    order[n_order++] = static_cast<uint8_t>(i);
  }
  // Insertion sort by RSSI, strongest first; tiny list, stable enough.
  for (size_t i = 1; i < n_order; ++i) {
    const uint8_t key = order[i];
    size_t j = i;
    while (j > 0 && rssi[order[j - 1]] < rssi[key]) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = key;
  }

  for (size_t i = 0; i < n_order; ++i) {
    const int32_t r = rssi[order[i]];
    if (r == INT32_MIN) {
      Serial.printf("  %u. %s (not in range)\n", static_cast<unsigned>(i + 1),
                    services::wifi_networks::at(order[i]).ssid);
    } else {
      Serial.printf("  %u. %s (%ld dBm)\n", static_cast<unsigned>(i + 1),
                    services::wifi_networks::at(order[i]).ssid,
                    static_cast<long>(r));
    }
  }
  return n_order;
}

bool connectSavedNetwork(bool show_ui) {
  adoptPortalCredentials();

  const size_t saved = services::wifi_networks::count();
  if (saved == 0) {
    return false;
  }

  if (saved == 1) {
    const services::wifi_networks::Network& net = services::wifi_networks::at(0);
    return tryConnectWithUi(net.ssid, net.pass, show_ui);
  }

  uint8_t order[services::wifi_networks::kMaxNetworks];
  const size_t n_order = orderCandidatesByScan(order, sizeof(order));
  for (size_t i = 0; i < n_order; ++i) {
    if (tryCandidateOnce(services::wifi_networks::at(order[i]), show_ui,
                         config::kWifiCandidateAttemptMs)) {
      return true;
    }
  }
  return false;
}

bool openConfigPortal() {
  services::config_server::stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

bool bootButtonConsumeStandby() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool s = s_boot_standby_pending;
  if (s) {
    s_boot_standby_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return s;
}

unsigned long bootButtonHeldMs() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool down = s_boot_is_down;
  const unsigned long since = s_boot_down_ms;
  portEXIT_CRITICAL(&s_boot_mux);
  if (!down) {
    return 0;
  }
  const unsigned long now = millis();
  return (now >= since) ? (now - since) : 0;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  ensureWifiManager();

  // Tier 1 — soft: ask the supplicant to re-associate with the network it
  // already knows. Cheap, keeps the radar on screen, and handles the common
  // case (AP dropped us for a few seconds).
  if (s_soft_reconnect_tries < config::kWifiSoftReconnectTries) {
    ++s_soft_reconnect_tries;
    Serial.printf("WiFi soft reconnect %u/%u\n", s_soft_reconnect_tries,
                  config::kWifiSoftReconnectTries);
    prepareSta();
    WiFi.reconnect();
    const unsigned long deadline = millis() + config::kWifiSoftReconnectWaitMs;
    while (millis() < deadline) {
      if (wifiLinkUp()) {
        return true;
      }
      bootButtonPollLongPress();
      delay(20);
    }
    return wifiLinkUp();
  }

  // Tier 2 — full: re-begin with the saved credentials and show the connecting
  // screen. Still never opens the portal, so a long outage cannot strand the
  // device on the setup screen.
  Serial.println("WiFi full reconnect...");
  if (connectSavedNetwork(true)) {
    return true;
  }
  // Back to cheap soft tries: alternating keeps a long outage from turning into
  // a chain of 45-second blocking attempts.
  s_soft_reconnect_tries = 0;
  return false;
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!services::config_server::running()) {
      services::config_server::begin();  // announces mDNS itself
      s_link_up_pending = false;
      markConfigured();
    } else if (s_link_up_pending) {
      s_link_up_pending = false;
      markConfigured();
      // A reconnect invalidates the old mDNS responder: re-announce so
      // plane-radar.local keeps resolving without a reboot.
      services::config_server::announce();
    }
    bootButtonPollLongPress();
    services::config_server::handle();
  } else if (services::config_server::running()) {
    services::config_server::stop();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      markConfigured();
      adoptPortalCredentials();  // the freshly provisioned network joins the list
      announceConnected();
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    announceConnected();
    return true;
  }

  const bool configured = services::wifi_networks::count() > 0 ||
                          storedWifiCredentials() || everConfigured();

  if (configured && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    announceConnected();
    return true;
  }

  if (configured) {
    // Deliberately no portal here. A router that is slow to come up after a
    // power cut, or a moment of weak signal, must not throw the user back into
    // Wi-Fi setup — loop() keeps retrying the saved network, and the portal
    // stays reachable on demand by holding BOOT for 10 s.
    Serial.println("Saved WiFi not reachable — retrying in the background");
    WiFi.setAutoReconnect(true);  // let the supplicant keep trying on its own
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("No saved WiFi — opening setup portal");

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    markConfigured();
    adoptPortalCredentials();  // the freshly provisioned network joins the list
    announceConnected();
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
