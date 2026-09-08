#!/usr/bin/env python3
"""Offline moving-map bridge for the Ground Station (x86 PoC).

Single stdlib-only process that:
  * listens for MSP forwarded by `msposd --out 127.0.0.1:14560`,
    extracts aircraft lat/lon/heading/course,
  * serves the Leaflet viewer and a hybrid tile endpoint: offline MBTiles when a tile is
    cached, else a live proxy to OSM when online (so the user can browse to find a place),
  * downloads the currently-viewed area into the MBTiles for offline use,
  * an SSE position feed and a small settings endpoint.

See documentation/offline-map-overlay-spec.md.
"""

import http.client
import json
import math
import os
import re
import signal
import socket
import sqlite3
import ssl
import sys
import struct
import tempfile
import threading
import time
import urllib.request
import zipfile
import zlib
from configparser import ConfigParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, urljoin

HERE = os.path.dirname(os.path.abspath(__file__))

# Standalone (PyInstaller) awareness. When frozen into a single binary, bundled
# read-only assets (web/) are unpacked to sys._MEIPASS, while writable data
# (config.ini, maps/, state.ini, landmarks.db) must live next to the executable
# so it survives restarts. In normal script mode both are just HERE, so the dev
# workflow (mapserver.py + mapwin) is unchanged.
_FROZEN = getattr(sys, "frozen", False)
APP_DIR = os.path.dirname(sys.executable) if _FROZEN else HERE  # writable data root
RES_DIR = getattr(sys, "_MEIPASS", HERE)                        # bundled read-only assets

CONFIG_PATH = os.path.join(APP_DIR, "config.ini")
# Offline storage keeps three levels: the chosen detail zoom plus two coarser
# fallbacks, two steps apart. Detail zoom is user-selectable ([map] detail_zoom);
# each step up quadruples the tiles a given area needs, so the area that fits in
# MAX_TILES shrinks accordingly (~95 km square at z15, ~12 km at z18). The panel
# labels each level with its ground resolution (~3.6 m/pixel at z15).
DETAIL_MIN, DETAIL_MAX = 12, 18
DETAIL_DEFAULT = 15
BROWSE_MIN, BROWSE_MAX = 2, 18  # live-proxy browse range
MAX_TILES = 12000              # refuse offline downloads larger than this


def zoom_set(detail):
    """The three stored zoom levels for a detail zoom: [detail-4, detail-2, detail]."""
    return [detail - 4, detail - 2, detail]


def clamp_detail(v):
    """Clamp a detail-zoom value into [DETAIL_MIN, DETAIL_MAX]. Takes no lock, so
    it is safe to call from code already holding config_lock."""
    try:
        return max(DETAIL_MIN, min(DETAIL_MAX, int(v)))
    except (TypeError, ValueError):
        return DETAIL_DEFAULT


def detail_zoom():
    """The configured detail (highest stored) zoom, clamped to the allowed range."""
    with config_lock:
        return clamp_detail(config["map"].get("detail_zoom", DETAIL_DEFAULT))


def zooms():
    """The zoom levels currently stored for offline / flight use, ascending."""
    return zoom_set(detail_zoom())

# Selectable basemaps — all keyless and OK for app/proxy use (unlike OSM's volunteer
# servers, which 403 bulk/proxy traffic). ESRI uses {z}/{y}/{x} ordering; the OSM-style
# sources use {z}/{x}/{y} and may include {s} for a/b/c subdomain rotation (see
# _fmt_tile). The Esri layers serve JPEG and the OSM-derived ones PNG; both render in the
# browser and on the OSD (see BASEMAP_FORMAT / OSD_TILE_FORMATS below).
# You are responsible for each provider's usage terms and attribution.
BASEMAPS = {
    "Satellite": "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    "Streets": "https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}",
    "Topo": "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}",
    "OpenTopoMap": "https://a.tile.opentopomap.org/{z}/{x}/{y}.png",
    # keyed provider: {key} is filled from [server] tile_key in the gitignored
    # config.ini, so no API key is committed to source.
    "Thunderforest": "https://api.thunderforest.com/outdoors/{z}/{x}/{y}.png?apikey={key}",
}
# NB: volunteer OSM servers (CyclOSM, OSM-Humanitarian, tile.openstreetmap.org) are
# deliberately NOT listed — they throttle/block app/proxy/bulk traffic and make the
# preview go blank mid-browse. Use custom sources via [server] tile_url at your own risk.

# Tile format each basemap serves (verified against the live endpoints).
BASEMAP_FORMAT = {
    "Satellite": "JPEG",
    "Streets": "JPEG",
    "Topo": "JPEG",
    "OpenTopoMap": "PNG",
    "Thunderforest": "PNG",
}
# Formats the native in-OSD renderer can decode: PNG via cairo's own reader, JPEG
# via the vendored stb_image decoder in osd/util/map_render.c. Every basemap here
# renders both in this browser UI and on the OSD in flight, so nothing is greyed
# out for format reasons; a basemap serving something else would be.
OSD_TILE_FORMATS = {"PNG", "JPEG"}


def basemap_issues(tile_key):
    """Why each basemap can't be selected, keyed by name.

    tile_key: the configured API key ("" if none).
    Returns: {basemap: short reason} for unusable basemaps; usable ones absent.
    """
    issues = {}
    for name, url in BASEMAPS.items():
        fmt = BASEMAP_FORMAT.get(name)
        if "{key}" in url and not tile_key:
            issues[name] = "needs key"
        elif fmt not in OSD_TILE_FORMATS:
            issues[name] = f"{fmt or '?'} — not on OSD"
    return issues

DEFAULTS = {
    "server": {
        "port": "8088",
        "udp_listen": "127.0.0.1:14560",
        "mbtiles": "./maps/area.mbtiles",
        "web_root": "./web",
        # used only when [map] basemap is not one of BASEMAPS (custom source)
        "tile_url": BASEMAPS["Satellite"],
        "tile_delay_ms": "60",
        # API key for keyed basemaps (fills {key} in the tile URL, e.g. Thunderforest)
        "tile_key": "",
    },
    "map": {
        "zoom": "15",
        # highest stored zoom; the offline set is [detail-4, detail-2, detail]
        "detail_zoom": str(DETAIL_DEFAULT),
        # per-zoom sources, one per stored level, coarse -> detail. Empty means
        # "use `basemap` for every level" (the simple, single-source case).
        "sources": "",
        "basemap": "Satellite",
        # download terrain elevation for the same area (separate maps/elevation.db)
        "elevation": "1",
        "center_lat": "", "center_lon": "",
    },
}


def current_tile_url():
    """Return the {z}/{x}/{y} tile URL template for the active basemap.

    Returns: the URL string from BASEMAPS, or the custom [server] tile_url
    when the configured basemap is not a known BASEMAPS entry.
    """
    with config_lock:
        bm = config["map"].get("basemap", "Satellite")
        custom = config["server"]["tile_url"]
    return BASEMAPS.get(bm, custom)

SUBDOMAINS = "abc"          # hosts to rotate through for tile URLs that use {s}

def _fmt_tile(template, z, x, y):
    """Fill a {z}/{x}/{y} tile-URL template. Also substitutes {s} (a/b/c subdomain
    rotation) and {key} (the provider API key from [server] tile_key, kept in the
    gitignored config.ini). Templates without those fields simply ignore them."""
    s = SUBDOMAINS[(x + y) % len(SUBDOMAINS)]
    with config_lock:
        key = config["server"].get("tile_key", "")
    return template.format(s=s, z=z, x=x, y=y, key=key)

USER_AGENT = "msposd-gs-map/0.1 (+https://github.com/OpenIPC/msposd)"


def tile_ctype(data):
    """Guess a tile's MIME type from its magic bytes.

    data: raw tile bytes.
    Returns: "image/jpeg" for a JPEG SOI marker, else "image/png".
    """
    return "image/jpeg" if data[:2] == b"\xff\xd8" else "image/png"


def load_config():
    """Load config.ini layered over DEFAULTS.

    Returns: a ConfigParser seeded with DEFAULTS then overlaid with any
    values found in CONFIG_PATH.
    """
    cfg = ConfigParser()
    cfg.read_dict(DEFAULTS)
    cfg.read(CONFIG_PATH)
    return cfg


def save_config():
    """Write the in-memory config back to CONFIG_PATH, holding config_lock."""
    with config_lock:
        with open(CONFIG_PATH, "w") as fh:
            config.write(fh)


config = load_config()
config_lock = threading.Lock()
db_lock = threading.Lock()   # serialize all SQLite access (one writer + many readers, same file)

# web/ is a bundled read-only asset (RES_DIR); tile caches are writable (APP_DIR).
WEB_ROOT = os.path.normpath(os.path.join(RES_DIR, config["server"]["web_root"]))
# Per-basemap caches live in the maps/ folder as <basemap>.mbtiles, so switching
# basemaps offline serves the right tiles instead of a mix.
MAPS_DIR = os.path.dirname(os.path.normpath(os.path.join(APP_DIR, config["server"]["mbtiles"])))
LANDMARKS_DB = os.path.join(MAPS_DIR, "landmarks.db")
landmarks_db_lock = threading.Lock()

# Terrain elevation, kept in its own DB because it does not depend on the basemap.
# Source is the AWS Open Data "terrarium" set: ordinary XYZ PNG tiles whose pixels
# encode metres rather than colour, elev = (R*256 + G + B/256) - 32768.
ELEVATION_DB = os.path.join(MAPS_DIR, "elevation.db")
elevation_db_lock = threading.Lock()
DEM_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
# z12 is ~28 m/pixel at 43°N, which matches the ~30 m native posting of the
# underlying SRTM-derived data. Higher zooms only interpolate: measured on three
# summits, z10 through z15 return the same metre values. Storing above z12 costs
# 4x per level and adds no information.
DEM_ZOOM = 12
# Each z12 tile is a fixed 256*256 int16 = 128 KiB, so the DEM area needs its own
# ceiling: at detail z12 a MAX_TILES download spans ~11250 z12 tiles = 1.5 GB.
# 1000 tiles is ~228x228 km / 131 MB — wide enough that a line-of-sight horizon
# runs into terrain rather than into the edge of the downloaded area.
MAX_DEM_TILES = 1000
DEM_NODATA = -32768


def current_basemap():
    """Return the active basemap name from config (default "Satellite")."""
    with config_lock:
        return config["map"].get("basemap", "Satellite")


def parse_sources(raw, fallback, n):
    """Parse a "src,src,src" list into exactly n valid basemap names.

    Lock-free, so it is safe to call while holding config_lock. Anything missing,
    unknown or short falls back to `fallback`, which keeps configs written before
    per-zoom sources existed (and hand-edited ones) working unchanged.
    """
    out = [s.strip() for s in (raw or "").split(",") if s.strip()]
    out = [s for s in out if s in BASEMAPS]
    if len(out) < n:
        out += [fallback] * (n - len(out))
    return out[:n]


def sources():
    """Per-zoom basemap names, aligned with zooms() (ascending, coarse -> detail)."""
    with config_lock:
        m = config["map"]
        detail = clamp_detail(m.get("detail_zoom", DETAIL_DEFAULT))
        return parse_sources(m.get("sources", ""), m.get("basemap", "Satellite"),
                             len(zoom_set(detail)))


def source_for(z, zs=None, srcs=None):
    """The basemap to use at zoom z: the source of the nearest stored level.

    Browsing ranges over BROWSE_MIN..BROWSE_MAX while only len(zooms()) levels are
    stored, so preview zooms between/outside them snap to the closest stored level
    -- the same rule nearestStoredZoom() uses in the viewer.
    """
    zs = zs if zs is not None else zooms()
    srcs = srcs if srcs is not None else sources()
    best = min(range(len(zs)), key=lambda i: abs(zs[i] - z))
    return srcs[best]


def pack_id(srcs=None):
    """Build the configured preview id and unnamed-download fallback.

    srcs: Optional source snapshot; defaults to the configured zoom sources.
    Returns: Distinct source names joined in ascending-zoom order. A uniform mix
    remains its plain basemap name for compatibility with existing caches.
    """
    srcs = srcs if srcs is not None else sources()
    seen = []
    for s in srcs:
        if s not in seen:
            seen.append(s)
    return "+".join(seen)


