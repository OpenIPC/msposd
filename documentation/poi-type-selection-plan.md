# Plan — POI Type/Subtype Selection (preflight tree → OSD filter)

Status: **IMPLEMENTED** (2026-06-26). Server + viewer + OSD done; native build and
end-to-end DB flow (seed → /poi-types → /poi-selection → OSD JOIN) verified. The
landmarks `kind` column is noisy (88 kinds incl. `bus`/`bench`/`addr:city`); the
tree renders the raw GROUP BY faithfully, collapsed-by-default and scrollable.

Goal: in `./map.sh preflight`, show a checkbox tree of the landmark `kind`/`subtype`
groups (with counts) at the **bottom** of the map. The user checks which types to
draw on the OSD. The selection is **persisted in landmarks.db** and reloaded into
the tree on the next map load, and the OSD ([poi_osd.c](../osd/util/poi_osd.c))
draws only the selected `kind`/`subtype` pairs.

Builds on [poi-osd-direction-plan.md](poi-osd-direction-plan.md).

## 1. Data model — new table `poi_selection`

In `gs/maps/landmarks.db`, created by `open_landmarks_db()` alongside `landmarks`:

```sql
CREATE TABLE IF NOT EXISTS poi_selection(
  kind    TEXT NOT NULL,
  subtype TEXT NOT NULL,        -- '' when the landmark has no subtype
  enabled INTEGER NOT NULL,     -- 0/1
  PRIMARY KEY(kind, subtype)
);
```

`subtype` is normalized to `''` (never NULL) so the primary key and the OSD join
are simple. A row exists per (kind, subtype) the user has touched; absence = unset.

### Default (decision: place-only)
On first use (table empty), the server **seeds** `enabled=1` for every (kind,
subtype) where `kind='place'` and `enabled=0` for the rest — preserving today's OSD
behavior until the user changes it. Seeding happens lazily when `/poi-types` is
first served and the table has no rows.

## 2. Server — `gs/mapserver.py`

### Schema
- `open_landmarks_db()`: add the `CREATE TABLE IF NOT EXISTS poi_selection(...)`.

### `GET /poi-types`
Returns the grouped tree joined with the saved selection:

```sql
SELECT l.kind,
       COALESCE(NULLIF(l.subtype,''),'') AS subtype,
       COUNT(*) AS cnt,
       COALESCE(s.enabled,0) AS enabled
FROM landmarks l
LEFT JOIN poi_selection s
  ON s.kind = l.kind AND s.subtype = COALESCE(NULLIF(l.subtype,''),'')
GROUP BY l.kind, subtype
ORDER BY l.kind, subtype;
```
(Count = total rows, decision.) If the table is empty, seed place-only first (§1),
then run the query. Response: `[{kind, subtype, count, enabled}]`.

### `POST /poi-selection`
Body: `{"selection":[{"kind","subtype","enabled"}...]}`. Upsert each row
(`INSERT ... ON CONFLICT(kind,subtype) DO UPDATE SET enabled=...`). Guarded by
`landmarks_db_lock`. Returns `{}`.

### Routes
- `do_GET`: add `/poi-types`.
- `do_POST`: add `/poi-selection`.
- Reuse `_json_body()`, `_send()`, `open_landmarks_db()`, `landmarks_db_lock`.

## 3. UI — `gs/web/viewer.html` (preflight only)

New **bottom panel** `#poipanel` (hidden in overlay modes, like `#panel`), a
scrollable checkbox tree:

```
[POI types to show on OSD]
☑ place           (97)        ← kind header: tri-state, toggles all children
   ☑ city          (3)
   ☑ town          (12)
   ☑ village       (70)
   ...
☐ natural         (40)
   ☐ peak          (8)
   ☐ water         (32)
☐ ...
```

- Dependency-free: build nested `<div>`/`<label><input type=checkbox>` from the
  `/poi-types` JSON, grouped by `kind`. Kind header checkbox = checked/unchecked/
  indeterminate from its children.
- On any toggle: debounce, then `POST /poi-selection` with the full current state.
- On load (preflight): `GET /poi-types` → build tree with saved `enabled` states.
- Counts shown per node; kind header shows the sum of its subtypes.
- Collapsible kinds (click header to expand/collapse) to keep it compact.

CSS: fixed bottom, translucent like `#panel`, `max-height` ~35vh with
`overflow:auto`; `body.overlay #poipanel { display:none }`.

## 4. OSD — `osd/util/poi_osd.c`

Replace the hard-coded `kind='place'` filter in `poi_load_bbox()` with a join on
`poi_selection`:

```sql
SELECT l.name_en, l.lat, l.lon
FROM landmarks l
JOIN poi_selection s
  ON s.enabled=1 AND s.kind=l.kind
 AND s.subtype = COALESCE(NULLIF(l.subtype,''),'')
WHERE l.name_en IS NOT NULL AND l.name_en <> ''
  AND l.lat BETWEEN ? AND ? AND l.lon BETWEEN ? AND ?;
```

- Keeps the existing English-name + bbox filters (unchanged OSD behavior otherwise).
- **Graceful fallback:** if `poi_selection` does not exist yet (OSD ran before the
  map ever created it), `sqlite3_prepare_v2` fails → fall back to the current
  `kind='place'` query so the OSD is never unexpectedly blank. Logged once.
- Selection changes are picked up on the next bbox reload (on movement). No extra
  config; no new ini keys.

## 5. Decisions (locked with user, 2026-06-26)

1. **Default = place-only** — seed place subtypes ON when the table is empty;
   preserves current OSD output.
2. **Count = total rows** — `COUNT(*)` per (kind,subtype), matching the user's
   `GROUP BY`. OSD still draws only English-named, in-range ones (noted in UI hint).
3. **New table in landmarks.db**, not a separate file — selection lives with the
   data it filters; both server and OSD already open this DB.
4. **subtype normalized to `''`** (not NULL) for clean PK/join.
5. **OSD fallback to place-only** if the table is missing — never blank by surprise.

## 6. Verify plan (Phase 4)

- `map.sh preflight`: tree appears at bottom with kinds/subtypes + counts matching
  `SELECT kind,subtype,count(*) ... GROUP BY ...`.
- Toggle a subtype → `poi_selection` row updated; reload map → state restored.
- Kind header tri-state: partial children → indeterminate; toggling header sets all.
- OSD: build `native`, run with a selection of e.g. only `place/city` → only cities
  drawn; enable `natural/peak` → peaks appear after the next bbox reload.
- Fresh DB with no `poi_selection`: OSD falls back to place-only; first `/poi-types`
  seeds place-on and the tree reflects it.

## 7. Out of scope

- Per-subtype OSD styling (icon/colour). All markers stay the current circle+label.
- Editing/clustering counts by name_en (UI shows totals only).
- Range/altitude auto-scaling (separate plan).
