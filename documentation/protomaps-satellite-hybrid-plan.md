# Satellite + Protomaps hybrid map — implementation plan

Status: implemented; see *Revisions after review* at the end for changed decisions.

## 1. Result and scope

Add **Satellite Hybrid** to preflight's map sources. It combines the existing
Satellite imagery with Protomaps roads, road names, village names, and building
outlines. Users prepare an area and export a normal raster MBTiles pack for the
ground station.

Default overlay settings:

| Setting | Default | Appearance |
| --- | --- | --- |
| Roads | On | Thin light lines with dark edging; width depends on road class and zoom |
| Road names | On | Readable text with a dark halo, where space permits |
| Village names | On | Village and hamlet labels, prioritised over road names |
| Town/city names | Off | Separate switch, following the earlier request for independent control |
| Buildings | On | Subtle outlines at zoom 15 and above, preserving the imagery underneath |

These switches apply to the hybrid source at all selected zooms. Changing them
changes the preview and future packs. Downloaded packs keep their original style.
Contours and hillshading are a separate future addition; this phase implements
the requested roads, settlements, and buildings.

## 2. Architecture and decisions

```text
Existing Satellite source or selected pure-satellite MBTiles
                              +
             Regional Protomaps vector extract
                              +
                 Overlay settings and fonts
                              |
               Preflight raster composition
                              |
                  256 x 256 JPEG tiles
                              |
             New MBTiles pack -> export -> OSD
```

- **Render in Python during preflight.** Use Pillow for drawing/compositing, a
  small built-in MVT decoder, and the official PMTiles Python reader for local
  extracts. The official `pmtiles` CLI performs regional extraction; it is
  downloaded and checksum-verified on first remote use rather than bundled.
- **Keep the renderer narrowly scoped.** Implement the required line, polygon,
  and text styles, rather than a general map style engine. This fits the current
  Python/PyInstaller app and avoids requiring a browser to stay open to finish
  downloading. It does introduce dependencies beyond the current stdlib server.
- **One raster implementation serves preview and export.** Leaflet displays the
  generated tiles through the existing local tile endpoint. This keeps the
  saved map's appearance consistent with its preview.
- **Use JPEG for the finished imagery.** Start with quality 90, no chroma
  subsampling, and baseline encoding. Validate thin lines and text against PNG
  reference images before fixing the export settings. Store ordinary 256-pixel
  tiles compatible with the existing decoder and TMS row convention.
- **Keep vector data on the preparation computer.** The OSD needs only the
  finished raster pack. Native changes are limited to displaying attribution.
- **Use a versioned style and a fixed data build per job.** Snapshot source,
  bounds, zooms, settings, font/style versions, and Protomaps build identity at
  job creation; settings changes cannot alter an in-progress pack.

## 3. Data acquisition and reuse

1. Resolve an available Protomaps v4 build from its published build manifest;
   validate its schema and freeze the URL/build ID for the job. Allow a local
   extract or configured mirror as an alternative. Do not use an expiring
   hard-coded daily URL or make browser tile requests directly to the planet file.
2. Use `pmtiles extract` to download only the selected region, expanded to cover
   rendering buffers. Read archive metadata for its maximum zoom. Extract from
   zoom 0 through the highest source zoom needed, rather than downloading the
   planet. Reuse a local extract only when its build, coverage, and zooms fit.
3. For output above the source maximum (currently z15), use parent vector tiles
   and transform coordinates into output space. Evaluate visibility, line widths,
   and font sizes at the output zoom. Enlarging geometry adds no new OSM detail.
4. Reuse an explicitly selected existing pure-satellite pack wherever it contains
   the exact tile requested. Validate provenance and coverage; do not treat a
   mixed or already-composited pack as raw imagery. Download uncovered imagery
   through the existing Satellite source when online. Report gaps offline.
5. Write every hybrid pack to a new file. Keep input packs read-only. Track the
   imagery source pack identity in metadata when reusing it.
6. Keep extracts and preview tiles in a separate bounded working-cache directory,
   outside the exported map-pack list. Publish completed extracts atomically;
   incomplete downloads must never count as usable cached data.

