param(
    [Parameter(Mandatory = $true)] [string] $Generator,
    [Parameter(Mandatory = $true)] [string] $FfmpegExe,
    [Parameter(Mandatory = $true)] [string] $ScratchRoot,
    [ValidateSet('Collision', 'Timeout')] [string] $Case
)

$ErrorActionPreference = 'Stop'
$root = Join-Path $ScratchRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

if ($Case -eq 'Collision') {
    $output = Join-Path $root 'output'
    New-Item -ItemType Directory -Path $output | Out-Null
    $existing = Join-Path $output 'reader_stereo_front_aac.mp4'
    [System.IO.File]::WriteAllText($existing, 'existing fixture must survive')
    try {
        & $Generator -FfmpegExe $FfmpegExe -OutputDirectory $output | Out-Null
        throw 'generator accepted an existing destination'
    } catch {
        if ($_.Exception.Message -notmatch 'already exists|destination exists') { throw }
    }
    if ([System.IO.File]::ReadAllText($existing) -ne 'existing fixture must survive') {
        throw 'generator changed the existing destination'
    }
    if ((Get-ChildItem -LiteralPath $output).Count -ne 1) {
        throw 'generator wrote another output before preflighting all destinations'
    }
    Write-Output "PASS collision refusal: $output"
    return
}

$source = Join-Path $root 'SleepingFfmpeg.cs'
$fake = Join-Path $root 'SleepingFfmpeg.exe'
$marker = Join-Path $root 'child.pid'
[System.IO.File]::WriteAllText($source, @'
using System;
using System.IO;
using System.Threading;
class SleepingFfmpeg {
    static int Main() {
        File.WriteAllText(Environment.GetEnvironmentVariable("AVE_READER_FAKE_CHILD_MARKER"),
            System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
        Thread.Sleep(30000);
        return 0;
    }
}
'@)
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc /nologo /target:exe "/out:$fake" $source
if ($LASTEXITCODE -ne 0) { throw "fake FFmpeg compile failed: $LASTEXITCODE" }
$env:AVE_READER_FAKE_CHILD_MARKER = $marker
$start = [System.Diagnostics.Stopwatch]::StartNew()
try {
    try {
        & $Generator -FfmpegExe $fake -OutputDirectory (Join-Path $root 'output') -ProcessTimeoutSeconds 2 | Out-Null
        throw 'generator accepted a timed-out child'
    } catch {
        if ($_.Exception.Message -notmatch 'timed out') { throw }
    }
} finally {
    Remove-Item Env:AVE_READER_FAKE_CHILD_MARKER -ErrorAction SilentlyContinue
}
$start.Stop()
if ($start.Elapsed.TotalSeconds -gt 10) { throw "child timeout was not bounded: $($start.Elapsed)" }
if (-not (Test-Path -LiteralPath $marker)) { throw 'fake child never started' }
$pidOfChild = [int][System.IO.File]::ReadAllText($marker)
if (Get-Process -Id $pidOfChild -ErrorAction SilentlyContinue) {
    throw "timed-out child remains alive: $pidOfChild"
}
Write-Output "PASS bounded timeout: $($start.Elapsed.TotalSeconds)s, child $pidOfChild stopped, $root"