def valid_pack(name):
    """True if name is a pack id: one or more known basemaps joined by '+'.

    Guards the ?src= query param so it can only ever name a pack this server could
    itself have written (mbtiles_for also sanitises the path).
    """
    return bool(name) and all(p in BASEMAPS for p in name.split("+"))


def mbtiles_for(basemap):
    """Build the MBTiles cache path for a basemap.

    basemap: basemap name (non-alphanumeric chars are sanitised to '_').
    Returns: absolute path to <MAPS_DIR>/<safe-name>.mbtiles.
    """
    safe = re.sub(r"[^A-Za-z0-9_-]", "_", basemap or "Satellite")
    return os.path.join(MAPS_DIR, safe + ".mbtiles")


def pack_name_base(name, srcs=None):
    """Normalize an optional user map name into a portable filename stem.

    name: Requested display/file name; an optional .mbtiles suffix is removed.
    srcs: Source list used for the fallback name when name is empty.
    Returns: Non-empty ASCII stem containing only letters, digits, '_' and '-'.
    """
    raw = name.strip() if isinstance(name, str) else ""
    if raw.lower().endswith(".mbtiles"):
        raw = raw[:-8]
    if not raw:
        raw = pack_id(srcs)
    safe = re.sub(r"[^A-Za-z0-9_-]+", "_", raw).strip("_-")[:80].rstrip("_-")
    if not safe:
        safe = re.sub(r"[^A-Za-z0-9_-]+", "_", pack_id(srcs)).strip("_-") or "map"
    # These basenames are reserved even with an extension on Windows.
    reserved = {"CON", "PRN", "AUX", "NUL",
                *(f"COM{i}" for i in range(1, 10)),
                *(f"LPT{i}" for i in range(1, 10))}
    if safe.upper() in reserved:
        safe = "map_" + safe
    return safe


def allocate_pack_id(name, srcs=None, timestamp=None):
    """Choose a new pack id without overwriting an existing filesystem entry.

    name: Optional requested map name.
    srcs: Source list used when the requested name is empty.
    timestamp: Optional Unix timestamp for deterministic tests; defaults to now.
    Returns: Available filename stem, timestamped only when the base already exists.
    """
    base = pack_name_base(name, srcs)
    if not os.path.lexists(mbtiles_for(base)):
        return base
    stamp = time.strftime("%Y%m%d_%H%M%S", time.gmtime(
        time.time() if timestamp is None else timestamp))
    candidate = f"{base}_{stamp}"
    serial = 2
    while os.path.lexists(mbtiles_for(candidate)):
        candidate = f"{base}_{stamp}_{serial}"
        serial += 1
    return candidate


def downloaded_pack_exists(name):
    """Check whether a safe pack id names a regular MBTiles file in MAPS_DIR.

    name: Filename stem supplied by the preflight inventory UI.
    Returns: True only for a non-symlink file contained directly in MAPS_DIR.
    """
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", name):
        return False
    path = mbtiles_for(name)
    root = os.path.realpath(MAPS_DIR)
    return (os.path.dirname(os.path.realpath(path)) == root and
            os.path.isfile(path) and not os.path.islink(path))


def resolve_pack(name):
    """Resolve a query-string pack name without permitting arbitrary paths.

    name: Logical configured pack id or downloaded filename stem.
    Returns: A pack id accepted by mbtiles_for(), or None when invalid.
    """
    if valid_pack(name):
        return name
    if downloaded_pack_exists(name):
        return name
    return None

STATIC_WHITELIST = {
    "viewer.html": "text/html; charset=utf-8",
    "leaflet.js": "application/javascript",
    "leaflet.css": "text/css",
    "icons/plane.svg": "image/svg+xml",
}

# ---------------------------------------------------------------------------
# Telemetry state
# ---------------------------------------------------------------------------

state_lock = threading.Lock()
latest = {"lat": None, "lon": None, "heading": 0, "course": 0, "fix": 0, "sats": 0}
state_seq = 0


def valid_coord(lat, lon):
    """Validate a GPS fix.

    lat, lon: decimal degrees (may be None).
    Returns: True if both are in range and not the 0,0 null-island fix.
    """
    return (
        lat is not None and lon is not None
        and -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0
        and not (abs(lat) < 1e-7 and abs(lon) < 1e-7)
    )


def update_state(**kw):
    """Merge telemetry fields into `latest` and bump state_seq for SSE.

    kw: telemetry keys to update (lat, lon, heading, course, fix, sats).
    """
    global state_seq
    with state_lock:
        latest.update(kw)
        state_seq += 1


MSP_RAW_GPS = 106
MSP_ATTITUDE = 108
MSP_CMD_STATUS = 101
MSP_CMD_STATUS_EX = 150

# ---------------------------------------------------------------------------
# Shared home/target state.
#   home   : captured live from GPS on the OSD station at arm; station-local
#            runtime, kept in state.ini (also read by msposd.c via ini_parser).
#   targets: up to TARGET_SLOTS preflight-authored named points, stored in
#            landmarks.db `waypoints` so they travel with the POIs as part of the
#            map pack (see osd/util/poi_osd.c). Slot 0 keeps the historical
#            kind='target' row, so packs and ground stations predating the extra
#            slots keep working unchanged.
# ---------------------------------------------------------------------------

STATE_PATH = os.path.join(APP_DIR, "state.ini")
state_io_lock = threading.Lock()
TARGET_SLOTS = 5


def target_kind(i):
    """DB `kind` for slot i. Slot 0 stays 'target' for backward compatibility."""
    return "target" if i == 0 else "target%d" % (i + 1)


# targets: list of TARGET_SLOTS entries, each None or {"name","lat","lon"}
geo = {"targets": [None] * TARGET_SLOTS, "home": None}
armed_state = False
seen_disarmed = False                  # gates home capture (see on_status)
home_pending = False


def load_targets_from_db():
    """Return the target slots from landmarks.db as a TARGET_SLOTS-long list."""
    out = [None] * TARGET_SLOTS
    try:
        with landmarks_db_lock:
            conn = open_landmarks_db()
            rows = dict((r[0], (r[1], r[2], r[3])) for r in conn.execute(
                "SELECT kind, lat, lon, name FROM waypoints"))
            conn.close()
    except sqlite3.Error:
        return out
    for i in range(TARGET_SLOTS):
        r = rows.get(target_kind(i))
        if r:
            out[i] = {"lat": r[0], "lon": r[1], "name": r[2] or ""}
    return out


def clip_name(s, maxbytes=31):
    """Trim a waypoint name to fit the C side's fixed buffer without splitting a
    character. poi_osd.c reads these into char[32] and snprintf() cuts on bytes,
    so slicing by characters here could hand it invalid UTF-8 (any non-ASCII name
    is ~2 bytes per character)."""
    b = str(s).encode("utf-8")[:maxbytes]
    while b:
        try:
            return b.decode("utf-8")
        except UnicodeDecodeError:
            b = b[:-1]        # dropped a continuation byte; step back a byte
    return ""


def save_targets_to_db(slots):
    """Persist the target slots to landmarks.db `waypoints`.

    slots: TARGET_SLOTS-long list; each entry {"name","lat","lon"} or None.
    An empty slot deletes its row, so clearing a target really removes it from
    the pack rather than leaving a stale point for the OSD to draw.
    """
    with landmarks_db_lock:
        conn = open_landmarks_db()
        for i, t in enumerate(slots):
            kind = target_kind(i)
            if t:
                conn.execute(
                    "INSERT OR REPLACE INTO waypoints(kind, lat, lon, name) VALUES(?,?,?,?)",
                    (kind, float(t["lat"]), float(t["lon"]), clip_name(t.get("name", ""))))
            else:
                conn.execute("DELETE FROM waypoints WHERE kind=?", (kind,))
        conn.commit()
        conn.close()


def load_geo_state():
    """Load home from state.ini and the target from landmarks.db into `geo`.

    Home is read only when its `set=1`; malformed values become None. The target
    lives in the DB now, but a legacy [target] in state.ini is honoured as a
    one-time migration fallback and copied into the DB.
    """
    cfg = ConfigParser()
    cfg.read(STATE_PATH)

    def pt(sec):
        """Read a [sec] lat/lon point; (lat, lon) when set=1 and parseable, else None."""
        if cfg.has_section(sec) and cfg.get(sec, "set", fallback="0") == "1":
            try:
                return (cfg.getfloat(sec, "lat"), cfg.getfloat(sec, "lon"))
            except ValueError:
                return None
        return None

    geo["home"] = pt("home")

    geo["targets"] = load_targets_from_db()
    if geo["targets"][0] is None:                   # migration: adopt legacy state.ini target
        legacy = pt("target")
        if legacy:
            geo["targets"][0] = {"lat": legacy[0], "lon": legacy[1], "name": ""}
            save_targets_to_db(geo["targets"])


def save_geo_state():
    """Atomically write the home point to state.ini (targets live in the DB).

    Uses a temp file + os.replace so a concurrent C-side read (msposd.c)
    never observes a half-written file.
    """
    cfg = ConfigParser()
    cfg.add_section("home")
    v = geo["home"]
    cfg.set("home", "set", "1" if v else "0")
    cfg.set("home", "lat", f"{v[0]:.7f}" if v else "")
    cfg.set("home", "lon", f"{v[1]:.7f}" if v else "")
    tmp = STATE_PATH + ".tmp"
    with state_io_lock:                 # atomic write so a C read never sees half a file
        with open(tmp, "w") as fh:
            cfg.write(fh)
        os.replace(tmp, STATE_PATH)


def on_status(now_armed):
    """Track armed state and arm a one-shot home capture on rising edge.

    now_armed: armed flag from the latest MSP status frame.
    Home is captured only on a witnessed disarmed->armed transition, so a
    mid-flight restart keeps the persisted home instead of recapturing.
    """
    # Capture home only on a real disarmed->armed transition we actually witnessed.
    # If the server restarts mid-flight the first frame is already armed (no witnessed
    # disarm), so we keep the persisted home instead of recapturing at the wrong spot.
    global armed_state, seen_disarmed, home_pending
    armed_edge = disarmed_edge = False
    with state_lock:
        if not now_armed and armed_state:
            disarmed_edge = True
        if not now_armed:
            seen_disarmed = True
        rising = now_armed and not armed_state
        armed_state = now_armed
        if rising and seen_disarmed:
            home_pending = True
            armed_edge = True
    if disarmed_edge:
        print("[mapserver] DISARMED")
    if armed_edge:
        print("[mapserver] ARMED — waiting for GPS fix to capture home")
    maybe_capture_home()


def maybe_capture_home():
    """Capture home at the current position once a GPS fix arrives.

    No-op unless a home capture is pending (armed edge seen) and a valid fix
    exists. On success, stores home in `geo` and persists state.ini.
    """
    global home_pending
    pos = None
    with state_lock:
        if home_pending and latest["fix"] and latest["lat"] is not None:
            pos = (latest["lat"], latest["lon"])
            home_pending = False
    if pos:
        geo["home"] = pos
        save_geo_state()
        print(f"[mapserver] HOME captured: lat={pos[0]:.6f} lon={pos[1]:.6f}")


def parse_msp_frames(buf):
    """Yield (cmd, payload) for each CRC-valid MSP v1 frame in a datagram.

    buf: raw bytes possibly containing several '$M' framed messages.
    Yields: (command_id, payload_bytes) tuples; bad frames are skipped.
    """
    i, n = 0, len(buf)
    while i + 6 <= n:
        if buf[i] != 0x24 or buf[i + 1] != 0x4D:
            i += 1
            continue
        length, cmd = buf[i + 3], buf[i + 4]
        end = i + 5 + length
        if end >= n:
            break
        payload = buf[i + 5:end]
        crc = length ^ cmd
        for b in payload:
            crc ^= b
        if crc == buf[end]:
            yield cmd, payload
            i = end + 1
        else:
            i += 1