Protomaps supplies only the vector overlay. Its open-data licence does not
change the permissions attached to the existing satellite imagery. Preserve
imagery credits and include OpenStreetMap attribution in the map and export.

## 4. Rendering and labels

- Decode `roads`, `places`, and `buildings` from the documented v4 schema. Filter
  settlement labels by `kind_detail` so villages/hamlets and towns/cities are
  independently controlled. Ignore address points and unrelated POI layers.
- Respect source feature visibility/ranking. Road labels use available names;
  do not invent labels where OSM has none. Prefer local names, with an English
  fallback. Bundle a redistributable font with Latin and Cyrillic support and
  record other script coverage as a limitation until suitable fonts are added.
- Draw building polygons with their holes, then roads, then text. Sort road
  geometry using layer/rank information so bridges and crossings are consistent.
- Render globally aligned groups of tiles (initially 4 x 4) with a surrounding
  buffer, then cut into 256-pixel tiles. This gives text room across individual
  tile boundaries while bounding memory use.
- Use deterministic label anchors, stable feature ordering, and collision checks
  over surrounding candidates. Deduplicate features repeated in vector buffers.
  Check neighbouring render groups as well as tiles: a buffer alone does not
  guarantee consistent label placement across group edges.
- Place road names along sufficiently straight segments with appropriate rotation;
  skip labels that cannot fit. Village labels have higher collision priority.
- Include all source tiles needed for the buffer and handle tile-coordinate
  orientation, parent zoom transforms, longitude wrapping, and polar limits.
- Missing vector coverage or decoding errors must be reported as hybrid failures;
  never silently save plain satellite imagery as a successful hybrid tile.
  A valid empty vector tile is acceptable.

## 5. Preflight integration

- Register Satellite Hybrid as a generated source in the current source catalogue;
  preserve normal provider URLs and per-zoom source selection.
- Add the overlay switches and optional existing satellite-pack selector to the
  preflight controls. Persist settings and expose them in `/settings` and `/status`.
- Prepare vectors for the viewport with a bounded background job. Show a clear
  loading state during extraction; debounce panning and reuse coverage. Preview
  requests share work rather than starting an extractor for every tile.
- Include the data/style/settings identity in preview URLs and cache keys, so
  switching labels cannot return yesterday's cached appearance.
- Add progress phases for vector preparation and composition, followed by the
  existing POI/elevation phases. Bound worker concurrency, memory, cache size,
  and extraction subprocess lifetime; keep telemetry and status requests responsive.
- Display source-download bytes, generated-pack bytes, and temporary storage
  separately. Measure a representative 200–500 MB pack; do not promise unchanged
  size or processing time before measuring composition overhead.
- Save style settings, build identity, attribution, bounds, zooms, and completion
  status in MBTiles metadata. Export only finished hybrid packs and include an
  attribution text file. Existing packs remain readable without new metadata.
- Display attribution in the browser and native map without covering scale/AGL
  readouts. Read native credits on pack load, not once per frame.

## 6. Files

| File | Planned change |
| --- | --- |
| `gs/mapserver.py` | Generated-source dispatch, settings, jobs, cache identity, metadata, export, and satellite-pack reuse |
| `gs/protomaps_source.py` (new) | Build resolution, bounded extraction, local vector reads, and source caching |
| `gs/hybrid_render.py` (new) | Geometry transformation, styling, label placement, and raster composition |
| `gs/web/viewer.html` | Hybrid controls, preview loading, progress, and attribution |
| `gs/assets/fonts/` (new) | Bundled font and its licence |
| `gs/requirements-hybrid.txt` (new) | Pinned rendering/reader dependencies |
| `gs/pack/` | Bundle dependencies and fonts; the pinned pmtiles CLI is fetched at runtime with checksum verification |
| `gs/run-map.sh`, `gs/run-map.bat` | Development dependency setup/checks and clear missing-component messages |
| `.github/workflows/preflight-pack.yml` | Package and smoke-test the hybrid source on the existing OS matrix |
| `osd/util/map_render.c` | Load and display pack attribution using existing Cairo rendering |
| `tests/` | Small deterministic fixtures and hybrid integration checks |
| `gs/README.md`, `gs/pack/README.md` | User settings, dependencies, styling limitations, and packaging |
| `documentation/gs-map-internals.md`, `documentation/build-preflight-release.md` | Data flow, metadata, dependencies, and release verification |

