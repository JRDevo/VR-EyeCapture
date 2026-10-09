<#
    make_flat.ps1 -- make a normal 2D video from a side-by-side recording for viewers without a headset:
    crops the LEFT eye (left half), re-encodes with NVENC HEVC (falls back to x264 if NVENC is missing), copies the audio.

      powershell -ExecutionPolicy Bypass -File scripts\make_flat.ps1 <file.mp4> [more.mp4 ...] [-Width 3840] [-Mbps 40] [-Aspect 16:9|eye]
    Writes <file>_FLAT.mp4 next to each input. -Width scales the result (0 = keep the eye's own width). -Aspect 16:9 (default) centre-crops the tall eye
    image to 16:9 for normal screens; -Aspect eye keeps the full eye image. Needs ffmpeg (winget install Gyan.FFmpeg.Essentials, or pass -Ffmpeg).
#>
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)][string[]]$Files,
    [string]$Ffmpeg = '',
    [int]$Width = 3840,
    [int]$Mbps = 40,
    [string]$Aspect = '16:9'
)
$ErrorActionPreference = 'Stop'
if (-not $Ffmpeg) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { $Ffmpeg = $cmd.Source } else {
        $c = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffmpeg.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($c) { $Ffmpeg = $c.FullName } else { Write-Host 'ffmpeg not found: winget install Gyan.FFmpeg.Essentials (or pass -Ffmpeg <path>)' -ForegroundColor Red; exit 1 }
    }
}
$rc = 0
$vf = 'crop=iw/2:ih:0:0'
if ($Aspect -eq '16:9') { $vf += ',crop=iw:min(ih\,iw*9/16)' }
if ($Width -gt 0) { $vf += ",scale=${Width}:-2" }
$hasNvenc = (& $Ffmpeg -hide_banner -encoders 2>$null | Select-String 'hevc_nvenc') -ne $null
foreach ($f in $Files) {
    if (-not (Test-Path $f)) { Write-Host "missing: $f" -ForegroundColor Red; $rc = 1; continue }
    $p = (Resolve-Path $f).Path
    $out = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($p), [System.IO.Path]::GetFileNameWithoutExtension($p) + '_FLAT.mp4')
    Write-Host "$f -> $out"
    if ($hasNvenc) { $venc = @('-c:v', 'hevc_nvenc', '-preset', 'p5', '-rc', 'vbr', '-b:v', "${Mbps}M", '-maxrate', "$($Mbps * 2)M", '-tag:v', 'hvc1') }
    else           { $venc = @('-c:v', 'libx264', '-preset', 'medium', '-crf', '18') }
    & $Ffmpeg -hide_banner -loglevel error -y -i $p -map 0:v:0 -map '0:a?' -vf $vf @venc -c:a copy -movflags +faststart $out
    if ($LASTEXITCODE -ne 0) { Write-Host '  ffmpeg failed' -ForegroundColor Red; $rc = 1 } else { Write-Host '  ok' -ForegroundColor Green }
}
exit $rc
