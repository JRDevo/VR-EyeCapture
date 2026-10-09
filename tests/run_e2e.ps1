<#
    run_e2e.ps1 -- end-to-end layer test over every swapchain layout and both D3D APIs, through the real OpenXR loader,
    the INSTALLED layer (run scripts\install.ps1 first) and tests/mock_runtime (no headset). Verifies every MP4.
      powershell -ExecutionPolicy Bypass -File tests\run_e2e.ps1
    The recording cases run with XR_EYE_CAPTURE_REQUIRE_TRAY=0 (no tray app needed). The last case runs WITHOUT that
    override and without the tray and expects the layer to stay dormant (pass-through, nothing recorded).
    In-headset indicator (vr_indicator = 1, default): the mock runtime validates and counts the quad layer the layer
    appends. D3D11 cases: valid quad on at least as many frames as were recorded, never on every frame (hidden before /
    after), no invalid quad, log line "appended AFTER the capture". D3D12: no quad + "not available on D3D12" line.
    Extra case d3d11 / two with vr_indicator = 0: no quad, and the recorded pixels where the dot would be seen (both eyes)
    equal the test pattern and the indicator-on recording (the indicator never reaches the video). Dormant: no quad.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$exe = Join-Path $bin 'layer_e2e.exe'
if (-not (Test-Path $exe)) { throw "build first: $exe" }
$results = @()
$ffCand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffmpeg.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$ffmpegExe = if ($ffCand) { $ffCand.FullName } else { (Get-Command ffmpeg).Source }
function QuadInfo($o) {
    $q = [regex]::Match($o, 'quad layers: valid on (\d+) frames, invalid (\d+)')
    $f = [regex]::Match($o, 'session destroyed: (\d+) frames')
    return @{ ok = $q.Success; valid = [int]$q.Groups[1].Value; bad = [int]$q.Groups[2].Value; total = [int]$f.Groups[1].Value }
}
# Region of the recorded frame where the indicator would be seen (view-space (0.22, -0.16, -0.8), fov +-0.8 rad):
# x_tan 0.275 -> u 0.634 -> x 1216, y_tan -0.2 -> v 0.597 -> y 645 (left eye; the right eye at +1920). Both regions lie
# in the eye-id band of the pattern: left (200,40,40), right (40,40,200). Returns the worst deviation from the band
# colour and the RGB bytes of the region (for the on / off comparison).
function IndicatorRegion($file) {
    $raw = Join-Path ([System.IO.Path]::GetTempPath()) ("eyecap_ind_{0}.rgb" -f $PID)
    & $ffmpegExe -v error -y -ss 2.0 -i $file -frames:v 1 -vf 'scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=rgb24' -f rawvideo $raw
    $px = [System.IO.File]::ReadAllBytes($raw); Remove-Item $raw -ErrorAction SilentlyContinue
    $W = 3840; $worst = 0; $vals = New-Object System.Collections.Generic.List[int]
    foreach ($eye in 0, 1) {
        $ref = if ($eye -eq 0) { @(200, 40, 40) } else { @(40, 40, 200) }
        for ($y = 620; $y -lt 670; $y += 2) {
            for ($x = 1176 + 1920 * $eye; $x -lt 1256 + 1920 * $eye; $x += 2) {
                $o = ($y * $W + $x) * 3
                for ($k = 0; $k -lt 3; $k++) { $d = [math]::Abs($px[$o + $k] - $ref[$k]); if ($d -gt $worst) { $worst = $d }; $vals.Add($px[$o + $k]) }
            }
        }
    }
    return @{ worst = $worst; vals = $vals }
}
$indOnFile = $null
$prevRequireTray = $env:XR_EYE_CAPTURE_REQUIRE_TRAY
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
# Recording cases do not talk to a tray that may be running (it would post-process the test MP4s into MKVs and delete
# them). The dormant case below runs without this override.
$prevTrayIpc = $env:XR_EYE_CAPTURE_TRAY_IPC
$env:XR_EYE_CAPTURE_TRAY_IPC = '0'
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
        # indicator
        $qi = QuadInfo $o
        $logTxt = Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
        $written = [regex]::Match([string]$logTxt, 'frames written (\d+)')
        $nw = if ($written.Success) { [int]$written.Groups[1].Value } else { -1 }
        if ($api -eq 'd3d11') {
            $indOk = $qi.ok -and $qi.bad -eq 0 -and $qi.valid -ge $nw -and $nw -gt 0 -and $qi.valid -lt $qi.total -and
                     ([string]$logTxt -match 'indicator: quad layer appended AFTER the capture')
            if ($layout -eq 'two' -and $runOk) { $indOnFile = $m.Groups[1].Value.Trim() }
        } else {
            $indOk = $qi.ok -and $qi.valid -eq 0 -and $qi.bad -eq 0 -and ([string]$logTxt -match 'indicator: not available on D3D12 yet')
        }
        Write-Host ("  indicator: quad layer valid on {0} of {1} frames, invalid {2}; frames recorded {3} -> {4}" -f $qi.valid, $qi.total, $qi.bad, $nw, $(if ($indOk) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($indOk) { 'Green' } else { 'Red' })
        $results += [pscustomobject]@{ api = $api; layout = $layout; run = $runOk; verify = ($verOk -and $indOk) }
    }
}

