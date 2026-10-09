<#
    make_mkv.ps1 -- fallback if YouTube does not pick up the 3D flag of an MP4: remux (no re-encode) into MKV with the
    Matroska stereo_mode = left_right tag, which YouTube also reads.

      powershell -ExecutionPolicy Bypass -File scripts\make_mkv.ps1 <file.mp4> [more.mp4 ...]
    Writes <file>.mkv next to each input. Needs ffmpeg (winget install Gyan.FFmpeg.Essentials, or pass -Ffmpeg).
#>
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)][string[]]$Files,
    [string]$Ffmpeg = ''
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
foreach ($f in $Files) {
    if (-not (Test-Path $f)) { Write-Host "missing: $f" -ForegroundColor Red; $rc = 1; continue }
    $out = [System.IO.Path]::ChangeExtension((Resolve-Path $f).Path, '.mkv')
    Write-Host "$f -> $out"
    & $Ffmpeg -hide_banner -loglevel error -y -i $f -map 0 -c copy -metadata:s:v:0 stereo_mode=left_right $out
    if ($LASTEXITCODE -ne 0) { Write-Host '  ffmpeg failed' -ForegroundColor Red; $rc = 1 } else { Write-Host '  ok' -ForegroundColor Green }
}
exit $rc

