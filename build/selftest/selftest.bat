@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 selftest.cpp ..\..\src\minhook\src\buffer.c ..\..\src\minhook\src\hook.c ..\..\src\minhook\src\trampoline.c ..\..\src\minhook\src\hde\hde64.c /link kernel32.lib user32.lib
echo cl exit %errorlevel%
