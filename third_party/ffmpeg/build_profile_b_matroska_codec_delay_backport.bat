@echo off
setlocal EnableExtensions

if "%~1"=="" goto :Usage
if "%~2"=="" goto :Usage

set "ARCHIVE=%~f1"
set "LAB_ROOT=%~f2"
set "SOURCE_DIR=%LAB_ROOT%\versions\ffmpeg-7.1.4-matroska-codec-delay-backport"
set "BUILD_DIR=%LAB_ROOT%\builds\ffmpeg-7.1.4-matroska-codec-delay-backport"
set "PREFIX_WIN=%LAB_ROOT%\install\profile_b_ffmpeg_7_1_4_matroska_codec_delay_backport"
set "SCRIPT_DIR=%~dp0"
set "PATCH=%SCRIPT_DIR%patches\0880458e4c-matroska-codec-delay-skip.patch"
set "VCVARS=C:\PROGRA~2\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%ARCHIVE%" (
    echo ERROR: source archive not found: %ARCHIVE%
    exit /b 1
)
if not exist "%PATCH%" (
    echo ERROR: patch not found: %PATCH%
    exit /b 1
)
if exist "%SOURCE_DIR%" (
    echo ERROR: refusing to overwrite source: %SOURCE_DIR%
    exit /b 1
)
if exist "%BUILD_DIR%" (
    echo ERROR: refusing to overwrite build: %BUILD_DIR%
    exit /b 1
)
if exist "%PREFIX_WIN%" (
    echo ERROR: refusing to overwrite install: %PREFIX_WIN%
    exit /b 1
)

for /f %%H in ('powershell -NoProfile -Command "(Get-FileHash -LiteralPath '%ARCHIVE%' -Algorithm SHA256).Hash"') do set "ARCHIVE_SHA256=%%H"
if /I not "%ARCHIVE_SHA256%"=="71F4AAC3573ED9060489CB62526A6C7DDA815AE10993789611ACD7BE9FA9FBF4" (
    echo ERROR: unexpected FFmpeg 7.1.4 archive SHA-256: %ARCHIVE_SHA256%
    exit /b 1
)
for /f %%H in ('powershell -NoProfile -Command "(Get-FileHash -LiteralPath '%PATCH%' -Algorithm SHA256).Hash"') do set "PATCH_SHA256=%%H"
if /I not "%PATCH_SHA256%"=="B5130FC2814CB3E04B2EF4331B687C2EB6C832A288749F479936082955995A80" (
    echo ERROR: unexpected backport patch SHA-256: %PATCH_SHA256%
    exit /b 1
)

set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
if not exist "%SEVENZIP%" (
    echo ERROR: 7-Zip not found: %SEVENZIP%
    exit /b 1
)
if not exist "%VCVARS%" (
    echo ERROR: VS 2022 x64 toolchain not found
    exit /b 1
)
if not exist "%BASH%" (
    echo ERROR: MSYS2 bash not found
    exit /b 1
)

if not exist "%LAB_ROOT%\versions" mkdir "%LAB_ROOT%\versions"
if not exist "%LAB_ROOT%\builds" mkdir "%LAB_ROOT%\builds"
if not exist "%LAB_ROOT%\install" mkdir "%LAB_ROOT%\install"
set "EXTRACT_DIR=%LAB_ROOT%\versions\ffmpeg-7.1.4-extract"
mkdir "%EXTRACT_DIR%" || exit /b 1
"%SEVENZIP%" x "%ARCHIVE%" "-o%EXTRACT_DIR%" -y >nul || exit /b 1
"%SEVENZIP%" x "%EXTRACT_DIR%\ffmpeg-7.1.4.tar" "-o%LAB_ROOT%\versions" -y >nul || exit /b 1
rmdir /S /Q "%EXTRACT_DIR%"
ren "%LAB_ROOT%\versions\ffmpeg-7.1.4" "ffmpeg-7.1.4-matroska-codec-delay-backport" || exit /b 1
mkdir "%BUILD_DIR%" || exit /b 1
mkdir "%PREFIX_WIN%" || exit /b 1

git -C "%SOURCE_DIR%" apply --check --no-index --include=libavformat/matroskadec.c "%PATCH%" || exit /b 1
git -C "%SOURCE_DIR%" apply --no-index --include=libavformat/matroskadec.c "%PATCH%" || exit /b 1

call "%VCVARS%" >nul || exit /b 1
set "PATH=%PATH%;C:\msys64\usr\bin;C:\msys64\mingw64\bin"
set "MSYS2_PATH_TYPE=inherit"

