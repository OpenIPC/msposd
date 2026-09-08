# Phase-1 Spec — Offline Moving-Map Overlay for the Ground Station (x86 PoC)

Status: **IMPLEMENTED.** Phase 1 (this document) — the Python/WebKit ground-station
bridge — ships in `gs/` (`mapserver.py` + `web/` + `mapwin`). Phase 2 — a **native C
moving-map renderer inside `msposd`** that draws directly into the OSD with Cairo, no
browser — has since been built; see **§14**. This document is kept as the design record
for the Python bridge; §14 documents the native in-OSD path (which supersedes the
"native C rewrite deferred to Future" note below).

Scope of the original Phase 1: x86 proof-of-concept, single Python script, `msposd`
unmodified. That gate has been passed on both x86 and the Radxa ground station.

## 1. Objective & Scope

A standalone, offline moving-map aid for the **ground station only**, proven first on
x86/XFCE. It shows aircraft position, course vector, an optional user-set target and
target vector, over offline OSM tiles, at three fixed zoom levels, in a window occupying
~25% of screen, toggled via xbindkeys.

In scope (PoC):
- Aircraft marker (rotates with heading), auto-centered.
- Course/heading vector (projected ahead).
- User-settable target marker + target vector (aircraft → target).
- 3 fixed zooms (z11 / z13 / z15), app-controlled only (no wheel zoom).
- **Hybrid tiles**: served from the offline MBTiles when cached, else proxied live from
  OSM when online — so the user can pan/zoom-browse the world to find their area.
- **Browse-and-download UX**: scroll/zoom to the area, click "Download visible area";
  the current viewport is fetched at z11/13/15 into the MBTiles. Resumable (skips cached
  tiles); a live tile-count estimate guards against over-large jobs (`MAX_TILES`).
- Online/offline status indicator.
- Settings persisted to a config file.
- Runs standalone pre-flight (browse / download / review / planning), no video/`msposd`.

Out of scope (PoC): breadcrumb trail, range rings, multiple waypoints, no-fly zones,
terrain shading, record/playback. North-up only (course-up deferred). MOBAC remains an
alternative way to pre-build the MBTiles, but is no longer required.

Deferred to **Future** (post-PoC): ARM/Buildroot image work, optional rewrite of the
bridge in C/libevent for footprint.

## 2. Architecture

```
upstream MSP (wfb-ng / SITL) ──> msposd --osd --out 127.0.0.1:14560      [UNCHANGED]
                                              │ UDP MSP
                                              ▼
   ┌──────────────────────── mapserver.py (single Python script) ──────────────────────┐
   │  • UDP listener  : parse MSP_RAW_GPS → lat/lon/course; MSP_ATTITUDE → heading       │
   │  • GET /pos      : Server-Sent Events feed (server → page), 1–5 Hz                   │
   │  • GET /tiles/{z}/{x}/{y}.png : tile bytes from MBTiles (sqlite3)                    │
   │  • GET /  /viewer.html /leaflet.js /leaflet.css /rotatedmarker.js /plane.svg         │
   │  • GET /settings  : current target + zoom as JSON                                    │
   │  • POST /settings : set target lat/lon + zoom → persist to config.ini                │
   └────────────────────────────────────┬───────────────────────────────────────────────┘
                                         │ http://127.0.0.1:8088
                                         ▼
                          cog (WPE WebKit) → viewer.html + Leaflet
```

Two processes (`mapserver.py`, `cog`), one launch script. `mapserver.py` is a pure MSP
consumer, exactly like `msposd`. Feed it by running the existing ground `msposd` with
`--out 127.0.0.1:14560` (flag already exists — no `msposd` code change).

Decision: the bridge parses MSP itself (~30 lines) rather than having `msposd` emit JSON.
Rationale — Leaflet (browser) cannot read raw UDP and must fetch tiles + HTML over HTTP
regardless, so a web-facing server process is unavoidable; putting it in `msposd` would
grow `msposd` *and* still require this process. One bridge owning all web-facing work
keeps `msposd` focused.

## 3. File Structure

```
gs/
├── mapserver.py           # single script: UDP MSP parse + HTTP (SSE, tiles, static, settings)
├── web/
│   ├── viewer.html
│   ├── leaflet.js  leaflet.css   # vendored (see fetch-leaflet.sh)
│   └── icons/plane.svg
├── maps/
│   └── area.mbtiles       # offline tiles (z11,z13,z15), produced by MOBAC
├── config.ini             # runtime settings (created/updated by mapserver.py)
├── run-map.sh             # launches mapserver.py + cog
└── README.md
```

## 4. mapserver.py — implementation & routes

