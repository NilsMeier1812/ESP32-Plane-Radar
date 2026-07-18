#!/usr/bin/env python3
"""Build the airport dataset from OurAirports.

The overlay draws a label at every airport position and, when real runway
geometry is available, the runway line underneath it. Labels do not depend
on runways, so the goal here is broad airport-position coverage; runways are
included only when OurAirports has actual threshold coordinates.

Coverage:
  * large_airport worldwide (as before), and airports already shipped are
    kept even if OurAirports later reclassifies them (see KEEP_EXISTING);
  * every medium_airport / small_airport on the configured continents
    (REGION_CONTINENTS, default Europe) -- included for their position even
    when they have no usable runway geometry.

EXTRA_AIRPORTS remains as a manual backup for fields that OurAirports does
not list at all.
"""

from __future__ import annotations

import csv
import io
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "data" / "large_airports.h"
OUT_CPP = ROOT / "src" / "data" / "large_airports_data.cpp"

AIRPORTS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "airports.csv"
)
RUNWAYS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "runways.csv"
)

# OurAirports continent codes on which medium/small airports are included in
# addition to large airports worldwide. "EU" == Europe.
REGION_CONTINENTS = {"EU"}
REGION_TYPES = {"medium_airport", "small_airport"}

# Keep airports that are already in the generated dataset even if OurAirports
# later reclassifies them out of the worldwide "large_airport" set, so a
# regeneration never silently drops a field that used to be shown.
KEEP_EXISTING = True

# Manual backup for airfields OurAirports does not list at all. Each entry is a
# position that gets a label like any other airport; add one only if a field you
# want is genuinely missing from OurAirports. Ignored when the ident is already
# produced by the automatic pipeline.
#   ident: (latitude_deg, longitude_deg)
EXTRA_AIRPORTS: dict[str, tuple[float, float]] = {
    "EDQH": (49.582708, 10.878339),  # Herzogenaurach (now also from OurAirports)
}


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


def existing_idents() -> set[str]:
    """Idents already present in the generated data file (may be empty)."""
    if not KEEP_EXISTING or not OUT_CPP.exists():
        return set()
    text = OUT_CPP.read_text(encoding="utf-8")
    try:
        block = text.split("kAirports[] = {", 1)[1].split("};", 1)[0]
    except IndexError:
        return set()
    return set(re.findall(r'\{"(\w+)"', block))


def build_dataset() -> tuple[
    list[tuple[str, int, int]],
    list[tuple[int, int, int, int, int, int]],
]:
    airports = fetch_csv(AIRPORTS_URL)
    runways = fetch_csv(RUNWAYS_URL)
    keep = existing_idents()

    # Selected airports: ident -> (lat_e7, lon_e7). Every selected airport gets a
    # label from its position; runway geometry is added below only when present.
    selected: dict[str, tuple[int, int]] = {}
    for a in airports:
        ident = (a.get("ident") or "").strip()
        if len(ident) != 4:
            continue
        a_type = a.get("type")
        in_region = (
            a.get("continent") in REGION_CONTINENTS and a_type in REGION_TYPES
        )
        if a_type != "large_airport" and not in_region and ident not in keep:
            continue
        lat = coord_e7(a.get("latitude_deg"))
        lon = coord_e7(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        selected[ident] = (lat, lon)

    for ident, (lat_deg, lon_deg) in EXTRA_AIRPORTS.items():
        if ident not in selected:
            selected[ident] = (coord_e7(str(lat_deg)), coord_e7(str(lon_deg)))

    airport_rows = sorted(
        (ident, lat, lon) for ident, (lat, lon) in selected.items()
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
            (airport_index[airport], le_lat, le_lon, he_lat, he_lon, length_m)
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
