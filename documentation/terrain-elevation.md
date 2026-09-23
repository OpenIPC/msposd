# Terrain elevation — the DEM layer and height above ground

Preflight can download the terrain height of the flying area alongside the map tiles and
store it as a grid of metres above sea level. This document covers what that layer is,
what it can and cannot tell you, and the implemented AGL (height above ground) calculation
and displays used in flight.

**Status**

| | state |
| --- | --- |
| Download + storage (preflight) | **built** — checkbox on by default |
| Lookup API (`elevation_at`, `/elevation`, `tiles_info.py`) | **built** |
| Pointer readout on the preflight map | **built** |
| Link horizon (viewshed) — [§5](#5-link-horizon-viewshed) | **built** |
| C lookup (`terrain_elevation_at`) | **built** — ground builds only |
| Ground-side AGL calculation | **built** — [§4](#4-agl-by-home-calibration) |
| `!AGL!` OSD replacement | **built** — ground builds only |
| Bottom-centre map AGL | **built** — ground builds only |

Storage schema lives in
[`gs-map-internals.md`](gs-map-internals.md) §1.3 and is not repeated here.

---

## 1. Where the data comes from

AWS Open Data **"terrarium"** terrain tiles: ordinary XYZ PNG tiles whose pixels encode
metres rather than colour.

```
https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png
elev_m = (R * 256 + G + B / 256) - 32768
```

No API key, no registration. It is an Open Data bucket with no announced SLA, and the
underlying data is a mosaic (SRTM and others) carrying per-source attribution
obligations. Because everything is cached into `elevation.db` at download time, an
outage upstream only ever affects downloading — never a flight.

**Mapbox Terrain-RGB** is the better-maintained, Copernicus-based alternative and uses a
different encoding (`-10000 + (R*65536 + G*256 + B) * 0.1`). It needs an API key, which
is why it is not the default.

### Why z12, and why not higher

z12 is ~28 m/pixel at 43°N, which matches the ~30 m native posting of the source data.
Higher zoom levels only interpolate — they return no new information while costing 4×
the tiles per level. Measured on three summits:

| point | z10 | z12 | z14 | z15 | actual |
| --- | --- | --- | --- | --- | --- |
| Musala | 2893 | 2894 | 2894 | 2895 | 2925 |
| Mont Blanc | 4757 | 4779 | 4780 | 4781 | 4808 |

z12 through z15 agree within 1–2 m. The layer is therefore fixed at z12 and the panel
offers no zoom choice for it.

The residual ~30 m underestimate on sharp summits is the 30 m grid averaging a peak away,
not a decoding error — see the accuracy check below.

---

## 2. What it can and cannot tell you

**It is bare-earth terrain, not a picture of what you would hit.**

- **No obstacles.** No buildings, masts, power lines or towers.
- **Vegetation reads high.** The SRTM-derived source is radar; over forest the return
  comes from within the canopy rather than the ground, so wooded areas are
  systematically *higher* than the real ground by a substantial fraction of the tree
  height.
- **Vertical accuracy** is roughly ±16 m at 90% confidence, and worse on slopes.

This is terrain awareness — "the ground is rising ahead", "that valley is 200 m lower" —
not obstacle clearance. It will happily report 40 m of clearance over a forest whose
trees are 35 m tall.

> **Datum.** Values are metres above the **geoid** (EGM96 / mean sea level). Raw GNSS
> height is above the WGS84 **ellipsoid**, about 35 m higher in Bulgaria. Mixing the two
> produces an error the height of a ten-storey building. [§4](#4-agl-by-home-calibration)
> deals with this.

---

## 3. Verification record

Decoding and storage were checked against independent references rather than assumed.

**Decoder.** `mapserver.py` is stdlib-only, so `decode_png_rgb()` implements the narrow
PNG subset these tiles use (256×256, 8-bit, colour type 2, non-interlaced): parse IHDR,
concatenate IDAT, `zlib.decompress`, unfilter the five scanline filters. Verified
byte-identical to PIL on a real tile (1369 sampled pixels, **0 mismatches**), ~32 ms per
tile. Malformed input — empty, truncated, JPEG, non-PNG — raises `ValueError` rather than
escaping as `zlib.error`.

**Absolute accuracy**, decoded values against known heights:

| point | known | decoded | diff |
| --- | --- | --- | --- |
| Black Sea off Varna | 0 | **0.0** | 0.0 |
| Dead Sea shore | −420 | −412 | +8 |
| Musala | 2925 | 2894 | −31 |
| Mont Blanc | 4808 | 4780 | −28 |

Open water reading exactly 0.0 is the strongest single check that the encoding and the
sign convention are right. The summit shortfalls are grid averaging, consistent across
zoom levels.

**Pipeline.** `elevation_at()` agrees with a direct PIL decode of the same tile to within
**0.4 m** (int16 rounding plus bilinear-vs-nearest) at five points, and correctly returns
`None` — not 0 — outside the downloaded box.

**Storage cost.** Every z12 tile is a fixed 256×256 int16 = **128 KiB**, independent of
terrain. A typical 27 × 14 km area is 8 tiles ≈ 1 MB. Stored uncompressed for direct
`memcpy`/`frombuffer` access; zlib was measured at only 4.1× (coastal), 1.8× (Rila) and
1.5× (Mont Blanc) on real terrain, and `compression` in `meta` leaves room to revisit it.

**The cap.** `MAX_DEM_TILES = 1000` (~228 × 228 km, ~125 MB) — sized so a line-of-sight
horizon runs into terrain rather than into the edge of the data. Without it, a download at
detail z12 spans the same bbox as ~11250 z12 DEM tiles ≈ **1.5 GB**. Oversized areas skip
the DEM with a message rather than silently producing that.

---

## 4. AGL by home calibration

The ground-side C lookup needed by this calculation is now available as
`terrain_elevation_at(lat, lon, &elevation_m)` in `osd/util/terrain_elevation.c`.
It reads `gs/maps/elevation.db` directly, returns `false` when the coordinate is
not covered or the input/database is invalid, and has no dependency on the Python
mapserver. Failures leave the caller's output unchanged. Home calibration and numeric
AGL are implemented in `osd/util/terrain_agl.c`; the ground renderer injects the result
into `!AGL!` placeholders.

### The idea

At a witnessed disarmed→armed transition, `terrain_agl.c` uses the latest valid
`MSP_RAW_GPS` position and GPS altitude. At that moment the aircraft is on the ground at
a known position, so the difference between its reported altitude and the DEM's height
there is stored as one calibrated offset:

```
offset = alt_home − terrain(home)
AGL    = alt_now − offset − terrain(here)
```

### Why it works — and what it quietly simplifies

Expanding the substitution:

```
AGL = (alt_now − alt_home) + terrain(home) − terrain(here)
```

**The absolute altitude cancels.** Only the *change* in altitude since arming survives,
plus the terrain difference between home and the current position. Three consequences:

1. **The geoid/ellipsoid problem disappears.** The ~35 m separation appears in both
   altitude terms and subtracts out, and it varies well under a metre across any area
   you would fly. This is the largest single error the calibration removes, and it
   removes it cleanly.
2. **One synchronized message supplies position and altitude.** Both values come from
   `MSP_RAW_GPS`, so GPS position is not mixed with a separately timed fused or
   barometric altitude. Home calibration removes the initial GPS bias, but not vertical
   drift later in the flight.
3. **DEM regional bias partly cancels too**, to the extent it is constant across the
   flying area.

### What it does *not* fix

The offset is only as constant as the DEM's error, and that error is not spatially
uniform. Calibrating over an open field measures the error *for open fields*; two
kilometres away over woodland the DEM can be 10–20 m optimistic about where the ground
is, in exactly the place that matters most.

**Slope sensitivity at the calibration point** is the other trap. The offset samples one
location, so horizontal GPS error at home converts into vertical error through the local
slope. Measured on downloaded terrain — the spread of DEM values within a
GPS-error-sized neighbourhood:

| home terrain | spread within ±5 m | within ±30 m |
| --- | --- | --- |
| flat / gentle (city, hilltop) | 0.4 m | 2–3 m |
| broken ground (valley edge, coast) | 3.3 m | **18.9 m** |

Where you arm changes calibration quality by an order of magnitude. Arming on flat open
ground yields a sub-metre offset; arming beside an embankment can poison it by metres
before takeoff.

### Runtime behaviour

- The latest individual GPS sample is used at arming; readings are not averaged.
- At ground-renderer startup, `terrain_elevation_available()` enables the calculation
  only when `gs/maps/elevation.db` opens read-only with the supported schema and contains
  terrain tiles. Neither `[map] enabled` nor the presence of `!AGL!` controls calculation.
- Each `!AGL!` placeholder is display-only: it is replaced with rounded metres in a
  five-character field, or `----m` while no trustworthy value is available.
- When the moving map is visible, it shows `AGL <value> m` at its bottom centre. The map
  reads the cached AGL result each frame and does not trigger another terrain lookup.
- Calibration requires a disarmed state witnessed by this process, preventing a restart
  in flight from treating the current position as home.
- If MSP status arrives before GPS, the first valid GPS sample within two seconds may
  complete calibration. It is never performed later in the flight.
- Before calibration, each valid GPS update reports the direct difference
  `gps_altitude − terrain(here)`. This remains useful but may contain the GPS/DEM datum
  difference described above.
- If `terrain(home)` is unavailable, no offset is created; direct uncalibrated AGL can
  still be reported wherever current terrain is available.
- Once calibrated, each valid GPS update applies the offset and refreshes the numeric AGL
  value regardless of the current armed state.
- A GPS fix older than three seconds makes the result unavailable.
- Disarming preserves the offset and the displayed AGL. The next witnessed
  disarmed→armed transition replaces it after a successful fresh calibration; a failed
  attempt does not erase an existing offset. A disarmed GPS sample alone can never
  establish an offset.
- Starting while already armed cannot safely establish an offset, so direct uncalibrated
  AGL is shown until this process witnesses a later disarmed→armed transition.
- Negative AGL is retained as useful evidence of terrain, GPS, or DEM error.

### Expected quality

After calibration the initial datum and receiver bias — the part that would otherwise be
wrong by tens of metres and constant — is gone. What remains is GPS altitude drift and
DEM error against real ground:
roughly ±10–20 m in vegetated or steep terrain, better over open flat ground. Good enough
for terrain awareness and a rising-ground warning. Not good enough to fly a low pass on.

---

## 5. Link horizon (viewshed)

Built. The **Link horizon** panel draws where terrain stops blocking the line of sight
from a point the user clicks, given an antenna height and an aircraft altitude.
The boundary and filled centre marker are dark blue so the computed area and antenna
position share one visual identity. `GET /viewshed?lat=&lon=&ant=&alt=&max_km=&az=`
returns the ring.

The panel arms with a checkbox; the next map click sets the centre, further clicks move
it, and any parameter change re-runs at the same point. Unticking clears the ring.
Arming is **mutually exclusive with waypoint placement** — a map click must do exactly
one thing — so enabling one disarms the other in both directions.

### Method

Classic radial sweep (the R2 algorithm). For each azimuth, walk outward one DEM sample
at a time tracking the running maximum terrain *elevation angle*; the aircraft is
visible while the angle needed to see it still exceeds that.

```
drop(r) = r² / (2 · k · R_earth)              k = 4/3, standard radio refraction
terrain angle  a(r)    = (z(r)   − drop(r) − h_obs) / r
angle to target n(r)   = (h_t    − drop(r) − h_obs) / r
blocked when n(r) < max(a) so far
```

`drop` **must be applied to the aircraft as well as the terrain.** Subtracting it only
from the terrain makes curvature *lengthen* the horizon, which is backwards — over flat
ground it yields an infinite horizon instead of ~50 km. This was a real bug caught by
the analytic test below.

Because `n(r) = (h_t − h_obs)/r − r/(2kR)` decreases strictly while `max(a)` only rises,
each azimuth blocks exactly once. The result is a single closed ring rather than
disconnected patches — which is only true because the aircraft altitude is **fixed**. An
AGL-referenced target would make `h_t` follow the terrain, break monotonicity, and
require a raster instead.

Fresnel-zone clearance is deliberately **not** modelled: the result is a terrain-only
bound, and 60% of the first Fresnel zone is 3–7 m at typical ranges — far below the
DEM's own ±16 m error.

### Validation

Against synthetic flat ground the sweep reproduces the textbook radio horizon
`√(2kR·h_ant) + √(2kR·h_aircraft)` exactly:

| antenna | aircraft | computed | theory |
| --- | --- | --- | --- |
| 5 m | 100 m | 50.4 km | 50.4 km |
| 5 m | 300 m | 80.6 km | 80.6 km |
| 15 m | 100 m | 57.2 km | 57.2 km |

On real terrain near Varna (5 m / 100 m): median 5.8 km, min 1.9, max 23.4. Raising the
aircraft to 300 m takes the median to 12.2 km; raising the *antenna* from 2 m to 20 m
moves it from 5.7 to 6.3 km. **Aircraft altitude buys far more horizon than antenna
height.**

Cost is ~310 ms for 360 azimuths over 30 km, run synchronously in the request. Tile
loading is bounded by the requested radius (a 30 km disc is ~36 tiles), not by the size
of the DB.

### What it is not

**An upper bound, not a prediction.** The DEM is bare earth, so the trees, buildings and
masts near the antenna — which usually decide a real link — are absent from the data. A
20 m treeline 100 m away blocks everything below ~9° while the computed ring happily
extends 20 km. The panel says *"terrain alone won't block you closer than this"* for
exactly this reason, and that wording should not be softened.

The other limit is coverage: azimuths that run off the downloaded area stop there and
are counted separately, with the panel warning when it happens. A horizon is only as
wide as the DEM behind it — hence the 1000-tile cap.

---

## 6. Reference

- [`gs-map-internals.md`](gs-map-internals.md) §1.3 — `elevation.db` schema, metadata
  keys, XYZ-vs-TMS warning, lookup API.
- [`../gs/README.md`](../gs/README.md) — the user-facing capability.
- `gs/mapserver.py` — `decode_png_rgb()`, `terrarium_to_int16()`, `download_elevation()`,
  `elevation_at()`, `elevation_summary()`, `viewshed()`, `_dem_area()`.
