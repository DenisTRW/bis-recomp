@echo off
rem Startet den PC-Prototyp im Fenster (SDL3). Baut vorher alles, was sich geaendert hat.
rem Tasten: WASD (oder Pfeile) = Steuerkreuz, K = A, J = B, I = X, U = Y, Enter = Start,
rem         Ruecktaste = Select, Q = L, E = R, F11 = Vollbild, Esc = Beenden
setlocal
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
set "PATH=%ROOT%\tools\w64devkit\bin;%PATH%"
set "SDL=%ROOT%\tools\SDL3-3.4.20\x86_64-w64-mingw32"
if not exist "%ROOT%\build\pc" mkdir "%ROOT%\build\pc"
python -I "%ROOT%\recomp\recomp.py" "%ROOT%" || exit /b 1
python -I "%ROOT%\recomp\gen_ninja.py" "%ROOT%" -O1 || exit /b 1
"%ROOT%\tools\ninja.exe" -C "%ROOT%\build\recomp" || exit /b 1
gcc -O2 -w -DRC_TRACE -DPLATFORM_SDL -I "%ROOT%\recomp\include" -I "%ROOT%\build\recomp" -I "%~dp0." -I "%SDL%\include" ^
    -o "%ROOT%\build\pc\bis_pc.exe" "%~dp0boot_test.c" "%~dp0sched.c" "%~dp0threads_hle.c" "%~dp0arm7_hle.c" ^
    "%~dp0card.c" "%~dp0vram.c" "%~dp0render2d.c" "%~dp0gpu3d.c" "%~dp0smc.c" "%~dp0png.c" "%~dp0hostprof.c" "%~dp0frontend_sdl.c" ^
    "%ROOT%\build\recomp\obj\*.o" -L "%SDL%\lib" -lSDL3 -lwinmm || exit /b 1
copy /y "%SDL%\bin\SDL3.dll" "%ROOT%\build\pc\" >nul
rem RecompLauncher-Beschreibung neben das Programm legen (siehe recomplauncher.json)
copy /y "%ROOT%\recomplauncher.json" "%ROOT%\build\pc\" >nul
"%ROOT%\build\pc\bis_pc.exe" "%ROOT%\extract\eur" %*
