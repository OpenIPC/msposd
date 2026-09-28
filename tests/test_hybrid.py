"""Deterministic tests for hybrid geometry, labels, archive reads, previews, and server export."""

import functools
import gzip
import io
import json
import math
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gs'))
from PIL import Image
import mapbox_vector_tile
from pmtiles.writer import Writer
from pmtiles.tile import Compression, TileType, zxy_to_tileid
import hybrid_render
import hybrid_service
from hybrid_render import compose_tile, features, render_overlay
from hybrid_service import HybridService, HybridSession
import protomaps_source
from protomaps_source import SourceManager, VectorSource, decode_tile, padded_bounds
import mapserver

FONTS = Path(__file__).resolve().parents[1] / 'gs/assets/fonts'
DEFAULTS = mapserver.HYBRID_DEFAULTS
OFF = {k: False if isinstance(v, bool) else v for k, v in DEFAULTS.items()}   # every layer switched off

# Encoder input for source tile 15/100/100: a road, a building with a hole, a village and a city.
FIXTURE_LAYERS = {
    'roads': [{'id': 1, 'properties': {'kind': 'major_road', 'kind_detail': 'primary', 'name': 'Main Road', 'sort_rank': 3,
                                       'min_zoom': 12.5, 'is_bridge': False, 'layer': -1},
               'geometry': {'type': 'LineString', 'coordinates': [[0, 800], [4096, 800]]}}],
    'buildings': [{'id': 2, 'properties': {'kind': 'building'}, 'geometry': {'type': 'Polygon',
                   'coordinates': [[[1000, 1000], [2500, 1000], [2500, 2500], [1000, 2500], [1000, 1000]],
                                   [[1400, 1400], [1400, 2100], [2100, 2100], [2100, 1400], [1400, 1400]]]}}],
    'places': [{'id': 3, 'properties': {'kind': 'locality', 'kind_detail': 'village', 'name': 'Село'},
                'geometry': {'type': 'Point', 'coordinates': [1800, 3100]}},
               {'id': 4, 'properties': {'kind': 'locality', 'kind_detail': 'city', 'name': 'City'},
                'geometry': {'type': 'Point', 'coordinates': [1800, 3800]}}],
}


def encode_fixture():
    """Return (MVT bytes, layer names) of FIXTURE_LAYERS from the reference encoder."""
    layers = [{'name': name, 'features': items} for name, items in FIXTURE_LAYERS.items()]
    return (mapbox_vector_tile.encode(layers, default_options={'y_coord_down': True}),
            list(FIXTURE_LAYERS))


def satellite(z, x, y):
    """Return synthetic dark imagery for XYZ tile z/x/y, without network access."""
    output = io.BytesIO()
    Image.new('RGB', (256, 256), (20, 50, 30)).save(output, 'PNG')
    return output.getvalue()


class FixtureSource:
    """Provide the decoded fixture tile at 15/100/100 and cover every area."""
    maxzoom = 15

    def tile(self, z, x, y):
        """Return the fixture at source XYZ 15/100/100; other coordinates are empty."""
        return decode_tile(encode_fixture()[0]) if (z, x, y) == (15, 100, 100) else {}

    def covers(self, bounds, z):
        """Report complete coverage for any W/S/E/N bounds and zoom."""
        return True


class Terrain:
    """Synthetic elevation: rolling hills, or flat ground when height is 0."""

    def __init__(self, height=150):
        """Store hill amplitude in metres."""
        self.height = height

    @functools.lru_cache(maxsize=64)
    def tile(self, z, x, y):
        """Return 256 x 256 row-major elevations for tile z/x/y, continuous across tiles."""
        return [300 + self.height * math.sin((x * 256 + i) / 9) * math.cos((y * 256 + j) / 13)
                for j in range(256) for i in range(256)]


HILLS = Terrain()


def render(source, z, gx, gy, settings, terrain=HILLS):
    """Return {(z, x, y): JPEG} for every tile of a group, composed over synthetic imagery."""
    overlay = render_overlay(source, z, gx, gy, settings, FONTS, terrain)
    return {(z, x, y): compose_tile(overlay, gx, gy, x, y, satellite(z, x, y))
            for x in range(gx, gx + hybrid_render.GROUP) for y in range(gy, gy + hybrid_render.GROUP)}


