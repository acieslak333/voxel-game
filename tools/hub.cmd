@echo off
REM Launch the voxel-game Tools Hub -> http://127.0.0.1:5005  (double-click me)
REM A dashboard that starts/stops the editors and browses blocks/items/recipes.
REM Needs:  pip install flask pyyaml
setlocal
cd /d "%~dp0.."
set "PY=python"
where python >nul 2>nul || set "PY=py"
"%PY%" tools\hub.py %*
REM Keep the window open if it exited with an error (e.g. missing deps).
if errorlevel 1 pause