Runtime: Python 3 standard library where possible. Recommended deps: `aiohttp` (HTTP +
SSE + async UDP in one loop) **or** stdlib `http.server` + `socketserver` if avoiding
pip; `sqlite3` is stdlib. Pick at Draft; `aiohttp` is the simpler single-loop option.

Port: `8088` (configurable). Bind `127.0.0.1` only.

| Method | Route | Response |
| ------ | ----- | -------- |
| GET | `/` , `/viewer.html` | static `web/viewer.html` |
| GET | `/leaflet.js` etc. | static assets from `web/` (whitelist; no path traversal) |
| GET | `/pos` | `text/event-stream`, one SSE event per update |
| GET | `/tiles/{z}/{x}/{y}.png` | `image/png` tile blob, or 204 if absent |
| GET | `/status` | `{online, mbtiles, center, zoom, max_tiles, download}` |
| GET | `/settings` | `application/json` current target+zoom+last center |
| POST | `/settings` | accept JSON, persist, 200 |
| GET | `/download` | `application/json` download job status |
| POST | `/download` | start download `{north,south,east,west}` → `{started,total}` |

The tile route is a hybrid: MBTiles first, else (zoom in browse range and `/status`
online) a live OSM proxy; otherwise 204. A background probe sets the online flag every
15 s so offline operation never stalls on proxy timeouts.

SSE event payload (at GPS update rate, capped ~5 Hz):
```json
{"lat":43.2111,"lon":27.9111,"heading":217,"course":214,"fix":1,"sats":11}
```
`heading` from MSP_ATTITUDE; `course` from MSP_RAW_GPS. Target is **not** in the feed —
the page owns it (set via /settings), so it survives GPS dropouts.

Static-asset serving: explicit filename whitelist, reject anything containing `..`.

Concurrency: one async event loop runs the UDP receiver and the HTTP server together;
latest fix is held in a shared variable and fanned out to connected `/pos` SSE clients.

## 5. MSP parsing (in mapserver.py)

Reuse the wire layout proven in `osd.c`. Parse only two commands; ignore the rest.

`MSP_RAW_GPS` (cmd 106) payload:
- `[0]` fixType, `[1]` numSat
- `[2..5]`  lat  int32, degrees ×1e7   ← osd.c does NOT read this; we do
- `[6..9]`  lon  int32, degrees ×1e7   ← same
- `[10..11]` alt, `[12..13]` speed
- `[14..15]` groundCourse, decidegrees (÷10)   (osd.c:532 reads alt/speed/course only)

`MSP_ATTITUDE` (cmd 108) payload: `[4..5]` heading (degrees). Matches osd.c:483.

Frame sync: MSP v1 — `$M<dir>` `len` `cmd` `payload` `crc(xor of len..payload)`. Read
full UDP datagrams from `msposd --out` (already aggregated MSP); scan for frames;
validate CRC; drop partials. ~30 lines.

## 6. MBTiles access (sqlite3, stdlib)

```sql
SELECT tile_data FROM tiles
WHERE zoom_level = ?z AND tile_column = ?x AND tile_row = ?ymbt;
```
**Critical:** MBTiles stores rows in TMS (bottom-origin) order; Leaflet/XYZ requests
top-origin `{y}`. Convert before the query:
```
ymbt = (1 << z) - 1 - y
```
Read connections open read-only per request; if the MBTiles file does not exist yet
(before any download) tiles return HTTP 204. Missing tile → 204 (Leaflet shows blank, no
error spam). Send `Cache-Control: max-age=604800` so WebKit caches tiles.

### 6.1 Tile download (pre-flight, online)

`POST /download {north, south, east, west}` (the viewer sends the current map bounds)
starts a background worker; `GET /download` returns `{state, done, total, failed, msg}`
which the page polls. The worker:
- computes the tile x/y ranges of the bbox for z11/z13/z15 (`bbox_tile_ranges` /
  `deg2num`, standard slippy-map);
- refuses jobs over `MAX_TILES` (8000) — guards against a huge area and is polite;
- fetches each missing tile from `tile_url` (default OSM, configurable) with a real
  `User-Agent` and a `tile_delay_ms` gap, inserting into the MBTiles (TMS y-flip),
  `INSERT OR REPLACE`, committing every 10 tiles — so it is resumable;
- records the area center in `[map] center_lat/lon` so the viewer can center on it for
  pre-flight review even with no GPS fix.

Note: bulk OSM tile fetching is subject to the OSM tile usage policy; keep radii modest
or point `tile_url` at your own/another source for large areas.

## 7. viewer.html (Leaflet)

