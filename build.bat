@echo off
setlocal enabledelayedexpansion

rem MacTab build script.
rem
rem Usage:
rem   build.bat            release build
rem   build.bat debug      debug build
rem   build.bat clean      wipe the build directory
rem
rem Needs Visual Studio 2022 (or Build Tools 2022) with the "Desktop
rem development with C++" workload and a Windows 10/11 SDK >= 10.0.17763.0.

set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"

if /i "%~1"=="clean" (
    echo Removing %BUILD_DIR%
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    exit /b 0
)

set "CONFIG=Release"
if /i "%~1"=="debug" set "CONFIG=Debug"

rem --- Locate and enter the MSVC environment -------------------------------
rem vcvars sets WindowsSdkDir / WindowsSDKVersion, which CMakeLists uses to
rem find the C++/WinRT headers. Skip if we are already inside a dev prompt.
if not defined VCINSTALLDIR (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo ERROR: vswhere.exe not found. Install Visual Studio 2022 or Build Tools 2022.
        exit /b 1
    )

    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * ^
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
            -property installationPath`) do set "VSPATH=%%i"

    if not defined VSPATH (
        echo ERROR: No Visual Studio installation with the C++ toolset was found.
        echo        Install the "Desktop development with C++" workload.
        exit /b 1
    )

    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 (
        echo ERROR: Failed to initialise the MSVC environment.
        exit /b 1
    )
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake is not on PATH. Install CMake, or use the copy bundled
    echo        with Visual Studio ^(add it to PATH^).
    exit /b 1
)

rem --- Configure and build -------------------------------------------------
rem Ninja if available (much faster); otherwise fall back to the VS generator.
where ninja >nul 2>&1
if errorlevel 1 (
    set "GENERATOR=-G "Visual Studio 17 2022" -A x64"
    set "BUILDARGS=--config %CONFIG%"
) else (
    set "GENERATOR=-G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%"
    set "BUILDARGS="
)

cmake -S "%ROOT%" -B "%BUILD_DIR%" %GENERATOR%
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" %BUILDARGS% --parallel
if errorlevel 1 exit /b 1

echo.
for %%f in ("%BUILD_DIR%\bin\MacTab.exe") do (
    echo Built %%~ff  ^(%%~zf bytes^)
)
echo.
echo Run with --diag to write a log to %%LOCALAPPDATA%%\MacTab\diag.log
endlocal
