@echo off
rem Builds build\latenzy.exe  (requires mingw-w64 GCC 10+ in PATH, e.g. WinLibs or MSYS2)
setlocal
where g++ >nul 2>nul || (echo g++ not found in PATH & exit /b 1)
if not exist build mkdir build
g++ -std=c++20 -O2 -municode -DUNICODE -D_UNICODE src\latenzy.cpp -o build\latenzy.exe ^
    -lwinhttp -liphlpapi -lshell32 -luser32 -lgdi32 -lcomctl32 -lgdiplus -luxtheme -lole32 ^
    -mwindows -static -s
if errorlevel 1 (echo Build failed & exit /b 1)
echo Built build\latenzy.exe
