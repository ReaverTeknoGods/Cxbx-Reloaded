@ECHO off
SETLOCAL

:: Generate build files if not already present
IF NOT EXIST "build\Cxbx-Reloaded.sln" (
    ECHO Generating Visual Studio project files...
    cmake -B build -G "Visual Studio 17 2022" -A Win32
    IF ERRORLEVEL 1 (
        ECHO CMake generation failed!
        EXIT /B 1
    )
)

:: Build Release by default, or use first argument
SET CONFIG=Release
IF NOT "%1"=="" SET CONFIG=%1

ECHO Building %CONFIG%...
cmake --build build --config %CONFIG% -- /m
IF ERRORLEVEL 1 (
    ECHO Build failed!
    EXIT /B 1
)

ECHO.
ECHO Build successful! Output in build\bin\%CONFIG%\