def handle_msp(cmd, p):
    """Decode one MSP frame and update telemetry/armed state.

    cmd: MSP command id. p: payload bytes.
    Handles RAW_GPS (position/course), ATTITUDE (heading) and STATUS (armed).
    """
    if cmd == MSP_RAW_GPS and len(p) >= 16:
        lat = struct.unpack_from("<i", p, 2)[0] / 1e7
        lon = struct.unpack_from("<i", p, 6)[0] / 1e7
        if not valid_coord(lat, lon):
            lat, lon = None, None
        update_state(
            fix=p[0], sats=p[1],
            lat=lat, lon=lon,
            course=struct.unpack_from("<h", p, 14)[0] / 10.0,
        )
        maybe_capture_home()
    elif cmd == MSP_ATTITUDE and len(p) >= 6:
        update_state(heading=struct.unpack_from("<h", p, 4)[0])
    elif cmd in (MSP_CMD_STATUS, MSP_CMD_STATUS_EX) and len(p) >= 7:
        on_status(bool(p[6] & 0x01))


def udp_listener():
    """Bind the configured UDP socket and forward MSP frames forever.

    Runs in a daemon thread; each datagram is parsed and dispatched to
    handle_msp. Blocks indefinitely on recvfrom.
    """
    host, port = config["server"]["udp_listen"].split(":")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, int(port)))
    print(f"[mapserver] listening for MSP on udp://{host}:{port}")
    while True:
        data, _ = sock.recvfrom(4096)
        for cmd, payload in parse_msp_frames(data):
            handle_msp(cmd, payload)


# ---------------------------------------------------------------------------
# Connectivity probe (so the UI can show online/offline and we avoid slow
# proxy timeouts when offline)
# ---------------------------------------------------------------------------

online_lock = threading.Lock()
online_ok = False


def online_check_loop():
    """Probe the active tile server every 15s and update `online_ok`.

    Runs in a daemon thread so the UI can show online/offline and the tile
    handler can skip slow proxy timeouts when offline.
    """
    global online_ok
    while True:
        probe = _fmt_tile(current_tile_url(), 0, 0, 0)
        ok = False
        try:
            req = urllib.request.Request(probe, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=5) as r:
                r.read(1)
            ok = True
        except Exception:
            ok = False
        with online_lock:
            online_ok = ok
        time.sleep(15)


def is_online():
    """Return the last connectivity-probe result (True if online)."""
    with online_lock:
        return online_ok


# ---------------------------------------------------------------------------
# Tile math + MBTiles
# ---------------------------------------------------------------------------


def deg2num(lat, lon, z):
    """Convert lat/lon to a Slippy-map tile X/Y at zoom z.

    lat, lon: decimal degrees. z: zoom level.
    Returns: (x, y) tile indices clamped to the valid 0..2^z-1 range.
    """
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def num2deg(x, y, z):
    """Convert a Slippy-map tile X/Y at zoom z to its NW-corner lat/lon.

    x, y: tile indices. z: zoom level.
    Returns: (lat, lon) of the tile's north-west corner.
    """
    n = 1 << z
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return lat, x / n * 360.0 - 180.0


def bbox_tile_ranges(north, south, east, west, z):
    """Compute the tile X and Y index ranges covering a bbox at zoom z.

    north/south/east/west: bbox edges in degrees. z: zoom level.
    Returns: (xrange, yrange) of XYZ tile indices spanning the box.
    """
    x0, _ = deg2num(north, west, z)
    x1, _ = deg2num(north, east, z)
    _, y0 = deg2num(north, west, z)  # north -> smaller y
    _, y1 = deg2num(south, west, z)
    return range(min(x0, x1), max(x0, x1) + 1), range(min(y0, y1), max(y0, y1) + 1)


def plan_total(north, south, east, west, levels=None):
    """Count tiles a bbox download would cover across all stored zooms.

    north/south/east/west: bbox edges in degrees.
    levels: Optional zoom-level snapshot; defaults to the current configuration.
    Returns: Total tile count summed over the selected zoom levels.
    """
    total = 0
    for z in levels if levels is not None else zooms():
        xs, ys = bbox_tile_ranges(north, south, east, west, z)
        total += len(xs) * len(ys)
    return total


def open_mbtiles(basemap, write=False):
    """Open (and, when writing, create) a basemap's MBTiles database.

    basemap: basemap name. write: True to create schema and open read-write,
    False to open read-only.
    Returns: an sqlite3 connection with a 10s busy timeout.
    """
    path = mbtiles_for(basemap)
    if write:
        os.makedirs(MAPS_DIR, exist_ok=True)
        conn = sqlite3.connect(path, timeout=10)
        conn.execute("PRAGMA busy_timeout=10000")
        conn.execute(
            "CREATE TABLE IF NOT EXISTS tiles("
            "zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_data BLOB)"
        )
        conn.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS tile_index "
            "ON tiles(zoom_level, tile_column, tile_row)"
        )
        conn.execute("CREATE TABLE IF NOT EXISTS metadata(name TEXT, value TEXT)")
        return conn
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=10)
    conn.execute("PRAGMA busy_timeout=10000")
    return conn


def read_tile(basemap, z, x, y):
    """Read one tile from a basemap's offline MBTiles cache.

    basemap: basemap name. z/x/y: XYZ tile coordinates (converted to TMS).
    Returns: tile bytes, or None if the cache or tile is absent.
    """
    if not os.path.exists(mbtiles_for(basemap)):
        return None
    ymbt = (1 << z) - 1 - y
    with db_lock:
        conn = open_mbtiles(basemap, write=False)
        try:
            row = conn.execute(
                "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                (z, x, ymbt),
            ).fetchone()
            return row[0] if row else None
        finally:
            conn.close()


# Kept-alive tile connections, one per (scheme, host) and per thread.
#
# A fresh TLS handshake per tile costs more than the tile transfer itself, so
# reusing the connection is the single biggest download speedup — and it lowers
# load on the tile server too, which matters for the volunteer-run ones.
#
# Thread-local rather than a shared pool: http.client connections are not
# thread-safe, and the download worker and the HTTP handler threads both fetch.
# Each thread's connections are closed when it exits and the local dies.
_tile_conns = threading.local()
_TILE_TIMEOUT = 15
_REDIRECTS = (301, 302, 303, 307, 308)


def _tile_conn(scheme, host):
    """Return a kept-alive connection to (scheme, host) for the calling thread."""
    pool = getattr(_tile_conns, "pool", None)
    if pool is None:
        pool = _tile_conns.pool = {}
    conn = pool.get((scheme, host))
    if conn is None:
        conn = (http.client.HTTPSConnection(host, timeout=_TILE_TIMEOUT,
                                            context=ssl.create_default_context())
                if scheme == "https"
                else http.client.HTTPConnection(host, timeout=_TILE_TIMEOUT))
        pool[(scheme, host)] = conn
        return conn, True                       # freshly opened
    return conn, False                          # reused, may be stale


def _drop_tile_conn(scheme, host):
    """Discard a connection that failed or that the server asked to close."""
    pool = getattr(_tile_conns, "pool", None)
    if not pool:
        return
    conn = pool.pop((scheme, host), None)
    if conn is not None:
        try:
            conn.close()
        except Exception:
            pass


def _tile_get(url, hops=3):
    """GET a tile URL over a kept-alive connection, following redirects.

    Returns: response body bytes. Raises on network or non-200 HTTP status.
    """
    u = urlparse(url)
    scheme, host = u.scheme or "https", u.netloc
    target = u.path + (f"?{u.query}" if u.query else "")
    # One retry: a pooled connection the server closed while idle fails on its
    # next use, which is normal and must not surface as a tile error.
    for _ in range(2):
        conn, fresh = _tile_conn(scheme, host)
        try:
            conn.request("GET", target, headers={"User-Agent": USER_AGENT,
                                                 "Accept": "image/*,*/*"})
            r = conn.getresponse()
            body = r.read()                     # always drain, or the socket is unusable
        except (OSError, http.client.HTTPException):
            _drop_tile_conn(scheme, host)
            if fresh:
                raise                           # a real failure, not a stale socket
            continue
        if r.will_close or r.getheader("Connection", "").lower() == "close":
            _drop_tile_conn(scheme, host)
        if r.status in _REDIRECTS:
            loc = r.getheader("Location")
            if not loc or hops <= 0:
                raise OSError(f"tile redirect loop or missing Location ({r.status})")
            return _tile_get(urljoin(url, loc), hops - 1)
        if r.status != 200:
            raise OSError(f"tile HTTP {r.status}")
        return body
    raise OSError("tile connection lost")


def fetch_tile(basemap, z, x, y):
    """Fetch one tile live from a basemap's remote tile server.

    basemap: basemap name. z/x/y: XYZ tile coordinates.
    Returns: tile bytes. Raises on network/HTTP error.
    """
    url = _fmt_tile(BASEMAPS.get(basemap, config["server"]["tile_url"]), z, x, y)
    return _tile_get(url)


def coverage_in_bbox(basemap, z, north, south, east, west):
    """Return present XYZ (x, y) tiles for a basemap within a bbox at zoom z."""
    path = mbtiles_for(basemap)
    if not os.path.exists(path):
        return []
    xs, ys = bbox_tile_ranges(north, south, east, west, z)
    ymax = (1 << z) - 1
    rmin, rmax = ymax - (ys.stop - 1), ymax - ys.start   # XYZ y range -> TMS row range
    with db_lock:
        conn = open_mbtiles(basemap, write=False)
        try:
            rows = conn.execute(
                "SELECT tile_column, tile_row FROM tiles WHERE zoom_level=? "
                "AND tile_column BETWEEN ? AND ? AND tile_row BETWEEN ? AND ?",
                (z, xs.start, xs.stop - 1, rmin, rmax),
            ).fetchall()
        finally:
            conn.close()
    return [[c, ymax - r] for c, r in rows]    # back to XYZ y


def box_km(nlat, wlon, slat, elon):
    """Ground size of a lat/lon box in km (x = east-west, y = north-south).

    Uses the spherical approximation at the box's mid-latitude, which is well
    within the accuracy needed to judge "does my flight fit in this area".
    """
    km_y = (nlat - slat) * 111.32
    km_x = (elon - wlon) * 111.32 * math.cos(math.radians((nlat + slat) / 2))
    return abs(km_x), abs(km_y)


def cache_summary(basemap):
    """Per-zoom tile counts and covered box for a basemap's offline cache.

    basemap: Safe configured or downloaded pack id.
    Returns: JSON-ready pack metadata, or an empty summary when absent.
    Also reports the cache size on disk and the stored tile format (PNG and JPEG
    both render, in the browser and on the OSD). Raises ValueError when tile
    coordinates or zooms are not valid MBTiles values.
    """
    path = mbtiles_for(basemap)
    if not os.path.exists(path):
        return {"basemap": basemap, "total": 0, "zooms": []}
    with db_lock:
        conn = open_mbtiles(basemap, write=False)
        try:
            # substr() keeps this to the leading signature instead of loading a
            # whole tile blob per level.
            rows = conn.execute(
                "SELECT zoom_level, COUNT(*), MIN(tile_column), MAX(tile_column), "
                "MIN(tile_row), MAX(tile_row), substr(tile_data, 1, 8) "
                "FROM tiles GROUP BY zoom_level ORDER BY zoom_level"
            ).fetchall()
            try:
                metadata = dict(conn.execute("SELECT name, value FROM metadata").fetchall())
            except sqlite3.Error:
                metadata = {}
        finally:
            conn.close()
    zooms, total, fmts = [], 0, []
    for z, cnt, minc, maxc, minr, maxr, magic in rows:
        values = (z, cnt, minc, maxc, minr, maxr)
        if not all(isinstance(value, int) for value in values):
            raise ValueError("non-integer tile index in MBTiles database")
        if not 0 <= z <= 30:
            raise ValueError(f"invalid MBTiles zoom level: {z}")
        tile_max = (1 << z) - 1
        if (cnt < 1 or minc < 0 or minr < 0 or maxc > tile_max or maxr > tile_max or
                minc > maxc or minr > maxr):
            raise ValueError(f"invalid tile coordinates at zoom {z}")
        total += cnt
        signature = bytes(magic or b"")
        if signature[:2] == b"\xff\xd8":
            zfmt = "JPEG"
        elif signature == b"\x89PNG\r\n\x1a\n":
            zfmt = "PNG"
        else:
            zfmt = "UNKNOWN"
        if zfmt not in fmts:
            fmts.append(zfmt)
        ymax = (1 << z) - 1
        nlat, wlon = num2deg(minc, ymax - maxr, z)
        slat, elon = num2deg(maxc + 1, (ymax - minr) + 1, z)
        km_x, km_y = box_km(nlat, wlon, slat, elon)
        zooms.append({"z": z, "count": cnt,
                      "n": round(nlat, 4), "w": round(wlon, 4),
                      "s": round(slat, 4), "e": round(elon, 4),
                      "km_x": round(km_x, 1), "km_y": round(km_y, 1), "fmt": zfmt})
    # A blended pack holds more than one format, so report every format present
    # rather than whichever one a single sampled tile happened to be.
    fmt = "+".join(fmts) if fmts else None
    source_levels = []
    raw_sources = metadata.get("sources", "")
    if not isinstance(raw_sources, str):
        raw_sources = ""
    for item in raw_sources.split(","):
        try:
            z_text, source = item.split(":", 1)
            source_levels.append({"z": int(z_text), "source": source.strip()})
        except (ValueError, TypeError):
            continue
    source_levels.sort(key=lambda item: item["z"])
    source_names = []
    for item in source_levels:
        if item["source"] and item["source"] not in source_names:
            source_names.append(item["source"])
    if not source_names and basemap in BASEMAPS:
        source_names = [basemap]
    if not source_names:
        inferred = basemap.split("_")
        if inferred and all(name in BASEMAPS for name in inferred):
            source_names = inferred
    top = zooms[-1] if zooms else None
    try:
        lm_bytes = os.path.getsize(LANDMARKS_DB) if os.path.exists(LANDMARKS_DB) else 0
    except OSError:
        lm_bytes = 0
    return {"basemap": basemap, "total": total, "zooms": zooms, "fmt": fmt,
            "bytes": os.path.getsize(path), "lm_bytes": lm_bytes,
            "min_zoom": zooms[0]["z"] if zooms else None,
            "max_zoom": top["z"] if top else None,
            "km_x": top["km_x"] if top else None, "km_y": top["km_y"] if top else None,
            "bounds": ({"n": top["n"], "w": top["w"], "s": top["s"], "e": top["e"]}
                       if top else None),
            "map_type": "+".join(source_names) if source_names else None,
            "source_levels": source_levels}


