# GS offline map — technical reference

Implementation detail for the ground-station map: storage formats, HTTP surface,
configuration keys and the C-side integration. For what the tool *does* and how to
use it, see [`../gs/README.md`](../gs/README.md). For the original design record see
[`offline-map-overlay-spec.md`](offline-map-overlay-spec.md).

Two programs share this data:

| | preflight (`gs/`) | in flight (`msposd`) |
| --- | --- | --- |
| language | Python 3, stdlib only | C |
| writes | `maps/*.mbtiles`, `maps/landmarks.db`, `maps/elevation.db`, `config.ini` | `state.ini [home]` |
| reads | all of the above | the packs + `landmarks.db` + `elevation.db`, read-only |

---

## 1. Storage

### 1.1 Tile packs — `maps/<pack>.mbtiles`

Standard MBTiles: `tiles(zoom_level, tile_column, tile_row, tile_data)` with a unique
index on the triple, plus a `metadata(name, value)` table. Rows are TMS (`tile_row`
counts from the south), so both readers convert: `tms_row = (2^z - 1) - y`.

**Stored zoom levels** are three, derived from the user's *detail zoom* `d`:

```
zooms = [d - 4, d - 2, d]        d ∈ [12, 18], default 15
```

Positional, not fixed numbers — changing `d` moves all three. The C renderer never
assumes these values; it discovers what a pack holds with
`SELECT DISTINCT zoom_level FROM tiles ORDER BY zoom_level`.

**Pack identity** is allocated once for every download. The optional **Map name** is
normalised to a portable ASCII filename stem; when it is empty, the distinct per-zoom
sources are joined coarse→detail to form the stem. `mbtiles_for()` maps that id to
`maps/<id>.mbtiles`.

The server reserves the file with exclusive creation before starting its worker. If the
plain filename exists, it tries `<stem>_YYYYMMDD_HHMMSS.mbtiles` (UTC), followed by
`_<n>` only if another request already claimed that timestamp. Consequently a download
never opens an existing pack for writing. Windows device names such as `CON` are also
prefixed so exported packs remain portable.

A blended download records what it built in the metadata table:

```
sources = 13:OpenTopoMap,15:OpenTopoMap,17:Satellite
```

### 1.2 Points — `maps/landmarks.db`

| table | contents | written by | read by |
| --- | --- | --- | --- |
| `landmarks` | Overpass-fetched named features | preflight download | `poi_osd.c` |
| `poi_selection` | which POI kinds/subtypes are enabled | preflight panel | `poi_osd.c` |
| `waypoints` | user-authored named points | preflight panel | `poi_osd.c` |

```sql
CREATE TABLE waypoints(kind TEXT PRIMARY KEY, lat REAL NOT NULL, lon REAL NOT NULL, name TEXT)
```

Waypoint slots use `kind` values `target`, `target2` … `target5`. **Slot 1 keeps the
historical `kind='target'`**, so a pack authored by an older preflight still shows its
point, and a ground station that only knows about one target still finds it. Clearing
a slot deletes its row rather than blanking it.

`name` is capped at **31 bytes** (not characters) and trimmed on a UTF-8 character
boundary by `clip_name()`, because the C side reads it into `char[32]` where
`snprintf` truncates by byte — a Cyrillic name is ~2 bytes per character and a naive
character-slice would hand the renderer invalid UTF-8.

### 1.3 Terrain elevation — `maps/elevation.db`

Height above sea level for the downloaded area, as decoded metre grids rather than
images. Its own file because terrain does not depend on the basemap, so one DEM serves
every pack. Written when **Download elevation data** is ticked (`[map] elevation`,
default on). Ground builds of `msposd` can query it through
`terrain_elevation_at(lat, lon, &elevation_m)`. `terrain_agl.c` consumes that lookup and
`MSP_RAW_GPS` to calculate numeric AGL for the placeholder and moving-map displays.

```sql
CREATE TABLE meta(name TEXT PRIMARY KEY, value TEXT);
CREATE TABLE elevation(
  zoom INTEGER, tile_x INTEGER, tile_y INTEGER,   -- XYZ, NOT TMS
  width INTEGER, height INTEGER,
  min_m INTEGER, max_m INTEGER,                   -- range without decoding the blob
  data BLOB,                                      -- width*height int16 LE, row 0 = north
  PRIMARY KEY(zoom, tile_x, tile_y));
```