Init:
- `L.map` with `zoomControl:false`, `attributionControl:false`, `scrollWheelZoom:false`,
  `doubleClickZoom:false`, `keyboard:false`, fade/zoom animations off.
- `L.tileLayer('/tiles/{z}/{x}/{y}.png', {minZoom:11, maxZoom:15})`.
- Layers created once, then mutated: `plane` (`L.divIcon` holding plane.svg, rotated via
  CSS `transform`), `headingLine` (polyline), `targetLine` (polyline), `targetMarker`
  (CircleMarker).

Telemetry:
- `new EventSource('/pos')`; on message → `plane.setLatLng`, `plane.setRotationAngle`,
  recompute heading endpoint (~750 m ahead), `map.panTo([lat,lon])`.
- Heading endpoint (flat-earth, fine <1 km):
  `lat2 = lat + d·cos(hdg)/111320`, `lon2 = lon + d·sin(hdg)/(111320·cos(lat))`.

Controls (on-screen buttons; map UX otherwise locked):
- Zoom cycle: `Z1→Z2→Z3→Z1` = setZoom 11/13/15, then `panTo` aircraft.
- Target panel: lat/lon inputs + Set/Clear → `POST /settings`; updates `targetMarker`
  and `targetLine`. Loaded from `GET /settings` on page load.

Locked for PoC: north-up; only the plane icon rotates; target marker visible at all
zooms (CircleMarker, fixed pixel radius).

## 8. cog / WPE launch (x86 / XFCE)

Same `viewer.html` will later run on ARM unchanged; PoC targets x86 only.
- x86 XFCE (X11): run `cog` under Xwayland, or use WebKitGTK (`MiniBrowser` / a tiny GTK
  host) for convenience. Window sized ~25% of screen.
```
cog -O fullscreen=false --geometry=640x640 http://127.0.0.1:8088/viewer.html
```
(Exact geometry / always-on-top flags finalized in Draft.)

## 9. xbindkeys toggle (x86)

Bind a key (e.g. `Alt+M`) in `~/.xbindkeysrc` to a helper that **show/hides** the cog
window (keep WebKit warm — instant, near-zero idle at 1–5 Hz) rather than start/stop.
On X11: `xdotool search --class cog windowunmap/windowmap` (or `wmctrl`).

## 10. Config (config.ini)

```ini
[server]
port = 8088
udp_listen = 127.0.0.1:14560
mbtiles = ./maps/area.mbtiles
web_root = ./web

[map]
zoom = 13            ; last used (11/13/15)
target_lat =          ; empty = no target
target_lon =
```
`mapserver.py` reads at startup, rewrites `[map]` on `POST /settings`.

## 11. Run / setup (no build step)

- `mapserver.py` is interpreted — no compile. `python3 gs/mapserver.py`.
- Deps: Python 3, `sqlite3` (stdlib), optionally `aiohttp` (`pip install aiohttp`).
- Runtime deps: `cog`/WPE (or WebKitGTK) and `xbindkeys`/`xdotool` on the x86 box.
- `run-map.sh` starts `mapserver.py` then `cog`.

## 12. Verify (Phase 4 plan)

1. **mapserver.py:** feed a recorded MSP UDP capture (SITL path in
   `HowToSimulateInFlightMSPData.txt`) → confirm `/pos` SSE emits correct moving
   lat/lon/heading.
2. **Tiles:** `curl /tiles/13/x/y.png` returns bytes; verify a known tile renders
   right-side-up (TMS flip correct) in the browser.
3. **End-to-end x86:** run `msposd ... --out 127.0.0.1:14560`, `mapserver.py`, `cog`;
   drive SITL; confirm marker moves, heading vector follows course, target vector
   tracks a set target, zoom cycles, toggle hides/shows.
4. **Standalone pre-flight:** run `mapserver.py`+`cog` with no `msposd`; confirm map
   browses and target can be set.

## 13. Decisions (locked — approved 2026-06-22)

1. Ports: HTTP `8088`, UDP listen `127.0.0.1:14560`.
2. Tile format: **MBTiles** (single SQLite file).
3. **Python stdlib only** — no `aiohttp`. `ThreadingHTTPServer` + a UDP listener
   thread + per-request SQLite. Runs with zero `pip` install.
4. Toggle: `Alt+M` via xbindkeys, **show/hide** the cog window (keep WebKit warm).
5. Heading vector: **fixed 750 m**.
6. Plane rotation: **CSS-rotated `L.divIcon`**, not the Leaflet.RotatedMarker plugin
   (one fewer offline asset to vendor).

## 14. Phase 2 — Native OSD moving-map renderer (`osd/util/map_render.c`)

