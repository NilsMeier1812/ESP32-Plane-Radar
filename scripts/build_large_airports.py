#!/usr/bin/env python3
"""Build runway dataset from OurAirports (large_airport only)."""

from __future__ import annotations

import csv
import io
import math
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "data" / "large_airports.h"

# Airfields that OurAirports does not classify as "large_airport" but that we
# still want on the map (e.g. smaller regional fields). Keyed by ICAO ident:
#   ident: (latitude_deg, longitude_deg)
# To add a field, look up its reference point on ourairports.com and add a
# matching runway below so it actually gets drawn (labels only appear for
# airports that have at least one runway on screen).
EXTRA_AIRPORTS: dict[str, tuple[float, float]] = {
    "EDQH": (49.582708, 10.878339),  # Herzogenaurach
}

# Manual runway segments for the extra airfields above, used when OurAirports
# has no threshold coordinates for the runway. Each entry lists one or more
# runways as (true_bearing_deg, length_m); the endpoints are derived from the
# airport reference point so the runway is centred on it.
EXTRA_RUNWAYS: dict[str, list[tuple[float, float]]] = {
    "EDQH": [(70.0, 700)],  # runway 07/25, ~700 m
}
OUT_CPP = ROOT / "src" / "data" / "large_airports_data.cpp"

AIRPORTS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "airports.csv"
)
RUNWAYS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "runways.csv"
)

def fetch_csv(url: str) -> list[dict[str, str]]:
    with urllib.request.urlopen(url, timeout=60) as resp:
        text = resp.read().decode("utf-8")
    return list(csv.DictReader(io.StringIO(text)))


def coord_e7(s: str | None) -> int | None:
    if not s or not s.strip():
        return None
    return int(round(float(s) * 1e7))


def is_h_designator(s: str) -> bool:
    if not s or s[0] != "H":
        return False
    rest = s[1:]
    if not rest:
        return True
    if rest[0] in "-_":
        return True
    return rest.isdigit()


def is_helipad(row: dict[str, str]) -> bool:
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    if not is_h_designator(le) and not is_h_designator(he):
        return False
    try:
        length_ft = int(row.get("length_ft") or 0)
    except ValueError:
        length_ft = 0
    if is_h_designator(le) and is_h_designator(he):
        return True
    return length_ft < 2500


def offset_e7(
    lat_deg: float, lon_deg: float, bearing_deg: float, dist_m: float
) -> tuple[int, int]:
    """Point at ``dist_m`` from (lat, lon) along ``bearing_deg`` (true)."""
    r_earth = 6371000.0
    br = math.radians(bearing_deg)
    d_lat = math.degrees((dist_m * math.cos(br)) / r_earth)
    d_lon = math.degrees(
        (dist_m * math.sin(br)) / (r_earth * math.cos(math.radians(lat_deg)))
    )
    return coord_e7(str(lat_deg + d_lat)), coord_e7(str(lon_deg + d_lon))


