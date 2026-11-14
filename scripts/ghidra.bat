@echo off
rem Startet die Ghidra-Oberfläche mit dem JDK aus tools\
call "%~dp0ghidra-env.bat"
start "" "%GHIDRA%\ghidraRun.bat"