**`tile_x`/`tile_y` are XYZ, not the TMS row order the sibling `.mbtiles` files use.**
Two files in the same folder therefore index rows oppositely; reading this table with
MBTiles' flipped `y` mirrors the terrain north–south, which looks plausible and is
wrong. The convention is also recorded in `meta` (`tile_scheme`, `row_order`).

`meta` is written on every open and makes the file self-describing without this
document: `zoom`, `encoding` (`int16_le`), `compression` (`none`), `nodata` (`-32768`),
`tile_scheme`, `row_order`, `units`, `vertical_datum`, `source`.

Stored uncompressed so any reader can `memcpy`/`frombuffer` it directly. zlib would save
only 1.5–4× on real terrain (measured: 4.1× coastal, 1.8× Rila, 1.5× Mont Blanc) and
`compression` in `meta` leaves room to change that without breaking readers.

**Source and resolution.** AWS Open Data "terrarium" XYZ PNG tiles,
`elev = (R*256 + G + B/256) - 32768`. Fixed at **z12** — ~28 m/pixel at 43°N, matching
the ~30 m native posting of the SRTM-derived data. Higher zooms only interpolate:
measured on three summits, z10 through z15 return the same metre values, so storing
above z12 costs 4× per level for no information.

**Size and the cap.** Every z12 tile is a fixed 256×256 int16 = **128 KiB**, independent
of terrain. A typical 27×14 km area is 8 tiles ≈ 1 MB, but the same bbox at detail z12
would be ~11250 tiles ≈ 1.5 GB — so `MAX_DEM_TILES` (1000, ≈228×228 km / 125 MB) skips the
DEM with a message rather than silently producing that. The DEM covers exactly the tile
download's bbox and does not count against `MAX_TILES`.

**Lookup.** `elevation_at(lat, lon)` bilinearly interpolates the 4 nearest samples
(nearest-sample gives ~28 m stair-steps). Returns `None` — never 0 — outside stored
coverage, so "no data" stays distinguishable from a real sea-level reading. Also exposed
as `GET /elevation?lat=&lon=` and `tiles_info.py --lat --lon`.

The C equivalent is `terrain_elevation_at(double lat, double lon, double *elevation_m)`
in `osd/util/terrain_elevation.c`. It implements the same half-pixel alignment,
bilinear interpolation and edge fallback as Python. It opens the database read-only for
each call and returns `false` for missing coverage, unsupported metadata, malformed tile
data, invalid coordinates or SQLite errors. Failed calls leave the output value unchanged.
Tile dimensions, blob type and exact blob length are validated before sample access, and
global pixel arithmetic is 64-bit to remain defined even with hostile metadata. The source
is linked only into native and Rockchip builds, so Air Unit builds do not gain an SQLite
dependency.

**AGL calculation.** `terrain_agl.c` stores one offset at a witnessed
disarmed→armed transition:

```
altitude_offset = gps_altitude_at_home - terrain_at_home
raw_AGL = current_gps_altitude - terrain_at_current_position
calibrated_AGL = raw_AGL - altitude_offset
```

It uses the latest single `MSP_RAW_GPS` sample without averaging. Calibration requires a
fresh valid fix and terrain coverage at home; a status-before-GPS ordering difference is
tolerated for two seconds. Current terrain is refreshed with each valid GPS message, and
the result becomes unavailable after three seconds without a valid update. The state is
process-local and is not written to `state.ini`. Disarming preserves a successfully
calibrated offset, and subsequent GPS updates continue calculating AGL regardless of the
current armed state. Only a witnessed disarmed→armed transition can replace the offset;
a failed recalibration does not erase an existing offset, and a disarmed GPS sample alone
never calibrates it. Before calibration—including startup while already airborne—the
displayed value is the direct GPS-altitude-minus-terrain difference; after calibration the
offset is applied automatically. At ground-side startup,
`terrain_elevation_available()` enables the logic only when `elevation.db` has the
supported schema and at least one terrain tile. This is independent of both the map
configuration and `!AGL!`, ensuring the arming calibration is captured before either
display is used. The five-character placeholder is replaced in place with rounded metres
(for example, `  42m` or ` -15m`), or `----m` while unavailable. A visible moving map
also draws `AGL <value> m` at its bottom centre from the same cached result.

**Read paths take no `elevation_db_lock`.** That lock serialises the writer, and
`download_elevation()` holds it across its whole tile loop — so a reader that took it
would stall for the entire download. The preflight panel's pointer readout polls
`/elevation` continuously, straight through downloads, so `elevation_at()` and
`elevation_summary()` open `mode=ro` connections and rely on SQLite's own locking plus
`busy_timeout`; both already degrade to `None`/empty on `sqlite3.Error`. The lock remains
for the writer and for `serve_export()`, which needs a consistent snapshot. Measured: 55
pointer lookups served during an 8.6 s download, worst case 80 ms.

