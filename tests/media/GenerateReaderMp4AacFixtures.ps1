param(
    [Parameter(Mandatory = $true)] [string] $FfmpegExe,
    [Parameter(Mandatory = $true)] [string] $OutputDirectory,
    [ValidateRange(1, 3600)] [int] $ProcessTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
$ffmpeg = (Resolve-Path -LiteralPath $FfmpegExe).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path

# Keep the accepted recipes intact. Every destination must be fresh, including
# the private demux fixture, before any child is started.
$recipes = @(
    @{ Name = 'reader_demux_aac.m4a'; Label = 'demux'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=12',
        '-map', '0:a:0', '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', '-f', 'ipod') },
    @{ Name = 'reader_stereo_front_aac.mp4'; Label = 'front-moov'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-map', '0:a:0', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-movflags', '+faststart', '-f', 'mp4') },
    @{ Name = 'reader_stereo_tail_aac.mp4'; Label = 'tail-moov'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-map', '0:a:0', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'mp4') },
    @{ Name = 'reader_two_aac_default_second.mp4'; Label = 'two-AAC'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-f', 'lavfi', '-i', 'sine=frequency=977:sample_rate=44100:duration=2',
        '-map', '0:a:0', '-map', '1:a:0', '-c:a', 'aac', '-b:a', '96k',
        '-disposition:a:0', '0', '-disposition:a:1', 'default', '-movflags', '+faststart', '-f', 'mp4') },
    @{ Name = 'reader_mp4_mp3_control.mp4'; Label = 'MP4/MP3'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-map', '0:a:0', '-c:a', 'libmp3lame', '-b:a', '128k', '-f', 'mp4') },
    @{ Name = 'reader_mp4_alac_control.m4a'; Label = 'M4A/ALAC'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-map', '0:a:0', '-c:a', 'alac', '-f', 'ipod') },
    @{ Name = 'reader_mp4_no_audio.mp4'; Label = 'no-audio'; Args = @(
        '-f', 'lavfi', '-i', 'color=c=black:s=16x16:r=1:d=2',
        '-an', '-c:v', 'mpeg4', '-f', 'mp4') },
    @{ Name = 'reader_cover_only_aac.mp4'; Label = 'cover-only'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-f', 'lavfi', '-i', 'color=c=red:s=16x16:r=1:d=1',
        '-map', '0:a:0', '-map', '1:v:0', '-c:a', 'aac', '-b:a', '128k',
        '-c:v', 'mjpeg', '-frames:v', '1', '-disposition:v:0', 'attached_pic', '-f', 'mp4') },
    @{ Name = 'reader_single_video_aac.mp4'; Label = 'single-video'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-f', 'lavfi', '-i', 'color=c=red:s=16x16:r=1:d=2',
        '-map', '0:a:0', '-map', '1:v:0', '-c:a', 'aac', '-b:a', '128k',
        '-c:v', 'mpeg4', '-f', 'mp4') },
    @{ Name = 'reader_two_video_aac.mp4'; Label = 'two-video'; Args = @(
        '-f', 'lavfi', '-i', 'sine=frequency=719:sample_rate=48000:duration=2',
        '-f', 'lavfi', '-i', 'color=c=red:s=16x16:r=1:d=2',
        '-f', 'lavfi', '-i', 'color=c=blue:s=16x16:r=1:d=2',
        '-map', '0:a:0', '-map', '1:v:0', '-map', '2:v:0', '-c:a', 'aac', '-b:a', '128k',
        '-c:v', 'mpeg4', '-f', 'mp4') }
)
$destinations = @($recipes | ForEach-Object { Join-Path $outputRoot $_.Name })
foreach ($destination in $destinations) {
    if (Test-Path -LiteralPath $destination) {
        throw "Reader fixture destination already exists: $destination"
    }
}

function Invoke-FixtureFfmpeg([string] $label, [string[]] $arguments, [string] $destination) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $ffmpeg
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # These fixed arguments and Windows paths contain no quotes. Quoting every
    # value also keeps destinations with spaces intact on Windows PowerShell 5.
    $start.Arguments = ((@('-hide_banner', '-nostats', '-loglevel', 'error', '-n') +
        $arguments + @($destination)) | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    $started = $false
    try {
        if (-not $process.Start()) { throw "FFmpeg $label fixture could not start" }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($ProcessTimeoutSeconds * 1000)) {
            if (-not $process.HasExited) { $process.Kill() }
            if (-not $process.WaitForExit(5000)) {
                throw "FFmpeg $label fixture timed out after $ProcessTimeoutSeconds seconds; owned child could not be stopped; output: $destination"
            }
            if (-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) {
                throw "FFmpeg $label fixture timed out after $ProcessTimeoutSeconds seconds; owned child stopped, output drain failed; output: $destination"
            }
            throw "FFmpeg $label fixture timed out after $ProcessTimeoutSeconds seconds; owned child stopped; output: $destination; stderr: $($stderr.Result); stdout: $($stdout.Result)"
        }
        if (-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) {
            throw "FFmpeg $label fixture output drain timed out; output: $destination"
        }
        if ($process.ExitCode -ne 0) {
            throw "FFmpeg $label fixture generation failed with exit code $($process.ExitCode); output: $destination; stderr: $($stderr.Result); stdout: $($stdout.Result)"
        }
    } finally {
        if ($started -and -not $process.HasExited) {
            $process.Kill()
            [void] $process.WaitForExit(5000)
        }
        $process.Dispose()
    }
}

foreach ($recipe in $recipes) {
    Invoke-FixtureFfmpeg $recipe.Label $recipe.Args (Join-Path $outputRoot $recipe.Name)
}

$fixture = $destinations[0]
[pscustomobject]@{
    fixturePath = $fixture
    fixtureSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    generatedFixtures = $destinations[1..($destinations.Count - 1)]
    generatorPath = $PSCommandPath
    generatorSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    ffmpegPath = $ffmpeg
    ffmpegSha256 = (Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash
} | ConvertTo-Json
