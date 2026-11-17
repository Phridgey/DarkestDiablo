@echo off
rem Run once on Windows. Links the build into the frozen game's Mods folder.
rem Usage: tools\link.cmd "D:\Games\D2R-darkestdiablo"
set GAME=%~1
if "%GAME%"=="" set GAME=D:\Games\D2R-darkestdiablo
set TARGET=%~dp0..\build\darkestdiablo
if not exist "%TARGET%" (echo Run "python tools\build.py" first. & exit /b 1)
rem Remove an old link (rmdir without /s only removes the link, never the build it points to).
if exist "%GAME%\Mods\darkestdiablo" rmdir "%GAME%\Mods\darkestdiablo" 2>nul
if exist "%GAME%\Mods\darkestdiablo" (echo %GAME%\Mods\darkestdiablo is a real folder, not a link. Move it away first. & exit /b 1)
if not exist "%GAME%\Mods" mkdir "%GAME%\Mods"
mklink /J "%GAME%\Mods\darkestdiablo" "%TARGET%"
