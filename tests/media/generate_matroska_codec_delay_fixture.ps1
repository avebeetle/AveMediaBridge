param(
    [Parameter(Mandatory = $true)]
    [string] $FfmpegExe,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$sampleRate = 48000
$channels = 2
# Keep source plus the 1024-frame priming delay block-aligned so this
# fixture proves leading CodecDelay independently of terminal padding.
$frames = 144384
$dataBytes = $frames * $channels * 4

$ffmpeg = (Resolve-Path -LiteralPath $FfmpegExe).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$master = Join-Path $outputRoot 'matroska_codec_delay_master_f32.wav'
$fixture = Join-Path $outputRoot 'matroska_codec_delay_fixture.mka'

$stream = [System.IO.File]::Create($master)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([uint32](36 + $dataBytes))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
    $writer.Write([uint32]16)
    $writer.Write([uint16]3)
    $writer.Write([uint16]$channels)
    $writer.Write([uint32]$sampleRate)
    $writer.Write([uint32]($sampleRate * $channels * 4))
    $writer.Write([uint16]($channels * 4))
    $writer.Write([uint16]32)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([uint32]$dataBytes)

    for ($frame = 0; $frame -lt $frames; ++$frame) {
        $t = [double]$frame / $sampleRate
        $left = 0.27 * [Math]::Sin(2.0 * [Math]::PI * (311.0 * $t + 23.0 * $t * $t)) +
            0.11 * [Math]::Sin(2.0 * [Math]::PI * 1177.0 * $t)
        $right = 0.24 * [Math]::Sin(2.0 * [Math]::PI * (419.0 * $t + 17.0 * $t * $t)) +
            0.09 * [Math]::Sin(2.0 * [Math]::PI * 1531.0 * $t)
        $writer.Write([single]$left)
        $writer.Write([single]$right)
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -i $master -map 0:a:0 -c:a aac -b:a 128k -f matroska $fixture
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg fixture generation failed with exit code $LASTEXITCODE"
}

[pscustomobject]@{
    masterPath = $master
    fixturePath = $fixture
    expectedFrames = $frames
    expectedCodecDelayFrames = 1024
    masterSha256 = (Get-FileHash -LiteralPath $master -Algorithm SHA256).Hash
    fixtureSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
} | ConvertTo-Json