def build_dataset() -> tuple[
    list[tuple[str, int, int]],
    list[tuple[int, int, int, int, int, int]],
]:
    airports = fetch_csv(AIRPORTS_URL)
    runways = fetch_csv(RUNWAYS_URL)

    large_idents: dict[str, tuple[int, int]] = {}
    for a in airports:
        if a.get("type") != "large_airport":
            continue
        ident = (a.get("ident") or "").strip()
        if len(ident) != 4:
            continue
        lat = coord_e7(a.get("latitude_deg"))
        lon = coord_e7(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        large_idents[ident] = (lat, lon)

    for ident, (lat_deg, lon_deg) in EXTRA_AIRPORTS.items():
        large_idents[ident] = (coord_e7(str(lat_deg)), coord_e7(str(lon_deg)))

    airport_rows = sorted(
        (ident, lat, lon) for ident, (lat, lon) in large_idents.items()
    )
    airport_index = {ident: idx for idx, (ident, _, _) in enumerate(airport_rows)}

    segments: list[tuple[int, int, int, int, int, int]] = []
    for r in runways:
        if r.get("closed") == "1":
            continue
        airport = (r.get("airport_ident") or "").strip()
        if airport not in airport_index:
            continue
        if is_helipad(r):
            continue
        try:
            length_ft = int(r.get("length_ft") or 0)
        except ValueError:
            continue
        if length_ft <= 0:
            continue
        le_lat = coord_e7(r.get("le_latitude_deg"))
        le_lon = coord_e7(r.get("le_longitude_deg"))
        he_lat = coord_e7(r.get("he_latitude_deg"))
        he_lon = coord_e7(r.get("he_longitude_deg"))
        if None in (le_lat, le_lon, he_lat, he_lon):
            continue
        length_m = int(round(length_ft * 0.3048))
        segments.append(
            (
                airport_index[airport],
                le_lat,
                le_lon,
                he_lat,
                he_lon,
                length_m,
            )
        )

    for ident, runway_specs in EXTRA_RUNWAYS.items():
        if ident not in airport_index:
            continue
        lat_deg, lon_deg = EXTRA_AIRPORTS[ident]
        for bearing_deg, length_m in runway_specs:
            half = length_m / 2.0
            le_lat, le_lon = offset_e7(lat_deg, lon_deg, bearing_deg + 180.0, half)
            he_lat, he_lon = offset_e7(lat_deg, lon_deg, bearing_deg, half)
            segments.append(
                (
                    airport_index[ident],
                    le_lat,
                    le_lon,
                    he_lat,
                    he_lon,
                    int(round(length_m)),
                )
            )

    segments.sort(key=lambda row: (row[0], -row[5]))
    return airport_rows, segments


def render_header(airport_count: int, segment_count: int) -> str:
    return "\n".join(
        [
            "// Generated by scripts/build_large_airports.py — do not edit.",
            "#pragma once",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace data::large_airports {",
            "",
            "struct Airport {",
            "  char ident[5];",
            "  int32_t lat_e7;",
            "  int32_t lon_e7;",
            "};",
            "",
            "struct Runway {",
            "  uint16_t airport_idx;",
            "  int32_t le_lat_e7;",
            "  int32_t le_lon_e7;",
            "  int32_t he_lat_e7;",
            "  int32_t he_lon_e7;",
            "  uint16_t length_m;",
            "};",
            "",
            f"constexpr size_t kAirportCount = {airport_count};",
            f"constexpr size_t kRunwayCount = {segment_count};",
            "",
            "extern const Airport kAirports[];",
            "extern const Runway kRunways[];",
            "",
            "}  // namespace data::large_airports",
            "",
        ]
    )


def render_cpp(
    airport_rows: list[tuple[str, int, int]],
    segments: list[tuple[int, int, int, int, int, int]],
) -> str:
    lines = [
        "// Generated by scripts/build_large_airports.py — do not edit.",
        '#include "data/large_airports.h"',
        "",
        "namespace data::large_airports {",
        "",
        "const Airport kAirports[] = {",
    ]
    for ident, lat, lon in airport_rows:
        lines.append(f'  {{"{ident}", {lat}, {lon}}},')
    lines += [
        "};",
        "",
        "const Runway kRunways[] = {",
    ]
    for airport_idx, le_lat, le_lon, he_lat, he_lon, length_m in segments:
        lines.append(
            f"  {{{airport_idx}, {le_lat}, {le_lon}, {he_lat}, {he_lon}, {length_m}}},"
        )
    lines += [
        "};",
        "",
        "}  // namespace data::large_airports",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    airport_rows, segments = build_dataset()
    header = render_header(len(airport_rows), len(segments))
    cpp = render_cpp(airport_rows, segments)

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(header, encoding="utf-8")
    OUT_CPP.write_text(cpp, encoding="utf-8")
    print(
        f"wrote {OUT_H.name} + {OUT_CPP.name} "
        f"({len(segments)} segments, {len(airport_rows)} airports)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
