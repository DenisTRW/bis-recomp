@echo off
rem Ghidra ohne Oberfläche (analyzeHeadless), alle Argumente werden durchgereicht
call "%~dp0ghidra-env.bat"
call "%GHIDRA%\support\analyzeHeadless.bat" %*
