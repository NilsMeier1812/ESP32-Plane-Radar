#pragma once

#include <cstdint>

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/**
 * How often the link has dropped since boot. A screen that blinks every few
 * seconds looks the same whether the Wi-Fi is flapping or the radar is
 * redrawing badly; this number separates the two.
 */
uint32_t wifiDisconnectCount();

/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Latched medium hold (standby toggle). */
bool bootButtonConsumeStandby();
/** How long BOOT has currently been held (0 when released). */
unsigned long bootButtonHeldMs();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();
