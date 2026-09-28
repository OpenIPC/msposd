"""Shared hybrid preparation and raster rendering for browser and download jobs."""

from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import threading

from hybrid_render import STYLE_VERSION, compose_tile, render_overlay
from protomaps_source import GROUP, Cancelled, SourceManager, TerrainCache, contains, group_bounds, padded_bounds

RENDER_SLOTS = threading.BoundedSemaphore(2)
MAX_OVERLAYS = 8
MAX_AREAS = 6


class HybridSession:
    """One immutable style/imagery snapshot drawing from prepared vector areas."""

    def __init__(self, options, fonts, satellite, vectors=(), terrain=None):
        """Store frozen options, font folder, satellite(z, x, y) callback, initial vector areas and terrain."""
        self.options, self.fonts, self.satellite, self.terrain = options, fonts, satellite, terrain
        self.vectors = list(vectors)
        self.lock = threading.Lock()
        self.overlays = OrderedDict()
        self.rendering = {}

    def add(self, vectors):
        """Make a newly prepared vector area available, keeping the most recent MAX_AREAS."""
        with self.lock:
            if vectors not in self.vectors:
                self.vectors.append(vectors)
                del self.vectors[:-MAX_AREAS]

    def covers(self, bounds, z):
        """Return whether any prepared area supplies W/S/E/N bounds at output zoom z."""
        with self.lock:
            return any(v.covers(bounds, z) for v in self.vectors)

    def _overlay(self, z, gx, gy):
        """Return (covered, overlay) for a group, rendering it at most once concurrently."""
        key = (z, gx, gy)
        with self.lock:
            if key in self.overlays:
                self.overlays.move_to_end(key)
                return True, self.overlays[key]
            gate = self.rendering.setdefault(key, threading.Lock())
        with gate:
            try:
                with self.lock:
                    if key in self.overlays:
                        return True, self.overlays[key]
                    needed = group_bounds(z, gx, gy, gx, gy)
                    vectors = next((v for v in reversed(self.vectors) if v.covers(needed, z)), None)
                if vectors is None:
                    return False, None
                with RENDER_SLOTS:
                    overlay = render_overlay(vectors, z, gx, gy, self.options['style'], self.fonts,
                                             self.terrain)
                with self.lock:
                    self.overlays[key] = overlay
                    while len(self.overlays) > MAX_OVERLAYS:
                        self.overlays.popitem(last=False)
                return True, overlay
            finally:
                with self.lock:
                    self.rendering.pop(key, None)

    def tile(self, z, x, y):
        """Return composed JPEG for XYZ z/x/y, or None when its vectors are not prepared.

        Raises ValueError when imagery is unavailable; network errors propagate.
        """
        if not (0 <= x < 2 ** z and 0 <= y < 2 ** z):
            raise ValueError('Hybrid tile outside the world')
        gx, gy = x // GROUP * GROUP, y // GROUP * GROUP
        covered, overlay = self._overlay(z, gx, gy)
        if not covered:
            return None
        raw = self.satellite(z, x, y)
        if raw is None:
            raise ValueError('Satellite tile unavailable in selected pack and network')
        return compose_tile(overlay, gx, gy, x, y, raw)


class PreviewJob:
    """Background vector preparation for one preview session and area."""

    def __init__(self, identity, bounds, zoom, future, cancel):
        """Store session identity, W/S/E/N bounds, source zoom, future and cancel Event."""
        self.identity, self.bounds, self.zoom = identity, bounds, zoom
        self.future, self.cancel = future, cancel


