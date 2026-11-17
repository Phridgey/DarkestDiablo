@echo off
rem Builds the launcher into build\launcher\DarkestDiabloLauncher.exe (one file, no Python needed to run it).
rem Usage (from the repo folder): tools\build-launcher.cmd
setlocal
set ROOT=%~dp0..
python -m pip install --quiet pyinstaller==6.11.1 || exit /b 1
python -m PyInstaller --noconfirm --onefile --windowed --name DarkestDiabloLauncher ^
  --distpath "%ROOT%\build\launcher" --workpath "%ROOT%\build\launcher-obj" --specpath "%ROOT%\build\launcher-obj" ^
  --paths "%ROOT%\launcher" "%ROOT%\launcher\darkestdiablo_launcher.py" || exit /b 1
echo.
echo Built build\launcher\DarkestDiabloLauncher.exe