The readout polls at 150 ms and skips movements under ~11 m — finer than the 28 m grid,
so the interpolated value cannot have changed meaningfully. Typical lookup is 1.6 ms.

> **Datum.** These are metres above the **geoid** (EGM96/MSL). Raw GNSS height is above
> the WGS84 **ellipsoid**, ~35 m higher in Bulgaria. Directly subtracting terrain MSL
> from ellipsoidal GPS altitude would therefore be wrong by tens of metres. The home
> `altitude_offset` calibration above cancels that initial constant datum difference;
> subsequent GPS vertical drift remains.

What the data is worth, how it was verified, and the home-calibrated AGL implementation are in
[`terrain-elevation.md`](terrain-elevation.md).

### 1.4 Runtime state — `state.ini`

Holds `[home]` only: captured live on the station at the disarmed→armed transition, so
it is station-local rather than part of the pack. Written atomically (temp file +
`os.replace`) so a concurrent C-side read never sees a half-written file. A legacy
`[target]` section is honoured once and migrated into `waypoints`.

**Consequence:** the preflight→flight handoff is DB-only. Tiles and points travel;
only `[home]` stays behind.

---

## 2. HTTP surface (`mapserver.py`)

Served on `127.0.0.1:<port>` (default 8088, `[server] port`). Single-instance by port:
a second launch that finds a *responding* server just reopens the browser and exits.

| method | path | purpose |
| --- | --- | --- |
| GET | `/` `/viewer.html` | the app shell (`Cache-Control: no-store`) |
| GET | `/pos` | SSE stream of parsed MSP position/heading/armed |
| GET | `/status` | everything the panel needs (see below) |
| GET | `/settings` · POST | zoom, detail zoom, basemap, per-zoom sources |
| GET | `/tiles/{z}/{x}/{y}?src=&offline=` | cache first, then live proxy |
| GET | `/cache?src=` | per-zoom counts, covered box, size, format, elevation summary |
| GET | `/packs` | every downloaded pack with sources, zooms, counts, size and bounds |
| DELETE | `/packs?id=` | delete one existing regular pack file; shared databases remain |
| GET | `/coverage?src=&z=&n=&s=&e=&w=` | which tiles are present in a bbox |
| GET/POST | `/download` | progress / start a bbox download (see phases below) |
| GET | `/export?src=` | zip of the pack + `landmarks.db` + `elevation.db` |
| GET | `/landmarks` `/poi-types` · POST `/poi-selection` | POI data and selection |
| GET | `/elevation?lat=&lon=` | terrain height in m (`null` outside coverage) |
| GET | `/viewshed?lat=&lon=&ant=&alt=&max_km=&az=` | terrain line-of-sight horizon ring |
| POST | `/target` | waypoint slots |

`/status` carries `zooms`, `detail_zoom`, `detail_min`/`detail_max`, `sources`, `pack`,
`basemaps`, `basemaps_disabled` (name → reason), `targets`, `target_slots`, `home`,
`max_tiles` and download progress. `target` (slot 1 alone) is repeated under the old
key for overlay clients that predate multiple waypoints.

`/target` accepts `{"targets":[{name,lat,lon}|null, …]}`; a shorter list leaves later
slots untouched, so the panel can post only what it edited. The legacy `{lat,lon}`
body still works and writes slot 1.

`?src=` names a **pack**. Configured packs remain one or more known basemap names joined
by `+`. `/packs` also exposes filename-stem ids for regular `.mbtiles` files discovered
directly inside the configured maps directory, allowing preflight to inspect imported or
previously configured packs. `resolve_pack()` accepts only those two forms; stems are
restricted to safe characters, must already exist and cannot be symlinks, so an arbitrary
query string cannot escape `maps/`.

The preflight viewer keeps its configured online-preview `PACK` separate from
`VIEW_PACK`. Basemap/per-zoom controls define the sources for `/download`, but the server
allocates a fresh pack id for every request. The selected view pack is used only while
**Show downloaded** or **Offline mode** is active; tile and coverage requests then use
that pack id and its actual stored zoom list with network fallback disabled. Selection
is browser-local and does not rewrite either map configuration file. Cache-only zoom
navigation treats that stored list as a discrete sequence: a one-level Leaflet request
is redirected in the requested direction to the next available level. Enabling either
mode while centred outside the selected pack fits its bounds before snapping to a stored
zoom, avoiding an unexplained blank view.

