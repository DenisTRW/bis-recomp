@echo off
rem Builds BISLauncher.exe in the repository root.
rem Uses the C# compiler and WPF that ship with Windows (.NET Framework 4.x) - no SDK needed.
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319"
if not exist "%FW%\csc.exe" set "FW=%WINDIR%\Microsoft.NET\Framework\v4.0.30319"
if not exist "%FW%\csc.exe" (
    echo .NET Framework 4.x C# compiler not found.
    exit /b 1
)
"%FW%\csc.exe" /nologo /target:winexe /optimize+ /out:"%ROOT%\BISLauncher.exe" ^
    /r:"%FW%\WPF\PresentationFramework.dll" /r:"%FW%\WPF\PresentationCore.dll" /r:"%FW%\WPF\WindowsBase.dll" ^
    /r:"%FW%\System.Xaml.dll" /r:"%FW%\System.Web.Extensions.dll" /r:System.dll /r:System.Core.dll ^
    "%~dp0Launcher.cs" || exit /b 1
copy /y "%~dp0game.json" "%ROOT%\game.json" >nul
echo Built %ROOT%\BISLauncher.exe
