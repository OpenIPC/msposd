#!/bin/bash
# Vendor Leaflet into web/ for fully-offline use. Run once while online.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
VER="1.9.4"
BASE="https://unpkg.com/leaflet@${VER}/dist"
curl -fL -o "$HERE/web/leaflet.js"  "$BASE/leaflet.js"
curl -fL -o "$HERE/web/leaflet.css" "$BASE/leaflet.css"
# Leaflet's CSS references images/ relative to leaflet.css; grab the marker shadow set.
mkdir -p "$HERE/web/images"
for f in marker-icon.png marker-icon-2x.png marker-shadow.png layers.png layers-2x.png; do
  curl -fL -o "$HERE/web/images/$f" "$BASE/images/$f" || true
done
echo "Leaflet $VER vendored into $HERE/web/"
