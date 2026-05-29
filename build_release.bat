@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion

set "PROJECT_ROOT=%~dp0"
for %%I in ("%PROJECT_ROOT%.") do set "PROJECT_ROOT=%%~fI"
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "FFMPEG_DIR=%PROJECT_ROOT%\ffmpeg"
set "RELEASE_DIR=%BUILD_DIR%\Release"
set "LAB_EXE=%RELEASE_DIR%\AveMediaBridgeLabApp.exe"
set "BRIDGE_DLL=%RELEASE_DIR%\AveMediaBridge.dll"
set "AVEVOICE_RELEASE=D:\rvc\c++\DragonianVoice\AveVoice\build\Release"

echo ============================================================
echo AveMediaBridge Release build
echo ============================================================
echo Project root     : %PROJECT_ROOT%
echo FFmpeg dir       : %FFMPEG_DIR%
echo Build dir        : %BUILD_DIR%
echo Lab executable   : %LAB_EXE%
echo Bridge DLL       : %BRIDGE_DLL%
echo AveVoice staging : %AVEVOICE_RELEASE%
echo.

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe was not found. Run from x64 Native Tools Command Prompt for VS 2022.
    exit /b 1
)

where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: cmake.exe was not found in PATH.
    exit /b 1
)

call :CheckFfmpegLayout || exit /b 1

echo [1/4] Configuring CMake...
cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

echo.
echo [2/4] Building Release targets...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo ERROR: CMake build failed.
    exit /b 1
)

if exist "%RELEASE_DIR%\AveMediaBridge.exe" (
    del /F /Q "%RELEASE_DIR%\AveMediaBridge.exe" >nul 2>nul
)

if not exist "%LAB_EXE%" (
    echo ERROR: Expected Lab executable was not produced:
    echo   %LAB_EXE%
    exit /b 1
)
if not exist "%BRIDGE_DLL%" (
    echo ERROR: Expected DLL was not produced:
    echo   %BRIDGE_DLL%
    exit /b 1
)

echo.
echo [3/4] Verifying private FFmpeg DLL layout for LabApp...
for %%D in (avformat avcodec avutil swresample) do (
    dir /b "%RELEASE_DIR%\Lib\ffmpeg\%%D-*.dll" >nul 2>nul
    if errorlevel 1 (
        echo ERROR: Missing %RELEASE_DIR%\Lib\ffmpeg\%%D-*.dll
        exit /b 1
    )
)

echo.
echo [4/4] Staging AveMediaBridge module for future AveVoice runtime...
if not exist "%AVEVOICE_RELEASE%\Modules" mkdir "%AVEVOICE_RELEASE%\Modules"
if not exist "%AVEVOICE_RELEASE%\Lib\ffmpeg" mkdir "%AVEVOICE_RELEASE%\Lib\ffmpeg"

copy /Y "%BRIDGE_DLL%" "%AVEVOICE_RELEASE%\Modules\" >nul
if errorlevel 1 (
    echo ERROR: Failed to stage AveMediaBridge.dll.
    exit /b 1
)

for %%D in (avformat avcodec avutil swresample) do (
    for %%F in ("%FFMPEG_DIR%\bin\%%D-*.dll") do (
        if exist "%%~fF" (
            echo Staging %%~nxF
            copy /Y "%%~fF" "%AVEVOICE_RELEASE%\Lib\ffmpeg\" >nul
            if errorlevel 1 (
                echo ERROR: Failed to stage %%~nxF
                exit /b 1
            )
        )
    )
)

echo.
echo SUCCESS: AveMediaBridge Release build completed.
echo CMake targets:
echo   AveMediaBridgeCore ^(static library^)
echo   AveMediaBridge ^(DLL^)
echo   AveMediaBridgeLabApp ^(console lab app^)
echo.
echo Lab executable:
echo   %LAB_EXE%
echo DLL:
echo   %BRIDGE_DLL%
echo Future AveVoice module:
echo   %AVEVOICE_RELEASE%\Modules\AveMediaBridge.dll
echo Future AveVoice private FFmpeg dir:
echo   %AVEVOICE_RELEASE%\Lib\ffmpeg
if exist "%AVEVOICE_RELEASE%\AveVoice.exe" (
    echo AveVoice.exe:
    echo   %AVEVOICE_RELEASE%\AveVoice.exe
) else (
    echo NOTE: AveVoice.exe is not present in the staging folder yet. The module layout was prepared without connecting AveVoice to it.
)
exit /b 0

:CheckFfmpegLayout
set "MISSING=0"
if not exist "%FFMPEG_DIR%\include\libavformat\avformat.h" (
    echo ERROR: Missing %FFMPEG_DIR%\include\libavformat\avformat.h
    set "MISSING=1"
)
if not exist "%FFMPEG_DIR%\include\libavcodec\avcodec.h" (
    echo ERROR: Missing %FFMPEG_DIR%\include\libavcodec\avcodec.h
    set "MISSING=1"
)
if not exist "%FFMPEG_DIR%\include\libavutil\avutil.h" (
    echo ERROR: Missing %FFMPEG_DIR%\include\libavutil\avutil.h
    set "MISSING=1"
)
if not exist "%FFMPEG_DIR%\include\libswresample\swresample.h" (
    echo ERROR: Missing %FFMPEG_DIR%\include\libswresample\swresample.h
    set "MISSING=1"
)
for %%L in (avformat avcodec avutil swresample) do (
    if not exist "%FFMPEG_DIR%\lib\%%L.lib" (
        echo ERROR: Missing %FFMPEG_DIR%\lib\%%L.lib
        set "MISSING=1"
    )
)
for %%D in (avformat avcodec avutil swresample) do (
    dir /b "%FFMPEG_DIR%\bin\%%D-*.dll" >nul 2>nul
    if errorlevel 1 (
        echo ERROR: Missing %FFMPEG_DIR%\bin\%%D-*.dll
        set "MISSING=1"
    )
)
if "%MISSING%"=="1" (
    echo ERROR: Local FFmpeg Profile B layout is incomplete.
    exit /b 1
)
echo FFmpeg layout: OK
exit /b 0
