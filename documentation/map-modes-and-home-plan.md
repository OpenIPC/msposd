# Plan — Map Modes, WebKitGTK Renderer, Target & Home Persistence

Status: **DRAFT — awaiting approval & decisions (§9)**. Phase 1 (Spec). No code yet.

Builds on [offline-map-overlay-spec.md](offline-map-overlay-spec.md). Goal: turn the
single Firefox window into a lightweight, cross-platform (x86 + Radxa Zero 2W) viewer
with three run modes, click-to-set target, and automatic home capture on ARM.

## 1. Renderer decision

Move overlay rendering to **WebKitGTK** via a small `mapwin` host (Python 3 + `gi` +
`WebKit2 4.1`), which is already installed here and available as the same Debian packages
on the Radxa (`gir1.2-webkit2-4.1`, `python3-gi`). Lighter than Chromium, borderless +
always-on-top capable, transparent-background capable (for overlay over video).

- **Preflight (mode 1)** uses `mapwin` as a decorated/maximized config window.
- **Preview / Full (modes 2/3)** use `mapwin` as borderless overlay windows.

The same `viewer.html` is loaded in all cases; mode-specific behavior is driven by URL
query params, so there is one UI codebase.

## 2. New / changed files

```
gs/
├── mapwin            # NEW: Python GTK3+WebKit2 host (executable, takes window args)
├── map.sh            # NEW: mode launcher wrapper (preflight|preview|full [flags])
├── mapserver.py      # CHANGED: STATUS/armed parsing, home capture, state file, /target
├── web/viewer.html   # CHANGED: modes via query params, click-to-target, house icon, fit mode
├── state.ini         # NEW (generated): shared target+home (see §6)
└── run-map.sh        # kept as a thin alias to `map.sh preflight`
```

## 3. `mapwin` (WebKitGTK host)

A ~80-line GTK3 + WebKit2 window. Arguments:

| Flag | Meaning |
| ---- | ------- |
| `--url URL` | page to load (required) |
| `--fullscreen` | fullscreen window (mode 3, preflight on Radxa) |
| `--geometry WxH+X+Y` | explicit size/pos (mode 2 default = ¼ screen, bottom-right) |
| `--borderless` | undecorated + keep-above + skip taskbar (modes 2/3 overlay) |
| `--transparent` | RGBA visual + transparent background (future: map over video) |

mapwin only manages the *window*; all map logic stays in `viewer.html`.

## 4. Modes & CLI (`map.sh`)

| Command | Window | Toolbox | Telemetry behavior |
| ------- | ------ | ------- | ------------------ |
| `map.sh preflight` | fullscreen (any browser) | yes | free browse; download; click-to-target |
| `map.sh preview --follow plane` | ¼ screen, borderless, keep-above | no | plane centered, map moves, fixed zoom |
| `map.sh preview --follow fit` | ¼ screen, borderless, keep-above | no | fit home+plane in view (auto zoom/center) |
| `map.sh full --follow plane\|fit` | fullscreen, borderless | no | as above, fullscreen (OSD renders over it) |

`map.sh` starts `mapserver.py` if not already running, then launches the browser/`mapwin`
with the right URL params and window flags. The "executable that takes arguments" is
`mapwin`; `map.sh` is the convenience wrapper.

## 5. `viewer.html` behavior by query param

- `?mode=preflight` → toolbox visible (basemap, download, target fields), **map click sets
  target**, full pan/zoom. (current behavior + click-to-target)
- `?mode=preview|full` → toolbox hidden, user interaction locked; layers driven by SSE.
  - `&follow=plane` → `map.panTo(plane)` each fix, fixed zoom (config `zoom`).
  - `&follow=fit` → `map.fitBounds([home, plane])` each fix → scale & center from the two
    coordinates.
- **House icon** for home (from `/status.home`) in every mode once home is known.
- Plane / heading vector / target vector as today.

## 6. Shared state file — target + home

Both `mapserver.py` (write) and `msposd.c` (read) need it. **Recommended format: INI**,
because msposd already ships an INI parser (`osd/util/ini_parser.c`, `ReadIniInt`/Float),
so the C side is a few lines and no JSON dependency. (Decision in §9.)

`gs/state.ini` (path also readable by msposd via config):
```ini
[target]
set = 1            ; 0 = no target
lat = 43.2500
lon = 27.8500

[home]
set = 1            ; 0 = home not yet captured
lat = 43.1430
lon = 27.9340
```
- `mapserver.py` writes `[target]` on user set/clear, `[home]` on ARM capture.
- Written atomically (temp file + rename) so a concurrent C read never sees a half file.
- `msposd.c` integration (reading/drawing target & home) is **out of scope for this plan**;
  we only guarantee a C-friendly format and stable path. A follow-up task wires msposd.c.

## 7. Home capture on ARM (`mapserver.py`)

- Parse `MSP_CMD_STATUS` (cmd 101; also 150 STATUS_EX): `armed = payload[6] & 0x01`.
- Track armed edge. On **disarmed→armed** with a valid GPS fix, set `home = current pos`
  and persist to `[home]`. If armed fires before a fix, capture on the first valid fix
  while still armed and home unset this session.
- **On startup, load `[home]` from `state.ini`.** Do not overwrite it except on a fresh
  arm edge — so restarting the server mid-flight keeps the correct home (the plane is no
  longer at home, but the persisted value is right).
- Expose `home` in `/status` for the house icon.

## 8. `mapserver.py` endpoint changes

- `GET /status` → add `home: {lat,lon}|null`, `target: {lat,lon}|null`, `armed: bool`.
- `POST /target {lat,lon|null}` → set/clear target, persist `[target]`. (viewer click + fields)
- Keep `/pos`, `/tiles`, `/download`, `/settings`, basemaps as-is.
- State-file IO helpers (load on boot, atomic write).

## 9. Decisions (locked — approved 2026-06-22)

1. **State file format: INI** — `gs/state.ini`, reusing msposd's `ini_parser.c` on the C side.
2. **Phasing: Phase A now** — modes 1+2, click-to-target, ARM home capture. **Mode 3
   (fullscreen + OSD-over-map) deferred to Phase B.**
3. **Renderer: Python GTK3 + WebKit2 `mapwin`.**

Target storage moves out of `config.ini [map]` into `state.ini [target]`; `config.ini`
keeps zoom/basemap/center. The viewer reads target+home from `/status`, sets target via
`POST /target`.

## 10. Verify plan (Phase 4)

- `mapwin`: opens borderless ¼-screen keep-above window at the right geometry (x86 now).
- Home: simulate ARM via an injected MSP_STATUS frame while a fix is present → `state.ini`
  `[home]` written; restart server → home reloaded → house icon shown at the same spot.
- Target: click map in preflight → `[target]` written and re-read on reload.
- Modes: `follow=plane` keeps plane centered; `follow=fit` keeps both home and plane in view.
- `sim_msp.py` extended to optionally send ARMED so the whole flow is testable on the desk.
```
