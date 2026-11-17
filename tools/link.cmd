@echo off
rem Run once on Windows. Links the build into the frozen game's Mods folder.
rem Usage: tools\link.cmd "D:\Games\D2R-mephistorghast"
set GAME=%~1
if "%GAME%"=="" set GAME=D:\Games\D2R-mephistorghast
set TARGET=%~dp0..\build\mephistorghast
if not exist "%TARGET%" (echo Run "python tools\build.py" first. & exit /b 1)
rem Remove an old link (rmdir without /s only removes the link, never the build it points to).
if exist "%GAME%\Mods\mephistorghast" rmdir "%GAME%\Mods\mephistorghast" 2>nul
if exist "%GAME%\Mods\mephistorghast" (echo %GAME%\Mods\mephistorghast is a real folder, not a link. Move it away first. & exit /b 1)
if not exist "%GAME%\Mods" mkdir "%GAME%\Mods"
mklink /J "%GAME%\Mods\mephistorghast" "%TARGET%"
