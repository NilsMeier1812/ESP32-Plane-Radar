"""Projection, simplification and clipping for the map tile generator.

Pure standard library on purpose. The heavy geodata work (reading .osm.pbf,
assembling ways) is done by osmium in CI; what is left here is arithmetic, and
keeping it dependency-free means it can be unit tested anywhere — including in
environments with no network to install shapely or GDAL into.
"""

import math

# Tile-local coordinate span. See FORMAT.md.
TILE_UNITS = 4096
# Lines are clipped this far outside the tile so a feature that leaves and
# re-enters still joins across the seam instead of showing a notch.
CLIP_MARGIN_UNITS = 128

# Web Mercator cannot represent the poles; this is the standard cutoff.
MAX_LATITUDE = 85.05112878


def project(lon, lat):
    """lon/lat degrees -> Web Mercator, normalised so the world is 0..1.

    y grows southward, matching XYZ tile numbering.
    """
    lat = max(-MAX_LATITUDE, min(MAX_LATITUDE, lat))
    x = (lon + 180.0) / 360.0
    s = math.sin(math.radians(lat))
    y = 0.5 - math.log((1.0 + s) / (1.0 - s)) / (4.0 * math.pi)
    return x, y


def tile_range(bbox, zoom):
    """(min_lon, min_lat, max_lon, max_lat) -> inclusive tile x/y bounds."""
    n = 1 << zoom
    x0, y0 = project(bbox[0], bbox[3])  # north-west corner
    x1, y1 = project(bbox[2], bbox[1])  # south-east corner
    return (
        max(0, int(math.floor(x0 * n))),
        max(0, int(math.floor(y0 * n))),
        min(n - 1, int(math.floor(x1 * n))),
        min(n - 1, int(math.floor(y1 * n))),
    )


def metres_per_world_unit(lat):
    """Ground metres per unit of the normalised 0..1 Mercator square.

    Mercator scales by 1/cos(lat), so a tolerance expressed in metres has to be
    converted at the latitude it will be applied at. The generator uses the
    centre of the region, which is accurate enough over a few hundred km.
    """
    equator_m = 2.0 * math.pi * 6378137.0
    return equator_m * math.cos(math.radians(lat))


def simplify(points, tolerance):
    """Douglas-Peucker in normalised Mercator units.

    Iterative rather than recursive: a coastline way can carry tens of
    thousands of points and Python's recursion limit is not worth fighting.
    """
    if len(points) < 3 or tolerance <= 0.0:
        return list(points)

    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    tol_sq = tolerance * tolerance

    while stack:
        first, last = stack.pop()
        if last <= first + 1:
            continue
        ax, ay = points[first]
        bx, by = points[last]
        dx, dy = bx - ax, by - ay
        seg_sq = dx * dx + dy * dy

        worst = 0.0
        worst_i = -1
        for i in range(first + 1, last):
            px, py = points[i]
            if seg_sq == 0.0:
                ddx, ddy = px - ax, py - ay
            else:
                # Projection parameter of p onto the segment, clamped so the
                # distance is to the segment and not to its infinite line.
                t = ((px - ax) * dx + (py - ay) * dy) / seg_sq
                t = max(0.0, min(1.0, t))
                ddx = px - (ax + t * dx)
                ddy = py - (ay + t * dy)
            d_sq = ddx * ddx + ddy * ddy
            if d_sq > worst:
                worst = d_sq
                worst_i = i

        if worst_i >= 0 and worst > tol_sq:
            keep[worst_i] = True
            stack.append((first, worst_i))
            stack.append((worst_i, last))

    return [p for p, k in zip(points, keep) if k]


def _outcode(x, y, lo, hi):
    code = 0
    if x < lo:
        code |= 1
    elif x > hi:
        code |= 2
    if y < lo:
        code |= 4
    elif y > hi:
        code |= 8
    return code


def clip_polyline(points, lo, hi):
    """Clip a polyline to the square [lo, hi]^2, returning a list of pieces.

    Cohen-Sutherland per segment. A polyline that wanders in and out of the
    tile produces several pieces, which is why this returns a list: drawing
    them as one line would cut straight across the gaps.
    """
    pieces = []
    current = []

    for i in range(len(points) - 1):
        ax, ay = points[i]
        bx, by = points[i + 1]
        code_a = _outcode(ax, ay, lo, hi)
        code_b = _outcode(bx, by, lo, hi)
        accepted = False

        while True:
            if not (code_a | code_b):
                accepted = True
                break
            if code_a & code_b:
                break
            outside = code_a if code_a else code_b
            if outside & 8:
                x = ax + (bx - ax) * (hi - ay) / (by - ay)
                y = hi
            elif outside & 4:
                x = ax + (bx - ax) * (lo - ay) / (by - ay)
                y = lo
            elif outside & 2:
                y = ay + (by - ay) * (hi - ax) / (bx - ax)
                x = hi
            else:
                y = ay + (by - ay) * (lo - ax) / (bx - ax)
                x = lo
            if outside == code_a:
                ax, ay = x, y
                code_a = _outcode(ax, ay, lo, hi)
            else:
                bx, by = x, y
                code_b = _outcode(bx, by, lo, hi)

        if not accepted:
            # Segment misses the box entirely: whatever we were building ends.
            if len(current) >= 2:
                pieces.append(current)
            current = []
            continue

        if not current:
            current = [(ax, ay)]
        elif current[-1] != (ax, ay):
            # The visible part restarts somewhere else along the box edge.
            if len(current) >= 2:
                pieces.append(current)
            current = [(ax, ay)]
        current.append((bx, by))

    if len(current) >= 2:
        pieces.append(current)
    return pieces
