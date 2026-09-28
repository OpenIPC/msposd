"""Bounded regional extraction and reading of Protomaps v4 vector data."""

from collections import OrderedDict
import gzip
import functools
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import tarfile
import threading
import time
import urllib.request
import weakref
import zipfile

MANIFEST = "https://build-metadata.protomaps.dev/builds.json"
MAX_CACHE = 1024 * 1024 * 1024
MAX_EXTRACT_SECONDS = 600
PLANET_MAXZOOM = 15          # highest zoom published in Protomaps v4 builds
GROUP = 4                    # output tiles per render-group side
PAD = 2                      # neighbouring output tiles read around each group

PMTILES_VERSION = "1.31.2"
PMTILES_ASSETS = {
    ("Linux", "x86_64"): ("go-pmtiles_1.31.2_Linux_x86_64.tar.gz", "3ed7dbf4ec2e6dfe5e25b6f70d1ffc932729f93c86db353bf514dd71010a312f"),
    ("Linux", "arm64"): ("go-pmtiles_1.31.2_Linux_arm64.tar.gz", "f8bd47e7ea866863489cad588fbaf2f31f42e5821f7a03f009b3769f05801cb1"),
    ("Darwin", "x86_64"): ("go-pmtiles-1.31.2_Darwin_x86_64.zip", "1f0dc02eee6c58312dd6c509faee1b5c32f0596568af1bf51f1b034e7a88a65b"),
    ("Darwin", "arm64"): ("go-pmtiles-1.31.2_Darwin_arm64.zip", "40528f7f616fcbf91207cd48c8fc023d213f6d86c0cbf1f748732803d1880f3d"),
    ("Windows", "x86_64"): ("go-pmtiles_1.31.2_Windows_x86_64.zip", "a658baa4d7e55020aef6ca17bd9ff9faa1582671266b36f58c52db0ac8e785a1"),
    ("Windows", "arm64"): ("go-pmtiles_1.31.2_Windows_arm64.zip", "8780a17453c63af757917a694cbbb50b943db89cc3f1b07e6fd62c1ff8e6963b"),
}


class Cancelled(RuntimeError):
    """Raised when a superseded preview extraction is stopped."""


