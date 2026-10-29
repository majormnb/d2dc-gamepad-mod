@echo off
rem Build Div2Patch.dll with the WSL MinGW i686 cross-compiler.
rem Run from this folder (gamepad_mod). The known-good command line
rem (NOTES §149): -static-libgcc is REQUIRED (without it the DLL pulls
rem libgcc_s_dw2-1.dll and the game silently fails to load it);
rem -lwinmm is required for the timer/mixer imports.
wsl.exe -e bash -lc "cd \"$(wslpath '%CD%')\" && i686-w64-mingw32-g++ -shared -O2 -static -static-libgcc -o Div2Patch.dll div2gamepad.c div2gamepad.def -lwinmm"
if errorlevel 1 goto fail
wsl.exe -e bash -lc "cd \"$(wslpath '%CD%')\" && sha256sum Div2Patch.dll"
echo BUILD OK
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
