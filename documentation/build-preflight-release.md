# Build a release package — preflight map

How to package the preflight map (`map.sh preflight` / `gs/mapserver.py`) into a **single
self-contained binary per OS** that end users run without installing Python. It starts a
local server and opens the map in the user's **default browser** (no Python/WebKit to
install). Writable `config.ini`, `state.ini` and `maps/` are created next to the
executable on first run. Map tiles, shared landmarks and shared elevation data live
inside `maps/`.

## Prerequisites
- Python **3.9+** and `pip` on the build machine (`pyinstaller` is installed automatically).
- Build **on each target OS** — a Windows `.exe` needs Windows, a macOS build needs a Mac.

PyInstaller bundles a bootloader for the host OS; it does not cross-compile. Running
`build.sh` on Linux or WSL produces a Linux ELF executable. Copying or renaming that
file to `.exe` will produce Windows' “not a valid app” error. Use native Windows with
`build.bat`, or use the Windows artifact from CI.

## Build (current OS)
```bash
./gs/pack/build.sh      # Linux / macOS
gs\pack\build.bat       # Windows
```
Output: **`dist/msposd-preflight`** (`.exe` on Windows) — one file.

On Linux, verify the artifact type before publishing it:

```bash
file dist/msposd-preflight
# ... ELF 64-bit ...
```

## Build all three OSes at once (CI)
The `.github/workflows/preflight-pack.yml` matrix builds Linux/macOS/Windows. Trigger it:
- **Manually:** GitHub → Actions → *preflight-pack* → *Run workflow*, or
- **By tag:** push a tag matching `preflight-v*`, e.g.
  ```bash
  git tag preflight-v1.0 && git push origin preflight-v1.0
  ```
Download the three binaries from the run's **Artifacts**.

## Run / verify
```bash
./dist/msposd-preflight               # starts server, opens the browser
./dist/msposd-preflight --port 9000   # custom port
./dist/msposd-preflight --no-browser  # server only
```
Ctrl+C (or closing the console) stops it.

## Install elsewhere on Linux

No installation step is required. The binary can be copied to another writable folder
on the same Linux system:

```bash
mkdir -p ~/Apps/msposd-preflight
cp dist/msposd-preflight ~/Apps/msposd-preflight/
chmod +x ~/Apps/msposd-preflight/msposd-preflight
~/Apps/msposd-preflight/msposd-preflight
```

Its new containing directory becomes the writable application directory. Copy the old
`config.ini`, `state.ini` and `maps/` into that directory to preserve settings, map
packs, landmarks and elevation data. Otherwise it starts with defaults and creates data
as needed. Only one server can own the default HTTP port; stop the old copy or start the
new one with `--port 9000`.

## Ship it
Give users the single binary. On first run it creates its data folder alongside itself;
each area download creates a new `maps/<pack>.mbtiles` without modifying older packs.
Select the desired entry under **Downloaded maps**, then use **Export selected map…** to
create a zip containing that pack plus `landmarks.db` and `elevation.db` for the
OSD/flight station.

## Notes
- **Unsigned binaries** trip macOS Gatekeeper / Windows SmartScreen — code-sign (and
  notarize on macOS) for public release; fine as-is for internal use.
- Some Windows AV engines false-positive on PyInstaller one-file builds.
- For distribution across different Linux releases, build on an appropriately old
  compatible Linux baseline; copying within the build machine itself is safe.
- The dev launcher opens preflight in the system browser by default. Use
  `./map.sh preflight --GTK` for the compatibility WebKit `mapwin` window.

See [`../gs/pack/README.md`](../gs/pack/README.md) for how the packaging works internally.
