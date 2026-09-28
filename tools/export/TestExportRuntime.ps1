param(
    [Parameter(Mandatory)][string]$CandidateRoot,
    [Parameter(Mandatory)][string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$candidate = (Resolve-Path -LiteralPath $CandidateRoot).Path
$output = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $output) { throw "Refusing existing evidence directory: $output" }
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$profile = Get-Content -Raw (Join-Path $candidate 'candidate-manifest.json') | ConvertFrom-Json
$profileSpec = Get-Content -Raw (Join-Path $repo 'tools\export\export-runtime-profile.json') | ConvertFrom-Json
$baseline = Get-Content -Raw (Join-Path $repo 'third_party\ffmpeg\build-manifest\ffmpeg-7.1.4-matroska-codec-delay-backport.json') | ConvertFrom-Json
$patch = Join-Path $repo 'third_party\ffmpeg\patches\0880458e4c-matroska-codec-delay-skip.patch'
if ($profile.backportCommit -ne $baseline.backportCommit -or
    $profile.sourceSha256 -ne $baseline.baseSourceSha256 -or
    $profile.patchSha256 -ne $baseline.patchSha256 -or
    (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash -ne $baseline.patchSha256) { throw 'Source or backport provenance mismatch' }
function Flags([string]$command) {
    return @([regex]::Matches($command, '--\S+') | ForEach-Object { $_.Value } | Where-Object { $_ -notlike '--prefix=*' } | Sort-Object)
}
$baseFlags = Flags $baseline.build.configureCommand
$candidateFlags = Flags $profile.configureOptions
$added = @($candidateFlags | Where-Object { $_ -notin $baseFlags })
$removed = @($baseFlags | Where-Object { $_ -notin $candidateFlags })
if ($added.Count -ne 1 -or $added[0] -ne '--enable-encoder=pcm_f32le' -or $removed.Count) { throw "Configure delta unexpected: added=$added removed=$removed" }
$config = Join-Path $profile.buildDirectory 'ffbuild\config.mak'
if (-not (Select-String -LiteralPath $config -Pattern '^CONFIG_PCM_F32LE_ENCODER=yes$' -Quiet)) { throw 'Candidate encoder disabled' }
& git -C $profile.sourceDirectory apply --reverse --check --no-index --include=libavformat/matroskadec.c $patch
if ($LASTEXITCODE) { throw 'Backport not present in candidate source' }
New-Item -ItemType Directory -Path $output | Out-Null
$decoder = 'C:\ProgramData\chocolatey\lib\ffmpeg\tools\ffmpeg\bin\ffmpeg.exe'
$decoderHash = (Get-FileHash -LiteralPath $decoder -Algorithm SHA256).Hash
if ($decoderHash -ne 'B90225987BDD042CCA09A1EFB5E34E9848F2D1DBF5FBCD388753A44145522997') { throw 'Reference decoder hash mismatch' }
$referenceRoot = Split-Path $decoder -Parent
$probe = Join-Path $referenceRoot 'ffprobe.exe'
$probeHash = (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash
if ($probeHash -ne '05E8FA639450F8191635192871AE37A3EC3E4638FA12F3B7D49C6522BA16A8ED') { throw 'Reference probe hash mismatch' }
$version = & $decoder -version
if ($LASTEXITCODE) { throw 'Reference decoder version query failed' }
$version | Set-Content -LiteralPath (Join-Path $output 'reference-decoder-version.txt')
$runtime = foreach ($name in @('avcodec-61.dll','avformat-61.dll','avutil-59.dll','swresample-5.dll')) {
    $path = Join-Path $profile.installDirectory "bin\$name"
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $entry = @($profile.runtimeDlls | Where-Object name -eq $name)
    if ($entry.Count -ne 1 -or $hash -ne $entry[0].sha256 -or
        $hash -ne $profileSpec.candidateDllSha256.PSObject.Properties[$name].Value) { throw "Runtime DLL manifest mismatch: $name" }
    $info = (Get-Item $path).VersionInfo
    $expectedVersion = @{'avcodec-61.dll'='61.19.101';'avformat-61.dll'='61.7.102';'avutil-59.dll'='59.39.100';'swresample-5.dll'='5.3.100'}[$name]
    if ($info.FileVersion -ne $expectedVersion -or $info.ProductVersion -ne '7.1.4') { throw "Runtime version mismatch: $name" }
    [ordered]@{ name=$name; sha256=$hash; fileVersion=$info.FileVersion; productVersion=$info.ProductVersion }
}
foreach ($name in @('avcodec.lib','avformat.lib','avutil.lib','swresample.lib')) {
    $path = Join-Path $profile.installDirectory "lib\$name"
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $entry = @($profile.importLibraries | Where-Object name -eq $name)
    if ($entry.Count -ne 1 -or $hash -ne $entry[0].sha256 -or
        $hash -ne $profileSpec.candidateImportLibrarySha256.PSObject.Properties[$name].Value) { throw "Import library manifest mismatch: $name" }
}
$build = Join-Path $output 'bridge-build'
& cmake -S $repo -B $build "-DAVEMEDIABRIDGE_FFMPEG_ROOT=$($profile.installDirectory)" | Tee-Object -FilePath (Join-Path $output 'configure.log')
if ($LASTEXITCODE) { throw 'Bridge configure failed' }
& cmake --build $build --config Release --target AveMediaBridgeFloatWavTests AveMediaBridgeExportScratchTests AveMediaBridgeExportAbiTests AveMediaBridgeExportJobStateTests | Tee-Object -FilePath (Join-Path $output 'build.log')
if ($LASTEXITCODE) { throw 'Bridge export build failed' }
$env:AMBE_EXPECT_FLOAT_WAV = '1'
$env:AMBE_EXPORT_EVIDENCE_ROOT = $output
try {
    & ctest --test-dir $build -C Release --output-on-failure -R '^AveMediaBridgeTests.export_(float_wav|scratch|abi|state)$' | Tee-Object -FilePath (Join-Path $output 'ctest.log')
    if ($LASTEXITCODE) { throw 'Export CTest failed' }
} finally {
    Remove-Item Env:AMBE_EXPECT_FLOAT_WAV,Env:AMBE_EXPORT_EVIDENCE_ROOT -ErrorAction SilentlyContinue
}
function Check-Reference([string]$stem, [int]$channels) {
    $wav = Join-Path $output "$stem.wav"
    $raw = Join-Path $output "$stem.reference.f32le"
    & $decoder -v error -i $wav -map 0:a:0 -c:a pcm_f32le -f f32le $raw
    if ($LASTEXITCODE) { throw "Reference PCM decode failed: $stem" }
    $wantMono = [byte[]](0,0,0,0, 0,0,0,128, 1,0,0,0, 0,0,128,63, 0,0,0,192)
    $expected = New-Object byte[] ($wantMono.Length * $channels)
    for ($sample = 0; $sample -lt 5; ++$sample) {
        for ($channel = 0; $channel -lt $channels; ++$channel) {
            [Array]::Copy($wantMono, $sample * 4, $expected, ($sample * $channels + $channel) * 4, 4)
        }
    }
    $actual = [IO.File]::ReadAllBytes($raw)
    if ($actual.Length -ne $expected.Length) { throw "Reference PCM length mismatch: $stem" }
    for ($i = 0; $i -lt $actual.Length; ++$i) {
        if ($actual[$i] -ne $expected[$i]) { throw "Reference PCM bit mismatch: $stem byte $i" }
    }
    $json = & $probe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels -of json $wav
    if ($LASTEXITCODE) { throw "Reference metadata probe failed: $stem" }
    $metadata = ($json -join "`n") | ConvertFrom-Json
    if ($metadata.streams.Count -ne 1 -or $metadata.streams[0].codec_name -ne 'pcm_f32le' -or
        [int]$metadata.streams[0].sample_rate -ne 48000 -or [int]$metadata.streams[0].channels -ne $channels) {
        throw "Reference rate/layout/codec mismatch: $stem"
    }
    return [ordered]@{ sample=$stem; channels=$channels; frames=5; decodedBytes=$actual.Length; bitExact=$true }
}
$referenceChecks = @(
    (Check-Reference 'finite-mono-48000' 1),
    (Check-Reference 'finite-stereo-48000' 2),
    (Check-Reference 'forced-rf64-mono-48000' 1)
)
$report = [ordered]@{
    status = 'candidate-qualified-not-deployed'; candidateRoot=$candidate; evidenceDirectory=$output
    sourceSha256=$profile.sourceSha256; patchSha256=$profile.patchSha256; backportCommit=$profile.backportCommit
    addedConfigureFlags=$added; removedConfigureFlags=$removed; runtimeDlls=$runtime
    decoderPath=$decoder; decoderSha256=$decoderHash; ffprobePath=$probe; ffprobeSha256=$probeHash
    referenceDecoderVersion=$version[0]; referenceChecks=$referenceChecks
    exportCTest='4/4 passed'; bridgeBuild=$build
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'qualification.json') -Encoding UTF8
Write-Host 'EXPORT_RUNTIME_QUALIFIED_CANDIDATE_ONLY'