Status: **implemented** for ground-station builds (`_x86` and `__ROCKCHIP__`). Unlike the
Phase-1 Python/WebKit bridge, this renderer runs **inside `msposd`** and draws the map
directly into the OSD canvas with Cairo — no browser, no second process. It reads the
**same MBTiles** the preflight tool downloads (read-only), plus `gs/state.ini` (home) and
the landmarks DB (target). It is `#include`d into `osd.c` and composited each frame from
the GS render path. Master switch `[map] enabled=1` in `msposd.ini`; fully inert when `0`.

### 14.1 Config — `[map]` in `msposd.ini`

| Key | Meaning |
| --- | ------- |
| `enabled` | 0/1 master switch (also gates the hotkeys). |
| `mbtiles` | path to the offline MBTiles (resolved next to the binary if relative). |
| `zoom` | requested zoom, snapped to a level stored in the MBTiles. |
| `follow` | `plane` / `north` / `center` / `fit` (see §14.2). |
| `lead`, `plane_y` | plane lead offset (plane mode) / plane vertical position (center mode). |
| `layout` | `corner` (a window) or `full` (whole OSD). |
| `geometry` | `WxH+X+Y` absolute px — overrides the anchor scheme. |
| `corner`,`width`,`height`,`margin` | anchor scheme: named corner + size/margin, each **px or %** of the overlay. |
| `opacity` | 0..100 %. |
| `scalebar` | distance scale bar on/off. |
| `frame` | rounded window frame on/off (§14.4). |
| `on_top` | map over the OSD glyphs (1) or beneath them (0). |
| `recenter_px`,`heading_gate_deg` | re-render thresholds (plane drift / course turn) to keep CPU low. |

### 14.2 Follow modes
- **plane** — north-up; plane offset opposite its course so more map shows ahead.
- **north** — north-up, plane centred.
- **center** — track-up (map rotates on course), plane pinned near the bottom edge.
- **fit** — north-up; zoom+centre to hold **all of plane, home and target** in view
  (bounding box of whatever ≥2 of those points exist; falls back to fixed zoom for one).

### 14.3 Markers & scale
Plane triangle (points along heading, or up in track-up mode), **home** green house,
**target** two concentric rings — **light-red outer + red inner** — and an unrotating
distance scale bar. Tiles are held static and only the plane marker moves between
re-renders.

### 14.4 Window frame
In `corner`/`geometry` layout the tiles are clipped to a rounded rectangle and framed
with a soft outer halo, a 2 px semi-transparent border and a 1 px inner bevel, so the map
reads as a floating window. Screen-space (never rotates in track-up). Skipped for `full`.

### 14.5 Status / problem messages
When the map can't render, the window still draws with a dark panel + framed border and a
centred message so the cause is visible instead of a blank/absent overlay:
- **No offline map file** / `<path>` — MBTiles missing (open is retried on a 3 s backoff,
  so a file that appears after a download recovers on its own — no longer disables the map).
- **Map file has no tiles** / `<path>` — empty MBTiles.
- **Cannot open map** / `<sqlite error>` — corrupt/locked/permissions.
- **No offline map here** / `GPS <lat>, <lon>` — position outside the downloaded coverage.
- **Waiting for GPS fix…** — no plane position or home yet.

### 14.6 Home & target sources
**Home** is read from `gs/state.ini`, resolved **next to the `msposd` binary** (not the
CWD) so it matches mapserver's `APP_DIR/state.ini` regardless of launch directory.
**Target** comes from the landmarks DB via `poi_get_target()`. Both refresh at ~1 Hz and
trigger a recompose when they change.

### 14.7 Controls
- **x86 keyboard** (active only while `enabled=1`): `g` show/hide, `+`/`-` zoom,
  `f` cycle follow mode.
- **Radxa (HW buttons, no keyboard):** planned via the RC/MSP channel stream feeding a
  `map_control()` dispatcher — see [`map_control_via_RC.md`](map_control_via_RC.md).

### 14.8 Tile formats — PNG and JPEG
The native path decodes **PNG** with Cairo's reader and **JPEG** with a vendored
single-file decoder (`osd/util/stb_image.h`, built JPEG-only from `map_render.c`), so
imagery basemaps such as Esri World Imagery render natively as well as in the WebKit map.
JPEG matters for imagery specifically: an aerial tile is ~22 KB as JPEG against ~62 KB for
the same tile as PNG. Accepted formats are declared by `OSD_TILE_FORMATS` in
`gs/mapserver.py`, which greys out any basemap serving something the OSD cannot decode.
See `gs/README.md` for the per-basemap format.
