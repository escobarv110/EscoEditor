@echo off
setlocal
rem =============================================================================
rem  EscoEditor.asi - build script (Visual Studio 2026, x64, static CRT)
rem  Output: dist\EscoEditor.asi
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

if not exist "%HERE%build" mkdir "%HERE%build"

echo [1/4] Resources (every FiveM game build id, see fx_asi_build_all.rc)...
rc /nologo /fo "%HERE%build\fx_asi_build_all.res" "%HERE%fx_asi_build_all.rc"
if errorlevel 1 exit /b 1
pushd "%HERE%"
rc /nologo /fo "%HERE%build\escoflare.res" "%HERE%escoflare.rc"
set "RCERR=%errorlevel%"
popd
if not "%RCERR%"=="0" exit /b 1

echo [2/4] Compiling the plugin...
cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DNDEBUG /Fo"%HERE%build\\" /c "%HERE%src\main.cpp"
if errorlevel 1 exit /b 1

echo [3/4] Compiling MinHook (BSD-2)...
cl /nologo /O2 /MT /W3 /DNDEBUG /Fo"%HERE%build\\" /c "%HERE%src\minhook\src\buffer.c" "%HERE%src\minhook\src\hook.c" "%HERE%src\minhook\src\trampoline.c" "%HERE%src\minhook\src\hde\hde64.c"
if errorlevel 1 exit /b 1

echo [4/4] Linking...
link /nologo /DLL /MAP:"%HERE%build\EscoEditor.map" /OUT:"%HERE%build\EscoEditor.asi" "%HERE%build\main.obj" "%HERE%build\buffer.obj" "%HERE%build\hook.obj" "%HERE%build\trampoline.obj" "%HERE%build\hde64.obj" "%HERE%build\fx_asi_build_all.res" "%HERE%build\escoflare.res" kernel32.lib user32.lib
if errorlevel 1 exit /b 1

if not exist "%HERE%dist" mkdir "%HERE%dist"
copy /Y "%HERE%build\EscoEditor.asi" "%HERE%dist\" >nul
echo Done: %HERE%dist\EscoEditor.asi
endlocal
