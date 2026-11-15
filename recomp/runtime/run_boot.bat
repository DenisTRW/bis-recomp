@echo off
rem Baut den Start-Test: Recompiler, Ninja-Build, Laufzeit (Scheduler + HLE) und startet ihn
rem Aufruf: recomp\runtime\run_boot.bat [frames] [budget_mio] [funktion_hex]
setlocal
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "PATH=%ROOT%\tools\w64devkit\bin;%PATH%"
if not exist "%ROOT%\build\test" mkdir "%ROOT%\build\test"
if not exist "%ROOT%\build\frames" mkdir "%ROOT%\build\frames"
python -I "%ROOT%\recomp\recomp.py" "%ROOT%" || exit /b 1
python -I "%ROOT%\recomp\gen_ninja.py" "%ROOT%" -O1 || exit /b 1
"%ROOT%\tools\ninja.exe" -C "%ROOT%\build\recomp" || exit /b 1
gcc -O1 -w -DRC_TRACE -I "%ROOT%\recomp\include" -I "%ROOT%\build\recomp" -I "%~dp0." ^
    -o "%ROOT%\build\test\boot_test.exe" "%~dp0boot_test.c" "%~dp0sched.c" "%~dp0threads_hle.c" "%~dp0arm7_hle.c" "%~dp0card.c" "%~dp0vram.c" "%~dp0render2d.c" "%~dp0gpu3d.c" "%~dp0smc.c" "%~dp0png.c" "%~dp0hostprof.c" ^
    "%ROOT%\build\recomp\obj\*.o" -lwinmm || exit /b 1
"%ROOT%\build\test\boot_test.exe" "%ROOT%\extract\eur" %*