There is an existing uncommitted change in `gs/README.md`; preserve it during
implementation. Every added/modified function gets the project-required concise
documentation for its purpose, parameters, and return value.

## 7. Implementation gates

### A. Spec approval

Approve this plan before source changes, as required by `AGENTS.md`.

### B. Draft: prove the dependency and rendering path first

Build a small hybrid sample covering a village, named roads, and buildings at
z13/15/17. Verify extraction, Cyrillic text, parent-tile transforms, adjacent
render groups, and a frozen Windows/Linux package. Record time, peak memory,
output size, and binary-size increase. If the selected stack cannot produce
acceptable labels or portable packages, revise the design before full integration.

Then integrate source settings, preview, downloads, satellite reuse, export,
attribution, and the existing per-zoom selection flow.

### C. Simplify

Review for duplicate preview/export render paths, unnecessary settings, repeated
decoding, unbounded caches, and unrelated refactoring. Keep only the required
provider-specific logic and shared composition path.

### D. Verify

- Unit fixtures: XYZ/TMS orientation, output above z15, polygon holes, settlement
  filters, deterministic labels, Unicode, and cache invalidation.
- Visual checks: roads/buildings align with imagery; labels remain legible over
  light/dark imagery; no cut or inconsistent labels across tile/group boundaries.
- Integration: identical preview/saved tiles for the same snapshot; missing or
  interrupted vectors; read-only reuse of a satellite pack; mixed per-zoom sources;
  fresh pack identity; export/import; network-disabled rendering after restart.
- Resource checks: representative 200–500 MB pack, bounded memory/cache usage,
  reported disk overhead, and responsive telemetry during composition.
- Regression: existing basemaps, downloaded-pack selection, POIs, DEM/AGL, and
  old packs without attribution metadata continue to work.
- Packaging: frozen-app smoke tests on Linux, Windows, and macOS; clean machine
  startup without separately installed Python or pmtiles. Build native ground
  targets affected by the attribution change and visually inspect OSD placement.
- Report unavailable platform/hardware checks explicitly rather than claiming
  they passed. No release/publishing step is included in this plan.

## References

- [Protomaps downloads and regional extracts](https://docs.protomaps.com/basemaps/downloads)
- [Protomaps v4 layer schema](https://docs.protomaps.com/basemaps/layers)
- [Official pmtiles CLI](https://docs.protomaps.com/pmtiles/cli)
- [PMTiles implementations, including Python](https://github.com/protomaps/PMTiles)
- [Python MVT decoder](https://github.com/tilezen/mapbox-vector-tile)
- [Pillow drawing API](https://pillow.readthedocs.io/en/stable/reference/ImageDraw.html)

## Revisions after review

- **Rendering:** each 4 x 4 group's overlay is rendered once and cached; imagery
  is fetched and composed per requested tile. Downloads walk the area group by
  group, so tall areas no longer re-render groups or re-fetch imagery.
- **Preview:** the preview id depends only on the style/imagery snapshot. A
  session keeps up to six prepared vector areas; panning inside them never
  reloads tiles, and tiles that waited for new vectors are re-requested alone.
  A newer preview area cancels a superseded extraction.
- **Cache:** extracts are evicted least-recently-used to 75% of 1 GiB, never
  while referenced. Automatically resolved builds reuse any cached automatic
  extract covering the area, so a new daily build does not force a re-download.
- **Failed tiles:** as with other packs, a finished job is complete and records
  `failed_tiles`; only an interrupted job stays incomplete and unexportable.
- **Dependencies:** `mapbox-vector-tile` (with shapely, pyclipper, protobuf) is a
  test-only reference encoder (`gs/requirements-hybrid-dev.txt`).
- **Dateline:** extraction bounds are clamped at ±180°; roads and labels from
  across the dateline may be missing within two tiles of it.
- **Platforms without a pmtiles build** (for example Linux armv7) can still use a
  local Protomaps archive via `protomaps_source`.
