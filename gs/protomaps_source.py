"""Reading Protomaps v4 vector data from local or remote PMTiles archives, with tile caching."""

from concurrent.futures import ThreadPoolExecutor
import functools
import gzip
import io
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import struct
import threading
import time
import urllib.request

MANIFEST = "https://build-metadata.protomaps.dev/builds.json"
MAX_CACHE = 1024 * 1024 * 1024   # vector tile cache bound in bytes
BUILD_MAX_AGE = 7 * 86400        # reuse a cached automatic build this long before checking for newer
PLANET_MAXZOOM = 15              # highest zoom published in Protomaps v4 builds
GROUP = 4                        # output tiles per render-group side
PAD = 2                          # neighbouring output tiles read around each group
FETCH_THREADS = 4
CHUNK_GAP = 64 * 1024            # bytes of unneeded data worth fetching to join two tiles in one request
CHUNK_MAX = 2 * 1024 * 1024      # largest single range request


def _fields(buf):
    """Yield (field number, wire type, value) from protobuf bytes buf.

    Varints yield ints; length-delimited and fixed-width fields yield bytes.
    """
    i, n = 0, len(buf)
    def varint():
        """Return the varint at the current offset and advance past it."""
        nonlocal i
        shift = result = 0
        while True:
            b = buf[i]
            i += 1
            result |= (b & 0x7F) << shift
            if b < 0x80:
                return result
            shift += 7
    while i < n:
        key = varint()
        wire = key & 7
        if wire == 0:
            value = varint()
        elif wire == 2:
            size = varint()
            value, i = buf[i:i + size], i + size
        elif wire in (1, 5):
            size = 8 if wire == 1 else 4
            value, i = buf[i:i + size], i + size
        else:
            raise ValueError("Unsupported protobuf wire type")
        yield key >> 3, wire, value


def _packed(wire, value):
    """Return the uint32 list held by a packed (wire 2) or single varint field."""
    if wire == 0:
        return [value]
    out, i = [], 0
    while i < len(value):
        shift = result = 0
        while True:
            b = value[i]
            i += 1
            result |= (b & 0x7F) << shift
            if b < 0x80:
                break
            shift += 7
        out.append(result)
    return out


def _value(buf):
    """Decode one MVT Value message buf into a Python scalar."""
    for field, _, value in _fields(buf):
        if field == 1:
            return value.decode("utf-8", "replace")
        if field == 2:
            return struct.unpack("<f", value)[0]
        if field == 3:
            return struct.unpack("<d", value)[0]
        if field == 4:
            return value - (1 << 64) if value >= 1 << 63 else value
        if field == 5:
            return value
        if field == 6:
            return (value >> 1) ^ -(value & 1)
        if field == 7:
            return bool(value)
    return None


def _geometry(commands):
    """Decode MVT geometry command integers into parts (lists of (x, y) points)."""
    parts, x, y, i = [], 0, 0, 0
    while i < len(commands):
        command, count = commands[i] & 7, commands[i] >> 3
        i += 1
        if command == 7:                       # ClosePath: rings are closed when drawn
            continue
        for _ in range(count):
            dx, dy = commands[i], commands[i + 1]
            i += 2
            x += (dx >> 1) ^ -(dx & 1)
            y += (dy >> 1) ^ -(dy & 1)
            if command == 1:                   # every MoveTo point starts a new part
                parts.append([])
            parts[-1].append((x, y))
    return parts


def decode_tile(data):
    """Decode an uncompressed Mapbox Vector Tile.

    data: MVT protobuf bytes.
    Returns: {layer: {'extent', 'features': [{'id', 'properties', 'type', 'parts'}]}},
    with y pointing down, type 1/2/3 for point/line/polygon, and polygon rings as parts.
    """
    layers = {}
    for field, _, layer_bytes in _fields(data):
        if field != 3:
            continue
        name, extent, keys, values, raw = '', 4096, [], [], []
        for f, wire, value in _fields(layer_bytes):
            if f == 1:
                name = value.decode("utf-8", "replace")
            elif f == 2:
                raw.append(value)
            elif f == 3:
                keys.append(value.decode("utf-8", "replace"))
            elif f == 4:
                values.append(_value(value))
            elif f == 5:
                extent = value
        features = []
        for feature_bytes in raw:
            feature = {'id': 0, 'properties': {}, 'type': 0, 'parts': []}
            for f, wire, value in _fields(feature_bytes):
                if f == 1:
                    feature['id'] = value
                elif f == 2:
                    tags = _packed(wire, value)
                    feature['properties'].update(
                        (keys[tags[k]], values[tags[k + 1]]) for k in range(0, len(tags) - 1, 2))
                elif f == 3:
                    feature['type'] = value
                elif f == 4:
                    feature['parts'] = _geometry(_packed(wire, value))
            features.append(feature)
        layers[name] = {'extent': extent, 'features': features}
    return layers


