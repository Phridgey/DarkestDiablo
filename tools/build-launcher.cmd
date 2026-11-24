@echo off
rem Builds the launcher into build\launcher\DarkestDiabloLauncher.exe (one file, no Python needed to run it).
rem Works with Python 3.12 to 3.14 (PyInstaller 6.22.3; 6.11.1 refused Python 3.14).
rem It bundles the game file reader (build\extract\extract.exe), built first if missing.
rem Usage (from the repo folder): tools\build-launcher.cmd
setlocal
set ROOT=%~dp0..
if not exist "%ROOT%\build\extract\extract.exe" (call "%ROOT%\tools\build-extract.cmd" || exit /b 1)
python -m pip install --quiet pyinstaller==6.22.3 || exit /b 1
python -m PyInstaller --noconfirm --onefile --windowed --name DarkestDiabloLauncher ^
  --icon "%ROOT%\launcher\darkestdiablo.ico" ^
  --distpath "%ROOT%\build\launcher" --workpath "%ROOT%\build\launcher-obj" --specpath "%ROOT%\build\launcher-obj" ^
  --add-binary "%ROOT%\build\extract\extract.exe;." ^
  --add-data "%ROOT%\launcher\darkestdiablo.ico;." ^
  --paths "%ROOT%\launcher" "%ROOT%\launcher\darkestdiablo_launcher.py" || exit /b 1
echo.
echo Built build\launcher\DarkestDiabloLauncher.exe
