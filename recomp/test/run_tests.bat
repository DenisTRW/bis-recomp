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
"%OUT%\test_pad_update.exe" "%ROOT%\extract\eur\arm9\arm9.bin" || exit /b 1
echo.
echo === Scheduler-Tests ===
gcc -O2 -Wall -Wextra -I "%ROOT%\recomp\include" -I "%ROOT%\recomp\runtime" ^
    -o "%OUT%\test_sched.exe" "%~dp0test_sched.c" "%ROOT%\recomp\runtime\sched.c" || exit /b 1
"%OUT%\test_sched.exe" || exit /b 1

echo.
echo === MI_CpuFillFast ===
python -I "%ROOT%\recomp\recomp.py" "%ROOT%" --only MI_CpuFillFast --out "%OUT%\fill" || exit /b 1
gcc -O2 -w -I "%ROOT%\recomp\include" -I "%OUT%\fill" -I "%~dp0." ^
    -o "%OUT%\test_memfill.exe" "%~dp0test_memfill.c" "%~dp0harness.c" "%~dp0functable_empty.c" "%OUT%\fill\itcm.c" || exit /b 1
"%OUT%\test_memfill.exe" || exit /b 1

echo.
echo === MI_CpuCopyFast ===
python -I "%ROOT%\recomp\recomp.py" "%ROOT%" --only MI_CpuCopyFast,func_01ff82d8,MI_CpuCopyFast_Rest10,func_01ff82f0,func_01ff8300,func_01ff8310,func_01ff8320,func_01ff8330,func_01ff8340,func_01ff8350,func_01ff8360,func_01ff8370,MI_CpuCopyFastBackward,func_01ff83b4,MI_CpuCopyFastBackward_Rest10,func_01ff83cc,func_01ff83dc,func_01ff83ec,func_01ff83fc,func_01ff840c,func_01ff841c,func_01ff842c,func_01ff843c,func_01ff844c,func_01ff845c --out "%OUT%\copy" || exit /b 1
gcc -O2 -w -I "%ROOT%\recomp\include" -I "%OUT%\copy" -I "%~dp0." ^
    -o "%OUT%\test_memcopy.exe" "%~dp0test_memcopy.c" "%~dp0harness.c" "%OUT%\copy\functable.c" "%OUT%\copy\itcm.c" || exit /b 1
"%OUT%\test_memcopy.exe" || exit /b 1

echo.
echo === 64-Bit-Division (mehrere Einstiege) ===
python -I "%ROOT%\recomp\recomp.py" "%ROOT%" --only func_02045164,func_02045324,func_0204536c,func_02045578 --out "%OUT%\lldiv" || exit /b 1
gcc -O2 -w -I "%ROOT%\recomp\include" -I "%OUT%\lldiv" -I "%~dp0." ^
    -o "%OUT%\test_lldiv.exe" "%~dp0test_lldiv.c" "%~dp0harness.c" "%OUT%\lldiv\functable.c" "%OUT%\lldiv\main.c" || exit /b 1
"%OUT%\test_lldiv.exe" || exit /b 1
