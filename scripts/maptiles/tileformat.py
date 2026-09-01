"""Pack and unpack the PRT1 tile format. See FORMAT.md for the layout.

The unpacker is not used by the generator itself — it exists so the tests can
verify a round trip, and so the same reference implementation is available when
the firmware-side parser is written.
"""

import struct

MAGIC = b"PRT1"
HEADER = struct.Struct("<4sBBHII")
LAYER_HEADER = struct.Struct("<BBHI")
POINT_COUNT = struct.Struct("<H")

LAYER_COAST = 1
LAYER_WATER = 2
LAYER_RIVER = 3
LAYER_ROAD_MAJOR = 4
LAYER_ROAD_MINOR = 5
LAYER_RAIL = 6

LAYER_NAMES = {
    LAYER_COAST: "coast",
    LAYER_WATER: "water",
    LAYER_RIVER: "river",
    LAYER_ROAD_MAJOR: "road_major",
    LAYER_ROAD_MINOR: "road_minor",
    LAYER_RAIL: "rail",
}

FLAG_CLOSED = 0x01

# int16 range, enforced here so a generator bug becomes a loud error instead of
# a silently wrapped coordinate that draws a line across the whole tile.
_COORD_MIN = -32768
_COORD_MAX = 32767


def pack_tile(zoom, tile_x, tile_y, layers):
    """layers: list of (layer_id, flags, [polyline, ...]) with int coordinates.

    Layers with no lines are dropped. Returns None if nothing is left, which
    is the signal not to write the tile at all.
    """
    blocks = []
    for layer_id, flags, lines in layers:
        usable = [ln for ln in lines if len(ln) >= 2]
        if not usable:
            continue
        payload = bytearray()
        for line in usable:
            if len(line) > 0xFFFF:
                raise ValueError("polyline too long for a uint16 point count")
            payload += POINT_COUNT.pack(len(line))
            for x, y in line:
                if not (_COORD_MIN <= x <= _COORD_MAX and
                        _COORD_MIN <= y <= _COORD_MAX):
                    raise ValueError("coordinate %d,%d outside int16" % (x, y))
                payload += struct.pack("<hh", x, y)
        blocks.append(
            LAYER_HEADER.pack(layer_id, flags, len(usable), len(payload)) +
            bytes(payload))

    if not blocks:
        return None

    header = HEADER.pack(MAGIC, zoom, len(blocks), 0, tile_x, tile_y)
    return header + b"".join(blocks)


def unpack_tile(data):
    """Inverse of pack_tile. Returns (zoom, tile_x, tile_y, layers)."""
    magic, zoom, layer_count, _reserved, tile_x, tile_y = HEADER.unpack_from(
        data, 0)
    if magic != MAGIC:
        raise ValueError("not a PRT1 tile")

    offset = HEADER.size
    layers = []
    for _ in range(layer_count):
        layer_id, flags, line_count, byte_len = LAYER_HEADER.unpack_from(
            data, offset)
        offset += LAYER_HEADER.size
        end = offset + byte_len
        lines = []
        for _ in range(line_count):
            n = POINT_COUNT.unpack_from(data, offset)[0]
            offset += POINT_COUNT.size
            pts = []
            for _ in range(n):
                x, y = struct.unpack_from("<hh", data, offset)
                offset += 4
                pts.append((x, y))
            lines.append(pts)
        if offset != end:
            raise ValueError("layer %d payload length mismatch" % layer_id)
        layers.append((layer_id, flags, lines))

    return zoom, tile_x, tile_y, layers
