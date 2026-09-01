#pragma once

#include <LovyanGFX.hpp>

namespace ui {

/**
 * The off-screen frame buffer the radar composites into, so other full-screen
 * views can reuse it and stay flicker-free. Null when the allocation failed;
 * callers then draw straight to the panel.
 */
LGFX_Sprite* radarFrameSprite();

/**
 * Claim the frame buffer during setup, before Wi-Fi and TLS have carved up the
 * heap. It needs 115 kB in one piece (240x240x16bpp), and once the heap is
 * fragmented that block is simply not there any more — the allocation then
 * fails for good and every redraw falls back to clearing the panel directly,
 * which looks like the whole screen flickering. So take it while it is easy,
 * and never give it back.
 */
void radarPrepareFrameSprite();

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/**
 * Auto zoom: widen the range when the radar stays empty, tighten it when it
 * gets crowded. Call once per fetch; a no-op unless ui::radar::autoZoom() is
 * on. Returns true when the range changed, so the caller can redraw the grid.
 */
bool radarAutoZoomTick();

}  // namespace ui
