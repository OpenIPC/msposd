#!/bin/bash
# Alias for the preflight mode (browse / download / set target), in the system
# browser by default. Extra args are forwarded, so `./run-map.sh --GTK` opens the
# WebKitGTK window instead. See map.sh for preview/full overlay modes.
exec "$(cd "$(dirname "$0")" && pwd)/map.sh" preflight "$@"