class HybridService:
    """Preview sessions keyed by style snapshot, and shared download preparation."""

    def __init__(self, cache_dir, resource_dir, tool_dir, terrain_fetch=None):
        """Use writable cache_dir, bundled resource_dir fonts and tool_dir for the extractor.

        terrain_fetch: Optional callable(z, x, y) returning terrarium PNG bytes for hillshading.
        """
        self.manager = SourceManager(Path(cache_dir) / 'vectors', tool_dir)
        self.terrain = TerrainCache(Path(cache_dir) / 'terrain', terrain_fetch) if terrain_fetch else None
        self.fonts = Path(resource_dir) / 'assets' / 'fonts'
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix='hybrid-preview')
        self.lock = threading.Lock()
        self.sessions = OrderedDict()
        self.job = None

    def prepare(self, options, bounds, zooms, satellite, progress=None):
        """Return a download session for frozen options, N/S/E/W bounds, zooms and imagery callback."""
        padded = [padded_bounds(*bounds, z) for z in zooms]
        union = (min(b[0] for b in padded), min(b[1] for b in padded),
                 max(b[2] for b in padded), max(b[3] for b in padded))
        vectors = self.manager.prepare(options['source'], union, max(zooms), progress)
        return HybridSession(options, self.fonts, satellite, [vectors], self.terrain)

    def preview(self, options, bounds, z, satellite):
        """Ensure vectors for the N/S/E/W viewport at zoom z; return {'id', 'state'[, 'message']}.

        The id depends only on options, so panning inside prepared areas keeps it.
        satellite: Imagery callback used when a new session is created.
        """
        identity = hashlib.sha256(json.dumps([STYLE_VERSION, options], sort_keys=True).encode()).hexdigest()[:24]
        needed = padded_bounds(*bounds, z)
        with self.lock:
            session = self.sessions.get(identity)
            if session is None:
                session = self.sessions[identity] = HybridSession(options, self.fonts, satellite,
                                                                  terrain=self.terrain)
            self.sessions.move_to_end(identity)
            while len(self.sessions) > 2:
                self.sessions.popitem(last=False)
        if session.covers(needed, z):
            return {'id': identity, 'state': 'ready'}
        try:
            vectors = self.manager.find(options['source'], needed, z)
        except (OSError, ValueError) as exc:
            return {'id': identity, 'state': 'error', 'message': str(exc)}
        if vectors is not None:
            session.add(vectors)
            return {'id': identity, 'state': 'ready'}
        with self.lock:
            job = self.job
            if job is not None and not job.future.done():
                if job.identity == identity and job.zoom >= z and contains(job.bounds, needed):
                    return {'id': identity, 'state': 'loading'}
                job.cancel.set()
                job.future.cancel()
            self.job = PreviewJob(identity, needed, z, None, threading.Event())
            self.job.future = self.executor.submit(self._prepare_preview, session, options,
                                                   needed, z, self.job.cancel)
        return {'id': identity, 'state': 'loading'}

    def _prepare_preview(self, session, options, bounds, z, cancel):
        """Extract W/S/E/N bounds at zoom z for session unless cancel is set; return nothing."""
        if cancel.is_set():
            raise Cancelled('Superseded by a newer preview area')
        session.add(self.manager.prepare(options['source'], bounds, z, cancel=cancel))

    def status(self, identity):
        """Return JSON-ready preparation state for preview session identity."""
        with self.lock:
            session = self.sessions.get(identity)
            job = self.job if self.job is not None and self.job.identity == identity else None
        if session is None:
            return {'state': 'expired'}
        if job is not None:
            if not job.future.done():
                return {'state': 'loading', 'message': 'Preparing roads and buildings…'}
            exc = None if job.future.cancelled() else job.future.exception()
            if exc is not None and not isinstance(exc, Cancelled):
                return {'state': 'error', 'message': str(exc)}
        with session.lock:
            paths = [v.path for v in session.vectors]
        return {'state': 'ready', 'vector_bytes': sum(Path(p).stat().st_size for p in paths)}

    def tile(self, identity, z, x, y):
        """Return preview JPEG for session identity at XYZ z/x/y, or None when not prepared."""
        with self.lock:
            session = self.sessions.get(identity)
        return None if session is None else session.tile(z, x, y)
