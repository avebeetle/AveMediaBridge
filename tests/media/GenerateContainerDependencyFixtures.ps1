param(
    [Parameter(Mandatory=$true)][string]$FfmpegExe,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,180)][int]$ProcessTimeoutSeconds=90
)
$ErrorActionPreference='Stop'
$encoder=(Resolve-Path -LiteralPath $FfmpegExe).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Fixture output must be fresh' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$root=(Resolve-Path -LiteralPath $OutputDirectory).Path
$audio=@('-f','lavfi','-i','sine=frequency=719:sample_rate=48000:duration=1')
$video=@('-f','lavfi','-i','color=c=red:s=32x32:r=8:d=1')
$codecs=@('-c:a','aac','-b:a','96k','-c:v','libx264','-pix_fmt','yuv420p','-threads','1')
# -use_editlist 0 keeps generated timing simple. AAC roll sgpd/sbgp tables
# remain present and are deliberately tested under the restricted roll-only
# dependency policy. Existing reader fixture recipes remain unchanged.
$recipes=@(
    @{Name='front.mp4';Reason='AAC/H264 front moov';Args=$audio+$video+@('-map','0:a','-map','1:v')+$codecs+@('-movflags','+faststart')},
    @{Name='tail.mp4';Reason='AAC/H264 tail moov';Args=$audio+$video+@('-map','0:a','-map','1:v')+$codecs},
    @{Name='multiple.mp4';Reason='two local AAC tracks and H264';Args=$audio+$video+@('-map','0:a','-map','0:a','-map','1:v')+$codecs},
    @{Name='no-audio.mp4';Reason='H264 only: independence is separate from audio import eligibility';Args=$video+@('-an','-c:v','libx264','-pix_fmt','yuv420p','-threads','1')},
    @{Name='cover.mp4';Reason='local AAC with iTunes cover bytes, no timed video';Args=$audio+$video+@('-map','0:a','-map','1:v','-c:a','aac','-c:v','mjpeg','-frames:v','1','-disposition:v','attached_pic','-threads','1')}
)
$manifest=@()
foreach($recipe in $recipes) {
    $dest=Join-Path $root $recipe.Name
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$encoder;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $argsList=@('-hide_banner','-nostats','-loglevel','error','-n')+$recipe.Args+@('-use_editlist','0','-f','mp4',$dest)
    $info.Arguments=($argsList | ForEach-Object { '"'+$_+'"' }) -join ' '
    $child=[Diagnostics.Process]::new();$child.StartInfo=$info
    try {
        if(-not $child.Start()){throw 'Could not start generator'}
        $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
        if(-not $child.WaitForExit($ProcessTimeoutSeconds*1000)) { $child.Kill();[void]$child.WaitForExit(5000);throw 'Fixture generation timeout' }
        if(-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)){throw 'Fixture log timeout'}
        if($child.ExitCode -ne 0){throw $stderr.Result}
    } finally { if(-not $child.HasExited){$child.Kill();[void]$child.WaitForExit(5000)};$child.Dispose() }
    $manifest += [pscustomobject]@{name=$recipe.Name;reason=$recipe.Reason;arguments=$argsList;sha256=(Get-FileHash -LiteralPath $dest).Hash}
}
[pscustomobject]@{generator=$PSCommandPath;generatorSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
    ffmpeg=$encoder;ffmpegSha256=(Get-FileHash -LiteralPath $encoder).Hash;fixtures=$manifest} | ConvertTo-Json -Depth 8
