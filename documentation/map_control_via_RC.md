# Map control via RC/MSP channels (Radxa, no keyboard)

Plan for controlling the native OSD moving-map (zoom, follow mode, visibility) on a
Radxa ground station that has no keyboard — only HW buttons — where `msposd` runs as
a module inside a larger multi-component system.

**Decision inputs (confirmed):**
- HW buttons are already encoded onto **RC channels present in the MSP/MAVLink stream**
  that `msposd` receives.
- Command interface must support **both absolute and relative** semantics.

## Background: the integration surface today

**Map control entry points** — currently reachable only via X11 keys in
[`osd/util/Render_gs.c:145-185`](../osd/util/Render_gs.c):
- `map_toggle_enabled()` → show/hide ([`map_render.c:217`](../osd/util/map_render.c))
- `map_zoom_step(±1)` → zoom ([`map_render.c:230`](../osd/util/map_render.c))
- `map_follow_cycle()` → mode plane→center→north→fit ([`map_render.c:246`](../osd/util/map_render.c))

On the Radxa GS an X server *does* run (`Init()` opens `:0`,
[`Render_gs.c:242`](../osd/util/Render_gs.c)) but there is no keyboard, so the key path
is unreachable.

**Existing event-ingress mechanisms** (reused by this plan):
- **libevent** main loop over serial/UDP bufferevents ([`msposd.c`](../msposd.c)).
- **RC-channel command dispatch** — `-c/--channels` → `ProcessChannel()` runs
  `/usr/bin/channels.sh <ch> <val>` when a channel holds a level
  ([`msposd.c:613-663`](../msposd.c)); `handle_stickcommands()` turns stick gestures into
  a menu ([`osd/msp/msp_displayport.c:156`](../osd/msp/msp_displayport.c)). This is the
  precedent for "external event → action", including a debounce/persist pattern
  (`ChannelPersistPeriodMS`, [`msposd.c:603,629-636`](../msposd.c)).

## Options considered

| Option | How buttons reach msposd | Pros | Cons |
|---|---|---|---|
| **A. Local control socket** (Unix-domain / UDP 127.0.0.1) | daemon sends `map zoom +` | clean decoupling; language-agnostic; fits libevent; testable with `nc` | new socket surface; ~120 lines |
| **B. Control FIFO / file + inotify** | `echo map.mode=fit > /run/msposd.ctl` | trivial from shell; inotify already in-tree | awkward for event streams; one-writer |
| **C. Reuse RC channels** (in-process) | buttons → RC channel values in MSP stream | **zero new transport** — values already parsed & debounced; no X needed | needs the RC stream (present here) |
| D. Synthesize X keypresses (XTEST) | fake keystrokes into map window | no msposd change | brittle (focus/XTEST), X-coupled, toggles only — rejected |
| E. Signals (SIGUSR1/2) | `kill -USR1` | trivial | far too few distinct actions — rejected |

**Chosen: Option C** — buttons already ride the RC/MSP stream, so an in-process
RC→map binding layer needs no new transport, no X keyboard, and no socket to secure.

## Architecture

```
MSP/MAVLink stream ──> channels[18]  (already parsed in msposd.c)
                          │
                          ▼
                   map_rc_update(channels)   ← NEW: evaluate bindings each refresh
                          │  (band change / rising edge, debounced)
                          ▼
                   map_control("mode fit")   ← NEW: single dispatcher
                    ▲            │
      X keys ───────┘            ▼
   (Render_gs.c)      map_set_shown / set_follow / set_zoom
                      map_zoom_step / map_follow_cycle (relative)
```

### 1. Unified dispatcher `map_control(const char *cmd)`
In [`osd/util/map_render.c`](../osd/util/map_render.c) — single source of truth for
both absolute and relative commands:

```
show 0|1|toggle
mode plane|center|north|fit | next|prev
zoom <level> | + | -
```

- Refactor the X handler at [`Render_gs.c:170-184`](../osd/util/Render_gs.c) to call it
  (`map_control("zoom +")`, etc.) so keyboard and RC share identical logic.
- Add absolute setters `map_set_shown()`, `map_set_follow()`, `map_set_zoom_level()`.
  (Note: the current `map_toggle_enabled()` is really a show/hide *toggle*.)

### 2. RC binding layer `map_rc_update(uint16_t channels[18])`
Hooked where channels are refreshed (same site as `ProcessChannels()`,
[`msposd.c:665`](../msposd.c)) but **not** gated by `!armed` — the map must be
controllable in flight. Two binding kinds cover absolute + relative:

- **Position bindings (absolute):** a switch channel's *value bands* map to states —
  idempotent; a switch position always means the same thing.
- **Momentary bindings (relative):** a **rising edge** on a button channel issues a
  step, debounced with the existing `ChannelPersistPeriodMS` pattern so one press =
  one action.

Recommended default: a **stability window** (~200 ms) on position bindings so a band
must hold before it applies, riding out transitional PWM — reuse `ChannelPersistPeriodMS`.

### 3. Config — new `[map.rc]` block in [`msposd.ini`](../msposd.ini.example)
Fully data-driven, so channel numbers are not hard-coded. Wire only the channels you
actually have (e.g. one 3-position switch + one momentary button is a valid minimal
setup). Any key omitted / `0` = that binding is inactive.

```ini
[map.rc]
; RC channels are 1-based, as in the MSP stream. Values ~1000..2000.
mode_ch    = 6                 ; switch position -> follow mode (absolute)
mode_bands = plane,center,fit  ; N positions split evenly low..high
show_ch    = 7                 ; 2-pos: low = hide, high = show (absolute)
zoom_ch    = 8                 ; analog/rotary -> zoom level across MBTiles range (absolute)
zoom_in_ch  = 9                ; momentary button, rising edge -> zoom + (relative)
zoom_out_ch = 10               ; momentary button -> zoom - (relative)
mode_next_ch= 11               ; momentary button -> mode next (relative)
```

## Why this fits the constraints
- **Reuses the existing event pipeline** (RC channels already parsed & debounced) — no
  X keyboard, no extra daemon, no socket surface to secure.
- **Absolute + relative** both first-class: switches give idempotent state, buttons give
  steps.
- **Decoupled**: the button/GPIO component only needs to place values on RC channels,
  which it already does.
- **One dispatcher** keeps the x86 keyboard workflow and the Radxa RC workflow from
  drifting apart.

## Scope / effort
~150 lines: `map_control` + absolute setters + `map_rc_update` + config parse, plus the
small `Render_gs.c` refactor. No change to the render/draw path. Testable on x86 by
injecting channel values.

## Open items before implementation
- Confirm the actual channel numbers (or ship with the bindings above as commented,
  all-disabled defaults so nothing activates until set).
- Confirm the position-binding stability window default (~200 ms, reusing
  `ChannelPersistPeriodMS`).
