<#
    run_reload.ps1 -- the layer re-reads config.ini at the start of every recording (no headset, mock runtime, needs the INSTALLED layer).
      powershell -ExecutionPolicy Bypass -File tests\run_reload.ps1
    A scratch %APPDATA%\OpenXR-EyeCapture\config.ini (projection = flat, bitrate_mbps = 20) is used. layer_e2e records 3 s;
    XR_EYE_CAPTURE_AUTORECORD2 (test-only) starts a second 3 s recording 1 s after the first one stopped, in the SAME
    process. The script rewrites the scratch config (projection = vr180, bitrate_mbps = 60) as soon as the first MP4
    appears. Expected: the first file is *_SBS.mp4, the second *_VR180.mp4; the recorder's init lines say HEVC 20 Mb/s then
    HEVC 60 Mb/s (the synthetic test pattern compresses far below either rate, so the MP4 bit rates are only printed);
    the log has `config reloaded ... no changes` and then `... changed bitrate_mbps=20->60, projection=flat->vr180`.
    Skips (exit 0 with a message) when amtrucks / eurotrucks2 / obs64 is running.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\Release\layer_e2e.exe'
if (-not (Test-Path $exe)) { throw "build first: $exe" }
if (Get-Process amtrucks, eurotrucks2, obs64 -ErrorAction SilentlyContinue) { Write-Host 'SKIP: a GPU-heavy app is running'; exit 0 }
$b = Join-Path $root 'build\tt_b'
if (Test-Path $b) { Remove-Item -Recurse -Force $b }
New-Item -ItemType Directory (Join-Path $b 'appdata\OpenXR-EyeCapture'), (Join-Path $b 'out') | Out-Null
$ini = Join-Path $b 'appdata\OpenXR-EyeCapture\config.ini'
$cfg = (Get-Content (Join-Path $root 'config.ini') -Raw) -replace '(?m)^projection = \w+', 'projection = flat' -replace '(?m)^bitrate_mbps = \d+', 'bitrate_mbps = 20' -replace '(?m)^head_smoothing = [\d.]+', 'head_smoothing = 0'
Set-Content $ini $cfg -NoNewline -Encoding utf8
$old = @{}
foreach ($n in 'APPDATA', 'XR_EYE_CAPTURE_REQUIRE_TRAY', 'XR_EYE_CAPTURE_TRAY_IPC', 'XR_EYE_CAPTURE_AUTORECORD2', 'XR_EYE_CAPTURE_CONFIG') { $old[$n] = [Environment]::GetEnvironmentVariable($n) }
try {
    $env:APPDATA = Join-Path $b 'appdata'; $env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'; $env:XR_EYE_CAPTURE_TRAY_IPC = '0'; $env:XR_EYE_CAPTURE_AUTORECORD2 = '3'
    Remove-Item Env:\XR_EYE_CAPTURE_CONFIG -ErrorAction SilentlyContinue
    $out = Join-Path $b 'out'
    $p = Start-Process $exe -ArgumentList '--api', 'd3d11', '--layout', 'two', '--seconds', '12', '--record', '3', '--out-dir', $out -PassThru -RedirectStandardOutput (Join-Path $b 'e2e.txt') -NoNewWindow
    $edited = $false
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 200
        if (-not $edited -and (Get-ChildItem $out -Filter *.mp4 -ErrorAction SilentlyContinue)) {
            $c = (Get-Content $ini -Raw) -replace '(?m)^projection = flat', 'projection = vr180' -replace '(?m)^bitrate_mbps = 20', 'bitrate_mbps = 60'
            Set-Content $ini $c -NoNewline -Encoding utf8
            $edited = $true
        }
    }
    $p.WaitForExit()
} finally {
    foreach ($n in $old.Keys) { [Environment]::SetEnvironmentVariable($n, $old[$n]) }
}
$ffp = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffprobe.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $ffp) { $ffp = (Get-Command ffprobe).Source }
$files = @(Get-ChildItem $out -Filter *.mp4 | Sort-Object Name)
$log = Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8
$rel = [regex]::Matches($log, 'config reloaded for this recording from [^\r\n]*') | ForEach-Object { $_.Value }
Write-Host 'reload lines:'; $rel | ForEach-Object { Write-Host "  $_" }
$br = $files | ForEach-Object { [double](& $ffp -v error -show_entries format=bit_rate -of default=nw=1:nk=1 $_.FullName) / 1e6 }
for ($i = 0; $i -lt $files.Count; $i++) { Write-Host ("  {0}: {1:N1} Mb/s" -f $files[$i].Name, $br[$i]) }
$rates = ([regex]::Matches($log, 'recorder: init [^\r\n]*HEVC (\d+) Mb/s') | ForEach-Object { $_.Groups[1].Value }) -join ','
$ok = ($files.Count -eq 2) -and ($files[0].Name -match '_SBS\.mp4$') -and ($files[1].Name -match '_VR180\.mp4$') -and
      ($rel.Count -eq 2) -and ($rel[0] -match 'no changes$') -and ($rel[1] -match 'changed bitrate_mbps=20->60, projection=flat->vr180$') -and
      ($rates -eq '20,60')
Write-Host ("config reload at recording start: {0}" -f $(if ($ok) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
if ($ok) { exit 0 } else { exit 1 }
