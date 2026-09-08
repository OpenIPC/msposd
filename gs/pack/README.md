# Standalone preflight map app (Option A)

Package the preflight map (`gs/mapserver.py` + `web/`) into a **single downloadable
binary per OS** that end users run without installing Python. It starts a local
server and opens the map in the user's **default system browser** — no WebKit or
Chromium is bundled.

This is an *extra* packaging path. The normal development launcher also opens preflight
in the system browser by default; `./map.sh preflight --GTK` selects the compatibility
WebKit `mapwin` window instead. The preview/full WebKit modes remain available, while
the primary in-flight map is rendered natively by `msposd`.

## How it works
- `mapserver.py` is pure Python standard library, so it bundles cleanly.
- When frozen (PyInstaller), it auto-enables `--open-browser`: after the server
  binds `127.0.0.1`, it opens `…/viewer.html?mode=preflight` in the default browser.
- Read-only assets (`web/`) are bundled inside the binary; writable `config.ini`,
  `state.ini` and `maps/` are created **next to the executable** on first run.
  Each download creates a new `maps/<pack>.mbtiles`; `landmarks.db` and
  `elevation.db` are shared files in the same `maps/` directory. **Export selected
  map…** bundles those three files for transfer to the OSD station.

## Build locally
- **Linux / macOS:** `./gs/pack/build.sh`
- **Windows:** `gs\pack\build.bat`

Output: `dist/msposd-preflight` (`.exe` on Windows) — one self-contained file.

Requirements: Python 3.9+ and `pip` on the build machine. `pyinstaller` is
installed automatically by the scripts. You must build **on each target OS**
(a macOS app needs a Mac, etc.) — or use CI below.

PyInstaller is not a cross-compiler. `build.sh` run on Linux, including under WSL,
uses the Linux bootloader and produces an ELF executable. That file cannot run on
Windows and adding an `.exe` suffix does not change its format. For Windows, run
`build.bat` from a native Windows Python installation or use the Windows CI artifact.

You can confirm a local result before distributing it:

```bash
file dist/msposd-preflight
# ... ELF 64-bit ...       (Linux build)
```

## Build all three via CI
`.github/workflows/preflight-pack.yml` builds Linux/macOS/Windows binaries on a
matrix runner. Trigger it manually (workflow_dispatch) or by pushing a
`preflight-v*` tag; download the binaries from the run's artifacts.

## Run
```bash
./dist/msposd-preflight          # starts server, opens your browser
./dist/msposd-preflight --port 9000
./dist/msposd-preflight --no-browser   # server only
```
Close the console window or press Ctrl+C to stop.

## Install in another Linux folder

The one-file Linux build needs no installer or Python runtime. On the same Linux
machine, copy it into any writable directory and preserve or restore its executable bit:

```bash
mkdir -p ~/Apps/msposd-preflight
cp dist/msposd-preflight ~/Apps/msposd-preflight/
chmod +x ~/Apps/msposd-preflight/msposd-preflight
~/Apps/msposd-preflight/msposd-preflight
```

The executable's directory becomes its data directory. A fresh location starts with
default settings and creates data as needed; an established installation may contain:

```text
msposd-preflight
config.ini
state.ini
maps/
  <pack>.mbtiles
  landmarks.db
  elevation.db
```

Copy `config.ini`, `state.ini` and `maps/` alongside the binary if the new location
should retain existing settings and downloads. Starting another copy on the default
HTTP port reuses the server already listening there, and therefore its data directory.
Stop the existing process first, or use `--port 9000` for an independent preflight
instance.

## Notes
- **Unsigned binaries** trigger macOS Gatekeeper / Windows SmartScreen prompts.
  For public distribution, code-sign (and notarize on macOS). Fine for internal use.
- Some Windows antivirus engines flag PyInstaller one-file binaries (false positive).
- A Linux binary is most portable to the same Linux installation. Distributing it to
  other distributions may require building on an older compatible Linux baseline.
- Telemetry still works: the packaged process listens for MSP over UDP exactly as
  the script does — the browser choice doesn't affect it.
