#!/usr/bin/env python3
"""Turn OSM features into a PRT1 tile tree for the radar background.

Input is line-delimited GeoJSON as produced by:

    osmium export filtered.osm.pbf -f geojsonseq -o features.geojsonl

which keeps memory sane: one feature per line, streamed rather than loaded as
one giant JSON document.

Output is a directory tree of `z/x/y.prt` plus a `manifest.json` describing
what was generated, ready to be served as static files.
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import geom  # noqa: E402
import tileformat as tf  # noqa: E402

# Which OSM tags land in which layer. Order matters: the first rule that
# matches a feature wins, so a waterway that is also tagged as a road-ish thing
# still ends up in the water layers.
TAG_RULES = [
    (tf.LAYER_COAST, [("natural", {"coastline"})]),
    (tf.LAYER_WATER, [("natural", {"water"}), ("landuse", {"reservoir"}),
                      ("waterway", {"riverbank"})]),
    (tf.LAYER_RIVER, [("waterway", {"river", "canal"})]),
    (tf.LAYER_ROAD_MAJOR, [("highway", {"motorway", "trunk",
                                        "motorway_link", "trunk_link"})]),
    (tf.LAYER_ROAD_MINOR, [("highway", {"primary", "primary_link"})]),
    (tf.LAYER_RAIL, [("railway", {"rail"})]),
]

# Simplification tolerance in ground metres per zoom level. Chosen so detail
# per screen pixel stays roughly constant across the radar's range presets —
# see FORMAT.md for why one tolerance for all zooms is what makes coastlines
# look polygonal.
DEFAULT_ZOOMS = {9: 150.0, 11: 30.0}


def classify(props):
    for layer_id, rules in TAG_RULES:
        for key, values in rules:
            if props.get(key) in values:
                return layer_id
    return None


def geometry_lines(geometry):
    """Flatten any GeoJSON geometry into a list of coordinate sequences.

    Polygon rings come back as ordinary polylines. GeoJSON already repeats the
    first point at the end of a ring, so an outline draws correctly without the
    parser needing to know it was a polygon.
    """
    kind = geometry.get("type")
    coords = geometry.get("coordinates")
    if kind == "LineString":
        return [coords]
    if kind == "MultiLineString":
        return list(coords)
    if kind == "Polygon":
        return list(coords)
    if kind == "MultiPolygon":
        return [ring for poly in coords for ring in poly]
    return []


def load_features(path, bbox):
    """Stream the GeoJSON sequence, keeping what falls in the bbox.

    Returns {layer_id: [projected polyline, ...]}.
    """
    by_layer = {}
    kept = skipped = 0

    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            raw = raw.strip()
            # RFC 8142 allows a leading record separator on each line.
            if raw.startswith("\x1e"):
                raw = raw[1:]
            if not raw:
                continue
            try:
                feature = json.loads(raw)
            except json.JSONDecodeError:
                skipped += 1
                continue

            layer_id = classify(feature.get("properties") or {})
            if layer_id is None:
                skipped += 1
                continue

            for line in geometry_lines(feature.get("geometry") or {}):
                pts = [(p[0], p[1]) for p in line if len(p) >= 2]
                if len(pts) < 2:
                    continue
                # Keep or drop the whole polyline: filtering individual points
                # against the bbox would quietly join a line straight across
                # the part that was cut out. Tiles outside the region are not
                # written anyway, so keeping the overhang costs nothing and
                # makes features reach the region edge properly.
                if not any(bbox[0] <= lon <= bbox[2] and bbox[1] <= lat <= bbox[3]
                           for lon, lat in pts):
                    skipped += 1
                    continue
                by_layer.setdefault(layer_id, []).append(
                    [geom.project(lon, lat) for lon, lat in pts])
                kept += 1

    return by_layer, kept, skipped


def tiles_for_line(points, zoom):
    """Tile x/y range a projected polyline touches at this zoom."""
    n = 1 << zoom
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return (
        max(0, int(math.floor(min(xs) * n))),
        max(0, int(math.floor(min(ys) * n))),
        min(n - 1, int(math.floor(max(xs) * n))),
        min(n - 1, int(math.floor(max(ys) * n))),
    )


def build_zoom(by_layer, zoom, tolerance_m, centre_lat, out_dir, bbox):
    """Generate every tile at one zoom level. Returns statistics."""
    n = 1 << zoom
    tolerance_world = tolerance_m / geom.metres_per_world_unit(centre_lat)
    lo = -geom.CLIP_MARGIN_UNITS
    hi = geom.TILE_UNITS + geom.CLIP_MARGIN_UNITS

    # {(x, y): {layer_id: [line, ...]}}
    buckets = {}
    tx0, ty0, tx1, ty1 = geom.tile_range(bbox, zoom)

    for layer_id, lines in sorted(by_layer.items()):
        for line in lines:
            simplified = geom.simplify(line, tolerance_world)
            if len(simplified) < 2:
                continue
            ax0, ay0, ax1, ay1 = tiles_for_line(simplified, zoom)
            for tx in range(max(ax0, tx0), min(ax1, tx1) + 1):
                for ty in range(max(ay0, ty0), min(ay1, ty1) + 1):
                    local = [((x * n - tx) * geom.TILE_UNITS,
                              (y * n - ty) * geom.TILE_UNITS)
                             for x, y in simplified]
                    for piece in geom.clip_polyline(local, lo, hi):
                        quantised = _dedupe(
                            [(int(round(px)), int(round(py)))
                             for px, py in piece])
                        if len(quantised) >= 2:
                            buckets.setdefault((tx, ty), {}).setdefault(
                                layer_id, []).append(quantised)

    total_bytes = 0
    largest = 0
    for (tx, ty), layers in buckets.items():
        blob = tf.pack_tile(
            zoom, tx, ty,
            [(lid, 0, lines) for lid, lines in sorted(layers.items())])
        if blob is None:
            continue
        path = os.path.join(out_dir, str(zoom), str(tx))
        os.makedirs(path, exist_ok=True)
        with open(os.path.join(path, "%d.prt" % ty), "wb") as handle:
            handle.write(blob)
        total_bytes += len(blob)
        largest = max(largest, len(blob))

    count = len(buckets)
    return {
        "zoom": zoom,
        "tolerance_m": tolerance_m,
        "tiles": count,
        "bytes": total_bytes,
        "avg_bytes": round(total_bytes / count) if count else 0,
        "max_bytes": largest,
    }


def _dedupe(points):
    """Drop consecutive duplicates left behind by quantising to int16."""
    if not points:
        return []
    out = [points[0]]
    for p in points[1:]:
        if p != out[-1]:
            out.append(p)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("features", help="line-delimited GeoJSON from osmium")
    parser.add_argument("--out", required=True, help="output tile directory")
    parser.add_argument("--region", required=True, help="name for the manifest")
    parser.add_argument("--bbox", required=True,
                        help="min_lon,min_lat,max_lon,max_lat")
    parser.add_argument("--zoom", action="append", default=[],
                        metavar="Z:TOLERANCE_M",
                        help="repeatable; defaults to 9:150 and 11:30")
    args = parser.parse_args()

    bbox = tuple(float(v) for v in args.bbox.split(","))
    if len(bbox) != 4:
        parser.error("--bbox needs four comma-separated numbers")

    zooms = {}
    for spec in args.zoom:
        z, _, tol = spec.partition(":")
        zooms[int(z)] = float(tol)
    if not zooms:
        zooms = dict(DEFAULT_ZOOMS)

    centre_lat = (bbox[1] + bbox[3]) / 2.0

    print("reading %s" % args.features, flush=True)
    by_layer, kept, skipped = load_features(args.features, bbox)
    print("  %d polylines kept, %d features skipped" % (kept, skipped))
    for layer_id, lines in sorted(by_layer.items()):
        print("  %-11s %d" % (tf.LAYER_NAMES.get(layer_id, "?"), len(lines)))

    os.makedirs(args.out, exist_ok=True)
    stats = []
    for zoom in sorted(zooms):
        print("building zoom %d (tolerance %g m)" % (zoom, zooms[zoom]),
              flush=True)
        result = build_zoom(by_layer, zoom, zooms[zoom], centre_lat, args.out,
                            bbox)
        stats.append(result)
        print("  %d tiles, %.1f kB total, %d B average, %d B largest"
              % (result["tiles"], result["bytes"] / 1024.0,
                 result["avg_bytes"], result["max_bytes"]))

    manifest = {
        "format": "PRT1",
        "region": args.region,
        "bbox": list(bbox),
        "tile_units": geom.TILE_UNITS,
        "layers": {str(k): v for k, v in sorted(tf.LAYER_NAMES.items())},
        "zooms": stats,
    }
    with open(os.path.join(args.out, "manifest.json"), "w",
              encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    grand_total = sum(s["bytes"] for s in stats)
    print("total %.1f MB across %d tiles"
          % (grand_total / 1048576.0, sum(s["tiles"] for s in stats)))


if __name__ == "__main__":
    main()
