# Plan — POI Direction Markers on the OSD

Status: **IMPLEMENTED** (2026-06-25). Native (x86) build verified. One simplification
vs the spec: the planned `poi_osd.h` was folded into `poi_osd.c` (single `#include`d
TU, matching `Render_gs.c`), so no separate header exists.

Goal: draw nearby map landmarks (POIs) as small circles + name labels on the OSD,
projected to their real on-screen direction from the aircraft, so the pilot sees
"that city is over there." First iteration: POIs of `kind='place'` only.

## 1. Where it runs

Ground Side (GS) rendering only: `#if defined(_x86) || defined(__ROCKCHIP__)`.
That build links Cairo and runs on the same machine as `gs/`, so it has the
landmarks DB and the `drawCircleGS` / `drawText` primitives in
[osd/util/Render_gs.c](../osd/util/Render_gs.c). On the camera builds
(Sigmastar/Goke, busybox OpenIPC) the feature compiles out entirely.

**No SQLite dependency on the camera/OpenIPC firmware.** Two independent guards:
- The `#include "osd/util/poi_osd.c"` in `osd.c` is itself inside the GS `#if`,
  so `sqlite3.h` is never parsed on a camera build (no header, no symbols).
- `-lsqlite3` is added only to the `native` and `rockchip` link lines; the
  `goke`/`hisi`/`star6*` targets never link it.
`rockchip` is the Radxa ground-side board (per the map plan), not a camera — so
both SQLite-linked targets are genuinely ground side.

## 2. Files

```
osd/util/poi_osd.c   # NEW: DB load + per-frame projection + draw (all logic here)
osd/util/poi_osd.h   # NEW: DrawPOIs() declaration + POI struct
osd.c                # CHANGED: extract plane lat/lon; #include poi_osd.c; one DrawPOIs() call
Makefile             # CHANGED: add -lsqlite3 to `native` and `rockchip` targets
gs/.../config        # CHANGED: enable flag + db path (reuse existing ini parser)
```

`poi_osd.c` is `#include`d from `osd.c` under the GS guard, matching the repo's
existing single-translation-unit pattern (`msposd.c` includes `osd.c` includes
`Render_gs.c`). No SRCS/object reshuffle.

## 3. Data source

`gs/maps/landmarks.db`, table `landmarks`. **Bounding-box query around the plane,
refreshed on movement** (not a full-table load): re-query only when the plane has
moved more than ~¼ of the range since the last load.

```sql
SELECT name, name_en, lat, lon FROM landmarks
WHERE kind='place'
  AND lat BETWEEN :s AND :n            -- lat/lon stored as degrees
  AND lon BETWEEN :w AND :e;           -- bbox = plane ± range, in degrees
```
where `dLat = range_m/111320`, `dLon = range_m/(111320*cos(lat))`.

The returned candidates (a small set) are cached in a static array and scanned
each frame for the exact ±45° / ≤range filter — so there is **no per-frame SQL**,
the render loop stays allocation-free at 5–10 Hz, and DB hits happen only on
meaningful movement. If the file is missing/locked, log once and disable for the
session.

**Indexing:** a normal index on `lat,lon` (or `kind,lat,lon`) makes the bbox query
cheap and needs no schema change. For a future all-kinds expansion (tens of
thousands of rows) the same WHERE clause can be swapped for an R-Tree virtual
table populated by the Python writer (`store_landmarks`); R-Tree filters by bbox,
so C would still refine to the exact sector. Deferred — `place` is too small to
need it. See §7.5.

## 4. Telemetry needed (and the one gap)