`DELETE /packs?id=` applies the same safe-id and regular-file checks as pack selection,
rejects the pack currently being downloaded, and unlinks only its `.mbtiles` file. The
confirmation is performed in the viewer; `landmarks.db` and `elevation.db` are shared by
all packs and are deliberately untouched.

---

## 3. Tile formats

| basemap | format | bytes/tile @z15 |
| --- | --- | --- |
| Satellite / Streets / Topo (Esri) | JPEG (baseline, SOF0) | 22–30 KB |
| OpenTopoMap, Thunderforest | PNG | ~37 KB |

Both render in both places. Cairo reads PNG natively; JPEG is decoded by a vendored
single-file decoder, `osd/util/stb_image.h` (stb_image v2.30, MIT/public domain), built
`STBI_ONLY_JPEG` + `STBI_NO_STDIO` and used only from `map_render.c` — so camera builds
never see it and no new system dependency is introduced.

`map_decode_tile()` sniffs the SOI marker (`FF D8`) per *tile*, not per file, which is
what lets a blended pack hold PNG at the coarse levels and JPEG at the detail level.
Dimensions are rejected above 2048×2048 before decoding, and a failed decode is treated
as a missing tile.

Which formats the OSD accepts is declared by `OSD_TILE_FORMATS` in `mapserver.py`; a
basemap serving anything else is greyed out in the panel with its reason. Widening that
set is the only change needed if another format gains a decoder.

Aerial imagery is essentially always JPEG — the same Esri tile served as PNG is ~62 KB
against ~22 KB — so JPEG support is what makes imagery packs affordable to store.

---

## 4. Download budget

Tiles quadruple per zoom level, so the area that fits in `MAX_TILES` (12000) shrinks by
4× per step. Measured against a real cache (model predicted 7267 tiles for a 57×102 km
area at z15; the pack actually held 7233):

| detail | m/pixel @43°N | tile covers | max square @12000 tiles |
| --- | --- | --- | --- |
| z12 | 29 m | 14.3 km | ~1500 km |
| z14 | 7.2 m | 1789 m | 190 km |
| z15 | 3.6 m | 894 m | 95 km |
| z16 | 1.8 m | 447 m | 47 km |
| z17 | 0.9 m | 224 m | 24 km |
| z18 | 0.45 m | 112 m | 12 km |

Ground resolution is `156543.03 × cos(lat) / 2^z`; the panel quotes it at ~43° as a
fixed guide. Note the map's resolution outruns GPS accuracy past roughly z16 — beyond
that you are buying visual context, not better positioning.

Individual tile failures retry 3×. Every download gets a newly allocated pack, even
when the source and bbox match an earlier request, so an existing working map is never
modified. The shared elevation database remains reusable: terrain tiles already present
there do not need to be downloaded again.

**Progress is reported per phase.** `dl_status` carries `phase` — `tiles` → `poi` →
`elevation` → `done` — alongside `done`/`total`, and the UI's progress bar restarts for
each. Without this the tile phase reached 100% and then sat there through the POI and
elevation work, which reads as a hang. `download_elevation()` takes a `progress(done,
total)` callback and counts every planned tile including already-cached terrain, so the
bar advances rather than jumping. The POI phase is a single Overpass request with no
granularity, so it shows a full bar and its own label instead of a fake percentage.

### 4.1 Transport

`_tile_get()` fetches over a **kept-alive connection**, pooled per `(scheme, host)` in a
`threading.local()`. A fresh TLS handshake per tile used to cost ~125 ms — more than the
~40 ms transfer itself — so reuse is the dominant download speedup, and it lowers load on
the tile server rather than raising it.

Measured on 104 Esri tiles (z11/13/15), same bbox each run:

| transport | tile_delay_ms | tiles/s | ms/tile |
| --- | --- | --- | --- |
| one `urlopen` per tile (pre-change) | 60 | 4.6–7.9 | 127–217 |
| kept-alive | 60 | 12.3 | 81 |
| kept-alive | 30 | 20.7 | 48 |
| kept-alive | 10 | 39.6 | 25 |
| kept-alive | 0 | 72.0 | 14 |

With the handshake gone, `tile_delay_ms` (default 60) dominates: it is now ~4× the actual
per-tile work, where before it was a ~35% surcharge. Lowering it is the next lever, but it
is a politeness setting — commercial CDNs (Esri) do not need it; the volunteer-run
OpenTopoMap asks not to be strained by mass downloads.

