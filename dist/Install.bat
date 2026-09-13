@echo off
setlocal
rem Installs EscoEditor.asi + EscoFlare.fx (+ ini if none yet) into FiveM's
rem plugins folder. EscoFlare.fx is the lens flare shader; the plugin copies it
rem into ReShade's own shader folder when the game starts.
rem Removes the plugin's old name, EditorCamSpeed.asi - the two must never load
rem together (they hook the same functions) - and EscoDOF.fx, the shader that
rem 2.5 - 2.8 used and 2.9 no longer has. Your EditorCamSpeed.ini settings are
rem carried over automatically on the first start.
set "DST=%LOCALAPPDATA%\FiveM\FiveM.app\plugins"
if not exist "%DST%" (
    echo FiveM plugins folder not found at "%DST%".
    pause
    exit /b 1
)
copy /Y "%~dp0EscoEditor.asi" "%DST%\EscoEditor.asi" >nul
if errorlevel 1 (
    echo Could not copy the .asi - is FiveM running? Close it and try again.
    pause
    exit /b 1
)
copy /Y "%~dp0EscoFlare.fx" "%DST%\EscoFlare.fx" >nul
if errorlevel 1 echo Could not copy EscoFlare.fx - the lens flares will be unavailable.
if exist "%DST%\EditorCamSpeed.asi" (
    del /Q "%DST%\EditorCamSpeed.asi"
    echo Removed the old EditorCamSpeed.asi.
)
if exist "%DST%\EscoDOF.fx" (
    del /Q "%DST%\EscoDOF.fx"
    echo Removed EscoDOF.fx - 2.9 no longer uses it.
)
if exist "%DST%\reshade-shaders\Shaders\EscoDOF.fx" (
    del /Q "%DST%\reshade-shaders\Shaders\EscoDOF.fx"
    echo Removed reshade-shaders\Shaders\EscoDOF.fx.
)
if not exist "%DST%\EscoEditor.ini" if not exist "%DST%\EditorCamSpeed.ini" copy /Y "%DST%\EscoEditor.ini" "%DST%\EscoEditor.ini" >nul 2>nul
if not exist "%DST%\EscoEditor.ini" if not exist "%DST%\EditorCamSpeed.ini" copy /Y "%~dp0EscoEditor.ini" "%DST%\EscoEditor.ini" >nul
echo Installed EscoEditor.asi + EscoFlare.fx -^> %DST%
echo.
echo The light editor window needs ReShade 6.3+ WITH ADD-ON SUPPORT in FiveM
echo (the build that loads .addon64 files). The plugin registers with it by itself.
echo In the Rockstar Editor, open any marker's menu: the last row is EscoEditor.
echo Its Lights page opens the light editor and changes its key (L by default).
echo Log: %DST%\EscoEditor.log
pause
endlocal
