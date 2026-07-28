#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();

/** Blank screen for standby (caller also sleeps the panel). */
void statusScreenStandby();
/** Two-line hint shown while the BOOT button is held (gesture feedback). */
void statusScreenHoldHint(const char* line1, const char* line2);

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
