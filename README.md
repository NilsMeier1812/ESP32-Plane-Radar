# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~5 s).

## Controls (BOOT, GPIO 9, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash |
| **Hold ~1.5 s** | Toggle **standby** (screen off + polling paused; hold again to wake) |
| **Hold ~10 s** | Clear Wi‑Fi, location, and units; reboot into setup portal |

While holding, the screen shows which action will trigger on release. Standby is a low-power pause — the ESP32 keeps running but the panel sleeps and ADS‑B polling stops, so you can "switch it off" without pulling the cable. (The display has no separate backlight pin, so the backlight itself may stay dimly lit.)

During setup you can also hold BOOT at power-on to force a credential reset.

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). The same name is also sent as the DHCP hostname, so many routers resolve `plane-radar` even when mDNS is filtered. Some clients resolve `.local` slowly; the device shows its **IP address** for a moment right after connecting — use that if the name does not resolve.

To change **Wi‑Fi** later, hold **BOOT** to wipe credentials and reopen the setup AP. Everyday settings (center, scale, units, runways) live on the companion page below — no reset needed.

### Several networks

The device stores up to **5 networks** (`kMaxNetworks`), managed on the companion page under **WLAN‑Netzwerke** — no setup portal needed to add one. Enter the SSID (or hit **Suchen** to pick it from a scan) and the password; the network from the initial setup is added automatically.

When connecting, the device scans and tries the saved networks **strongest signal first**, so it just works when carried between places. Networks that are not in range are skipped and only tried at the end (a scan can miss hidden SSIDs). If none answer, it keeps retrying — the setup portal still never opens on its own.

Passwords are stored on the device so it can reconnect unattended, and are never sent back out by the config API.

### Staying connected

Once a network is saved, the device **never falls back to the setup screen on its own**. A slow router after a power cut, or a weak moment on the link, only produces a “Could not connect — retrying” screen while it keeps trying:

1. **soft reconnect** — re-associates without touching the radio state (the radar stays on screen); handles the usual short drop
2. **full reconnect** — re-begins with the saved credentials and shows the connect animation

The setup portal opens only when no credentials are stored, or after a deliberate reset (**hold BOOT for 10 s**).

If the link still drops often, or the config page is hard to reach, the two knobs in `config.h` are:

| Constant | Meaning |
|----------|---------|
| `kWifiTxPowerSta` | STA transmit power in quarter-dBm (`60` = 15 dBm default, `34` = 8.5 dBm). The C3 Super Mini's antenna is weak — lower values mean short range and frequent drops. Reduce it only if the board resets while connecting (weak USB supply). |
| `kWifiTxPowerAp` | Same for the setup AP, which is used from close by (`34` = 8.5 dBm). |

Wi‑Fi power save is kept off: with modem sleep on, the ESP misses packets between beacons, which is what makes the config page look offline.

## Companion config page

While the device is on your Wi‑Fi it serves a small config page at **`http://plane-radar.local`** or **`http://<device-ip>`**. Everything applies live and persists to NVS.

| Setting | How |
|---------|-----|
| **Center — ICAO code** | Type an airport code (e.g. `EDQH`); resolved on-device from the built-in airport dataset — no internet lookup |
| **Center — coordinates** | Enter latitude / longitude directly |
| **Center — phone GPS** | Opens the HTTPS helper page (below), which reads the phone’s location and sends it back |
| **Scale** | Pick a range preset (5 / 10 / 15 / 25 km) |
| **Track a flight** | Enter a callsign (e.g. `DLH400`) or registration; the radar centers on that aircraft and follows it. On signal loss it stays centered and shows a hint, reverting home after a longer outage. |
| **Wi‑Fi networks** | Add / remove saved networks (up to 5), with a scan to pick the SSID. The device connects to whichever saved network is in range with the strongest signal. |
| **Altitude filter** | Hide traffic below / above a given altitude in feet (0 = no bound) |
| **Display** | Miles vs km, runway overlay, aircraft trails, auto zoom |
| **Firmware** | Upload a `.bin` to update over Wi‑Fi; shows version, UTC time and TLS state |

