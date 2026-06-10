@echo off
rem Build the Windows release configuration from a plain (non-developer) shell:
rem loads the MSVC environment, then configures + builds the windows-release
rem preset. vcpkg is expected at %USERPROFILE%\vcpkg (see CMakePresets.json).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0.." || exit /b 1
cmake --preset windows-release || exit /b 1
cmake --build build\windows-release || exit /b 1
echo.
echo Build complete: build\windows-release\game.exe
endlocal
