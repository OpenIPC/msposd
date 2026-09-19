@echo off
REM Windows launcher for the GS offline map, preflight mode only.
REM Equivalent of ./run-map.sh: starts mapserver.py and opens the preflight page
REM in the default browser. The preview/full overlay windows need the WebKitGTK
REM mapwin host and X11 tools, so they are Linux-only; see map.sh.
REM
REM   gs\run-map.bat                 # start server, open browser
REM   gs\run-map.bat --port 9000     # extra args are passed to mapserver.py
REM
REM Needs Python 3.9+ from python.org (or the Microsoft Store). Nothing else:
REM mapserver.py is pure standard library. Leave this window open while you use
REM the map; Ctrl+C stops the server.
setlocal
set HERE=%~dp0

set PY=
where py >nul 2>&1 && set PY=py -3
if not defined PY where python >nul 2>&1 && set PY=python
if not defined PY (
  echo Python 3 was not found on PATH.
  echo Install it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
  exit /b 1
)

REM certifi supplies the root CAs; without it Let's Encrypt tile sources such
REM as OpenTopoMap fail TLS verification on many Windows machines.
%PY% -c "import certifi" >nul 2>&1 || (
  echo Installing certifi ^(root certificates for HTTPS tile sources^)...
  %PY% -m pip install --quiet certifi
)

%PY% "%HERE%mapserver.py" --open-browser %*
exit /b %errorlevel%