### Altitude filter

Enter a minimum and/or maximum altitude in feet; anything outside is hidden. `0` means "no bound", so `10000 / 0` shows only high traffic and `0 / 5000` only what is low. Aircraft that report no altitude stay visible — hiding them would drop traffic rather than filter it.

### Trails and auto zoom

Both default to **off**.

**Trails** draw the last six positions of each aircraft as breadcrumbs that fade toward the oldest, making the direction of travel obvious. History is keyed by ICAO address, so it survives the aircraft list being rebuilt on every fetch.

**Auto zoom** picks the range preset from how busy the sky is: it widens after 12 s of an empty radar and tightens after 20 s of five or more aircraft — but only while the tighter ring would still show something, so it cannot oscillate. A manual BOOT tap still overrides it at any time.

### Firmware update over Wi‑Fi (OTA)

The **Firmware** card takes the `firmware.bin` from a build (Actions artifact or `.pio/build/supermini/`) and flashes it over the LAN; the device reboots into the new version. A failed or interrupted upload changes nothing — the running version stays active, because the image is written to the *other* app slot and only becomes the boot target once it is complete and valid.

> **One-time USB flash needed.** The OTA layout splits the flash into two 1.9 MB app slots, which moves the app offset. A device running an older layout has to be flashed once over USB (use the merged image); every update after that can go over Wi‑Fi.

The card also shows the firmware version, the device's UTC time, and whether the ADS-B connection is certificate-verified.

### Time and TLS

The device syncs its clock over NTP once connected. That is what makes certificate validation possible at all — before the clock is set, every certificate looks "not yet valid", so the first fetches run unverified and the connection upgrades itself the moment the time lands.

If verification keeps failing (a root CA rotation newer than the firmware), the client says so on serial and in `/api/state` and continues unverified rather than leaving the radar blank. Set `kAdsbVerifyTls = false` in `config.h` to skip it entirely.

### Phone GPS helper (`docs/gps.html`)

Browsers only expose GPS over **HTTPS**, which the device (plain HTTP on the LAN) cannot serve. `docs/gps.html` is a static page meant to be hosted over HTTPS — enable **GitHub Pages** for the `/docs` folder, then point `config::kGpsHelperUrl` at it (default: `https://<user>.github.io/<repo>/gps.html`). The device’s “Aktuelle Position vom Handy” button opens it with `?device=<ip>`; the page reads GPS and redirects back to `http://<ip>/api/center?lat=…&lon=…`.

After a Wi‑Fi reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

### "No data" badge

When no fetch has succeeded for 15 s, a struck-through Wi‑Fi glyph with the age of the newest data appears at the bottom of the radar. An empty radar is otherwise ambiguous: quiet sky, or dead connection?

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 5 km / 3 mi | ~6.7 km |
| 10 km / 6 mi | ~13.3 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33.3 km |

Preset and miles/km choice persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle on the companion config page
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring** — red heading triangle, magenta speed vector (clipped at the ring), callsign / type / altitude tags
- **Outside the ring** (still within ADS-B fetch) — small **red dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**

As range decreases (or aircraft approach), targets move inward; beyond-ring dots become full symbols when they cross the outer ring.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (5 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display SPI | pins, `kDisplayInvert`, `kDisplayRgbOrder`, `kDisplaySpiWriteHz` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft` |

Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  services/
```

## Wiring (GC9A01 ↔ ESP32-C3 Super Mini)

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO **0** |
| CS | GPIO **1** |
| DC | GPIO **2** |
| SDA (MOSI) | GPIO **3** |
| SCL (SCLK) | GPIO **4** |
| BOOT (user) | GPIO **9** |

## Build

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: **`supermini`**
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**):

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/supermini/firmware-merged.bin`):

```bash
pio run -e supermini
pio run -t merge -e supermini
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | Push / PR to `main` | Artifact `plane-radar-supermini` (merged + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release asset `plane-radar-v1.0.0.bin` + `.sha256` |

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds firmware in CI and attaches the merged image to the release. Download from **Releases** on GitHub, then flash at **0x0** (ESP32-C3, 4 MB).

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
