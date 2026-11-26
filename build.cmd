@echo off
rem Builds build\<toolset>\Release\vista-sync.exe with the VS 2019 Build Tools.
rem Usage: build.cmd [v141_xp|v142]   (default v141_xp, the toolset for the XP machine)
setlocal
set TOOLSET=%1
if "%TOOLSET%"=="" set TOOLSET=v141_xp
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist %CMAKE% set CMAKE=cmake
cd /d "%~dp0"
%CMAKE% -S . -B build\%TOOLSET% -G "Visual Studio 16 2019" -A Win32 -T %TOOLSET% || exit /b 1
%CMAKE% --build build\%TOOLSET% --config Release -- /m /nologo /v:minimal || exit /b 1
echo Built build\%TOOLSET%\Release\vista-sync.exe
