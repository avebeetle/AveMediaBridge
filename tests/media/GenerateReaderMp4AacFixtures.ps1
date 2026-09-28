param(
    [Parameter(Mandatory = $true)] [string] $FfmpegExe,
    [Parameter(Mandatory = $true)] [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$ffmpeg = (Resolve-Path -LiteralPath $FfmpegExe).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path

# Deterministic, small MOV-family media for the private demux ownership test.
# The generator never changes reference media from the laboratory catalog.
$fixture = Join-Path $outputRoot 'reader_demux_aac.m4a'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=12' `
    -map '0:a:0' -c:a aac -b:a 192k -movflags +faststart -f ipod $fixture
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg fixture generation failed with exit code $LASTEXITCODE"
}

[pscustomobject]@{
    fixturePath = $fixture
    fixtureSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    generatorPath = $PSCommandPath
    generatorSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    ffmpegPath = $ffmpeg
    ffmpegSha256 = (Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash
} | ConvertTo-Json