Already in [osd.c](../osd.c#L133): `last_heading`, `last_altitude`, `pitch_degree`,
`OVERLAY_WIDTH/HEIGHT`, and the AHI focal length `f` / `pos_y` (boresight screen-y).

**Missing:** plane lat/lon. `MSP_RAW_GPS` ([osd.c:532](../osd.c#L532)) currently
reads only course/alt/speed. Add (3 lines + 2 statics):

```c
last_lat = *(int32_t *)&msp_message->payload[2];   // deg * 1e7
last_lon = *(int32_t *)&msp_message->payload[6];   // deg * 1e7
```

## 5. Per-frame algorithm (`DrawPOIs`)

For each cached POI:

1. **Distance + bearing** from plane (lat/lon ÷ 1e7) via haversine + initial bearing.
2. **Filters:** keep if `distance ≤ RANGE_M` (default 10 000 m) **and**
   `|normalize(bearing − last_heading)| ≤ 45°`.
3. **Horizontal projection** (full-screen width, anamorphic-aware):
   - `HFOV_deg = vFOV_deg * 4/3` (existing convention for 4:3 sensor → 16:9 video).
   - `f_h = (OVERLAY_WIDTH/2) / tan(HFOV_deg/2)`.
   - `az = normalize(bearing − last_heading)`.
   - `x = OVERLAY_WIDTH/2 + f_h * tan(az)`, clamp to `[0, OVERLAY_WIDTH]`.
4. **Vertical projection** (tied to the AHI horizon model, decision §7):
   - Elevation vs horizontal: `el = atan2(ref_alt − plane_alt, distance)`.
   - Approximation: POI at home altitude, `last_altitude` = height above home ⇒
     `ref_alt − plane_alt = −last_altitude`.
   - Screen angle from boresight: `ang = el − pitch_degree`.
   - `y = pos_y − f * tan(ang)` — identical mapping to the pitch-ladder lines at
     [osd.c:1030](../osd.c#L1030), so markers sit consistently with the drawn horizon.
   - Skip if `x`/`y` fall outside the overlay.
5. **Draw:** `drawCircleGS(x, y, 6, COLOR_WHITE, 2, false)` then
   `drawText(label, x + 9, y − 6, COLOR_WHITE, font, false, 1, 0)`.
   `label = name_en` if non-empty else `name` (decision §7).

`Transpose=false`: POIs are projected in absolute screen coords already; we do not
want the additional pitch/roll transform that `drawCircleGS(Transpose=true)` applies.

## 6. Call site & signature

One call in the GS-guarded AHI block, near the course-vector draw
([osd.c:1277](../osd.c#L1277)), where `f`, `pos_y`, `pitch_degree`, `vFOV_deg`,
`last_heading` are in scope:

```c
#if defined(_x86) || defined(__ROCKCHIP__)
  if (POI_Enabled)
    DrawPOIs(last_lat, last_lon, last_altitude, last_heading,
             pitch_degree, pos_y, f, vFOV_deg);
#endif
```

## 7. Decisions (locked with user, 2026-06-25)

1. **Vertical placement: elevation vs AHI horizon** — accurate; reuses `f`,
   `pitch_degree`, `pos_y`. (Rejected: fixed band near horizon — simpler but drifts.)
2. **Label: `name_en`, fall back to `name`** — prefer Latin/English, local name if empty.
3. **Bbox query refreshed on movement, candidates cached, no per-frame SQL** —
   re-query only after the plane moves ~¼ range; per-frame work is an in-memory
   scan of the small candidate set. Scales to all-POI later by swapping the WHERE.
4. **New `.c` `#include`d, not a separate object** — matches existing single-TU pattern.
5. **Altitude approximation** — all `place` POIs at home altitude; `last_altitude`
   treated as height-above-home (relative-alt FCs). User-approved simplification.
6. **SQLite is x86/rockchip-only** — guarded include + per-target link; busybox
   camera/OpenIPC builds have no SQLite dependency. (§1)

### 7.5 Deferred: R-Tree
R-Tree is the right index once we process all POI kinds (large N). It requires the
Python writer to create/populate an `rtree` virtual table alongside `landmarks`.
The C side already queries by bbox, so adopting it later is a WHERE-clause swap.
Not done now: `place` is a few hundred rows, where a plain `lat,lon` index wins.

## 8. Config (reuse existing ini parser: `osd/util/ini_parser.c` / `simple_ini.c`)

| Key | Default | Meaning |
| --- | --- | --- |
| `poi.enabled` | auto | optional master override; when absent, POIs start on only if `db_path` exists and contains landmarks |
| `poi.db_path` | `gs/maps/landmarks.db` | landmarks DB location |
| `poi.range_m` | `10000` | max distance (may later scale with altitude) |
| `poi.fov_deg` | `45` | half-angle filter around heading |

## 9. Build / dependencies

- Add `-lsqlite3` to the `native` ([Makefile:63](../Makefile#L63)) and `rockchip`
  ([Makefile:70](../Makefile#L70)) link lines. Requires `libsqlite3-dev` at build time
  and `libsqlite3` at runtime (already present transitively via system SQLite).
- No change to camera targets (feature compiled out).

## 10. Verify plan (Phase 4)

- Build `native`; run with `sim_msp.py` feeding a known lat/lon/heading/altitude.
- Place a synthetic `place` row at a known bearing/distance in the DB; confirm the
  marker lands at the predicted screen x (and y for a chosen altitude/pitch).
- Heading sweep: marker enters at ±45°, exits beyond it; range cut at 10 km.
- Altitude/pitch sanity: increasing `last_altitude` raises markers toward horizon;
  pitching the nose down lowers them, consistent with the AHI ladder.
- Missing DB: feature disables cleanly, OSD otherwise unaffected.

## 11. Out of scope (later)

- Per-POI elevation from the DB `ele` column (currently approximated to home alt).
- Non-`place` kinds (peaks, water, etc.) and clutter management / nearest-N capping.
- Range auto-scaling with altitude.
```