def bounds_for_tiles(z, left, top, right, bottom):
    """Return west/south/east/north for exclusive XYZ bounds at zoom z."""
    n = 2 ** z
    def latitude(y):
        """Return latitude in degrees for tile row y at the enclosing zoom."""
        return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return (left / n * 360 - 180, latitude(bottom),
            right / n * 360 - 180, latitude(top))


class TileCache:
    """SQLite store of fetched vector tiles and archive headers, bounded by size.

    Rows record the tile bytes as served (b'' for a tile absent from the archive), so a
    cached area renders identically offline. The least recently used rows are removed
    once the stored bytes exceed max_bytes; the file is reused rather than shrunk.
    """

    def __init__(self, path, max_bytes=MAX_CACHE):
        """Open or create the cache database at path with the given byte bound."""
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.max_bytes = max_bytes
        self.lock = threading.Lock()
        self.conn = sqlite3.connect(str(path), check_same_thread=False, isolation_level=None)
        self.conn.executescript(
            'PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;'
            'CREATE TABLE IF NOT EXISTS tiles(build TEXT, z INTEGER, x INTEGER, y INTEGER,'
            ' data BLOB, used INTEGER, PRIMARY KEY(build, z, x, y));'
            'CREATE INDEX IF NOT EXISTS tiles_used ON tiles(used);'
            'CREATE TABLE IF NOT EXISTS archives(build TEXT PRIMARY KEY, header BLOB,'
            ' metadata BLOB, auto INTEGER, stored INTEGER)')
        self.writes = 0

    def get(self, build, z, x, y):
        """Return cached tile bytes for build and z/x/y (b'' when known empty), or None if not cached."""
        with self.lock:
            row = self.conn.execute('SELECT data FROM tiles WHERE build=? AND z=? AND x=? AND y=?',
                                    (build, z, x, y)).fetchone()
            if row is not None:
                self.conn.execute('UPDATE tiles SET used=? WHERE build=? AND z=? AND x=? AND y=?',
                                  (time.time_ns(), build, z, x, y))
        return None if row is None else bytes(row[0])

    def has(self, build, z, x, y):
        """Return whether a tile row exists for build and z/x/y, without reading its data."""
        with self.lock:
            return self.conn.execute('SELECT 1 FROM tiles WHERE build=? AND z=? AND x=? AND y=?',
                                     (build, z, x, y)).fetchone() is not None

    def put(self, build, z, x, y, data):
        """Store tile bytes data for build and z/x/y; evict old rows every 200 writes."""
        with self.lock:
            self.conn.execute('INSERT OR REPLACE INTO tiles VALUES(?,?,?,?,?,?)',
                              (build, z, x, y, data, time.time_ns()))
            self.writes += 1
            if self.writes % 200 == 0:
                self.evict()

    def evict(self):
        """Delete least recently used tiles until stored bytes are under 75% of the bound. Caller holds lock."""
        used = self.conn.execute('SELECT coalesce(sum(length(data)), 0) FROM tiles').fetchone()[0]
        if used <= self.max_bytes:
            return
        target = self.max_bytes * 3 // 4
        for rowid, size in self.conn.execute('SELECT rowid, length(data) FROM tiles ORDER BY used').fetchall():
            if used <= target:
                break
            self.conn.execute('DELETE FROM tiles WHERE rowid=?', (rowid,))
            used -= size

    def archive(self, build):
        """Return stored (header bytes, metadata bytes) for build, or None."""
        with self.lock:
            row = self.conn.execute('SELECT header, metadata FROM archives WHERE build=?', (build,)).fetchone()
        return None if row is None else (bytes(row[0]), bytes(row[1]))

    def remember(self, build, header, metadata, auto):
        """Store header and metadata bytes for build; auto marks an automatically chosen build."""
        with self.lock:
            self.conn.execute('INSERT OR REPLACE INTO archives VALUES(?,?,?,?,?)',
                              (build, header, metadata, int(auto), int(time.time())))

    def latest_build(self, max_age=None):
        """Return the newest automatically chosen build URL stored within max_age seconds, or None."""
        with self.lock:
            row = self.conn.execute('SELECT build, stored FROM archives WHERE auto=1 ORDER BY stored DESC LIMIT 1').fetchone()
        if row is None or (max_age is not None and time.time() - row[1] > max_age):
            return None
        return row[0]


