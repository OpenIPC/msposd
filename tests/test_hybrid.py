"""Deterministic tests for hybrid geometry, labels, archive reads, previews, and server export."""

import functools
import gzip
import io
import json
import math
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
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
from protomaps_source import LocalArchive, RemoteArchive, TileCache, decode_tile, latest_build
import mapserver

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gs/pack'))

FONTS = Path(__file__).resolve().parents[1] / 'gs/assets/fonts'
DEFAULTS = mapserver.HYBRID_DEFAULTS
OFF = {k: False if isinstance(v, bool) else v for k, v in DEFAULTS.items()}   # every layer switched off
OFF['shade_source'] = 'terrain'                  # hillshade tests use the computed method
STYLE = dict(DEFAULTS, shade_source='terrain')   # defaults, without the network-only Esri shading

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

    identity = 'fixture'
    download_bytes = 0

    def prefetch(self, keys):
        """Nothing to fetch for an in-memory fixture."""


class Terrain:
    """Synthetic elevation: rolling hills, or flat ground when height is 0."""

    def __init__(self, height=150):
        """Store hill amplitude in metres."""
        self.height = height

    def prefetch(self, keys):
        """Nothing to fetch for synthetic terrain."""

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


def smoke():
    """Return the packaged smoke-test module, imported lazily because it imports this module's fixtures."""
    import smoke_hybrid
    return smoke_hybrid


def http_range(url, offset, length):
    """Return length bytes at offset of url using a plain urllib Range request."""
    request = urllib.request.Request(url, headers={'Range': f'bytes={offset}-{offset + length - 1}'})
    with urllib.request.urlopen(request, timeout=5) as response:
        return response.read()


