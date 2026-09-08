#!/bin/bash
# Mode launcher for the GS offline map.
#
#   ./map.sh preflight                 # config UI in the default system browser
#   ./map.sh preflight --GTK           # ...in the WebKitGTK window (mapwin) instead
#   ./map.sh preview --follow plane    # create or show borderless preview window
#   ./map.sh full    --follow fit      # create or show fullscreen overlay
#   ./map.sh preview --toggle          # hide/show the existing map window
#   ./map.sh --kill                    # kill existing map/webkit/server and exit
#
# Preflight opens in the system browser by default, matching the packaged
# dist/msposd-preflight app. In-flight the map is drawn by the native in-OSD
# renderer (osd/util/map_render.c), so the WebKit preview/full overlay modes and
# --GTK preflight are kept for compatibility rather than as the primary path.
#
# Normal mode reuses the existing map window if present. Use --kill to replace it
# with a new mode/follow/flag combination.
#
# Below-OSD overlay stacking is a 3-part contract with gs/mapwin and
# wfb-stabilizer/render_direct.py: the taskbar stays hidden only while the
# fullscreen video is the active window, so this script deliberately does NOT
# focus the map (render_direct.py keeps the video active; mapwin feeds the map
# its keys via a global grab). See mapwin's header for the full picture.

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SERVER_LOG="/tmp/msposd-mapserver.log"
MAPWIN_LOG="/tmp/msposd-mapwin.log"

choose_menu_args() {
  echo "GS offline map launcher — choose what to start:"
  echo
  echo "  1) preflight                 maximized config window"
  echo "  2) preview --follow plane    1/4-screen borderless preview window, plane centered"
  echo "  3) preview --follow fit      1/4-screen borderless preview window, fit plane+home+target"
  echo "  4) preview --follow center   1/4-screen rotating map, plane at bottom-centre"
  echo "  5) full    --follow plane    fullscreen borderless overlay"
  echo "  6) full    --follow center   fullscreen rotating map"
  echo
  printf "Enter choice [1-6] (default 1): "
  read -r choice
  case "${choice:-1}" in
    1) MENU_ARGS=(preflight) ;;
    2) MENU_ARGS=(preview --follow plane) ;;
    3) MENU_ARGS=(preview --follow fit) ;;
    4) MENU_ARGS=(preview --follow center) ;;
    5) MENU_ARGS=(full --follow plane) ;;
    6) MENU_ARGS=(full --follow center) ;;
    *) echo "invalid choice: $choice"; exit 1 ;;
  esac
}

if [ $# -eq 0 ]; then
  choose_menu_args
  set -- "${MENU_ARGS[@]}"
fi

MODE=""
if [ "${1:-}" = "--kill" ]; then
  MODE="kill"
  shift
else
  MODE="${1:-preflight}"
  case "$MODE" in
    preflight|preview|full) shift || true ;;
    *)
      echo "usage: $0 [--kill] | preflight|preview|full [--follow plane|fit|center] [--toggle] [--kill] [--topmost] [--transparent] [--GTK]"
      exit 1
      ;;
  esac
fi

FOLLOW="plane"
KILL_FIRST=0
TOGGLE=0
TOPMOST=0
# NB: deliberately not named BROWSER — that is the standard env var xdg-open and
# python's webbrowser read to pick a browser, and assigning it here would clobber
# the user's choice for the browser we are about to launch.
WANT_BROWSER=1  # preflight in the system browser (default); --GTK selects mapwin instead
EXTRA=()   # passthrough window flags for mapwin
while [ $# -gt 0 ]; do
  case "$1" in
    --follow)
      [ $# -ge 2 ] || { echo "missing value for --follow"; exit 1; }
      FOLLOW="$2"
      shift 2
      ;;
    --transparent)
      EXTRA+=(--transparent)
      shift
      ;;
    --topmost)
      TOPMOST=1
      EXTRA+=(--topmost)
      shift
      ;;
    --kill)
      KILL_FIRST=1
      shift
      ;;
    --toggle)
      TOGGLE=1
      shift
      ;;
    --browser)
      WANT_BROWSER=1   # kept for compatibility; this is now the default
      shift
      ;;
    --GTK|--gtk)
      WANT_BROWSER=0   # render preflight in the WebKitGTK window (mapwin)
      shift
      ;;
    *)
      echo "unknown arg: $1"
      exit 1
      ;;
  esac
done

PORT="$(python3 - "$HERE/config.ini" <<'PY'
import sys, configparser
c = configparser.ConfigParser(); c.read(sys.argv[1]); print(c["server"]["port"])
PY
)"
# Per-launch cache-buster: forces a fresh viewer fetch when we really launch a new
# window. Reused windows keep their current page and state warm.
CB="$(date +%s)"
BASE="http://127.0.0.1:${PORT}/viewer.html"
WINDOW_TITLE="msposd-map-${MODE}"