Two behaviours the pool must preserve, both covered by the retry in `_tile_get()`:
a server that closes an idle keep-alive socket makes the *next* request fail, which must
be retried transparently on a fresh connection rather than counted as a tile failure; and
the response body must always be drained (even on 404) or the socket cannot be reused.
Redirects are followed manually — `http.client` does not, whereas `urllib` did — which
matters for custom `[server] tile_url` providers.

### 4.2 Why throughput is bursty

Downloads run in bursts — long fast stretches broken by stalls. Ranked by contribution:

**Sporadic backend renders.** OpenTopoMap serves most of the world pre-rendered, but that
coverage thins out at z16–z18 over unremarkable terrain. A tile already on the backend's
disk costs ~40 ms; one that must be rendered costs **1.2–2.3 s**. mod_tile renders a whole
8×8 metatile at a time, so a single stall makes its ~63 neighbours fast — hence a stall
followed by a burst.

Do **not** read `X-Cache-Status: MISS` as "this tile was rendered". It only reports the
nginx edge cache. Measured at z16 over rural terrain, twelve consecutive tiles across a
metatile boundary all returned MISS at ~42 ms each, boundary tiles included — the backend
already had them. Stalls are sporadic and clustered where pre-rendered coverage runs out,
**not** periodic, and there is no fixed render-per-N-tiles ratio to plan around.

**Unequal zoom levels.** The worker walks the stored zooms in order, coarse first. The two
coarse levels are a few dozen tiles and effectively always pre-rendered; the detail level
is ~94% of the tiles and by far the most likely to be cold. A fast opening followed by a
long slower tail is structural, not a fault.

**Fresh-pack writes.** Every download starts with an exclusively reserved empty file, so
the tile lookup normally finds no rows and the worker fetches the complete requested
area. This intentionally gives stronger protection than resume-in-place: a completed
working pack is never opened for writing or partly changed by a later download.

**Mixed sources.** A topo-coarse/imagery-detail pack changes character at the level
boundary: Esri is a CDN with no render queue and is uniformly fast.

**Reporting granularity.** `set_dl()` and the SQLite commit fire only on `done % 10 == 0`,
so progress advances in steps of ten tiles — chunkier-looking than the underlying flow.

The variance predates the kept-alive change; that change only unmasked it. A constant
~125 ms handshake plus the 60 ms sleep once made a 1.5 s render ~7× a normal tile; against
a ~100 ms tile it is ~18×. The one client-side remedy is concurrency, which overlaps the
render waits instead of serialising them — the case where parallel workers buy more than
raw throughput.

The `for x: for y:` scan order walks metatiles coherently (a column of 8 y-values stays
within one metatile row, and the next 7 x-columns reuse whatever it rendered), so it
should not be "optimised" into something that scatters across them.

---

## 5. Configuration keys

### `gs/config.ini` (preflight; written by the panel)

| section | key | meaning |
| --- | --- | --- |
| server | `port` | HTTP port (default 8088) |
| server | `udp_listen` | MSP input, default `127.0.0.1:14560` |
| server | `mbtiles` | path whose *directory* becomes `maps/` |
| server | `tile_url` | custom source when `basemap` is not built in; fields `{z} {x} {y} {s} {key}` |
| server | `tile_key` | API key for keyed providers (gitignored) |
| server | `tile_delay_ms` | per-tile download throttle |
| map | `zoom` | last viewer zoom |
| map | `detail_zoom` | highest stored level, 12–18 |
| map | `sources` | per-zoom basemaps, coarse→detail; empty = use `basemap` for all |
| map | `basemap` | the single/uniform source |
| map | `elevation` | download terrain elevation with the tiles (default 1) |
| map | `center_lat` / `center_lon` | last downloaded centre |

### `msposd.ini` `[map]` (in-flight renderer)

`enabled` (default 0 — absent means off, i.e. previous behaviour), `mbtiles`,
`zoom`, `follow` (plane/north/fit/center), `lead`, `plane_y`, `layout`, `geometry`,
`corner`/`width`/`height`/`margin`, `opacity`, `scalebar`, `frame`, `on_top`,
`recenter_px`, `heading_gate_deg`, `track_vector`, `track_vector_len`,
`track_vector_min_speed`, `track_vector_alpha`, `help_font_size`.