def latest_build(cache):
    """Return the Protomaps v4 build URL to use.

    cache: TileCache whose recent automatic build is reused (BUILD_MAX_AGE), so daily builds
    do not force re-downloads. Otherwise the newest build in the published manifest; when
    that is unreachable, any cached automatic build is used.
    """
    cached = cache.latest_build(BUILD_MAX_AGE)
    if cached:
        return cached
    try:
        request = urllib.request.Request(MANIFEST, headers={"User-Agent": "msposd-gs-map/1.0"})
        with urllib.request.urlopen(request, timeout=30) as r:
            builds = json.loads(r.read(4 * 1024 * 1024))
    except OSError:
        fallback = cache.latest_build()
        if fallback:
            return fallback
        raise
    candidates = [b for b in builds if str(b.get('version', '')).startswith('4.')
                  and re.fullmatch(r'[\w.-]+\.pmtiles', b.get('key', ''))]
    if not candidates:
        raise ValueError('No compatible Protomaps v4 build available')
    return 'https://build.protomaps.com/' + max(candidates, key=lambda b: b['key'])['key']


class Archive:
    """Protomaps v4 archive read through read(offset, length), with cached directories."""

    def _open(self, header, metadata, identity):
        """Validate header and metadata bytes; set identity, maxzoom and compression."""
        from pmtiles.tile import Compression, TileType, deserialize_header
        self.identity = identity
        self.header = deserialize_header(header)
        if self.header['internal_compression'] == Compression.GZIP:
            metadata = gzip.decompress(metadata)
        meta = json.loads(metadata)
        layers = {v['id'] for v in meta.get('vector_layers', [])}
        if self.header['tile_type'] != TileType.MVT or not {'roads', 'places', 'buildings'} <= layers:
            raise ValueError("Expected Protomaps v4 roads, places and buildings")
        version = str(meta.get('version', ''))
        if version and not version.startswith('4.'):
            raise ValueError(f"Unsupported Protomaps schema {version}")
        self.maxzoom = self.header['max_zoom']
        self.directory = functools.lru_cache(maxsize=64)(self._directory)
        self.tile = functools.lru_cache(maxsize=96)(self._tile)
        self.download_bytes = 0

    def _directory(self, offset, length):
        """Return the decoded directory stored at offset/length."""
        from pmtiles.tile import deserialize_directory
        return deserialize_directory(self.read(offset, length))

    def locate(self, z, x, y):
        """Return (offset, length) of tile z/x/y in the archive, or None when absent."""
        from pmtiles.tile import find_tile, zxy_to_tileid
        tile_id = zxy_to_tileid(z, x, y)
        h = self.header
        offset, length = h['root_offset'], h['root_length']
        for _ in range(4):
            entry = find_tile(self.directory(offset, length), tile_id)
            if entry is None:
                return None
            if entry.run_length:
                return h['tile_data_offset'] + entry.offset, entry.length
            offset, length = h['leaf_directory_offset'] + entry.offset, entry.length
        return None

    def raw(self, z, x, y):
        """Return the stored (still compressed) bytes of tile z/x/y, or b'' when absent."""
        where = self.locate(z, x, y)
        return self.read(*where) if where else b''

    def decode(self, data):
        """Return decoded layers for stored tile bytes data; empty for an absent tile."""
        from pmtiles.tile import Compression
        if not data:
            return {}
        compression = self.header['tile_compression']
        if compression == Compression.GZIP:
            data = gzip.decompress(data)
        elif compression != Compression.NONE:
            raise ValueError('Unsupported vector compression')
        return decode_tile(data)

    def _tile(self, z, x, y):
        """Return decoded layers of tile z/x/y."""
        return self.decode(self.raw(z, x, y))

    def prefetch(self, keys):
        """Make (z, x, y) keys ready for tile(); local archives need nothing."""


class LocalArchive(Archive):
    """Archive read from a file on disk."""

    def __init__(self, path):
        """Open archive file path; identity is 'local:<file name>'."""
        self.path = str(path)
        header = self.read(0, 127)
        from pmtiles.tile import deserialize_header
        h = deserialize_header(header)
        self._open(header, self.read(h['metadata_offset'], h['metadata_length']), 'local:' + Path(path).name)

    def read(self, offset, length):
        """Read length bytes at offset; raise on truncation."""
        with open(self.path, 'rb') as f:
            f.seek(offset)
            data = f.read(length)
        if len(data) != length:
            raise ValueError('Truncated vector archive')
        return data


