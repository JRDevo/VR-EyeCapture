<#
    run_e2e.ps1 -- end-to-end layer test over every swapchain layout and both D3D APIs, through the real OpenXR loader,
    the INSTALLED layer (run scripts\install.ps1 first) and tests/mock_runtime (no headset). Verifies every MP4.
      powershell -ExecutionPolicy Bypass -File tests\run_e2e.ps1
    The recording cases run with XR_EYE_CAPTURE_REQUIRE_TRAY=0 (no tray app needed). The last case runs WITHOUT that
    override and without the tray and expects the layer to stay dormant (pass-through, nothing recorded); while an
    EyeCaptureTray runs that case is reported "SKIPPED" (neither passed nor failed; the other cases decide the exit code).
    In-headset indicator (vr_indicator = 1, default): the mock runtime validates and counts the quad layer the layer
    appends. D3D11 cases: valid quad on at least as many frames as were recorded, never on every frame (hidden before /
    after), no invalid quad, log line "appended AFTER the capture". D3D12: no quad + "not available on D3D12" line.
    Extra case d3d11 / two with vr_indicator = 0: no quad, and the recorded pixels where the dot would be seen (both eyes)
    equal the test pattern and the indicator-on recording (the indicator never reaches the video). Dormant: no quad.
    Overlays (record_overlays): d3d11 and d3d12 / two with layer_e2e --overlays (an opaque quad in the projection space, a
    50 % white VIEW-space quad, an opaque VIEW-space cylinder whose texture alpha is 0, a back-facing quad): the decoded
    frame must show each one at the place the mock's head pose + fov predict, with the expected colour (the 50 % quad
    blended in LINEAR light over the pattern), the back-facing quad absent and (D3D11) the indicator region unchanged.
    The same run also submits a passthrough-layer stub, a cube and a quad BEFORE the (source-alpha) projection layer
    (seen only where the pattern is alpha 0), an equirect2, an equirect v1 on a 10 m sphere, a 4x MSAA quad, an FP16
    quad, a 10-bit quad and a quad with XrCompositionLayerColorScaleBiasKHR: each checked at its predicted pixel(s).
    With record_overlays = 0 the same places show the plain pattern. The stale-pose cases use an opaque projection
    layer: the layers before it must be skipped with a log line.
      -OnlyOverlays: run only the overlay cases.
      -BuildLayer: test build\Release\eye_capture.dll instead of the installed (registered) copy -- the registered implicit
                   layer is disabled through its disable_environment and the build copy is enabled as an explicit layer
                   (XR_API_LAYER_PATH / XR_ENABLE_API_LAYERS). Nothing is copied or registered. Its log is build\Release\logs.
