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
 * Free the frame buffer (240x240x16bpp = 115 kB — by far the largest single
 * allocation). It is dead weight while the loop sits in a network call, and
 * that is exactly when a TLS handshake needs a big contiguous block. The next
 * draw allocates it again; if that fails, drawing falls back to the panel.
 */
void radarReleaseFrameSprite();

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
