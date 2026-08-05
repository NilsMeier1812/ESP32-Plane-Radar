#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();

/** Blank screen for standby (caller also sleeps the panel). */
void statusScreenStandby();
/** Two-line hint shown while the BOOT button is held (gesture feedback). */
void statusScreenHoldHint(const char* line1, const char* line2);

/**
 * Shown briefly after a successful connect: SSID plus both addresses of the
 * config page, so the IP is known even when mDNS (plane-radar.local) fails.
 */
void statusScreenConnected(const char* ssid, const char* host, const char* ip);

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
/** Same animation, shown while scanning for which saved network is in range. */
void statusScreenSearchingBegin();
void statusScreenConnectingTick();