`recenter_px` (default 48) is the drift threshold in *world pixels* that triggers a tile
recompose. World pixels per metre double with each zoom level, so at z18 the aircraft
crosses it 8× as often as at z15 — raise it if a high detail zoom costs too much CPU on
a small board.

If `mbtiles` names a file that does not exist, the first `*.mbtiles` in that folder is
loaded instead, so a renamed or rebuilt pack still shows a map.

---

## 6. C-side integration

Both files are `#include`d into `osd.c` inside `#if defined(_x86) || defined(__ROCKCHIP__)`,
so camera builds never compile them, never link sqlite3 and never see the JPEG decoder.

### `osd/util/poi_osd.c` — points

Reads waypoint slots with

```sql
SELECT lat, lon, name FROM waypoints WHERE kind='target' OR kind LIKE 'target_' ORDER BY kind
```

into `poi_targets[POI_TARGETS]`, refreshed at most once a second. `ORDER BY kind` puts
`target` first, then `target2`…`target5`. Exposed to `map_render.c` via
`poi_target_count()` and `poi_get_target_at()`. A missing file, missing table, missing
column or corrupt database all degrade to zero waypoints — verified against each case.

`DrawTarget()` runs *before* the `poi_enabled` check, so waypoints render even with the
POI feature switched off. Each draws a two-ring crosshair plus `"<name> <dist>"` in red.

### `osd/util/map_render.c` — the map

- Opens the pack read-only, discovers its zoom levels, snaps the requested zoom to the
  nearest available (`map_snap_zoom`).
- Composites tiles into a cached surface, redrawn only when the view really changes;
  the plane marker is stamped per frame over a copy, so the map holds still between
  recomposes.
- Keeps 128 decoded tile surfaces in an LRU keyed by `(z, x, y)`.
- `map_close_db()` finalises the prepared statement *before* closing the handle (sqlite
  refuses otherwise), resets the zoom list, and **flushes the tile LRU** — those entries
  carry no pack identity, so a switch without the flush would draw the previous pack's
  tiles.
- Pack cycling (`h`) lists `*.mbtiles` in the pack's folder alphabetically, matching an
  exact `.mbtiles` ending so sqlite's `-journal` side files are skipped. A pack that
  fails to open is skipped; if every candidate fails the previous pack is reopened.
  Runtime only — `msposd.ini` is not rewritten.
- Showing the map starts independent three-second filename and keyboard-guide timers.
  The filename is centred at the top; `G Hide`, `F Mode`, `H Type`, `P POIs` and
  `−/+ Zoom` are drawn down the left edge. Pack cycling restarts only the filename timer.
- Fit mode frames plane and home only. Waypoints are deliberately excluded: one distant
  point would zoom the map out far enough to lose all detail around the aircraft.

`POI_TARGETS` comes from `poi_osd.c`, included one line earlier in `osd.c`; `map_render.c`
carries an `#ifndef` fallback so it still compiles if that order changes.

### `osd/util/Render_gs.c` — input

Map keys are grabbed on the X root window only when `[map] enabled=1`; with the feature
off nothing new is grabbed and the keys pass through to the desktop. Each grab is
registered for the four NumLock/CapsLock combinations so an active lock key does not
silently disable them. Single-action keys drop X autorepeat.

**x86 only.** The Radxa ground station runs an X server but has no keyboard; control
there is planned over the RC/MSP channel stream — see
[`map_control_via_RC.md`](map_control_via_RC.md).

---

## 7. Packaged build

`gs/pack/mapserver.spec` freezes `mapserver.py` + `web/` into one binary with
PyInstaller. When frozen, `APP_DIR` is the executable's directory (so writable data sits
next to it) while `RES_DIR` is `sys._MEIPASS` (the bundled read-only assets), and
`--open-browser` defaults on. Editing files under `gs/` has **no effect** on an existing
binary until it is rebuilt — a frozen build carries its own snapshot, and its `maps/`
folder is separate from `gs/maps/`.

Moving the one-file executable changes `APP_DIR`. The new directory therefore gets its
own `config.ini`, `state.ini` and `maps/`; these must be copied beside the executable when
migrating an existing setup. A second process using the default HTTP port does not open
that second data directory: single-instance handling reuses the server already bound to
the port. Use a different `--port` or stop the first process when testing two locations.

The PyInstaller bootloader is host-platform-specific. `build.sh` on Linux or WSL creates
an ELF executable, while `build.bat` under native Windows creates the PE `.exe`. There is
no supported suffix change or direct Linux-to-Windows conversion; build on the target OS
or download the matching CI artifact.
