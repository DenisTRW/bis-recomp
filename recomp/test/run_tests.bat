@echo off
rem Baut und startet die Recompiler-Tests (braucht tools\w64devkit)
setlocal
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "PATH=%ROOT%\tools\w64devkit\bin;%PATH%"
set "OUT=%ROOT%\build\test"
if not exist "%OUT%" mkdir "%OUT%"

python -I "%ROOT%\recomp\recomp.py" "%ROOT%" --only Pad_Update --out "%OUT%\pad" || exit /b 1
gcc -O2 -Wall -Wno-unused-label -I "%ROOT%\recomp\include" -I "%OUT%\pad" -I "%~dp0." ^
    -o "%OUT%\test_pad_update.exe" "%~dp0test_pad_update.c" "%~dp0harness.c" "%~dp0functable_empty.c" "%OUT%\pad\main.c" || exit /b 1
"%OUT%\test_pad_update.exe" "%ROOT%\extract\eur\arm9\arm9.bin"