def install_extractor(directory):
    """Download and verify the official pmtiles CLI into directory.

    directory: Writable folder receiving the executable and its LICENSE.
    Returns: Path of the installed executable. Raises on unsupported platforms or bad downloads.
    """
    arch = platform.machine().lower()
    arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(arch, arch)
    asset = PMTILES_ASSETS.get((platform.system(), arch))
    if asset is None:
        raise RuntimeError(f"No pmtiles extractor for {platform.system()} {arch}; "
                           "set protomaps_source to a local Protomaps archive")
    name, digest = asset
    url = f"https://github.com/protomaps/go-pmtiles/releases/download/v{PMTILES_VERSION}/{name}"
    with urllib.request.urlopen(url, timeout=90) as response:
        data = response.read(100 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("pmtiles archive checksum mismatch")
    if name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            files = [(Path(n).name, archive.read(n)) for n in archive.namelist()
                     if Path(n).name in ("pmtiles", "pmtiles.exe", "LICENSE")]
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            files = [(Path(m.name).name, archive.extractfile(m).read())
                     for m in archive.getmembers() if m.isfile()
                     and Path(m.name).name in ("pmtiles", "LICENSE")]
    target = Path(directory)
    target.mkdir(parents=True, exist_ok=True)
    for filename, content in files:
        part = target / (filename + ".part")
        part.write_bytes(content)
        if filename.startswith("pmtiles"):
            os.chmod(part, 0o755)
        os.replace(part, target / filename)
    return target / ("pmtiles.exe" if os.name == "nt" else "pmtiles")


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


def group_bounds(z, gx0, gy0, gx1, gy1):
    """Return W/S/E/N vector coverage needed to render groups gx0..gx1, gy0..gy1 at z.

    Includes the PAD-tile label buffer and is clamped at the poles and dateline:
    geometry wrapping across ±180° is not extracted (documented limitation).
    """
    n = 2 ** z
    return bounds_for_tiles(z, max(0, gx0 - PAD), max(0, gy0 - PAD),
                            min(n, gx1 + GROUP + PAD), min(n, gy1 + GROUP + PAD))


def padded_bounds(north, south, east, west, z):
    """Return extraction bounds covering the bbox and buffered render groups at z."""
    n = 2 ** z
    def row(lat):
        """Return fractional XYZ row for latitude lat at the enclosing zoom."""
        r = math.radians(max(-85.05112878, min(85.05112878, lat)))
        return (1 - math.asinh(math.tan(r)) / math.pi) / 2 * n
    def group(tile):
        """Return the aligned group origin containing fractional tile index tile."""
        return min(n - 1, math.floor(tile)) // GROUP * GROUP
    return group_bounds(z, group((west + 180) / 360 * n), group(row(north)),
                        group((east + 180) / 360 * n), group(row(south)))


def contains(outer, inner):
    """Return whether W/S/E/N rectangle outer contains rectangle inner (1e-9° tolerance)."""
    e = 1e-9
    return (outer[0] <= inner[0] + e and outer[1] <= inner[1] + e and
            outer[2] >= inner[2] - e and outer[3] >= inner[3] - e)


class VectorSource:
    """Immutable local archive with bounded decoded-tile caching."""

    def __init__(self, path, identity, coverage=None):
        """Open archive path, labelled by public identity (build URL or local name).

        coverage: Optional (W/S/E/N bounds, maxzoom) of a regional extract; archives
        without it are treated as complete within their header bounds and zooms.
        """
        from pmtiles.reader import Reader
        from pmtiles.tile import TileType
        self.path, self.identity = str(path), identity
        reader = Reader(self._read)
        self.header = reader.header()
        meta = reader.metadata()
        layers = {v['id'] for v in meta.get('vector_layers', [])}
        if self.header['tile_type'] != TileType.MVT or not {'roads', 'places', 'buildings'} <= layers:
            raise ValueError("Expected Protomaps v4 roads, places and buildings")
        version = str(meta.get('version', ''))
        if version and not version.startswith('4.'):
            raise ValueError(f"Unsupported Protomaps schema {version}")
        self.maxzoom = self.header['max_zoom']
        self.native = self.maxzoom
        self.bounds = tuple(self.header[k] / 1e7 for k in
                            ('min_lon_e7', 'min_lat_e7', 'max_lon_e7', 'max_lat_e7'))
        if coverage is not None:
            self.bounds, self.maxzoom = tuple(coverage[0]), min(self.maxzoom, coverage[1])
            self.native = PLANET_MAXZOOM
        self.tile = functools.lru_cache(maxsize=96)(self._tile)

    def covers(self, bounds, z):
        """Return whether W/S/E/N bounds are available at the source zoom used for output z."""
        return min(z, self.native) <= self.maxzoom and contains(self.bounds, bounds)

    def _read(self, offset, length):
        """Read length bytes at offset in the archive; return bytes or raise on truncation."""
        with open(self.path, 'rb') as f:
            f.seek(offset)
            data = f.read(length)
        if len(data) != length:
            raise ValueError('Truncated vector archive')
        return data

    def _tile(self, z, x, y):
        """Decode source XYZ tile z/x/y; return layer dictionary, empty for absent tiles."""
        from pmtiles.reader import Reader
        from pmtiles.tile import Compression
        data = Reader(self._read).get(z, x, y)
        if data is None:
            return {}
        compression = self.header['tile_compression']
        if compression == Compression.GZIP:
            data = gzip.decompress(data)
        elif compression != Compression.NONE:
            raise ValueError('Unsupported vector compression')
        return decode_tile(data)


class SourceManager:
    """Regional extraction with a bounded, least-recently-used local working cache."""

    def __init__(self, cache_dir, tool_dir):
        """Use writable cache_dir for extracts and tool_dir for the pmtiles executable."""
        self.directory = Path(cache_dir)
        self.executable = Path(tool_dir) / ('pmtiles.exe' if os.name == 'nt' else 'pmtiles')
        self.lock = threading.Lock()           # cache bookkeeping only, never held while extracting
        self.build_lock = threading.Lock()
        self.install_lock = threading.Lock()
        self.build = None
        self.sources = OrderedDict()
        self.live = weakref.WeakSet()          # sources still referenced; never evicted

    def resolve(self):
        """Return the newest Protomaps v4 build URL, looked up once until reset."""
        with self.build_lock:
            if self.build is None:
                with urllib.request.urlopen(urllib.request.Request(MANIFEST, headers={"User-Agent": "msposd-gs-map/1.0"}), timeout=30) as r:
                    builds = json.loads(r.read(4 * 1024 * 1024))
                candidates = [b for b in builds if str(b.get('version', '')).startswith('4.')
                              and re.fullmatch(r'[\w.-]+\.pmtiles', b.get('key', ''))]
                if not candidates:
                    raise ValueError('No compatible Protomaps v4 build available')
                self.build = 'https://build.protomaps.com/' + max(candidates, key=lambda b: b['key'])['key']
            return self.build

    def find(self, configured, bounds, maxzoom):
        """Return cached vectors covering W/S/E/N bounds up to maxzoom, or None.

        configured: Local archive path, mirror URL, or '' for any automatic build.
        Raises ValueError when a configured local archive does not cover the area.
        """
        with self.lock:
            if configured and not configured.startswith(('https://', 'http://')):
                stamp = os.stat(configured)
                key = f'{os.path.abspath(configured)}:{stamp.st_size}:{stamp.st_mtime_ns}'
                vectors = self.sources.get(key) or VectorSource(configured, 'local:' + Path(configured).name)
                self._remember(key, vectors)
                if not vectors.covers(bounds, maxzoom):
                    raise ValueError('Local vector archive does not cover the selected area and label buffer')
                return vectors
            # Sidecar bounds describe extraction coverage, even when the archive header
            # retains the planet bounds. Never infer coverage from a missing vector tile.
            maxzoom = min(PLANET_MAXZOOM, maxzoom)
            for sidecar in self.directory.glob('*.json'):
                try:
                    entry = json.loads(sidecar.read_text())
                    path = sidecar.with_suffix('.pmtiles')
                    wanted = entry['source'] == configured if configured else entry.get('auto')
                    if (wanted and entry['zoom'] >= maxzoom and path.is_file()
                            and contains(entry['bounds'], bounds)):
                        key = str(path)
                        if key not in self.sources:
                            self._remember(key, VectorSource(path, entry['source'],
                                                             (entry['bounds'], entry['zoom'])))
                        os.utime(path)             # mark recently used for eviction
                        self._remember(key, self.sources[key])
                        return self.sources[key]
                except (OSError, ValueError, KeyError):
                    continue
            return None

    def prepare(self, configured, bounds, maxzoom, progress=None, cancel=None):
        """Return local vectors for W/S/E/N bounds and maxzoom, extracting when needed.

        configured: Local archive, mirror URL, or '' to use the current Protomaps build.
        progress: Optional callable receiving status text. cancel: Optional Event stopping extraction.
        """
        vectors = self.find(configured, bounds, maxzoom)
        if vectors is not None:
            return vectors
        source = configured or self.resolve()
        maxzoom = min(PLANET_MAXZOOM, maxzoom)
        if not self.executable.is_file():
            with self.install_lock:
                if not self.executable.is_file():
                    if progress:
                        progress('Downloading the pmtiles extractor…')
                    install_extractor(self.executable.parent)
        entry = {'source': source, 'bounds': list(bounds), 'zoom': maxzoom, 'auto': not configured}
        key = hashlib.sha256(json.dumps(entry, sort_keys=True).encode()).hexdigest()[:24]
        path = self.directory / (key + '.pmtiles')
        part = path.with_suffix('.part')
        logpath = path.with_suffix('.log')
        with self.lock:
            self.directory.mkdir(parents=True, exist_ok=True)
            used = self._evict()
        cmd = [str(self.executable), 'extract', source, str(part),
               '--bbox=' + ','.join(str(v) for v in bounds),
               f'--maxzoom={maxzoom}', '--download-threads=2']
        started = time.monotonic()
        try:
            with open(logpath, 'wb') as log:
                proc = subprocess.Popen(cmd, stdout=log, stderr=log)
                try:
                    while proc.poll() is None:
                        if cancel is not None and cancel.is_set():
                            raise Cancelled('Superseded by a newer preview area')
                        size = part.stat().st_size if part.exists() else 0
                        if size + used > MAX_CACHE or time.monotonic() - started > MAX_EXTRACT_SECONDS:
                            raise RuntimeError('Vector extraction exceeded its disk/time limit')
                        if progress:
                            progress(f'Preparing roads and buildings ({size // 1024} KiB written)')
                        time.sleep(0.2)
                    if proc.returncode:
                        tail = logpath.read_text(errors='replace')[-600:]
                        raise RuntimeError(f'Vector extraction failed: {tail}')
                finally:
                    if proc.poll() is None:
                        proc.kill()
                    proc.wait()
            vectors = VectorSource(part, source, (bounds, maxzoom))
            os.replace(part, path)
            vectors.path = str(path)
            path.with_suffix('.json').write_text(json.dumps(entry))
            with self.lock:
                self._remember(str(path), vectors)
            return vectors
        except Exception as exc:
            if not configured and not isinstance(exc, Cancelled):
                with self.build_lock:
                    self.build = None          # a retired daily build is looked up again
            raise
        finally:
            part.unlink(missing_ok=True)
            logpath.unlink(missing_ok=True)

    def _evict(self):
        """Delete least-recently-used unreferenced extracts until usage is under 75% of MAX_CACHE.

        Caller holds self.lock. Returns bytes still used by extracts and partial downloads.
        Raises RuntimeError when referenced extracts alone keep the cache full.
        """
        live = {v.path for v in self.live}
        files = sorted(self.directory.glob('*.pmtiles'), key=lambda p: p.stat().st_mtime)
        used = sum(p.stat().st_size for p in files) + sum(
            p.stat().st_size for p in self.directory.glob('*.part'))
        for path in files:
            if used <= MAX_CACHE * 3 // 4:
                break
            if str(path) in live:
                continue
            used -= path.stat().st_size
            self.sources.pop(str(path), None)
            path.unlink(missing_ok=True)
            path.with_suffix('.json').unlink(missing_ok=True)
        if used >= MAX_CACHE:
            raise RuntimeError('Vector cache is full of areas in use; close previews and retry')
        return used

    def _remember(self, key, source):
        """Retain source under key among the four most recent; track it as in use. Caller holds lock."""
        self.sources[key] = source
        self.sources.move_to_end(key)
        self.live.add(source)
        while len(self.sources) > 4:
            self.sources.popitem(last=False)


class TerrainCache:
    """Terrarium elevation tiles kept in a bounded least-recently-used disk cache."""

    MAX_FILES = 1500             # about 150 MB of terrarium PNG tiles

    def __init__(self, cache_dir, fetch):
        """Use writable cache_dir; fetch(z, x, y) returns terrarium PNG bytes or raises."""
        self.directory, self.fetch = Path(cache_dir), fetch
        self.lock = threading.Lock()
        self.tile = functools.lru_cache(maxsize=16)(self._tile)

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
