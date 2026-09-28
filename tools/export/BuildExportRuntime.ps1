param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][string]$LabRoot,
    [Parameter(Mandatory)][string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$archive = (Resolve-Path -LiteralPath $Archive).Path
$lab = [IO.Path]::GetFullPath($LabRoot)
$output = [IO.Path]::GetFullPath($OutputRoot)
if (-not $output.StartsWith($lab.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'OutputRoot must be a fresh child of LabRoot' }
if (Test-Path -LiteralPath $output) { throw "Refusing existing OutputRoot: $output" }
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$patch = Join-Path $repo 'third_party\ffmpeg\patches\0880458e4c-matroska-codec-delay-skip.patch'
$recipe = Join-Path $repo 'third_party\ffmpeg\build_profile_b_matroska_codec_delay_backport.bat'
$baseline = Get-Content -Raw (Join-Path $repo 'third_party\ffmpeg\build-manifest\ffmpeg-7.1.4-matroska-codec-delay-backport.json') | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne '71F4AAC3573ED9060489CB62526A6C7DDA815AE10993789611ACD7BE9FA9FBF4') { throw 'Archive hash mismatch' }
if ((Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash -ne 'B5130FC2814CB3E04B2EF4331B687C2EB6C832A288749F479936082955995A80') { throw 'Patch hash mismatch' }
if ($baseline.backportCommit -ne '0880458e4c27337718ca836e1193803a089fc690') { throw 'Unexpected baseline backport' }
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$vcvars = 'C:\PROGRA~2\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$bash = 'C:\msys64\usr\bin\bash.exe'
foreach ($tool in @($sevenZip,$vcvars,$bash)) { if (-not (Test-Path -LiteralPath $tool)) { throw "Missing tool: $tool" } }
$source = Join-Path $output 'source\ffmpeg-7.1.4'
$build = Join-Path $output 'build'
$install = Join-Path $output 'install'
New-Item -ItemType Directory -Path $output,$(Split-Path $source -Parent),$build,$install | Out-Null
$extract = Join-Path $output 'extract'
New-Item -ItemType Directory -Path $extract | Out-Null
& $sevenZip x $archive "-o$extract" -y | Out-Null
if ($LASTEXITCODE) { throw '7z xz extraction failed' }
& $sevenZip x (Join-Path $extract 'ffmpeg-7.1.4.tar') "-o$(Split-Path $source -Parent)" -y | Out-Null
if ($LASTEXITCODE) { throw '7z tar extraction failed' }
& git -C $source apply --check --no-index --include=libavformat/matroskadec.c $patch
if ($LASTEXITCODE) { throw 'Backport check failed' }
& git -C $source apply --no-index --include=libavformat/matroskadec.c $patch
if ($LASTEXITCODE) { throw 'Backport apply failed' }
function To-Msys([string]$value) { return (($value.Replace('\','/') -replace '^([A-Za-z]):','/$1').ToLowerInvariant()) }
$sourceMsys = To-Msys $source; $buildMsys = To-Msys $build; $installMsys = To-Msys $install
$lines = Get-Content -LiteralPath $recipe | Where-Object { $_ -match '^set "OPTS=' }
$flags = foreach ($line in $lines) {
    $value = $line -replace '^set "OPTS=','' -replace '"$','' -replace '^%OPTS% ',' '
    $value
}
$options = ($flags -join ' ').Replace('%PREFIX_MSYS%', $installMsys).Replace('--enable-encoder=pcm_s16le', '--enable-encoder=pcm_s16le --enable-encoder=pcm_f32le')
if (($options | Select-String -Pattern '--enable-encoder=pcm_f32le' -AllMatches).Matches.Count -ne 1) { throw 'Unexpected encoder recipe' }
$configure = "cd '$buildMsys' && '$sourceMsys/configure' $options && make -j$env:NUMBER_OF_PROCESSORS && make install"
$driver = Join-Path $output 'build-candidate.bat'
@(
    '@echo off',
    "call `"$vcvars`" >nul || exit /b 1",
    'set "PATH=%PATH%;C:\msys64\usr\bin;C:\msys64\mingw64\bin"',
    'set "MSYS2_PATH_TYPE=inherit"',
    "`"$bash`" --noprofile --norc -c `"$configure`""
) | Set-Content -LiteralPath $driver -Encoding Ascii
& cmd /c $driver
if ($LASTEXITCODE) { throw "FFmpeg configure/build failed: $LASTEXITCODE" }
New-Item -ItemType Directory -Path (Join-Path $install 'lib') -Force | Out-Null
foreach ($name in @('avformat','avcodec','avutil','swresample')) {
    $from = Join-Path $install "bin\$name.lib"
    if (-not (Test-Path -LiteralPath $from)) { throw "Missing import library: $from" }
    Copy-Item -LiteralPath $from -Destination (Join-Path $install "lib\$name.lib")
}
$manifest = [ordered]@{
    profile = 'wav-f32-native-v1'; status = 'candidate-not-deployed'; sourceArchive = $archive
    sourceSha256 = (Get-FileHash $archive -Algorithm SHA256).Hash
    patch = $patch; patchSha256 = (Get-FileHash $patch -Algorithm SHA256).Hash
    backportCommit = $baseline.backportCommit; patchedFiles = @('libavformat/matroskadec.c')
    sourceDirectory = $source; buildDirectory = $build; installDirectory = $install
    configureOptions = $options; compiler = (& cmd /c "`"$vcvars`" >nul && cl" 2>&1 | Select-Object -First 2 | Out-String).Trim()
    runtimeDlls = @(); importLibraries = @()
}
foreach ($name in @('avcodec-61.dll','avformat-61.dll','avutil-59.dll','swresample-5.dll')) {
    $item = Join-Path $install "bin\$name"
    if (-not (Test-Path -LiteralPath $item)) { throw "Missing DLL: $item" }
    $manifest.runtimeDlls += [ordered]@{ name=$name; bytes=(Get-Item $item).Length; sha256=(Get-FileHash $item -Algorithm SHA256).Hash }
}
foreach ($name in @('avcodec.lib','avformat.lib','avutil.lib','swresample.lib')) {
    $item = Join-Path $install "lib\$name"
    $manifest.importLibraries += [ordered]@{ name=$name; sha256=(Get-FileHash $item -Algorithm SHA256).Hash }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'candidate-manifest.json') -Encoding UTF8
Write-Host "EXPORT_RUNTIME_CANDIDATE_OK $install"
