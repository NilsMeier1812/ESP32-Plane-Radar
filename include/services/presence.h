#pragma once

namespace services::presence {

/** Auto-standby driven by heartbeats from the PC: while the PC keeps checking
 *  in the radar stays awake; when the heartbeats stop it drops to standby. */

enum class Edge { None, WentOnline, WentOffline };

/** Load the enabled flag. Call once after boot. */
void init();

bool enabled();
void setEnabled(bool on);

/** Record a heartbeat (called from the /api/presence endpoint). */
void heartbeat();

/** Edge in the online/offline state since the previous call. */
Edge update();

/** "off" (disabled) / "waiting" (grace, no heartbeat yet) / "online" / "offline". */
const char* stateStr();

}  // namespace services::presence
