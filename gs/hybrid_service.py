"""Shared hybrid rendering sessions for browser previews and download jobs."""

from collections import OrderedDict
import hashlib
import json
import os
from pathlib import Path
import threading

from hybrid_render import STYLE_VERSION, compose_tile, render_overlay, shade_mask
from protomaps_source import GROUP, LocalArchive, RemoteArchive, TerrainCache, TileCache, latest_build

MAX_OVERLAYS = 8          # at least mapserver.DOWNLOAD_WORKERS, so interleaved groups are not re-rendered


class HybridSession:
    """One immutable style/imagery snapshot with a bounded cache of rendered group overlays."""

    def __init__(self, options, vectors, terrain, fonts, satellite, hillshade=None):
        """Store frozen options, vector archive, terrain cache, font folder and satellite(z, x, y) callback.

        hillshade(z, x, y): Returns (image bytes, crop) of Esri hillshade for the 'esri' shade source.
        """
        self.options, self.vectors, self.terrain = options, vectors, terrain
        self.fonts, self.satellite, self.hillshade = fonts, satellite, hillshade
        self.lock = threading.Lock()
        self.overlays = OrderedDict()
        self.rendering = {}

    def _overlay(self, z, gx, gy):
        """Return the overlay for a group, rendering it at most once concurrently."""
        key = (z, gx, gy)
        with self.lock:
            if key in self.overlays:
                self.overlays.move_to_end(key)
                return self.overlays[key]
            gate = self.rendering.setdefault(key, threading.Lock())
        with gate:
            try:
                with self.lock:
                    if key in self.overlays:
                        return self.overlays[key]
                # Rendering fetches its vector and elevation tiles, so several groups at once
                # overlap those network waits.
                overlay = render_overlay(self.vectors, z, gx, gy, self.options['style'], self.fonts,
                                         self.terrain)
                with self.lock:
                    self.overlays[key] = overlay
                    while len(self.overlays) > MAX_OVERLAYS:
                        self.overlays.popitem(last=False)
                return overlay
            finally:
                with self.lock:
                    self.rendering.pop(key, None)

    def tile(self, z, x, y):
        """Return composed JPEG for XYZ z/x/y; raise when vectors or imagery are unavailable."""
        if not (0 <= x < 2 ** z and 0 <= y < 2 ** z):
            raise ValueError('Hybrid tile outside the world')
        gx, gy = x // GROUP * GROUP, y // GROUP * GROUP
        overlay = self._overlay(z, gx, gy)
        raw = self.satellite(z, x, y)
        if raw is None:
            raise ValueError('Satellite tile unavailable in selected pack and network')
        style, mask = self.options['style'], None
        if style['hillshade'] and style['shade_source'] == 'esri':
            if self.hillshade is None:
                raise ValueError('Esri hillshade is not available')
            mask = shade_mask(*self.hillshade(z, x, y), style['esri_contrast'])
        return compose_tile(overlay, gx, gy, x, y, raw, mask)


class HybridService:
    """Vector archives, terrain and preview sessions shared by the whole server."""

    def __init__(self, cache_dir, resource_dir, fetch_range, terrain_fetch=None, hillshade=None):
        """Use writable cache_dir and bundled resource_dir fonts.

        fetch_range(url, offset, length) reads remote archives; terrain_fetch(z, x, y)
        returns terrarium PNG bytes for hillshade and contours, or None to disable them;
        hillshade(z, x, y) returns (Esri hillshade bytes, crop) for the 'esri' shade source.
        """
        self.hillshade = hillshade
        self.cache = TileCache(Path(cache_dir) / 'vectors.db')
        self.fetch_range = fetch_range
        self.terrain = TerrainCache(Path(cache_dir) / 'terrain', terrain_fetch) if terrain_fetch else None
        self.fonts = Path(resource_dir) / 'assets' / 'fonts'
        self.lock = threading.Lock()
        self.archives = OrderedDict()
        self.sessions = OrderedDict()

    def archive(self, source):
        """Return the shared archive for configured source: a local path, a URL, or '' for the current build."""
        if source and not source.startswith(('https://', 'http://')):
            stat = os.stat(source)
            key = f'{os.path.abspath(source)}:{stat.st_size}:{stat.st_mtime_ns}'
        else:
            key = source
        with self.lock:
            if key in self.archives:
                self.archives.move_to_end(key)
                return self.archives[key]
        if key == source:
            archive = RemoteArchive(source or latest_build(self.cache), self.fetch_range, self.cache, not source)
        else:
            archive = LocalArchive(source)
        with self.lock:
            self.archives[key] = archive
            while len(self.archives) > 2:
                self.archives.popitem(last=False)
        return archive

    def prepare(self, options, satellite):
        """Return a private session for frozen options and imagery callback, as used by downloads."""
        return HybridSession(options, self.archive(options['source']), self.terrain, self.fonts, satellite,
                             self.hillshade)

    def tile(self, options, z, x, y, satellite):
        """Return a preview JPEG for options at XYZ z/x/y, sharing one session per style snapshot."""
        identity = hashlib.sha256(json.dumps([STYLE_VERSION, options], sort_keys=True).encode()).hexdigest()
        with self.lock:
            session = self.sessions.get(identity)
        if session is None:
            session = self.prepare(options, satellite)
            with self.lock:
                self.sessions[identity] = session
                while len(self.sessions) > 2:
                    self.sessions.popitem(last=False)
        return session.tile(z, x, y)
