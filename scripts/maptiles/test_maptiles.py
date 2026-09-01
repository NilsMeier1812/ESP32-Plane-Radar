#!/usr/bin/env python3
"""Self-tests for the tile generator, runnable with no network and no data.

The real geodata only exists inside the CI job, so everything that can be
checked with synthetic geometry is checked here: a bad clip or an off-by-one in
the tile maths would otherwise only surface as a wrong-looking map hours later.

Run: python3 scripts/maptiles/test_maptiles.py
"""

import json
import math
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build
import geom
import tileformat as tf

HERE = os.path.dirname(os.path.abspath(__file__))


class TestProjection(unittest.TestCase):
    def test_origin_and_corners(self):
        self.assertAlmostEqual(geom.project(-180.0, 0.0)[0], 0.0)
        self.assertAlmostEqual(geom.project(180.0, 0.0)[0], 1.0)
        self.assertAlmostEqual(geom.project(0.0, 0.0)[1], 0.5)

    def test_y_grows_southward(self):
        north = geom.project(0.0, 60.0)[1]
        south = geom.project(0.0, -60.0)[1]
        self.assertLess(north, south)

    def test_latitude_is_clamped(self):
        # Un-clamped this is a division by zero in the Mercator formula.
        self.assertTrue(math.isfinite(geom.project(0.0, 90.0)[1]))
        self.assertTrue(math.isfinite(geom.project(0.0, -90.0)[1]))

    def test_tile_range_covers_amsterdam(self):
        # The firmware's default centre, at the zoom used for the wide ranges.
        x0, y0, x1, y1 = geom.tile_range((4.0, 52.0, 5.5, 52.7), 9)
        self.assertLessEqual(x0, x1)
        self.assertLessEqual(y0, y1)
        n = 1 << 9
        self.assertTrue(0 <= x0 < n and 0 <= y0 < n)

    def test_metres_per_world_unit_shrinks_with_latitude(self):
        self.assertGreater(geom.metres_per_world_unit(0.0),
                           geom.metres_per_world_unit(52.0))


class TestSimplify(unittest.TestCase):
    def test_collinear_points_collapse(self):
        line = [(0.0, 0.0), (0.1, 0.0), (0.2, 0.0), (0.3, 0.0)]
        self.assertEqual(geom.simplify(line, 0.001), [(0.0, 0.0), (0.3, 0.0)])

    def test_endpoints_always_survive(self):
        line = [(i / 100.0, math.sin(i) / 1000.0) for i in range(100)]
        out = geom.simplify(line, 0.01)
        self.assertEqual(out[0], line[0])
        self.assertEqual(out[-1], line[-1])

    def test_a_real_deviation_is_kept(self):
        line = [(0.0, 0.0), (0.5, 0.5), (1.0, 0.0)]
        self.assertEqual(len(geom.simplify(line, 0.1)), 3)

    def test_tighter_tolerance_keeps_more(self):
        line = [(i / 500.0, math.sin(i / 3.0) / 50.0) for i in range(500)]
        coarse = len(geom.simplify(line, 0.01))
        fine = len(geom.simplify(line, 0.0001))
        self.assertLess(coarse, fine)

    def test_survives_deep_splitting(self):
        # A sawtooth forces a split at nearly every point, which is where a
        # recursive implementation would hit Python's stack limit. Kept to a
        # few thousand points: Douglas-Peucker is quadratic in the worst case
        # and this test should cost milliseconds, not minutes.
        line = [(i / 1500.0, (i % 2) / 1000.0) for i in range(1500)]
        self.assertGreater(len(geom.simplify(line, 1e-7)), 500)


class TestClip(unittest.TestCase):
    def test_fully_inside_is_untouched(self):
        line = [(10.0, 10.0), (90.0, 90.0)]
        self.assertEqual(geom.clip_polyline(line, 0.0, 100.0), [line])

    def test_fully_outside_yields_nothing(self):
        line = [(200.0, 200.0), (300.0, 300.0)]
        self.assertEqual(geom.clip_polyline(line, 0.0, 100.0), [])

    def test_crossing_is_cut_at_the_edge(self):
        pieces = geom.clip_polyline([(-50.0, 50.0), (50.0, 50.0)], 0.0, 100.0)
        self.assertEqual(len(pieces), 1)
        self.assertAlmostEqual(pieces[0][0][0], 0.0)
        self.assertAlmostEqual(pieces[0][-1][0], 50.0)

    def test_leaving_and_re_entering_makes_two_pieces(self):
        # This is the case that must not become one line: joining the pieces
        # would draw a segment straight across the excursion.
        line = [(10.0, 50.0), (40.0, 50.0), (40.0, 200.0),
                (60.0, 200.0), (60.0, 50.0), (90.0, 50.0)]
        pieces = geom.clip_polyline(line, 0.0, 100.0)
        self.assertEqual(len(pieces), 2)

    def test_margin_lets_a_line_overhang(self):
        lo = -geom.CLIP_MARGIN_UNITS
        hi = geom.TILE_UNITS + geom.CLIP_MARGIN_UNITS
        pieces = geom.clip_polyline([(-1000.0, 2000.0), (2000.0, 2000.0)],
                                    lo, hi)
        self.assertAlmostEqual(pieces[0][0][0], float(lo))


