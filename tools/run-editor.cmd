@echo off
REM Launch ONE voxel-game editor standalone (without the hub).
REM   Usage:  run-editor.cmd [hub^|worldgen^|recipe^|particle]   (default: hub)
REM Ports:  hub 5005 . worldgen 5000 . particle 5001 . recipe 5003
REM Needs:  pip install flask pyyaml ruamel.yaml
setlocal
cd /d "%~dp0.."
set "NAME=%~1"
if "%NAME%"=="" set "NAME=hub"
set "SCRIPT="
if /i "%NAME%"=="hub"       set "SCRIPT=tools\hub.py"
if /i "%NAME%"=="genmap"    set "SCRIPT=tools\worldgen_tool.py"
if /i "%NAME%"=="world"     set "SCRIPT=tools\worldgen_tool.py"
if /i "%NAME%"=="worldgen"  set "SCRIPT=tools\worldgen_tool.py"
if /i "%NAME%"=="biome"     set "SCRIPT=tools\worldgen_tool.py"
if /i "%NAME%"=="flora"     set "SCRIPT=tools\worldgen_tool.py"
if /i "%NAME%"=="recipe"    set "SCRIPT=tools\recipe_tool.py"
if /i "%NAME%"=="recipes"   set "SCRIPT=tools\recipe_tool.py"
if /i "%NAME%"=="particle"  set "SCRIPT=tools\particle_tool.py"
if /i "%NAME%"=="particles" set "SCRIPT=tools\particle_tool.py"
if not defined SCRIPT (
  echo unknown editor "%NAME%"  ^(hub^|genmap^|biome^|recipe^|particle^)
  exit /b 2
)
set "PY=python"
where python >nul 2>nul || set "PY=py"
"%PY%" %SCRIPT%
if errorlevel 1 pause
