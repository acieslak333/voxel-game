@echo off
REM Copy assets\models\* into the built game's asset dir so a Blockbench export shows
REM up WITHOUT a full rebuild. Restart the game after (skins load at startup).
REM Loop:  Blockbench export -> tools\deploy-models.cmd -> rerun the game
setlocal
cd /d "%~dp0.."
set "DST=build\bin\assets\models"
if not exist "%DST%" (
  echo deploy-models: "%DST%" not found - build the game once first.
  exit /b 1
)
xcopy "assets\models" "%DST%" /e /y /i >nul
echo deploy-models: copied assets\models (per-model dirs) -^> %DST%   (restart the game to see them)
