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
if (-not (Test-Path -LiteralPath $fixture)) {
    & $ffmpeg -hide_banner -nostats -loglevel error -y `
        -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=12' `
        -map '0:a:0' -c:a aac -b:a 192k -movflags +faststart -f ipod $fixture
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg demux fixture generation failed with exit code $LASTEXITCODE"
    }
}

$front = Join-Path $outputRoot 'reader_stereo_front_aac.mp4'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=2' `
    -map '0:a:0' -ac 2 -c:a aac -b:a 128k -movflags +faststart -f mp4 $front
if ($LASTEXITCODE -ne 0) { throw "front-moov fixture generation failed: $LASTEXITCODE" }

$tail = Join-Path $outputRoot 'reader_stereo_tail_aac.mp4'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=2' `
    -map '0:a:0' -ac 2 -c:a aac -b:a 128k -f mp4 $tail
if ($LASTEXITCODE -ne 0) { throw "tail-moov fixture generation failed: $LASTEXITCODE" }

$twoAac = Join-Path $outputRoot 'reader_two_aac_default_second.mp4'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=2' `
    -f lavfi -i 'sine=frequency=977:sample_rate=44100:duration=2' `
    -map '0:a:0' -map '1:a:0' -c:a aac -b:a 96k `
    -disposition:a:0 0 -disposition:a:1 default -movflags +faststart -f mp4 $twoAac
if ($LASTEXITCODE -ne 0) { throw "two-AAC fixture generation failed: $LASTEXITCODE" }

$mp3 = Join-Path $outputRoot 'reader_mp4_mp3_control.mp4'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=2' `
    -map '0:a:0' -c:a libmp3lame -b:a 128k -f mp4 $mp3
if ($LASTEXITCODE -ne 0) { throw "MP4/MP3 fixture generation failed: $LASTEXITCODE" }

$alac = Join-Path $outputRoot 'reader_mp4_alac_control.m4a'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'sine=frequency=719:sample_rate=48000:duration=2' `
    -map '0:a:0' -c:a alac -f ipod $alac
if ($LASTEXITCODE -ne 0) { throw "M4A/ALAC fixture generation failed: $LASTEXITCODE" }

$noAudio = Join-Path $outputRoot 'reader_mp4_no_audio.mp4'
& $ffmpeg -hide_banner -nostats -loglevel error -y `
    -f lavfi -i 'color=c=black:s=16x16:r=1:d=2' `
    -an -c:v mpeg4 -f mp4 $noAudio
if ($LASTEXITCODE -ne 0) { throw "no-audio fixture generation failed: $LASTEXITCODE" }

[pscustomobject]@{
    fixturePath = $fixture
    fixtureSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    generatedFixtures = @($front, $tail, $twoAac, $mp3, $alac, $noAudio)
    generatorPath = $PSCommandPath
    generatorSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    ffmpegPath = $ffmpeg
    ffmpegSha256 = (Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash
} | ConvertTo-Json
