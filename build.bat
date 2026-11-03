@echo off
setlocal
where g++ >nul 2>nul || (echo need mingw-w64 g++ and windres on PATH & exit /b 1)
where windres >nul 2>nul || (echo need windres on PATH & exit /b 1)
if not exist build mkdir build
windres src\resource.rc -O coff -o build\resource.o -I src || exit /b 1
g++ -O2 -municode -mwindows -static -o build\bubbles.scr src\bubbles.cpp build\resource.o -lwinmm -lgdi32 -luser32 -ld3d9 || exit /b 1
echo built build\bubbles.scr
