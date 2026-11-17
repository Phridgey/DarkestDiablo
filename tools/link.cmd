@echo off
rem Run once on Windows. Links the build into the frozen game's Mods folder.
rem Usage: tools\link.cmd "D:\Games\D2R-mephistorghast"
set GAME=%~1
if "%GAME%"=="" set GAME=D:\Games\D2R-mephistorghast
set TARGET=%~dp0..\build\mephistorghast
if not exist "%TARGET%" (echo Run "python tools\build.py" first. & exit /b 1)
if exist "%GAME%\Mods\mephistorghast" rmdir /s /q "%GAME%\Mods\mephistorghast"
if not exist "%GAME%\Mods" mkdir "%GAME%\Mods"
mklink /J "%GAME%\Mods\mephistorghast" "%TARGET%"
