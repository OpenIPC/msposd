@echo off
REM Build the standalone preflight map binary for Windows.
REM   gs\pack\build.bat
REM Produces: dist\msposd-preflight.exe
REM
REM Needs Python 3.12+ (same as CI): its OpenSSL 3 verifies current Let's
REM Encrypt chains. certifi is bundled so HTTPS tile sources verify on any
REM Windows PC regardless of the state of its certificate store.
REM
REM Interpreter: the "py" launcher's newest Python 3 when available (so an
REM older Python first on PATH does not matter), else plain "python".
setlocal
set HERE=%~dp0
pushd "%HERE%..\.."

set PY=python
where py >nul 2>&1 && set PY=py -3

%PY% -c "import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)"
if errorlevel 1 (
  echo.
  echo Build needs Python 3.12 or newer. Using "%PY%", which reports:
  %PY% --version
  echo.
  echo If an error was printed above, that Python installation is broken
  echo ^(for example its Lib folder is empty^). Repair or reinstall it, e.g.:
  echo   winget install --id Python.Python.3.12 --exact --force
  echo Otherwise install 3.12+ from https://www.python.org/downloads/windows/
  popd
  exit /b 1
)
%PY% -m pip install --quiet --upgrade pyinstaller certifi || goto :err
%PY% -m PyInstaller --clean --noconfirm "%HERE%mapserver.spec" || goto :err
echo.
echo Built dist\msposd-preflight.exe
echo Run it to start the server and open your browser.
popd
exit /b 0
:err
echo Build failed.
popd
exit /b 1
