#!/usr/bin/env bash
# Build the standalone preflight map binary for the current OS (Linux/macOS).
# Windows: use build.bat. CI builds all three (see .github/workflows/preflight-pack.yml).
#
#   ./gs/pack/build.sh
# Produces: dist/msposd-preflight  (a single self-contained executable)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
cd "$REPO"

PY="${PYTHON:-python3}"
"$PY" -m pip install --quiet --upgrade pyinstaller

"$PY" -m PyInstaller --clean --noconfirm "$HERE/mapserver.spec"

echo
echo "Built:"
ls -la "$REPO/dist/" | sed 's/^/  /'
echo
echo "Run it (starts the server + opens your browser):"
echo "  ./dist/msposd-preflight"