def downloaded_pack_summaries():
    """Describe every downloaded MBTiles pack in the configured maps directory.

    Returns: Sorted JSON-ready summaries; unreadable packs carry an error field.
    """
    packs = []
    try:
        entries = sorted(os.scandir(MAPS_DIR), key=lambda entry: entry.name.lower())
    except OSError:
        return packs
    configured_path = os.path.normcase(os.path.abspath(mbtiles_for(pack_id())))
    for entry in entries:
        if not entry.name.lower().endswith(".mbtiles") or entry.is_symlink():
            continue
        stem = entry.name[:-8]
        if not downloaded_pack_exists(stem):
            continue
        try:
            summary = cache_summary(stem)
            summary["compatible"] = bool(summary["total"] and summary["fmt"] and
                                         all(fmt in OSD_TILE_FORMATS
                                             for fmt in summary["fmt"].split("+")))
        except (OSError, sqlite3.Error, TypeError, ValueError, OverflowError) as exc:
            summary = {"basemap": stem, "total": 0, "zooms": [], "bytes": 0,
                       "compatible": False, "error": str(exc)}
        summary["id"] = stem
        summary["name"] = stem
        summary["active"] = (os.path.normcase(os.path.abspath(entry.path)) == configured_path)
        packs.append(summary)
    return packs


# ---------------------------------------------------------------------------
# Terrain elevation (DEM)
# ---------------------------------------------------------------------------