# preview window: square, side = screen height/2 - screen height/20, placed bottom-right
RES="$(xrandr 2>/dev/null | awk '/\*/{print $1; exit}')"
SW="${RES%x*}"; SH="${RES#*x}"; [ -n "$SW" ] || { SW=1280; SH=720; }
QH=$((SH/2 - SH/20)); QW=$QH; QX=$((SW-QW)); QY=$((SH-QH))

server_running() {
  pgrep -f "$HERE/mapserver.py" >/dev/null 2>&1
}

get_window_id() {
  xdotool search --name --all "^${WINDOW_TITLE}$" 2>/dev/null | head -n1
}

all_window_ids() {
  xdotool search --name --all "^msposd-map-.*$" 2>/dev/null || true
}

window_is_visible() {
  xwininfo -id "$1" 2>/dev/null | grep -q "Map State: IsViewable"
}

show_window() {
  local win_id="$1"
  xdotool windowmap "$win_id" >/dev/null 2>&1 || true
  if [ "$TOPMOST" -eq 1 ]; then
    wmctrl -i -r "$win_id" -b add,above >/dev/null 2>&1 || true
  fi
  xdotool windowraise "$win_id" >/dev/null 2>&1 || true
  # Topmost overlay: do NOT focus/activate the map. render_direct.py keeps the
  # fullscreen video active so it covers the taskbar, and the map gets its keys
  # via mapwin's global grab, not via focus. Non-overlay modes (e.g. preflight)
  # focus the map normally.
  if [ "$TOPMOST" -eq 0 ]; then
    xdotool windowfocus "$win_id" >/dev/null 2>&1 || true
    xdotool windowactivate "$win_id" >/dev/null 2>&1 || true
  fi
}

hide_window() {
  xdotool windowunmap "$1" >/dev/null 2>&1 || true
  # render_direct.py re-activates the video (covers the taskbar) once the map is gone.
}

force_restart() {
  # Close any live X11 windows first, then SIGKILL handles stopped/suspended
  # processes that still survive.
  local win_id
  while read -r win_id; do
    [ -n "$win_id" ] || continue
    xdotool windowclose "$win_id" >/dev/null 2>&1 || true
  done < <(all_window_ids)
  pkill -KILL -f "$HERE/mapserver.py" 2>/dev/null || true
  pkill -KILL -f "$HERE/mapwin" 2>/dev/null || true
  pkill -KILL -x WebKitWebProcess 2>/dev/null || true
  pkill -KILL -x WebKitNetworkProcess 2>/dev/null || true
}

ensure_server() {
  if server_running; then
    return
  fi
  nohup python3 "$HERE/mapserver.py" >"$SERVER_LOG" 2>&1 &
  sleep 1
}

launch_mapwin() {
  nohup "$HERE/mapwin" --title "$WINDOW_TITLE" "$@" >"$MAPWIN_LOG" 2>&1 &
}

if [ "$MODE" = "kill" ]; then
  force_restart
  exit 0
fi

if [ "$KILL_FIRST" -eq 1 ]; then
  force_restart
fi

ensure_server

# Window reuse/toggle applies to mapwin windows only. In browser preflight we own
# no X window, so skip the lookup — otherwise a leftover --GTK window would be
# raised instead of the browser opening.
USE_BROWSER=0
if [ "$MODE" = "preflight" ] && [ "$WANT_BROWSER" -eq 1 ]; then
  USE_BROWSER=1
fi

WIN_ID=""
if [ "$USE_BROWSER" -eq 0 ]; then
  WIN_ID="$(get_window_id || true)"
fi
if [ -n "$WIN_ID" ]; then
  if [ "$TOGGLE" -eq 1 ]; then
    if window_is_visible "$WIN_ID"; then
      hide_window "$WIN_ID"
    else
      show_window "$WIN_ID"
    fi
  else
    show_window "$WIN_ID"
  fi
  exit 0
fi

case "$MODE" in
  preflight)
    if [ "$USE_BROWSER" -eq 1 ]; then
      # Default: open preflight in the system browser, same as the packaged
      # dist/msposd-preflight app. Pass --GTK to use the WebKit window instead.
      URL="${BASE}?mode=preflight&v=${CB}"
      echo "opening $URL in the system browser"
      python3 -c "import webbrowser,sys; webbrowser.open(sys.argv[1])" "$URL" || true
    else
      launch_mapwin --url "${BASE}?mode=preflight&v=${CB}" --maximized
    fi
    ;;
  preview)
    launch_mapwin --url "${BASE}?mode=preview&follow=${FOLLOW}&v=${CB}" \
      --geometry "${QW}x${QH}+${QX}+${QY}" --borderless "${EXTRA[@]}"
    ;;
  full)
    launch_mapwin --url "${BASE}?mode=full&follow=${FOLLOW}&v=${CB}" \
      --fullscreen --borderless "${EXTRA[@]}"
    ;;
esac
