@echo off
setlocal
rem =============================================================================
rem  EscoDump.asi - writes a decrypted copy of the game's module to disk so the
rem  Enhanced port's byte patterns can be worked out offline. Reads only.
rem  Output: tools\escodump\EscoDump.asi
rem =============================================================================
set "HERE=%~dp0"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo vcvars64.bat not found - install Visual Studio 2026 with the C++ workload.
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0"
cl /nologo /O2 /MT /W4 /EHa /std:c++17 /DNDEBUG /LD escodump.cpp /link /DLL /OUT:EscoDump.asi kernel32.lib user32.lib
if errorlevel 1 exit /b 1
echo Done: %HERE%EscoDump.asi
endlocal
