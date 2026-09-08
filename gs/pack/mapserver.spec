# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller spec — build the standalone preflight map app (Option A).
#
# Bundles the pure-stdlib mapserver.py + the web/ assets into ONE native binary
# per OS. When run, the binary starts the local server and opens the preflight
# page in the user's DEFAULT system browser (mapserver.py auto-enables
# --open-browser when frozen), so nothing embeds or ships a browser engine.
#
# Writable data (config.ini, maps/, state.ini, landmarks.db) is created next to
# the executable at runtime; only the read-only web/ assets are bundled here.
#
# Build (from anywhere):   pyinstaller --clean --noconfirm gs/pack/mapserver.spec
# Output:                  dist/msposd-preflight[.exe]

import os

GS = os.path.abspath(os.path.join(SPECPATH, os.pardir))   # the gs/ directory

a = Analysis(
    [os.path.join(GS, "mapserver.py")],
    pathex=[GS],                                   # so `import tiles_info` resolves
    binaries=[],
    datas=[(os.path.join(GS, "web"), "web")],      # -> _MEIPASS/web (RES_DIR/web)
    hiddenimports=["tiles_info"],                  # imported lazily inside mapserver.py
    hookspath=[],
    runtime_hooks=[],
    excludes=["tkinter", "gi", "PIL", "numpy"],    # keep the binary lean
    noarchive=False,
)
pyz = PYZ(a.pure, a.zipped_data)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="msposd-preflight",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,        # shows the http://127.0.0.1 URL and allows Ctrl+C to quit
    disable_windowed_traceback=False,
)
