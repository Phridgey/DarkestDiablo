@echo off
rem Builds build\extract\extract.exe (the game file reader) with CascLib. Needs Visual Studio's C++ tools.
rem Usage (from the repo folder): tools\build-extract.cmd
setlocal
set ROOT=%~dp0..
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo Visual Studio not found. & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio C++ tools not found. Install "Desktop development with C++". & exit /b 1)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cmake -S "%ROOT%\tools\extract" -B "%ROOT%\build\extract-obj" -A x64 || exit /b 1
cmake --build "%ROOT%\build\extract-obj" --config Release || exit /b 1
echo Built build\extract\extract.exe