for /f "delims=" %%P in ('powershell -NoProfile -Command "('%SOURCE_DIR%' -replace '\\','/' -replace '^([A-Za-z]):','/$1').ToLowerInvariant()"') do set "SOURCE_MSYS=%%P"
for /f "delims=" %%P in ('powershell -NoProfile -Command "('%BUILD_DIR%' -replace '\\','/' -replace '^([A-Za-z]):','/$1').ToLowerInvariant()"') do set "BUILD_MSYS=%%P"
for /f "delims=" %%P in ('powershell -NoProfile -Command "('%PREFIX_WIN%' -replace '\\','/' -replace '^([A-Za-z]):','/$1').ToLowerInvariant()"') do set "PREFIX_MSYS=%%P"

set "OPTS=--toolchain=msvc --arch=x86_64 --target-os=win64 --prefix=%PREFIX_MSYS%"
set "OPTS=%OPTS% --enable-shared --disable-static --disable-everything --disable-doc --disable-programs"
set "OPTS=%OPTS% --disable-network --disable-avdevice --disable-avfilter --disable-swscale --disable-hwaccels"
set "OPTS=%OPTS% --enable-avformat --enable-avcodec --enable-avutil --enable-swresample --enable-protocol=file"
set "OPTS=%OPTS% --enable-demuxer=wav --enable-demuxer=mp3 --enable-demuxer=mov --enable-demuxer=matroska"
set "OPTS=%OPTS% --enable-demuxer=nut --enable-demuxer=flac --enable-demuxer=ogg --enable-demuxer=aac"
set "OPTS=%OPTS% --enable-demuxer=avi --enable-demuxer=aiff --enable-demuxer=au --enable-demuxer=w64"
set "OPTS=%OPTS% --enable-demuxer=caf --enable-demuxer=mpegts --enable-demuxer=mpegps --enable-demuxer=ac3"
set "OPTS=%OPTS% --enable-demuxer=eac3 --enable-demuxer=asf --enable-demuxer=flv --enable-muxer=wav"
set "OPTS=%OPTS% --enable-decoder=pcm_s16le --enable-decoder=pcm_s24le --enable-decoder=pcm_s32le"
set "OPTS=%OPTS% --enable-decoder=pcm_f32le --enable-decoder=pcm_f64le --enable-decoder=pcm_u8"
set "OPTS=%OPTS% --enable-decoder=pcm_s16be --enable-decoder=pcm_mulaw --enable-decoder=pcm_alaw"
set "OPTS=%OPTS% --enable-decoder=mp3 --enable-decoder=mp3float --enable-decoder=aac --enable-decoder=flac"
set "OPTS=%OPTS% --enable-decoder=vorbis --enable-decoder=opus --enable-decoder=alac --enable-decoder=mp2"
set "OPTS=%OPTS% --enable-decoder=ac3 --enable-decoder=eac3 --enable-decoder=wmav1 --enable-decoder=wmav2"
set "OPTS=%OPTS% --enable-encoder=pcm_s16le"
set "OPTS=%OPTS% --enable-parser=aac --enable-parser=mpegaudio --enable-parser=vorbis --enable-parser=opus"
set "OPTS=%OPTS% --enable-parser=h264 --enable-parser=mpegvideo --enable-parser=ac3"

echo CONFIGURE_COMMAND=%SOURCE_MSYS%/configure %OPTS%
"%BASH%" --noprofile --norc -c "cd '%BUILD_MSYS%' && '%SOURCE_MSYS%/configure' %OPTS% && make -j%NUMBER_OF_PROCESSORS% && make install" || exit /b 1

if not exist "%PREFIX_WIN%\lib" mkdir "%PREFIX_WIN%\lib"
for %%L in (avformat avcodec avutil swresample) do if exist "%PREFIX_WIN%\bin\%%L.lib" copy /Y "%PREFIX_WIN%\bin\%%L.lib" "%PREFIX_WIN%\lib\%%L.lib" >nul
for %%D in (avcodec-61.dll avformat-61.dll avutil-59.dll swresample-5.dll) do (
    if not exist "%PREFIX_WIN%\bin\%%D" (
        echo ERROR: missing %%D
        exit /b 1
    )
)

echo FFMPEG_714_MATROSKA_CODEC_DELAY_BACKPORT_BUILD_OK
exit /b 0

:Usage
echo usage: %~nx0 ^<ffmpeg-7.1.4.tar.xz^> ^<ffmpeglab-output-root^>
exit /b 2
