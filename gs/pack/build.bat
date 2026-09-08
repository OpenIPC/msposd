@echo off
REM Build the standalone preflight map binary for Windows.
REM   gs\pack\build.bat
REM Produces: dist\msposd-preflight.exe
setlocal
set HERE=%~dp0
pushd "%HERE%..\.."
python -m pip install --quiet --upgrade pyinstaller || goto :err
python -m PyInstaller --clean --noconfirm "%HERE%mapserver.spec" || goto :err
echo.
echo Built dist\msposd-preflight.exe
echo Run it to start the server and open your browser.
popd
exit /b 0
:err
echo Build failed.
popd
exit /b 1
