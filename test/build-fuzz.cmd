@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build\fuzz mkdir ..\build\fuzz

rem /fsanitize=address puts every read and write in the decoders under
rem AddressSanitizer. /Zi is what gives the report a usable stack.
cl /nologo /std:c++17 /Od /Zi /MT /EHsc /W4 /DUNICODE /D_UNICODE ^
   /fsanitize=address ^
   /Fo..\build\fuzz\ /Fd..\build\fuzz\fuzz.pdb ^
   fuzz.cpp ..\OzImage.cpp /Fe..\build\fuzz.exe ^
   /link ole32.lib windowscodecs.lib gdi32.lib || exit /b 1

echo OK: ..\build\fuzz.exe