class TestTileFormat(unittest.TestCase):
    def test_round_trip(self):
        layers = [
            (tf.LAYER_COAST, 0, [[(0, 0), (100, 200), (4096, 4096)]]),
            (tf.LAYER_ROAD_MAJOR, 0, [[(1, 2), (3, 4)], [(5, 6), (7, 8)]]),
        ]
        blob = tf.pack_tile(11, 1050, 674, layers)
        zoom, tx, ty, out = tf.unpack_tile(blob)
        self.assertEqual((zoom, tx, ty), (11, 1050, 674))
        self.assertEqual(out, layers)

    def test_empty_tile_is_not_written(self):
        self.assertIsNone(tf.pack_tile(9, 0, 0, []))
        self.assertIsNone(tf.pack_tile(9, 0, 0, [(tf.LAYER_RAIL, 0, [])]))

    def test_single_point_lines_are_dropped(self):
        self.assertIsNone(tf.pack_tile(9, 0, 0, [(tf.LAYER_RAIL, 0, [[(1, 1)]])]))

    def test_negative_coordinates_survive(self):
        # Clip margin coordinates are negative; int16 must be signed on both
        # the pack and unpack side or the overhang wraps to +32k.
        blob = tf.pack_tile(9, 0, 0, [(tf.LAYER_COAST, 0,
                                       [[(-128, -128), (4224, 4224)]])])
        _, _, _, out = tf.unpack_tile(blob)
        self.assertEqual(out[0][2][0][0], (-128, -128))

    def test_out_of_range_coordinate_is_rejected(self):
        with self.assertRaises(ValueError):
            tf.pack_tile(9, 0, 0, [(tf.LAYER_COAST, 0, [[(0, 0), (40000, 0)]])])

    def test_bad_magic_is_rejected(self):
        blob = bytearray(tf.pack_tile(9, 0, 0,
                                      [(tf.LAYER_RAIL, 0, [[(0, 0), (1, 1)]])]))
        blob[0:4] = b"XXXX"
        with self.assertRaises(ValueError):
            tf.unpack_tile(bytes(blob))

    def test_header_is_sixteen_bytes(self):
        # The firmware parser will read this as a fixed-size struct.
        self.assertEqual(tf.HEADER.size, 16)
        self.assertEqual(tf.LAYER_HEADER.size, 8)


class TestClassify(unittest.TestCase):
    def test_known_tags(self):
        self.assertEqual(build.classify({"natural": "coastline"}),
                         tf.LAYER_COAST)
        self.assertEqual(build.classify({"highway": "motorway"}),
                         tf.LAYER_ROAD_MAJOR)
        self.assertEqual(build.classify({"waterway": "canal"}),
                         tf.LAYER_RIVER)
        self.assertEqual(build.classify({"railway": "rail"}), tf.LAYER_RAIL)

    def test_irrelevant_tags_are_dropped(self):
        self.assertIsNone(build.classify({"highway": "residential"}))
        self.assertIsNone(build.classify({"amenity": "cafe"}))
        self.assertIsNone(build.classify({}))

    def test_water_wins_over_later_rules(self):
        self.assertEqual(
            build.classify({"natural": "water", "railway": "rail"}),
            tf.LAYER_WATER)


class TestGeometryFlattening(unittest.TestCase):
    def test_polygon_rings_become_polylines(self):
        poly = {"type": "Polygon",
                "coordinates": [[[0, 0], [1, 0], [1, 1], [0, 0]],
                                [[0.2, 0.2], [0.3, 0.2], [0.3, 0.3],
                                 [0.2, 0.2]]]}
        self.assertEqual(len(build.geometry_lines(poly)), 2)

    def test_multipolygon_is_flattened(self):
        multi = {"type": "MultiPolygon",
                 "coordinates": [[[[0, 0], [1, 0], [0, 0]]],
                                 [[[2, 2], [3, 2], [2, 2]]]]}
        self.assertEqual(len(build.geometry_lines(multi)), 2)

    def test_points_are_ignored(self):
        self.assertEqual(build.geometry_lines({"type": "Point",
                                               "coordinates": [0, 0]}), [])