def decode_png_rgb(buf):
    """Decode an 8-bit RGB/RGBA non-interlaced PNG using only the stdlib.

    This server carries no image library on purpose, and terrain tiles must be
    read losslessly (a JPEG round-trip would corrupt the encoded metres), so the
    few PNG features the DEM source actually uses are decoded here directly:
    colour type 2/6, bit depth 8, no interlacing.

    buf: PNG bytes.
    Returns: (width, height, channels, bytearray of raw samples).
    Raises ValueError on anything outside that subset.
    """
    if buf[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    idat = bytearray()
    w = h = ctype = None
    pos = 8
    while pos + 8 <= len(buf):
        ln = struct.unpack(">I", buf[pos:pos + 4])[0]
        typ = buf[pos + 4:pos + 8]
        body = buf[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace != 0:
                raise ValueError(
                    f"unsupported PNG (depth={depth} colour={ctype} interlace={interlace})")
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln                       # length + type + data + CRC
    if w is None:
        raise ValueError("PNG has no IHDR")
    nch = 3 if ctype == 2 else 4
    try:
        data = zlib.decompress(bytes(idat))
    except zlib.error as e:                  # truncated/corrupt IDAT
        raise ValueError(f"PNG inflate failed: {e}") from None
    stride = w * nch
    if len(data) < h * (stride + 1):
        raise ValueError("PNG truncated")
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for row in range(h):
        f = data[p]
        p += 1
        line = bytearray(data[p:p + stride])
        p += stride
        if f == 1:                                        # Sub
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:                                      # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:                                      # Average
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:                                      # Paeth
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            raise ValueError(f"bad PNG filter {f}")
        out[row * stride:(row + 1) * stride] = line
        prev = line
    return w, h, nch, out


def terrarium_to_int16(w, h, nch, px):
    """Convert decoded terrarium RGB samples to a grid of metres.

    Returns: (bytes of w*h little-endian int16, min_m, max_m), row-major and
    north-to-south — the same order the PNG rows arrive in.
    """
    grid = bytearray(w * h * 2)
    lo, hi = 32767, -32768
    o = 0
    for i in range(w * h):
        j = i * nch
        e = (px[j] * 256 + px[j + 1] + px[j + 2] / 256.0) - 32768.0
        e = int(round(e))
        # keep the nodata sentinel distinguishable from a real reading
        if e <= DEM_NODATA:
            e = DEM_NODATA + 1
        elif e > 32767:
            e = 32767
        if e < lo:
            lo = e
        if e > hi:
            hi = e
        struct.pack_into("<h", grid, o, e)
        o += 2
    return bytes(grid), lo, hi


def open_elevation_db():
    """Open (creating if needed) the elevation DB.

    Returns: an sqlite3 connection with a 10s busy timeout. Caller holds
    elevation_db_lock.
    """
    os.makedirs(MAPS_DIR, exist_ok=True)
    conn = sqlite3.connect(ELEVATION_DB, timeout=10)
    conn.execute("PRAGMA busy_timeout=10000")
    conn.execute("CREATE TABLE IF NOT EXISTS meta(name TEXT PRIMARY KEY, value TEXT)")
    # NB: tile_x/tile_y are XYZ, NOT the TMS row order the sibling .mbtiles files
    # use. Reading this table with MBTiles' flipped y mirrors the terrain
    # north-south, so the convention is recorded in meta as well.
    conn.execute(
        "CREATE TABLE IF NOT EXISTS elevation("
        "zoom INTEGER, tile_x INTEGER, tile_y INTEGER,"
        "width INTEGER, height INTEGER,"
        "min_m INTEGER, max_m INTEGER,"       # range without decoding the blob
        "data BLOB,"                          # width*height int16 LE, row 0 = north
        "PRIMARY KEY(zoom, tile_x, tile_y))"
    )
    for k, v in (("zoom", str(DEM_ZOOM)),
                 ("encoding", "int16_le"),
                 ("compression", "none"),
                 ("nodata", str(DEM_NODATA)),
                 ("tile_scheme", "xyz"),      # not tms
                 ("row_order", "north_to_south"),
                 ("units", "m"),
                 ("vertical_datum", "EGM96 geoid (mean sea level)"),
                 ("source", DEM_URL)):
        conn.execute("INSERT OR REPLACE INTO meta VALUES(?,?)", (k, v))
    conn.commit()
    return conn


def dem_on(m):
    """Read the elevation flag from an already-held [map] section.

    Lock-free on purpose: callers that already hold config_lock must use this,
    since config_lock is not reentrant.
    """
    return m.get("elevation", "1").strip().lower() not in ("0", "false", "no", "")


def dem_enabled():
    """Whether terrain elevation should be downloaded alongside tiles."""
    with config_lock:
        return dem_on(config["map"])


def dem_plan_total(north, south, east, west):
    """Number of DEM tiles the given bbox needs at DEM_ZOOM."""
    xs, ys = bbox_tile_ranges(north, south, east, west, DEM_ZOOM)
    return len(xs) * len(ys)


def download_elevation(north, south, east, west, progress=None):
    """Fetch and store terrain elevation for a bbox, as int16 metre grids.

    Covers exactly the same bbox as the tile download. Skips tiles already
    stored, so it resumes like the tile cache does.
    progress: optional callback(done, total) for the UI progress bar; `done`
    counts every planned tile including cached ones, so a resume still advances.
    Returns: (stored, failed, note). Raises only on a DB-level failure.
    """
    xs, ys = bbox_tile_ranges(north, south, east, west, DEM_ZOOM)
    want = len(xs) * len(ys)
    if want > MAX_DEM_TILES:
        return 0, 0, (f"elevation skipped ({want} > {MAX_DEM_TILES} tiles; "
                      f"area too large)")
    if progress:
        progress(0, want)
    delay = int(config["server"]["tile_delay_ms"]) / 1000.0
    stored = failed = seen = 0
    with elevation_db_lock:
        conn = open_elevation_db()
        try:
            for x in xs:
                for y in ys:
                    seen += 1
                    if progress:
                        progress(seen, want)
                    have = conn.execute(
                        "SELECT 1 FROM elevation WHERE zoom=? AND tile_x=? AND tile_y=?",
                        (DEM_ZOOM, x, y),
                    ).fetchone()
                    if have:
                        continue
                    data = None
                    for _ in range(3):              # retry transient fetch errors
                        try:
                            data = _tile_get(_fmt_tile(DEM_URL, DEM_ZOOM, x, y))
                            break
                        except Exception:
                            time.sleep(0.3)
                    if data is None:
                        failed += 1
                        continue
                    try:
                        w, h, nch, px = decode_png_rgb(data)
                        grid, lo, hi = terrarium_to_int16(w, h, nch, px)
                    except Exception as e:          # a bad tile must not kill the run
                        print(f"[mapserver] DEM decode failed at {DEM_ZOOM}/{x}/{y}: {e}")
                        failed += 1
                        continue
                    conn.execute(
                        "INSERT OR REPLACE INTO elevation VALUES(?,?,?,?,?,?,?,?)",
                        (DEM_ZOOM, x, y, w, h, lo, hi, grid),
                    )
                    stored += 1
                    if stored % 8 == 0:
                        conn.commit()
                    time.sleep(delay)
            conn.commit()
        finally:
            conn.close()
    note = f"{stored} elevation tiles" if stored else "elevation up to date"
    if failed:
        note += f" ({failed} failed)"
    return stored, failed, note


def elevation_at(lat, lon, bilinear=True):
    """Terrain height above sea level at a point, in metres.

    lat/lon: degrees. bilinear: interpolate across the 4 nearest samples;
    nearest-sample lookup otherwise gives ~28 m stair-steps.
    Returns: float metres, or None if that point was never downloaded.
    """
    if not os.path.exists(ELEVATION_DB):
        return None
    n = 1 << DEM_ZOOM
    try:
        r = math.radians(lat)
        fx = (lon + 180.0) / 360.0 * n
        fy = (1.0 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2.0 * n
    except (ValueError, ZeroDivisionError):
        return None

    tiles = {}

    def sample(gx, gy, size):
        """Read one sample from the global pixel grid, loading its tile as needed."""
        tx, ty = gx // size, gy // size
        if (tx, ty) not in tiles:
            row = conn.execute(
                "SELECT width, height, data FROM elevation "
                "WHERE zoom=? AND tile_x=? AND tile_y=?", (DEM_ZOOM, tx, ty)).fetchone()
            tiles[(tx, ty)] = row
        row = tiles[(tx, ty)]
        if not row:
            return None
        w, h, blob = row
        px, py = gx - tx * size, gy - ty * size
        if not (0 <= px < w and 0 <= py < h):
            return None
        off = (py * w + px) * 2
        if off + 2 > len(blob):
            return None
        v = struct.unpack_from("<h", blob, off)[0]
        return None if v == DEM_NODATA else float(v)

    # No elevation_db_lock here: that lock serialises the writer, which holds it for a
    # whole download, and the pointer readout polls straight through one. This is a
    # read-only connection, so SQLite's own locking plus busy_timeout is enough.
    try:
        conn = sqlite3.connect(f"file:{ELEVATION_DB}?mode=ro", uri=True, timeout=10)
        try:
            size = conn.execute(
                "SELECT width FROM elevation WHERE zoom=? LIMIT 1", (DEM_ZOOM,)).fetchone()
            if not size:
                return None
            size = size[0]
            # global pixel coordinates, offset by half a pixel so the samples
            # bracketing the point are the ones interpolated between
            gpx = fx * size - 0.5
            gpy = fy * size - 0.5
            x0, y0 = math.floor(gpx), math.floor(gpy)
            if not bilinear:
                return sample(int(round(gpx)), int(round(gpy)), size)
            dx, dy = gpx - x0, gpy - y0
            v00 = sample(int(x0), int(y0), size)
            v10 = sample(int(x0) + 1, int(y0), size)
            v01 = sample(int(x0), int(y0) + 1, size)
            v11 = sample(int(x0) + 1, int(y0) + 1, size)
            if v00 is None:
                return None
            # fall back to the anchor sample at the edge of stored coverage
            v10 = v00 if v10 is None else v10
            v01 = v00 if v01 is None else v01
            v11 = v10 if v11 is None else v11
            top = v00 + (v10 - v00) * dx
            bot = v01 + (v11 - v01) * dx
            return round(top + (bot - top) * dy, 1)
        finally:
            conn.close()
    except sqlite3.Error:
        return None


# --- line-of-sight horizon (viewshed) --------------------------------------
# Standard radio refraction: the atmosphere bends signals down, which is modelled
# as a 4/3 earth radius. Included because it is three lines; it only matters at
# long range over flat ground (a 30 km path drops 53 m against 71 m geometric),
# but it costs nothing to be right.
K_REFRACT = 4.0 / 3.0
R_EARTH = 6371000.0
VIEWSHED_MAX_KM = 100
VIEWSHED_MAX_AZ = 720


def _dem_area(lat0, lon0, radius_m):
    """Load the DEM tiles covering a disc into memory.

    Returns: (tiles dict keyed by (tx, ty), tile size in px), or (None, 0).
    Bounded by the requested radius, not by the whole DB — a 30 km disc is ~70
    tiles (~9 MB), where loading a full 1000-tile DB would be 131 MB.
    """
    if not os.path.exists(ELEVATION_DB):
        return None, 0
    dlat = radius_m / 111320.0
    dlon = radius_m / (111320.0 * max(0.05, math.cos(math.radians(lat0))))
    xs, ys = bbox_tile_ranges(lat0 + dlat, lat0 - dlat,
                              lon0 + dlon, lon0 - dlon, DEM_ZOOM)
    if not xs or not ys:
        return None, 0
    tiles, size = {}, 0
    try:
        conn = sqlite3.connect(f"file:{ELEVATION_DB}?mode=ro", uri=True, timeout=10)
        try:
            q = ("SELECT tile_x, tile_y, width, data FROM elevation WHERE zoom=? "
                 "AND tile_x BETWEEN ? AND ? AND tile_y BETWEEN ? AND ?")
            for tx, ty, w, blob in conn.execute(
                    q, (DEM_ZOOM, xs[0], xs[-1], ys[0], ys[-1])):
                tiles[(tx, ty)] = (w, blob)
                size = w
        finally:
            conn.close()
    except sqlite3.Error:
        return None, 0
    return (tiles, size) if tiles else (None, 0)


def viewshed(lat0, lon0, ant_h=5.0, target_h=100.0, max_m=30000, n_az=360):
    """Terrain line-of-sight horizon around a point, one radius per azimuth.

    lat0/lon0: the centre (ground station). ant_h: antenna height above the ground
    there. target_h: aircraft altitude above that same ground level.

    Walks each azimuth outward tracking the running maximum terrain elevation
    angle. Because the angle needed to see a fixed-altitude target falls with
    distance while the terrain horizon angle only rises, each azimuth blocks
    exactly once — so the result is a single closed ring rather than a set of
    patches.

    Returns: dict with the ring, the centre's ground height and stats, or
    {"error": ...} when the centre is not covered.
    """
    max_m = max(1000.0, min(float(max_m), VIEWSHED_MAX_KM * 1000.0))
    n_az = max(8, min(int(n_az), VIEWSHED_MAX_AZ))
    tiles, size = _dem_area(lat0, lon0, max_m)
    if not tiles:
        return {"error": "no elevation data for this area"}

    n = 1 << DEM_ZOOM

    def raw(gx, gy):
        t = tiles.get((gx // size, gy // size))
        if t is None:
            return None
        w, blob = t
        px, py = gx - (gx // size) * size, gy - (gy // size) * size
        off = (py * w + px) * 2
        if off < 0 or off + 2 > len(blob):
            return None
        v = struct.unpack_from("<h", blob, off)[0]
        return None if v == DEM_NODATA else v

    def sample(lat, lon):
        """Bilinear terrain height, or None outside the loaded tiles."""
        try:
            r = math.radians(lat)
            gpx = (lon + 180.0) / 360.0 * n * size - 0.5
            gpy = (1.0 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2.0 * n * size - 0.5
        except (ValueError, ZeroDivisionError):
            return None
        x0, y0 = math.floor(gpx), math.floor(gpy)
        v00 = raw(int(x0), int(y0))
        if v00 is None:
            return None
        v10 = raw(int(x0) + 1, int(y0))
        v01 = raw(int(x0), int(y0) + 1)
        v11 = raw(int(x0) + 1, int(y0) + 1)
        v10 = v00 if v10 is None else v10
        v01 = v00 if v01 is None else v01
        v11 = v10 if v11 is None else v11
        dx, dy = gpx - x0, gpy - y0
        top = v00 + (v10 - v00) * dx
        bot = v01 + (v11 - v01) * dx
        return top + (bot - top) * dy

    g0 = sample(lat0, lon0)
    if g0 is None:
        return {"error": "centre is outside the downloaded elevation data"}
    h_obs = g0 + ant_h
    h_t = g0 + target_h
    step = 156543.03392 * math.cos(math.radians(lat0)) / (1 << DEM_ZOOM)   # one sample
    reff = K_REFRACT * R_EARTH
    mlat = 111320.0
    mlon = 111320.0 * max(0.05, math.cos(math.radians(lat0)))

    ring, radii, n_cov = [], [], 0
    for i in range(n_az):
        az = 2 * math.pi * i / n_az
        sx, sy = math.sin(az), math.cos(az)
        max_ang = -9e9
        hit, coverage_limited = max_m, False
        r = step
        while r <= max_m:
            z = sample(lat0 + (sy * r) / mlat, lon0 + (sx * r) / mlon)
            if z is None:                       # ran off the downloaded data
                hit, coverage_limited = r, True
                break
            # Earth curves away from the observer, so everything at range r sits
            # lower than the observer's tangent plane by this much. It applies to
            # the AIRCRAFT as well as the terrain — dropping only the terrain would
            # make curvature extend the horizon instead of shortening it.
            drop = (r * r) / (2 * reff)
            a = (z - drop - h_obs) / r          # terrain horizon angle
            if a > max_ang:
                max_ang = a
            if (h_t - drop - h_obs) / r < max_ang:   # terrain now blocks the view
                hit = r
                break
            r += step
        if coverage_limited:
            n_cov += 1
        radii.append(hit)
        ring.append([round(lat0 + (sy * hit) / mlat, 6),
                     round(lon0 + (sx * hit) / mlon, 6)])

    s = sorted(radii)
    return {"center": {"lat": lat0, "lon": lon0, "ground_m": round(g0, 1)},
            "antenna_m": ant_h, "altitude_m": target_h,
            "ring": ring,
            "min_km": round(min(radii) / 1000.0, 2),
            "median_km": round(s[len(s) // 2] / 1000.0, 2),
            "max_km": round(max(radii) / 1000.0, 2),
            "coverage_limited": n_cov, "azimuths": n_az,
            "max_range_km": round(max_m / 1000.0, 1)}


def elevation_summary():
    """What the elevation DB holds, for the panel and tiles_info.

    Returns: {tiles, bytes, min_m, max_m, zoom} — tiles 0 when nothing is stored.
    """
    empty = {"tiles": 0, "bytes": 0, "min_m": None, "max_m": None, "zoom": DEM_ZOOM}
    if not os.path.exists(ELEVATION_DB):
        return empty
    try:                                  # read-only: no writer lock, see elevation_at()
        conn = sqlite3.connect(f"file:{ELEVATION_DB}?mode=ro", uri=True, timeout=10)
        try:
            cnt, lo, hi = conn.execute(
                "SELECT COUNT(*), MIN(min_m), MAX(max_m) FROM elevation WHERE zoom=?",
                (DEM_ZOOM,)).fetchone()
        finally:
            conn.close()
        return {"tiles": cnt or 0, "bytes": os.path.getsize(ELEVATION_DB),
                "min_m": lo, "max_m": hi, "zoom": DEM_ZOOM}
    except (sqlite3.Error, OSError):
        return empty


# ---------------------------------------------------------------------------
# Offline download of a viewed area
# ---------------------------------------------------------------------------

dl_lock = threading.Lock()
dl_status = {"state": "idle", "phase": "idle", "done": 0, "total": 0, "failed": 0,
             "msg": "", "pack": None, "lm_count": None, "dem_count": None}


def set_dl(**kw):
    """Update the shared download-status dict under dl_lock.

    kw: fields to merge into dl_status (state, done, total, failed, msg, ...).
    """
    with dl_lock:
        dl_status.update(kw)


def download_worker(basemap, zs, srcs, north, south, east, west):
    """Download a bbox's tiles (all stored zooms) into the pack, then POIs.

    basemap: Unique pack id to write. zs: snapshotted stored zoom levels.
    srcs: snapshotted tile sources aligned with zs.
    north/south/east/west: bbox edges in degrees.
    Runs in a thread; reports progress via set_dl and refuses areas over
    MAX_TILES. Also stores center in config and fetches Overpass landmarks.

    Each stored zoom is fetched from its own source, so one pack can hold e.g.
    topographic tiles at the coarse levels and imagery at the detail level.
    """
    total = plan_total(north, south, east, west, zs)
    if total > MAX_TILES:
        set_dl(state="error",
               msg=f"{total} tiles > limit {MAX_TILES}; reduce the area or the detail zoom")
        return
    set_dl(state="running", phase="tiles", done=0, total=total, failed=0,
           msg=f"downloading {basemap}", pack=basemap)
    delay = int(config["server"]["tile_delay_ms"]) / 1000.0
    done = failed = 0
    try:
        with db_lock:
            conn = open_mbtiles(basemap, write=True)
            # Record the mix this pack was built from, so the tool can tell when a
            # pack no longer matches the configured sources.
            conn.execute("DELETE FROM metadata WHERE name='sources'")
            conn.execute("INSERT INTO metadata VALUES('sources',?)",
                         (",".join(f"{z}:{s}" for z, s in zip(zs, srcs)),))
            conn.commit()
        try:
            for z, z_src in zip(zs, srcs):
                xs, ys = bbox_tile_ranges(north, south, east, west, z)
                ymax = (1 << z) - 1
                for x in xs:
                    for y in ys:
                        ymbt = ymax - y
                        with db_lock:
                            have = conn.execute(
                                "SELECT 1 FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                                (z, x, ymbt),
                            ).fetchone()
                        if not have:
                            data = None
                            for _ in range(3):          # retry transient fetch errors
                                try:
                                    data = fetch_tile(z_src, z, x, y)
                                    break
                                except Exception:
                                    time.sleep(0.3)
                            if data is not None:
                                with db_lock:
                                    conn.execute(
                                        "INSERT OR REPLACE INTO tiles VALUES(?,?,?,?)",
                                        (z, x, ymbt, data),
                                    )
                                time.sleep(delay)
                            else:
                                failed += 1
                        done += 1
                        if done % 10 == 0:
                            with db_lock:
                                conn.commit()
                            set_dl(done=done, failed=failed)
            with db_lock:
                conn.commit()
        finally:
            with db_lock:
                conn.close()
    except Exception as e:                              # never let the thread die silently
        set_dl(state="error", failed=failed, msg=f"download error: {e}")
        print(f"[mapserver] download error: {e}")
        return
    with config_lock:
        config["map"]["center_lat"] = str((north + south) / 2)
        config["map"]["center_lon"] = str((east + west) / 2)
    save_config()
    log_cache_summary(basemap)

    # Download POI landmarks for the same area (non-fatal if Overpass is unreachable)
    set_dl(phase="poi", done=done, failed=failed, msg="tiles saved; fetching POIs…")
    n_lm, lm_note = 0, "POIs unavailable"
    try:
        raw = fetch_landmarks(north, south, east, west)
        features = parse_landmarks(raw)
        n_lm = store_landmarks(features)
        lm_note = f"{n_lm} POIs"
        print(f"[mapserver] {n_lm} landmarks stored")
    except Exception as e:
        print(f"[mapserver] POI download failed: {e}")

    # Terrain elevation for the same area (non-fatal: a DEM failure must never
    # fail a tile download that already succeeded).
    dem_note, n_dem = "", 0
    if dem_enabled():
        # The bar restarts from 0 for this phase: the tile phase already reached
        # 100%, and leaving it there made the elevation download look like a hang.
        set_dl(phase="elevation", done=0, total=0, msg="downloading elevation…")

        def dem_progress(d, t):
            set_dl(done=d, total=t, msg=f"elevation {d}/{t}…")

        try:
            n_dem, _dem_failed, dem_note = download_elevation(
                north, south, east, west, progress=dem_progress)
            print(f"[mapserver] {dem_note}")
        except Exception as e:
            dem_note = "elevation unavailable"
            print(f"[mapserver] elevation download failed: {e}")

    set_dl(state="done", phase="done", done=done, total=total, failed=failed,
           msg=f"saved {basemap}.mbtiles ({failed} failed) · {lm_note}" +
               (f" · {dem_note}" if dem_note else ""),
           lm_count=n_lm, dem_count=n_dem)


def log_cache_summary(basemap):
    """Print the per-zoom cache summary (same view as tiles_info.py) after a download."""
    try:
        import tiles_info
        tiles_info.report(mbtiles_for(basemap), None, None)
    except Exception as e:
        print(f"[mapserver] cache summary failed: {e}")


# ---------------------------------------------------------------------------
# POI landmarks (Overpass API → local SQLite)
# ---------------------------------------------------------------------------

OVERPASS_URL = "https://overpass-api.de/api/interpreter"


def open_landmarks_db():
    """Open the landmarks SQLite DB, creating/migrating its schema.

    Drops and rebuilds the old kind/subtype/ele-only schema, ensures the
    landmarks and poi_selection tables exist.
    Returns: an sqlite3 connection with a 10s busy timeout.
    """
    os.makedirs(MAPS_DIR, exist_ok=True)
    conn = sqlite3.connect(LANDMARKS_DB, timeout=10)
    conn.execute("PRAGMA busy_timeout=10000")
    # Migrate old schema (kind/subtype/ele columns) → new schema (tags JSON blob)
    try:
        conn.execute("SELECT tags, kind, name_en FROM landmarks LIMIT 0")
    except sqlite3.OperationalError:
        conn.execute("DROP TABLE IF EXISTS landmarks")
        conn.execute("DROP INDEX IF EXISTS lm_osm")
    conn.execute(
        "CREATE TABLE IF NOT EXISTS landmarks("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "osm_type TEXT, osm_id INTEGER,"
        "name TEXT, name_en TEXT, lat REAL, lon REAL,"
        "kind TEXT, subtype TEXT, ele REAL,"   # indexed columns for SQL filtering
        "tags TEXT)"                           # full OSM tag JSON for everything else
    )
    conn.execute(
        "CREATE UNIQUE INDEX IF NOT EXISTS lm_osm ON landmarks(osm_type, osm_id)"
    )
    # Which kind/subtype pairs the OSD should draw (subtype '' = none). Persists the
    # preflight tree selection; read by both the server and osd/util/poi_osd.c.
    conn.execute(
        "CREATE TABLE IF NOT EXISTS poi_selection("
        "kind TEXT NOT NULL, subtype TEXT NOT NULL, enabled INTEGER NOT NULL,"
        "PRIMARY KEY(kind, subtype))"
    )
    # User-authored points (the preflight target, and named waypoints later). Kept
    # separate from the Overpass-populated `landmarks` table so a POI re-download
    # never clobbers them. Read by osd/util/poi_osd.c. A missing 'target' row means
    # no target is set. This is the preflight->flight target handoff (was state.ini).
    conn.execute(
        "CREATE TABLE IF NOT EXISTS waypoints("
        "kind TEXT PRIMARY KEY, lat REAL NOT NULL, lon REAL NOT NULL, name TEXT)"
    )
    conn.commit()
    return conn


def fetch_landmarks(north, south, east, west):
    """Query Overpass for all named features in the bbox; returns parsed JSON dict."""
    bbox = f"{south},{west},{north},{east}"
    # All named nodes + named non-highway ways/relations (roads would add thousands of
    # duplicate segments per road name and are not useful for landmark navigation).
    query = (
        "[out:json][timeout:60];\n(\n"
        f'  node["name"]({bbox});\n'
        f'  way["name"][!"highway"]({bbox});\n'
        f'  relation["name"][!"highway"]({bbox});\n'
        ");\nout center tags;\n"
    )
    req = urllib.request.Request(
        OVERPASS_URL, data=query.encode(),
        headers={"User-Agent": USER_AGENT,
                 "Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.loads(r.read())


def _classify(tags):
    """Return (kind, subtype) from OSM tags using a priority order."""
    for key in ("place", "natural", "amenity", "tourism", "historic",
                "aeroway", "waterway", "leisure", "landuse", "man_made",
                "military", "boundary", "shop", "office", "emergency"):
        val = tags.get(key)
        if val:
            return key, val
    for k, v in tags.items():
        if k not in ("name", "name:en", "source", "created_by", "note", "wikidata", "wikipedia"):
            return k, v
    return "other", "unknown"


def _parse_ele(tags):
    """Parse an elevation value from OSM tags.

    tags: OSM tag dict.
    Returns: the 'ele' tag as a float (first value if ';'-separated), or None.
    """
    try:
        return float(str(tags.get("ele", "")).split(";")[0].strip())
    except ValueError:
        return None


def parse_landmarks(resp):
    """Store every named feature with kind/subtype/ele columns + full tags JSON."""
    features = []
    for el in resp.get("elements", []):
        tags = el.get("tags", {})
        name = tags.get("name") or tags.get("name:en")
        if not name:
            continue
        osm_type, osm_id = el["type"], el["id"]
        if osm_type == "node":
            lat, lon = el.get("lat"), el.get("lon")
        else:
            c = el.get("center", {})
            lat, lon = c.get("lat"), c.get("lon")
        if lat is None or lon is None:
            continue
        kind, subtype = _classify(tags)
        features.append({
            "osm_type": osm_type, "osm_id": osm_id,
            "name": name, "name_en": tags.get("name:en") or None,
            "lat": lat, "lon": lon,
            "kind": kind, "subtype": subtype, "ele": _parse_ele(tags),
            "tags": json.dumps(tags, ensure_ascii=False),
        })
    return features


def store_landmarks(features):
    """Upsert landmark list into the local DB; returns count stored."""
    with landmarks_db_lock:
        conn = open_landmarks_db()
        try:
            conn.executemany(
                "INSERT OR REPLACE INTO landmarks"
                "(osm_type,osm_id,name,name_en,lat,lon,kind,subtype,ele,tags)"
                " VALUES(:osm_type,:osm_id,:name,:name_en,:lat,:lon,:kind,:subtype,:ele,:tags)",
                features,
            )
            conn.commit()
        finally:
            conn.close()
    return len(features)


def query_landmarks(north, south, east, west):
    """Return cached landmarks within a bbox with all columns."""
    if not os.path.exists(LANDMARKS_DB):
        return []
    with landmarks_db_lock:
        conn = open_landmarks_db()
        try:
            rows = conn.execute(
                "SELECT name,name_en,lat,lon,kind,subtype,ele,tags FROM landmarks"
                " WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?",
                (south, north, west, east),
            ).fetchall()
        finally:
            conn.close()
    return [{"name": r[0], "name_en": r[1], "lat": r[2], "lon": r[3],
             "kind": r[4], "subtype": r[5], "ele": r[6],
             "tags": json.loads(r[7])} for r in rows]


def landmarks_count():
    """Return the number of stored landmarks (0 if the DB is absent)."""
    if not os.path.exists(LANDMARKS_DB):
        return 0
    with landmarks_db_lock:
        conn = open_landmarks_db()
        try:
            return conn.execute("SELECT COUNT(*) FROM landmarks").fetchone()[0]
        finally:
            conn.close()


def poi_types():
    """Grouped kind/subtype counts joined with the saved enable state.
    Seeds place-only on first use (empty selection table)."""
    if not os.path.exists(LANDMARKS_DB):
        return []
    with landmarks_db_lock:
        conn = open_landmarks_db()
        try:
            # First run: seed enabled=1 for place, 0 for everything else.
            if conn.execute("SELECT COUNT(*) FROM poi_selection").fetchone()[0] == 0:
                conn.executemany(
                    "INSERT OR IGNORE INTO poi_selection(kind, subtype, enabled)"
                    " VALUES(?,?,?)",
                    [(k, st, 1 if k == "place" else 0) for k, st in conn.execute(
                        "SELECT DISTINCT kind, COALESCE(NULLIF(subtype,''),'')"
                        " FROM landmarks")],
                )
                conn.commit()
            rows = conn.execute(
                "SELECT l.kind, COALESCE(NULLIF(l.subtype,''),'') AS st,"
                "       COUNT(*) AS cnt, COALESCE(s.enabled,0) AS en"
                " FROM landmarks l"
                " LEFT JOIN poi_selection s"
                "   ON s.kind=l.kind AND s.subtype=COALESCE(NULLIF(l.subtype,''),'')"
                " GROUP BY l.kind, st ORDER BY l.kind, st"
            ).fetchall()
        finally:
            conn.close()
    return [{"kind": r[0], "subtype": r[1], "count": r[2], "enabled": bool(r[3])}
            for r in rows]


def save_poi_selection(items):
    """Upsert [{kind, subtype, enabled}] rows into poi_selection."""
    with landmarks_db_lock:
        conn = open_landmarks_db()
        try:
            conn.executemany(
                "INSERT INTO poi_selection(kind, subtype, enabled) VALUES(?,?,?)"
                " ON CONFLICT(kind, subtype) DO UPDATE SET enabled=excluded.enabled",
                [(str(it["kind"]), str(it.get("subtype", "")), 1 if it["enabled"] else 0)
                 for it in items],
            )
            conn.commit()
        finally:
            conn.close()


def start_download(name, zs, srcs, north, south, east, west):
    """Reserve a unique pack and spawn its download worker unless busy.

    name: Optional requested map name. zs: stored zoom snapshot.
    srcs: source snapshot used for the fallback filename and downloaded tiles.
    north/south/east/west: bbox edges in degrees.
    Returns: Reserved pack id, False when busy, or None on filesystem failure.
    """
    with dl_lock:
        if dl_status["state"] == "running":
            return False
        try:
            os.makedirs(MAPS_DIR, exist_ok=True)
        except OSError as exc:
            dl_status.update(state="error", phase="error",
                             msg=f"cannot create maps folder: {exc}")
            return None
        while True:
            basemap = allocate_pack_id(name, srcs)
            try:
                fd = os.open(mbtiles_for(basemap), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
                os.close(fd)
                break
            except FileExistsError:
                continue
            except OSError as exc:
                dl_status.update(state="error", phase="error",
                                 msg=f"cannot create map file: {exc}")
                return None
        dl_status.update(state="running", phase="starting", done=0, total=0, failed=0,
                         msg=f"starting {basemap}", pack=basemap,
                         lm_count=None, dem_count=None)
    try:
        threading.Thread(
            target=download_worker,
            args=(basemap, zs, srcs, north, south, east, west), daemon=True
        ).start()
    except RuntimeError as exc:
        try:
            os.remove(mbtiles_for(basemap))
        except OSError:
            pass
        set_dl(state="error", phase="error", msg=f"cannot start download: {exc}")
        return None
    return basemap


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        """Silence the default per-request stderr logging."""
        pass

    def handle(self):
        """Run the request loop, swallowing client-abort socket errors.

        The browser cancels in-flight tile loads while panning/zooming; this
        absorbs the resulting broken-pipe/reset instead of logging a traceback.
        """
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send(self, code, body=b"", ctype="text/plain", extra=None):
        """Write an HTTP response with body, content type and headers.

        code: status code. body: bytes or str. ctype: Content-Type.
        extra: optional header dict (defaults Cache-Control to no-store).
        """
        if isinstance(body, str):
            body = body.encode()
        extra = dict(extra or {})
        # Default to no-store so WebKitGTK never serves stale JSON (e.g. /poi-types
        # checkbox states). Tiles/static pass an explicit Cache-Control to override.
        extra.setdefault("Cache-Control", "no-store")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra.items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json_body(self):
        """Read and parse the request body as JSON.

        Returns: the decoded object ({} when the body is empty).
        Raises: ValueError on malformed JSON.
        """
        length = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(length) or b"{}")

    def do_GET(self):
        """Route GET requests to the matching handler (static, SSE, JSON, tiles)."""
        path = urlparse(self.path).path
        if path in ("/", "/viewer.html"):
            return self.serve_static("viewer.html")
        if path == "/pos":
            return self.serve_sse()
        if path == "/status":
            return self.serve_status()
        if path == "/settings":
            return self.serve_settings_get()
        if path == "/download":
            with dl_lock:
                return self._send(200, json.dumps(dl_status), "application/json")
        if path == "/coverage":
            return self.serve_coverage()
        if path == "/cache":
            return self.serve_cache()
        if path == "/packs":
            return self.serve_packs()
        if path == "/elevation":
            return self.serve_elevation()
        if path == "/viewshed":
            return self.serve_viewshed()
        if path == "/landmarks":
            return self.serve_landmarks()
        if path == "/poi-types":
            return self._send(200, json.dumps(poi_types()), "application/json")
        if path == "/export":
            return self.serve_export()
        if path.startswith("/tiles/"):
            return self.serve_tile(path)
        return self.serve_static(path.lstrip("/"))

    def do_POST(self):
        """Route POST requests (settings, target, download, poi-selection)."""
        path = urlparse(self.path).path
        if path == "/settings":
            return self.serve_settings_post()
        if path == "/target":
            return self.serve_target_post()
        if path == "/download":
            return self.serve_download_post()
        if path == "/poi-selection":
            return self.serve_poi_selection_post()
        self._send(404, b"not found")

    def do_DELETE(self):
        """Route deletion of one explicitly named downloaded map pack."""
        path = urlparse(self.path).path
        if path == "/packs":
            return self.serve_pack_delete()
        self._send(404, b"not found")

    def serve_static(self, name):
        """Serve a whitelisted static file from WEB_ROOT (no-store cached).

        name: requested file name; must be in STATIC_WHITELIST and path-safe.
        Sends 404 for unknown or traversal names.
        """
        ctype = STATIC_WHITELIST.get(name)
        if ctype is None or ".." in name:
            return self._send(404, b"not found")
        try:
            with open(os.path.join(WEB_ROOT, name), "rb") as fh:
                body = fh.read()
        except OSError:
            return self._send(404, b"not found")
        # Never cache the app shell/JS — otherwise WebKitGTK serves a stale
        # viewer.html across mapwin restarts and code changes don't take effect.
        self._send(200, body, ctype, {"Cache-Control": "no-store"})

    def serve_tile(self, path):
        """Serve a /tiles/{z}/{x}/{y} tile: offline cache first, else live proxy.

        path: the request path. The optional ?src= picks a pack and ?offline=
        forces cache-only. Sends 204 when no tile is available. The live proxy
        uses whichever source owns the nearest stored zoom, so browsing previews
        the same imagery the download would store at that level.
        """
        parts = path.split("/")
        try:
            z, x = int(parts[2]), int(parts[3])
            y = int(parts[4].split(".")[0])
        except (IndexError, ValueError):
            return self._send(400, b"bad tile")
        q = parse_qs(urlparse(self.path).query)
        src = q.get("src", [None])[0]
        offline = q.get("offline", [None])[0]      # "test offline" -> cache only, no proxy
        pack = resolve_pack(src) or pack_id()
        # 1) offline cache for this pack
        try:
            data = read_tile(pack, z, x, y)
        except sqlite3.Error:
            data = None
        # 2) live proxy when online (unless the user is testing offline coverage)
        if data is None and not offline and BROWSE_MIN <= z <= BROWSE_MAX and is_online():
            try:
                data = fetch_tile(source_for(z), z, x, y)
            except Exception:
                data = None
        if data is None:
            return self._send(204)
        self._send(200, data, tile_ctype(data), {"Cache-Control": "max-age=86400"})

    def serve_cache(self):
        """Send the per-zoom cache summary (plus landmark count) as JSON.

        Basemap is chosen by the ?src= query param, else the active basemap.
        """
        src = parse_qs(urlparse(self.path).query).get("src", [None])[0]
        basemap = resolve_pack(src) or pack_id()
        summary = cache_summary(basemap)
        summary["lm_count"] = landmarks_count()
        summary["elevation"] = elevation_summary()
        self._send(200, json.dumps(summary), "application/json")

    def serve_packs(self):
        """Send metadata for every downloaded map pack as JSON.

        Returns an array containing safe pack ids, zooms, sources, formats,
        sizes and maximum-detail coverage bounds.
        """
        self._send(200, json.dumps(downloaded_pack_summaries()), "application/json")

    def serve_pack_delete(self):
        """Permanently delete the safe existing .mbtiles file named by ?id=.

        Sends 409 when that pack is being downloaded, 404 when it does not name
        a regular pack file, and never removes shared landmarks or elevation.
        """
        name = parse_qs(urlparse(self.path).query).get("id", [None])[0]
        if not downloaded_pack_exists(name):
            return self._send(404, json.dumps({"error": "map pack not found"}),
                              "application/json")
        with dl_lock:
            if dl_status["state"] == "running" and dl_status.get("pack") == name:
                return self._send(409, json.dumps({"error": "map is still downloading"}),
                                  "application/json")
        try:
            with db_lock:
                if not downloaded_pack_exists(name):
                    return self._send(404, json.dumps({"error": "map pack not found"}),
                                      "application/json")
                os.remove(mbtiles_for(name))
        except OSError as exc:
            return self._send(500, json.dumps({"error": f"cannot delete map: {exc}"}),
                              "application/json")
        self._send(200, json.dumps({"deleted": name}), "application/json")

    def serve_elevation(self):
        """Send the stored terrain height at ?lat=&lon= as JSON.

        Replies {"elev_m": null} when the point was never downloaded, so the UI
        can tell "no coverage" from a real reading of 0 m at sea level.
        """
        q = parse_qs(urlparse(self.path).query)
        try:
            lat, lon = float(q["lat"][0]), float(q["lon"][0])
        except (KeyError, ValueError, IndexError):
            return self._send(400, b"need lat, lon")
        if not valid_coord(lat, lon):
            return self._send(400, b"bad lat/lon")
        self._send(200, json.dumps({"lat": lat, "lon": lon,
                                    "elev_m": elevation_at(lat, lon),
                                    "datum": "EGM96 (MSL)"}), "application/json")

    def serve_viewshed(self):
        """Send the terrain line-of-sight horizon around ?lat=&lon= as JSON.

        Query: lat, lon (required); ant (antenna height above ground, default 5),
        alt (aircraft altitude above the centre's ground, default 100),
        max_km (default 30), az (azimuth count, default 360).
        """
        q = parse_qs(urlparse(self.path).query)

        def num(key, default, lo, hi):
            try:
                return max(lo, min(hi, float(q[key][0])))
            except (KeyError, ValueError, IndexError):
                return default

        try:
            lat, lon = float(q["lat"][0]), float(q["lon"][0])
        except (KeyError, ValueError, IndexError):
            return self._send(400, b"need lat, lon")
        if not valid_coord(lat, lon):
            return self._send(400, b"bad lat/lon")
        res = viewshed(lat, lon,
                       ant_h=num("ant", 5.0, 0.0, 500.0),
                       target_h=num("alt", 100.0, 1.0, 10000.0),
                       max_m=num("max_km", 30.0, 1.0, VIEWSHED_MAX_KM) * 1000.0,
                       n_az=int(num("az", 360, 8, VIEWSHED_MAX_AZ)))
        self._send(200, json.dumps(res), "application/json")

    def serve_export(self):
        """Stream a zip of the map pack (.mbtiles + landmarks.db + elevation.db).

        This is the preflight->flight handoff: the user saves it wherever they
        want (the browser's download picks the location) and copies it to the OSD
        station's gs/maps/. Basemap comes from ?src=, else the active one.
        """
        src = parse_qs(urlparse(self.path).query).get("src", [None])[0]
        basemap = resolve_pack(src) or pack_id()
        mb = mbtiles_for(basemap)
        if not os.path.exists(mb):
            return self._send(404, b"nothing downloaded for this basemap yet")

        # Build the zip in a temp file (ZIP_STORED: tiles/db are already compact,
        # so skip the CPU of deflating ~100 MB), then stream it out.
        safe = os.path.splitext(os.path.basename(mb))[0]
        tmp = tempfile.NamedTemporaryFile(prefix="mappack_", suffix=".zip", delete=False)
        tmp.close()
        try:
            with zipfile.ZipFile(tmp.name, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
                zf.write(mb, os.path.basename(mb))
                with landmarks_db_lock:                # keep landmarks.db read-consistent
                    if os.path.exists(LANDMARKS_DB):
                        zf.write(LANDMARKS_DB, "landmarks.db")
                with elevation_db_lock:
                    if os.path.exists(ELEVATION_DB):
                        zf.write(ELEVATION_DB, "elevation.db")

            size = os.path.getsize(tmp.name)
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition",
                             f'attachment; filename="{safe}-mappack.zip"')
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            with open(tmp.name, "rb") as fh:
                while True:
                    chunk = fh.read(256 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        finally:
            try:
                os.remove(tmp.name)
            except OSError:
                pass

    def serve_landmarks(self):
        """Send cached landmarks within the ?n/s/e/w bbox as JSON.

        Sends 400 if any bbox query parameter is missing or non-numeric.
        """
        q = parse_qs(urlparse(self.path).query)
        try:
            n, s = float(q["n"][0]), float(q["s"][0])
            e, w = float(q["e"][0]), float(q["w"][0])
        except (KeyError, ValueError):
            return self._send(400, b"need n, s, e, w")
        self._send(200, json.dumps(query_landmarks(n, s, e, w)), "application/json")

    def serve_coverage(self):
        """Send which tiles are cached within the ?z/n/s/e/w bbox as JSON.

        Returns a truncated empty result when the requested area exceeds 4000
        tiles. Sends 400 on missing/invalid query parameters.
        """
        q = parse_qs(urlparse(self.path).query)
        try:
            z = int(q["z"][0])
            n, s = float(q["n"][0]), float(q["s"][0])
            e, w = float(q["e"][0]), float(q["w"][0])
        except (KeyError, ValueError):
            return self._send(400, b"need z, n, s, e, w")
        src = q.get("src", [None])[0]
        basemap = resolve_pack(src) or pack_id()
        xs, ys = bbox_tile_ranges(n, s, e, w, z)
        want = len(xs) * len(ys)
        if want > 4000:
            return self._send(200, json.dumps({"z": z, "truncated": True, "present": []}),
                              "application/json")
        present = coverage_in_bbox(basemap, z, n, s, e, w)
        self._send(200, json.dumps({"z": z, "want": want, "present": present}),
                   "application/json")

    def serve_status(self):
        """Send overall server status as JSON.

        Includes online state, cache presence, saved center/zoom, download
        progress, basemap list, armed flag and target/home points.
        """
        with config_lock:
            m = config["map"]
            center = ([float(m["center_lat"]), float(m["center_lon"])]
                      if m["center_lat"] and m["center_lon"] else None)
            zoom = int(m["zoom"])
            basemap = m.get("basemap", "Satellite")
            key = config["server"].get("tile_key", "")
        # basemaps the UI greys out: missing API key, or a tile format the native
        # OSD renderer cannot decode. Maps name -> short reason shown in the option.
        disabled = basemap_issues(key)
        srcs = sources()                  # per-zoom sources (config_lock released above)
        with dl_lock:
            dl = dict(dl_status)
        with state_lock:
            ar = armed_state
        tgts, hm = list(geo["targets"]), geo["home"]
        body = json.dumps({
            "online": is_online(),
            "mbtiles": os.path.exists(mbtiles_for(pack_id(srcs))),
            "center": center, "zoom": zoom, "max_tiles": MAX_TILES, "download": dl,
            "detail_zoom": detail_zoom(), "detail_min": DETAIL_MIN,
            "detail_max": DETAIL_MAX, "zooms": zooms(),
            "sources": srcs, "pack": pack_id(srcs),
            "basemaps": list(BASEMAPS.keys()), "basemaps_disabled": disabled,
            "basemap": basemap,
            "elevation": dem_enabled(), "dem_zoom": DEM_ZOOM,
            "max_dem_tiles": MAX_DEM_TILES,
            "armed": ar,
            "targets": tgts, "target_slots": TARGET_SLOTS,
            # slot 0 repeated under the old key so overlay clients that predate
            # multiple targets keep drawing one
            "target": ({"lat": tgts[0]["lat"], "lon": tgts[0]["lon"]}
                       if tgts[0] else None),
            "home": {"lat": hm[0], "lon": hm[1]} if hm else None,
        })
        self._send(200, body, "application/json")

    def serve_settings_get(self):
        """Send the saved zoom and center settings as JSON."""
        with config_lock:
            m = config["map"]
            body = json.dumps({
                "zoom": int(m["zoom"]),
                "detail_zoom": clamp_detail(m.get("detail_zoom", DETAIL_DEFAULT)),
                "elevation": dem_on(m),        # lock-free: config_lock is held here
                "center_lat": m["center_lat"] or None,
                "center_lon": m["center_lon"] or None,
            })
        self._send(200, body, "application/json")

    def serve_settings_post(self):
        """Update zoom, detail zoom and/or basemap from a JSON body, then persist.

        Sends 400 on malformed JSON; unknown basemaps are ignored and detail_zoom
        is clamped to the selectable range.
        """
        try:
            data = self._json_body()
        except ValueError:
            return self._send(400, b"bad json")
        with config_lock:
            m = config["map"]
            if "zoom" in data:
                m["zoom"] = str(int(data["zoom"]))
            if "detail_zoom" in data:
                m["detail_zoom"] = str(clamp_detail(data["detail_zoom"]))
            if "elevation" in data:
                m["elevation"] = "1" if data["elevation"] else "0"
            if data.get("basemap") in BASEMAPS:
                # the simple control: one source for every stored level
                m["basemap"] = data["basemap"]
                m["sources"] = ""
            if isinstance(data.get("sources"), list):
                n = len(zoom_set(clamp_detail(m.get("detail_zoom", DETAIL_DEFAULT))))
                srcs = parse_sources(",".join(str(s) for s in data["sources"]),
                                     m.get("basemap", "Satellite"), n)
                m["sources"] = ",".join(srcs)
                # keep `basemap` meaningful for a uniform mix (and for old readers)
                if len(set(srcs)) == 1:
                    m["basemap"] = srcs[0]
                    m["sources"] = ""
        save_config()
        self._send(200, b"{}", "application/json")

    def serve_target_post(self):
        """Set the target slots from JSON, then persist them to landmarks.db.

        Preferred body: {"targets": [ {name,lat,lon} | null, ... ]} -- shorter
        lists leave the remaining slots untouched, so the panel can send just the
        slot it edited. The legacy {"lat","lon"} body still works and writes slot
        0 (null lat/lon clears it). Sends 400 on bad JSON or coordinates.
        """
        try:
            d = self._json_body()
        except ValueError:
            return self._send(400, b"bad json")

        slots = list(geo["targets"])
        if isinstance(d.get("targets"), list):
            for i, t in enumerate(d["targets"][:TARGET_SLOTS]):
                if not isinstance(t, dict) or t.get("lat") is None or t.get("lon") is None:
                    slots[i] = None
                    continue
                try:
                    slots[i] = {"lat": float(t["lat"]), "lon": float(t["lon"]),
                                "name": clip_name(t.get("name", ""))}
                except (ValueError, TypeError):
                    return self._send(400, b"bad lat/lon")
        elif d.get("lat") is None or d.get("lon") is None:
            slots[0] = None
        else:
            try:
                slots[0] = {"lat": float(d["lat"]), "lon": float(d["lon"]),
                            "name": str(d.get("name", ""))[:31]}
            except (ValueError, TypeError):
                return self._send(400, b"bad lat/lon")

        geo["targets"] = slots
        save_targets_to_db(slots)
        self._send(200, b"{}", "application/json")

    def serve_poi_selection_post(self):
        """Persist the POI enable selection from a JSON {selection: [...]} body.

        Sends 400 if the body is malformed or selection is not a list of
        valid {kind, subtype, enabled} items.
        """
        try:
            d = self._json_body()
        except ValueError:
            return self._send(400, b"bad json")
        sel = d.get("selection")
        if not isinstance(sel, list):
            return self._send(400, b"need selection list")
        try:
            save_poi_selection(sel)
        except (KeyError, TypeError):
            return self._send(400, b"bad selection item")
        self._send(200, b"{}", "application/json")

    def serve_download_post(self):
        """Start an offline download for the bbox in a JSON body.

        Body needs north/south/east/west and accepts an optional map name. Sends
        409 if the area exceeds MAX_TILES or a download is already running, else
        200 with the count and collision-free pack id.
        """
        try:
            d = self._json_body()
            if not isinstance(d, dict):
                raise TypeError
            north, south = float(d["north"]), float(d["south"])
            east, west = float(d["east"]), float(d["west"])
        except (ValueError, TypeError, KeyError):
            return self._send(400, b"need north, south, east, west")
        with config_lock:
            m = config["map"]
            detail = clamp_detail(m.get("detail_zoom", DETAIL_DEFAULT))
            zs = zoom_set(detail)
            srcs = parse_sources(m.get("sources", ""),
                                 m.get("basemap", "Satellite"), len(zs))
        total = plan_total(north, south, east, west, zs)
        if total > MAX_TILES:
            return self._send(409, json.dumps({"error": f"area too large ({total} tiles) — zoom in", "total": total}), "application/json")
        pack = start_download(d.get("name"), zs, srcs, north, south, east, west)
        if pack is False:
            return self._send(409, json.dumps({"error": "busy"}), "application/json")
        if pack is None:
            with dl_lock:
                error = dl_status.get("msg") or "cannot create map file"
            return self._send(500, json.dumps({"error": error}), "application/json")
        self._send(200, json.dumps({"started": True, "total": total, "pack": pack}),
                   "application/json")

    def serve_sse(self):
        """Stream live position updates to the client as Server-Sent Events.

        Pushes a new event whenever telemetry changes and a keep-alive comment
        every 2s. Returns when the client disconnects.
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        last_seq, last_beat = -1, 0.0
        try:
            while True:
                with state_lock:
                    seq, snap = state_seq, dict(latest)
                now = time.time()
                if seq != last_seq and snap["lat"] is not None:
                    self.wfile.write(f"data: {json.dumps(snap)}\n\n".encode())
                    self.wfile.flush()
                    last_seq, last_beat = seq, now
                elif now - last_beat > 2.0:
                    self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
                    last_beat = now
                time.sleep(0.2)
        except (BrokenPipeError, ConnectionResetError):
            pass


def server_responds(port, timeout=1.5):
    """True if a mapserver instance is actually answering HTTP on the port.

    Distinguishes a live instance (reuse it) from a process that merely holds the
    port but is not serving — e.g. one suspended with Ctrl+Z, or hung.
    """
    import urllib.error
    try:
        urllib.request.urlopen(f"http://127.0.0.1:{port}/viewer.html", timeout=timeout)
        return True
    except urllib.error.HTTPError:
        return True    # answered with an HTTP status -> it is alive
    except Exception:
        return False   # refused / timed out / not answering


def main():
    """Start the server: restore state, launch UDP/online threads, serve HTTP.

    Installs a SIGTERM handler for clean shutdown and blocks in
    serve_forever until interrupted or terminated.

    With --open-browser (the default when packaged as a standalone binary) it
    opens the preflight page in the user's default browser instead of relying on
    the WebKit `mapwin` host — so the standalone app needs no bundled browser.
    """
    import argparse
    import errno
    import webbrowser

    ap = argparse.ArgumentParser(description="Offline preflight map server")
    ap.add_argument("--port", type=int, default=int(config["server"]["port"]),
                    help="HTTP port (default from config.ini)")
    ap.add_argument("--open-browser", dest="open_browser", action="store_true",
                    default=_FROZEN,
                    help="open the preflight page in the system browser "
                         "(default: on when packaged, off in dev)")
    ap.add_argument("--no-browser", dest="open_browser", action="store_false",
                    help="do not open a browser (dev default; used with mapwin)")
    args = ap.parse_args()

    port = args.port
    url = f"http://127.0.0.1:{port}/viewer.html?mode=preflight"

    # Single-instance: bind the HTTP port first. If it is already taken, another
    # copy of the app is running (closing the browser does not stop the detached
    # server) — so instead of crashing with "address already in use", check
    # whether that instance is actually serving:
    #   * responding  -> reuse it: just reopen the browser and exit.
    #   * not responding -> it is stuck (suspended with Ctrl+Z, or hung). We can't
    #     take the port from it, so tell the user how to clear it.
    # Bind before starting the UDP/online threads so a re-launch never fights over
    # the MSP socket either.
    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError as e:
        if e.errno != errno.EADDRINUSE:
            raise
        if server_responds(port):
            print(f"[mapserver] already running on 127.0.0.1:{port}; reopening browser")
            if args.open_browser:
                webbrowser.open(url)
            return
        print(f"[mapserver] ERROR: port {port} is in use but no server is responding.")
        print("[mapserver] A previous instance is probably suspended (Ctrl+Z) or hung.")
        print("[mapserver] Clear it, then relaunch:")
        print("[mapserver]   - if you background/suspended it: run 'fg' then press Ctrl+C, or 'kill %1'")
        print(f"[mapserver]   - otherwise free the port, e.g.: fuser -k {port}/tcp   (Linux)")
        print("[mapserver] Tip: stop this server with Ctrl+C, not Ctrl+Z (Ctrl+Z only freezes it).")
        sys.exit(1)

    # Mark request-handler threads as daemon so they never block shutdown.
    httpd.daemon_threads = True

    load_geo_state()   # restore target + home (home survives a mid-flight restart)
    if geo["home"]:
        print(f"[mapserver] home loaded: {geo['home'][0]:.6f},{geo['home'][1]:.6f}")
    threading.Thread(target=udp_listener, daemon=True).start()
    threading.Thread(target=online_check_loop, daemon=True).start()
    # Handle SIGTERM (sent by map.sh / systemd / kill) the same as Ctrl+C:
    # call httpd.shutdown() from a side thread so serve_forever() exits cleanly.
    signal.signal(signal.SIGTERM,
                  lambda *_: threading.Thread(target=httpd.shutdown, daemon=True).start())
    print(f"[mapserver] http://127.0.0.1:{port}  (web_root={WEB_ROOT})")
    print(f"[mapserver] per-basemap tile caches in {MAPS_DIR}")

    if args.open_browser:
        # open once the socket is accepting, so the first load succeeds
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
        print(f"[mapserver] opening {url} in the system browser")
        print("[mapserver] leave this window open; press Ctrl+C to stop (not Ctrl+Z).")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        httpd.shutdown()
    finally:
        httpd.server_close()
        print("[mapserver] stopped")


if __name__ == "__main__":
    main()