def write_archive(path, tiles=((15, 100, 100),)):
    """Write the fixture into a gzip PMTiles archive at path at each XYZ in tiles; return path."""
    encoded, names = encode_fixture()
    with path.open('wb') as file:
        writer = Writer(file)
        for tile_id in sorted(zxy_to_tileid(*tile) for tile in tiles):
            writer.write_tile(tile_id, gzip.compress(encoded))
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

    def test_hybrid_defaults(self):
        """Satellite Hybrid starts with roads, both place types, contours at 22 and Esri shading at 3."""
        on = {k for k, v in DEFAULTS.items() if v is True}
        self.assertEqual(on, {'roads', 'villages', 'cities', 'contours', 'hillshade'})
        self.assertEqual((DEFAULTS['contour_alpha'], DEFAULTS['shade_source'], DEFAULTS['esri_contrast']),
                         (22, 'esri', 3))

    def test_village_labels_smaller_than_towns(self):
        """Village names use an 11 px font, 20% below the 14 px town names."""
        class Places(FixtureSource):
            """One village or town label at 15/100/100."""
            detail = 'village'
            def tile(self, z, x, y):
                """Return the configured place in tile 15/100/100."""
                if (z, x, y) != (15, 100, 100):
                    return {}
                return {'places': {'extent': 4096, 'features': [{'id': 1, 'type': 1, 'parts': [[(2048, 2048)]],
                        'properties': {'kind_detail': self.detail, 'name': 'Ееее'}}]}}
        source, heights = Places(), {}
        for detail in ('village', 'town'):
            source.detail = detail
            overlay = render_overlay(source, 15, 100, 100, dict(OFF, villages=True, cities=True), FONTS)
            box = overlay.getbbox()
            heights[detail] = box[3] - box[1]
        self.assertLess(heights['village'], heights['town'])
        self.assertAlmostEqual(heights['village'] / heights['town'], 0.8, delta=0.12)

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
        session = HybridSession({'style': STYLE}, FixtureSource(), None, FONTS,
                                lambda z, x, y: fetches.append((x, y)) or satellite(z, x, y))
        with patch.object(hybrid_service, 'render_overlay', wraps=render_overlay) as renders:
            for x, y in mapserver.tile_order(range(400, 408), range(400, 448)):
                session.tile(17, x, y)
        self.assertEqual(renders.call_count, 24)
        self.assertEqual(len(fetches), 8 * 48)

    def test_interleaved_groups_render_once_in_parallel(self):
        """Interleaving keeps every tile once, spreads a batch over distinct groups, and never re-renders."""
        order = list(mapserver.tile_order(range(400, 416), range(400, 412)))      # 12 groups of 16 tiles
        mixed = mapserver.interleave_groups(order, 6)
        self.assertEqual(sorted(mixed), sorted(order))
        self.assertEqual(len({(x // 4, y // 4) for x, y in mixed[:6]}), 6)
        session = HybridSession({'style': STYLE}, FixtureSource(), None, FONTS, satellite)
        with patch.object(hybrid_service, 'render_overlay', wraps=render_overlay) as renders:
            from concurrent.futures import ThreadPoolExecutor
            with ThreadPoolExecutor(6) as pool:
                list(pool.map(lambda t: session.tile(17, *t), mixed))
        self.assertEqual(renders.call_count, 12)

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
        session = HybridSession({'style': STYLE}, FixtureSource(), None, FONTS, flaky)
        with patch.object(hybrid_service, 'render_overlay', wraps=render_overlay) as renders:
            with self.assertRaises(OSError):
                session.tile(17, 400, 400)
            self.assertIsNotNone(session.tile(17, 400, 400))
            session.tile(17, 401, 401)
        self.assertEqual(renders.call_count, 1)

    def test_archive_decode(self):
        """Decode a real gzip-compressed PMTiles MVT fixture with correct coordinate orientation."""
        with tempfile.TemporaryDirectory() as directory:
            source = LocalArchive(write_archive(Path(directory) / 'sample.pmtiles'))
            self.assertEqual(source.identity, 'local:sample.pmtiles')
            self.assertEqual(source.tile(15, 100, 100)['places']['features'][0]['properties']['name'], 'Село')
            self.assertEqual(source.tile(15, 99, 99), {})
            self.assertEqual(source.maxzoom, 15)

    def test_remote_archive_reads_ranges_and_caches(self):
        """Tiles come by HTTP range with directories cached in memory and tiles in SQLite for offline reuse."""
        with tempfile.TemporaryDirectory() as directory:
            server = smoke().serve_ranges(write_archive(Path(directory) / 'planet.pmtiles'))
            url = f'http://127.0.0.1:{server.server_port}/planet.pmtiles'
            calls = []
            def fetch(target, offset, length):
                """Record and perform one range read."""
                calls.append((offset, length))
                return http_range(target, offset, length)
            try:
                cache = TileCache(Path(directory) / 'v.db')
                archive = RemoteArchive(url, fetch, cache, auto=True)
                self.assertEqual(len(calls), 2)                              # header, metadata
                self.assertEqual(archive.tile(15, 100, 100)['places']['features'][0]['properties']['name'], 'Село')
                self.assertEqual(len(calls), 4)                              # root directory, tile
                self.assertEqual(archive.tile(15, 99, 99), {})
                archive.prefetch([(15, 100, 100), (15, 99, 99), (15, 98, 98)])
                self.assertEqual(len(calls), 4)                              # directory reused, absences recorded
                self.assertGreater(archive.download_bytes, 0)
                offline = RemoteArchive(url, lambda *args: (_ for _ in ()).throw(OSError('offline')), cache)
                self.assertEqual(offline.tile(15, 100, 100)['roads']['features'][0]['properties']['name'], 'Main Road')
                self.assertEqual(offline.tile(15, 98, 98), {})
                with self.assertRaises(OSError):
                    offline.tile(15, 97, 97)
                self.assertEqual(latest_build(cache), url)                   # recent build reused, no manifest
            finally:
                server.shutdown()

    def test_prefetch_merges_neighbouring_tiles(self):
        """A group's uncached tiles arrive in one merged range request and decode individually."""
        with tempfile.TemporaryDirectory() as directory:
            tiles = [(15, x, y) for x in (100, 101) for y in (100, 101)]
            server = smoke().serve_ranges(write_archive(Path(directory) / 'p.pmtiles', tiles))
            url = f'http://127.0.0.1:{server.server_port}/p.pmtiles'
            calls = []
            def fetch(target, offset, length):
                """Record and perform one range read."""
                calls.append((offset, length))
                return http_range(target, offset, length)
            try:
                archive = RemoteArchive(url, fetch, TileCache(Path(directory) / 'v.db'))
                archive.prefetch(tiles + [(15, 102, 102)])
                self.assertEqual(len(calls), 4)                              # header, metadata, directory, one chunk
                for tile in tiles:
                    self.assertEqual(archive.tile(*tile)['roads']['features'][0]['properties']['name'], 'Main Road')
                self.assertEqual(archive.tile(15, 102, 102), {})
                self.assertEqual(len(calls), 4)
            finally:
                server.shutdown()

    def test_tile_cache_eviction(self):
        """Once over its bound the cache drops the least recently used tiles down to 75%."""
        with tempfile.TemporaryDirectory() as directory:
            cache = TileCache(Path(directory) / 'v.db', max_bytes=1000)
            for i in range(4):
                cache.put('b', 15, i, 0, b'x' * 300)
                time.sleep(0.002)
            cache.get('b', 15, 0, 0)                                         # oldest row becomes most recent
            with cache.lock:
                cache.evict()
            self.assertEqual([cache.has('b', 15, i, 0) for i in range(4)], [True, False, False, True])

    def test_fetch_range(self):
        """mapserver.fetch_range reads exact byte ranges and refuses to fetch while offline."""
        with tempfile.TemporaryDirectory() as directory:
            path = write_archive(Path(directory) / 'p.pmtiles')
            server = smoke().serve_ranges(path)
            url = f'http://127.0.0.1:{server.server_port}/p.pmtiles'
            try:
                with patch.object(mapserver, 'is_online', return_value=True):
                    self.assertEqual(mapserver.fetch_range(url, 0, 7), b'PMTiles')
                    self.assertEqual(mapserver.fetch_range(url, 10, 20), path.read_bytes()[10:30])
                with patch.object(mapserver, 'is_online', return_value=False), self.assertRaises(OSError):
                    mapserver.fetch_range(url, 0, 7)
            finally:
                server.shutdown()

    def test_preview_sessions_share_overlays_per_style(self):
        """Preview tiles reuse one session per style snapshot; a style change starts a new one."""
        with tempfile.TemporaryDirectory() as directory:
            vector = write_archive(Path(directory) / 'local.pmtiles')
            service = HybridService(directory, FONTS.parents[1], lambda *args: 1 / 0)
            options = {'style': STYLE, 'source': str(vector)}
            with patch.object(hybrid_service, 'render_overlay', wraps=render_overlay) as renders:
                service.tile(options, 17, 400, 400, satellite)
                service.tile(options, 17, 401, 401, satellite)
                self.assertEqual(renders.call_count, 1)
                service.tile(dict(options, style=dict(STYLE, cities=False)), 17, 400, 400, satellite)
                self.assertEqual(renders.call_count, 2)

    def test_shade_mask_blend(self):
        """White hillshade leaves imagery unchanged; grey darkens it, more with higher contrast."""
        def png(colour):
            """Return a 256 x 256 PNG filled with colour."""
            out = io.BytesIO()
            Image.new('RGB', (256, 256), colour).save(out, 'PNG')
            return out.getvalue()
        imagery = png((200, 160, 120))
        def centre(shade, contrast, crop=(0, 0, 256)):
            """Return the blended centre pixel."""
            mask = hybrid_render.shade_mask(shade, crop, contrast)
            return Image.open(io.BytesIO(compose_tile(None, 0, 0, 0, 0, imagery, mask))).getpixel((128, 128))
        near = lambda a, b: all(abs(p - q) <= 3 for p, q in zip(a, b))
        self.assertTrue(near(centre(png((255, 255, 255)), 3), (200, 160, 120)))
        self.assertTrue(near(centre(png((191, 191, 191)), 1), (150, 120, 90)))      # x 0.75
        self.assertTrue(near(centre(png((191, 191, 191)), 3), (50, 40, 30)))        # darkness x 3
        self.assertTrue(near(centre(png((0, 0, 0)), 1, (128, 128, 128)), (0, 0, 0)))

    def test_esri_shading_in_hybrid_keeps_roads_bright(self):
        """With Esri shading the imagery darkens but roads drawn on top keep their colour."""
        out = io.BytesIO()
        Image.new('RGB', (256, 256), (128, 128, 128)).save(out, 'PNG')
        grey = out.getvalue()
        style = dict(OFF, roads=True, hillshade=True, shade_source='esri', esri_contrast=1)
        requests = []
        session = HybridSession({'style': style}, FixtureSource(), None, FONTS, satellite,
                                lambda z, x, y: requests.append((z, x, y)) or (grey, (0, 0, 256)))
        shaded = Image.open(io.BytesIO(session.tile(15, 100, 100)))
        self.assertEqual(requests, [(15, 100, 100)])
        plain = HybridSession({'style': dict(style, shade_source='terrain')}, FixtureSource(), None, FONTS,
                              satellite, lambda *args: 1 / 0)               # Esri never asked for
        unshaded = Image.open(io.BytesIO(plain.tile(15, 100, 100)))
        near = lambda a, b: all(abs(p - q) <= 6 for p, q in zip(a, b))
        self.assertTrue(near(unshaded.getpixel((100, 200)), (20, 50, 30)))
        self.assertTrue(near(shaded.getpixel((100, 200)), (10, 25, 15)))          # imagery x 0.5
        self.assertTrue(near(shaded.getpixel((100, 50)), unshaded.getpixel((100, 50))))   # road on top, not shaded

    def test_shaded_basemap_falls_back_to_parent_hillshade(self):
        """Above Esri's hillshade zooms the parent tile's matching quarter is enlarged and used."""
        def png(colour, split=None):
            """Return a PNG; with split, the right half is white."""
            image = Image.new('RGB', (256, 256), colour)
            if split:
                image.paste((255, 255, 255), (128, 0, 256, 256))
            out = io.BytesIO()
            image.save(out, 'PNG')
            return out.getvalue()
        requests = []
        def tile_get(url):
            """Serve imagery, a 404 for z17 hillshade and a half-dark z16 parent."""
            requests.append(url)
            if 'World_Imagery' in url:
                return png((200, 200, 200))
            if '/tile/17/' in url:
                raise mapserver.TileHTTPError(404)
            return png((128, 128, 128), split=True)
        mapserver._hillshade.cache_clear()
        with patch.object(mapserver, '_tile_get', side_effect=tile_get):
            left = Image.open(io.BytesIO(mapserver.fetch_shaded(17, 800, 800, 1))).getpixel((128, 128))
            self.assertEqual(mapserver.esri_hillshade(17, 801, 801)[1], (128, 128, 128))   # quarter of z16 parent
            right = Image.open(io.BytesIO(mapserver.fetch_shaded(17, 801, 800, 1))).getpixel((128, 128))
        self.assertLess(left[0], 120)                      # left child of z16/400/400: dark half
        self.assertGreater(right[0], 190)                  # right child: white half, imagery unchanged
        self.assertEqual(sum('/tile/16/' in u for u in requests), 1)   # parent fetched once for both
        mapserver._hillshade.cache_clear()

    def test_live_tile_retries_once_on_network_error(self):
        """A reset connection is retried once for live tiles; a 404 is not."""
        server = mapserver.ThreadingHTTPServer(('127.0.0.1', 0), mapserver.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        base = f'http://127.0.0.1:{server.server_port}/tiles/13/10/10?hv=0'
        try:
            with patch.object(mapserver, 'source_for', return_value=mapserver.SHADED), \
                    patch.object(mapserver, 'is_online', return_value=True):
                with patch.object(mapserver, 'fetch_tile', side_effect=[ConnectionResetError(104, 'reset'), b'jpeg']) as fetch:
                    with urllib.request.urlopen(base) as response:
                        self.assertEqual(response.read(), b'jpeg')
                    self.assertEqual(fetch.call_count, 2)
                with patch.object(mapserver, 'fetch_tile', side_effect=mapserver.TileHTTPError(404)) as fetch:
                    with self.assertRaises(urllib.error.HTTPError) as failure:
                        urllib.request.urlopen(base)
                    self.assertEqual((failure.exception.code, fetch.call_count), (503, 1))
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_low_zoom_preview_is_plain_imagery(self):
        """Below HYBRID_PREVIEW_MIN_ZOOM the hybrid preview proxies imagery instead of rendering."""
        server = mapserver.ThreadingHTTPServer(('127.0.0.1', 0), mapserver.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        base = f'http://127.0.0.1:{server.server_port}/tiles/{{}}/10/10?hv=0'
        try:
            with patch.object(mapserver, 'source_for', return_value=mapserver.HYBRID), \
                    patch.object(mapserver, 'is_online', return_value=True), \
                    patch.object(mapserver, 'read_tile', return_value=None), \
                    patch.object(mapserver, 'fetch_tile', return_value=b'\xff\xd8plain') as fetch, \
                    patch.object(mapserver, 'hybrid_service', side_effect=AssertionError('rendered')):
                with urllib.request.urlopen(base.format(mapserver.HYBRID_PREVIEW_MIN_ZOOM - 1)) as response:
                    self.assertEqual(response.read(), b'\xff\xd8plain')
                fetch.assert_called_once_with(mapserver.HYBRID, mapserver.HYBRID_PREVIEW_MIN_ZOOM - 1, 10, 10)
                with self.assertRaises(urllib.error.HTTPError):          # at the limit it renders (here: fails)
                    urllib.request.urlopen(base.format(mapserver.HYBRID_PREVIEW_MIN_ZOOM))
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_failed_probe_tolerated_after_recent_success(self):
        """A failed probe keeps the server online while other requests recently succeeded."""
        with patch.object(mapserver, 'online_ok', False), patch.object(mapserver, 'online_last_ok', float('-inf')):
            self.assertFalse(mapserver.record_probe(False))
            mapserver.mark_online()
            self.assertTrue(mapserver.is_online())
            self.assertTrue(mapserver.record_probe(False))
            with patch.object(mapserver.time, 'monotonic', return_value=time.monotonic() + mapserver.ONLINE_GRACE + 1):
                self.assertFalse(mapserver.record_probe(False))
            self.assertTrue(mapserver.record_probe(True))

    def test_generated_downloads_run_in_parallel(self):
        """Shaded tiles download several at a time and are all stored; plain sources stay sequential."""
        import contextlib
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            for name, value in (('MAPS_DIR', directory), ('LANDMARKS_DB', directory + '/l.db'),
                                ('ELEVATION_DB', directory + '/e.db')):
                stack.enter_context(patch.object(mapserver, name, value))
            for name, value in (('fetch_landmarks', {'elements': []}), ('save_config', None), ('log_cache_summary', None)):
                stack.enter_context(patch.object(mapserver, name, return_value=value))
            stack.enter_context(patch.dict(mapserver.config['map'], {'elevation': '0'}))
            threads, active, peak, lock = set(), [0], [0], threading.Lock()
            def slow(basemap, z, x, y, contrast=None):
                """Stand-in fetch taking 0.2 s and recording concurrency."""
                with lock:
                    threads.add(threading.get_ident()); active[0] += 1; peak[0] = max(peak[0], active[0])
                time.sleep(0.2)
                with lock:
                    active[0] -= 1
                return b'\xff\xd8' + bytes([x % 256, y % 256])
            stack.enter_context(patch.object(mapserver, 'fetch_tile', side_effect=slow))
            north, west = mapserver.num2deg(400, 400, 17)
            south, east = mapserver.num2deg(404, 406, 17)
            bounds = (north - 1e-7, south + 1e-7, east - 1e-7, west + 1e-7)
            started = time.monotonic()
            mapserver.download_worker('shaded-test', [17], [mapserver.SHADED], *bounds)
            took = time.monotonic() - started
            self.assertEqual(mapserver.dl_status['state'], 'done', mapserver.dl_status)
            self.assertEqual((mapserver.dl_status['done'], mapserver.dl_status['failed']), (24, 0))
            self.assertLess(took, 24 * 0.2 / 3)                       # well under a sequential run
            self.assertEqual(peak[0], mapserver.DOWNLOAD_WORKERS)
            self.assertEqual(mapserver.read_tile('shaded-test', 17, 403, 402), b'\xff\xd8' + bytes([403 % 256, 402 % 256]))
            peak[0] = 0
            mapserver.download_worker('plain-test', [17], ['Satellite'], *bounds)
            self.assertEqual(peak[0], 1)

    def test_malformed_style_settings(self):
        """Invalid stored style JSON falls back to defaults instead of breaking /status."""
        self.assertEqual(mapserver.hybrid_style('not json'), DEFAULTS)
        self.assertEqual(mapserver.hybrid_style('[1]'), DEFAULTS)
        self.assertEqual(mapserver.hybrid_style('{"roads": "yes", "cities": false}'), dict(DEFAULTS, cities=False))
        alpha = DEFAULTS['contour_alpha']
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": 150}')['contour_alpha'], alpha)
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": "50"}')['contour_alpha'], alpha)
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": true}')['contour_alpha'], alpha)
        self.assertEqual(mapserver.hybrid_style('{"contour_alpha": 50}')['contour_alpha'], 50)
        self.assertEqual(mapserver.hybrid_style('{"shade_relief": 21}')['shade_relief'], 5)
        self.assertEqual(mapserver.hybrid_style('{"shade_relief": 12, "shade_dark": 70}')['shade_dark'], 70)
        self.assertEqual(mapserver.hybrid_style('{"shade_source": "esri"}')['shade_source'], 'esri')
        self.assertEqual(mapserver.hybrid_style('{"shade_source": "other"}')['shade_source'], DEFAULTS['shade_source'])
        self.assertEqual(mapserver.hybrid_style('{"esri_contrast": 6}')['esri_contrast'], 3)
        esri = dict(DEFAULTS, hillshade=True, shade_source='esri')
        self.assertIn(mapserver.ESRI_HILLSHADE_CREDIT, mapserver.credits(esri)[mapserver.HYBRID])
        self.assertNotIn(mapserver.ESRI_HILLSHADE_CREDIT, mapserver.credits(STYLE)[mapserver.HYBRID])

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
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            root = Path(directory)
            vector, maps = smoke().fixtures(root)
            engine = HybridService(root / 'cache', FONTS.parents[1], lambda *args: 1 / 0)
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
            session = engine.prepare(options, mapserver.hybrid_satellite(options))
            expected = session.tile(17, 400, 400)
            mapserver.download_worker('hybrid-test', [17], [mapserver.HYBRID], *bounds, hybrid=options)
            self.assertEqual(mapserver.dl_status['state'], 'done', mapserver.dl_status)
            self.assertEqual(mapserver.read_tile('hybrid-test', 17, 400, 400), expected)
            metadata = mapserver.pack_metadata('hybrid-test')
            self.assertEqual((metadata['complete'], metadata['failed_tiles'], metadata['format']), ('1', '0', 'jpg'))
            self.assertEqual(metadata['protomaps_build'], 'local:vectors.pmtiles')
            self.assertEqual(metadata['attribution'], mapserver.HYBRID_CREDIT)
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


if __name__ == '__main__':
    unittest.main()