#>
param([switch]$BuildLayer, [switch]$OnlyOverlays)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$layerLog = Join-Path $root 'logs\eye_capture.log'
if ($BuildLayer) {
    $env:DISABLE_XR_APILAYER_JRDEVO_eye_capture = '1'
    # a folder with only our manifest (the loader reads every json in XR_API_LAYER_PATH), pointing at the build DLL
    $lp = Join-Path $root 'build\e2e_layer'
    New-Item -ItemType Directory -Force $lp | Out-Null
    $m = Get-Content (Join-Path $bin 'XR_APILAYER_JRDEVO_eye_capture.json') -Raw | ConvertFrom-Json
    $m.api_layer.library_path = (Join-Path $bin 'eye_capture.dll')
    $m | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $lp 'XR_APILAYER_JRDEVO_eye_capture.json') -Encoding ascii
    $env:XR_API_LAYER_PATH = $lp
    $env:XR_ENABLE_API_LAYERS = 'XR_APILAYER_JRDEVO_eye_capture'
    $layerLog = Join-Path $bin 'logs\eye_capture.log'
    Write-Host "-BuildLayer: testing $bin\eye_capture.dll (registered copy disabled for these processes)" -ForegroundColor Yellow
}
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
# These cases test the flat side-by-side path whatever config.ini says ([dome] projection = vr180 records a dome).
$prevCfg = $env:XR_EYE_CAPTURE_CONFIG
$env:XR_EYE_CAPTURE_CONFIG = 'projection=flat'
foreach ($api in $(if ($OnlyOverlays) { @() } else { @('d3d11', 'd3d12') })) {
    foreach ($layout in @('two', 'array', 'wide', 'msaa')) {
        $out = Join-Path $root "build\e2e_out\${api}_$layout"
        if (Test-Path $out) { Remove-Item -Recurse -Force $out }
        Write-Host "`n=== $api / $layout" -ForegroundColor Cyan
        $o = & $exe --api $api --layout $layout --seconds 6 --record 4 --out-dir $out 2>&1 | Out-String
        $o.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
        $m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
        $runOk = ($LASTEXITCODE -eq 0) -and $m.Success
        $verOk = $false
        $logTxt = Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
        $written = [regex]::Match([string]$logTxt, 'frames written (\d+), dropped (\d+)')
        $nw = if ($written.Success) { [int]$written.Groups[1].Value } else { -1 }
        $nd = if ($written.Success) { [int]$written.Groups[2].Value } else { -1 }
        # Dropped frames: the recorder never makes the game wait -- a ring slot whose previous copy is still on the GPU ("gpu
        # busy", D3D12) or a full ring drops that frame and counts it in the summary; the file keeps the real timestamps, so its
        # average rate is 90 x written / (written + dropped). Measured 2026-10-18: since 2026-10-17 this PC drops 1-30 of ~345
        # frames per D3D12 case (0 before), with the same counts for builds from before 2026-10-17 (c1c4fb6, c1a72c5, 39c00d4)
        # -- the machine, not the code. So the rate is checked against the frames really written, and the drops get their own
        # limit (10 %): a real stall (a copy that waits, an encoder that falls behind) drops far more than that.
        $fpsWant = 90.0
        $dropOk = $written.Success -and ($nw + $nd) -gt 0 -and $nd -le 0.10 * ($nw + $nd)
        if ($written.Success -and ($nw + $nd) -gt 0) { $fpsWant = 90.0 * $nw / ($nw + $nd) }
        $dropLine = [regex]::Match([string]$logTxt, 'dropped \d+ \([^)]*\)').Value
        Write-Host ("  dropped frames: {0} of {1} ({2}); file rate wanted {3:N2} fps -> {4}" -f $nd, ($nw + $nd), $dropLine, $fpsWant,
            $(if ($dropOk) { 'ok (limit 10 %)' } else { 'FAIL (over 10 % or no summary)' })) -ForegroundColor $(if ($dropOk) { 'Green' } else { 'Red' })
        if ($runOk) {
            # autorecord counts from the first frame; encoder start-up takes ~0.2-0.4 s of it
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $m.Groups[1].Value.Trim() -EyeW 1920 -EyeH 1080 -Fps $fpsWant -Seconds 3.7 -DurationTolerance 0.3 -ExpectAudio
            $verOk = ($LASTEXITCODE -eq 0) -and $dropOk
        }
        # indicator
        $qi = QuadInfo $o
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
if (-not $OnlyOverlays) {
$out = Join-Path $root 'build\e2e_out\d3d11_two_noindicator'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
Write-Host "`n=== d3d11 / two, vr_indicator = 0" -ForegroundColor Cyan
$env:XR_EYE_CAPTURE_CONFIG = 'projection=flat;vr_indicator=0'
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
}

# ---- overlays (record_overlays): quad / cylinder layers composited into the recording ----
# Mock: head at (0, 1.6, 0) with identity orientation, eyes at x -0.032 / +0.032, fov +-0.8 rad, 1920x1080 per eye.
function OvPx($x, $y, $z, $eye) {   # projection-space point -> pixel (x in the SBS frame, y)
    $T = [math]::Tan(0.8); $ex = if ($eye -eq 0) { -0.032 } else { 0.032 }
    $u = (($x - $ex) / (-$z) + $T) / (2 * $T); $v = ($T - ($y - 1.6) / (-$z)) / (2 * $T)
    return @([int][math]::Round($u * 1920 + 1920 * $eye), [int][math]::Round($v * 1080))
}
function Frame2s($file) {
    $raw = Join-Path ([System.IO.Path]::GetTempPath()) ("eyecap_ov_{0}.rgb" -f $PID)
    & $ffmpegExe -v error -y -ss 2.0 -i $file -frames:v 1 -vf 'scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=rgb24' -f rawvideo $raw
    $px = [System.IO.File]::ReadAllBytes($raw); Remove-Item $raw -ErrorAction SilentlyContinue
    return $px
}
function BoxAvg($px, $cx, $cy, $dx, $dy) {
    $s = @(0.0, 0.0, 0.0); $n = 0
    for ($y = $cy - $dy; $y -le $cy + $dy; $y++) { for ($x = $cx - $dx; $x -le $cx + $dx; $x++) {
        $o = ($y * 3840 + $x) * 3; for ($k = 0; $k -lt 3; $k++) { $s[$k] += $px[$o + $k] }; $n++ } }
    return @([math]::Round($s[0] / $n), [math]::Round($s[1] / $n), [math]::Round($s[2] / $n))
}
function ColDiff($a, $b) { $m = 0; for ($k = 0; $k -lt 3; $k++) { $d = [math]::Abs($a[$k] - $b[$k]); if ($d -gt $m) { $m = $d } }; return $m }
function Dec($c) { $c = $c / 255.0; if ($c -le 0.04045) { $c / 12.92 } else { [math]::Pow(($c + 0.055) / 1.055, 2.4) } }
function Enc($l) { if ($l -le 0.0031308) { 255 * 12.92 * $l } else { 255 * (1.055 * [math]::Pow($l, 1 / 2.4) - 0.055) } }
function FrameAt($file, $t) {
    $raw = Join-Path ([System.IO.Path]::GetTempPath()) ("eyecap_st_{0}.rgb" -f $PID)
    & $ffmpegExe -v error -y -ss $t -i $file -frames:v 1 -vf 'scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=rgb24' -f rawvideo $raw
    $px = [System.IO.File]::ReadAllBytes($raw); Remove-Item $raw -ErrorAction SilentlyContinue
    return $px
}
function YellowCentroid($px, $cx, $cy, $dx, $dy) {
    $sx = 0.0; $n = 0
    for ($y = $cy - $dy; $y -le $cy + $dy; $y++) { for ($x = [math]::Max(0, $cx - $dx); $x -le [math]::Min(3839, $cx + $dx); $x++) {
        $o = ($y * 3840 + $x) * 3
        if ($px[$o] -gt 200 -and $px[$o + 1] -gt 200 -and $px[$o + 2] -lt 80) { $sx += $x; $n++ } } }
    if ($n -lt 50) { return $null }
    return ($sx / $n)
}
$bandC = @(@(200, 40, 40), @(40, 40, 200))   # the pattern's eye-id band (rows 540..674): left / right
$aB = 128.0 / 255.0
# --- ray layers (equirect / cube): pixel <-> direction for the mock's eyes (identity orientation, fov +-0.8 rad) ---
$T8 = [math]::Tan(0.8)
function DirPx($d, $eye) {   # direction (x, y, z) from the eye -> pixel of the SBS frame (infinitely far: no parallax)
    $tx = $d[0] / -$d[2]; $ty = $d[1] / -$d[2]
    return @([int][math]::Round(($tx + $T8) / (2 * $T8) * 1920 + 1920 * $eye), [int][math]::Round(($T8 - $ty) / (2 * $T8) * 1080))
}
function YawDir($az, $el, $yaw) {   # layer-frame (azimuth from -Z towards +X, elevation) -> projection-space direction, layer turned $yaw about +Y
    $x = [math]::Sin($az) * [math]::Cos($el); $y = [math]::Sin($el); $z = -[math]::Cos($az) * [math]::Cos($el)
    return @(($x * [math]::Cos($yaw) + $z * [math]::Sin($yaw)), $y, (-$x * [math]::Sin($yaw) + $z * [math]::Cos($yaw)))
}
function CubeAt($px, $py, $yaw) {   # eye-relative pixel -> @(face, u, v) of a cube turned $yaw about +Y (TextureCube rules)
    $tx = ($px / 1920) * 2 * $T8 - $T8; $ty = $T8 - ($py / 1080) * 2 * $T8
    # layer frame = inverse rotation of the world direction (tx, ty, -1)
    $x = $tx * [math]::Cos(-$yaw) + (-1) * [math]::Sin(-$yaw); $y = $ty; $z = -$tx * [math]::Sin(-$yaw) + (-1) * [math]::Cos(-$yaw)
    $ax = [math]::Abs($x); $ay = [math]::Abs($y); $az = [math]::Abs($z)
    if ($ax -ge $ay -and $ax -ge $az) { $ma = $ax; if ($x -gt 0) { $f = 0; $s = -$z } else { $f = 1; $s = $z }; $t = -$y }
    elseif ($ay -ge $az) { $ma = $ay; if ($y -gt 0) { $f = 2; $t = $z } else { $f = 3; $t = -$z }; $s = $x }
    else { $ma = $az; if ($z -gt 0) { $f = 4; $s = $x } else { $f = 5; $s = -$x }; $t = -$y }
    return @($f, (0.5 * ($s / $ma + 1)), (0.5 * ($t / $ma + 1)))
}
$cubeFace = @(@(0, 0, 160), @(160, 160, 0), @(160, 0, 160), @(0, 160, 160), @(100, 100, 100), @(0, 160, 0))
$kGrey = @(0, 32, 64, 96, 128, 160, 192, 224, 255)
function AddLin($a, $b) { return @(0, 1, 2 | ForEach-Object { [math]::Round((Enc ([math]::Min(1.0, (Dec $a[$_]) + (Dec $b[$_]))))) }) }
function EncLin($l) { return @($l | ForEach-Object { [math]::Round((Enc $_)) }) }
foreach ($ovCase in @(@{ api = 'd3d11'; on = $true }, @{ api = 'd3d12'; on = $true }, @{ api = 'd3d11'; on = $false })) {
    $api = $ovCase.api; $on = $ovCase.on
    $name = "${api}_two_overlays" + $(if ($on) { '' } else { '_off' })
    $out = Join-Path $root "build\e2e_out\$name"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    Write-Host "`n=== $api / two, overlays, record_overlays = $(if ($on) { 1 } else { 0 })" -ForegroundColor Cyan
    $env:XR_EYE_CAPTURE_CONFIG = 'projection=flat;head_smoothing=0;record_overlays=' + $(if ($on) { '1' } else { '0' })
    $o = & $exe --api $api --layout two --seconds 6 --record 4 --out-dir $out --overlays 2>&1 | Out-String
    if ($null -ne $prevCfg) { $env:XR_EYE_CAPTURE_CONFIG = $prevCfg } else { Remove-Item Env:\XR_EYE_CAPTURE_CONFIG -ErrorAction SilentlyContinue }
    $o.Trim().Split("`n") | Where-Object { $_ -match 'E2E_|mock runtime|FAIL' } | ForEach-Object { Write-Host "  $_" }
    $m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
    $runOk = ($LASTEXITCODE -eq 0) -and $m.Success
    $logTxt = [string](Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
    $logTxt -split "`r?`n" | Where-Object { $_ -match 'overlay' } | ForEach-Object { Write-Host ('  log: ' + $_.Substring([math]::Min(31, $_.Length))) }
    $ok = $runOk -and ($o -match 'cylinder layers on [1-9]')
    if ($runOk) {
        $px = Frame2s $m.Groups[1].Value.Trim()
        foreach ($eye in 0, 1) {
            $bg = $bandC[$eye]
            # A: opaque green quad in the projection space, centre (0, 1.45, -2)
            $p = OvPx 0.0 1.45 -2.0 $eye; $a = BoxAvg $px $p[0] $p[1] 20 15
            $want = if ($on) { @(0, 255, 0) } else { $bg }
            $okA = (ColDiff $a $want) -le 12
            # B: 50 % white VIEW-space quad (unpremultiplied), centre (-0.30, -0.128, -1) from the head; blended in linear light
            $p = OvPx -0.30 (1.6 - 0.128) -1.0 $eye; $b = BoxAvg $px $p[0] $p[1] 10 6
            $lin = @(0, 1, 2 | ForEach-Object { [math]::Round((Enc ($aB + (1 - $aB) * (Dec $bg[$_])))) })
            $gam = @(0, 1, 2 | ForEach-Object { [math]::Round(255 * $aB + (1 - $aB) * $bg[$_]) })
            $want = if ($on) { $lin } else { $bg }
            $okB = (ColDiff $b $want) -le 12
            # C: opaque orange cylinder (texture alpha 0, no blend flag), arc centre (0.5, 0.9, -1.5) from the head
            $p = OvPx 0.5 (1.6 + 0.9) -1.5 $eye; $c = BoxAvg $px $p[0] $p[1] 10 6
            $okC = if ($on) { (ColDiff $c @(255, 128, 0)) -le 12 } else { ([math]::Max($c[0], [math]::Max($c[1], $c[2])) - [math]::Min($c[0], [math]::Min($c[1], $c[2]))) -le 20 }
            # D: back-facing quad -> never drawn, the pattern stays
            $p = OvPx 0.7 1.41 -1.5 $eye; $dd = BoxAvg $px $p[0] $p[1] 10 6
            $okD = (ColDiff $dd $bg) -le 12
            # E: opaque cyan quad, eyeVisibility LEFT: in the left eye only
            $p = OvPx -1.094 1.4284 -1.5 $eye; $ee = BoxAvg $px $p[0] $p[1] 20 5
            $okE = (ColDiff $ee $(if ($on -and $eye -eq 0) { @(0, 255, 255) } else { $bg })) -le 12
            $eyeOk = $okA -and $okB -and $okC -and $okD -and $okE
            Write-Host ("  eye {0}: A quad {1} ({2}) | B 50% quad {3} want {4} (gamma-space blend would be {5}) ({6}) | C cylinder {7} ({8}) | D back face {9} ({10})" -f
                $eye, ($a -join ','), $(if ($okA) { 'ok' } else { 'FAIL' }), ($b -join ','), ($want -join ','), ($gam -join ','),
                $(if ($okB) { 'ok' } else { 'FAIL' }), ($c -join ','), $(if ($okC) { 'ok' } else { 'FAIL' }), ($dd -join ','),
                $(if ($okD) { 'ok: not drawn' } else { 'FAIL' })) -ForegroundColor $(if ($eyeOk) { 'Green' } else { 'Red' })
            Write-Host ("         E left-eye-only quad {0} ({1})" -f ($ee -join ','), $(if ($okE) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($eyeOk) { 'Green' } else { 'Red' })
            $ok = $ok -and $eyeOk
            # ---- the wider layer set: (name, measured, wanted) per check; with record_overlays = 0 the pattern instead ----
            $chk = @()
            $add = { param($n, $px2, $want2, $dx = 4, $dy = 3) $m2 = BoxAvg $px $px2[0] $px2[1] $dx $dy; $script:chk += , @($n, $m2, $want2) }
            # H: opaque purple quad BEHIND the (source-alpha) projection: visible under the alpha-0 black grey step, hidden under the red block
            & $add 'H behind, under alpha 0' (OvPx -1.864 3.144 -2.0 $eye) $(if ($on) { @(128, 0, 255) } else { @(0, 0, 0) })
            & $add 'H behind, under alpha 255' (OvPx -1.864 2.324 -2.0 $eye) @(255, 0, 0)
            # K: cube BEHIND the projection (turned +45 deg about +Y): seen only in the alpha-0 grey band A, added to the grey
            # (the pattern's band A is premultiplied colour with alpha 0); face + texel from the TextureCube rules
            foreach ($cp in @(@(320, 200), @(800, 200), @(1200, 150), @(960, 30))) {
                $fuv = CubeAt $cp[0] $cp[1] ([math]::PI / 4)
                $cc = if ($fuv[0] -eq 5 -and $fuv[1] -lt 0.25) { @(90, 0, 200) } else { $cubeFace[$fuv[0]] }
                $g = $kGrey[[math]::Floor($cp[0] * 9 / 1920)]
                & $add ('K cube face {0} u {1:F2}' -f $fuv[0], $fuv[1]) @(($cp[0] + 1920 * $eye), $cp[1]) $(if ($on) { AddLin @($g, $g, $g) $cc } else { @($g, $g, $g) })
            }
            if ($on) {
                # I: equirect2, infinite, yaw -0.64: left half (255,0,128) / right half (0,64,255) at azimuth -+0.03, elevation -0.33
                & $add 'I equirect2 left half' (DirPx (YawDir -0.03 -0.33 -0.64) $eye) @(255, 0, 128)
                & $add 'I equirect2 right half' (DirPx (YawDir 0.03 -0.33 -0.64) $eye) @(0, 64, 255)
                $pOut = DirPx (YawDir 0.0 -0.12 -0.64) $eye   # above upperVerticalAngle (-0.27): not covered -> band C
                & $add 'I equirect2 outside (pattern)' $pOut $bandC[$eye]
                # J: equirect v1 on the 10 m sphere around the head (parallax of the eye offset), halves at azimuth -+0.03, elevation -0.5
                foreach ($jj in @(@(-0.03, @(0, 200, 200), 'left'), @(0.03, @(200, 0, 200), 'right'))) {
                    $dj = YawDir $jj[0] -0.5 -0.64
                    & $add ('J equirect r=10 {0} half' -f $jj[2]) (OvPx (10 * $dj[0]) (1.6 + 10 * $dj[1]) (10 * $dj[2]) $eye) $jj[1]
                }
                & $add 'M 4x MSAA quad' (OvPx 1.45 2.40 -2.0 $eye) @(0, 128, 128) 10 6
                & $add 'F FP16 quad (linear 0.2,0.6,0.2)' (OvPx 1.45 2.057 -2.0 $eye) (EncLin @(0.2, 0.6, 0.2)) 10 6
                & $add 'T 10-bit quad (linear 0.8,0.3,0.5)' (OvPx 1.45 1.714 -2.0 $eye) (EncLin @(0.8, 0.3, 0.5)) 10 6
                & $add 'S colour scale/bias (0.5,0.25,0.2)' (OvPx 1.02 2.057 -2.0 $eye) (EncLin @(0.5, 0.25, 0.2)) 10 6
            }
            foreach ($c2 in $chk) {
                $ok2 = (ColDiff $c2[1] $c2[2]) -le 12
                Write-Host ("         {0}: {1} want {2} ({3})" -f $c2[0], ($c2[1] -join ','), ($c2[2] -join ','), $(if ($ok2) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($ok2) { 'Green' } else { 'Red' })
                $ok = $ok -and $ok2
            }
        }
        if ($api -eq 'd3d11') {   # the indicator (shown in the headset during the recording) is never in the video
            $ind = IndicatorRegion $m.Groups[1].Value.Trim()
            $okI = $ind.worst -le 12 -and ($o -match 'quad layers: valid on [1-9]')
            Write-Host ("  indicator region vs pattern: worst {0} (limit 12) -> {1}" -f $ind.worst, $(if ($okI) { 'ok: not recorded' } else { 'FAIL' })) -ForegroundColor $(if ($okI) { 'Green' } else { 'Red' })
            $ok = $ok -and $okI
        }
        # G: projection-space quad with a NEW pose every frame (x = 0.4 sin(2 pi 0.1 tau)): at t = 1 s and 3 s of the video
        # its centre must be where THAT frame's pose puts it (video t = displayTime - first recorded displayTime)
        $d0 = [regex]::Match($logTxt, 'first video frame \(t = 0 in the file\): displayTime (\d+)'); $t0 = [regex]::Match($o, 'E2E_T0_NS=(\d+)')
        $okG = $d0.Success -and $t0.Success
        $gTxt = @()
        if ($okG) {
            $offs = ([double]$d0.Groups[1].Value - [double]$t0.Groups[1].Value) / 1e9
            foreach ($t in 1.0, 3.0) {
                $pxG = FrameAt $m.Groups[1].Value.Trim() $t
                $xw = 0.4 * [math]::Sin(2 * [math]::PI * 0.1 * ($t + $offs))
                foreach ($eye in 0, 1) {
                    $p = OvPx $xw 0.228 -2.0 $eye
                    $c = YellowCentroid $pxG $p[0] $p[1] 300 20
                    if ($on) { $g1 = $null -ne $c -and [math]::Abs($c - $p[0]) -le 4 } else { $g1 = $null -eq $c }
                    $gTxt += ('t {0} eye {1}: {2} vs {3}' -f $t, $eye, $(if ($null -ne $c) { '{0:F1}' -f $c } else { 'none' }), $p[0])
                    $okG = $okG -and $g1
                }
            }
        }
        Write-Host ("  G per-frame pose quad (centre x): {0} -> {1}" -f ($gTxt -join ' | '), $(if ($okG) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($okG) { 'Green' } else { 'Red' })
        $ok = $ok -and $okG
        if ($on) {
            # layer order: 1 passthrough stub, 2 cube, 3 quad H, 4 projection, 5 A, 6 B, 7 C, 8 D, 9 E, 10 G, 11 I, 12 J, 13 M, 14 F, 15 T, 16 S
            $need = @('overlay: layer 5 \(quad\) RECORDED', 'overlay: layer 6 \(quad\) RECORDED', 'overlay: layer 7 \(cylinder\) RECORDED',
                      'overlay: layer 1 \(XR_FB_passthrough layer, XrStructureType 1000118003\) before the projection layer -- not recorded: this layer type is not supported',
                      'overlay: layer 2 \(cube\) RECORDED BEHIND the projection layer', 'overlay: layer 3 \(quad\) RECORDED BEHIND the projection layer',
                      'overlay: layer 11 \(equirect2\) RECORDED', 'overlay: layer 12 \(equirect\) RECORDED',
                      'overlay: layer 13 \(quad\) RECORDED: .* 4x MSAA \(resolved\)', 'overlay: layer 14 \(quad\) RECORDED: .*R16G16B16A16_FLOAT',
                      'overlay: layer 15 \(quad\) RECORDED: .*R10G10B10A2_UNORM', 'overlay: layer 16 \(quad\) RECORDED: .*colour scale \(0.50 0.25 0.00 1.00\) bias \(0.00 0.00 0.20 0.00\)',
                      'overlay summary: [1-9]\d* frames with overlays composited', 'atlas setup failed 0 ',
                      'recorder: overlays: R8G8B8A8 atlas', 'recorder: overlays: R16G16B16A16_FLOAT atlas', 'recorder: overlays: R10G10B10A2 atlas')
            $missing = @($need | Where-Object { $logTxt -cnotmatch $_ })
            $okLog = $missing.Count -eq 0 -and ($o -match 'equirect layers on [1-9]\d* frames, equirect2 on [1-9]\d*, cube on [1-9]\d*, other layer types on [1-9]') -and
                     ($logTxt -notmatch 'not recorded: (multisampled|swapchain format)')
            if ($missing.Count) { $missing | ForEach-Object { Write-Host "  missing log line: $_" -ForegroundColor Red } }
        } else {
            $okLog = ($logTxt -match 'record_overlays = 0') -and ($logTxt -notmatch 'overlay summary') -and ($logTxt -cnotmatch 'RECORDED')
        }
        Write-Host ("  log lines: {0}" -f $(if ($okLog) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($okLog) { 'Green' } else { 'Red' })
        $ok = $ok -and $okLog
    }
    $results += [pscustomobject]@{ api = $api; layout = "two, overlays $(if ($on) { 'on' } else { 'off' })"; run = $runOk; verify = $ok }
}

# ---- overlays with a stale submitted pose: VIEW-space layers follow the SUBMITTED head ----
# layer_e2e --stale-pose: mock head motion (tests/test_motion.h), the app submits the views located 33 ms before
# displayTime (submitted != located, as in ETS2), raw eye images (symmetric_fov = 0). The opaque VIEW-space cylinder C
# must sit at the same pixel in every frame (centroid of its orange pixels at 5 times, both eyes).
function OrangeCentroid($px, $cx, $cy, $dx, $dy) {
    $sx = 0.0; $sy = 0.0; $n = 0
    for ($y = [math]::Max(0, $cy - $dy); $y -le [math]::Min(1079, $cy + $dy); $y++) {
        for ($x = $cx - $dx; $x -le $cx + $dx; $x++) {
            $o = ($y * 3840 + $x) * 3
            if ($px[$o] -gt 200 -and $px[$o + 1] -gt 80 -and $px[$o + 1] -lt 175 -and $px[$o + 2] -lt 70) { $sx += $x; $sy += $y; $n++ }
        }
    }
    if ($n -lt 50) { return $null }
    return @(($sx / $n), ($sy / $n), $n)
}
$tanD = { param($deg) [math]::Tan($deg * [math]::PI / 180) }
foreach ($api in @('d3d11', 'd3d12')) {
    $out = Join-Path $root "build\e2e_out\${api}_two_overlays_stale"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    Write-Host "`n=== $api / two, overlays, stale submitted pose (head motion, submitted != located)" -ForegroundColor Cyan
    $env:XR_EYE_CAPTURE_CONFIG = 'projection=flat;symmetric_fov=0;head_smoothing=0;record_overlays=1'
    $o = & $exe --api $api --layout two --seconds 6 --record 4 --out-dir $out --stale-pose --opaque-projection 2>&1 | Out-String
    if ($null -ne $prevCfg) { $env:XR_EYE_CAPTURE_CONFIG = $prevCfg } else { Remove-Item Env:\XR_EYE_CAPTURE_CONFIG -ErrorAction SilentlyContinue }
    $m = [regex]::Match($o, 'E2E_OUTPUT=(.+)')
    $runOk = ($LASTEXITCODE -eq 0) -and $m.Success
    $logTxt = [string](Get-Content (Join-Path $out 'eye_capture.log') -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
    $ok = $runOk -and ($logTxt -cmatch 'layer 7 \(cylinder\) RECORDED: .*VIEW space: on the head of the submitted views')
    # --opaque-projection: the layers before the projection layer are hidden by it -> skipped with a log line, not drawn
    $okHidden = ($logTxt -cmatch 'layer 2 \(cube\) is BEFORE the projection layer, which is opaque') -and
                ($logTxt -cmatch 'layer 3 \(quad\) is BEFORE the projection layer, which is opaque') -and ($logTxt -cnotmatch 'RECORDED BEHIND')
    Write-Host ("  layers before the opaque projection layer: skipped with a log line -> {0}" -f $(if ($okHidden) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($okHidden) { 'Green' } else { 'Red' })
    $ok = $ok -and $okHidden
    if ($runOk) {
        foreach ($eye in 0, 1) {
            # expected centre: VIEW point (0.5, 0.9, -1.5) seen from the eye at -+0.032 on the head's X, mock fovs
            $tl = & $tanD $(if ($eye -eq 0) { -50 } else { -40 }); $tr = & $tanD $(if ($eye -eq 0) { 40 } else { 50 })
            $ex = if ($eye -eq 0) { -0.032 } else { 0.032 }
            $ux = ((0.5 - $ex) / 1.5 - $tl) / ($tr - $tl); $vy = (1.0 - 0.6) / 2.0
            $ecx = [int][math]::Round($ux * 1920 + 1920 * $eye); $ecy = [int][math]::Round($vy * 1080)
            $cs = @()
            foreach ($t in 0.8, 1.4, 2.0, 2.6, 3.2) {
                $c = OrangeCentroid (FrameAt $m.Groups[1].Value.Trim() $t) $ecx $ecy 260 90
                $cs += , $c
            }
            $valid = @($cs | Where-Object { $_ -ne $null })
            $spread = 0.0
            if ($valid.Count -eq 5) {
                $xs = $valid | ForEach-Object { $_[0] }; $ys = $valid | ForEach-Object { $_[1] }
                $spread = [math]::Max((($xs | Measure-Object -Max).Maximum - ($xs | Measure-Object -Min).Minimum), (($ys | Measure-Object -Max).Maximum - ($ys | Measure-Object -Min).Minimum))
            }
            $near = $valid.Count -eq 5 -and [math]::Abs($valid[0][0] - $ecx) -le 15 -and [math]::Abs($valid[0][1] - $ecy) -le 6
            $eyeOk = $valid.Count -eq 5 -and $spread -le 1.5 -and $near
            Write-Host ("  eye {0}: cylinder centroid at t 0.8 / 1.4 / 2.0 / 2.6 / 3.2 s: {1} | expected near {2},{3} | spread {4:F2} px (limit 1.5) -> {5}" -f
                $eye, (($cs | ForEach-Object { if ($_) { '{0:F1},{1:F1}' -f $_[0], $_[1] } else { 'none' } }) -join ' / '), $ecx, $ecy, $spread,
                $(if ($eyeOk) { 'ok: fixed in the view' } else { 'FAIL' })) -ForegroundColor $(if ($eyeOk) { 'Green' } else { 'Red' })
            $ok = $ok -and $eyeOk
        }
        # how far the located head (displayTime) is from the submitted one (33 ms earlier) at these times: what the
        # cylinder would have moved with xrLocateSpace placement (yaw / pitch difference of tests/test_motion.h)
        $d0 = [regex]::Match($logTxt, 'first video frame \(t = 0 in the file\): displayTime (\d+)'); $t0 = [regex]::Match($o, 'E2E_T0_NS=(\d+)')
        if ($d0.Success -and $t0.Success) {
            $off = ([double]$d0.Groups[1].Value - [double]$t0.Groups[1].Value) / 1e9; $mx = 0.0
            foreach ($t in 0.8, 1.4, 2.0, 2.6, 3.2) {
                $a = $t + $off; $b = $a - 0.0333333
                $dy = 2 * ([math]::Sin(2 * [math]::PI * 3.3 * $a) - [math]::Sin(2 * [math]::PI * 3.3 * $b))
                $dp = 2 * ([math]::Sin(2 * [math]::PI * 4.1 * $a + 1) - [math]::Sin(2 * [math]::PI * 4.1 * $b + 1))
                $mx = [math]::Max($mx, [math]::Sqrt($dy * $dy + $dp * $dp))
            }
            $pxPerDeg = 1920 / ([math]::Tan(40 * [math]::PI / 180) + [math]::Tan(50 * [math]::PI / 180)) * [math]::PI / 180
            Write-Host ("  (located vs submitted head at those times: up to {0:F2} deg = ~{1:F0} px -- the shift xrLocateSpace placement would show)" -f $mx, ($mx * $pxPerDeg))
        }
    }
    $results += [pscustomobject]@{ api = $api; layout = 'two, overlays, stale pose'; run = $runOk; verify = $ok }
}

# ---- dormant case: no override, no tray -> the layer must do nothing but forward ----
$skipped = @()
Remove-Item Env:\XR_EYE_CAPTURE_REQUIRE_TRAY -ErrorAction SilentlyContinue
if ($null -ne $prevTrayIpc) { $env:XR_EYE_CAPTURE_TRAY_IPC = $prevTrayIpc } else { Remove-Item Env:\XR_EYE_CAPTURE_TRAY_IPC -ErrorAction SilentlyContinue }
$trayUp = $true
try { $mx = [System.Threading.Mutex]::OpenExisting('Local\OpenXR-EyeCapture-TrayAlive'); $mx.Dispose() } catch { $trayUp = $false }
Write-Host "`n=== dormant (no tray, no override)" -ForegroundColor Cyan
if ($OnlyOverlays) {
    Write-Host '  skipped (-OnlyOverlays)'
} elseif ($trayUp) {
    # not a failure of the layer: the case cannot run while a tray holds the TrayAlive marker (it is not counted)
    Write-Host '  SKIP: an EyeCaptureTray is running (it holds Local\OpenXR-EyeCapture-TrayAlive) -- close it to run the dormant case' -ForegroundColor Yellow
    $skipped += 'dormant (EyeCaptureTray running)'
} else {
    $logFile = $layerLog
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
Get-Content $layerLog -Tail 400 | Select-String 'frame layout \(|recording summary' | Select-Object -Last 16 | ForEach-Object { '  ' + $_.Line.Substring([math]::Min(37, $_.Line.Length)) }
foreach ($s in $skipped) { Write-Host "SKIPPED: $s" -ForegroundColor Yellow }
if ($results | Where-Object { -not ($_.run -and $_.verify) }) { exit 1 }
exit 0
