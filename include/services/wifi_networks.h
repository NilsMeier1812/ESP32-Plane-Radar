#pragma once

#include <cstddef>

namespace services::wifi_networks {

/**
 * The saved Wi-Fi networks, persisted to NVS.
 *
 * The device keeps a small list instead of the single set of credentials the
 * Wi-Fi provisioning portal stores, so it can be carried between places (home,
 * office, holiday flat) and pick whichever network is actually in range. The
 * portal credentials are folded into this list on first use.
 *
 * Passwords are stored so the device can reconnect unattended; they are never
 * handed back out over the config API.
 */

constexpr size_t kMaxNetworks = 5;
constexpr size_t kSsidLen = 33;  // 32 chars + NUL
constexpr size_t kPassLen = 65;  // 64 chars + NUL

struct Network {
  char ssid[kSsidLen];
  char pass[kPassLen];
};

/** Load the list from NVS; call once during setup(). */
void init();

size_t count();
const Network& at(size_t index);

/** True if the SSID is already in the list (case-sensitive, like the radio). */
bool contains(const char* ssid);

/**
 * Add a network, or update the password of one already stored. Returns false
 * for an empty SSID or when the list is full (kMaxNetworks).
 */
bool add(const char* ssid, const char* pass);

/** Remove by SSID. Returns false when the SSID is not in the list. */
bool remove(const char* ssid);

/** Forget every network (part of the BOOT-hold Wi-Fi reset). */
void clear();

}  // namespace services::wifi_networks
