@echo off
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC compiler 'cl' was not found.
    echo Run this script from an x64 Native Tools Command Prompt for Visual Studio.
    exit /b 1
)

cl /nologo /std:c++17 /EHsc /O2 /LD /Fo"%~dp0ActiveAeroMod.obj" "%~dp0ActiveAeroMod.cpp" /link /OUT:"%~dp0ActiveAeroMod.dll"
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built: %~dp0ActiveAeroMod.dll
endlocal