class RemoteArchive(Archive):
    """Archive read over HTTP range requests, with every tile cached locally.

    Cached tiles (and the archive header) are served without network access, so an area
    seen once renders offline; an uncached tile while offline raises from fetch_range.
    """

    def __init__(self, url, fetch_range, cache, auto=False):
        """Open url via fetch_range(url, offset, length); cache is a TileCache; auto marks a manifest build."""
        self.url, self.fetch_range, self.cache = url, fetch_range, cache
        stored = cache.archive(url)
        if stored is None:
            header = self.read(0, 127)
            from pmtiles.tile import deserialize_header
            h = deserialize_header(header)
            stored = (header, self.read(h['metadata_offset'], h['metadata_length']))
            cache.remember(url, *stored, auto)
        self._open(*stored, url)
        self.pool = ThreadPoolExecutor(max_workers=FETCH_THREADS, thread_name_prefix='vector-fetch')
        self.count_lock = threading.Lock()

    def read(self, offset, length):
        """Return length bytes at offset of the archive URL."""
        return self.fetch_range(self.url, offset, length)

    def _tile(self, z, x, y):
        """Return decoded layers of tile z/x/y from the cache, fetching and storing it when missing."""
        data = self.cache.get(self.url, z, x, y)
        if data is None:
            data = self.raw(z, x, y)
            self._store(z, x, y, data)
        return self.decode(data)

    def _store(self, z, x, y, data):
        """Cache tile bytes data for z/x/y and count them as downloaded."""
        with self.count_lock:
            self.download_bytes += len(data)
        self.cache.put(self.url, z, x, y, data)

    def prefetch(self, keys):
        """Fetch uncached (z, x, y) keys, merging neighbouring tiles into few parallel range requests.

        Tiles are stored in spatial order, so a group's tiles are nearly contiguous; runs
        separated by less than CHUNK_GAP bytes share one request. The first failure propagates.
        """
        located = []
        for key in keys:
            if self.cache.has(self.url, *key):
                continue
            where = self.locate(*key)
            if where is None:
                self._store(*key, b'')
            else:
                located.append((*where, key))
        chunks = []
        for offset, length, key in sorted(located):
            if chunks and offset - chunks[-1][0] + length <= CHUNK_MAX and offset - chunks[-1][1] <= CHUNK_GAP:
                chunks[-1][1] = max(chunks[-1][1], offset + length)
                chunks[-1][2].append((offset, length, key))
            else:
                chunks.append([offset, offset + length, [(offset, length, key)]])

        def fetch(chunk):
            """Read one merged range and store every tile inside it."""
            start, end, members = chunk
            data = self.read(start, end - start)
            for offset, length, key in members:
                self._store(*key, data[offset - start:offset - start + length])
        list(self.pool.map(fetch, chunks))


class TerrainCache:
    """Terrarium elevation tiles kept in a bounded least-recently-used disk cache."""

    MAX_FILES = 1500             # about 150 MB of terrarium PNG tiles

    def __init__(self, cache_dir, fetch):
        """Use writable cache_dir; fetch(z, x, y) returns terrarium PNG bytes or raises."""
        self.directory, self.fetch = Path(cache_dir), fetch
        self.lock = threading.Lock()
        self.tile = functools.lru_cache(maxsize=16)(self._tile)
        self.pool = ThreadPoolExecutor(max_workers=FETCH_THREADS, thread_name_prefix='terrain-fetch')

    def prefetch(self, keys):
        """Fetch and decode (z, x, y) keys not yet on disk in parallel; the first failure propagates."""
        missing = [k for k in keys if not (self.directory / '{}_{}_{}.png'.format(*k)).exists()]
        if missing:
            list(self.pool.map(lambda k: self.tile(*k), missing))

    def _tile(self, z, x, y):
        """Return elevations in metres for terrarium tile z/x/y as a flat 256 x 256 row-major list."""
        from PIL import Image
        path = self.directory / f'{z}_{x}_{y}.png'
        try:
            data = path.read_bytes()
            os.utime(path)
        except OSError:
            data = self.fetch(z, x, y)
            with self.lock:
                self.directory.mkdir(parents=True, exist_ok=True)
                part = path.with_suffix('.part')
                part.write_bytes(data)
                os.replace(part, path)
                files = sorted(self.directory.glob('*.png'), key=lambda p: p.stat().st_mtime)
                for old in files[:max(0, len(files) - self.MAX_FILES)]:
                    old.unlink(missing_ok=True)
        with Image.open(io.BytesIO(data)) as image:
            if image.size != (256, 256):
                raise ValueError('Terrain tiles must be 256 x 256')
            pixels = image.convert('RGB').getdata()
        return [r * 256 + g + b / 256 - 32768 for r, g, b in pixels]
