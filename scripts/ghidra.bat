@echo off
rem Startet die Ghidra-Oberflaeche mit dem JDK aus tools\
call "%~dp0ghidra-env.bat"
start "" "%GHIDRA%\ghidraRun.bat"