def write_archive(path):
    """Write the fixture as a gzip PMTiles archive at path; return path."""
    encoded, names = encode_fixture()
    with path.open('wb') as file:
        writer = Writer(file)
        writer.write_tile(zxy_to_tileid(15, 100, 100), gzip.compress(encoded))
        writer.finalize({'tile_type': TileType.MVT, 'tile_compression': Compression.GZIP},
                        {'version': '4.0.0', 'vector_layers': [{'id': name} for name in names]})
    return path


class HybridTests(unittest.TestCase):
    """Check spatial and offline guarantees with small deterministic fixtures."""

    def test_decoder_matches_reference_encoder(self):
        """Decode properties of every scalar type, lines, polygon rings and points."""
        layers = decode_tile(encode_fixture()[0])
        road = layers['roads']['features'][0]
        self.assertEqual(road['properties'], FIXTURE_LAYERS['roads'][0]['properties'])
        self.assertEqual((road['type'], road['parts']), (2, [[(0, 800), (4096, 800)]]))
        building = layers['buildings']['features'][0]
        self.assertEqual(building['type'], 3)
        self.assertEqual(len(building['parts']), 2)
        self.assertEqual({p for ring in building['parts'] for p in ring},
                         {tuple(p) for ring in FIXTURE_LAYERS['buildings'][0]['geometry']['coordinates'] for p in ring})
        village = layers['places']['features'][0]
        self.assertEqual((village['type'], village['parts'], village['properties']['name']),
                         (1, [[(1800, 3100)]], 'Село'))
        self.assertEqual(layers['roads']['extent'], 4096)

    def test_overzoom_and_y_direction(self):
        """Check z17 geometry scaling and north-to-south coordinate orientation."""
        result = list(features(FixtureSource(), 17, 400, 400))
        road = next(f for f in result if f[0] == 'roads')
        self.assertEqual(road[3], [[(102400, 102600), (103424, 102600)]])

    def test_style_changes_and_holes(self):
        """Check independent label switches and unfilled building interiors."""
        source = FixtureSource()
        base = OFF
        image = render(source, 15, 100, 100, base)[15, 100, 100]
        for setting in (k for k, v in DEFAULTS.items() if isinstance(v, bool)):
            changed = render(source, 15, 100, 100, dict(base, **{setting: True}))[15, 100, 100]
            self.assertNotEqual(image, changed, setting)
        buildings = render(source, 15, 100, 100, dict(base, buildings=True))
        decoded = Image.open(io.BytesIO(buildings[15, 100, 100]))
        self.assertLess(sum(abs(a - b) for a, b in zip(decoded.getpixel((110, 110)), (20, 50, 30))), 10)

    def test_road_classes(self):
        """Unpaved tracks are never drawn; footways are thin grey at z17 only; rails are dashed."""
        class Roads(FixtureSource):
            """Provide one horizontal line of a given OSM type across the tile containing 15/100/100."""
            detail = 'track'
            def tile(self, z, x, y):
                """Return the configured road in the tile enclosing 15/100/100 at zoom z; others are empty."""
                if z > 15 or x != y or x != 100 >> (15 - z):
                    return {}
                return {'roads': {'extent': 4096, 'features': [
                    {'id': 1, 'type': 2, 'parts': [[(0, 2048), (4096, 2048)]],
                     'properties': {'kind': 'path', 'kind_detail': self.detail}}]}}
        source = Roads()
        def column(z, detail):
            """Return overlay pixels down a column crossing the line at zoom z, or None if empty."""
            source.detail = detail
            tx = 100 << (z - 15) if z >= 15 else 100 >> (15 - z)
            gx = tx // 4 * 4
            overlay = render_overlay(source, z, gx, gx, DEFAULTS, FONTS)
            return None if overlay is None else [overlay.getpixel(((tx - gx) * 256 + 100, y))
                                                 for y in range(overlay.height)]
        self.assertIsNone(column(15, 'track'))
        self.assertIsNone(column(17, 'path'))
        self.assertIsNone(column(15, 'footway'))
        footway = [p for p in column(17, 'footway') if p[3]]
        self.assertEqual(len(footway), 1)                         # 1 px wide, no dark casing
        r, g, b, _ = footway[0]
        self.assertLess(max(r, g, b) - min(r, g, b), 40)          # greyish
        self.assertIsNotNone(column(11, 'unclassified'))          # paved roads from z10
        self.assertIsNone(column(9, 'unclassified'))
        source.detail = 'rail'
        rail = render_overlay(source, 15, 100, 100, DEFAULTS, FONTS)
        centre = [rail.getpixel((x, 128))[:3] for x in range(0, 60)]
        self.assertGreater(len(set(centre)), 1)                   # alternating dash and gap

    def test_hillshade(self):
        """Hills are shaded seamlessly; flat ground and the switch turned off draw nothing."""
        empty = type('Empty', (FixtureSource,), {'tile': lambda self, z, x, y: {}})()
        style = OFF
        self.assertIsNone(render_overlay(empty, 15, 100, 100, dict(style, hillshade=True), FONTS, Terrain(0)))
        self.assertIsNone(render_overlay(empty, 15, 100, 100, style, FONTS, HILLS))
        shaded = render_overlay(empty, 15, 100, 100, dict(style, hillshade=True), FONTS, HILLS)
        alphas = [a for *_, a in shaded.getdata()]
        self.assertGreater(max(alphas), 60)
        self.assertLessEqual(max(alphas), round(DEFAULTS["shade_dark"] / 100 * 255))
        # Adjacent groups at an overzoomed level join exactly like one wider render.
        left = render_overlay(empty, 17, 400, 400, dict(style, hillshade=True), FONTS, HILLS)
        right = render_overlay(empty, 17, 404, 400, dict(style, hillshade=True), FONTS, HILLS)
        with patch('hybrid_render.GROUP', 8):
            wide = render_overlay(empty, 17, 400, 400, dict(style, hillshade=True), FONTS, HILLS)
        self.assertEqual(list(left.getdata()), list(wide.crop((0, 0, 1024, 1024)).getdata()))
        self.assertEqual(list(right.getdata()), list(wide.crop((1024, 0, 2048, 1024)).getdata()))

    def test_contours(self):
        """Hills give thin grey contour lines; flat ground and the switch turned off give none."""
        self.assertTrue(hybrid_render.contour_lines(HILLS, 15, 100, 100))
        self.assertEqual(hybrid_render.contour_lines(Terrain(0), 15, 100, 100), [])
        empty = type('Empty', (FixtureSource,), {'tile': lambda self, z, x, y: {}})()
        self.assertIsNone(render_overlay(empty, 15, 100, 100, OFF, FONTS, HILLS))
        drawn = render_overlay(empty, 15, 100, 100, dict(OFF, contours=True, contour_alpha=60), FONTS, HILLS)
        self.assertEqual(set(drawn.getdata()) - {(0, 0, 0, 0)}, {hybrid_render.CONTOUR_COLOUR + (153,)})

    def test_village_streets_hidden(self):
        """Residential and service roads are not drawn; unclassified roads still are."""
        for detail, drawn in (('residential', False), ('service', False), ('unclassified', True)):
            self.assertEqual(hybrid_render.ROAD_CLASS.get(detail) is not None, drawn, detail)

    def test_terrain_cache(self):
        """Terrarium tiles decode to metres and are served from disk after the first fetch."""
        buf = io.BytesIO()
        Image.new('RGB', (256, 256), (128, 100, 128)).save(buf, 'PNG')   # 100.5 m
        calls = []
        with tempfile.TemporaryDirectory() as directory:
            cache = protomaps_source.TerrainCache(directory, lambda z, x, y: calls.append(1) or buf.getvalue())
            self.assertEqual(cache.tile(12, 1, 2)[0], 100.5)
            again = protomaps_source.TerrainCache(directory, lambda z, x, y: 1 / 0)
            self.assertEqual(again.tile(12, 1, 2)[-1], 100.5)
        self.assertEqual(len(calls), 1)

    def test_labels_across_render_group_boundary(self):
        """Compare adjacent groups against one larger render to detect clipped or inconsistent labels."""
        class EdgeSource(FixtureSource):
            """Provide a village label crossing a render-group boundary."""
            def tile(self, z, x, y):
                """Return a boundary label at XYZ 15/103/101; all other tiles are empty."""
                if (z, x, y) != (15, 103, 101):
                    return {}
                return {'places': {'extent': 4096, 'features': [
                    {'id': None, 'type': 1, 'parts': [[(4000, 2000)]],
                     'properties': {'kind_detail': 'village', 'name': 'Boundary village'}}]}}
        source = EdgeSource()
        tiles = {**render(source, 15, 100, 100, DEFAULTS), **render(source, 15, 104, 100, DEFAULTS)}
        with patch('hybrid_render.GROUP', 8):
            reference = render(source, 15, 100, 100, DEFAULTS)
        for key, data in tiles.items():
            self.assertEqual(data, reference[key], key)

    def test_download_order_renders_each_group_once(self):
        """A tall area in download order renders every group once and fetches imagery once per tile."""
        fetches = []
        session = HybridSession({'style': DEFAULTS}, FONTS,
                                lambda z, x, y: fetches.append((x, y)) or satellite(z, x, y), [FixtureSource()])
        with patch.object(hybrid_service, 'render_overlay', wraps=render_overlay) as renders:
            for x, y in mapserver.tile_order(range(400, 408), range(400, 448)):
                session.tile(17, x, y)
        self.assertEqual(renders.call_count, 24)
        self.assertEqual(len(fetches), 8 * 48)

    def test_download_order_covers_ranges_once(self):
        """Group ordering yields each tile of unaligned ranges exactly once."""
        order = list(mapserver.tile_order(range(3, 10), range(5, 7)))
        self.assertEqual(sorted(order), [(x, y) for x in range(3, 10) for y in range(5, 7)])

    def test_imagery_failure_does_not_rerender_group(self):
        """A failed satellite fetch retries only that tile, reusing the cached overlay."""
        failures = [True]
        def flaky(z, x, y):
            """Fail the first request, then return imagery."""
            if failures.pop() if failures else False:
                raise OSError('network')
            return satellite(z, x, y)
        session = HybridSession({'style': DEFAULTS}, FONTS, flaky, [FixtureSource()])
        with patch.object(hybrid_service, 'render_overlay', wraps=render_overlay) as renders:
            with self.assertRaises(OSError):
                session.tile(17, 400, 400)
            self.assertIsNotNone(session.tile(17, 400, 400))
            session.tile(17, 401, 401)
        self.assertEqual(renders.call_count, 1)

    def test_uncovered_tile_waits_for_vectors(self):
        """A session returns None, not plain imagery, until an area covering the tile is added."""
        class Nowhere(FixtureSource):
            """Cover no area."""
            def covers(self, bounds, z):
                """Report no coverage."""
                return False
        session = HybridSession({'style': DEFAULTS}, FONTS, satellite, [Nowhere()])
        self.assertIsNone(session.tile(17, 400, 400))
        session.add(FixtureSource())
        self.assertIsNotNone(session.tile(17, 400, 400))

    def test_archive_decode(self):
        """Decode a real gzip-compressed PMTiles MVT fixture with correct coordinate orientation."""
        with tempfile.TemporaryDirectory() as directory:
            source = VectorSource(write_archive(Path(directory) / 'sample.pmtiles'), 'fixture')
            self.assertEqual(source.tile(15, 100, 100)['places']['features'][0]['properties']['name'], 'Село')
            self.assertEqual(source.tile(15, 99, 99), {})
            self.assertEqual(source.maxzoom, 15)

    def test_extract_coverage_zoom(self):
        """A z13 regional extract never serves z15 output, which needs z15 source detail."""
        with tempfile.TemporaryDirectory() as directory:
            area = (10, 10, 11, 11)
            source = VectorSource(write_archive(Path(directory) / 'x.pmtiles'), 'u', (area, 13))
            self.assertTrue(source.covers(area, 13))
            self.assertFalse(source.covers(area, 15))
            self.assertFalse(source.covers((9, 10, 11, 11), 13))

    def test_padding_poles_and_dateline(self):
        """Extract buffers stay within world bounds and clamp at the dateline instead of wrapping."""
        west, south, east, north = padded_bounds(85, 84, 180, 179, 10)
        self.assertEqual(east, 180)
        self.assertGreater(west, 170)
        self.assertLessEqual(north, 85.051129)
        self.assertLess(south, north)

    def test_cache_evicts_least_recent_unused_extracts(self):
        """Eviction removes old idle extracts and keeps those referenced by live sessions."""
        with tempfile.TemporaryDirectory() as directory, patch.object(protomaps_source, 'MAX_CACHE', 1000):
            manager = SourceManager(directory, directory)
            paths = []
            for index in range(4):
                path = Path(directory) / f'{index}.pmtiles'
                path.write_bytes(b'x' * 300)
                path.with_suffix('.json').write_text('{}')
                os.utime(path, (index, index))
                paths.append(path)
            class Live:
                """Stand-in referenced extract."""
                path = str(paths[0])
            live = Live()
            manager.live = [live]
            used = manager._evict()
            self.assertTrue(paths[0].exists())         # oldest, but in use
            self.assertFalse(paths[1].exists())
            self.assertFalse(paths[1].with_suffix('.json').exists())
            self.assertTrue(paths[3].exists())
            self.assertLessEqual(used, 750)

    def test_preview_session_reuse_and_cancel(self):
        """Pans inside prepared areas keep the id; uncovered areas cancel the superseded extraction."""
        started, release = threading.Event(), threading.Event()
        calls = []
        class Vectors(FixtureSource):
            """Cover one W/S/E/N area."""
            path = __file__
            def __init__(self, bounds):
                """Store covered bounds."""
                self.bounds = bounds
            def covers(self, bounds, z):
                """Return containment of bounds."""
                return protomaps_source.contains(self.bounds, bounds)
        def prepare(configured, bounds, zoom, progress=None, cancel=None):
            """Record the request and block until released or cancelled."""
            calls.append((bounds, cancel))
            started.set()
            while not release.wait(0.01):
                if cancel.is_set():
                    raise protomaps_source.Cancelled('stop')
            return Vectors(bounds)
        with tempfile.TemporaryDirectory() as directory:
            service = HybridService(directory, FONTS.parents[1], directory)
            options = {'style': DEFAULTS, 'source': 'https://example.invalid/x.pmtiles'}
            view = (45.01, 45.0, 10.01, 10.0)
            with patch.object(service.manager, 'find', return_value=None), \
                    patch.object(service.manager, 'prepare', side_effect=prepare):
                first = service.preview(options, view, 15, satellite)
                self.assertEqual(first['state'], 'loading')
                self.assertTrue(started.wait(5))
                self.assertEqual(service.preview(options, view, 15, satellite)['state'], 'loading')
                self.assertEqual(len(calls), 1)
                far = (46.01, 46.0, 11.01, 11.0)
                self.assertEqual(service.preview(options, far, 15, satellite)['id'], first['id'])
                self.assertTrue(calls[0][1].is_set())
                release.set()
                service.job.future.result(timeout=5)
                self.assertEqual(service.status(first['id'])['state'], 'ready')
                self.assertEqual(service.preview(options, far, 15, satellite)['state'], 'ready')
                restyled = service.preview(dict(options, style=dict(DEFAULTS, cities=True)), far, 15, satellite)
                self.assertNotEqual(restyled['id'], first['id'])
                service.executor.shutdown(wait=True)

    def test_malformed_style_settings(self):
        """Invalid stored style JSON falls back to defaults instead of breaking /status."""
        self.assertEqual(mapserver.hybrid_style('not json'), DEFAULTS)
        self.assertEqual(mapserver.hybrid_style('[1]'), DEFAULTS)
        self.assertEqual(mapserver.hybrid_style('{"roads": "yes", "cities": true}'), dict(DEFAULTS, cities=True))
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": 150}')['contour_alpha'], 30)
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": "50"}')['contour_alpha'], 30)
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": true}')['contour_alpha'], 30)
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": 50}')['contour_alpha'], 50)
        self.assertEqual(mapserver.hybrid_style('{"shade_relief": 21}')['shade_relief'], 5)
        self.assertEqual(mapserver.hybrid_style('{"shade_relief": 12, "shade_dark": 70}')['shade_dark'], 70)

    def test_offline_pack_reuse_and_tms(self):
        """Read only the selected pure-satellite tile, preserving its XYZ/TMS placement."""
        with tempfile.TemporaryDirectory() as directory, patch.object(mapserver, 'MAPS_DIR', directory):
            path = Path(directory) / 'Satellite.mbtiles'
            conn = sqlite3.connect(path)
            conn.executescript('CREATE TABLE metadata(name,value); CREATE TABLE tiles(zoom_level,tile_column,tile_row,tile_data);')
            conn.execute('INSERT INTO metadata VALUES(?,?)', ('sources', '15:Satellite'))
            raw = satellite(15, 100, 100)
            conn.execute('INSERT INTO tiles VALUES(?,?,?,?)', (15, 100, 2**15 - 1 - 100, raw))
            conn.commit(); conn.close()
            original = path.read_bytes()
            with patch.dict(mapserver.config['map'], {'hybrid_satellite_pack': 'Satellite'}), patch.object(mapserver, 'is_online', return_value=False):
                options = mapserver.hybrid_options()
                callback = mapserver.hybrid_satellite(options)
                self.assertEqual(callback(15, 100, 100), raw)
                self.assertIsNone(callback(15, 100, 101))
            self.assertEqual(path.read_bytes(), original)

    def test_download_matches_preview_and_exports_offline(self):
        """Create a hybrid pack from local inputs; verify preview parity, metadata, busy guard and export."""
        import contextlib
        import urllib.request
        import urllib.error
        import zipfile
        from protomaps_source import bounds_for_tiles
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gs/pack'))
        from smoke_hybrid import fixtures
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            root = Path(directory)
            vector, maps = fixtures(root)
            engine = HybridService(root / 'cache', FONTS.parents[1], root / 'bin')
            stack.enter_context(patch.object(mapserver, 'MAPS_DIR', str(maps)))
            stack.enter_context(patch.object(mapserver, 'LANDMARKS_DB', str(maps / 'landmarks.db')))
            stack.enter_context(patch.object(mapserver, 'ELEVATION_DB', str(maps / 'elevation.db')))
            stack.enter_context(patch.object(mapserver, '_hybrid_service', engine))
            stack.enter_context(patch.object(mapserver, 'is_online', return_value=False))
            stack.enter_context(patch.object(mapserver, 'fetch_landmarks', return_value={'elements': []}))
            stack.enter_context(patch.object(mapserver, 'save_config'))
            stack.enter_context(patch.object(mapserver, 'log_cache_summary'))
            stack.enter_context(patch.dict(mapserver.config['server'], {'protomaps_source': str(vector)}))
            stack.enter_context(patch.dict(mapserver.config['map'], {'hybrid_satellite_pack': 'Satellite', 'elevation': '0',
                                                                     'hybrid_style': json.dumps({'hillshade': False})}))
            west, south, east, north = bounds_for_tiles(17, 400, 400, 401, 401)
            bounds = (north - 1e-7, south + 1e-7, east - 1e-7, west + 1e-7)
            options = mapserver.hybrid_options()
            session = engine.prepare(options, bounds, [17], mapserver.hybrid_satellite(options))
            expected = session.tile(17, 400, 400)
            mapserver.download_worker('hybrid-test', [17], [mapserver.HYBRID], *bounds, hybrid=options)
            self.assertEqual(mapserver.dl_status['state'], 'done', mapserver.dl_status)
            self.assertEqual(mapserver.read_tile('hybrid-test', 17, 400, 400), expected)
            metadata = mapserver.pack_metadata('hybrid-test')
            self.assertEqual((metadata['complete'], metadata['failed_tiles'], metadata['format']), ('1', '0', 'jpg'))
            self.assertEqual(metadata['protomaps_build'], 'local:vectors.pmtiles')
            self.assertNotIn(directory, json.dumps(metadata))
            server = mapserver.ThreadingHTTPServer(('127.0.0.1', 0), mapserver.Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            try:
                base = f'http://127.0.0.1:{server.server_port}'
                with urllib.request.urlopen(base + '/tiles/17/400/400?src=hybrid-test&offline=1') as response:
                    self.assertEqual(response.read(), expected)
                with urllib.request.urlopen(base + '/export?src=hybrid-test') as response:
                    archive = zipfile.ZipFile(io.BytesIO(response.read()))
                    self.assertIn('ATTRIBUTION.txt', archive.namelist())
                    self.assertIn('hybrid-test.mbtiles', archive.namelist())
                    self.assertNotIn(directory, archive.read('ATTRIBUTION.txt').decode())
                # A rejected hybrid request must not overwrite a running job's status.
                with patch.dict(mapserver.dl_status, {'state': 'running', 'pack': 'other'}), \
                        patch.dict(mapserver.config['map'], {'hybrid_satellite_pack': 'missing_pack', 'basemap': mapserver.HYBRID, 'sources': ''}):
                    body = json.dumps(dict(north=north, south=south, east=east, west=west)).encode()
                    with self.assertRaises(urllib.error.HTTPError) as rejected:
                        urllib.request.urlopen(urllib.request.Request(base + '/download', data=body,
                                                                      headers={'Content-Type': 'application/json'}))
                    self.assertEqual(rejected.exception.code, 400)
                    self.assertEqual(mapserver.dl_status['state'], 'running')
                with sqlite3.connect(maps / 'hybrid-test.mbtiles') as conn:
                    conn.execute("UPDATE metadata SET value='0' WHERE name='complete'")
                with self.assertRaises(urllib.error.HTTPError) as failure:
                    urllib.request.urlopen(base + '/export?src=hybrid-test')
                self.assertEqual(failure.exception.code, 409)
            finally:
                server.shutdown(); server.server_close(); thread.join()
                engine.executor.shutdown(wait=True)


if __name__ == '__main__':
    unittest.main()