# ---- indicator off: no quad; the recorded region where the dot would be is the pattern, same as with the indicator ----
$out = Join-Path $root 'build\e2e_out\d3d11_two_noindicator'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
Write-Host "`n=== d3d11 / two, vr_indicator = 0" -ForegroundColor Cyan
$prevCfg = $env:XR_EYE_CAPTURE_CONFIG
$env:XR_EYE_CAPTURE_CONFIG = 'vr_indicator=0'
$o = & $exe --api d3d11 --layout two --seconds 6 --record 4 --out-dir $out 2>&1 | Out-String
if ($null -ne $prevCfg) { $env:XR_EYE_CAPTURE_CONFIG = $prevCfg } else { Remove-Item Env:\XR_EYE_CAPTURE_CONFIG -ErrorAction SilentlyContinue }
$m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
$runOk = ($LASTEXITCODE -eq 0) -and $m.Success
$qi = QuadInfo $o
$verOk = $false
if ($runOk -and $indOnFile) {
    $off = IndicatorRegion $m.Groups[1].Value.Trim()
    $on = IndicatorRegion $indOnFile
    $maxDiff = 0
    for ($i = 0; $i -lt $off.vals.Count; $i++) { $d = [math]::Abs($off.vals[$i] - $on.vals[$i]); if ($d -gt $maxDiff) { $maxDiff = $d } }
    $verOk = $qi.ok -and $qi.valid -eq 0 -and $off.worst -le 12 -and $on.worst -le 12 -and $maxDiff -le 12
    Write-Host ("  quad layers with vr_indicator = 0: {0}; dot region (both eyes) vs pattern colour: off {1}, on {2}; on vs off max diff {3} (limit 12) -> {4}" -f
        $qi.valid, $off.worst, $on.worst, $maxDiff, $(if ($verOk) { 'ok: the indicator is not in the recording' } else { 'FAIL' })) -ForegroundColor $(if ($verOk) { 'Green' } else { 'Red' })
} else { Write-Host '  run failed or no indicator-on recording to compare' -ForegroundColor Red }
$results += [pscustomobject]@{ api = 'd3d11'; layout = 'two, no indicator'; run = $runOk; verify = $verOk }

# ---- dormant case: no override, no tray -> the layer must do nothing but forward ----
Remove-Item Env:\XR_EYE_CAPTURE_REQUIRE_TRAY -ErrorAction SilentlyContinue
if ($null -ne $prevTrayIpc) { $env:XR_EYE_CAPTURE_TRAY_IPC = $prevTrayIpc } else { Remove-Item Env:\XR_EYE_CAPTURE_TRAY_IPC -ErrorAction SilentlyContinue }
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
    $qd = QuadInfo $o
    $runOk = $runOk -and $qd.ok -and $qd.valid -eq 0
    Write-Host "  indicator quad layers while dormant: $($qd.valid) (want 0)"
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
