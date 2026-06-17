@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion

set "PROJECT_ROOT=%~dp0"
for %%I in ("%PROJECT_ROOT%.") do set "PROJECT_ROOT=%%~fI"
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "RELEASE_DIR=%BUILD_DIR%\Release"
set "BRIDGE_DLL=%RELEASE_DIR%\AveMediaBridge.dll"
set "LAB_EXE=%RELEASE_DIR%\AveMediaBridgeLabApp.exe"
set "FFMPEG_RUNTIME_DIR=%RELEASE_DIR%\Lib\ffmpeg"

echo ============================================================
echo AveMediaBridge Release build
echo ============================================================
echo Project root: %PROJECT_ROOT%
echo Build dir   : %BUILD_DIR%
echo.

where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: cmake.exe was not found in PATH.
    exit /b 1
)

echo [1/2] Configuring CMake...
cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

echo.
echo [2/2] Building Release targets...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo ERROR: CMake build failed.
    exit /b 1
)

echo.
echo Release outputs:
echo   DLL        : %BRIDGE_DLL%
echo   LabApp     : %LAB_EXE%
echo   FFmpeg DLLs: %FFMPEG_RUNTIME_DIR%
echo.
echo If AveVoice needs the updated runtime, copy/deploy AveMediaBridge.dll to:
echo   D:\rvc\c++\DragonianVoice\AveVoice\build\Release\Modules\AveMediaBridge.dll
echo.
echo SUCCESS: AveMediaBridge Release build completed.
exit /b 0
