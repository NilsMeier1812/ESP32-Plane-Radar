# Plane Radar Tile format (`PRT1`)

A map tile the ESP32-C3 can parse with a `for` loop: no gzip, no protobuf, no
allocation beyond the buffer the tile was read into. That is the whole design
goal — the two obvious off-the-shelf choices (raster PNG, Mapbox vector tiles)
both need a 32 kB contiguous inflate window, which is exactly the block the TLS
handshake is already competing for on this board.

Everything is little-endian, which is the native order on the ESP32, so the
device can point a struct at the buffer instead of decoding fields one by one.

## Layout

```
Header — 16 bytes
  offset  size  field
  0       4     magic "PRT1"
  4       1     zoom
  5       1     layer_count
  6       2     reserved (0)
  8       4     tile_x   (uint32)
  12      4     tile_y   (uint32)

Then layer_count layer blocks, each:
  offset  size  field
  0       1     layer_id     (see below)
  1       1     flags        reserved, always 0 in PRT1
  2       2     line_count   (uint16)
  4       4     byte_len     (uint32) size of the payload that follows
  8       ...   payload

Payload — line_count polylines, each:
  offset  size  field
  0       2     point_count  (uint16)
  2       4*n   int16 x, int16 y per point
```

`byte_len` exists so a device can skip a layer it does not know or does not
want to draw (a firmware with roads switched off never has to walk the points).
That keeps the format forward-compatible: new layer ids can be added without
breaking older firmware.

There is no polygon type. Water bodies arrive as their rings, and GeoJSON
already repeats a ring's first point at the end, so an outline draws correctly
from the same loop that draws a road — the parser never needs to know which it
is looking at. Filled water would need a scanline fill on the device and a
different format; it is not worth it under a radar, where thin outlines read
better anyway.

## Coordinates

Tile-local, `0..kTileUnits` (4096) spanning the tile edge to edge, as int16.

Values outside that range are normal and intentional: clipping keeps a margin
(`kClipMarginUnits`) so a line that leaves and re-enters the tile still joins up
correctly instead of showing a notch at the seam. int16 leaves ample room for
that margin.

Resolution per unit:

| zoom | tile ≈ (at 52°N) | metres per unit |
|------|------------------|-----------------|
| 9    | 50 km            | 12 m            |
| 11   | 12 km            | 3 m             |

Both are far finer than the simplification tolerance at their level, so
quantisation is never the limiting factor on how the coastline looks.

## Layer ids

| id | name         | source tags                                          |
|----|--------------|------------------------------------------------------|
| 1  | `coast`      | `natural=coastline`                                   |
| 2  | `water`      | `natural=water`, `landuse=reservoir` (area outlines)  |
| 3  | `river`      | `waterway=river`, `waterway=canal`                    |
| 4  | `road_major` | `highway=motorway`, `highway=trunk`                   |
| 5  | `road_minor` | `highway=primary`                                     |
| 6  | `rail`       | `railway=rail`                                        |

## Zoom levels

The radar shows a 13–67 km wide view across its four range presets — a span of
only 5×, so two levels cover it with detail-per-pixel kept roughly constant:

| zoom | used for ranges | simplification |
|------|-----------------|----------------|
| 9    | 15 km, 25 km    | 150 m          |
| 11   | 5 km, 10 km     | 30 m           |

Generating one dataset for all zooms is what makes coastlines look polygonal:
a tolerance fine enough for the closest range is wasted bytes at the widest,
and a tolerance cheap enough for the widest is visibly angular at the closest.

### How many tiles have to be resident

A z9 tile is about 48 km across at 52°N, and the widest view is 67 km, so the
visible area can straddle **3×3 = 9 tiles**, not the 2×2 an initial estimate
suggested. The preview page reports the real count and the total bytes for a
given centre — that total, not the per-tile size, is what has to fit the
device's cache budget.

## Missing tiles

A tile with no features in any layer is not written. The device treats HTTP 404
as "nothing to draw here" — open sea and featureless land cost no storage and
no transfer.

## Version history

`PRT1` is deliberately unoptimised: points are absolute int16 pairs, 4 bytes
each, where delta coding with a one-byte common case would likely halve that.
That choice waits for measurements from the pilot region — the RAM budget on
the device is what decides whether the extra parser complexity is worth it, and
guessing before the numbers exist is how the frame buffer got too big.
