<#
    run_e2e.ps1 -- end-to-end layer test over every swapchain layout and both D3D APIs, through the real OpenXR loader,
    the INSTALLED layer (run scripts\install.ps1 first) and tests/mock_runtime (no headset). Verifies every MP4.
      powershell -ExecutionPolicy Bypass -File tests\run_e2e.ps1
    The recording cases run with XR_EYE_CAPTURE_REQUIRE_TRAY=0 (no tray app needed). The last case runs WITHOUT that
    override and without the tray and expects the layer to stay dormant (pass-through, nothing recorded).
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$exe = Join-Path $bin 'layer_e2e.exe'
if (-not (Test-Path $exe)) { throw "build first: $exe" }
$results = @()
$prevRequireTray = $env:XR_EYE_CAPTURE_REQUIRE_TRAY
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
foreach ($api in @('d3d11', 'd3d12')) {
    foreach ($layout in @('two', 'array', 'wide', 'msaa')) {
        $out = Join-Path $root "build\e2e_out\${api}_$layout"
        if (Test-Path $out) { Remove-Item -Recurse -Force $out }
        Write-Host "`n=== $api / $layout" -ForegroundColor Cyan
        $o = & $exe --api $api --layout $layout --seconds 6 --record 4 --out-dir $out 2>&1 | Out-String
        $o.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
        $m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
        $runOk = ($LASTEXITCODE -eq 0) -and $m.Success
        $verOk = $false
        if ($runOk) {
            # autorecord counts from the first frame; encoder start-up takes ~0.2-0.4 s of it
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $m.Groups[1].Value.Trim() -EyeW 1920 -EyeH 1080 -Seconds 3.7 -DurationTolerance 0.3 -ExpectAudio
            $verOk = ($LASTEXITCODE -eq 0)
        }
        $results += [pscustomobject]@{ api = $api; layout = $layout; run = $runOk; verify = $verOk }
    }
}

# ---- dormant case: no override, no tray -> the layer must do nothing but forward ----
Remove-Item Env:\XR_EYE_CAPTURE_REQUIRE_TRAY -ErrorAction SilentlyContinue
$trayUp = $true
try { $mx = [System.Threading.Mutex]::OpenExisting('Local\OpenXR-EyeCapture-TrayAlive'); $mx.Dispose() } catch { $trayUp = $false }
Write-Host "`n=== dormant (no tray, no override)" -ForegroundColor Cyan
if ($trayUp) {
    Write-Host '  EyeCaptureTray is running -- close it for the dormant case' -ForegroundColor Red
    $results += [pscustomobject]@{ api = 'd3d11'; layout = 'dormant'; run = $false; verify = $false }
} else {
    $logFile = Join-Path $root 'logs\eye_capture.log'
    $before = if (Test-Path $logFile) { (Get-Item $logFile).Length } else { 0 }
    $out = Join-Path $root 'build\e2e_out\dormant'
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    $o = & $exe --api d3d11 --layout two --seconds 3 --record 2 --out-dir $out --expect-dormant 2>&1 | Out-String
    $runOk = ($LASTEXITCODE -eq 0) -and ($o -match 'E2E_DORMANT_OK')
    $o.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
    Start-Sleep -Milliseconds 300
    $fs = [System.IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite')
    $null = $fs.Seek($before, 'Begin')
    $newLog = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd()
    $fs.Dispose()
    $dormantLine = [bool]($newLog -match 'EyeCaptureTray not running .{1,3} layer dormant for this session')
    $clean = ($newLog -notmatch 'control: ') -and ($newLog -notmatch 'recording summary') -and ($newLog -notmatch 'capture AVAILABLE') -and
             -not (Get-ChildItem $out -Filter *.mp4 -ErrorAction SilentlyContinue)
    Write-Host "  log has 'layer dormant' line: $dormantLine; no control thread / recorder / capture in the log, no mp4: $clean"
    $results += [pscustomobject]@{ api = 'd3d11'; layout = 'dormant'; run = $runOk; verify = ($dormantLine -and $clean) }
}
if ($null -ne $prevRequireTray) { $env:XR_EYE_CAPTURE_REQUIRE_TRAY = $prevRequireTray }

Write-Host ''
$results | Format-Table -AutoSize
Write-Host 'Layer log lines for these runs:'
Get-Content (Join-Path $root 'logs\eye_capture.log') -Tail 400 | Select-String 'frame layout \(|recording summary' | Select-Object -Last 16 | ForEach-Object { '  ' + $_.Line.Substring([math]::Min(37, $_.Line.Length)) }
if ($results | Where-Object { -not ($_.run -and $_.verify) }) { exit 1 }
exit 0
