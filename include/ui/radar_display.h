#pragma once

namespace ui {

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
