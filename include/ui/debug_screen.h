#pragma once

#include <cstdint>

namespace ui {

/**
 * On-device diagnostics, readable without the companion page.
 *
 * When the radar stops showing aircraft, the interesting state (why the last
 * fetch failed, how much heap is left, whether the clock and TLS came up) is
 * exactly the state that is hard to reach: the config page shares the same
 * blocked main loop. So it goes on the display, as short numeric codes that
 * fit a 240x240 round panel and can be read out over the phone.
 *
 * Reached with a double tap on BOOT; single tap then flips pages, double tap
 * again returns to the radar.
 */

constexpr uint8_t kDebugPageCount = 2;

/** Render one page into the shared frame buffer and push it to the panel. */
void debugScreenDraw(uint8_t page);

/** Dump the same values to the serial log, in one block. */
void debugScreenLog();

}  // namespace ui