class TestEndToEnd(unittest.TestCase):
    """Drive build.py exactly as CI does, on synthetic Dutch-ish geometry."""

    def _write_features(self, path):
        features = [
            {"type": "Feature",
             "properties": {"natural": "coastline"},
             "geometry": {"type": "LineString",
                          "coordinates": [[4.0, 52.0], [4.4, 52.3],
                                          [4.6, 52.5], [4.9, 52.9]]}},
            {"type": "Feature",
             "properties": {"highway": "motorway"},
             "geometry": {"type": "LineString",
                          "coordinates": [[4.8, 52.3], [5.0, 52.35],
                                          [5.3, 52.4]]}},
            {"type": "Feature",
             "properties": {"natural": "water"},
             "geometry": {"type": "Polygon",
                          "coordinates": [[[5.0, 52.5], [5.4, 52.5],
                                           [5.4, 52.7], [5.0, 52.7],
                                           [5.0, 52.5]]]}},
            # Outside the bbox entirely — must not reach any tile.
            {"type": "Feature",
             "properties": {"railway": "rail"},
             "geometry": {"type": "LineString",
                          "coordinates": [[20.0, 20.0], [20.1, 20.1]]}},
            # Not a layer we carry.
            {"type": "Feature",
             "properties": {"amenity": "cafe"},
             "geometry": {"type": "Point", "coordinates": [4.9, 52.4]}},
        ]
        with open(path, "w", encoding="utf-8") as handle:
            for feature in features:
                handle.write("\x1e" + json.dumps(feature) + "\n")

    def test_generates_parseable_tiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "features.geojsonl")
            out = os.path.join(tmp, "tiles")
            self._write_features(src)

            result = subprocess.run(
                [sys.executable, os.path.join(HERE, "build.py"), src,
                 "--out", out, "--region", "test",
                 "--bbox", "3.3,50.7,7.3,53.6",
                 "--zoom", "9:150", "--zoom", "11:30"],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

            manifest_path = os.path.join(out, "manifest.json")
            self.assertTrue(os.path.exists(manifest_path))
            with open(manifest_path, encoding="utf-8") as handle:
                manifest = json.load(handle)
            self.assertEqual(manifest["format"], "PRT1")
            self.assertEqual([z["zoom"] for z in manifest["zooms"]], [9, 11])
            self.assertTrue(all(z["tiles"] > 0 for z in manifest["zooms"]))

            written = []
            for root, _dirs, files in os.walk(out):
                written.extend(os.path.join(root, f) for f in files
                               if f.endswith(".prt"))
            self.assertTrue(written)

            seen_layers = set()
            for path in written:
                with open(path, "rb") as handle:
                    zoom, tx, ty, layers = tf.unpack_tile(handle.read())
                # The path must agree with the header, or the device would
                # happily draw a tile from somewhere else entirely.
                parts = path.split(os.sep)
                self.assertEqual((str(zoom), str(tx), "%d.prt" % ty),
                                 (parts[-3], parts[-2], parts[-1]))
                for layer_id, _flags, lines in layers:
                    seen_layers.add(layer_id)
                    for line in lines:
                        self.assertGreaterEqual(len(line), 2)

            self.assertIn(tf.LAYER_COAST, seen_layers)
            self.assertIn(tf.LAYER_ROAD_MAJOR, seen_layers)
            self.assertIn(tf.LAYER_WATER, seen_layers)
            self.assertNotIn(tf.LAYER_RAIL, seen_layers)

    def test_finer_zoom_keeps_more_detail(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "features.geojsonl")
            out = os.path.join(tmp, "tiles")
            # A wiggly coastline: the whole point of two zoom levels is that
            # the fine one keeps wiggles the coarse one throws away.
            coords = [[4.0 + i * 0.002, 52.0 + math.sin(i / 2.0) * 0.004]
                      for i in range(400)]
            with open(src, "w", encoding="utf-8") as handle:
                handle.write(json.dumps({
                    "type": "Feature",
                    "properties": {"natural": "coastline"},
                    "geometry": {"type": "LineString",
                                 "coordinates": coords}}) + "\n")

            subprocess.run(
                [sys.executable, os.path.join(HERE, "build.py"), src,
                 "--out", out, "--region", "test",
                 "--bbox", "3.3,50.7,7.3,53.6",
                 "--zoom", "9:150", "--zoom", "11:30"],
                capture_output=True, text=True, check=True)

            with open(os.path.join(out, "manifest.json"),
                      encoding="utf-8") as handle:
                zooms = {z["zoom"]: z for z in json.load(handle)["zooms"]}
            self.assertGreater(zooms[11]["bytes"], zooms[9]["bytes"])

    def test_tiles_stay_within_the_ram_budget(self):
        """A tile must fit the device's cache, or the format is wrong.

        The widest view straddles up to 3x3 tiles, so with a ~30 kB cache a
        single tile has to stay under roughly 3.4 kB. 8 kB is the looser bound
        that still catches a tolerance change making tiles explode; the real
        number comes from the CI run on Dutch data, and the preview page
        reports the resident total for an actual centre.
        """
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "features.geojsonl")
            out = os.path.join(tmp, "tiles")
            self._write_features(src)
            subprocess.run(
                [sys.executable, os.path.join(HERE, "build.py"), src,
                 "--out", out, "--region", "test",
                 "--bbox", "3.3,50.7,7.3,53.6"],
                capture_output=True, text=True, check=True)
            with open(os.path.join(out, "manifest.json"),
                      encoding="utf-8") as handle:
                for zoom in json.load(handle)["zooms"]:
                    self.assertLess(zoom["max_bytes"], 8192)


if __name__ == "__main__":
    unittest.main(verbosity=2)
