@echo off
rem Setzt JAVA_HOME und GHIDRA auf die Kopien in tools\
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
for /d %%D in ("%ROOT%\tools\jdk-25*") do set "JAVA_HOME=%%~fD"
for /d %%D in ("%ROOT%\tools\ghidra_*") do set "GHIDRA=%%~fD"
set "PATH=%JAVA_HOME%\bin;%PATH%"
